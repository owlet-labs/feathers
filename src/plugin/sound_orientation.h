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
#include "dsp/slice_basis.h"

#include <array>

// WHICH PLANE EACH SOUND IS ON — the state of record for stage 3's first verb.
//
// Rotate is per sound, and the global pin was the wrong shape for it: one plane held by hand for
// EVERY sound is an audition handle, useful for hearing one orientation deliberately and useless for
// a verb that re-aims the plane around the sound you are holding. This is the per-sound channel the
// Explore corner turns.
//
// UNPINNED IS THE DEFAULT AND IT IS THE MIGRATION GUARANTEE. A sound whose entry is unpinned plays
// at rotate 0 — `rotateFor` in slice_build.h — which under M is the born-open plane its own home
// gives it, the same plane it played before the channel existed. So a patch carrying none of this
// builds precisely the character it built before, and that is structural rather than a promise:
// "unturned" and "born" are the same state.
//
// PRECEDENCE, and it is worth stating because two pins can exist at once: a sound's own pin wins over
// the global audition pin. The specific beats the general, which is what makes the audition handle
// still usable as a reference while a sound is being rotated — and A12 in the assumption log records
// it as a choice rather than an accident.

namespace feathers {

struct SoundOrientations {
	/// Per slot. `pinned == false` — the default — means this sound derives its plane from its home,
	/// which is the engine as it has always been.
	std::array<SliceOrientationPin, kMaxSounds> pin{};

	friend bool operator==(const SoundOrientations&, const SoundOrientations&) = default;
};

/// One sound's plane turned further, cumulatively — the shape driftBy has, for the same reasons.
///
/// A sound that was deriving becomes PINNED at the plane it was already on plus the turn, which is
/// what makes the first nudge of an encoder continuous rather than a jump: `from` is the plane the
/// sound is currently playing, whether that came from a pin or from its home, and the caller passes
/// it in rather than this guessing.
inline SoundOrientations rotateSoundBy(const SoundOrientations& current, size_t slot,
                                       const SliceGenerator& from, const SliceRotation& delta) {
	SoundOrientations turned = current;
	if (slot < kMaxSounds) {
		turned.pin[slot].pinned = true;
		turned.pin[slot].generator = rotateBy(from, delta);
	}
	return turned;
}

/// The plane a sound plays on, given both channels. The sound's own pin first, then the global
/// audition pin, then the derivation — most specific to least.
inline SliceOrientationPin resolvedOrientation(const SoundOrientations& perSound, size_t slot,
                                               const SliceOrientationPin& global) {
	if (slot < kMaxSounds && perSound.pin[slot].pinned) {
		return perSound.pin[slot];
	}
	return global;
}

} // namespace feathers
