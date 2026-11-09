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

#include "device/instance_id.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// THE HOST SLOT POOL (backlog JU, T1).
//
// A fixed set of pre-declared host params, mapped at runtime to device controls. This file is only the
// BOOKKEEPING: which slot names which control, in which state, and which slot a new control gets. It knows
// nothing of CLAP, of values, or of threads. It is main-thread only, and it allocates freely, because
// nothing here runs on the audio thread.
//
// THE RULES IT ENFORCES, each one a design decision:
// - A control has AT MOST ONE slot. A target is a PATH of instance ids plus a control KEY, never a position,
//   so reordering a chain is a non-event.
// - A slot is taken in this order: a clean Empty, then a clean untouched Default (the lowest priority first),
//   then the least recently used clean Recent — taken ONLY when there is nothing Empty or Default left, and
//   announced — and only then, as the last resort, a HOST-MARKED slot (below) in the same order: Empty,
//   Default, Recent by LRU, every one announced. A Pinned or Anchored slot is never taken. "Lowest priority" is the fill order run
//   backwards: the Default filled LAST, by (rank, level, chainOrder), is the first taken. (The plan wrote
//   "highest (level, rank)", which would give up a nested device's headline before an outer device's #4 and
//   so contradict the tiers the fill itself uses.)
// - Removing a device frees ALL its slots, anchored ones included.
//   A freed slot keeps a note of what it held, so undoing the removal gets the same slots back, as long as
//   nothing reused them in between.
// - The capacity grows and never shrinks. A host project holds lanes on slot ids, so an id once declared
//   must stay declared.
// - **AN ID THE HOST HAS TOUCHED IS REUSED LAST**. A host keeps its automation lanes and
//   modulators on a slot's id after the slot is released, unpinned or freed, and a plugin cannot remove them;
//   whatever takes the id next is driven by them. So every slot carries a HOST MARK — set by any host write
//   or non-zero host modulation, kept through release, unpin and free, living on the slot INDEX and saved with
//   the pool — and a marked slot is taken only after every clean one, and announced.

namespace feathers::hostslots {

/// How many slots a pool starts with, and the least it can ever have. 256 since 2026-09-26 (a host keeps
/// its lanes and modulators on a slot id after the slot is released, so a bigger pool reuses an id later);
/// it was 64. A pool saved at 64 loads and grows (`restore`).
inline constexpr uint32_t kHostSlotCapacity = 256;

/// **THE MOST SLOTS A POOL MAY EVER HAVE — ONE LIMIT FOR THE POOL, ITS WRITER AND ITS READER.** `grow` refuses
/// past it and the constructor clamps to it, so no pool can hold a capacity its own saved document would be
/// refused for; `restore` refuses a snapshot past it, and the patch reader (`rack_state.cpp`) checks it
/// before anything is sized from a file. 4096 is room for a hand that exposes many controls, and not room
/// for a four-billion-slot allocation typed into a file.
inline constexpr uint32_t kMaxHostSlotCapacity = 4096;

using SlotIndex = uint32_t;

/// "No slot": what `slotOf` answers for an unslotted control and `touch` for a pool it could not take from.
inline constexpr SlotIndex kNoSlot = UINT32_MAX;

enum class SlotState : uint8_t {
	Empty,    ///< Nothing mapped. First to be taken.
	Default,  ///< Filled from a device's priority list. Taken lowest priority first, while untouched.
	Recent,   ///< Filled by a touch. Taken least recently used first, only when the pool is full.
	Pinned,   ///< "Expose to host". Only an explicit unpin frees it.
	Anchored, ///< The host wrote to it, or it was touched while recording. Only a release frees it.
};

/// What a slot names. `path` runs from the OUTERMOST device to the leaf that owns the control, and with `key`
/// it is the control's identity. `tag` (the device's type) and `label` (the qualified name the host shows,
/// e.g. "Filter · Cutoff") describe it and take no part in identity.
struct Target {
	std::vector<DeviceInstanceId> path;
	std::string key;
	std::string tag;
	std::string label;

	/// True when both name the same control: the same path and the same key.
	[[nodiscard]] bool sameControl(const Target& other) const {
		return path == other.path && key == other.key;
	}
	bool operator==(const Target&) const = default;
};

/// One control a device offers for the defaults. `level` is how deep its device sits (0 is outermost),
/// `rank` is its place in the device's priority list (0 is the headline), and `chainOrder` is the device's
/// position within its level.
struct Candidate {
	Target target;
	int32_t level = 0;
	int32_t rank = 0;
	int32_t chainOrder = 0;
};

/// What a `touch` or a `pin` did.
struct Allocation {
	enum class Took : uint8_t {
		Existing, ///< The control already had this slot. Nothing was allocated.
		Empty,    ///< An Empty slot.
		Default,  ///< An untouched Default slot, taken over. Nobody automated it, so it is silent.
		Recent,   ///< A used Recent slot, the last resort. `announce` is set.
		None,     ///< Nothing could be taken. `slot` is kNoSlot.
	};
	SlotIndex slot = kNoSlot;
	Took took = Took::None;
	/// What the slot held before a takeover (Default or Recent); empty otherwise.
	std::optional<Target> previous;
	/// Set exactly when a used Recent slot, or a host-marked slot, was taken. The tray says so (design §2).
	bool announce = false;
	/// The slot taken carries the HOST MARK: the host has written or modulated its id, and whatever lanes it
	/// kept there now drive this control. Implies `announce`; the tray says it in its own words.
	bool hostMarked = false;
};

/// One non-Empty slot as `snapshot` records it and `restore` reads it.
struct SlotRecord {
	SlotIndex index = 0;
	SlotState state = SlotState::Empty;
	Target target;
	uint64_t recency = 0;
	int32_t level = 0;
	int32_t rank = 0;
	int32_t chainOrder = 0;
	/// What `release` returns an Anchored slot to: Pinned if it was pinned when it was anchored, else Recent.
	SlotState beforeAnchor = SlotState::Recent;
	bool operator==(const SlotRecord&) const = default;
};

/// The whole pool, as the patch saves it. Only non-Empty slots are listed, in index order. Former-target
/// notes are not part of it: an undo does not outlive the session.
struct PoolSnapshot {
	uint32_t capacity = kHostSlotCapacity;
	uint64_t clock = 0;
	std::vector<SlotRecord> slots;
	/// **THE HOST-MARKED SLOT INDICES, ASCENDING** — Empty ones included, which is why they are not a field of
	/// `SlotRecord` (only non-Empty slots are listed there). Absent in a saved document means none.
	std::vector<SlotIndex> hostMarked;
	bool operator==(const PoolSnapshot&) const = default;
};

class SlotPool {
public:
	/// A pool of `capacity` slots, clamped to kHostSlotCapacity..kMaxHostSlotCapacity.
	explicit SlotPool(uint32_t capacity = kHostSlotCapacity);

	[[nodiscard]] uint32_t capacity() const { return static_cast<uint32_t>(slots_.size()); }
	/// Grows the pool to `capacity`. A smaller number changes nothing: the pool never shrinks. REFUSED (false,
	/// nothing changed) past kMaxHostSlotCapacity.
	bool grow(uint32_t capacity);

	/// The slot naming this control, or kNoSlot.
	[[nodiscard]] SlotIndex slotOf(const Target& target) const;

	[[nodiscard]] SlotState state(SlotIndex slot) const;
	/// What the slot names. An Empty slot's target is empty.
	[[nodiscard]] const Target& target(SlotIndex slot) const;
	/// The clock value of the slot's last touch; higher is more recent.
	[[nodiscard]] uint64_t recency(SlotIndex slot) const;

	/// A hand on this control. A control with a slot keeps it and moves to the front of the recent order.
	/// Otherwise a slot is taken in the order the header describes, and becomes Recent.
	///
	/// A touch on a slotted control changes only its place in the recent order, whatever its state. A DEFAULT
	/// slot stays Default: "untouched" means the host never wrote to it and no hand
	/// touched it WHILE RECORDING, and a plain touch is neither, so nothing the host holds can be on it and it
	/// is still taken over silently. What does protect it is `anchor`, which T6b calls for those two events.
	Allocation touch(const Target& target);

	/// "Expose to host": gives the control a slot as `touch` does, then makes it Pinned. An Anchored slot
	/// stays Anchored (the stronger state), and a later release returns it to Pinned.
	Allocation pin(const Target& target);
	/// Pinned → Recent. False when the slot is not Pinned.
	bool unpin(SlotIndex slot);

	/// The host wrote to this slot, or a hand touched it while recording. False for an Empty slot: an anchor
	/// protects a mapping, and an Empty slot has none.
	bool anchor(SlotIndex slot);
	/// Anchored → what it was before (Pinned, or Recent). False when the slot is not Anchored.
	bool release(SlotIndex slot);

	/// **THE HOST TOUCHED THIS SLOT'S ID** — a write or a non-zero modulation, bound or not. Any state, Empty
	/// included, and kept through `release`, `unpin`, `free`, `freeUnder` and a reuse: it is about the id, and
	/// the host's lanes stay on the id. Only `restore` replaces it. True on the transition (the saved pool
	/// changed; the generation bumps).
	bool markHost(SlotIndex slot);
	[[nodiscard]] bool hostMarked(SlotIndex slot) const;

	/// Fills EMPTY slots from the devices' priority lists and touches nothing else. Candidates go in tiers:
	/// every rank 0 before any rank 1, and within a rank, outer level first, then chain order. A candidate
	/// whose control already has a slot is skipped. Returns how many slots were filled.
	uint32_t fillDefaults(std::span<const Candidate> candidates);

	/// Frees every slot whose path starts with `prefix`, whatever its state, and keeps a note of what each
	/// held. An empty prefix frees nothing. Returns how many were freed.
	uint32_t freeUnder(std::span<const DeviceInstanceId> prefix);
	/// Frees ONE slot, whatever its state, and keeps a note of what it held exactly as `freeUnder` does, so a
	/// `restoreUnder` over its path can still bring it back. What a reconcile calls for a target that stopped
	/// resolving (a device replaced under its id, a key no longer declared). False for an Empty slot or an
	/// index past the capacity.
	bool free(SlotIndex slot);
	/// Undo of `freeUnder`: every still-Empty slot whose note lies under `prefix` gets its target and state
	/// back, unless that control has meanwhile got another slot. When one control has two notes (freed,
	/// slotted again, freed again), the LATER free is restored and the older note is spent. Returns how many
	/// were restored.
	uint32_t restoreUnder(std::span<const DeviceInstanceId> prefix);

	/// Names a target at its new path. The label is path-qualified ("Rack 2 · Delay · Mix"), so a move
	/// changes it, and only the caller knows the names of the devices along the path.
	using LabelFor = std::function<std::string(const Target&)>;

	/// A device moved between levels: `oldPrefix` is rewritten to `newPrefix` on every slot and every note
	/// under it, and each gets `labelFor(its rewritten target)` as its label. Ids, states and recency are
	/// kept. Returns how many slots (not notes) moved.
	///
	/// REFUSED WHOLE (nullopt, nothing changed) when a rewritten slot would name a control that another slot
	/// already names, or when either prefix is empty. Allowed through, that would leave two slots on one
	/// control, and `restore` refuses such a snapshot whole, so the next reload would lose every anchor.
	std::optional<uint32_t> rebase(std::span<const DeviceInstanceId> oldPrefix,
	                               std::span<const DeviceInstanceId> newPrefix, const LabelFor& labelFor);

	/// Bumps on every change to a mapping or a state. A touch that only moves a slot in the recent order
	/// does not bump it, because no binding changed.
	[[nodiscard]] uint64_t generation() const { return generation_; }

	/// The slots whose name may have changed since the last take (a new target, a free, a rebase, a
	/// restore), one flag per slot, sized to the capacity. Clears the set.
	std::vector<bool> takeRenamed();
	/// **EVERY SLOT AT OR UNDER `prefix` IS OWED NEW TEXT** — added to the renamed set and nothing else: no target,
	/// label, state or generation moves. For a container whose words for a value changed while the value did not
	/// (`HostSlotLink::retext`). Returns how many slots it marked; an empty prefix marks none.
	uint32_t retextUnder(std::span<const DeviceInstanceId> prefix);

	[[nodiscard]] PoolSnapshot snapshot() const;
	/// Replaces the pool with a snapshot. It is validated first (`whyInvalid`) and REFUSED WHOLE (false, pool
	/// unchanged) when that names a reason. A snapshot's capacity grows the pool; a smaller one does not
	/// shrink it. Every note is dropped.
	bool restore(const PoolSnapshot& snapshot);

	/// **WHY `restore` WOULD REFUSE THIS SNAPSHOT, IN WORDS; EMPTY WHEN IT WOULD NOT.** Refused: a capacity
	/// past kMaxHostSlotCapacity, an index at or past the capacity or repeated, a state that is not one of the
	/// five (or is Empty), a target with no path or no key or a kNoInstance anywhere on its path, two slots
	/// naming one control, a recency ahead of the clock, a clock of UINT64_MAX (the next touch would wrap it),
	/// or a `beforeAnchor` that is not Pinned or Recent; or a host mark past the capacity, out of ascending
	/// order, or listed twice. The patch reader says this reason in its load report.
	[[nodiscard]] static std::string whyInvalid(const PoolSnapshot& snapshot);

private:
	struct Slot {
		SlotRecord rec;
		/// What a freed slot held, for `restoreUnder`. Cleared when the slot is reused.
		std::optional<SlotRecord> note;
		/// When the note was written (`freeSerial_`): of two notes on one control, the later one is restored.
		uint64_t notedAt = 0;
		/// `markHost`. Outside `rec` on purpose: every road that resets `rec` keeps it.
		bool hostMarked = false;
	};

	[[nodiscard]] SlotIndex takeFor(Allocation& out);
	void markRenamed(SlotIndex slot);
	void bump() { ++generation_; }

	std::vector<Slot> slots_;
	std::vector<bool> renamed_;
	uint64_t clock_ = 0;
	uint64_t generation_ = 0;
	uint64_t freeSerial_ = 0;
};

} // namespace feathers::hostslots
