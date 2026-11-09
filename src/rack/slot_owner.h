/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/modulatable.h" // `HostSlotStatus`, what the right-click's status query answers in
#include "device/registry.h"    // `HostSlotLink`, the nested levels' road to this pool (T11)
#include "hostslots/slot_bus.h"
#include "hostslots/slot_pool.h"
#include "hostslots/text_site.h"
#include "rack/chain_record.h"

#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

// **THE HOST SLOTS OF ONE `ChainRecord` — WHAT EVERY PLUGIN THAT OWNS A POOL HOLDS** (backlog JU;
// tasks T6a, T6b, T10).
//
// TWO PLUGINS OWN A POOL: Feathers, whose `ParamBridge` holds its two chains, and the standalone rack, whose
// top `RackBridge` holds its one (decided 2026-09-25). Both keep their devices in a `ChainRecord`,
// and everything a pool needs from its owner is a question that record answers — does a target still name a
// control, what is it set to, which device a binding drives. So the pool, the lock-free roads either side of
// it and the service that joins them live HERE, once, over the record, and each bridge holds one of these
// and delegates. It was written first inside `ParamBridge` (T6a/T6b) and moved here whole for T10, so the
// two plugins cannot drift apart on a rule neither of them owns.
//
// WHAT STAYS WITH THE OWNER, because only it knows: HOW the record is published (`service` takes the
// owner's publish and calls it at the one point the remap window allows), which road a LOAD takes (a project
// restores the patch's pool; Feathers' preset road keeps the project's), when the patch is DIRTY, and the
// CLAP surface (`hostslots/slot_host.h`, which this file does not include: the rack library is CLAP-free here).
//
// FOUR ROADS MEET AT A BOUND CONTROL, and the bus is the authority the engine applies
// (`RackEngine::applyHostSlots`), so every road but the host's must reach it:
// - THE HOST (audio thread, or flush): the plugin denormalises through `descOf` and writes the bus
//   (`onHostValue`). `service` mirrors the value into the record and anchors the slot.
// - A HAND (the owner's control write, which undo and redo also take): `onHandWrite` SEEDS the bus before
//   the record publishes, and sends the new value to the host through the outbox — unless the write put the
//   value already there, which is not an edit (T6b review).
// - A LOAD, AN UNDO OF A REMOVE, A NEW MAPPING: the record's bindings are rebuilt from the pool by
//   `service`, and every binding that is new to the record is seeded from the record first.
// - A ROW REWRITTEN WHOLE, or a chain rebuilt: only the bound controls whose record value the write CHANGED
//   are seeded, and never over a host write the mirror has not committed (`captureBound` before the write,
//   `seedChangedBound` after it), by the owner, before it publishes.
// - AND A BACKSTOP in the mirror, for a road that forgot to seed: a record that disagrees with a bus the host
//   has not moved is seeded by ONE CAS against the snapshot it compared, so it loses to any host write in
//   between. A forgotten seed still costs the engine the old value until the next visit, so a new road that
//   writes a device field must still seed at the write.
//
// THE T6b REVIEW'S FIXES LIVE HERE, so both plugins have them: the packed {value, gen} bus cell read as one
// `load` and seeded by `seedIf`; `seedChangedBound`; the CAS backstop; the skip of a write that changed
// nothing; a hand noted slot-or-no-slot so a control bound or remapped mid-drag opens its Begin before its
// first Value; the remap window (a rebound slot's declaration nulled before the record changes and published
// only after the owner publishes); and the reconcile and rebind gated on the slot STRUCTURE rather than the
// record's revision, which every field write bumps.
//
// MESSAGE THREAD, like the record, except where a member says otherwise (`descOf`, `onHostValue`,
// `onHostMod`, the outbox's consumer side, `setHostRecording`).

namespace feathers::rack {

class SlotOwner {
public:
	using Bus = hostslots::SlotValueBus<>;
	using Outbox = hostslots::SlotOutbox<>;

	/// `record` must outlive this; the owner declares this after its record.
	explicit SlotOwner(ChainRecord& record) noexcept : record_(record) {}
	SlotOwner(const SlotOwner&) = delete;
	SlotOwner& operator=(const SlotOwner&) = delete;

	[[nodiscard]] hostslots::SlotPool& pool() noexcept { return pool_; }
	[[nodiscard]] const hostslots::SlotPool& pool() const noexcept { return pool_; }
	/// **THE LOCK-FREE CELLS BETWEEN THE HOST'S EVENTS AND THE ENGINE** (`slot_bus.h`). Its address is fixed
	/// for this object's life, which is what lets an engine hold a pointer to it (`setHostSlotBus`).
	[[nodiscard]] Bus& bus() noexcept { return bus_; }
	[[nodiscard]] const Bus& bus() const noexcept { return bus_; }
	/// **THE EDITOR-TO-HOST RING** (`slot_bus.h`). This produces (main thread); the plugin drains it in
	/// `process` and `params.flush` (`hostslots::drainOutbox`).
	[[nodiscard]] Outbox& outbox() noexcept { return outbox_; }

	//--------------------------------------------------------------------------------------------//
	// TARGETS AND NAMES (T6a)                                                                      //
	//--------------------------------------------------------------------------------------------//

	/// **THE SLOT TARGET FOR CONTROL `control` OF `device`** — path `[device]` (this record is the outermost
	/// level), the control's KEY, the device's tag, and the label the host shows (`labelOf`). nullopt for a
	/// device the record does not hold or a control it does not declare.
	[[nodiscard]] std::optional<hostslots::Target> targetOf(DeviceInstanceId device, size_t control) const;

	/// **WHAT THE HOST CALLS A SLOT MAPPED TO THIS CONTROL: "Device · Control"** — the device's own name
	/// (`traits.name`) and the control as its card captions it (the declared short form, else the name
	/// without the device's own leading word). The device is numbered by its position in its chain, as the
	/// route list numbers it, ONLY when another device of the same type is in any chain: a slot's label is
	/// read in a host's narrow lane, where "Filter 1 · Cutoff" beside no other filter is noise. Empty for an
	/// unknown device or control.
	///
	/// **A LABEL IS FIXED WHEN THE SLOT IS TAKEN** (it is stored in the pool's `Target` and saved with it): a
	/// twin added later does not renumber an existing slot, and removing the twin does not un-number it.
	/// Chosen to spare the host rename churn — every renumber would be a `RESCAN_INFO` for a name the player
	/// did not change.
	[[nodiscard]] std::string labelOf(DeviceInstanceId device, size_t control) const;

	/// **DOES THIS TARGET NAME A CONTROL THE RECORD HOLDS, AND WHAT IS IT SET TO.** The path's one id must be
	/// a device the record holds whose tag matches the target's (ids are patch-scoped, so after a load the
	/// same id may be a different device), and the key must be one of its controls. `*value` is the
	/// record's field, or the control's default past `fieldsPresent`. A NESTED path (T11) resolves from what
	/// its levels reported — and a PENDING one (a level not reported yet) resolves from the catalogue, so a
	/// reconcile keeps it; its value is the bus's once its level has adopted the binding (see the NESTED banner).
	bool resolve(const hostslots::Target& target, const device::ParamDesc** desc, double* value) const;

	/// **FREES EVERY SLOT WHOSE TARGET DOES NOT RESOLVE** (plan §0.7, §1.3, §2: "a key or tag that no longer
	/// resolves at load leaves the slot Empty, including an anchored one"). Returns how many. Each is freed
	/// alone (`SlotPool::free`), keeping its note, so every other slot's undo note survives. Runs at every
	/// load, and from every main-thread visit through `reconcileIfMoved` — which is what catches the roads
	/// that change a device without removing it (a replacement under the same id, a chain rebuilt whole).
	uint32_t reconcile();
	/// `reconcile`, only when the slot STRUCTURE (which device, by id and declaration, is where; the binding
	/// table) or the pool's generation moved since the last one — the per-visit form, cheap when nothing
	/// changed. NOT the record's revision, which every field write bumps: while automation plays the mirror
	/// writes one every visit, and a revision gate reran the whole reconcile for nothing (T6b review).
	uint32_t reconcileIfMoved();

	/// **A LOAD PUTS THIS POOL IN PLACE** — the host's `state.load`, a project opening (plan §0.8). An absent
	/// snapshot, or one `SlotPool::restore` refuses, is an EMPTY pool. Then `reconcile`, because the record
	/// may have re-id'd a device (plan §0.7). Call AFTER the record holds the loaded chains.
	/// **THE LOAD WINDOW** (integration review): every declaration is nulled and the host-wrote and touched
	/// bits so far are discarded, so until the next visit rebinds, a host write is dropped rather than scaled
	/// by the old patch's declaration, and nothing about the old pool anchors a slot of the restored one.
	/// **AND A SAVED ENTRY NAMING A CONTROL NO SLOT MAY CARRY (`ParamDesc::hostSlot` false) IS DROPPED, NOT SEATED** — counted
	/// in the return, its label the first, for the load report (`LoadReport::Kind::SlotsDropped`).
	struct Refused {
		uint32_t count = 0;
		std::string first;
	};
	Refused restore(const std::optional<hostslots::PoolSnapshot>& snapshot);

	/// **WHAT A SAVE WRITES AS `hostSlots`**: the pool's snapshot, or nullopt when no slot is mapped — so a
	/// patch that never exposed a control is byte-for-byte what it was before slots existed.
	[[nodiscard]] std::optional<hostslots::PoolSnapshot> snapshotForSave() const;

	//--------------------------------------------------------------------------------------------//
	// THE WRITE ROADS (T6b)                                                                        //
	//--------------------------------------------------------------------------------------------//

	/// **THE DECLARATION OF THE CONTROL SLOT `n` DRIVES, for the audio thread** — what a host's normalised
	/// write is denormalised against. Published by `service` beside the binding; null for a slot that drives
	/// nothing. The pointer is into a device's static declaration, never freed. Lock-free, no allocation.
	[[nodiscard]] const device::ParamDesc* descOf(size_t n) const noexcept {
		return n < desc_.size() ? desc_[n].load(std::memory_order_acquire) : nullptr;
	}

	/// **THE HOST WROTE SLOT `n`** — AUDIO THREAD in `process`, or whichever thread the host flushes on
	/// (while a plugin sleeps, flush is the ONLY road a host write takes, plan §0.13). Denormalised against
	/// `descOf` and written to the bus, which sets the `hostWrote` bit the next visit anchors and mirrors.
	/// FALSE, and nothing written, for a slot that drives nothing yet (Empty, or mapped since the last
	/// visit): there is no declaration to denormalise against. On true the caller asks for a main-thread
	/// visit (`request_callback`), which is what brings the mirror round with no editor open. No allocation.
	bool onHostValue(hostslots::SlotIndex n, double normal) noexcept;

	/// **GLOBAL HOST MODULATION ON SLOT `n`** — an offset on top of its value (design §4), in the slot's
	/// normalised units, which the engine scales by the control's span. Not an anchor (`SlotValueBus::onHostMod`).
	/// Audio thread; the caller has already refused a per-note modulation.
	void onHostMod(hostslots::SlotIndex n, double amount) noexcept {
		bus_.onHostMod(n, static_cast<float>(amount));
	}

	/// **WHETHER THE HOST SAYS IT IS RECORDING** (`CLAP_TRANSPORT_IS_RECORDING`), as of its last transport.
	/// Written by the audio thread every block, read by a gesture on the main thread: a hand on a slotted
	/// control while this is set anchors the slot (design §5).
	void setHostRecording(bool recording) noexcept { hostRecording_.store(recording, std::memory_order_relaxed); }
	[[nodiscard]] bool hostRecording() const noexcept { return hostRecording_.load(std::memory_order_relaxed); }

	/// **A HAND TOOK / LET GO OF A CONTROL** (`DeviceControl::beginGesture`/`endGesture`, through the owner's
	/// `DeviceHost`). A control with a slot sends the host Begin / End on it, and a Begin while the host
	/// records marks the slot touched-while-recording (anchored at the next visit). THE HAND IS NOTED SLOT OR
	/// NO SLOT (T6b review): a control bound, or remapped, mid-drag opens its Begin on its new slot at its
	/// first Value there (`onHandWrite`). A full hand table forgets the hand, which costs only that late Begin.
	void beginGesture(DeviceInstanceId device, size_t control) noexcept;
	void endGesture(DeviceInstanceId device, size_t control) noexcept;

	/// **THE RECENT RING: A HAND TOOK THIS CONTROL** (T9; design §2, §4). The owner's `touchControl`, called
	/// just before `beginGesture`, from a hand and nothing else (see `DeviceHost::touchControl`).
	/// `SlotPool::touch`: a slotted control moves to the front of the recent order and nothing else happens;
	/// an unslotted one takes Empty → the lowest-priority untouched Default → the least recently used Recent
	/// (only when nothing else is left), and becomes Recent. A pool of Pinned and Anchored gives nothing, and
	/// says nothing. True iff a NEW mapping was made (the saved pool changed: the owner marks dirty).
	///
	/// **WHAT THE HOST SEES ON THE FIRST DRAG** — the binding (and the declaration the audio thread needs) is
	/// published at the NEXT main-thread visit, as every mapping is (the remap window), so nothing reaches the
	/// host on the new slot before it. At that visit, after the rename is owed (one `RESCAN_INFO` per visit):
	/// 1. `Value(v)` OUTSIDE ANY GESTURE — the takeover: the slot now reports this control (design §4: a
	///    plain value, so nothing records it);
	/// 2. `Begin`, then `Value(v)` again inside it — the hand's gesture opens where the hand has the control
	///    NOW, so a host recording takes the drag from this point (and a Begin while recording anchors, T6b);
	/// 3. then every later drag Value, and `End` at release. A hand that had ALREADY let go by the visit (a
	///    click, or a drag shorter than one editor frame) gets its `End` at once: its edit is one gesture
	///    carrying its final value.
	/// `v` is the control's record value at the visit, so the motion INSIDE the frame before the visit is not
	/// sent point by point: the host hears where it got to. The engine is unaffected (it runs the record).
	bool touch(DeviceInstanceId device, size_t control) noexcept;
	/// `touch`'s body for any target, this level's or a nested one's (`nestedTouch`). May throw (allocation).
	bool touchTarget(const hostslots::Target& target);
	/// **WHAT THE TRAY SAYS ABOUT A RECYCLED SLOT** (design §2: the last resort is announced), then cleared:
	/// "Host slot 23: Delay · Mix -> Filter · Cutoff" — the slot as the host numbers it, the label it held and
	/// the label it holds now (" -> ", because the editor's font has no arrow glyph). Only a touch that
	/// recycled a used Recent sets it; an Empty or Default takeover is silent. HELD WHILE ANY HAND IS DOWN, so a
	/// knob's own readout during the drag cannot overwrite it a frame later: said once the hand lets go. Of two
	/// before a take, the later one is said.
	[[nodiscard]] std::string takeNotice();

	/// THE HOST SLOT THE RECORD BINDS TO THIS CONTROL, or `kNoSlot`. What the engine drives, so what every
	/// write road must tell.
	[[nodiscard]] hostslots::SlotIndex boundTo(DeviceInstanceId device, size_t control) const noexcept;

	/// **A HAND WROTE `coerced` TO A CONTROL, AND THE RECORD HOLDS IT; THE OWNER PUBLISHES NEXT.** For a bound
	/// control: the bus FIRST (the engine applies the bus over the record at every drain, so a value that
	/// reached only the record would be put back to the host's last one by the very next block), with the
	/// seed's generation taken as committed so the mirror does not read the hand back as the host; and the
	/// host hears the new value on the slot — inside the gesture `beginGesture` opened, so it records.
	///
	/// **A WRITE OF THE VALUE ALREADY THERE IS NOT AN EDIT** (T6b review): `recordHadIt` is the owner's reading,
	/// taken BEFORE its record write, of whether the record already held `coerced`. When it did AND the host has
	/// not moved the bus since the mirror last committed, nothing is seeded and the host is told nothing — a row
	/// writer puts every field of a device, and the ones it did not change must neither seed over a host write
	/// nor send the host a Value outside any gesture. A HAND (a gesture open on this control, `beginGesture`)
	/// writing the record's value while the host's newer one is unmirrored IS a real edit, and wins; the same
	/// write with no hand on the control — a row writer carrying it — leaves the host's value to the mirror.
	///
	/// **NO VALUE OUTSIDE ITS GESTURE AFTER A REMAP** (T6b review): a hand that took this control before it had
	/// this slot opens a Begin here first; a gesture still open on the slot for ANOTHER control is closed, so
	/// this Value is not filed under it.
	void onHandWrite(DeviceInstanceId device, size_t control, float coerced, bool recordHadIt) noexcept;
	/// **`recordHadIt`, AS BOTH OWNERS READ IT** before their write: the record's value of the control — its
	/// field, or its declared default past `fieldsPresent` — is `value`. A default counts: a fresh device's row
	/// may carry no fields at all, and a settings road writing the default it already ran with is no edit.
	[[nodiscard]] bool recordHolds(DeviceInstanceId device, size_t control, float value) const noexcept {
		return record_.holds(device) && recordValueOf(device, control) == value;
	}

	/// **THE BOUND CONTROLS' RECORD VALUES BEFORE A ROAD REWRITES ROWS** (T6b review), so the seed after it can
	/// tell the controls the write CHANGED from the ones it merely carried at their old value.
	struct BoundBefore {
		std::array<float, hostslots::kSlotBusCapacity> value {};
		std::array<bool, hostslots::kSlotBusCapacity> had {};
	};
	/// Of device `device`'s bound controls, or of every bound control for `kNoInstance`. Before the write.
	[[nodiscard]] BoundBefore captureBound(DeviceInstanceId device) const noexcept;
	/// **SEED ONLY WHAT THE WRITE CHANGED, AND NEVER OVER THE HOST.** For each control captured in `before`
	/// whose record value moved: skipped if the host has written the slot since the mirror last committed it
	/// (its write is newer than anything the record knows, and the mirror will put it in the record), else ONE
	/// CAS from what was observed to the record's value (`SlotValueBus::seedIf`), which loses to a host write
	/// landing in between. After the write, before the owner publishes.
	void seedChangedBound(const BoundBefore& before) noexcept;

	//--------------------------------------------------------------------------------------------//
	// THE RIGHT-CLICK'S VERBS (T7) — moved here at the integration so the standalone rack's knobs     //
	// get "Expose to host" too. DIRTY MARKING STAYS WITH EACH OWNER: each verb says whether it       //
	// changed the pool, and the owner marks its patch its own way.                                   //
	//--------------------------------------------------------------------------------------------//

	/// **WHERE A CONTROL STANDS WITH THE POOL, for its right-click and its knob's mark.** Asked of the POOL,
	/// not the record's bindings: a pin made this tick is Pinned at once, while its binding waits for the next
	/// visit. `Unslotted` for a control no slot names, with `canExpose` saying whether a pin would find a slot
	/// (exactly when `SlotPool::takeFor` would: an Empty, a Default or a Recent); `Unavailable` for a control
	/// the record does not hold (no such device, an index past its declarations, a control with no key).
	/// One walk, no `Target` built: a knob asks this when the pool's generation moves.
	[[nodiscard]] device::HostSlotStatus statusOf(DeviceInstanceId device, size_t control) const;
	/// **"EXPOSE TO HOST"**: `SlotPool::pin` on the control's target. The host hears the slot's new name at
	/// the next visit (T6a's batching). A slot recycled from a used Recent sets `*displaced` to the label it
	/// held. True iff the pool changed — false when nothing could be taken, or it was Pinned already.
	bool expose(DeviceInstanceId device, size_t control, std::string* displaced);
	/// Pinned → Recent (`SlotPool::unpin`: the slot stays mapped, and becomes recyclable). True iff it changed.
	bool unpin(DeviceInstanceId device, size_t control);
	// NO `releaseAnchor`: no menu offers it, since the host's lanes stay on the id whatever
	// the plugin does. `SlotPool::release` remains as the pool's API.

	/// **THE SLOTS' SHARE OF A MAIN-THREAD VISIT, AFTER `reconcileIfMoved`** (plan §1.3):
	/// 1. every slot the host wrote, or a hand touched while recording, is ANCHORED;
	/// 2. the record's bindings are rebuilt from the pool when the slot structure or the pool moved — so a
	///    binding whose slot was freed vanishes — each new one seeded from the record first;
	/// 3. the RECORD MIRROR: a slot whose bus generation moved past what was committed has its value
	///    written into the record — no undo step and NOT marked dirty, as a core param's host automation is
	///    not (the host owns its automation; the patch did not change). A slot the host has not moved whose
	///    record disagrees is seeded from the record by CAS (the backstop above);
	/// 4. `publish()` — the owner's own publish of the record — when any of that changed it;
	/// 5. ONLY THEN the rebound slots' declarations (`descOf`) for the audio thread: the REMAP WINDOW (T6b
	///    review). A slot whose binding changed had its declaration NULLED in step 2, before the record
	///    changed, so until the record that binds its new target is published a host write on it is dropped
	///    rather than denormalised by one target's range and applied by the engine to the other.
	/// A `publish` that throws leaves the declarations nulled and the publish owed: the next visit publishes
	/// and then hands them over.
	/// `anchored` counts every slot the host wrote or a hand touched while recording, already-Anchored ones
	/// included. `poolChanged` is true when a slot was NEWLY anchored — the saved pool changed, so the owner
	/// marks its patch dirty (T7 review: releasing an anchor marks dirty, so taking one must too; a host
	/// write on a slot that is already Anchored changes nothing saved, and marks nothing).
	struct Visit {
		uint32_t anchored = 0;
		bool recordChanged = false;
		bool poolChanged = false;
	};
	template <class Publish>
	Visit service(Publish&& publish) noexcept {
		Visit visit = serviceRecord();
		visit.recordChanged |= publishOwed_;
		if (visit.recordChanged) {
			try {
				publish();
				publishOwed_ = false;
			}
			catch (...) {
				publishOwed_ = true;
				return visit; // the declarations wait for the record that binds them
			}
		}
		publishPendingDescs();
		return visit;
	}

	//--------------------------------------------------------------------------------------------//
	// NESTED LEVELS (T11) — a control inside a nested rack takes its slot from THIS pool (design §7). //
	//--------------------------------------------------------------------------------------------//
	//
	// A nested level reaches this object through a `device::HostSlotLink` (`linkFor`), which carries its PATH
	// from here: the owner's record holds `path[0]`, the level at `[path[0]]` holds `path[1]`, and so on. What
	// this side keeps about a nested level is only what it REPORTED — its devices' ids and declarations, by
	// path — never a pointer to it (see `HostSlotLink`: nothing points down). What the level keeps is its own
	// record's side of the four roads (`rack::NestedSlots`): its bindings, its mirror and its seeds, over the
	// one bus, because only it can write its record. Here: the pool, the names, the gestures on the outbox,
	// the declarations the audio thread denormalises by, and the remap window across levels (`adopted`).
	//
	// A NESTED TARGET IS ONE OF THREE THINGS (`Reach`): Resolved (every level on its path reported, and the
	// leaf declares the key), Gone (some level holds no such device, or the leaf's kind or key changed) or
	// PENDING — a level on its path has not reported yet, as every nested level has not in the moment after a
	// project load. A pending slot is kept (a reconcile frees only Gone: an anchor must survive the load it
	// was saved for) and named from the catalogue, but binds nothing until its level reports.

	/// **THE LINK A CONTAINER AT `[self]` IN THIS RECORD IS HANDED** (the owning bridge's converger maker). Null
	/// bus or no dirty hook set is still a link; `dirtyHook` marks the owner's patch when a nested verb changes
	/// the pool, which is saved with the OUTERMOST patch (design §7).
	[[nodiscard]] device::HostSlotLink linkFor(DeviceInstanceId self) noexcept;
	void setDirtyHook(void* host, void (*markDirty)(void*)) noexcept {
		dirtyHost_ = host;
		markDirty_ = markDirty;
	}

	/// The link's verbs, callable directly (the link's trampolines call these). `path` names the device.
	void reportLevel(const device::SlotPath& prefix, uint8_t lane, std::span<const device::HostSlotLevelDevice> devices);
	/// **THE CONTAINER AT `path` NAMES SOME OF ITS OWN CONTROLS** (a nested rack's renamed macros — T11). Every slot under `path` is relabelled when that changed, which the host hears as one
	/// rename (`SlotPool::rebase` onto the same path: ids, states and recency kept). True if anything changed.
	bool reportCaptions(const device::SlotPath& path, std::span<const device::HostSlotCaption> captions);
	/// **THE CONTAINER AT `path` PRINTS ITS CONTROLS ANEW** (`HostSlotLink::retext`): every slot at or under it is owed a
	/// rescan (`SlotPool::retextUnder`). Returns how many.
	uint32_t retextUnder(const device::SlotPath& path);
	/// **A HAND CHANGED A CONTROL THE DEVICE'S OWN WORDS FOLLOW** (a gate on a device with `TextOps::parse` — the
	/// Rack's Mode): its slots are owed new text. Called from `onHandWrite` for a write that changed the record.
	void retextIfWordsFollow(DeviceInstanceId device, size_t control) noexcept;
	/// **THE DEVICE BEHIND A ONE-LEVEL TARGET, FOR ITS OWN WORDS** (`device::TextOps`): its id, and in `site` its text
	/// hook, the control's index and its fields as the record holds them (defaults filled). The surface is the
	/// caller's to add — the bridges file the instances. False for a nested target or one that does not resolve.
	bool textSite(const hostslots::Target& target, DeviceInstanceId* device, hostslots::TextSite* site) const;
	[[nodiscard]] device::HostSlotStatus nestedStatusOf(const device::SlotPath& path, size_t control) const;
	bool nestedExpose(const device::SlotPath& path, size_t control, std::string* displaced);
	bool nestedUnpin(const device::SlotPath& path, size_t control);
	/// A NESTED HAND took the control at `path` (T9): `touch`, by path. True iff a new mapping was made.
	bool nestedTouch(const device::SlotPath& path, size_t control) noexcept;
	/// Frees every slot under `path` (its notes kept, as `freeUnder` keeps them), and forgets the levels there.
	void nestedFreeUnder(const device::SlotPath& path);
	/// **BACKLOG OA, AT ANY DEPTH** (`path` from the outermost device): the undo of a free — the slots noted under
	/// `path` come back where nothing took them (`SlotPool::restoreUnder`); and a device moved a level, every slot
	/// and note under `from` re-pointed under `to` and relabelled (`SlotPool::rebase`), false when refused whole.
	/// Either marks the owner's patch when the pool changed.
	bool restoreUnderPath(std::span<const DeviceInstanceId> path);
	bool rebasePath(std::span<const DeviceInstanceId> from, std::span<const DeviceInstanceId> to);
	/// **A NESTED HAND'S GESTURES AND VALUES, ON THE OUTBOX** — the same rules `beginGesture`/`onHandWrite` keep
	/// for this record's controls (one gesture per slot, a remap closes the old one, a Begin while the host
	/// records marks the slot), keyed by PATH so two levels' equal ids never share one.
	void nestedBeginGesture(const device::SlotPath& path, size_t control, hostslots::SlotIndex n) noexcept;
	void nestedEndGesture(const device::SlotPath& path, size_t control) noexcept;
	void nestedHandValue(const device::SlotPath& path, size_t control, hostslots::SlotIndex n, float value01,
	                     bool hand) noexcept;
	[[nodiscard]] uint64_t nestedBindingGeneration() const noexcept { return nestedBindingGen_; }
	/// Every slot bound to a control of a device directly under `prefix`: `fn(slot, device, tag, key)`.
	template <class F>
	void forEachBindingUnder(const device::SlotPath& prefix, F&& fn) const {
		for (size_t n = 0; n < nestedBindings_.size(); ++n) {
			const NestedBinding& b = nestedBindings_[n];
			if (b.live && b.prefix == prefix) {
				const hostslots::Target& t = pool_.target(static_cast<hostslots::SlotIndex>(n));
				fn(static_cast<hostslots::SlotIndex>(n), b.device, t.tag.c_str(), t.key.c_str());
			}
		}
	}
	/// The level at `prefix`/`lane` has published what `bindingGeneration` said. See `HostSlotLink::adopted`.
	void levelAdopted(const device::SlotPath& prefix, uint8_t lane, uint64_t bindingGeneration) noexcept;

	/// The target a nested control would take, named "Chain · Delay · Mix"; nullopt until its level reported it.
	[[nodiscard]] std::optional<hostslots::Target> nestedTargetOf(const device::SlotPath& path, size_t control) const;
	[[nodiscard]] std::string nestedLabelOf(const device::SlotPath& path, size_t control) const;

	/// TEST SEAMS: how many nested levels have reported, and whether slot `n`'s declaration is being held
	/// back for a level to adopt its binding.
	[[nodiscard]] size_t nestedLevelCountForTest() const noexcept { return levels_.size(); }
	[[nodiscard]] bool heldForTest(size_t n) const noexcept { return n < held_.size() && held_[n]; }

private:
	enum class Reach : uint8_t { Resolved, Pending, Gone };
	/// Where a nested target stands (see the banner); `*leaf` its device's declaration when known.
	[[nodiscard]] Reach reachNested(const hostslots::Target& target, const device::Descriptor** leaf) const;
	/// Whether `prefix` still names a chain of containers: the owner's record holds a container at `prefix[0]`,
	/// and each reported level on the way holds the next as a container. Pending counts as reaching.
	[[nodiscard]] bool prefixReaches(std::span<const DeviceInstanceId> prefix) const;
	/// The levels reported at exactly `prefix` (any lane).
	template <class F>
	void forEachLevelAt(std::span<const DeviceInstanceId> prefix, F&& fn) const {
		for (const NestedLevel& level : levels_) {
			if (std::equal(level.prefix.begin(), level.prefix.end(), prefix.begin(), prefix.end())) {
				fn(level);
			}
		}
	}
	/// The declaration of the device at `path`, from the reports (null when not reported, or not held).
	[[nodiscard]] const device::Descriptor* reportedDescriptorOf(const device::SlotPath& path) const;
	/// A device's display name in its level: its name, numbered by its position there when a twin stands beside it.
	[[nodiscard]] std::string ownDeviceName(DeviceInstanceId device) const;
	[[nodiscard]] std::string levelDeviceName(std::span<const DeviceInstanceId> prefix, DeviceInstanceId device) const;
	/// A control as its card captions it — `labelOf`'s second half.
	[[nodiscard]] static std::string captionOf(const device::Descriptor& desc, size_t control);
	/// The same, unless the container at `path` named that control itself (`reportCaptions`).
	[[nodiscard]] std::string captionAt(std::span<const DeviceInstanceId> path, const device::Descriptor& desc,
	                                    size_t control) const;
	/// A target's label as it would be built now — for a relabel; its stored label when it no longer resolves.
	[[nodiscard]] std::string labelForTarget(const hostslots::Target& target) const;
	/// The status of whichever control the pool names by `path` and `key` — `statusOf`'s walk, for any depth.
	[[nodiscard]] device::HostSlotStatus statusFor(std::span<const DeviceInstanceId> path, const char* key) const;
	/// Close whatever nested gesture is open on slot `n` (a remap, or one of this record's controls taking it).
	void closeNestedGestureOn(size_t n) noexcept;
	/// Hand the audio thread the held declarations once every reported level has adopted what they bind.
	void releaseHeldIfAdopted() noexcept;
	void markOwnerDirty() noexcept {
		if (markDirty_ != nullptr) {
			markDirty_(dirtyHost_);
		}
	}

	struct NestedLevel {
		std::vector<DeviceInstanceId> prefix;
		uint8_t lane = 0;
		std::vector<device::HostSlotLevelDevice> devices;
		uint64_t adopted = 0;
	};
	/// ONE SLOT BOUND TO A CONTROL INSIDE A NESTED LEVEL: the level's prefix and the device's id there. The control
	/// is the pool target's key, which the level resolves itself.
	struct NestedBinding {
		device::SlotPath prefix;
		DeviceInstanceId device = kNoInstance;
		bool live = false;
		friend bool operator==(const NestedBinding&, const NestedBinding&) = default;
	};
	struct NestedGesture {
		device::SlotPath path;
		size_t control = 0;
		bool open = false;
	};
	std::vector<NestedLevel> levels_;
	/// Names containers gave their own controls (`reportCaptions`), by the container's path.
	struct Caption {
		std::vector<DeviceInstanceId> path;
		device::HostSlotCaption caption;
	};
	std::vector<Caption> captions_;
	/// Moves when a report changed what some level holds, or a level was forgotten — the gate of the reconcile
	/// and the binding rebuild, as the record's structure is.
	uint64_t levelsRevision_ = 0;
	uint64_t reconciledLevelsRevision_ = UINT64_MAX;
	uint64_t boundLevelsRevision_ = UINT64_MAX;
	/// Per slot, sized from `kSlotBindingCapacity` (heap: a path is not small).
	std::vector<NestedBinding> nestedBindings_ = std::vector<NestedBinding>(kSlotBindingCapacity);
	uint64_t nestedBindingGen_ = 1;
	std::vector<NestedGesture> nestedGestures_ = std::vector<NestedGesture>(kSlotBindingCapacity);
	/// THE REMAP WINDOW ACROSS LEVELS: a slot whose nested binding changed keeps its declaration off the audio
	/// thread (`held_`, the one it will get in `heldDesc_`) until every level has adopted `heldAt_`.
	std::vector<bool> held_ = std::vector<bool>(kSlotBindingCapacity, false);
	std::vector<const device::ParamDesc*> heldDesc_ = std::vector<const device::ParamDesc*>(kSlotBindingCapacity, nullptr);
	uint64_t heldAt_ = 0;
	void* dirtyHost_ = nullptr;
	void (*markDirty_)(void*) = nullptr;

	/// Steps 1-3 of `service`: everything but the owner's publish and the declarations after it.
	Visit serviceRecord() noexcept;
	/// Step 5 of `service`.
	void publishPendingDescs() noexcept;
	/// Rebuild the record's bindings from the pool; true if the record changed. Every slot whose binding
	/// changed has its declaration NULLED here, and every slot's wanted one is left in `pendingDescs_`.
	bool republishBindings() noexcept;
	/// The record's value of a control: its field, or its declared default past `fieldsPresent`.
	[[nodiscard]] float recordValueOf(DeviceInstanceId device, size_t control) const noexcept;

	/// **WHAT THE SLOTS DEPEND ON IN THE RECORD, AND NOTHING ELSE: which device (by id and declaration) is
	/// where, and the binding table** (T6b review). The reconcile and the binding rebuild are gated on THIS
	/// moving, not on `ChainRecord::revision`, which `setField` bumps. `orderRevision` would not do: it misses a
	/// replacement in place (another tag under the same id at the same position), which is exactly what the
	/// reconcile exists to catch. Compared EXACTLY, not hashed, so a collision cannot skip a reconcile.
	struct SlotStructure {
		struct Entry {
			DeviceInstanceId id = kNoInstance;
			const device::Descriptor* desc = nullptr;
			friend bool operator==(const Entry&, const Entry&) = default;
		};
		/// Room for every device either plugin's record holds today (Feathers: 12; the rack: its lanes).
		static constexpr size_t kDevices = 64;
		std::array<Entry, kDevices> devices {};
		size_t count = 0;
		/// More devices than `devices` holds: never equal to anything, so every visit reruns (correct, slow).
		bool overflow = false;
		std::array<SlotBinding, kSlotBindingCapacity> bindings {};
		bool valid = false; ///< false until first taken, so the first visit runs
		friend bool operator==(const SlotStructure& a, const SlotStructure& b) {
			return a.valid && b.valid && !a.overflow && !b.overflow && a.count == b.count && a.devices == b.devices
			       && a.bindings == b.bindings;
		}
	};
	[[nodiscard]] SlotStructure slotStructure() const noexcept;

	/// A Begin that went out, per slot, so its End goes to the same slot (`endGesture`); and a hand on a
	/// control, slot or no slot (`handGestures_`).
	struct OpenGesture {
		DeviceInstanceId device = kNoInstance;
		size_t control = 0;
		bool open = false;
	};
	[[nodiscard]] bool handOn(DeviceInstanceId device, size_t control) const noexcept;
	/// Begin on slot `n` for this control: any gesture still open on `n` is ended first (a control that died
	/// mid-drag, a remap), as is this control's gesture on any OTHER slot; and a Begin while the host records
	/// marks the slot touched-while-recording.
	void openSlotGesture(hostslots::SlotIndex n, DeviceInstanceId device, size_t control) noexcept;

	ChainRecord& record_;
	hostslots::SlotPool pool_;
	Bus bus_;
	Outbox outbox_;
	std::array<std::atomic<const device::ParamDesc*>, hostslots::kSlotBusCapacity> desc_ {};
	std::atomic<bool> hostRecording_ { false };
	/// THE BUS GENERATION EACH SLOT'S VALUE IS IN THE RECORD AT: a seed returns its own, so the mirror does
	/// not read a hand's write back as the host's.
	std::array<uint32_t, hostslots::kSlotBusCapacity> committedGen_ {};
	/// What `reconcile` last saw (`reconcileIfMoved`'s gate). Invalid/max, so the first visit reconciles.
	SlotStructure reconciledStructure_ {};
	uint64_t reconciledGeneration_ = UINT64_MAX;
	/// What the binding rebuild last saw (its gate, like `reconciledStructure_`).
	SlotStructure boundStructure_ {};
	uint64_t boundGeneration_ = UINT64_MAX;
	/// THE REMAP WINDOW (see `service`): the declarations a rebuild wants, handed to the audio thread only
	/// after the owner has published the record that binds them.
	std::array<const device::ParamDesc*, hostslots::kSlotBusCapacity> pendingDescs_ {};
	bool descsPending_ = false;
	/// The owner's last publish threw: the next visit publishes whether or not anything else changed.
	bool publishOwed_ = false;
	std::array<OpenGesture, hostslots::kSlotBusCapacity> gestureOpen_ {};
	/// **A HAND ON A CONTROL, SLOT OR NO SLOT** — so a control that GAINS a slot mid-drag (or whose slot is
	/// remapped mid-drag) opens a Begin on its new slot before its first Value there, rather than sending the
	/// host a Value inside no gesture. A few at once at most (one pointer, plus a multi-touch surface).
	std::array<OpenGesture, 8> handGestures_ {};
	/// **SLOTS A HAND'S TOUCH TOOK SINCE THE LAST REBIND** (T9): the rebind that binds one reports the takeover
	/// to the host (`touch` says what goes out). Cleared by that rebind, and by a load.
	std::array<bool, hostslots::kSlotBusCapacity> touched_ {};
	/// What `takeNotice` hands the tray next.
	std::string notice_;
	/// The takeover report for slot `n`, now bound to `binding` (steps 1-3 of `touch`'s list).
	void reportTakeover(size_t n, const SlotBinding& binding) noexcept;
	/// **A NESTED CONTROL'S TAKEOVER, OWED UNTIL ITS LEVEL ADOPTS THE BINDING** (backlog NG, 2026-10-17). A touch
	/// on a control inside a nested rack took its slot, the host heard its NAME at the rebind, and then nothing:
	/// `touched_` was cleared there with no report, because the control's value lives in the level's record and
	/// the bus is not seeded from it until the level adopts (`resolve` reads the default before that). So the
	/// first touch of a nested knob never reached the host as a touch — measured: the slot renamed, no event on
	/// it. Set at that rebind; paid in `releaseHeldIfAdopted`, when the bus holds the control's value.
	std::array<bool, hostslots::kSlotBusCapacity> nestedTakeoverOwed_ {};
	/// The takeover for a NESTED slot `n`, in `reportTakeover`'s shape: the value outside any gesture, then the
	/// edit as one gesture — unless a hand's own gesture is already open on the slot, which is telling the host.
	void reportNestedTakeover(size_t n) noexcept;
};

} // namespace feathers::rack
