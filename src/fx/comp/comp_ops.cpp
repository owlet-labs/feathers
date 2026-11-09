/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/comp/comp_dsp.h"
#include "fx/comp/comp_lanes.h"
#include "fx/comp/comp_types.h"

#include "backplane/axes.h"
#include "rack/inner_chain.h"
#include "rack/lazy_lane.h"

#include <algorithm>
#include <new>

// COMP AS A HOST SEES IT (backlog QG): the DSP (`CompDsp`, feathers-fx) and the Compander's encode/decode lane, a lazy
// `rack::InnerChain` held here because this target can see the rack — the Delay's `delay_ops.cpp`, the same component
// (`rack::LazyLane`, factored out of it unchanged).
//
// **THE LANE RUNS ONLY IN COMPANDER MODE**, between the halves (`CompDsp::processCompander`). In Compress and Limit its
// devices are kept, saved and drained, but not run — by design, switching away loses nothing. Its LATENCY is the
// device's in every mode: the Compander's dry waits for it, and Compress and Limit pad their output by it, so a Mode flip
// never moves the latency the host compensates (backlog LE's contract, kept).

namespace feathers::fx::comp {

namespace {

/// THE LANE'S AXES: the Delay's feedback lane's (no core sources or destinations; sixteen slots).
constexpr backplane::Axes kLaneAxes {
	.coreSources = 0,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};

struct OpsComp {
	CompDsp dsp {};
	rack::LazyLane lane { kLaneAxes, "compander" }; // the spike recorder's path: `COMP/compander/...`

	OpsComp() = default;
	OpsComp(const OpsComp&) = delete;
	OpsComp& operator=(const OpsComp&) = delete;

	/// THE LANE FOR THIS BLOCK, AND ITS DRAIN — the Delay's `beginBlock`: a zero-cost pickup, the drain and the latency
	/// taken once for the host block, and the insert handed to the DSP. A lane holding nothing is no lane.
	void beginBlock() {
		rack::InnerChain* const running = lane.take();
		CompDsp::LaneInsert insert {};
		if (running != nullptr) {
			running->beginHostBlock();
			if (running->engine().chainLength(0) > 0) {
				insert.context = running;
				insert.process = [](void* context, float* left, float* right, int frames) {
					static_cast<rack::InnerChain*>(context)->process(left, right, frames);
				};
				insert.latency = running->hostBlockLatency();
			}
		}
		dsp.lane = insert;
	}
	void endBlock() {
		if (rack::InnerChain* const running = lane.running(); running != nullptr) {
			running->endHostBlock();
		}
	}

	/// **SOUND STILL TO COME FROM THE LANE** keeps the device awake — only while it runs (Compander, enabled).
	[[nodiscard]] bool holdsTail() const noexcept {
		return dsp.state.enabled && dsp.state.mode == static_cast<int>(Mode::Compander) && lane.holdsTail();
	}

	/// THE LANE'S DECLARED LATENCY, MESSAGE THREAD — 0 while unbuilt.
	[[nodiscard]] uint32_t laneLatency(double sampleRate) const noexcept {
		const rack::InnerChain* owned = lane.owned();
		return owned != nullptr ? owned->latencySamples(sampleRate) : 0;
	}
};

OpsComp& asOps(void* storage) {
	return *static_cast<OpsComp*>(storage);
}
const OpsComp& asOps(const void* storage) {
	return *static_cast<const OpsComp*>(storage);
}

rack::LazyLane& laneOf(void* storage) {
	return asOps(storage).lane;
}

using LaneOps = rack::LazyLaneOps<&laneOf, kCompanderLaneId>;

void forEachRamp(CompDsp& d, auto&& f) {
	for (dyn::LinearRamp* r : { &d.makeupDb, &d.mix, &d.driveDb, &d.ceilingDb, &d.link, &d.bypass, &d.cmpRatio, &d.cmpRange,
	                            &d.cmpRatio2 }) {
		f(*r);
	}
}

} // namespace

rack::InnerChain* companderLaneOf(void* surface) noexcept {
	return surface != nullptr ? asOps(surface).lane.owned() : nullptr;
}
const rack::InnerChain* companderLaneOf(const void* surface) noexcept {
	return companderLaneOf(const_cast<void*>(surface));
}
rack::InnerChain* ensureCompanderLane(void* surface) {
	return surface != nullptr ? asOps(surface).lane.ensure() : nullptr;
}
const CompDsp& dspOf(const void* storage) {
	return asOps(storage).dsp;
}

float gainDbForTest(const void* storage) {
	const CompDsp& d = asOps(storage).dsp;
	return std::min(d.gainDb[0], d.gainDb[1]);
}
float detectorDbForTest(const void* storage) { return asOps(storage).dsp.detectorDb; }
float lastGainForTest(const void* storage) { return asOps(storage).dsp.lastGain; }
float envelopeForTest(const void* storage, size_t channel) { return asOps(storage).dsp.env[std::min<size_t>(channel, 1)]; }
float secondEnvelopeForTest(const void* storage, size_t channel) {
	return asOps(storage).dsp.env2[std::min<size_t>(channel, 1)];
}
float firstHalfDbForTest(const void* storage, size_t channel) {
	return asOps(storage).dsp.firstDb[std::min<size_t>(channel, 1)];
}
float secondHalfDbForTest(const void* storage, size_t channel) {
	return asOps(storage).dsp.secondDb[std::min<size_t>(channel, 1)];
}
float gainDbForTest(const void* storage, size_t channel) { return asOps(storage).dsp.gainDb[std::min<size_t>(channel, 1)]; }
float ceilingStageDbForTest(const void* storage) { return asOps(storage).dsp.stageDb; }
float peakOverCeilingForTest(const void* storage) { return asOps(storage).dsp.peakOverCeiling; }

const device::ContainerOps kContainerOps = LaneOps::make();

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsComp),
    .stateAlign = alignof(OpsComp),
    .construct =
        [](void* storage) {
	        new (storage) OpsComp();
	        asOps(storage).dsp.refresh();
        },
    .destruct = [](void* storage) { asOps(storage).~OpsComp(); },
    .prepare =
        [](void* storage, double sampleRate, int maxFrames) {
	        OpsComp& o = asOps(storage);
	        CompDsp& ops = o.dsp;
	        ops.sampleRate = std::max(sampleRate, 1.0);
	        forEachRamp(ops, [&](dyn::LinearRamp& r) { r.prepare(ops.sampleRate); });
	        ops.inPeak.prepare(ops.sampleRate);
	        ops.outPeak.prepare(ops.sampleRate);
	        ops.gr.prepare(ops.sampleRate);
	        ops.grA.prepare(ops.sampleRate);
	        ops.grB.prepare(ops.sampleRate);
	        // THE DELAY AND THE STAGES, SIZED FOR THIS RATE — the one place they allocate.
	        const uint32_t n = latencySamplesFor(ops.sampleRate);
	        ops.late.assign(n, CompDsp::Late {});
	        ops.pos = 0;
	        for (dsp::LookaheadLimiter* lim : { &ops.lrLim, &ops.msLim, &ops.stageLim }) {
		        lim->prepare(ops.sampleRate, n);
	        }
	        ops.lrBusy = ops.msBusy = ops.stageBusy = 0;
	        ops.refresh();
	        // THE LANE, AND THE PADS THAT WAIT FOR IT — sized only while a lane is built (a lane built later with a latent
	        // device in it moves the latency, and the host's restart re-prepares here).
	        o.lane.prepare(ops.sampleRate, std::max(maxFrames, CompDsp::kChunk));
	        if (o.lane.owned() != nullptr) {
		        const uint32_t laneLate = o.laneLatency(ops.sampleRate);
		        ops.dryPad.prepare(ops.sampleRate, laneLate);
		        ops.outPad.prepare(ops.sampleRate, laneLate);
	        }
        },
    .reset =
        [](void* storage) {
	        OpsComp& o = asOps(storage);
	        CompDsp& ops = o.dsp;
	        ops.clearDetector();
	        forEachRamp(ops, [](dyn::LinearRamp& r) { r.reset(); });
	        ops.inPeak.reset();
	        ops.outPeak.reset();
	        ops.gr.reset();
	        ops.grA.reset();
	        ops.grB.reset();
	        // NOTHING IN FLIGHT: the delay silent, every stage as if just switched on.
	        std::fill(ops.late.begin(), ops.late.end(), CompDsp::Late {});
	        ops.pos = 0;
	        for (dsp::LookaheadLimiter* lim : { &ops.lrLim, &ops.msLim, &ops.stageLim }) {
		        lim->reset();
	        }
	        ops.lrBusy = ops.msBusy = ops.stageBusy = 0;
	        ops.peakOverCeiling = 0.0f;
	        ops.dryPad.reset();
	        ops.outPad.reset();
	        if (o.lane.owned() != nullptr) {
		        o.lane.owned()->reset();
	        }
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, so absent fields land on their DECLARED defaults.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        CompDsp& ops = asOps(storage).dsp;
	        ops.state = stateOf(row, take);
	        ops.refresh();
        },
    // THE NOTE AND THE CLOCK REACH THE LANE — a keytracking filter or a synced delay inside it needs them.
    .setNoteContext = [](void* storage, const device::NoteContext& note) { asOps(storage).lane.setNoteContext(note); },
    .setTransportClock =
        [](void* storage, const device::TransportClock& clock) { asOps(storage).lane.setTransportClock(clock); },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        OpsComp& o = asOps(storage);
	        o.beginBlock();
	        o.dsp.process(left, right, frames);
	        o.endBlock();
        },
    // NO MONO ROAD: a stereo pair of detectors is the design, and the host hands a mono buffer as two.
    .processMono = nullptr,
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asOps(storage).dsp.sink = sink; },
    // THE AUDIO HALF OF THE OUTER GATE, cached for a lane not built yet.
    .setOuterSources =
        [](void* storage, const device::OuterSources& outer) { asOps(storage).lane.setOuterSources(outer); },
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    // NO WINDOWED CONTROLS (the plan): the plain `setFields` road, with every audio gain ramped.
    .windowedControls = {},
    .setFieldWindows = nullptr,
    // **THE SAME IN EVERY MODE AND WHETHER ENABLED OR NOT** (backlog LE): Limit's lookahead, Compress padded to it — and,
    // since backlog QG, the Compander lane's latency on top in every mode (the lane's devices' own declaration), so no
    // stored field can move it and no Mode flip asks the host to restart.
    .latencySamples = [](const void* storage, const float*, size_t, double sampleRate) -> uint32_t {
	    const double rate = std::max(sampleRate, 1.0);
	    return latencySamplesFor(rate) + (storage != nullptr ? asOps(storage).laneLatency(rate) : 0u);
    },
    .holdsTail = [](const void* storage) { return asOps(storage).holdsTail(); },
    // BACKLOG JO: the bus road hands the plugin's sidechain just before `processStereo`; only Detector: Side reads it.
    .setSidechain = [](void* storage, const float* left, const float* right, int frames, bool sounding) {
	    CompDsp& ops = asOps(storage).dsp;
	    ops.scL = left;
	    ops.scR = right;
	    ops.scFrames = frames;
	    ops.scSounding = sounding;
    },
};

} // namespace feathers::fx::comp
