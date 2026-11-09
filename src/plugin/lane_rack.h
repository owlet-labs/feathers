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

#pragma once

#include "backplane/axes.h"
#include "device/registry.h"
#include "backplane/container_bodies.h"
#include "plugin/feathers_chains.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "rack/chain_state.h"
#include "rack/converger.h"
#include "rack/rack_engine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>

namespace feathers {

/// **FEATHERS' DEVICES — THE FX LANE AND THE MOD STRIP — RUN ON THE RACK ENGINE** — the same
/// `rack::RackEngine` the standalone rack plugin is and the rack device holds. The lane, the standalone
/// effect and the chain inside a chain are one piece of code; this is Feathers joining it.
///
/// IT REPLACED TWO `FxRunner`s AND A `ModRunner` — a bus runner, a per-voice runner and a modulator
/// runner, three chain implementations beside the rack's. The engine runs the per-voice arrangement
/// (`RackEngine::setVoices`), the modulators on Feathers' control tick (`tickModulators`) and the note
/// lane at the instant a note arrives (`processNotes`), so one engine is all of it. **ITS SLOTS ARE ITS
/// OWN** (kill-lanes step 3a): the engine keeps each device, under the record's id, in whichever slot it
/// chose at adoption — not at the record's lane — and every per-slot road here is in those slots.
///
/// **THE ENGINE BECOMES WHAT THE RECORD SAYS, THROUGH `rack::Converger`** — the one road every host
/// of the engine takes. Feathers' desired state is a
/// `rack::ChainState` of two chains, NUMBERED IN RANK ORDER: chain 0 the strip (modulators and note
/// devices), chain 1 the fx lane (audio devices, and modulators and note devices too since 2026-10-04).
/// Each chain is its ordered run of devices — no lanes (kill-lanes step 3b).
///
/// **THE BRIDGE'S RECORD IS WHAT THIS READS** (chain-as-a-device 5b, C4–C5): the plugin's visit provisions
/// from the record's state naming its publish, and the synth syncs from the snapshot it took, naming the
/// same — so an arrival from a publish the block has not taken yet is kept, not handed back. The
/// `FxChain`/`ModRack` overloads remain for a caller with chains as values (a case, a synth with no
/// bridge); they build the state here (`stateOfChains`) and name no publish.
///
/// **MODULATION IS FEATHERS' OWN, RESOLVED BY THE ENGINE.** The engine runs on Feathers' axes
/// (`feathersAxes()`), so a destination's coordinate here is the one `ParamSet` publishes for it, and
/// it resolves the lane's devices through the routes the synth hands it by identity (`setHostRoutes`): each
/// node's inputs point at the cells its sources own — no bus, no mask; a node reads what its upstream
/// published (kill-lanes §2.4b). `Synth::modulatedRowAt`, the second resolve that used to run beside it, is gone.
///
/// THREADS: `provision`, `reap`, `prepare` and the setters are the MESSAGE thread; `sync` and everything
/// between it and `finish` are the AUDIO thread, and `sync` is a drain point — nothing else touches the
/// engine's chain while it runs.
class LaneRack {
public:
	LaneRack();
	~LaneRack();
	LaneRack(const LaneRack&) = delete;
	LaneRack& operator=(const LaneRack&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                               //
	//--------------------------------------------------------------------------------------------//

	/// WHERE THE LANE'S DEVICES COME FROM. Feathers' own list by default — Feathers is the host — and
	/// the plugin entry point may hand another. Before provisioning.
	void setCatalogue(const device::Catalogue& catalogue) noexcept { engine_.setCatalogue(catalogue); }

	/// **THE HOST SLOTS' BUS** (T6b) — see `RackEngine::setHostSlotBus`. Before activate (no block running).
	void setHostSlotBus(rack::RackEngine::SlotBus* bus) noexcept { engine_.setHostSlotBus(bus); }

	/// **THE ROUTES, BY IDENTITY, HANDED TO THE ENGINE WHOLE — and the host node's output cells with them**
	/// (kill-lanes §2.4b). `ParamSet` keeps the staged snapshot and the cells; the synth hands both here at
	/// its drain and the engine builds every device node's inputs from them. The bus this class kept —
	/// one flat array of every source per reading slot, filled and masked each tick — is gone: a node
	/// reads the cells its inputs point at.
	void setHostRoutes(std::span<const backplane::Route> routes) noexcept { engine_.setHostRoutes(routes); }
	void setHostRoutes(const backplane::RouteRun& routes) noexcept { engine_.setHostRoutes(routes); }
	/// ROOM FOR `routes` IN THE ENGINE'S ROUTE STORES — message thread, allocates (backlog OJ). See `RackEngine::reserveRoutes`.
	void reserveRoutes(size_t routes) { engine_.reserveRoutes(routes); }
	[[nodiscard]] size_t routeRoomRefusals() const noexcept { return engine_.routeRoomRefusals(); }

	/// **THE HOST NODE'S OUTPUTS, PUSHED INTO THE ENGINE'S OWN CELLS** — one per Feathers source, macros
	/// then weights, after `ParamSet::publishCoreSources` and before the lane ticks, so a route from a
	/// macro reads this block's value with no delay. This replaced a span of `ParamSet`'s cells that the
	/// engine's inputs pointed straight into: twelve float copies per
	/// control tick, for one road instead of two to "where does a rack's macro come from".
	void setCoreSources(std::span<const float> cells) noexcept {
		for (size_t i = 0; i < cells.size(); ++i) {
			engine_.setMacro(i, cells[i]);
		}
	}

	/// **WHERE A DEVICE'S OUTPUT CELLS ARE**, for the synth's own node (its parameters) to point its inputs
	/// at — the collapsed cell, and the per-voice run for an output that declares `perVoice`. Null when the
	/// engine does not run the device or the device does not declare the output. Valid until the next
	/// drain that moves a device.
	[[nodiscard]] const float* outputCellOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputCellOf(device, output);
	}
	[[nodiscard]] const float* outputWindowForVoiceOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputWindowForVoiceOf(device, output);
	}
	[[nodiscard]] const float* outputWindowOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputWindowOf(device, output);
	}
	/// THE RIGHT EAR OF A BLOCK-RATE OUTPUT — cell, tick window, and the byte saying whether it is a pair now (backlog KU).
	[[nodiscard]] const float* outputCellROf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputCellROf(device, output);
	}
	[[nodiscard]] const float* outputWindowROf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputWindowROf(device, output);
	}
	[[nodiscard]] const uint64_t* outputPairsOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputPairsOf(device, output);
	}
	/// The engine's inputs and stereo-live fixed point, rebuilt now if stale — before the synth builds its own.
	void settleInputs() noexcept { engine_.settleInputs(); }
	[[nodiscard]] const float* outputCellForVoiceOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputCellForVoiceOf(device, output);
	}
	/// AND WHICH NOTE EACH VOICE HOLDS (backlog MV, B) — see `RackEngine::setVoiceNote`.
	void setVoiceNote(size_t voice, int32_t noteId, int16_t channel, int16_t key) noexcept {
		engine_.setVoiceNote(voice, noteId, channel, key);
	}
	void clearVoiceNote(size_t voice) noexcept { engine_.clearVoiceNote(voice); }
	/// THE SYNTH WRITES A PER-VOICE READING — it knows its voices and the note each holds; the engine
	/// knows neither.
	void writeOutputForVoice(DeviceInstanceId device, size_t output, size_t voice, float value) noexcept {
		engine_.writeOutputForVoice(device, output, voice, value);
	}
	/// A VOICE STARTS A NOTE (backlog FF): its reading, from this sample — see `RackEngine::startOutputForVoice`.
	void startOutputForVoice(DeviceInstanceId device, size_t output, size_t voice, float value) noexcept {
		engine_.startOutputForVoice(device, output, voice, value);
	}
	/// AND ITS RIGHT EAR WHILE THE OUTPUT IS A PAIR (backlog PA) — see `RackEngine::writeOutputPairForVoice`.
	void writeOutputPairForVoice(DeviceInstanceId device, size_t output, size_t voice, int32_t noteId, int16_t channel,
	                             int16_t key, bool start) noexcept {
		engine_.writeOutputPairForVoice(device, output, voice, noteId, channel, key, start);
	}
	[[nodiscard]] const float* outputWindowRForVoiceOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputWindowRForVoiceOf(device, output);
	}

	/// THE METER CONNECTOR, called for every device the bus runs, on every sync, with the device's
	/// POSITION in the chain — `FxRunner`'s contract, which the bridge's meter rows are keyed on. Every
	/// sync rather than at adoption because the editor installs it after the lane already holds devices.
	void setDspConnector(std::function<void(const char* tag, size_t position, void* storage)> connector) {
		connector_ = std::move(connector);
	}

	/// THE RATE, THE VOICE COUNT AND THE HOST'S LONGEST BLOCK. While inactive. Live devices are re-prepared in place — a device
	/// keeps its instance across a reactivation — and anything provisioned at the old rate but not yet
	/// adopted is dropped and rebuilt by the next `provision`. `voices` reaches devices built from here
	/// on; Feathers' count is a constant, so no live device ever holds a different number.
	void prepare(double sampleRate, size_t voices, int maxFrames);

	/// BUILD WHAT THE CHAIN WANTS AND THE LANE DOES NOT HAVE, and post it for the next `sync`. A device
	/// is rebuilt when its KIND, its TAG, its INSTANCE ID or — for a container — its body's generation
	/// changed, and for nothing else: a knob moving keeps the instance and its memory. Returns how many
	/// were built. Reaps first.
	size_t provision(const FxChain& chain, const ModRack& strip, ContainerBodies* bodies = nullptr);
	/// THE SAME, FROM THE BRIDGE'S RECORD — its state, and the publish it went out as
	/// (`PublishedChainState::sequence`), which is what lets a sync still on an older publish keep an
	/// arrival rather than hand it back.
	size_t provision(const rack::ChainState& state, uint32_t forPublish, ContainerBodies* bodies);
	/// THE HOST OUTSIDE FOR A CHAIN THIS LANE PROVISIONS — rack roadmap §7; forwarded to the converger.
	void setOuterGateMaker(device::OuterGate (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
		converger_.setOuterGateMaker(make, host);
	}
	/// EVERY CHAIN IN THE LANE'S OWN SERVICE — `Converger::serviceContainers` (host slots T11).
	void serviceContainers(const rack::ChainState& state, ContainerBodies* bodies) {
		converger_.serviceContainers(state, bodies);
	}
	/// THE HOST SLOT POOL FOR A CHAIN THIS LANE PROVISIONS — host slots T11; forwarded to the converger.
	void setSlotLinkMaker(device::HostSlotLink (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
		converger_.setSlotLinkMaker(make, host);
	}
	/// **EVERY CHAIN IN THE LANE UNHOOKED FROM ITS HOST — the outer gate and the slot link** (rack-teardown-uaf). Both
	/// point at the plugin's bridge, which dies before the synth that holds this lane; called first in the plugin's
	/// destructor, so a nested rack's teardown asks the bridge nothing. See `rack::Converger::unhookSlotLinks`.
	void unhookHostLinks() noexcept {
		converger_.unhookOuterGates();
		converger_.unhookSlotLinks();
	}
	/// What the chains in this lane dropped at their service since last taken — see `Converger::takeOuterDrops`.
	[[nodiscard]] rack::RouteRefusals takeOuterDrops() noexcept { return converger_.takeOuterDrops(); }
	/// THE LANE ALONE — an empty strip. For a host (or a case) with no modulators.
	size_t provision(const FxChain& chain, ContainerBodies* bodies = nullptr) {
		return provision(chain, noStrip(), bodies);
	}
	/// A STRIP HOLDING NOTHING.
	static const ModRack& noStrip() noexcept;

	/// DESTROY WHAT THE AUDIO THREAD HAS FINISHED WITH — the engine's parked devices, and provisions it
	/// adopted (husks) or declined.
	void reap() noexcept;

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD                                                                                 //
	//--------------------------------------------------------------------------------------------//

	/// THE CHAIN IS SETTLED FOR THE NEXT SEGMENT: a block still open from this render ends; arrivals
	/// are adopted and unwanted devices removed; the engine's order is made the chain's; every row is
	/// written; the connector runs; the next block begins with the bus note and the clock. Called
	/// wherever Feathers settles its parameters — once per render, and again on a motion tick when an
	/// envelope is routed — so a device's arrangement is decided per segment, as `FxRunner`'s was.
	void sync(const FxChain& chain, const ModRack& strip, const device::NoteContext& busNote,
	          const device::TransportClock& clock);
	void sync(const FxChain& chain, const device::NoteContext& busNote, const device::TransportClock& clock) {
		sync(chain, noStrip(), busNote, clock);
	}
	/// THE SAME, FROM A SNAPSHOT OF THE BRIDGE'S PUBLISHED STATE and the publish it is a copy of.
	void sync(const rack::ChainState& state, uint32_t published, const device::NoteContext& busNote,
	          const device::TransportClock& clock);
	/// `sync` IN ITS TWO HALVES, for a caller that must act between them: the engine becomes `state`
	/// (answering whether any device changed ENGINE SLOT, which a plan compiled against the engine's layout
	/// must be compiled again for), then the next segment begins. AUDIO THREAD, a drain point.
	bool converge(const rack::ChainState& state, uint32_t published);
	/// The same, from chains handed in as values (no publish to name).
	bool converge(const FxChain& chain, const ModRack& strip);
	void begin(const device::NoteContext& busNote, const device::TransportClock& clock);

	/// **WHERE THE ENGINE KEEPS DEVICE `id`**, or `kNoSlot` — the engine's own slot (kill-lanes step 3a), the
	/// index every per-slot road on this class (`outputAt`, `playheadAt`, `drivePeakAtLane`) and
	/// the plan `ParamSet` compiles are in. It is not the record's lane and must not be mixed with one.
	[[nodiscard]] size_t slotOf(DeviceInstanceId id) const noexcept { return engine_.slotOf(id); }

	/// **WHERE THE ENGINE KEEPS THE DEVICE THE LAST CONVERGED STATE HAS AT `position` OF `chain`** — the
	/// state names each device (the record's id from a publish, its own from chains handed in as values), so
	/// a caller holding a place in a chain asks this rather than reproducing the naming rule. `kNoSlot`
	/// before the first converge, for a place nothing stands at, or for a device the engine does not run.
	[[nodiscard]] size_t slotOfStatePosition(size_t chain, size_t position) const noexcept {
		const DeviceInstanceId id = deviceOfStatePosition(chain, position);
		return id == kNoInstance ? rack::RackEngine::kNoSlot : engine_.slotOf(id);
	}
	/// AND THE WAY BACK: the device the engine runs in its slot `slot`, or `kNoInstance`.
	[[nodiscard]] DeviceInstanceId deviceAt(size_t slot) const noexcept {
		return slot < engine_.slotCount() && engine_.slotAtLane(slot).live() ? engine_.slotAtLane(slot).id : kNoInstance;
	}
	/// **THE DEVICE THE LAST CONVERGED STATE HAS AT `position` OF `chain`** — the state's own name for it,
	/// whatever road the state came from. A caller holding a place in a chain asks this and then speaks in
	/// ids. `kNoInstance` before the first converge and for a place nothing stands at.
	[[nodiscard]] DeviceInstanceId deviceOfStatePosition(size_t chain, size_t position) const noexcept {
		return converged_ == nullptr ? kNoInstance : converged_->at(chain, position).instanceId;
	}

	/// THE RENDER IS OVER: the open block ends, so what it parked becomes reapable.
	void finish() noexcept;

	/// **THE STRIP ADVANCES ONE CONTROL TICK** — `frames` samples owed to every modulator, each ticked
	/// at its own declared rate, with the tick's gate, retrigger and clock. Each
	/// modulator's row is resolved first, against its lane's bus — which the synth fills before this.
	void tickModulators(const device::ModTickContext& context, int frames);

	/// **THE NOTE LANE, AT THE INSTANT A NOTE ARRIVES** — every note device in chain order, `out` holding
	/// what left it (up to `capacity`). A pass-through when nothing claims the stream.
	size_t processNotes(const device::NoteEvent* in, size_t count, device::NoteEvent* out, size_t capacity) {
		return engine_.processNotesNow(in, count, out, capacity);
	}

	/// **THE NOTE LANE'S CLOCK CALL** — what self-clocked devices (an arp) emit at this sample, through the devices
	/// after them; `RackEngine::emitNotesNow`. Nothing, and no limit, when no device emits.
	size_t emitNotes(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out, size_t capacity,
	                 uint32_t* samplesUntilNext) {
		return engine_.emitNotesNow(clock, sampleNow, out, capacity, samplesUntilNext);
	}

	/// **A MODULATOR'S OUTPUT, UNGATED; ONE VOICE'S VALUE OF A PER-VOICE OUTPUT; WHERE IT STANDS — ALL BY
	/// THE DEVICE'S ID** (kill-lanes step 3a). They took the engine's slot, which is a `size_t` a record
	/// lane also fits: a site that missed the conversion would read another device and say nothing (the DG
	/// shape — OWLTT's bars frozen after a drag). An id cannot be mistaken for a lane. Zero, or false, for a
	/// device this engine is not running.
	[[nodiscard]] float outputOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.outputAt(engine_.slotOf(device), output);
	}
	/// **WHAT A ROUTE FROM THIS DEVICE RESOLVES AGAINST** — the node's published cell, the disable rule applied
	/// (kill-lanes §2.4b). `outputOf` above is the device's own ungated reading.
	[[nodiscard]] float publishedOutputOf(DeviceInstanceId device, size_t output) const noexcept {
		return engine_.publishedOutputAt(engine_.slotOf(device), output);
	}
	[[nodiscard]] size_t voices() const noexcept { return engine_.voiceCount(); }
	[[nodiscard]] float outputForNoteOf(DeviceInstanceId device, size_t output, int32_t noteId, int16_t channel,
	                                    int16_t key) const noexcept {
		return engine_.outputForNoteAt(engine_.slotOf(device), output, noteId, channel, key);
	}
	/// FALSE IS "CANNOT SAY" AND LEAVES THE OUTPUTS UNTOUCHED — registry.h's rule for playheads.
	bool playheadOf(DeviceInstanceId device, uint32_t* ordinal, float* phase) const;
	/// The level beside it (`ModOps::playheadLevel`); false, untouched, when the device publishes none.
	bool playheadLevelOf(DeviceInstanceId device, float* level) const;
	/// The Trig's fire count (backlog MV) — `RackEngine::trigFiresAt`.
	bool trigFiresOf(DeviceInstanceId device, uint32_t* fires) const;
	/// The newest events, newest first — `RackEngine::recentEventsAt`.
	size_t recentEventsOf(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const;
	/// The extra state the device changed itself (backlog MX) — `RackEngine::extraStateAt`.
	uint64_t extraStateOf(DeviceInstanceId device, float* extra, size_t capacity, size_t* count) const;

	[[nodiscard]] bool anyPerVoice() const noexcept { return engine_.anyPerVoice(); }
	/// A per-voice device with a stereo route this block (backlog KU) — see `RackEngine::anyPerVoiceStereo`.
	[[nodiscard]] bool anyPerVoiceStereo() const noexcept { return engine_.anyPerVoiceStereo(); }
	void processVoice(size_t voice, float* left, float* right, int frames,
	                  const device::NoteContext& note) {
		engine_.processVoice(voice, left, right, frames, note);
	}
	/// The sidechain for the next `processBus` — `RackEngine::setSidechain`.
	void setSidechain(const float* left, const float* right, bool sounding) noexcept { engine_.setSidechain(left, right, sounding); }
	void processBus(float* left, float* right, int frames) { engine_.processBus(left, right, frames); }
	void setTickPhase(int phase) noexcept { engine_.setTickPhase(phase); }
	/// The song's clock at the next chunk's first sample — see `RackEngine::setClock`.
	void setClock(const device::TransportClock& clock) noexcept { engine_.setClock(clock); }
	void resetVoice(size_t voice) noexcept { engine_.resetVoice(voice); }
	void reset() noexcept { engine_.reset(); }

	/// THE DRIVE METER OF ONE DEVICE — `RackEngine::drivePeakAt`'s contract, by id.
	bool drivePeakOf(DeviceInstanceId device, float& peak) const {
		return engine_.drivePeakAt(engine_.slotOf(device), peak);
	}
	/// THE COURTESY OUTPUT METER OF ONE DEVICE (backlog MK) — `RackEngine::outputPeakAt`'s contract, by id.
	bool outputPeakOf(DeviceInstanceId device, float& peak) const {
		return engine_.outputPeakAt(engine_.slotOf(device), peak);
	}

	//--------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                   //
	//--------------------------------------------------------------------------------------------//

	/// THE BUS INSTANCE OF THE FIRST DEVICE IN CHAIN ORDER WITH `tag`. With `running`, only one the bus
	/// actually RAN this segment — null for a device in its per-voice arrangement, which is the
	/// complementary-halves claim `FxRunner`'s seam made.
	[[nodiscard]] const void* busStorageForTag(const char* tag, bool running) const;
	/// THE INSTANCE FOR `voice` OF THE FIRST DEVICE IN CHAIN ORDER WITH `tag` THAT RAN PER VOICE.
	[[nodiscard]] const void* voiceStorageForTag(size_t voice, const char* tag) const;
	/// HOW LATE THE LANE'S OUTPUT IS AT `sampleRate` — `RackEngine::latencySamples` (backlog JM).
	[[nodiscard]] uint32_t latencySamples(double sampleRate) const noexcept { return engine_.latencySamples(sampleRate); }
	/// HOW LATE THE PER-VOICE ROAD IS THIS BLOCK — `RackEngine::perVoiceLatencySamples`. The Synth delays each voice's gain by it.
	[[nodiscard]] uint32_t perVoiceLatencySamples() const noexcept { return engine_.perVoiceLatencySamples(); }
	/// How much per-voice latency the Synth compensates, in all — `RackEngine::setLatentPerVoiceCap`. MESSAGE THREAD.
	void setLatentPerVoiceCap(uint32_t samples) noexcept { engine_.setLatentPerVoiceCap(samples); }
	/// WHETHER A DEVICE ON THE LANE STILL HOLDS SOUND TO COME — `RackEngine::holdsTail` (backlog MC).
	[[nodiscard]] bool holdsTail() const noexcept { return engine_.holdsTail(); }
	/// THE CLOCK THE LANE WAS LAST HANDED.
	[[nodiscard]] const device::TransportClock& clockForTest() const noexcept { return clock_; }
	[[nodiscard]] const rack::RackEngine& engineForTest() const noexcept { return engine_; }
	/// THE ROAD BEFORE BACKLOG NS — every per-voice device built with its voices — for a case to compare against.
	void setEagerVoicesForTest(bool eager) noexcept { converger_.setEagerVoicesForTest(eager); }
	/// **WHAT THE ENGINE RUNS FOR ONE DEVICE, AND ITS BUS INSTANCE — BY ID** (kill-lanes step 3a): null for
	/// a device this engine is not running. By id for the reason the output roads above are: a slot and a
	/// record lane are both `size_t`, and a site that confused them would read another device in silence.
	[[nodiscard]] const device::Descriptor* descriptorOfForTest(DeviceInstanceId device) const noexcept {
		const size_t slot = engine_.slotOf(device);
		return slot < engine_.slotCount() ? engine_.slotAtLane(slot).live_desc() : nullptr;
	}
	[[nodiscard]] const void* storageOfForTest(DeviceInstanceId device) const noexcept {
		const size_t slot = engine_.slotOf(device);
		return descriptorOfForTest(device) != nullptr ? engine_.slotAtLane(slot).storage.get() : nullptr;
	}
	/// AND THE DESCRIPTOR IN ONE OF THE ENGINE'S OWN SLOTS — for the plan's coordinate space (the placement
	/// `ParamSet` compiles through), which is the one reader that legitimately holds a slot.
	[[nodiscard]] const device::Descriptor* descriptorAtSlot(size_t slot) const noexcept {
		return slot < engine_.slotCount() ? engine_.slotAtLane(slot).live_desc() : nullptr;
	}
	/// HOW MANY PROVISIONS ARE WAITING IN THE MAILBOXES.
	[[nodiscard]] size_t pendingCountForTest() const noexcept { return converger_.pendingCountForTest(); }

	/// FEATHERS' POLICY — every kind in the fx lane (`maySitInLane`), modulators and note devices in the strip.
	static bool fits(const device::Descriptor& desc, size_t chain);

private:

	rack::RackEngine engine_;
	/// THE ROAD FROM THE STATE TO THE ENGINE — declared after the engine it holds a reference to.
	rack::Converger converger_;
	/// THE STATE EACH HALF LAST BUILT — one per thread, so neither side ever reads the other's.
	rack::ChainState provisionState_ { kMaxDeviceSlots, 2 };
	rack::ChainState syncState_ { kMaxDeviceSlots, 2 };
	/// THE STATE THE LAST `converge` TOOK — not owned: it is the caller's (the synth's snapshot) or this
	/// object's own `syncState_`, and both outlive the block. Read by `slotOfStatePosition`.
	const rack::ChainState* converged_ = nullptr;
	std::function<void(const char*, size_t, void*)> connector_;
	device::TransportClock clock_ {};
	/// WHETHER A BLOCK IS OPEN — between a `sync` and the next `sync` or `finish`.
	bool open_ = false;
};

} // namespace feathers
