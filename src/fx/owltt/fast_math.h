/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Fast math approximations ported from the firmware's fast_math.h.
//
// THE PORT: the firmware uses these approximations for speed on embedded hardware. They are part of
// the sound character — the envelope follower's attack/release curves and the level detection's
// log/exp all use these.
//
// **THIS PARAGRAPH USED TO END "For exact parity, we use the same approximations rather than
// std::exp/log", AND THAT IS NO LONGER TRUE OF `fastExp`.** Firmware parity is not kept for its own
// sake here. The approximation was measured 34x wrong at the argument `runEnvelope` hands it, past
// its own clamp it was 5073x wrong, and it was 1.7x SLOWER than `std::exp` on this machine — so
// parity was buying an error rather than a saving. `fastLog` and `fastTan` are unchanged and the
// parity argument still holds for them; see `fastExp` for the measurements and for why every one of
// its call sites is control-rate.

namespace feathers::fx::owltt {

/// **IT IS `std::exp` NOW, AND THAT IS BOTH FASTER AND EXACT — measured, not assumed.**
///
/// THE BIT-HACK WAS WRONG WHERE IT WAS USED. `correction = 1 + 0.08x^2` is a fudge fitted near zero
/// and it grows without bound, so the further from zero the worse it gets — and every caller here
/// works on the NEGATIVE side, which is where the error lives. Measured against `std::exp`:
///
///     x        -0.1   -1.0   -1.33   -2.0   -3.0   -5.0   -10.0   -20.0   -25.0
///     ratio    1.026  1.143  1.166   1.359  1.805  3.107  9.517   34.18   5073
///
/// `runEnvelope` evaluates `fastExp(timeConstant * numSamples)`, which at a 0.5 ms attack and a
/// 512-sample block is x = -21 — **past the clamp, where the answer was five thousand times too
/// large.** An envelope coefficient that is too large is an envelope that does not move, which is
/// the shape of every "it feels slow" report this device has had.
///
/// **AND IT WAS NEVER IN A SAMPLE LOOP.** Every call site is once per block per band —
/// `setAttack`, `setRelease`, `runEnvelope`, the gap path, and `gain = fastExp(envelope_)` — about
/// eighteen calls per block. MEASURED on this machine, 10^7 calls each: **`fastExp` 6.32 ns,
/// `std::exp` 3.67 ns.** The approximation is 1.7x SLOWER than the libm function it was avoiding,
/// so there is no trade here to weigh; the change is faster and correct.
///
/// THE CLAMP STAYS. It is overflow protection on the positive side and costs nothing on the
/// negative, where `exp(-20)` is already 2e-9.
[[gnu::always_inline]] inline float fastExp(float x) {
	return std::exp(std::fmax(-20.0f, std::fmin(20.0f, x)));
}

/// Fast natural log using IEEE 754 bit manipulation.
[[gnu::always_inline]] inline float fastLog(float x) {
	if (x <= 0.0f) {
		return -100.0f;
	}
	union {
		float f;
		int32_t i;
	} u;
	u.f = x;
	int32_t e = (u.i >> 23) - 127;
	u.i = (u.i & 0x007FFFFF) | 0x3F800000;
	float m = u.f;
	float t = m - 1.0f;
	float logM = t * (1.0f - t * (0.5f - t * 0.33f));
	return static_cast<float>(e) * 0.6931472f + logM;
}

/// Fast tan(x) using Padé (5,4) approximation.
[[gnu::always_inline]] inline float fastTan(float x) {
	float x2 = x * x;
	float x4 = x2 * x2;
	float num = x * (945.0f - 105.0f * x2 + x4);
	float den = 945.0f - 420.0f * x2 + 15.0f * x4;
	return num / den;
}

} // namespace feathers::fx::owltt
