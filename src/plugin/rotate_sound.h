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

#include "dsp/slice_build.h"
#include "plugin/sound_orientation.h"
#include "plugin/sound_residue.h"

// TURNING A SOUND'S PLANE WITHOUT HAVING TO KNOW WHICH PLANE IT IS ON — and without moving the sound
// that is standing on it.
//
// Its own header because of what it needs: deriving a plane from a home means the character layer's
// builders, and the parameter bridge must not drag those in — the bridge is deliberately buildable
// without the firmware headers. A control includes this; the road does not.

namespace feathers {

/// A TURN IS TWO WRITES, so the verb returns both and a caller cannot store one without the other.
///
/// Before the decision a rotation was one write — the new plane — and that is exactly the shape that
/// let the standing sound move: the plane changed and nothing recorded what the change would cost the
/// sound already out in it. Returning the pair makes the second write as hard to forget as the first.
struct RotatedSound {
	SoundOrientations planes;
	SoundResidue residue;
};

/// The turn a surface should call: `from` DERIVED rather than supplied, and the residue folded.
///
/// The three-argument `rotateSoundBy` takes the plane to turn FROM, which is correct and dangerous: a
/// caller that has not worked out what plane the sound is currently on passes a default-constructed
/// generator, and a default-constructed generator is the DEGENERATE plane — aperture zero, the line the
/// engine was before any of this. The sound then jumps to that plane on the first click of an encoder,
/// silently, and no test can catch it because the call is perfectly well-formed.
///
/// So this one works it out: the sound's own pin if it has one, the global audition pin if not, and
/// otherwise the plane its home gives it — which is exactly what the engine will do with the result.
///
/// `drift` is where this sound is standing, and it is required rather than defaulted because it is the
/// whole decision: a turn banked against the wrong drift preserves the wrong point, and a turn banked
/// against no drift at all is the A10 behaviour that was rejected. See A26 in the assumption log for why it
/// is the stored drift rather than the puck's Weather-scaled travel.
///
/// `gammaBase` is REQUIRED, and for the same reason `drift` is. A sound carried along its line is
/// somewhere else, so the plane its home gives it is a different plane — and this function does not
/// merely read that plane, it BANKS THE RESIDUE against it. Derive from the un-carried home and the
/// fold preserves a point the sound is not standing on, then writes that mistake into the patch.
/// Defaulting it would make forgetting it compile silently, which is exactly how three arguments of
/// this shape have already gone missing in this feature.
inline RotatedSound rotateSoundBy(const SoundOrientations& planes, const SoundResidue& residue,
                                  size_t slot, uint16_t zone, float phaseOffset,
                                  const SliceOrientationPin& global, const SliceRotation& delta,
                                  AnchorPoint drift, double gammaBase) {
	const SliceOrientationPin live = slot < kMaxSounds && planes.pin[slot].pinned ? planes.pin[slot]
	                                                                             : global;
	// THE PLANE THIS SOUND IS CURRENTLY ON, which under M is one number: the turn it carries, or none.
	// An unpinned sound turns FROM ZERO, and that is the born-open decision rather than a shortcut —
	// rotate 0 is the open plane its own home gives it, so the first click of the encoder moves it from
	// where it already was, and the corner counts the hand's own turn instead of an offset from a
	// number nobody chose.
	//
	// THE OLD HAZARD HERE DIED WITH THE APERTURE, and its warning deserves an answer rather than a
	// deletion: a default-constructed generator used to BE the degenerate plane, so a caller who passed
	// one sent the sound to a closed plane on the first click, silently and well-formedly. Under M a
	// default-constructed generator is rotate 0, which is exactly what an unturned sound already plays
	// on — the dangerous value and the correct value are now the same value.
	SliceGenerator from;
	if (live.pinned) {
		from = live.generator;
	}
	// Derived through the same pure function the planes below are turned by, so the fold banks the
	// difference between the plane that WAS playing and the one that will be — not between two
	// spellings of them.
	const SliceGenerator to = rotateBy(from, delta);
	// WHERE the turn happened. M reads home as well as the turn, so the fold has to bank against the
	// planes at this sound's own place — the same home the engine builds its character at, base
	// included. Both planes take the same one: a rotation moves the turn, never the place.
	const double home = sliceHome(zone, phaseOffset, gammaBase);
	return { rotateSoundBy(planes, slot, from, delta),
		     foldSoundResidue(residue, slot, from, to, drift, home) };
}

} // namespace feathers
