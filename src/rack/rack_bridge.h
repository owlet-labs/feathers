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
 */

#pragma once

#include "backplane/device_host.h"
#include "backplane/inner_routes.h"
#include "backplane/route_list.h"
#include "backplane/container_bodies.h"
#include "composite/composite_types.h"
#include "device/extra_state.h"
#include "device/subscribers.h"
#include "rack/chain_record.h"
#include "rack/converger.h"
#include "rack/fragment.h"
#include "rack/held_falls.h"
#include "rack/published_chain.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack/nested_slots.h"
#include "rack/slot_owner.h"
#include "hostslots/slot_bus.h"

#include "mod/modulator.h"

#include <atomic>
#include <bit>
#include <memory>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <span>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

//--------------------------------------------------------------------------------------------------//
// THE BRIDGE CONTRACT — the standard's, not this file's                                              //
//--------------------------------------------------------------------------------------------------//
//
// **THE BRIDGE IS THE PROTOCOL** (vision §7.5). A device standard whose two hosts talk
// to their editors in two different ways is two standards. `ParamBridge` and `RackBridge` are two
// IMPLEMENTORS of it rather than two shapes that happen to rhyme — same protocol, NOT the same class
// (decided 2026-09-03): `ParamBridge` is welded to `clap_id` and the parameter queue, and a rack with no
// parameters must not inherit CLAP's parameter model to get a handoff.
//
// **AND THE PROTOCOL IS DESIRED STATE**. THE CONTRACT:
//
//   THE RECORD — the editor edits a `ChainRecord` on the message thread: which devices, in which order,
//   with which rows, and every route between them. An edit is a change to the record, visible to the
//   editor the instant the hand makes it, because the editor reads the record.
//
//   THE PUBLISH — after every change the record's state (`PublishedChainState`) and its routes
//   (`RouteList`) are published, and whatever the new state needs built is provisioned there, on the
//   message thread (`Converger::provision`).
//
//   THE DRAIN — at the two sites no block can be running (`process`, `paramsFlush`), the engine takes
//   each publish WHOLE (a snapshot that saw a publish land is refused and taken next time) and becomes
//   it (`Converger::converge`). The engine never hears a verb, so no verb can be refused out of the
//   editor's sight: a lane the engine could not take is rebuilt at the next service and converged to,
//   and the record — what the hand made — stands throughout.
//
//   THE READ HALF — what only the engine can say (a modulated control, a meter, a playhead, a macro's
//   value) is MIRRORED from it once a block, per-cell atomics, as before.
//
// **WHAT THIS REPLACED, AND WHY THE REPLACEMENT IS SMALLER.** The forward channel was VERBS — a ring
// of add/remove/move/field edits, applied at the drain against whatever the engine then was — and
// verbs in flight needed an OVERLAY (the hand's pending edits composed over a mirror of the engine's
// order), a seqlock'd order frame the overlay could read, a torn-frame counter for when it could not, a
// patch channel for loads too big for the ring, a bounded wait on it, refusals that arrived after the
// call that caused them, and a refused-edit counter so a card that "vanished at the next publish" could
// at least be counted. Every one of those existed because the editor drew one thing and the engine
// held another. Now the editor draws the record, and the engine converges to it.
//
// WHAT IS STILL AN EVENT, ON A SMALL QUEUE: a macro written by the hand (the macros are the rack's CLAP
// parameters, the engine's to hold), and the note gate and retrigger — events, not state.

namespace feathers::rack {

class ExportTable;

/// **ONE EVENT ON ITS WAY TO THE ENGINE** — a macro, the note gate, a retrigger. Not chain state: that
/// is the record's (see the contract above).
struct RackEdit {
	enum class Kind : uint8_t { Macro, Gate, Retrigger, MacroPolarity };
	Kind kind = Kind::Macro;
	size_t a = 0; ///< macro index
	float value = 0.0f;
};

/// **THE EVENT CHANNEL — one producer, one consumer (the drain).** A full queue refuses and counts; it
/// does not block and it does not allocate. It cannot fill at UI rates: a hand emits ~60 values a
/// second and the drain runs ~200 times a second, against a ring of 1024.
class RackEditQueue {
public:
	static constexpr size_t kCapacity = 1024;

	bool push(const RackEdit& e) noexcept {
		const size_t w = write_.load(std::memory_order_relaxed);
		const size_t next = (w + 1) % kCapacity;
		if (next == read_.load(std::memory_order_acquire)) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		ring_[w] = e;
		write_.store(next, std::memory_order_release);
		return true;
	}

	bool pop(RackEdit& out) noexcept {
		const size_t r = read_.load(std::memory_order_relaxed);
		if (r == write_.load(std::memory_order_acquire)) {
			return false;
		}
		out = ring_[r];
		read_.store((r + 1) % kCapacity, std::memory_order_release);
		return true;
	}

	uint32_t droppedCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
	std::array<RackEdit, kCapacity> ring_ {};
	std::atomic<size_t> write_ { 0 };
	std::atomic<size_t> read_ { 0 };
	std::atomic<uint32_t> dropped_ { 0 };
};

/// **THE RACK'S EDITOR-FACING HOST.** The cards and the editor address this; nothing in the rack's UI
/// addresses `RackEngine`.
class RackBridge final : public backplane::DeviceHost, public FragmentSite {
public:
	/// ALLOCATES — the record, the publishes, the mirrors — once, on the message thread, before any block
	/// runs. **THE ENGINE'S CURRENT CONTENTS BECOME THE RECORD**: a bridge built over an engine that
	/// already holds devices and routes takes them as what the rack IS, and the engine keeps running
	/// them (`Converger::adoptRunning`) rather than rebuilding them at the first drain.
	///
	/// **`shared` IS THE COUNTER THIS BRIDGE ISSUES INSTANCE IDS FROM, OR NULL TO KEEP ITS OWN** — gap
	/// G3. A bridge that owns its counter starts at `kFirstInstance`, so a holder with TWO of them
	/// hands out the same ids twice; `ContainerOps`' four route verbs take a
	/// `device::Destination{deviceId, index}` with no lane argument, so the holder must dispatch on the
	/// id alone and both lanes answer for it. MEASURED before the fix
	/// (`tests/test_inner_chain.cpp`): both chains issued id 1, and `descriptorOf(1)` was non-null in
	/// each. A holder that passes ONE counter to both makes the id unambiguous by construction.
	///
	/// **IT MUST OUTLIVE THE BRIDGE**, and a holder that can be MOVED must not keep it as a plain
	/// member — see `Sklitter`, whose counter is heap-held for exactly this reason.
	///
	/// **`slots` SAYS WHETHER THIS BRIDGE OWNS A HOST SLOT POOL** (backlog JU, T10; decided 2026-09-25: the
	/// standalone rack owns one, eight macros then 64 slots). `OwnPool` only for the OUTERMOST rack
	/// — the standalone plugin's — because one pool is owned by the outermost plugin: a rack
	/// inside Feathers or inside another rack has no host of its own to declare parameters to, never writes
	/// `hostSlots` and ignores it on load. Its controls get slots from the outermost pool through the link its
	/// host hands it (T11, `setHostSlots`, `NestedSlots`); with no link, none.
	enum class HostSlots : uint8_t { None, OwnPool };
	explicit RackBridge(RackEngine& engine, backplane::InstanceCounter* shared = nullptr,
	                    HostSlots slots = HostSlots::None);
	~RackBridge() override;
	RackBridge(const RackBridge&) = delete;
	RackBridge& operator=(const RackBridge&) = delete;

	/// "NO SLOT", RE-EXPORTED SO THE UI NEED NOT NAME THE ENGINE FOR A CONSTANT.
	static constexpr size_t kNoSlot = RackEngine::kNoSlot;

	//---------------------------------------------------------------------------------------------//
	// THE HOST INTERFACE                                                                          //
	//---------------------------------------------------------------------------------------------//

	/// THE BASE'S DELETED `size_t` OVERLOADS, VISIBLE HERE — an override hides them otherwise, and a caller
	/// holding a `RackBridge&` would pass a lane as an id and compile (`device_host.h`).
	using backplane::DeviceHost::fieldAt;
	using backplane::DeviceHost::setField;
	using backplane::DeviceHost::fieldsPresentAt;
	using backplane::DeviceHost::modulatedFieldAt;
	using backplane::DeviceHost::readingCountAt;
	using backplane::DeviceHost::readingAt;
	using backplane::DeviceHost::drivePeakAt;
	using backplane::DeviceHost::outputPeakAt;
	using backplane::DeviceHost::playheadAt;

	const backplane::Axes& axes() const noexcept override { return engine_.axes(); }
	/// THE RECORD'S STRUCTURE, BY ID — which is what the routing rule is about: a gesture's glow, a
	/// loader's gate and the record's own `setRoute` answer from one view.
	/// THE RECORD'S VIEW — which carries the host outside when this bridge is a chain's (`setOuterHost`).
	backplane::DeviceView devices() const noexcept override { return record_.devices(); }

	/// **THE HOST THIS CHAIN SITS IN, FOR THE GATE** — rack roadmap §7, the message-thread half of the
	/// reader. The engine is handed its half (the cells) at seating on the audio side; this bridge is
	/// handed the OUTER `DeviceHost` and this chain's own id by the chain's editor when it follows the
	/// device, so that `mayRoute` here — asked by every inner control that volunteers to read an
	/// outer source, and by the record before it writes a route — can ask the outer host's record
	/// whether that source is upstream. Nothing here reads the outer ENGINE's graph, which is the
	/// audio thread's. Kept on the record, because the record's own gate is the one `setRoute` asks.
	void setOuterGate(const device::OuterGate& gate) noexcept {
		record_.setOuterGate(gate);
		outerDirtyHost_ = gate.dirtyHost;
		outerMarkDirty_ = gate.markDirty;
		perVoiceUnavailable_ = gate.perVoiceUnavailable;
	}
	/// **THIS RACK'S SERVICE AS A CHAIN, ON ITS HOST'S BEAT** (`ContainerOps::service`): the message-thread
	/// service every rack has (`reapRetired`), then the hygiene across the boundary — routes whose outer
	/// source is gone or downstream are dropped, committed, and returned to be said.
	/// EVERY ROUTE THE RECORD HOLDS, by identity — what a host outside walks to show this chain's inner
	/// routes from one of its own sources (`ContainerOps::forEachRouteFrom`).
	template <typename F>
	void forEachRoute(F&& f) const {
		record_.graph().forEachRoute(std::forward<F>(f));
	}

	device::RouteDrop service() {
		reapRetired();
		// THE CONTAINERS' OWN CONTROLS (backlog NO): released ones pruned before anything may take them again, a mirror a load owes written.
		(void)serviceOwnControls(record_, bodies_, [this](backplane::Source s, backplane::Destination d) {
			setRouteAmount(s, d, 0.0f);
		});
		const device::RouteDrop dropped = record_.pruneOuterRoutes();
		if (dropped.count > 0) {
			commit();
			noteRefusedRoutes(RouteRefusals { dropped.count, dropped.firstSource, dropped.firstDestination });
		}
		// AND THE HOST SLOTS' SHARE, ON THE SAME BEAT (T11): report, pull, mirror, adopt — `NestedSlots::service`.
		if (nested_ != nullptr) {
			nested_->service([this] { commit(); });
		}
		return dropped;
	}

	/// **THE POOL OUTSIDE, FOR THIS RACK AS A CHAIN IN ANOTHER** (host slots T11, `ContainerOps::setHostSlots`).
	/// A bridge that owns a pool ignores it. Otherwise the link is kept; the engine is handed the link's bus
	/// the first time (before any block runs on it — this is a provision); and every container this rack
	/// holds is re-handed its own link, so a rack whose link arrived after its body loaded links its children.
	void setHostSlots(const device::HostSlotLink& link);
	/// THE LINK THIS RACK HANDS A CONTAINER IT HOLDS AT `self` (its converger's maker): its own pool's, when it
	/// owns one; its link with `self` appended, when it is nested; none when it has neither.
	[[nodiscard]] device::HostSlotLink slotLinkFor(DeviceInstanceId self) noexcept;
	/// The nested half, or null for the outermost rack. TEST SEAM and the macro road's.
	[[nodiscard]] NestedSlots* nestedSlots() noexcept { return nested_.get(); }
	/// **THIS RACK'S MACROS ARE ITS DEVICE'S CONTROLS ONE LEVEL UP** (T11: a nested rack's macros are
	/// exposable, labelled "Rack 1 · Wobble" once renamed). Said by the rack DEVICE (`rack_device.cpp`) and by
	/// nothing else — a SKLITTER lane's macros are not its holder's controls — so a rename here relabels the
	/// slot on the device's Macro control (`NestedSlots::setOwnCaptions`).
	void setMacrosAreDeviceControls(bool yes);
	/// **WHAT THIS RACK REFUSED OR DROPPED AND HAS NOT YET SAID** — a load's refusals (`loadPatch`) and the
	/// service's drops, kept until a surface takes them: the standalone rack's GUI on its tick, a chain's
	/// editor when it follows. The same road Feathers' bridge has, so both hosts say the same things.
	///
	/// **A LOAD REPORT SINCE BACKLOG JF** (`LoadReport`): a load's whole account, of which the refused routes
	/// are one kind. A service's drops ADD to what is held; a load's report is ADDED too, so a chain loaded
	/// twice before anybody looked says both.
	void noteRefusedRoutes(const RouteRefusals& refused) { loadReport_.mergeRoutes(refused); }
	void noteLoadReport(const LoadReport& report) { loadReport_.merge(report); }
	[[nodiscard]] LoadReport takeLoadReport() {
		LoadReport out = std::move(loadReport_);
		loadReport_ = LoadReport {};
		return out;
	}
	[[nodiscard]] bool hasOuterGate() const noexcept { return record_.hasOuterGate(); }
	/// THE GATE THIS HOST HANDS A CHAIN IT PROVISIONS (`Converger`): this record's answer, for that chain.
	[[nodiscard]] device::OuterGate outerGateFor(DeviceInstanceId self) const noexcept {
		device::OuterGate gate;
		gate.host = &record_;
		gate.self = self;
		gate.upstream = [](const void* host, DeviceInstanceId self, backplane::Source s) {
			return static_cast<const ChainRecord*>(host)->sourceUpstreamOf(s, self);
		};
		// AN EDIT IN THAT CHAIN MARKS THIS RACK'S PATCH — the chain's body is saved in it (backlog JV).
		gate.dirtyHost = const_cast<RackBridge*>(this); // a bridge is never const-defined
		gate.markDirty = [](void* host) { static_cast<RackBridge*>(host)->markStateDirty(); };
		// AND ITS OWN CONTROLS' MIRROR (backlog NO: a Rack's chain level, mute and solo), written as any field is.
		gate.setOwnField = [](void* host, DeviceInstanceId self, size_t control, float value) {
			auto* bridge = static_cast<RackBridge*>(host);
			if (!bridge->record_.storesField(self, control, value)) { // the value already there is no edit
				bridge->setField(self, control, value);
			}
		};
		// AND WHAT THIS CHAIN WAS TOLD ABOUT VOICES, handed on: a Chain inside a Rack has none either (backlog NO).
		gate.perVoiceUnavailable = perVoiceUnavailable_;
		return gate;
	}
	[[nodiscard]] const RackEngine& engineForTest() const noexcept { return engine_; }

	/// THE RECORD'S ROW. Absent is the declared default, never zero (`device_row.h`'s rule).
	float fieldAt(DeviceInstanceId device, size_t control) const override;
	/// INTO THE RECORD, at once; the engine has it after the next drain.
	void setField(DeviceInstanceId device, size_t control, float value) override;
	/// A MOMENTARY'S RELEASE: held until a drain has the rise (`HeldFalls`).
	void releaseField(DeviceInstanceId device, size_t control, float value) override;
	std::vector<backplane::Route> takePrunedRoutes() override { return record_.takePrunedRoutes(); }
	size_t fieldsPresentAt(DeviceInstanceId device) const override {
		// A DEVICE THIS RECORD DOES NOT HOLD HAS A DEAD ROW, whose width is zero — the one answer that gates
		// every reader below it.
		return record_.rowOf(device).fieldsPresent;
	}

	/// A MEMBER CHAIN'S BODY, from this rack's carrier (`bodies_`) — a rack in a rack draws its head from it.
	std::span<const uint8_t> containerBodyOf(DeviceInstanceId device) const override { return bodies_.bodyFor(device); }
	uint64_t containerBodyGenerationOf(DeviceInstanceId device) const override { return bodies_.generationOf(device); }
	void* containerSurfaceOf(DeviceInstanceId device) const override { return bodies_.surfaceOf(device); }
	/// A HAND'S BLOCKS (`DeviceHost::setBlock`): kept in this carrier, and an edit of one is an edit of the patch.
	::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) override { return &bodies_; }
	const ::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) const override { return &bodies_; }
	void onBlockEdited(DeviceInstanceId) override { markStateDirty(); }
	/// A CHAIN ACCESS SITE (backlog NR): filed empty under `siteKeyOf(device)` and committed, so the converger builds it.
	bool openSite(DeviceInstanceId device) override;
	bool hasSite(DeviceInstanceId device) const override { return bodies_.has(siteKeyOf(device)); }
	bool loadSite(DeviceInstanceId device, std::span<const uint8_t> body) override;
	/// HOW MANY CHAINS DOWN THIS BRIDGE'S CHAIN STANDS — its record's catalogue's (`Catalogue::depth`), known from the
	/// moment the chain was built, headless or not.
	size_t nestingDepth() const noexcept override { return record_.depth(); }
	bool holdsBody(std::span<const uint8_t> body) const override;
	[[nodiscard]] const device::Catalogue& catalogue() const noexcept { return record_.catalogue(); }
	backplane::Source siteExport(DeviceInstanceId owner, backplane::Source inner) override;
	/// A source inside the Chain or Rack `container`, as this host names it (backlog NZ (b); `exportFromContainer`).
	backplane::Source containerExport(DeviceInstanceId container, backplane::Source inner) override;
	/// A rack runs per voice only with voices of its own (Feathers' lane does; the standalone rack has none).
	bool hostsVoices() const override { return engine_.voiceCount() > 0; }
	/// Held against this rack's reap (backlog JX) — the carrier its converger reads pins from.
	void pinContainerSurface(const void* surface) override { bodies_.pin(surface); }
	void unpinContainerSurface(const void* surface) override { bodies_.unpin(surface); }
	/// WHERE MODULATION LEFT A CONTROL — the engine's, while the engine runs what the record has for this
	/// device; the base until it does, rather than another device's reading.
	float modulatedFieldAt(DeviceInstanceId device, size_t control) const override;
	/// AND THE RIGHT EAR (backlog KU) — true only while the engine reported a stereo route reaching the control.
	bool modulatedFieldRAt(DeviceInstanceId device, size_t control, float& right) const override;

	double sampleRate() const noexcept override { return relaxedLoad(m_.sampleRate); }

	size_t readingCountAt(DeviceInstanceId device) const noexcept override {
		const size_t slot = mirrorSlotOf(device);
		return slot != kNoSlot ? loadSize(m_.readingCount[slot]) : 0;
	}
	float readingAt(DeviceInstanceId device, size_t index) const noexcept override {
		const size_t slot = mirrorSlotOf(device);
		if (slot == kNoSlot || index >= readingStride_) {
			return 0.0f;
		}
		return loadFloat(m_.readings[slot * readingStride_ + index]);
	}
	/// FORWARDED — `RackEngine` owns the device storage and is the only thing that can answer — and only
	/// while it runs what the record has for this device.
	bool drivePeakAt(DeviceInstanceId device, float& peak) const override {
		const size_t slot = mirrorSlotOf(device);
		return slot != kNoSlot && engine_.drivePeakAt(slot, peak);
	}
	/// THE COURTESY OUTPUT METER (backlog MK), forwarded the same way: the engine measures it and holds the atomic.
	bool outputPeakAt(DeviceInstanceId device, float& peak) const override {
		const size_t slot = mirrorSlotOf(device);
		return slot != kNoSlot && engine_.outputPeakAt(slot, peak);
	}
	bool playheadAt(DeviceInstanceId device, uint32_t& ordinal, float& phase) const override;
	/// The level beside it (`mod::Playhead::level`), mirrored with it.
	bool playheadLevelAt(DeviceInstanceId device, float& level) const override;
	/// **THE TEMPO THE ENGINE LAST RAN AGAINST, as Feathers' bridge answers it** (`ParamBridge::rackSecondsPerBeat`) — the
	/// LFO card draws its smoothing against the period the ENGINE uses (`lfo::rateHz`). This answered the default
	/// zero in the rack for every host (backlog NJ (a)), so a synced card here drew its curve at the no-tempo rate.
	/// Zero while the host has not said. Mirrored by `publish`.
	double secondsPerBeat() const noexcept override { return secondsPerBeat_.load(std::memory_order_relaxed); }
	bool trigFiresAt(DeviceInstanceId device, uint32_t& fires) const override;
	size_t recentEventsAt(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const override;

	/// THE RECORD'S — one read of the graph, no table copied.
	float routeAmount(backplane::Source source, backplane::Destination destination) const override {
		return record_.routeAmount(source, destination);
	}
	/// INTO THE RECORD, through its gate — a pair the routing rule refuses is not written.
	void setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) override;
	/// THE AS-MODULATED READOUT'S ANSWER (backlog PU) — this engine's, through `publish`.
	bool probeRoute(backplane::Source source, backplane::Destination destination, backplane::RouteProbe& out) override {
		routeProbe_.watch(source, destination);
		return routeProbe_.read(source, destination, out);
	}
	void unwatchRoute() override { routeProbe_.unwatch(); }
	/// THE ROUTE'S CURVE, the record's.
	bool supportsRouteTransfers() const override { return true; }
	backplane::Transfer routeTransfer(backplane::Source source, backplane::Destination destination) const override {
		return record_.routeTransfer(source, destination);
	}
	bool setRouteTransfer(backplane::Source source, backplane::Destination destination, backplane::Transfer t) override;
	bool routedElsewhere(backplane::Destination destination, backplane::Source except) const override {
		return record_.graph().routedElsewhere(destination, except);
	}
	void beginRouteGesture() override {}
	void endRouteGesture() override {}
	/// **A HAND ON A CARD'S KNOB, BRACKETED FOR THE DAW** (host slots, T10): with a pool, a control that holds
	/// a slot sends the host Begin / End on it (`SlotOwner::beginGesture`), so a drag records into its lane
	/// as one gesture. Without one there is no host parameter to tell.
	/// A NESTED RACK (T11) sends its gestures UP, by path, to the pool that owns the slot (`NestedSlots`).
	/// A FIRST GRAB IS SERVICED HERE, for `ParamBridge::beginControlGesture`'s reason (`setFirstGrabService`).
	void beginControlGesture(DeviceInstanceId device, size_t control) override {
		if (slots_ != nullptr) {
			slots_->beginGesture(device, control);
			if (std::exchange(firstGrab_, false) && firstGrabService_) {
				firstGrabService_();
			}
		}
		else if (nested_ != nullptr) {
			nested_->beginGesture(device, control);
		}
	}
	void endControlGesture(DeviceInstanceId device, size_t control) override {
		if (slots_ != nullptr) {
			slots_->endGesture(device, control);
		}
		else if (nested_ != nullptr) {
			nested_->endGesture(device, control);
		}
	}
	/// **A HAND TOOK A CONTROL: THE RECENT RING** (host slots, T9) — `SlotOwner::touch`, marking the host dirty
	/// when a new mapping changed the saved pool. A NESTED rack (T11) sends the touch up its link by path, to the
	/// pool that owns its slots, which marks THAT plugin's patch; with no link (a third party's host) nothing.
	void touchControl(DeviceInstanceId device, size_t control) override {
		if (slots_ == nullptr) {
			if (nested_ != nullptr) {
				nested_->touch(device, control);
			}
			return;
		}
		firstGrab_ = markedIf(slots_->touch(device, control));
	}
	/// What `beginControlGesture` runs after a first grab: the owning plugin's main-thread visit. Main thread.
	void setFirstGrabService(std::function<void()> service) { firstGrabService_ = std::move(service); }
	/// `SlotOwner::takeNotice` — what the tray says about the pool (T9); empty with no pool. The editor's tick asks.
	[[nodiscard]] std::string takeSlotNotice() { return slots_ != nullptr ? slots_->takeNotice() : std::string {}; }
	/// **THE RIGHT-CLICK'S HOST ROWS** (T7, on the rack at the integration): `SlotOwner`'s status and verbs,
	/// so the standalone rack's knobs offer "Expose to host" as Feathers' do. A bridge with no pool (a nested
	/// rack) keeps `DeviceHost`'s answers: `Unavailable`, and every verb refuses. EACH VERB THAT CHANGED THE
	/// POOL MARKS THE HOST DIRTY (`markStateDirty`, mainline's JV road): the pool is saved with the patch, and
	/// a slot's state is not a parameter, so nothing else tells the host its project changed.
	///
	/// **A NESTED RACK (T11) ASKS THE POOL THAT OWNS ITS SLOTS** — the outermost plugin's, up its link, by path
	/// (`NestedSlots`). Its verbs mark THAT plugin's patch, where the pool is saved, and not this rack's body,
	/// which carries no slots. A nested rack with no link (a third party hosting it) answers `Unavailable`.
	device::HostSlotStatus hostSlotOf(DeviceInstanceId device, size_t control) const override {
		if (slots_ != nullptr) {
			return slots_->statusOf(device, control);
		}
		return nested_ != nullptr ? nested_->statusOf(device, control) : device::HostSlotStatus {};
	}
	bool exposeToHost(DeviceInstanceId device, size_t control, std::string* displaced) override {
		if (displaced != nullptr) {
			displaced->clear();
		}
		if (slots_ == nullptr) {
			return nested_ != nullptr && nested_->expose(device, control, displaced);
		}
		return markedIf(slots_->expose(device, control, displaced));
	}
	bool unpinFromHost(DeviceInstanceId device, size_t control) override {
		if (slots_ == nullptr) {
			return nested_ != nullptr && nested_->unpin(device, control);
		}
		return markedIf(slots_->unpin(device, control));
	}
	/// A touch that took a NEW slot, owed a service at the Begin that follows it (`beginControlGesture`).
	bool firstGrab_ = false;
	std::function<void()> firstGrabService_;
	/// `changed`, with the host told its patch is dirty when it is.
	bool markedIf(bool changed) noexcept {
		if (changed) {
			markStateDirty();
		}
		return changed;
	}
	uint64_t hostSlotGeneration() const noexcept override {
		if (slots_ != nullptr) {
			return slots_->pool().generation();
		}
		return nested_ != nullptr ? nested_->generation() : 0;
	}

	/// THE ARMED SOURCE'S ROUTE LIST (rack roadmap §7f): the record's own routes, named by descriptor and
	/// position, and the routes inside the chains this rack holds.
	void forEachRouteFrom(backplane::Source source, void* ctx,
	                      void (*f)(void*, backplane::Destination)) const override {
		record_.graph().forEachRoute([&](const backplane::Route& r) {
			if (r.source == source) {
				f(ctx, r.destination);
			}
		});
	}
	void forEachRouteInto(backplane::Destination destination, void* ctx,
	                      void (*f)(void*, backplane::Source)) const override {
		record_.graph().forEachRoute([&](const backplane::Route& r) {
			if (r.destination == destination) {
				f(ctx, r.source);
			}
		});
	}
	std::string destinationName(backplane::Destination destination) const override;
	std::vector<InnerRoute> innerRoutesFrom(backplane::Source source) const override;
	float innerRouteAmount(backplane::Source source, DeviceInstanceId chain,
	                       backplane::Destination inner) const override;
	bool setInnerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                         float amount) override;
	backplane::Transfer innerRouteTransfer(backplane::Source source, DeviceInstanceId chain,
	                                       backplane::Destination inner) const override;
	bool setInnerRouteTransfer(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                           backplane::Transfer t) override;
	std::string innerDestinationName(DeviceInstanceId chain, backplane::Destination inner) const override;

	bool hasExpandedViewFor(const device::Descriptor& desc) const override {
		return engine_.hasExpandedViewFor(desc);
	}
	backplane::CorePolicy corePolicy() const noexcept override { return engine_.corePolicy(); }

	//---------------------------------------------------------------------------------------------//
	// THE EDITOR'S CHAIN VERBS AND READS — all of them the record's, every device named by its id  //
	//---------------------------------------------------------------------------------------------//

	/// A DEVICE AT THE END. Returns its id — the record issued it, so the answer is true at once — or
	/// `kNoInstance` for a tag this build cannot run or a full rack.
	DeviceInstanceId addDevice(const char* tag);
	/// A DEVICE IMMEDIATELY BEFORE `anchor` (backlog CR). `kNoInstance` when the anchor is not in the
	/// chain — inserting before nothing is not appending.
	DeviceInstanceId addDeviceBefore(DeviceInstanceId anchor, const char* tag);
	void removeDevice(DeviceInstanceId device);
	/// A DROP, BY POSITION — a rotation, every device between shifting by one.
	bool moveDevice(size_t from, size_t to);
	/// The same drop, named by the device.
	bool moveDeviceTo(DeviceInstanceId device, size_t to);

	//--------------------------------------------------------------------------------------------//
	// SOME DEVICES AT ONCE — backlog OA (`rack/fragment.h`): copy, paste, duplicate, remove, group.   //
	//--------------------------------------------------------------------------------------------//
	ChainRecord& fragmentRecord() override { return record_; }
	size_t fragmentChain() const override { return 0; }
	ContainerBodies& fragmentBodies() override { return bodies_; }
	backplane::InstanceCounter& fragmentIds() override { return ids_; }
	CoreCodec fragmentCodec() const override { return rackCoreCodec(engine_.axes()); }
	DeviceViews* fragmentViews() override { return &cardViews_; }
	DeviceLabels* fragmentLabels() override { return &outputLabels_; }
	void fragmentCatchUp() override { harvestExtraState(); }
	void fragmentFreeSlots(DeviceInstanceId device) override;
	void fragmentRestoreSlots(DeviceInstanceId device) override;
	bool fragmentRebaseSlots(DeviceInstanceId device, DeviceInstanceId container, bool into) override;
	void fragmentChanged(const DeviceLabels& labelsBefore) override;
	uint64_t fragmentOrigin() const override { return fragmentOrigin_; }
	std::weak_ptr<const void> fragmentLife() const override { return life_; }
	bool fragmentHostsVoices() const override { return engine_.voiceCount() > 0; }

	/// **THE MESSAGE THREAD'S SERVICE** — destroys what the drain parked, and builds again any device the
	/// engine could not take. Called by the editor's tick and after every edit. Never from the audio thread.
	void reapRetired();
	/// MESSAGE THREAD. Free the export rows of the containers here that no route reads, once the audio thread has drained
	/// the publish that dropped the last one (`ExportTable::sweep`). Returns how many it freed.
	size_t sweepExports();
	/// **THE TABLE OF THE CONTAINER THIS RACK IS THE INSIDE OF** (a Chain's, a Rack chain's, a site's PRE), or null at the
	/// top (backlog NZ, any depth). A row of it may name an export of a holder here — the re-export one hop out — and that
	/// row is a READER of the holder's row as surely as a route is, so the sweep keeps it. Set by the owner at adopt.
	void setOwnerExports(const ExportTable* table) noexcept { ownerExports_ = table; }
	/// **AN EXPORT A HOLDER HERE ACTUALLY HAS** — `source` names a container's or a site's row that is minted: the test an
	/// owner applies before re-exporting it (`ContainerOps::exportRow`). False for anything else.
	[[nodiscard]] bool namesLiveExport(backplane::Source source) const;
	/// **THE RECORD CATCHES UP WITH WHAT ITS DEVICES CHANGED THEMSELVES** (backlog MX, `rack/extra_state_harvest.h`) —
	/// every snapshot the block published, under the basis guard. The main thread; `reapRetired` and `patch` call it.
	void harvestExtraState();

	/// **THE TAG THE RECORD HOLDS FOR `device`**, all NUL for one it does not hold. With `descriptorOf`
	/// null, that is a device this build does not have — kept, and drawn as "device missing".
	device::Tag heldTagOf(DeviceInstanceId device) const { return record_.rowOf(device).tag; }
	/// **WHAT A TAG WOULD RUN AS HERE**, or null — the record's own catalogue, asked before a row is
	/// placed. It is the question an ADD MENU has to answer, and it cannot be answered from the build's
	/// device table: a chain is handed its catalogue at `adopt` and may hold fewer devices than the
	/// process knows about. A menu offering what this returns null for is the honesty rule's failure
	/// case — a row that does nothing when pressed.
	const device::Descriptor* descriptorFor(const char* tag) const { return record_.descriptorFor(tag); }
	/// HOW MANY DEVICES THE RACK'S CHAIN MAY HOLD — a count, never an address. The rack keeps one chain, so
	/// this is what the editor asks before offering another card (`rack_editor.cpp`).
	size_t capacity() const noexcept { return record_.capacity(0); }
	/// THE DEVICE AT `position` IN THE CHAIN, or `kNoInstance`.
	DeviceInstanceId deviceAtPosition(size_t position, size_t chain = 0) const noexcept {
		return record_.deviceAt(chain, position);
	}
	/// WHERE `device` STANDS IN ITS CHAIN, or `kNoSlot`.
	size_t positionOf(DeviceInstanceId device) const noexcept {
		const size_t at = record_.positionOf(device);
		return at == ChainRecord::kNoPosition ? kNoSlot : at;
	}
	size_t chainLength(size_t chain = 0) const noexcept { return record_.length(chain); }

	//---------------------------------------------------------------------------------------------//
	// EVENTS — macros, the gate, a retrigger                                                      //
	//---------------------------------------------------------------------------------------------//

	float macro(size_t index) const {
		return index < m_.macro.size() ? loadFloat(m_.macro[index]) : 0.0f;
	}
	void setMacro(size_t index, float value);
	/// **A HAND ON A MACRO'S KNOB, TOLD TO THE HOST** (backlog NG, 2026-10-17). The macros ARE the outermost rack's
	/// first eight host parameters, so the editor's drag goes out as Feathers' does for its own: Begin, every
	/// Value, End, on the macro's id. Without this the rack told the host nothing — Bitwig's recently-touched list
	/// never saw a macro move, its automation recorded none, and its copy of the value went stale. Only the bridge
	/// that owns the pool reports (a nested rack's macros are not host parameters, they are its chain device's
	/// controls); the plugin drains the outbox at both of its output sites (`RackPlugin::drainHostOutboxes`).
	/// `setMacroByHand` is `setMacro` plus the Value; the host's own writes stay on `setMacro`, which says nothing.
	using MacroOutbox = hostslots::SlotOutbox<composite::kMacroCount, 256>;
	void beginMacroGesture(size_t index) noexcept;
	void setMacroByHand(size_t index, float value);
	void endMacroGesture(size_t index) noexcept;
	/// The editor's macro moves bound for the host; null on a bridge that owns no pool. Main thread pushes, the
	/// audio thread (or `params.flush`) drains.
	[[nodiscard]] MacroOutbox* macroOutbox() noexcept { return macroOutbox_.get(); }
	/// **WHICH WAY MACRO `index` SWINGS (backlog EG)** — the hand's mode, held here (message thread) and
	/// queued to the engine like a macro value; saved with the patch beside the macros. An edit.
	[[nodiscard]] bool macroBipolar(size_t index) const noexcept {
		return index < macroBipolar_.size() && macroBipolar_[index] != 0;
	}
	void setMacroBipolar(size_t index, bool bipolar);
	/// **EACH MACRO'S TYPE AND NAME — ParamBridge's verbs, on its rules, for the rack's eight.** Held here
	/// (message thread) and saved with the patch beside the polarity, and NOT queued: no block reads either
	/// (a type is how the hand drives the value, and the value is all the engine sees), and a name is a
	/// `std::string`, which has no business on the lock-free road to the audio thread. So neither is a
	/// `RackEdit`. Out of range is ignored and an unchanged value moves nothing. An edit, like the polarity:
	/// each marks the patch dirty (`takeStateDirty`).
	[[nodiscard]] device::MacroType macroType(size_t index) const noexcept {
		return index < macroType_.size() ? macroType_[index] : device::MacroType::Knob;
	}
	/// A type past the last is refused, not clamped: the menu offers only real ones.
	void setMacroType(size_t index, device::MacroType type);
	/// **WHAT A MACRO IS CALLED IN PROSE** — the name given to it, or "Macro N" when there is none.
	[[nodiscard]] std::string macroName(size_t index) const;
	/// Whether the macro carries a name of its own rather than the default.
	[[nodiscard]] bool macroRenamed(size_t index) const noexcept {
		return index < macroName_.size() && !macroName_[index].empty();
	}
	/// THE RENAME. Clamped by `device::clampMacroName`; empty, or the default spelled out, clears the name
	/// (`device::normalisedMacroName`), so "renamed" always means a name that differs from the default.
	void setMacroName(size_t index, std::string_view name);
	/// **TOLD WHEN A MACRO'S NAME MOVES** — by setMacroName when it changes and by `loadPatch` for each
	/// macro whose effective name changed, on the message thread, after the bridge holds the new name.
	/// A LIST, as `ParamBridge::subscribeMacroRename` is and for its reason: the editor and the host (A9)
	/// both listen, and each holds the token that ends only its own.
	[[nodiscard]] device::Subscription subscribeMacroRename(std::function<void(size_t)> listener) {
		return macroRenameSubscribers_.subscribe(std::move(listener));
	}
	/// A MACRO'S POLARITY IS ITS MODE, so the ring on any knob a macro is armed at asks here rather
	/// than the record's device view, which knows nothing of core sources.
	bool sourceIsBipolarNow(backplane::Source source) const override {
		return source.isDevice() ? backplane::DeviceHost::sourceIsBipolarNow(source) : macroBipolar(source.index);
	}
	/// THE NOTE GATE, queued: a note-on and the note-off behind it keep their order, which is what the
	/// derived retrigger edge depends on.
	void setGate(bool gate) { (void)queue_.push(RackEdit { RackEdit::Kind::Gate, 0, gate ? 1.0f : 0.0f }); }
	/// A NOTE-ON, beside the gate: the gate is a summary of what is held, this is an event.
	void retrigger() { (void)queue_.push(RackEdit { RackEdit::Kind::Retrigger, 0, 0.0f }); }

	void setExpandedViewQuery(std::function<bool(const device::Descriptor&)> q) {
		engine_.setExpandedViewQuery(std::move(q));
	}

	//---------------------------------------------------------------------------------------------//
	// THE AUDIO SIDE                                                                               //
	//---------------------------------------------------------------------------------------------//

	/// **THE DRAIN** — called from EXACTLY TWO SITES, `process()` and `paramsFlush()`, which CLAP
	/// guarantees never run at once; that, not a lock, is why every write to engine state here is safe.
	/// Takes each publish whole, converges the engine to it, then the events. No allocation.
	void drain();

	/// **THE MIRRORS, ONCE A BLOCK, AFTER THE WORK** — what only the engine can say. No allocation.
	void publish(const RackEngine& engine) noexcept;

	/// THE RATE (OR THE LONGEST BLOCK) CHANGES — while inactive. Provisions built at the old one are dropped and built
	/// again.
	void prepare(double sampleRate, int maxFrames);

	//---------------------------------------------------------------------------------------------//
	// STATE — the record is the rack; a patch is the record as a value                            //
	//---------------------------------------------------------------------------------------------//

	/// **WHAT A SAVE WRITES** — the record, whole: every device in chain order (a tag this build cannot
	/// run included, in its place), its row, a container's body, how its card is shown, every route, and
	/// the macros.
	RackPatch patch() const;
	/// **WHICH PRESET THE RACK PLUGIN'S OWN BROWSER LAST LOADED OR SAVED** (backlog QC) — saved at the patch's root
	/// (`RackPatch::preset`), so an engine restart reopens the browser on it. Not an edit.
	[[nodiscard]] const PresetIdentity& topPreset() const noexcept { return topPreset_; }
	void setTopPreset(const PresetIdentity& identity) { topPreset_ = identity; }

	/// **IS THIS DEVICE'S CARD FOLDED** — view state the patch carries. The bridge holds it rather than the editor, so it survives an editor that closes and
	/// cannot survive a patch that is replaced; `rack_state.h` says why a fold cannot live on the desk.
	[[nodiscard]] bool deviceFolded(DeviceInstanceId device) const noexcept {
		return (cardViews_.flagsOf(device) & kViewFolded) != 0;
	}
	/// Folding is an EDIT of the patch, not of the sound: nothing is published and no block hears it.
	/// It still marks the patch dirty, as Feathers' does: a save would write it.
	void setDeviceFolded(DeviceInstanceId device, bool folded) noexcept {
		const uint32_t was = cardViews_.flagsOf(device);
		const uint32_t now = folded ? (was | kViewFolded) : (was & ~kViewFolded);
		if (now == was) {
			return; // a fold that folds nothing is not an edit
		}
		cardViews_.set(device, now);
		markStateDirty();
	}

	/// **WHAT A DEVICE'S OUTPUT IS CALLED ON THIS INSTANCE** — `ParamBridge::outputLabel`'s verbs on its
	/// rules: the player's label, else the declared name, else "Source". Held beside the folds and saved
	/// with them, and a relabel marks the patch dirty as a fold does.
	/// The `backplane::DeviceHost` verbs of the same names, so a card built over this host renames too.
	[[nodiscard]] std::string outputLabel(DeviceInstanceId device, size_t output) const override;
	[[nodiscard]] bool outputRelabelled(DeviceInstanceId device, size_t output) const noexcept override {
		return !outputLabels_.labelOf(device, output).empty();
	}
	/// Clamped; empty or the declared name clears; an undeclared output is ignored; unchanged moves nothing.
	void setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label) override;
	/// WHICH PRESET A DEVICE'S PANEL LAST LOADED OR SAVED (backlog QC) — the label map holds it, and the patch with it.
	[[nodiscard]] PresetIdentity devicePreset(DeviceInstanceId device) const override {
		const PresetIdentity* p = outputLabels_.presetOf(device);
		return p != nullptr ? *p : PresetIdentity {};
	}
	void setDevicePreset(DeviceInstanceId device, const PresetIdentity& identity) override {
		(void)outputLabels_.setPreset(device, identity);
	}
	/// **TOLD WHEN AN OUTPUT'S LABEL MOVES** — by `setOutputLabel` and by `loadPatch` once per (device,
	/// output) whose label differs from before. A list, `subscribeMacroRename`'s shape.
	[[nodiscard]] device::Subscription
	subscribeOutputLabel(std::function<void(DeviceInstanceId, size_t)> listener) override {
		return outputLabelSubscribers_.subscribe(std::move(listener));
	}
	/// **WHICH OF A DEVICE'S OUTPUTS ITS CARD SHOWS** (`DeviceHost::outputsShown`; Pattern design §7.10) — view state, never
	/// saved: a card sets it whenever it is built. A change tells the output-label listeners once, so each host re-lays.
	[[nodiscard]] uint64_t outputsShown(DeviceInstanceId device) const noexcept override {
		const auto it = outputsShown_.find(device);
		if (it != outputsShown_.end()) {
			return it->second;
		}
		const device::Descriptor* d = descriptorOf(device);
		return d != nullptr ? d->outputsShownByDefault() : ~uint64_t { 0 };
	}
	void setOutputsShown(DeviceInstanceId device, uint64_t mask) override {
		const uint64_t was = outputsShown(device);
		if (was == mask) {
			return;
		}
		const device::Descriptor* d = descriptorOf(device);
		if (d != nullptr && mask == d->outputsShownByDefault()) {
			outputsShown_.erase(device);
		}
		else {
			outputsShown_[device] = mask;
		}
		outputLabelSubscribers_.notify(device, static_cast<size_t>(std::countr_zero(was ^ mask)));
	}

	/// **HOW THE CANVAS IS SHOWN** — its extent and its fold, in the patch like a card's fold (2026-09-13: the partition size and expanded state persist with the patch).
	/// The editor reads it
	/// on the beat and writes it when a hand moves the partition or folds the band. Message thread.
	[[nodiscard]] CanvasView canvasView() const noexcept { return canvasView_; }
	void setCanvasView(CanvasView view) noexcept {
		if (view == canvasView_) {
			return; // a view that changes nothing is not an edit
		}
		canvasView_ = view;
		markStateDirty();
	}

	/// **WHETHER THE MACROS COLUMN IS FOLDED** (backlog QN) — patch state, `canvasView`'s posture: the editor writes it when its
	/// rail folds or unfolds and reads it on the beat, so a load (or a fresh instance) reopens the column the way it was left.
	[[nodiscard]] bool macrosCollapsed() const noexcept { return macrosCollapsed_; }
	void setMacrosCollapsed(bool collapsed) noexcept {
		if (collapsed == macrosCollapsed_) {
			return;
		}
		macrosCollapsed_ = collapsed;
		markStateDirty();
	}

	/// **WHETHER THE PATCH HAS BEEN EDITED SINCE THIS WAS LAST ASKED, and clears it** (backlog JV) —
	/// `ParamBridge::takeStateDirty`'s shape. The rack's plugin drains it once per main-thread visit and
	/// calls `clap_host_state.mark_dirty`, because nearly all of the rack is not a parameter: every device,
	/// row, route, body, fold, label, the canvas and the macros' modes, types and names. A macro's VALUE is
	/// not here — the host sent it, so the host knows.
	///
	/// THE POLICY IS FEATHERS': a write that changes what a save would write marks; one that changes
	/// nothing does not; and a LOAD never does — opening a patch must not report it modified. View state
	/// the patch does not carry (an expanded view open, a drawer, a solo) never reaches this bridge.
	/// A chain inside this rack marks it too (`outerGateFor`), and so does a body edited through its
	/// surface (`bodyEdited`).
	[[nodiscard]] bool takeStateDirty() noexcept { return stateDirty_.exchange(false, std::memory_order_relaxed); }
	/// **A LOAD THAT IS AN EDIT** (backlog OU): a preset picked by hand replaces the patch through `loadPatch`, which marks
	/// nothing, and the project has still been changed. This says so.
	void markEdited() noexcept { markStateDirty(); }
	/// See `DeviceHost::perVoiceUnavailable` — what this rack's holder said in its gate.
	const char* perVoiceUnavailable() const override { return perVoiceUnavailable_; }
	/// See `DeviceHost::bodyEdited`. A device this rack does not hold marks nothing.
	void bodyEdited(DeviceInstanceId device) override {
		if (record_.rowOf(device).live()) {
			markStateDirty();
		}
	}

	/// **A LOAD — THE RECORD BECOMES THE PATCH, HERE, NOW.** On the message thread; the engine converges
	/// at the next drain, and the editor draws the new rack at once. What the routing rule refuses is
	/// known before this returns — there is no later landing to wait for. A patch with nothing in it
	/// (`present == false`) is a fresh instance and changes nothing.
	LoadReport loadPatch(const RackPatch& patch);

	//---------------------------------------------------------------------------------------------//
	// HOST SLOTS (backlog JU, T10) — the pool, when this bridge owns one (`HostSlots::OwnPool`)       //
	//---------------------------------------------------------------------------------------------//
	//
	// `SlotOwner` over this bridge's record: the object Feathers' `ParamBridge` holds too, so the two plugins
	// keep one set of rules (`rack/slot_owner.h` has them). This bridge's share of its roads: `setField` is
	// the hand's (`onHandWrite`), `removeDevice` frees the device's slots, `patch`/`loadPatch` carry the pool,
	// and `serviceHostSlots` is the main-thread visit the plugin runs.

	/// THE POOL AND ITS ROADS, or null for a bridge that owns none. Its address is fixed for the bridge's life.
	[[nodiscard]] SlotOwner* hostSlots() noexcept { return slots_.get(); }
	[[nodiscard]] const SlotOwner* hostSlots() const noexcept { return slots_.get(); }

	/// **THE SLOTS' SHARE OF A MAIN-THREAD VISIT** (plan §1.3): what stopped resolving is freed
	/// (`reconcileIfMoved`), then `SlotOwner::service` anchors, rebinds and mirrors, and the record is
	/// committed when that changed it — NOT an edit, and nothing is marked: the host owns its automation.
	/// Returns how many slots it anchored; 0 for a bridge with no pool. Message thread.
	uint32_t serviceHostSlots() noexcept;

	/// `SlotOwner::setHostRecording`, from the plugin's transport — audio thread, lock-free.
	void setHostRecording(bool recording) noexcept {
		if (slots_ != nullptr) {
			slots_->setHostRecording(recording);
		}
	}

	//---------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                  //
	//---------------------------------------------------------------------------------------------//

	/// THE MIRROR BEFORE ANY BLOCK RUNS — a host may open the editor first.
	void primeForTest() { publish(engine_); }
	const ChainRecord& recordForTest() const noexcept { return record_; }
	/// The routes as published (backlog OJ cases count its growths and stand at its capacity).
	const backplane::RouteList& routeListForTest() const noexcept { return publishedRoutes_; }
	/// HOW MANY EVENTS THE CHANNEL HAS DROPPED — zero is the only healthy value.
	uint32_t droppedEditsForTest() const noexcept { return queue_.droppedCount(); }
	/// HOW MANY PROVISIONS ARE WAITING FOR A DRAIN.
	size_t pendingProvisionsForTest() const noexcept { return converger_.pendingCountForTest(); }
	/// HOW MANY DRAINS RAN THE CONVERGE PASS, and HOW MANY PUBLISHES WALKED THE MIRROR — backlog NR's skips, as numbers.
	uint64_t convergesForTest() const noexcept { return converges_; }
	uint64_t mirrorWalksForTest() const noexcept { return mirrorWalks_; }
	/// HOW MANY SITES THIS HOST'S CONVERGER HOLDS (backlog NR), wanted or waiting to be freed.
	size_t siteCountForTest() const noexcept { return converger_.siteCountForTest(); }
	/// WHAT THE MIRROR SAYS THE ENGINE RUNS FOR `device` — the engine's word, as of the last `publish`, or
	/// null for a device it did not run then. For a case proving the editor's view is the record's and
	/// never this.
	const device::Descriptor* mirrorDescriptorOfForTest(DeviceInstanceId device) const noexcept {
		for (size_t slot = 0; slot < mirrorLanes_.load(std::memory_order_acquire); ++slot) {
			if (relaxedLoad(m_.id[slot]) == device) {
				return relaxedLoad(m_.desc[slot]);
			}
		}
		return nullptr;
	}

private:
	/// AFTER EVERY CHANGE TO THE RECORD: build what it needs, then publish it and its routes — in that
	/// order, so a drain that sees the new state finds its arrivals already waiting.
	void commit();
	/// **AN EDIT OF THE PATCH** — this bridge's flag, and the host outside's when this rack is a chain in one.
	/// Nothing while `loadPatch` runs: a load is not an edit, here or in the patch around it.
	void markStateDirty() noexcept {
		if (loading_) {
			return;
		}
		stateDirty_.store(true, std::memory_order_relaxed);
		if (outerMarkDirty_ != nullptr) {
			outerMarkDirty_(outerDirtyHost_);
		}
	}
	/// **WHERE THE MIRROR HOLDS THE ENGINE'S WORD ABOUT `device`**, or `kNoSlot` — the engine's slot, found
	/// by the id each slot's words are stamped with (kill-lanes step 3a: the engine chooses its slots, and
	/// the record holds an ORDER rather than an address, so nothing but the id joins the two). A slot counts only while it runs the device
	/// the record has under that id NOW, as the record describes it — a mirror a publish behind is not read
	/// as another device's.
	size_t mirrorSlotOf(DeviceInstanceId device) const noexcept {
		if (device == kNoInstance) {
			return kNoSlot;
		}
		const device::Descriptor* wanted = descriptorOf(device);
		for (size_t slot = 0; slot < mirrorLanes_.load(std::memory_order_acquire); ++slot) {
			if (relaxedLoad(m_.id[slot]) == device) {
				const device::Descriptor* running = relaxedLoad(m_.desc[slot]);
				return running != nullptr && running == wanted ? slot : kNoSlot;
			}
		}
		return kNoSlot;
	}

	/// A RELAXED LOAD OF A MIRROR WORD THROUGH A CONST `this` — `atomic_ref<const T>` is C++26 and
	/// libc++ lacks it (macOS CI run 34093203285), so every const read goes through a non-const ref.
	/// Sound: no referenced object is const-DEFINED.
	template <class T>
	static T relaxedLoad(const T& word) noexcept {
		return std::atomic_ref<T>(const_cast<T&>(word)).load(std::memory_order_relaxed);
	}
	static float loadFloat(const float& cell) noexcept { return relaxedLoad(cell); }
	static size_t loadSize(const size_t& cell) noexcept { return relaxedLoad(cell); }

	RackEngine& engine_;
	/// See `secondsPerBeat`: written by `publish` (audio thread), read by the cards.
	std::atomic<double> secondsPerBeat_ { 0.0 };
	/// THE AS-MODULATED READOUT'S CHANNEL (backlog PU): asked by the editor, answered by `publish`.
	backplane::RouteProbeChannel routeProbe_;
	/// See `noteRefusedRoutes`.
	LoadReport loadReport_ {};

	// THE MESSAGE THREAD'S — the record, and what it has handed across.
	/// **THE COUNTER THIS BRIDGE FALLS BACK ON WHEN NOBODY LENDS IT ONE.** Declared before `ids_`, which
	/// may bind to it, and before `record_`, which is constructed from `ids_`.
	backplane::InstanceCounter own_;
	/// **WHERE INSTANCE IDS COME FROM — `own_`, or a holder's** (gap G3; see the constructor). It is a
	/// reference rather than a pointer because `ChainRecord` takes one and there is no "no counter"
	/// state: a bridge always issues from somewhere.
	backplane::InstanceCounter& ids_;
	ChainRecord record_;
	/// CONTAINER CONTENTS BY INSTANCE ID — a rack in a slot, loaded from a patch. Built into the device
	/// on this thread at provision; saved back out from here.
	ContainerBodies bodies_;
	/// HOW EACH DEVICE'S CARD IS SHOWN — see `deviceFolded`. Message thread only, beside the bodies and for
	/// the same reason: written by a load, read by a save, never published.
	DeviceViews cardViews_;
	/// WHAT EACH DEVICE'S OUTPUTS ARE CALLED — see `outputLabel`. Beside the folds, on their road.
	DeviceLabels outputLabels_;
	/// Which preset the rack plugin's own browser last loaded or saved (backlog QC) — `topPreset`.
	PresetIdentity topPreset_ {};
	device::Subscribers<DeviceInstanceId, size_t> outputLabelSubscribers_;
	/// WHICH OUTPUTS EACH DEVICE'S CARD SHOWS — see `outputsShown`. Absent: all.
	std::unordered_map<DeviceInstanceId, uint64_t> outputsShown_;
	CanvasView canvasView_;
	bool macrosCollapsed_ = false;
	PublishedChains published_;
	/// THE ROUTES, BY IDENTITY, AS THIS THREAD LAST PUBLISHED THEM — what the drain compiles.
	backplane::RouteList publishedRoutes_;

	// THE HANDOVER — mailboxes between the two threads.
	Converger converger_;

	// THE DRAIN'S — what it last took whole, and the routes the engine resolves through.
	ChainState drained_;
	/// WHERE A SNAPSHOT LANDS BEFORE IT IS KNOWN TO BE WHOLE — `PublishedChainState::adopt`.
	ChainState drainStaging_;
	/// **THE DRAIN'S TWO STATES, GROWN** (unbounded devices step 6) — when the record outgrows them, the main
	/// thread builds a bigger pair (`offerDrainGrowth`), the drain copies into them and swaps (moves only), and
	/// the main thread frees the old pair. Idle, Pending, Taken — the synth's and the engine's shape.
	struct DrainStates {
		ChainState drained;
		ChainState staging;
	};
	std::unique_ptr<DrainStates> drainGrowth_;
	std::atomic<int> drainGrowthState_ { 0 };
	size_t drainCapacity_ = 0; ///< the main thread's record of the shape it last gave the drain
	void offerDrainGrowth();
	void adoptDrainGrowth() noexcept;
	void reapDrainGrowth() noexcept;
	/// Grow the mirror's pools to `lanes`. Main thread.
	void growMirror(size_t lanes);
	size_t growthToken_ = 0;
	/// HOW MANY LANES EVERY MIRROR POOL REACHES — the bound every walk of the mirror uses, stored after the pools
	/// have grown, because each pool rounds to its own chunk and `size()` of one says nothing about another.
	std::atomic<size_t> mirrorLanes_ { 0 };
	/// **THE LAST PUBLISH WROTE AN EMPTY MIRROR, OVER THIS LAYOUT AND THIS MANY LANES** (backlog NR's first cheap
	/// win) — so the next one, finding the same, has nothing to write per lane: an empty lane's words are constants.
	/// A device arriving moves `layoutRevision`, and a mirror growing moves the lane count. Audio thread only.
	bool publishedEmpty_ = false;
	uint32_t publishedEmptyLayout_ = 0;
	size_t publishedEmptyLanes_ = 0;
	uint64_t converges_ = 0;   ///< see `convergesForTest`
	uint64_t mirrorWalks_ = 0; ///< see `mirrorWalksForTest`
	uint32_t drainedSequence_ = 0;
	/// `drainedSequence_` for the message thread (`sweepExports`): the last publish the audio thread adopted.
	std::atomic<uint32_t> drainedMirror_ { 0 };
	/// **A RISE NO BLOCK HAS READ IS NOT UNDONE BY A FALL** (`HeldFalls`): a quick press's release waits here for the drain.
	HeldFalls heldFalls_;
	/// **WHERE A WHOLE SNAPSHOT OF `publishedRoutes_` LANDS AT THE DRAIN**, before it is handed to the engine
	/// by identity (`RackEngine::setHostRoutes`, kill-lanes §2.4b) — the engine copies it, so this is reused
	/// the next drain. A `RouteStage`, grown with `publishedRoutes_` by its growth hook (backlog OJ).
	backplane::RouteStage routeStaging_;
	uint32_t routesSequence_ = 0;

	RackEditQueue queue_;

	/// **THE MIRRORS** — plain values reached through `atomic_ref`, this codebase's idiom for the shape.
	/// **PER ENGINE SLOT, EACH STAMPED WITH THE DEVICE IT IS ABOUT** — the slot is the engine's private index,
	/// so a reader finds its device by `id` (`mirrorSlotOf`), never by a number of its own.
	struct Mirror {
		// POOLS (unbounded devices step 6): the engine grows, and these grow with it on the main thread while the
		// block goes on writing them — nothing moves. `size()` is a capacity, past the engine's lanes.
		backplane::ChunkedPool<DeviceInstanceId, 64> id;            ///< per slot: which device the words below are about
		backplane::ChunkedPool<const device::Descriptor*, 64> desc; ///< per slot: what the ENGINE runs
		backplane::ChunkedPool<float, 2048> modulated;              ///< slot-major, `controlStride_` wide
		backplane::ChunkedPool<float, 2048> modulatedR;             ///< the right ear (KU), shaped as `modulated`
		backplane::ChunkedPool<uint64_t, 128> stereoLive;           ///< per slot, kControlMaskWords words: which controls are stereo-live
		backplane::ChunkedPool<size_t, 64> fieldsPresent;           ///< per slot, the engine's width
		backplane::ChunkedPool<float, 4096> readings;               ///< slot-major, kMaxReadings wide
		backplane::ChunkedPool<size_t, 64> readingCount;            ///< per slot
		backplane::ChunkedPool<uint64_t, 64> playhead;              ///< ordinal << 32 | phase bits, per slot
		backplane::ChunkedPool<float, 64> playheadLevel;            ///< per slot, the level beside it, negative for none
		backplane::ChunkedPool<uint64_t, 64> trigFires;             ///< per slot, 1 << 32 | count, or 0 for none (MV)
		/// slot-major, `device::kRecentEvents` wide: the newest events, a word each (`device::packRecentEvent`)
		backplane::ChunkedPool<uint64_t, 1024> recent;
		/// per slot, the extra state the device changed itself (backlog MX, `device/extra_state.h`)
		backplane::ChunkedPool<device::ExtraStateSnapshot, 16> extraState;
		std::vector<float> macro;
		double sampleRate = 0.0;
	};
	Mirror m_;
	/// THE BLOCK'S SCRATCH FOR ONE DEVICE'S EXTRA STATE, on its way into `m_.extraState` — the audio thread's, no allocation.
	std::array<float, device::kMaxExtraStateFloats> extraScratch_ {};
	/// The macros' modes — the message thread's, not mirrored from the engine: the hand is the source.
	std::vector<uint8_t> macroBipolar_;
	/// Their types and names — the message thread's alone, never queued (see `setMacroType`). A name is
	/// held EMPTY when it is the default.
	std::vector<device::MacroType> macroType_;
	std::vector<std::string> macroName_;
	device::Subscribers<size_t> macroRenameSubscribers_;
	/// See `takeStateDirty`. Message thread on both sides; atomic for `ParamBridge::stateDirty_`'s shape.
	std::atomic<bool> stateDirty_ { false };
	/// Inside `loadPatch`, whose own verbs must not read as edits (`markStateDirty`).
	bool loading_ = false;
	/// The host outside this rack, when it is a chain in one — `device::OuterGate::markDirty`.
	void* outerDirtyHost_ = nullptr;
	void (*outerMarkDirty_)(void*) = nullptr;
	size_t controlStride_ = 0;
	/// See `setOwnerExports`.
	const ExportTable* ownerExports_ = nullptr;
	size_t readingStride_ = 0;
	/// THE HOST SLOTS, for the outermost rack only (see the constructor) — held apart so a nested bridge
	/// carries no pool, bus or outbox at all. After `record_`, which it names.
	std::unique_ptr<SlotOwner> slots_;
	/// See `macroOutbox` — made with `slots_`, for the same one bridge.
	std::unique_ptr<MacroOutbox> macroOutbox_;
	/// **A NESTED RACK'S SHARE OF THE OUTERMOST POOL** (T11) — for every bridge that owns none; unlinked, and so
	/// answering "no slot", until a host hands it a link. After `record_`, which it names.
	std::unique_ptr<NestedSlots> nested_;
	/// See `setMacrosAreDeviceControls`.
	bool macrosAreDeviceControls_ = false;
	/// See `perVoiceUnavailable`; from the holder's gate (`setOuterGate`).
	const char* perVoiceUnavailable_ = nullptr;
	/// The renamed macros, as captions of the device's Macro controls, sent up (`setMacrosAreDeviceControls`).
	void pushMacroCaptions();
	/// See `fragmentOrigin`: a serial, never this bridge's address.
	const uint64_t fragmentOrigin_ = mintFragmentOrigin();
	/// See `fragmentLife`: an undo record over this bridge holds the weak half.
	std::shared_ptr<const void> life_ = std::make_shared<char>(0);
};

} // namespace feathers::rack
