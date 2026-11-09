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

#include <algorithm>
#include <cmath>

// THE NE570-STYLE COMPANDER'S MATHS, ONCE (backlog QG) — Buckets' chip in the Delay and COMP's Compander mode both
// include this, so the two cannot drift apart.
//
// A compander is a level follower and a power law around a reference level `ref`: the follower tracks the rectified
// level with a fast attack and a slower release (both one-poles in the LINEAR domain, the chip's capacitor), and the
// gain is `(env / ref) ^ exponent`, clamped to a range. **AT `exponent = 1/R − 1` THE STATIC TRANSFER IS R:1 ABOUT
// `ref`**: an input steady at level L leaves at `ref · (L / ref)^(1/R)` — loud is turned down, quiet is turned UP, and
// `ref` is left alone. The NE570 is R = 2 (exponent −0.5). The expander undoing it uses the inverse exponent.
//
// **BUCKETS' RENDER IS BIT-IDENTICAL THROUGH THESE** (leave compander alone): every expression is
// the one `delay.cpp` wrote inline before the move, in the same order, and `bench_delay_lane --render` compared byte
// for byte before and after is the proof. Change nothing here without re-running that comparison.

namespace feathers::dsp::compander {

/// THE GAIN LAW'S FLOOR ON THE ENVELOPE: silence reads 1e-5 (-100 dBFS), so the power law never sees a zero. With an
/// expanding (positive) exponent that is a gain near zero; with a compressing one it is the top of the range.
inline constexpr float kEnvelopeFloor = 1.0e-5f;

/// THE FOLLOWER'S ONE-POLE STEP FOR A TIME CONSTANT, as `1 − e^(−1/(t·sr))`, never under one sample. Buckets' form.
[[nodiscard]] inline float coefFor(double seconds, double sampleRate) {
	return static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, seconds * sampleRate)));
}

/// ONE STEP OF THE FOLLOWER: attack while the level is above the envelope, release while below. Anything under
/// `silenceFloor` is flushed to exactly 0, so a tail ends in exact silence rather than a denormal.
inline void follow(float& env, float level, float attack, float release, float silenceFloor) {
	env += (level - env) * (level > env ? attack : release);
	if (env < silenceFloor) {
		env = 0.0f;
	}
}

/// THE GAIN FOR AN ENVELOPE: `(env / ref) ^ exponent`, clamped to [lo, hi]. Compress with `1/R − 1` (−0.5 at 2:1).
[[nodiscard]] inline float gain(float env, float ref, float exponent, float lo, float hi) {
	return std::clamp(std::pow(std::max(env, kEnvelopeFloor) / ref, exponent), lo, hi);
}

/// THE COMPRESSING EXPONENT FOR A RATIO R:1 — `1/R − 1`; 0 at 1:1 (no change), −0.5 at the chip's 2:1.
[[nodiscard]] inline float compressExponent(float ratio) {
	return 1.0f / std::max(ratio, 1.0f) - 1.0f;
}

} // namespace feathers::dsp::compander
