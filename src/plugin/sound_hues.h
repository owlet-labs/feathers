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

#include "plugin/params.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// WHICH COLOUR EACH SOUND WEARS.
//
// ON THE SHIPPED RAIL, reordering should retain the original colour. The colour used to be a
// function of the SLOT — `kSeedHues[slot]`, computed at draw time and stored nowhere — so a splice
// repainted whatever it moved. The mock has never worked that way: its `hueOf(i)` reads a hue held on
// the SOUND, and its reorder splices the sounds, so colour travels with them. The palette's own
// comment says why (2026-08-12): a sound's colour has to name the sound. A
// name that changes when a row moves is not a name.
//
// AN INDEX, NOT A COLOUR, and that is the whole of the storage decision. What is stored is which of
// the eight palette entries a sound wears; what that entry LOOKS like stays in the theme, where a
// palette change still reaches every surface at once. A patch therefore records eight small numbers
// and no colour values, and re-theming the instrument does not have to migrate anybody's patches.
//
// IT IS NOT PART OF THE IDENTITY, and this is the line the whole two-level model is drawn along. What
// a sound SOUNDS like is `SoundIdentity`; what it is CALLED is this. So it does not travel with a
// clone — a copy is an independent sound and gets a name of its own — and it is not something a hand
// authors, which is why `isEmpty` must never consult it: a slot whose colour was permuted by a
// reorder is not a slot somebody has used.
//
// NOR IS IT DERIVED. It was worth asking, since derived state needs no block: every candidate that
// travels with a sound is authored and mutable — zone, offset, gamma, drift, plane, residue are all
// scrubbed by Explore — so a derived hue would change colour WHILE THE SOUND IS BEING EDITED, which
// is worse than the bug it would fix and the exact opposite of naming.

namespace feathers {

/// The palette entry each slot's sound wears, by index.
struct SoundHues {
	/// Defaults to the identity — slot 0 wears the first colour, slot 1 the second — because that is
	/// exactly what the rail drew before any of this existed. A patch with no hues block loads this
	/// and renders identically to the day it was saved, which is what makes the block safe to add.
	std::array<uint8_t, kMaxSounds> index { 0, 1, 2, 3, 4, 5, 6, 7 };

	friend bool operator==(const SoundHues&, const SoundHues&) = default;
};

static_assert(SoundHues {}.index.size() == kMaxSounds, "every sound needs a colour of its own");

/// What slot `n` wears, as a palette index that is always in range.
///
/// FOLDED RATHER THAN TRUSTED. The value arrives from a patch file, and a byte out of range would
/// otherwise index a palette off its end — the one way a colour can become a crash.
inline size_t hueIndexOf(const SoundHues& hues, size_t slot) noexcept {
	return slot < kMaxSounds ? static_cast<size_t>(hues.index[slot]) % kMaxSounds : 0;
}

/// The colour to give a sound that is about to exist: the lowest palette index none of the sounds in
/// `occupied` is wearing.
///
/// LOWEST-UNUSED RATHER THAN A COUNTER, which is where this parts company with the mock. `forkSound`
/// pushes `hue: S.sounds.length`, and that collides the moment a delete has left a gap — remove the
/// second of three sounds, add one, and the newcomer wears a colour already on screen. The mock only
/// survives it because the read modulos. A list of eight slots with a delete verb has gaps as a matter
/// of course, so the rule has to be one that cannot collide: eight sounds, eight colours, and the
/// lowest one nobody is wearing is always available until all eight are.
///
/// `occupied` is the slots that hold a sound — empty slots are skipped, because the colour of a slot
/// with nothing in it is not worn by anybody and is free to give away.
inline uint8_t lowestUnusedHue(const SoundHues& hues, std::span<const size_t> occupied) noexcept {
	std::array<bool, kMaxSounds> worn {};
	for (const size_t slot : occupied) {
		worn[hueIndexOf(hues, slot)] = true;
	}
	for (size_t at = 0; at < kMaxSounds; ++at) {
		if (!worn[at]) {
			return static_cast<uint8_t>(at);
		}
	}
	// Every colour is worn, which means every slot is full and nothing is about to be made. Answering
	// the first rather than failing: the callers all check for room before they get here, and a
	// silent zero is a better shape than an exception nobody can act on.
	return 0;
}

} // namespace feathers
