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
#include "hostslots/slot_bus.h"
#include "hostslots/slot_pool.h"
#include "hostslots/text_site.h"

#include <clap/clap.h>

#include <cstdint>

// THE HOST SLOTS AS CLAP SEES THEM (backlog JU, T6a) — the glue
// both plugins share, and the one file under `src/hostslots/` that may include CLAP.
//
// THE READ SIDE, AND THE ROAD OUT. It answers `params.get_info`, `get_value`, `value_to_text` and
// `text_to_value` for the 64 slot ids, and says when the host is owed a rescan. It holds no state of its own
// beyond "what did the host last hear": the pool says what each slot names, and a `ControlLookup` the owning
// plugin supplies says what that control is and where it stands. The outbox's drain to the host
// (`drainOutbox`, T10 — shared from Feathers' T6b drain so the rack's cannot differ) is here too; host
// events IN are the owner's (`rack::SlotOwner::onHostValue`, and `ParamBridge`'s bus).
//
// **SLOT IDS LIVE AT 0x1000 + n, OUTSIDE EVERY CORE ID** (`kParamIdSpan` in `plugin/params.h` is far below
// it). So every call site in a plugin asks `isSlotId` BEFORE its own `findParam`: a slot id handed to the
// core table is simply "no such parameter", which would hide a slot from the host rather than crash, and so
// would be the quiet kind of wrong.

namespace feathers::hostslots {

/// **ONE LIMIT, TWO HEADERS.** `slot_bus.h` cannot include `slot_pool.h` (T2's rule), so its capacity is a
/// twin; this is the first file to see both, and the one that refuses to build if they part.
static_assert(kHostSlotCapacity == kSlotBusCapacity,
              "the pool and the bus must agree about how many slots there are");

/// The first slot's CLAP id. A slot's id is `kFirstSlotId + n`, fixed for ever: a host lane is keyed by it.
inline constexpr clap_id kFirstSlotId = 0x1000;

/// How many slot params a plugin DECLARES. The count is fixed while active (plan §0.1), so it is the
/// pool's starting capacity, not whatever a pool has grown to; growth is an append of ids at a rebuild.
inline constexpr uint32_t kDeclaredSlots = kHostSlotCapacity;

[[nodiscard]] constexpr bool isSlotId(clap_id id) noexcept {
	return id >= kFirstSlotId && id < kFirstSlotId + kDeclaredSlots;
}
[[nodiscard]] constexpr clap_id slotId(SlotIndex n) noexcept { return kFirstSlotId + n; }
[[nodiscard]] constexpr SlotIndex slotOfId(clap_id id) noexcept {
	return isSlotId(id) ? static_cast<SlotIndex>(id - kFirstSlotId) : kNoSlot;
}

/// **WHAT A SLOT'S TARGET IS AND WHERE IT STANDS, asked of the plugin that owns the devices.** The pool knows
/// a target by path and key; only the owner can turn that into a declaration and a current value. False when
/// the target does not resolve (its device is gone, or its key is not one the device declares).
class ControlLookup {
public:
	virtual ~ControlLookup() = default;
	/// On success `*desc` is the control's declaration and `*value` its value in device units.
	virtual bool lookup(const Target& target, const device::ParamDesc** desc, double* value) const = 0;
	/// **WHERE THE TARGET'S DEVICE PRINTS ITS OWN WORDS** (`TextSite`), or false — then the slot prints in the
	/// declared unit, as every slot did before rack stage 2. Defaulted, so an owner with no instances to offer
	/// says nothing.
	virtual bool textSite(const Target& target, TextSite* site) const {
		(void)target;
		(void)site;
		return false;
	}
};

/// The module every slot is listed under.
inline constexpr const char* kSlotModule = "Slots";

/// **ONE OUTBOX EVENT, AS THE HOST HEARS IT** — `CLAP_EVENT_PARAM_GESTURE_BEGIN` / `_END`, or a global
/// `CLAP_EVENT_PARAM_VALUE` of `value01`, on slot `slot`'s id. True iff the host now has it (`try_push`);
/// false for a null queue. No allocation.
bool emitSlotEvent(const clap_output_events* out, SlotEventKind kind, uint16_t slot, float value01) noexcept;
/// The same event on any parameter `id` — the rack's macros (backlog NG), which ride a `SlotOutbox` of their own
/// and are not slots. `emitSlotEvent` is this on `slotId(slot)`.
bool emitParamEvent(const clap_output_events* out, SlotEventKind kind, clap_id id, float value01) noexcept;

/// **THE SLOTS' OUTBOX TO THE HOST** (plan §1.2) — `SlotOutbox::drain` through `emitSlotEvent`, so its debt
/// rules hold (an owed End before a new Begin, a Value re-read fresh, a Begin never retried). Both plugins
/// call it from `process` AND `params.flush` — while a plugin sleeps, flush is the only road out (plan
/// §0.13). The drain's own contract applies: one consumer at a time, which CLAP guarantees for those two.
void drainOutbox(SlotOutbox<>& outbox, const clap_output_events* out) noexcept;

class SlotHost {
public:
	SlotHost(SlotPool& pool, const ControlLookup& lookup) noexcept : pool_(pool), lookup_(lookup) {}

	/// `params.get_info` for slot `n` (0-based, NOT a param index). An Empty slot, or one whose target does
	/// not resolve, is "Slot N" (1-based) and HIDDEN — which only CLAP honours; the VST3 wrapper never
	/// re-reads the flag (design §1), so there it is a visible "Slot N". Otherwise the target's label.
	/// Always 0..1, default 0, not stepped, automatable and modulatable: none of that may change while active.
	bool info(SlotIndex n, clap_param_info* info) const noexcept;

	/// The slot's value as a 0..1 position along its target's declared range (`device::toNormal`); 0 for an
	/// Empty or unresolved slot. False for an id that is not a slot.
	bool value(clap_id id, double* value) const noexcept;

	/// `value` (0..1) as the TARGET's own text — "1.2 kHz", "Saw" — through `device::formatValue` on the
	/// device units `fromNormal` gives. An Empty slot prints the bare position. False for a non-slot id.
	/// **THE DEVICE IS ASKED FIRST** (rack stage 2, §3): a device whose `TextOps` both prints AND reads back
	/// (`format` and `parse`) prints by its own state — the Rack's position as `Chain 2` — where the owner can
	/// offer the instance (`ControlLookup::textSite`); a hook that declines falls through to the unit.
	bool valueToText(clap_id id, double value, char* display, uint32_t size) const noexcept;

	/// The DAW's type-in: `display` read in the TARGET's unit (`device::parseValue`, `valueToText`'s inverse),
	/// returned as a 0..1 position. False for a non-slot id, an Empty or unresolved slot, or text that is no value.
	/// The device's own `parse` is asked first, on the same terms as `valueToText`'s `format`.
	bool textToValue(clap_id id, const char* display, double* value) const noexcept;

	/// **WHAT THE HOST IS OWED SINCE THE LAST ASK: `RESCAN_VALUES | RESCAN_INFO | RESCAN_TEXT` when any
	/// slot was renamed (a new target, a free, a restore), else 0.** It TAKES the pool's renamed set, so a run
	/// of takeovers between two asks is one rescan, not one each — the caller asks once per main-thread visit.
	/// Main thread.
	clap_param_rescan_flags rescanOwed();

private:
	/// The target's declaration and value, or false for an Empty or unresolved slot.
	bool resolve(SlotIndex n, const device::ParamDesc** desc, double* value) const noexcept;

	SlotPool& pool_;
	const ControlLookup& lookup_;
};

} // namespace feathers::hostslots
