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

#include "plugin/gui_bridge.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/sound_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>

// WHAT ONE OF FEATHERS' OWN EDITS IS — a sound slot's whole state, and the record that puts it back.
//
// SPLIT OUT OF edit_history.h, WHICH IS NOW THE CARD FLOOR'S. The undo STACK is shared by anything
// with widgets; these RECORDS are about sounds, stances, placements and hues, and a docked device has
// none of those.
//
// EVERY READER OF THESE TYPES IS A UI FILE — checked with a word-boundary search rather than a
// substring one, because a first attempt at this split matched `RideAlong` inside `RideAlongValues`
// (a different type, in `plugin/sound_variations.h`) and concluded the audio-side bridge depended on
// this header. It does not. Acting on that false hit is what created the include cycle that then
// looked like evidence for it.

namespace feathers::ui {


/// THE RIDE-ALONG FAMILY — V18: 'ride along' sound params (now including width, later more) follow the
/// clone and the stance, with one nesting: stance is itself a sound param, copied under clone just like
/// width.
///
/// So the family is one object with two kinds of member. The VALUE members (RideAlongValues —
/// width today) are the live slot's own settings of the sound's character; the stance LIST is its
/// one list-shaped member, travelling whole under clone, delete and undo exactly as it already
/// did, with each stored stance carrying its own copy of the value members. A clone copies the
/// whole family from its SOURCE — V18 overturning V14a's clone half — while `place` stays outside
/// it: place answers where-the-sound-stands and keeps the spent slot's rule (A34 — clones do not
/// stack). The defaulted comparison is the family's pin: the clone and round-trip cases compare
/// ONE of these, so a member added to RideAlongValues joins every claim automatically.
struct RideAlong {
	/// The live slot's WIDTH — the one family value member that is not already carried by
	/// SlotState.identity. Since V20 the family's value set is the explore box (zone, offset, the
	/// gamma base, drift, the plane with its residue, width), and every member except width IS the
	/// identity minus the vernier — so the live copies travel in `identity`, and duplicating them
	/// here would be two copies of one live value waiting to disagree. The stored stances carry the
	/// full set each (sound_variations.h).
	double width = 0.0;
	/// The remembered stances, each a full RideAlongValues record. See sound_variations.h.
	SoundVariations::List stances;

	friend bool operator==(const RideAlong&, const RideAlong&) = default;
};

/// Everything one slot is, for the purposes of putting it back.
///
/// The identity is what a sound SOUNDS like (sound_identity.h) and `enabled` is whether it is in the
/// patch at all. They travel together because a clone moves both in one act, and an undo that
/// restored the identity while leaving the slot switched on would put back half of what it took — the
/// same half-a-sound failure the identity struct itself has already had twice.
struct SlotState {
	SoundIdentity identity;
	bool enabled = true;
	/// THE RIDE-ALONG FAMILY — see above. What follows the SOUND: copied whole by a clone from its
	/// source, restored whole by an undo, reset whole by a delete.
	RideAlong rideAlong;
	/// AND WHERE IT STANDS. Not part of the IDENTITY and not a ride-along — A34 is unchanged, a
	/// clone still leaves where a sound is played from to the slot it lands in — but it is part of
	/// what a SLOT is, which is what this struct is for. The new-sound verb proved the difference:
	/// it places its sound in the emptiest part of the circle, and an undo that put back the
	/// identity and left the placement behind left the slot looking used, so the rail went on
	/// drawing a sound nobody had made. Restored like every other field, skipped when it has not
	/// moved.
	AnchorPoint place;
	/// AND THE COLOUR IT IS CALLED BY — a NAME rather than a sound, which is why it is here and not in
	/// SoundIdentity (sound_hues.h) and not in the RideAlong family either: a ride-along is a setting
	/// of the sound's character (V18) and a clone copies the family from its source, while a clone
	/// deliberately does NOT copy the hue — see clone(). It is in this struct because this struct is
	/// what the verbs that MOVE a sound between slots read and write whole: a reorder that carried
	/// the sound and left its colour behind would repaint the thing it moved, which is the bug the
	/// field exists to fix. Undo restores it like every other field.
	uint8_t hue = 0;
	/// AND ITS KNOBS (`perSoundKnobParam`): Smoothness, FM Depth, the field modifiers, Warp, Shape Amount, the bias knobs.
	/// The sound's settings, so they move with it — a clone copies them, a reorder carries them, a delete resets them,
	/// an undo restores them (per-sound settings follow a cloned sound). Until this member existed a
	/// reorder left them in the slot, on whichever sound moved in.
	std::array<double, kPerSoundKnobCount> knobs {};
	/// AND ITS DRAWN SPECTRAL CURVE (stored state, per sound since 2026-10-10), on the knobs' rules.
	spectral::DrawnCurve curve {};

	friend bool operator==(const SlotState&, const SlotState&) = default;
};

/// A slot's knobs as the parameter table declares them — what an untouched (or deleted) sound has.
std::array<double, kPerSoundKnobCount> untouchedKnobs(size_t slot);

/// Read a slot whole — identity and switch together.
SlotState readSlotState(const ParamBridge& bridge, size_t slot);
/// Write one back. Values that already match are not written, so restoring a state a slot already has
/// spends no gesture and marks nothing dirty — the rule writeIdentity already follows.
void writeSlotState(ParamBridge& bridge, size_t slot, const SlotState& state);

/// A record over one slot's whole state, which is what every verb this editor has today produces.
class SlotEdit : public EditRecord {
public:
	SlotEdit(ParamBridge& bridge, size_t slot, SlotState before, SlotState after, const char* verb)
	    : bridge_(bridge), slot_(slot), before_(std::move(before)), after_(std::move(after)),
	      verb_(verb) {}

	void undo() const override { writeSlotState(bridge_, slot_, before_); }
	void redo() const override { writeSlotState(bridge_, slot_, after_); }
	const char* verb() const override { return verb_; }
	std::optional<size_t> slot() const override { return slot_; }

private:
	/// A REFERENCE TO THE BRIDGE AND NOTHING ELSE OF THE EDITOR. The bridge outlives every widget and
	/// the history alike; a widget would not.
	ParamBridge& bridge_;
	size_t slot_ = 0;
	SlotState before_;
	SlotState after_;
	const char* verb_ = "Edit";
};

/// What a surface wants to do after the history has moved: point itself at the sound that changed.

/// RECORD ONE SLOT EDIT — was `EditHistory::recordSlot`, now a free function.
///
/// IT MOVED BECAUSE IT WAS THE ONE METHOD ON THE STACK THAT KNEW WHAT AN EDIT WAS. Everything else
/// `EditHistory` does is about ordering and undoing; this builds a `SlotEdit`, which needs the bridge.
/// A free function over the public `record()` does the same thing and leaves the stack ignorant.
void recordSlot(EditHistory& history, ParamBridge& bridge, size_t slot, const SlotState& before,
                const SlotState& after, const char* verb);

} // namespace feathers::ui
