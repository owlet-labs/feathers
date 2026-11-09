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

// FAST TAN AND TANH, WRITTEN FROM THE MATHEMATICS — the two approximations the filter family runs:
// `fasttan` for the bilinear prewarp g = tan(pi·fc/sr), and `fasttanh` for the drive stage's
// saturation curve. They replace the GPL `sst::basic_blocks::dsp` pair (2026-09-24 licence
// migration); nothing here was read from that code.
//
// THE SOURCE OF BOTH IS ONE CONTINUED FRACTION. Lambert's (1768) expansion
//
//     tanh x = x / (1 + x²/(3 + x²/(5 + x²/(7 + ...))))
//
// truncated after the 7 gives the [7/6] Padé approximant
//
//     tanh x ≈ x (135135 + 17325 x² + 378 x⁴ + x⁶) / (135135 + 62370 x² + 3150 x⁴ + 28 x⁶)
//
// and tan x = −i·tanh(i·x) turns every x² into −x², so the same coefficients with alternating signs
// are the [7/6] Padé approximant of tan. (Any table of Padé approximants — e.g. Baker & Graves-Morris,
// "Padé Approximants", or Abramowitz & Stegun 4.5.70 for the fraction itself — has them.)
//
// ERROR BOUNDS, MEASURED IN FLOAT against std::tan / std::tanh in double (the case
// "fast math: tan and tanh stay inside their stated bounds" in tests/test_fast_math_approx.cpp pins
// them):
//
//   - fasttanh on [−4.5, 4.5], the whole range any caller hands it (they clamp there): absolute error
//     at most 4.3e-5, reached at the ends, and under 1e-6 on [−2, 2]. PAST ±5 IT EXCEEDS 1 AND THEN
//     GROWS LIKE x/28, so it is NOT a saturator on its own — every caller clamps its argument first.
//   - fasttan on [0, 0.499·π], the prewarp's whole range: relative error at most 3e-7, a couple of
//     float ulps. The fraction alone would lose accuracy toward the pole at π/2, so the argument is
//     RANGE-REDUCED: above π/4, tan x = 1 / tan(π/2 − x), and π/2 − x is formed from a two-part π/2 so
//     that the small difference is not swamped by the rounding of π/2 itself (the subtraction of the
//     high part is exact by Sterbenz's lemma, since x is within a factor of two of it). Without that
//     split the relative error at 0.499·π is 1.4e-5; with it, 2.5e-7.

// FEATHERS_FORCE_SIMDE takes the arm64 road on x86-64 too, so scripts/clang-preflight.sh compiles what macOS does.
#if !defined(FEATHERS_FORCE_SIMDE)                                                                        \
    && (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#endif

namespace feathers::dsp {

namespace fast_math_detail {

/// The [7/6] Padé approximant of tan on |y| <= π/4, where its truncation error is far below a float ulp.
inline float tanPade76(float y) {
	const float y2 = y * y;
	return y * (135135.0f - y2 * (17325.0f - y2 * (378.0f - y2)))
	       / (135135.0f - y2 * (62370.0f - y2 * (3150.0f - 28.0f * y2)));
}

/// π/4, and π/2 split into a float head and the remainder the head could not hold.
inline constexpr float kQuarterPi = 0.785398163397448309616f;
inline constexpr float kHalfPiHi = 1.57079637050628662109375f;     // float(π/2), 0x3FC90FDB
inline constexpr float kHalfPiLo = -4.37113900018624283e-8f;       // π/2 − kHalfPiHi

} // namespace fast_math_detail

/// tan(x) for |x| < π/2. See the file header for the method and its measured error.
inline float fasttan(float x) {
	using namespace fast_math_detail;
	const bool negative = x < 0.0f;
	const float a = negative ? -x : x;
	float r;
	if (a <= kQuarterPi) {
		r = tanPade76(a);
	}
	else {
		r = 1.0f / tanPade76((kHalfPiHi - a) + kHalfPiLo);
	}
	return negative ? -r : r;
}

/// Four lanes of fasttan. Lane by lane through the scalar, so a lane and the scalar agree to the bit —
/// a coefficient solve runs this once per control tick, where four scalar calls cost nothing.
inline __m128 fasttanSSE(__m128 x) {
	alignas(16) float lanes[4];
	_mm_store_ps(lanes, x);
	for (float& lane : lanes) {
		lane = fasttan(lane);
	}
	return _mm_load_ps(lanes);
}

/// tanh(x), for |x| <= 4.5 — callers clamp. See the file header.
inline float fasttanh(float x) {
	const float x2 = x * x;
	return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)))
	       / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
}

} // namespace feathers::dsp
