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

#include "dsp/constellation.h"

#include <array>
#include <cstddef>
#include <cstdint>

// HOW MUCH OF EACH SOUND YOU ARE HEARING, BY SLOT — the activation weights, read back out.
//
// The engine has always computed these: the blend is `Σ w·sound`, and `w` is where the puck stands
// relative to every anchor. What was missing is a way to LOOK at them. The mandate is modulator
// outs that follow the activation blend weights, and the first thing such an out needs is the number
// itself, in a form addressed the way a player addresses a sound — by its slot on the rail.
//
// DERIVED, NEVER SAVED. There is no state block here and there never will be one: every one of these
// numbers is a function of the anchors, the puck and which sounds are switched on, all of which the
// patch already holds. Storing them would be storing an answer beside its question and inviting the
// two to disagree.
//
// BY SLOT, WHICH THE CHARACTER IS NOT. The character packs its live sounds — a switched-off slot takes
// no place at all — so `c.sounds[2]` is the third LIVE sound rather than the sound on rail row 2. A
// readout indexed that way would relabel every meter the moment a sound was switched off. This is the
// unpacked form, and the packing is undone exactly once, where the packing itself is done.
//
// ABSENT IS NOT ZERO, and the distinction is the whole reason `live` exists beside the values. A sound
// at weight 0.0 is a sound you are standing infinitely far from — it is in the patch, on the tour,
// and turning towards it would bring it in. A sound that is switched off is not in the blend at all,
// and a meter that drew the two the same way would be lying about what the instrument can be played
// into. Same rule the character's own packing follows, which is where this reads it from.

namespace feathers {

/// Eight sounds, eight bits.
static_assert(kMaxSounds <= 8, "the live mask is a byte");

struct SoundWeights {
	/// Per slot, normalised across the LIVE sounds so the live entries sum to 1. A slot that is not
	/// live holds exactly 0.0 — see `live` for why that is not the same as a live sound at zero.
	std::array<double, kMaxSounds> weight {};

	/// Which slots are in the blend at all, one bit per slot. Empty when there is nothing to hear:
	/// no sound switched on, which is a patch making silence.
	uint8_t live { 0 };

	bool isLive(size_t slot) const {
		return slot < kMaxSounds && (live & (uint8_t { 1 } << slot)) != 0;
	}

	/// This slot's share, or 0.0 for a slot that is not in the blend. The value a MODULATION source
	/// reads, where absent and silent are genuinely the same thing: a source that is not there pushes
	/// its destination nowhere. A meter asks `isLive` first, because a meter is drawing what the
	/// instrument IS rather than what it is currently sending.
	double at(size_t slot) const { return slot < kMaxSounds ? weight[slot] : 0.0; }

	void set(size_t slot, double share) {
		if (slot >= kMaxSounds) {
			return;
		}
		weight[slot] = share;
		live = static_cast<uint8_t>(live | (uint8_t { 1 } << slot));
	}

	friend bool operator==(const SoundWeights&, const SoundWeights&) = default;
};

} // namespace feathers
