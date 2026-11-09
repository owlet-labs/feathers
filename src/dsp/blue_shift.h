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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// WHAT MOTION DOES TO THE BLEND — §2b, and every field of it is zero for a parked puck.
//
// The two-way rule's asymmetry was never "the target is special": it was the direction Path travels,
// frozen into a role, because a two-point fade has only one direction to have. A constellation has no
// target — but it has a velocity, and that is the honest version of the same idea.
//
// IN ITS OWN HEADER BECAUSE BOTH ENGINES TAKE IT. It lived in morph_renderer.h while only the morph
// blend read it, and that is exactly how the stair engine came to be left out of a decision that names
// no engine: the shape of the code said whose feature it was. It is neither's.

namespace feathers {

struct BlueShift {
	/// Per sound, signed: positive while you close on it, negative while you leave it. It brightens
	/// what you are approaching and dims what you are leaving, which is navigation you can hear.
	std::span<const double> approach;
	/// Per sound: how much of each one's share would arrive next if you kept going. This is where the
	/// amplitude overshoot gets its direction back, without needing a target to aim at.
	std::span<const double> weightRate;
	/// The depth the push is applied at, already multiplied by the reactivity field and the audition's
	/// scale.
	float overshoot = 0.0f;

	/// Whether the DOPPLER half can change anything: some sound is being approached or left.
	///
	/// Separate from active() because the two halves are separately switchable, and the weight tilt
	/// must not run when every approach is zero — see tilt().
	bool hasApproach() const {
		for (const double a : approach) {
			if (a != 0.0) {
				return true;
			}
		}
		return false;
	}

	/// Whether anything here can change a sound. False for a parked puck and for a build with the
	/// scales down, and a blend TAKES A DIFFERENT PATH when it is false rather than the same path with
	/// zeros in it.
	///
	/// The reasoning is that normalised weights sum to 1.0 to fifteen places rather than exactly 1.0,
	/// so renormalising an untilted set could move every one of them in the last bit — which would make
	/// a build with the scales down something other than the build without any of this in it.
	///
	/// HONESTLY LABELLED: that is a guarantee rather than a repair. A known-bad with the branch removed
	/// does NOT fail the suite, because multiplying by a reciprocal within an ulp of one rounds back to
	/// the same double at every position tested. It is kept because "rounds back everywhere we tried"
	/// and "cannot change" are different claims.
	bool active() const { return overshoot != 0.0f || hasApproach(); }
};

/// The blend weights leaned the way the puck is travelling: approaching sounds count for more,
/// receding sounds for less, renormalised so the shares still sum to one.
///
/// Shared by both engines because it is a statement about the CONSTELLATION rather than about either
/// recipe — the same normalised weights go in, and what comes out is what every downstream quantity
/// should be averaged by. `out` must hold at least `n`; returns false when nothing was tilted, in
/// which case the caller must keep using the weights it already had, untouched.
inline bool tiltWeights(std::span<const double> weights, const BlueShift& blue, double* out, size_t n) {
	if (!blue.hasApproach() || blue.approach.size() < n || n == 0) {
		return false;
	}
	double total = 0.0;
	for (size_t k = 0; k < n; ++k) {
		// Floored well above zero rather than clamped at it: a share driven to nothing by motion would
		// make a sound VANISH as you turned away from it, and the plan asks for dimming.
		out[k] = weights[k] * std::max(0.1, 1.0 + blue.approach[k]);
		total += out[k];
	}
	// FINITE AS WELL AS POSITIVE, and the second half is not redundant. `total > 0.0` is true of
	// infinity, and dividing by it sends every share to zero — a blend with no sounds in it. The old
	// inline version guarded both; the guard was dropped in the hoist, and while nothing upstream can
	// reach it today this is a public helper now and its callers are not all written yet.
	if (!(total > 0.0) || !std::isfinite(total)) {
		return false;
	}
	const double scale = 1.0 / total;
	for (size_t k = 0; k < n; ++k) {
		out[k] *= scale;
	}
	return true;
}

} // namespace feathers
