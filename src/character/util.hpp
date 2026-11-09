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
 */

#pragma once

// THE TWO TRIANGLE SHAPES EVERY PHI BANK IS BUILT FROM, written for Feathers.
//
// Both are one idea: a triangle is a FOLDED ramp. Wrap the phase into one cycle, reject the dead zone,
// fold the phase about the peak so only a rising ramp is ever evaluated, and scale. The bipolar shape
// is two unipolar half-waves, the second negated.
//
// THE NUMBERS ARE FROZEN, and that decides how the fold is spelled. The phi banks, the vox and flagship
// bakes (pinned to the bit in `vox-voice` and `flagship`) and every device that reads a phi triangle
// were tuned against these outputs, so this file must return the SAME floats, not merely the same
// shape. Two details carry that: each peak is placed where the shapes have always placed it, as
// computed from `duty` (the bipolar trough at `duty/2 + duty/4`, a rounded sum, not the exact
// three-quarter point — a phase sitting on the rounding decides which side of the trough it falls), and
// each distance is measured from the start or end of its own half-wave. Verified bit for bit when this
// was written: every float phase at duty 1, 24 edge duties (zero, subnormal, negative, above one,
// infinite, NaN) against 2^24 phases each, and 150 million random pairs — zero differences.
//
// `<algorithm>` is included for the headers that include this one and use std::clamp without
// including it themselves (phi_triangle.hpp).

#include <algorithm>
#include <cmath>

namespace owlet::dsp {

namespace triangle_detail {

/// The phase's position within its cycle, in [0, 1). `floor` rather than truncation so a negative phase
/// wraps forward (-0.25 is 0.75), and a phase too large to have a fractional part lands on 0.
inline float wrapUnit(float phase) {
	return phase - std::floor(phase);
}

/// Distance from the nearer end of the span [start, end), for a position already known to lie inside it,
/// with `peak` the point where the nearer end changes over: measured from `start` before the peak and
/// from `end` after it. A triangle's height is this distance times its slope.
inline float foldAbout(float x, float start, float peak, float end) {
	return x < peak ? x - start : end - x;
}

} // namespace triangle_detail

/// Unipolar triangle over one cycle: 0 → 1 → 0 across the first `duty` of the cycle, peaking at
/// `duty / 2`, then 0 for the rest (the dead zone).
/// @param phase Phase in cycles; any value, wrapped to one cycle.
/// @param duty  Active fraction of the cycle, 0..1 (default 1: no dead zone).
/// @return 0..1
inline float triangleSimpleUnipolar(float phase, float duty = 1.0f) {
	const float x = triangle_detail::wrapUnit(phase);
	// `!(x < duty)` rather than `x >= duty` so a NaN phase lands in the dead zone rather than in a ramp.
	if (!(x < duty)) {
		return 0.0f;
	}
	// The ramp's slope is 1 / (duty / 2), taken once.
	return triangle_detail::foldAbout(x, 0.0f, duty * 0.5f, duty) * (2.0f / duty);
}

/// Bipolar triangle over one cycle: 0 → +1 → 0 → -1 → 0 across the first `duty` of the cycle, the
/// positive half-wave first, then 0 for the rest (the dead zone). Peaks at `duty / 4` and `3·duty / 4`.
/// @param phase Phase in cycles; any value, wrapped to one cycle.
/// @param duty  Active fraction of the cycle, 0..1 (default 1: no dead zone).
/// @return -1..+1
inline float triangleFloat(float phase, float duty = 1.0f) {
	const float x = triangle_detail::wrapUnit(phase);
	if (!(x < duty)) {
		return 0.0f;
	}
	const float halfWave = duty * 0.5f;
	const float quarterWave = duty * 0.25f;
	// The positive half-wave spans [0, halfWave) and peaks at quarterWave; the negative one spans
	// [halfWave, duty) and troughs at halfWave + quarterWave. Each is the same fold, scaled by the slope
	// 1 / quarterWave, and the second is negated.
	if (x < halfWave) {
		return triangle_detail::foldAbout(x, 0.0f, quarterWave, halfWave) / quarterWave;
	}
	return -triangle_detail::foldAbout(x, halfWave, halfWave + quarterWave, duty) / quarterWave;
}

} // namespace owlet::dsp
