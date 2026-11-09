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

// WHAT EACH SOUND'S ROTATIONS BANKED — the third channel of stage 3's state of record, beside the
// drift and the planes.
//
// DECISION, 2026-08-13, superseding A10: rotate must not change the sound when sitting on a single
// point. A10 held that a drifted sound legitimately moves when its plane is re-aimed,
// because it is out in the plane rather than at its home. That is true of the arithmetic and wrong as
// an instrument: on the 2-D chart every drag that is not exactly vertical commits drift, so under A10
// rotate preserved the standing sound almost never — the one thing a re-aiming verb must never do.
//
// So a rotation banks the difference of contributions it would otherwise have made, and the decode
// adds it back. See foldResidue in dsp/slice_basis.h for the arithmetic and for what it can promise.
//
// PER SLOT, NOT PER PACKED POSITION — A7, the same rule the drift follows and for the same reason: a
// sound switched off changes which slots are live, and a residue indexed by position would become
// somebody else's the moment it happened.
//
// EMPTY IS THE MIGRATION GUARANTEE and it is structural: every offset defaults to exactly 0.0, the
// decode adds it inside the parenthesis the travel products already sat in, and `0.0 + term` is `term`
// for every finite term. A patch that has never rotated a drifted sound builds the character it always
// built, to the bit.
//
// EVERY ENGINE IS FOLDED, whichever one is playing — see PlaneResidue. The oscillator type is a
// parameter a player can change after the fact, and a morph patch rotated at a drift then switched to
// stair would otherwise find its stair character had never heard of the turns. Vox joined the fold in
// V-C2 under that same argument, before it had a voice to be heard through: an engine whose rows are
// folded only from the day it makes sound is an engine every older patch reaches by a different road.

namespace feathers {

struct SoundResidue {
	/// Per slot. All zeros — the default — is the engine as it was before the fold existed.
	std::array<PlaneResidue, kMaxSounds> banked {};

	/// Whether anything has ever been banked. What the state block asks before deciding to write
	/// itself: five kilobytes of zeros in every patch is five kilobytes of nothing, and a patch that
	/// has never rotated a drifted sound should stay byte-identical to the one it was before this
	/// landed.
	bool anyBanked() const noexcept {
		// THROUGH THE FLAT ORDER, so every part is asked — present and future — without a loop per engine
		// that the next engine has to remember to add.
		for (const PlaneResidue& sound : banked) {
			for (size_t i = 0; i < kPlaneResidueValues; ++i) {
				if (planeResidueValue(sound, i) != 0.0) {
					return true;
				}
			}
		}
		return false;
	}

	friend bool operator==(const SoundResidue&, const SoundResidue&) = default;
};

/// One sound's residue after a turn from `before` to `after`, standing at `drift`.
///
/// PURE, AND ON THE STATE OF RECORD, the shape driftBy and rotateBy have and for the same reason: a
/// rewind is a stored number restored rather than an increment subtracted back off. The residue is
/// identity now — a breadcrumb that carried the plane and not this would land on the right orientation
/// with the wrong sound standing on it.
///
/// A slot past the end is ignored rather than clamped: there is no sound there to have turned, and
/// clamping would bank somebody else's difference.
/// `home` is WHERE THE TURN HAPPENED, and it is required for the same reason the drift is: M is a
/// function of the turn and the place, so a fold given the wrong home banks the difference between two
/// planes neither of which was ever playing. Both planes take the same one — a rotation moves the
/// turn, never the place.
inline SoundResidue foldSoundResidue(const SoundResidue& current, size_t slot,
                                     const SliceGenerator& before, const SliceGenerator& after,
                                     AnchorPoint drift, double home) {
	SoundResidue folded = current;
	if (slot < kMaxSounds) {
		const double from = sliceRotateOf(before);
		const double to = sliceRotateOf(after);
		folded.banked[slot].morph = foldResidue<kMorphTriangles>(current.banked[slot].morph, from, to,
		                                                         drift.x, drift.y, home);
		folded.banked[slot].stair = foldResidue<kStairTriangles>(current.banked[slot].stair, from, to,
		                                                         drift.x, drift.y, home);
		folded.banked[slot].vox = foldResidue<kVoxTriangles>(current.banked[slot].vox, from, to, drift.x,
		                                                     drift.y, home);
		folded.banked[slot].weave = foldResidue<kWeaveTriangles>(current.banked[slot].weave, from, to,
		                                                         drift.x, drift.y, home);
		folded.banked[slot].partials = foldResidue<kPartialTriangles>(current.banked[slot].partials, from,
		                                                              to, drift.x, drift.y, home);
	}
	return folded;
}

} // namespace feathers
