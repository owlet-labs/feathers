/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#include "fx/delay/delay.h"
#include "fx/delay/delay_lanes.h"
#include "fx/delay/delay_types.h"

#include "backplane/axes.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/inner_chain_lanes.h"
#include "rack/lazy_lane.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <new>
#include <span>
#include <vector>

// THE DELAY AS A HOST SEES IT: the DSP (`Delay`, feathers-fx) and its FEEDBACK FX lane, a `rack::InnerChain` held
// here because this target can see the rack (`LoopInsert` in delay.h).
//
// **THE LANE IS LAZY.** Until something asks for it — `ensureFeedbackLane` from an editor, or a body whose
// lane holds devices — there is no chain and the Delay costs what it cost before the lane existed. It is built on the
// MESSAGE thread, adopted, gated, slotted and prepared there from what the host handed this storage, and only then
// PUBLISHED through one atomic pointer; the audio thread takes it at its next block and hands it the audio-thread
// state it caches (the outer sources, the clock, the note). Never freed while the device lives, so the audio thread's
// pointer can never dangle; never keyed by address, so nothing compares it across time.
//
// **A LANE HOLDING NOTHING IS NO LANE**: the DSP is handed a null insert and runs the exact loop it ran before
// (asserted by the MB/MD pinned hashes, which run through here with and without a built, empty lane).
//
// **NO LATENCY OF ITS OWN.** The lane's is absorbed inside the loop (the DSP writes each return that much earlier),
// so nothing reaches the output late and `latencySamples` stays unset.

namespace feathers::fx::delay {

namespace {

/// THE LANE'S AXES: no core sources or destinations yet (§1.7: the holder road is backlog ME's arc), SKLITTER's
/// sixteen slots.
constexpr backplane::Axes kLaneAxes {
	.coreSources = 0,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};

struct OpsDelay {
	Delay dsp {};

	// THE LANE'S MACHINERY IS `rack::LazyLane` (factored out for COMP's encode/decode lane, backlog QG — unchanged).
	rack::LazyLane lane { kLaneAxes, "feedback" }; // the spike recorder's path: `DELAY/feedback/...` (backlog SR)

	OpsDelay() = default;
	OpsDelay(const OpsDelay&) = delete;
	OpsDelay& operator=(const OpsDelay&) = delete;

	rack::InnerChain* ensure() { return lane.ensure(); }

	/// THE LANE FOR THIS BLOCK, AND ITS DRAIN: the newly published lane is handed the cached audio-thread state; then
	/// a zero-frame `process` drains its pending edits (and publishes its mirror), so the insert handed to the DSP
	/// describes the lane as it is this block.
	void beginBlock() {
		rack::InnerChain* const running = lane.take();
		LoopInsert insert {};
		if (running != nullptr) {
			// THE DRAIN AND THE LATENCY, ONCE FOR THE HOST BLOCK; the chunk calls inside it skip both, and the mirror is
			// published once at `endBlock` (`InnerChain::beginHostBlock`; the cost pass, 2026-11-06).
			running->beginHostBlock();
			// **A LANE WHOSE EVERY DEVICE IS SWITCHED OFF AND PARKED IS NO LANE** (a disabled device changed
			// the loop): its output is its input, so the DSP runs the bare loop, bit for bit — not the chunked road with its
			// DC blocker, limiter and the parked devices' alignment delay. Switching one back on is seen at once
			// (`allParked` reads the enable), the lane runs again, and the device un-parks and fades in.
			if (running->engine().chainLength(0) > 0 && !running->engine().allParked()) {
				insert.context = running;
				insert.process = [](void* context, float* left, float* right, int frames) {
					static_cast<rack::InnerChain*>(context)->process(left, right, frames);
				};
				insert.latency = running->hostBlockLatency();
			}
		}
		dsp.setLoopInsert(insert);
	}

	/// THE HOST BLOCK'S END: the lane's mirror published once, after every chunk of it ran.
	void endBlock() {
		if (rack::InnerChain* const running = lane.running(); running != nullptr) {
			running->endHostBlock();
		}
	}

	[[nodiscard]] bool holdsTail() const noexcept {
		return dsp.holdsTail() || (dsp.target().enabled && lane.holdsTail());
	}
};

OpsDelay& asOps(void* storage) {
	return *static_cast<OpsDelay*>(storage);
}

rack::LazyLane& laneOf(void* storage) {
	return asOps(storage).lane;
}

/// THE CONTAINER TABLE (`rack::LazyLaneOps`): the one lane under `kFeedbackLaneId`, the host's handovers kept for a lane
/// not built yet, and a body that holds devices building the lane before it loads.
using LaneOps = rack::LazyLaneOps<&laneOf, kFeedbackLaneId>;

void prepareAudio(void* storage, double rate, int maxFrames) {
	OpsDelay& d = asOps(storage);
	d.dsp.prepare(rate);
	d.lane.prepare(rate > 0.0 ? rate : 48000.0, std::max(maxFrames, kLoopChunkWide)); // Buckets' chunk (backlog PV)
}

} // namespace

rack::InnerChain* feedbackLaneOf(void* surface) noexcept {
	return surface != nullptr ? asOps(surface).lane.owned() : nullptr;
}

const rack::InnerChain* feedbackLaneOf(const void* surface) noexcept {
	return feedbackLaneOf(const_cast<void*>(surface));
}

rack::InnerChain* ensureFeedbackLane(void* surface) {
	return surface != nullptr ? asOps(surface).ensure() : nullptr;
}

const device::ContainerOps kContainerOps = LaneOps::make();

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsDelay),
    .stateAlign = alignof(OpsDelay),
    .construct = [](void* storage) { new (storage) OpsDelay(); },
    .destruct = [](void* storage) { asOps(storage).~OpsDelay(); },
    .prepare = prepareAudio,
    .reset =
        [](void* storage) {
	        OpsDelay& d = asOps(storage);
	        d.dsp.reset();
	        if (d.lane.owned() != nullptr) {
		        d.lane.owned()->reset();
	        }
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asOps(storage).dsp.setTarget(stateOf(row, take).settings);
        },
    // THE NOTE REACHES THE LANE — a keytracking filter inside it needs the pitch (SKLITTER's reason).
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        OpsDelay& d = asOps(storage);
	        d.dsp.setNoteContext(note); // Sync = Note (S6b)
	        d.lane.setNoteContext(note);
        },
    // **THE ORDER MATTERS**: a designated initialiser must follow declaration order and `-Werror` catches it.
    .setTransportClock =
        [](void* storage, const device::TransportClock& clock) {
	        OpsDelay& d = asOps(storage);
	        d.dsp.setTransportClock(clock);
	        d.lane.setTransportClock(clock);
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        OpsDelay& d = asOps(storage);
	        d.beginBlock();
	        d.dsp.processStereo(left, right, frames);
	        d.endBlock();
        },
    .processMono =
        [](void* storage, float* mono, int frames) {
	        OpsDelay& d = asOps(storage);
	        d.beginBlock();
	        d.dsp.processMono(mono, frames);
	        d.endBlock();
        },
    // THE TAPS STRIP'S ROAD (backlog LF) — the generic sink every host installs.
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asOps(storage).dsp.setReadingsSink(sink); },
    // THE AUDIO HALF OF THE OUTER GATE, cached for a lane not built yet.
    .setOuterSources =
        [](void* storage, const device::OuterSources& outer) {
	        asOps(storage).lane.setOuterSources(outer);
        },
    .holdsTail = [](const void* storage) { return static_cast<const OpsDelay*>(storage)->holdsTail(); },
};

/// TEST ONLY — the DSP inside an instance (`DeviceHost::containerSurfaceOf` or the storage a test built).
const Delay& dspOf(const void* storage) {
	return static_cast<const OpsDelay*>(storage)->dsp;
}

} // namespace feathers::fx::delay
