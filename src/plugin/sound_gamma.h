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

// HOW FAR ALONG ITS OWN LINE A SOUND HAS BEEN CARRIED — the unbounded half of gamma.
//
// WHY THIS EXISTS, and the measurement that decided it. The rank census (tools/rank-census.cpp)
// asked how much of the character space is reachable and found that ROTATION reaches 12 of morph's
// 70 coordinates, while the HOME direction alone already varies 11 independent ones. The pump built
// to widen exploration is narrower than the walk it was meant to widen. So the cheapest large gain
// available is not more angles — it is letting home go as far as it likes.
//
// THE VERNIER ARRANGEMENT. The per-sound `Gamma` parameter keeps its id, its ±4 range and its
// automation exactly as they are; it becomes the FINE control. This base is the COARSE one, carries
// no id, and is unbounded. A sound's home is the two added together. That split is what lets gamma
// be unbounded without spending a parameter id on an unbounded range, which no host would know how
// to draw and no automation lane could hold.
//
// NOT A PARAMETER, for the reason the drift offsets are not: it is permanent patch state, it must
// survive a save, and it is moved by a gesture rather than automated. It rides a tagged trailing
// block after the ones already there, which is the A9 precedent — a block that is absent reads as
// its default, and the default is the zero that reproduces the old sound.
//
// DOUBLE, AND THAT IS LOAD-BEARING. `SoundCoord::phaseOffset` is a float, and a base folded into it
// would quantise: at a base of 1e5 a float ULP is about eight zones, so the vernier's whole range
// would vanish exactly where "unbounded" starts to mean something. Kept as a double and added into
// `home` after the float, the road stays finer than a cent out to a base of 1e10 and finer than a
// zone out to 1e12 — measured, not assumed.
//
// WHAT THE SPLIT COSTS, measured over a sweep rather than guessed from fixtures. Moving a quantity
// from the vernier into the base is NOT bit-identical: the vernier is rounded into a float and the
// base is not, so the two arrangements differ in home, and the builders can see it. Over 40,000
// random cases (zone, offset, vernier ±4, base ±1e5) the built character differs by at most
// 1.84e-04 — about six 16-bit LSBs.
//
// Small, but not the "third of an LSB" that seven hand-picked fixtures first suggested; that figure
// was eighteen times optimistic and is corrected here rather than quietly kept. The honest framing
// is RELATIVE: the split costs ~2e-04 while the vernier it is moving is worth ~1e-01, three orders
// larger. tests/test_character_blend.cpp holds the bound at 5e-04 with a known-bad that misses by
// the whole vernier.
//
// The alternative — widening phaseOffset to double — buys an exact split and costs the SAME ORDER of
// error in a worse place: every patch that already exists would move, by the same float-rounding
// mechanism and the same magnitude. Decided against on that asymmetry (2026-08-13): one identical
// error budget, and this way it is spent on a feature nobody has yet rather than on a promise
// already made to every patch that is saved.

namespace feathers {

struct SoundGamma {
	/// Per slot, in the same units the Gamma parameter is: whole steps along the sound's own line,
	/// where ±4 is the vernier's entire range. Unbounded here.
	std::array<double, kMaxSounds> base {};

	friend bool operator==(const SoundGamma&, const SoundGamma&) = default;
};

/// One sound carried further along its line, cumulatively — the same shape driftBy and rotateBy have.
///
/// PURE, AND ON THE STATE OF RECORD. The Explore corner encoder sends increments, and a stored base
/// that accumulates them can be rewound by restoring the number rather than by subtracting the
/// increments back off — §1.2's rule, and what makes a breadcrumb retraceable exactly rather than
/// approximately.
///
/// A slot past the end is ignored rather than clamped: there is no sound there to carry, and clamping
/// would silently move a different one.
inline SoundGamma nudgeGammaBase(const SoundGamma& from, size_t slot, double delta) {
	SoundGamma carried = from;
	if (slot < kMaxSounds) {
		carried.base[slot] = from.base[slot] + delta;
	}
	return carried;
}

} // namespace feathers
