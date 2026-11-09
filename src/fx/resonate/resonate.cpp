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

#include "fx/resonate/resonate_dsp.h"
#include "fx/resonate/resonate_lanes.h"
#include "fx/resonate/resonate_types.h"

#include "backplane/axes.h"
#include "backplane/graph.h"
#include "device/latency.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/inner_chain_lanes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>

// RESONATE'S RENDER PATH, NOTE ROAD AND FX LANES — the ops a host calls, over one ResonateDsp (resonate_dsp.h has
// what it does and why). The two ops tables share ONE storage: RackEngine takes the first non-null ops and
// allocates once, so `kNoteOps`' stateBytes/stateAlign are `kAudioOps`' and both construct the same object
// (registry.h, Descriptor::notes).
//
// **THE FX LANES** (take the fx). Two `rack::InnerChain`s, SKLITTER's mechanism, held here and
// saved as one body (`rack::laneContainerOps`). The DSP's lane tap hands over, per sample, the dry, the wet's TONE
// part, the DRONE, and the Mix and Enable it applied; this file runs the lanes and does the mix:
//
//     Drone to Wet FX ON (the default):  drone -> DRONE FX -+
//                                        tone  -> (wait) ---+-> WET FX -> wet
//     Drone to Wet FX OFF:               tone  -> WET FX ---+
//                                        drone -> DRONE FX -+---------> wet
//     out = dry + Enable x ((1 - Mix) dry + Mix wet - dry)      — the DSP's own mix, unchanged
//
// **WITH BOTH LANES EMPTY IT IS THE DSP'S OWN OUTPUT, BIT FOR BIT, IN BOTH ROUTINGS** — an empty chain is a true pass
// (`InnerChain::process`), `tone + drone` is the DSP's own sum in its own order, and this TU is under the same
// no-contraction rule. Asserted (tests/test_resonate_lanes.cpp), not argued.
//
// **LATENCY**: a lane holding a latent device makes its path late. ON, the drone is late by DRONE FX + WET FX and the
// tone is held back by DRONE FX to meet it at WET FX's input; OFF, each path is held to the later of the two. Either
// way the dry waits for the wet, and `latencySamples` reports the total — which a routing flip changes when both
// lanes are latent (the host restarts, as for any latency change).
//
// **CHUNKED AT `kLaneChunk`**, so the tap is a fixed buffer in this storage and nothing allocates on the audio thread.
// The DSP is partition-invariant to the bit (test_fx_devices' partition sweep), so the chunking changes nothing.

namespace feathers::fx::resonate {

namespace {

constexpr int kLaneChunk = 256;

/// EACH LANE'S AXES: no core sources (Resonate publishes no outputs), SKLITTER's sixteen slots and strides.
constexpr backplane::Axes kLaneAxes {
	.coreSources = 0,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};

float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

struct OpsResonate {
	ResonateDsp dsp {};
	/// THE ONE COUNTER BOTH LANES ISSUE IDS FROM (gap G3), so an inner id names one device in one lane.
	std::unique_ptr<backplane::InstanceCounter> laneIds = std::make_unique<backplane::InstanceCounter>();
	std::unique_ptr<rack::InnerChain> droneLane = std::make_unique<rack::InnerChain>(kLaneAxes, laneIds.get());
	std::unique_ptr<rack::InnerChain> wetLane = std::make_unique<rack::InnerChain>(kLaneAxes, laneIds.get());
	bool droneToWet = true;
	double sampleRate = 48000.0;

	std::array<std::array<float, kLaneChunk>, 2> dry {};
	std::array<std::array<float, kLaneChunk>, 2> tone {};
	std::array<std::array<float, kLaneChunk>, 2> drone {};
	std::array<float, kLaneChunk> mix {};
	std::array<float, kLaneChunk> enable {};
	ResonateDsp::LaneTap tap { { dry[0].data(), dry[1].data() },
		                       { tone[0].data(), tone[1].data() },
		                       { drone[0].data(), drone[1].data() },
		                       mix.data(),
		                       enable.data() };
	/// The dry waits for the wet; the tone waits for the drone lane (ON) or for the later lane (OFF); the drone (OFF).
	device::CompensationDelay dryDelay, toneDelay, droneDelay;

	OpsResonate() {
		dsp.setLaneTap(&tap);
		droneLane->engine().setCostLane("drone"); // the spike recorder's paths: `RESONATE/drone/DELA` (backlog SR)
		wetLane->engine().setCostLane("wet");
	}
	OpsResonate(const OpsResonate&) = delete;
	OpsResonate& operator=(const OpsResonate&) = delete;

	void fields(const float* fields, size_t fieldsPresent) {
		// THE DEVICE DECODES ITS OWN ROW, so absent fields land on their DECLARED defaults.
		Row row {};
		const size_t take = std::min(fieldsPresent, row.size());
		for (size_t i = 0; i < take; ++i) {
			row[i] = fields[i];
		}
		const Settings s = stateOf(row, take);
		droneToWet = s.droneToWetFx != 0;
		dsp.setSettings(s);
	}

	void prepare(double rate, int maxFrames) {
		sampleRate = rate;
		dsp.prepare(rate);
		// THE LANES ARE CHAINS, AND A CHAIN SIZES ITS SCRATCH TO THE HOST'S LONGEST BLOCK (backlog GO).
		droneLane->prepare(rate, maxFrames);
		wetLane->prepare(rate, maxFrames);
		dryDelay.prepare(rate);
		toneDelay.prepare(rate);
		droneDelay.prepare(rate);
	}

	void reset() {
		dsp.reset();
		droneLane->reset();
		wetLane->reset();
		dryDelay.reset();
		toneDelay.reset();
		droneDelay.reset();
	}

	void process(float* left, float* right, int frames) {
		for (int done = 0; done < frames;) {
			const int n = std::min(kLaneChunk, frames - done);
			processChunk(left + done, right + done, n);
			done += n;
		}
	}

	/// **WHETHER ANYTHING HELD WILL STILL COME OUT** (backlog MC): the Cycle, and either lane's devices, while switched
	/// on (off, the output is the dry alone). The compensation delays are the latency this device reports, which the
	/// host waits out on its own.
	[[nodiscard]] bool holdsTail() const noexcept {
		return dsp.enabled() && (dsp.holdsTail() || droneLane->holdsTail() || wetLane->holdsTail());
	}

	void processChunk(float* left, float* right, int n) {
		// THE DSP, ITS OWN MIX DISCARDED: everything this file needs comes back through the tap.
		dsp.process(left, right, n);
		float* tL = tone[0].data();
		float* tR = tone[1].data();
		float* dL = drone[0].data();
		float* dR = drone[1].data();
		// EACH LANE'S LATENCY IS READ AFTER IT RAN: `process` drains the lane's pending edits first, so a device added
		// this block is already counted (the chain aligns its own dry by the same reading).
		droneLane->process(dL, dR, n);
		const uint32_t ld = droneLane->latencySamples(sampleRate);
		uint32_t total = 0;
		if (droneToWet) {
			toneDelay.process(tL, tR, n, ld);
			droneDelay.process(dL, dR, n, 0);
			for (int i = 0; i < n; ++i) {
				tL[i] = tL[i] + dL[i];
				tR[i] = tR[i] + dR[i];
			}
			wetLane->process(tL, tR, n);
			total = ld + wetLane->latencySamples(sampleRate);
		}
		else {
			wetLane->process(tL, tR, n);
			const uint32_t lw = wetLane->latencySamples(sampleRate);
			total = std::max(ld, lw);
			toneDelay.process(tL, tR, n, total - lw);
			droneDelay.process(dL, dR, n, total - ld);
			for (int i = 0; i < n; ++i) {
				tL[i] = tL[i] + dL[i];
				tR[i] = tR[i] + dR[i];
			}
		}
		float* xL = dry[0].data();
		float* xR = dry[1].data();
		dryDelay.process(xL, xR, n, total);
		// THE DSP'S OWN MIX, term for term (resonate_dsp.cpp, "THE WET").
		for (int i = 0; i < n; ++i) {
			const float m = mix[static_cast<size_t>(i)];
			const float e = enable[static_cast<size_t>(i)];
			const float mixedL = (1.0f - m) * xL[i] + m * tL[i];
			const float mixedR = (1.0f - m) * xR[i] + m * tR[i];
			left[i] = finiteOr0(xL[i] + e * (mixedL - xL[i]));
			right[i] = finiteOr0(xR[i] + e * (mixedR - xR[i]));
		}
	}

	[[nodiscard]] uint32_t latency(const float* fields, size_t fieldsPresent) const {
		const auto at = static_cast<size_t>(Param::DroneToWetFx);
		const bool on = at < fieldsPresent && std::isfinite(fields[at]) ? fields[at] >= 0.5f
		                                                                : kParamDescs[at].def >= 0.5;
		const uint32_t ld = droneLane->latencySamples(sampleRate);
		const uint32_t lw = wetLane->latencySamples(sampleRate);
		return on ? ld + lw : std::max(ld, lw);
	}
};

OpsResonate& asResonate(void* storage) {
	return *static_cast<OpsResonate*>(storage);
}

void construct(void* storage) {
	new (storage) OpsResonate();
}
void destruct(void* storage) {
	asResonate(storage).~OpsResonate();
}
void prepareAudio(void* storage, double sampleRate, int maxFrames) {
	asResonate(storage).prepare(sampleRate, maxFrames);
}
// THE NOTE TABLE CARRIES NO LONGEST BLOCK (`NoteOps::prepare`); a rack prepares this object once, through the audio
// table, and never through this one (`rack_engine.cpp`'s fixed order: audio, mod, notes).
void prepare(void* storage, double sampleRate) {
	asResonate(storage).prepare(sampleRate, device::kDefaultMaxFrames);
}
void setFields(void* storage, const float* fields, size_t fieldsPresent) {
	asResonate(storage).fields(fields, fieldsPresent);
}

/// THE TWO LANES, AS THE BODY CODEC AND THE CONTAINER TABLE SEE THEM — stable ids (`Lane`), never positions.
std::array<rack::BundleLane, 2> lanesOf(void* storage) {
	auto& d = asResonate(storage);
	return { rack::BundleLane { static_cast<uint16_t>(Lane::Drone), d.droneLane.get() },
		     rack::BundleLane { static_cast<uint16_t>(Lane::Wet), d.wetLane.get() } };
}

template <class Fn>
void forEachLane(void* storage, Fn&& fn) {
	for (const rack::BundleLane& lane : lanesOf(storage)) {
		if (lane.chain != nullptr) {
			fn(*lane.chain);
		}
	}
}

} // namespace

rack::InnerChain* laneChainOf(void* surface, Lane lane) noexcept {
	if (surface == nullptr) {
		return nullptr;
	}
	auto& d = asResonate(surface);
	return lane == Lane::Drone ? d.droneLane.get() : d.wetLane.get();
}

const rack::InnerChain* laneChainOf(const void* surface, Lane lane) noexcept {
	return laneChainOf(const_cast<void*>(surface), lane);
}

bool droneToWetFxForTest(const void* surface) noexcept {
	return surface != nullptr && static_cast<const OpsResonate*>(surface)->droneToWet;
}

const device::ContainerOps kContainerOps = rack::laneContainerOps<&lanesOf>();

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsResonate),
    .stateAlign = alignof(OpsResonate),
    .construct = construct,
    .destruct = destruct,
    .prepare = prepareAudio,
    .reset = [](void* storage) { asResonate(storage).reset(); },
    .setFields = setFields,
    // THE NOTE REACHES THE LANES — a keytracking filter inside one needs the pitch (SKLITTER's reason).
    .setNoteContext = [](void* storage, const device::NoteContext& note) {
	    forEachLane(storage, [&](rack::InnerChain& c) { c.setNoteContext(note); });
    },
    .setTransportClock =
        [](void* storage, const device::TransportClock& clock) {
	        asResonate(storage).dsp.setTransportClock(clock);
	        forEachLane(storage, [&](rack::InnerChain& c) { c.setTransportClock(clock); });
        },
    .processStereo = [](void* storage, float* left, float* right,
                        int frames) { asResonate(storage).process(left, right, frames); },
    // NO MONO ROAD: the string's two pickups make it stereo by nature; the host hands a mono buffer as two.
    .processMono = nullptr,
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asResonate(storage).dsp.setSink(sink); },
    // THE AUDIO HALF OF THE OUTER GATE (`ContainerOps::setOuterGate` is the message-thread half), to both lanes.
    .setOuterSources =
        [](void* storage, const device::OuterSources& outer) {
	        forEachLane(storage, [&](rack::InnerChain& c) { c.setOuterSources(outer); });
        },
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    .windowedControls = {},
    .setFieldWindows = nullptr,
    // THE LANES' LATENCY, AS THE ROUTING COMPOSES IT — see the banner.
    .latencySamples = [](const void* storage, const float* fields, size_t fieldsPresent, double) -> uint32_t {
	    return static_cast<const OpsResonate*>(storage)->latency(fields, fieldsPresent);
    },
    .setFieldsR = nullptr,
    .holdsTail = [](const void* storage) { return static_cast<const OpsResonate*>(storage)->holdsTail(); },
};

const device::NoteOps kNoteOps {
    .stateBytes = sizeof(OpsResonate),
    .stateAlign = alignof(OpsResonate),
    .construct = construct,
    .destruct = destruct,
    .prepare = prepare,
    .setFields = setFields,
    // **A READER, SO A PASS-THROUGH**: every note continues down the chain untouched (in the synth the
    // voices play what leaves the lane), and a copy is queued for the next process call, where it moves
    // a head. Called with no notes at every piece start, which is a no-op here.
    .processNotes =
        [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
           size_t* outCount) {
	        const size_t n = std::min(inCount, outCapacity);
	        std::copy(in, in + n, out);
	        *outCount = n;
	        asResonate(storage).dsp.queueNotes(in, inCount);
        },
};

const ResonateDsp& dspForTest(const void* storage) {
	return static_cast<const OpsResonate*>(storage)->dsp;
}

ResonateDsp& dspForTest(void* storage) {
	return static_cast<OpsResonate*>(storage)->dsp;
}

} // namespace feathers::fx::resonate
