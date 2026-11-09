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

#include "plugin/lane_rack.h"

#include "device/latency.h"

#include "backplane/block_context.h"
#include "plugin/device_registry.h"

#include <algorithm>
#include <span>

namespace feathers {

bool LaneRack::fits(const device::Descriptor& desc, size_t chain) {
	// **THE FX LANE RUNS EVERY KIND; THE STRIP RUNS MODULATORS AND NOTE DEVICES.** The lane once kept to audio
	// devices — `FxRunner`'s half of the split it and `ModRunner` each kept — and that split was never what the
	// engine needed: it ticks the modulators and walks the notes of every chain., so the lane is the standalone rack's chain
	// (`Converger::fitsAnywhere`), and a Tracker in it governs the bus devices after it.
	// A CONTAINER THAT CARRIES NOTES (a Rack, backlog RN) is an fx-lane device still, never a strip one.
	return chain == kLaneChain ? maySitInLane(desc)
	                           : (desc.mod != nullptr || (desc.notes != nullptr && desc.container == nullptr));
}

const ModRack& LaneRack::noStrip() noexcept {
	static const ModRack kNone = [] {
		ModRack r {};
		r.count = 0;
		return r;
	}();
	return kNone;
}

LaneRack::LaneRack() : engine_(feathersAxes(), hostCatalogue()), converger_(engine_, &LaneRack::fits) {
	// **THE EMPTY STRIP IS BUILT HERE, NOT ON FIRST USE** (unbounded devices): a `ModRack` holds a vector now, so the
	// function-local static behind `noStrip()` allocates when it is first constructed — and its first use is a
	// `sync`, on the audio thread. Measured: a case that ran it first counted one allocation there.
	(void)noStrip();
}

// THE CONVERGER DIES FIRST (declared after the engine): what is in flight goes, then the engine.
LaneRack::~LaneRack() = default;


void LaneRack::prepare(double sampleRate, size_t voices, int maxFrames) {
	// A PROVISION NOT YET ADOPTED WAS PREPARED AT THE OLD RATE, and adopting it would run a device at
	// a rate that is not the host's. It goes, and the mirror forgets it, so the next `provision` builds
	// it again at this one.
	converger_.dropPending();
	engine_.setVoices(voices);
	engine_.prepare(sampleRate, maxFrames);
	// THIS LANE COMPENSATES LATENT PER-VOICE DEVICES up to `device::perVoiceLatencyBound` in all: the Synth delays each voice's
	// gain run and its free by `perVoiceLatencySamples`, in rings that size.
	engine_.setLatentPerVoiceCap(device::perVoiceLatencyBound(sampleRate));
}

size_t LaneRack::provision(const FxChain& chain, const ModRack& strip, ContainerBodies* bodies) {
	// ROOM FOR CHAINS HANDED IN BY VALUE (unbounded devices step 5) — this is the message thread. The audio
	// thread's twin, `syncState_`, is sized once: the by-value road is a case's and a bridgeless synth's, and the
	// product's road is the published state, which grows at its own drain.
	if (const size_t need = std::max(fxChainLength(chain), std::min(strip.count, strip.device.size()));
	    need > provisionState_.capacity()) {
		provisionState_ = rack::ChainState(std::max(need, 2 * provisionState_.capacity()), provisionState_.chainCount());
	}
	stateOfChains(chain, strip, &provisionState_);
	// NO PUBLISH TO NAME — chains handed in as values (a case, a synth with no bridge) are not a
	// published state, so an unmatched arrival goes back and the reap has it rebuilt.
	return converger_.provision(provisionState_, bodies, 0);
}

size_t LaneRack::provision(const rack::ChainState& state, uint32_t forPublish, ContainerBodies* bodies) {
	// THE RECORD'S STATE, NAMING THE PUBLISH IT WENT OUT AS — so a sync still on an older one keeps
	// what this builds rather than handing it back (see `Converger::provision`).
	return converger_.provision(state, bodies, forPublish);
}

void LaneRack::reap() noexcept {
	converger_.reap();
}

void LaneRack::sync(const FxChain& chain, const ModRack& strip, const device::NoteContext& busNote,
                    const device::TransportClock& clock) {
	stateOfChains(chain, strip, &syncState_);
	sync(syncState_, 0, busNote, clock); // chains as values: no publish to name
}

void LaneRack::sync(const rack::ChainState& state, uint32_t published, const device::NoteContext& busNote,
                    const device::TransportClock& clock) {
	(void)converge(state, published);
	begin(busNote, clock);
}

bool LaneRack::converge(const rack::ChainState& state, uint32_t published) {
	// THE SEGMENT BEFORE THIS ONE ENDS HERE, so everything below happens with no block in flight — a
	// removal parked now is reapable as soon as the message thread next looks.
	finish();

	// **THE ENGINE BECOMES THE STATE** — arrivals and departures, the order (the strip's chain first, then
	// the lane's; which routes are legal is the graph's cycle test, not this order — backlog IZ), and every BASE row, whose modulation the engine
	// resolves through Feathers' published edges (`setRouting`). See `rack::Converger`. `published` is
	// the publish `state` is a copy of: an arrival built for a later one is kept for the sync that has it.
	//
	// **WHETHER THE ENGINE'S LAYOUT MOVED IS THE ANSWER** — which device sits in which of the engine's own
	// slots (kill-lanes step 3a: the engine chooses). A caller that compiled anything against that layout
	// compiles again before the segment begins.
	const uint32_t before = engine_.layoutRevision();
	converger_.converge(state, published);
	converged_ = &state; // what names each device now — see `slotOfStateLane`
	const bool moved = engine_.layoutRevision() != before;

	// THE METER CONNECTOR, by POSITION in the fx chain — see `setDspConnector`. Each device found where the
	// ENGINE keeps it, by its id.
	if (connector_) {
		const std::span<const rack::DeviceState> lanePath = state.chain(kLaneChain);
		for (size_t position = 0; position < lanePath.size(); ++position) {
			const DeviceInstanceId id = lanePath[position].instanceId;
			const device::Descriptor* desc = converger_.runningDescriptorOf(id);
			const size_t slot = engine_.slotOf(id);
			if (desc == nullptr || slot == rack::RackEngine::kNoSlot) {
				continue;
			}
			const rack::Slot& s = engine_.slotAtLane(slot);
			if (s.storage != nullptr) {
				connector_(desc->traits.tag, position, s.storage.get());
			}
		}
	}
	return moved;
}

bool LaneRack::converge(const FxChain& chain, const ModRack& strip) {
	stateOfChains(chain, strip, &syncState_);
	return converge(syncState_, 0); // chains as values: no publish to name
}

void LaneRack::begin(const device::NoteContext& busNote, const device::TransportClock& clock) {
	clock_ = clock;
	// THE NEXT SEGMENT BEGINS: the note the bus follows, and the song's clock. No samples are owed to
	// the modulators here — the strip ticks on Feathers' control grid (`tickModulators`).
	engine_.setExternalNote(busNote);
	engine_.beginBlock(0, backplane::BlockContext { clock });
	open_ = true;
}

void LaneRack::finish() noexcept {
	if (open_) {
		engine_.endBlock();
		open_ = false;
	}
}

void LaneRack::tickModulators(const device::ModTickContext& context, int frames) {
	// THE GATE AND THE RETRIGGER THROUGH THE ENGINE'S OWN DOORS: `setGate` derives an edge from a rising
	// gate and `retrigger` latches one explicitly — a note under a held gate — and both are the same
	// latch, spent by the first tick that sees it.
	engine_.setGate(context.gate);
	if (context.retrigger) {
		engine_.retrigger();
	}
	engine_.tickModulators(frames, backplane::BlockContext { context.clock });
}

bool LaneRack::playheadOf(DeviceInstanceId device, uint32_t* ordinal, float* phase) const {
	if (ordinal == nullptr || phase == nullptr) {
		return false;
	}
	return engine_.playheadAt(engine_.slotOf(device), *ordinal, *phase);
}

bool LaneRack::playheadLevelOf(DeviceInstanceId device, float* level) const {
	return level != nullptr && engine_.playheadLevelAt(engine_.slotOf(device), *level);
}

uint64_t LaneRack::extraStateOf(DeviceInstanceId device, float* extra, size_t capacity, size_t* count) const {
	return engine_.extraStateAt(engine_.slotOf(device), extra, capacity, count);
}

bool LaneRack::trigFiresOf(DeviceInstanceId device, uint32_t* fires) const {
	return fires != nullptr && engine_.trigFiresAt(engine_.slotOf(device), *fires);
}

size_t LaneRack::recentEventsOf(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const {
	return engine_.recentEventsAt(engine_.slotOf(device), out, capacity);
}

const void* LaneRack::busStorageForTag(const char* tag, bool running) const {
	for (size_t position = 0; position < engine_.chainLength(); ++position) {
		const size_t lane = engine_.deviceAtPosition(position);
		if (lane == rack::RackEngine::kNoSlot) {
			continue;
		}
		const rack::Slot& slot = engine_.slotAtLane(lane);
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || !device::sameTag(d->traits.tag, tag)) {
			continue;
		}
		if (running && engine_.polyForTest(lane)) {
			continue; // in its per-voice arrangement: the bus is not running this one
		}
		return slot.storage.get();
	}
	return nullptr;
}

const void* LaneRack::voiceStorageForTag(size_t voice, const char* tag) const {
	for (size_t position = 0; position < engine_.chainLength(); ++position) {
		const size_t lane = engine_.deviceAtPosition(position);
		if (lane == rack::RackEngine::kNoSlot || !engine_.polyForTest(lane)) {
			continue;
		}
		const rack::Slot& slot = engine_.slotAtLane(lane);
		const device::Descriptor* d = slot.live_desc();
		if (d != nullptr && device::sameTag(d->traits.tag, tag) && voice < slot.voices.size()) {
			return slot.voices[voice].get();
		}
	}
	return nullptr;
}

} // namespace feathers
