/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "character/phi_triangle.hpp"
#include "device/transfer_curve.h"

#include <algorithm>

// THE ROUTE TRANSFER'S φ CURVE — the one part of a route's transfer that is Feathers' character code, cut out of
// the device SDK's `device/transfer_curve.h` (MIT) so that header includes nothing of Feathers'. Everything a
// curve CODE is lives there; this header evaluates the φ curve and gives the host `shapeTransfer`, which every
// resolve, readout and picture calls. Bit-identical to the single header it was cut from (`test_transfer_pin`).

namespace feathers::device {


// ---------------------------------------------------------------------------------------------------
// THE φ-TRIANGLE CURVE (backlog PY).
//
// **ONE HOUSE φ-TRIANGLE, ITS PHASE WARPED BY A SUM OF TWO MORE** — `PhiTriContext::eval`, the construct the
// terrain and every phase-space device walk, with the input as its position and the periods as its frequency:
//
//     warp(u) = 0.2·tri±(P·φ⁻¹·u) + 0.12·tri±(P·φ^−½·u)   tri± = the house triangle, centred (offset ¼)
//     f(u)    = tri(P·u + warp(u))                         tri  = the house unipolar triangle, 0 → 1 → 0
//
// over the input's magnitude u = |x| in 0..1, the sign kept (Log's and Exp's rule), so a rested source still
// adds nothing: f(0) = tri(0) = 0 exactly. **WHY A WARPED TRIANGLE AND NOT A PLAIN SUM.** A sum of triangles
// averages toward ½ and reaches 1 only where every term peaks at once, so its reach is a narrow band around the
// middle; ONE triangle read at a wandering phase is a triangle VALUE, so it spans 0..1 at every period count,
// and the φ-ratio warp is what makes successive periods unequal and never repeat. It is continuous (every term
// is) and deterministic (no seed: the irrational ratios decorrelate it, which the brief preferred over a stored
// seed). **THE PERIODS ARE HONEST**: the warp's slope is at most 4·(0.2·φ⁻¹ + 0.12·φ^−½)·P ≈ 0.87·P, under the
// carrier's P, so the phase never runs backwards — each period is one rise and one fall; the local rate runs
// from about 0.13 to 1.87 of the mean, so some periods are half as long and some several times as long — and
// there are P of them within ±0.32 (the warp's whole swing). A first draft warped at φ·P and φ²·P, depths ½
// and ¼: its phase reversed, and "16 per swing" drew 70 peaks.
//
// **THE PERIODS ARE `exp2(v)`, v in 0..7** — 1 to 128 periods per full swing, even in octaves, as MATH's
// Periods control stores them (`Unit::LogMultiple`). A route keeps v quantised to `kPhiStepsPerOctave`.

/// THE WARP'S TWO TERMS — depth and φ-power, in the house constants.
inline constexpr float kPhiWarpDepth1 = 0.2f;  ///< at φ⁻¹ · P
inline constexpr float kPhiWarpDepth2 = 0.12f; ///< at φ^−½ · P
/// THE PHASE NEVER REVERSES: the warp's steepest slope, per unit of P, stays under the carrier's 1.
static_assert(4.0f * (kPhiWarpDepth1 * owlet::dsp::phi::kPhiN100 + kPhiWarpDepth2 * owlet::dsp::phi::kPhiN050) < 0.9f);

/// **f(u) FOR u IN 0..1 AT `periods`** — see the note above. Output in 0..1. `u` outside 0..1 is clamped.
[[nodiscard]] inline float phiTriangleCurve(float u, float periods) noexcept {
	namespace phi = owlet::dsp::phi;
	const float x = std::clamp(u, 0.0f, 1.0f);
	const phi::PhiTriContext ctx { x, periods, 1.0f, 0.0 };
	// CENTRED: `eval`'s bipolar form is `2·tri − 1`, which starts a cycle at −1; the ¼ offset starts it at 0, so
	// the warp is 0 at u = 0 and f(0) is exactly the triangle's own 0.
	const float warp = kPhiWarpDepth1 * ctx.eval(phi::kPhiN100, 1.0f, 0.25f, true)
	                   + kPhiWarpDepth2 * ctx.eval(phi::kPhiN050, 1.0f, 0.25f, true);
	return ctx.eval(1.0f, 1.0f, warp, false);
}

/// **ONE SOURCE READING THROUGH `t`.** Lin answers `x` itself, unclamped — but no resolve calls this for
/// Lin; it is here for the picture and the cases. A code that is not a curve answers 0: a stored number no menu
/// can produce is silence, not a guess (MATH's rule).
[[nodiscard]] inline float shapeTransfer(Transfer t, float x) noexcept { return shapeTransferWith(t, x, phiTriangleCurve); }

/// **WHERE A φ CURVE PEAKS** — the input magnitude in 0..1 at which `phiTriangleCurve` is highest, found on a
/// 4096-point scan (well under a step of the steepest curve's peak). Every other curve's extremes lie at an end or
/// at zero; a φ curve's lie inside, so a readout that sweeps a route's reach reads here too (± for a bipolar
/// source). For the UI, never the audio thread.
[[nodiscard]] inline float phiPeakReading(Transfer t) noexcept {
	if (!isPhiTransfer(t)) {
		return 1.0f;
	}
	constexpr int kScan = 4096;
	const float periods = phiPeriods(t);
	float best = 0.0f;
	float at = 1.0f;
	for (int i = 0; i <= kScan; ++i) {
		const float u = static_cast<float>(i) / static_cast<float>(kScan);
		const float y = phiTriangleCurve(u, periods);
		if (y > best) {
			best = y;
			at = u;
		}
	}
	return at;
}

} // namespace feathers::device
