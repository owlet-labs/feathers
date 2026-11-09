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

#include "fx/sklitter/ring.h"

#include <cmath>
#include <cstddef>

// **A FRACTIONAL READ OF `fx::sklitter::Ring`, BY ABSOLUTE FRAME POSITION.**
//
// The delay wrote this first, as a private helper, with a note in backlog GX that it belonged on
// `Ring` "the day a third device wants it". The pitch shifter is that device — and it lives HERE
// rather than on `Ring` because `ring.h` sits in SKLITTER's directory, under that device's tests and
// other sessions' hands, and two free functions over its public surface need nothing from inside it.
//
// **"ABSOLUTE" IS THE CALLER'S MONOTONIC WRITE COUNTER, not a ring index.** `Ring::writePos()` wraps;
// a read head that drifts (a pitched grain, a gliding tap) wants a position that does not. The two
// agree — `counter % length == writePos` — for as long as the caller never calls `setLength` after
// `prepare` and resets its counter with the ring.

namespace feathers::fx {

/// FOLD AN ABSOLUTE FRAME POSITION INTO THE RING. `std::fmod` rather than an integer cast, because
/// the position may be NEGATIVE during the first lap — a tap reaching back past the start of the
/// recording — and casting that to `size_t` is undefined where folding it is merely silent (which
/// is the truth: that region has not been recorded yet).
[[nodiscard]] inline size_t ringIndex(double absolutePos, size_t length) noexcept {
	if (length == 0) {
		return 0;
	}
	const auto len = static_cast<double>(length);
	double m = std::fmod(absolutePos, len);
	if (m < 0.0) {
		m += len;
	}
	const auto i = static_cast<size_t>(m);
	return i < length ? i : 0;
}

/// ONE FRAME OUT OF THE RING, LINEARLY INTERPOLATED. A head that lands between samples and is read
/// nearest-neighbour steps audibly on every move.
inline void readLerp(const sklitter::Ring& ring, double absolutePos, float* left, float* right) noexcept {
	const size_t len = ring.length();
	if (len == 0) {
		*left = 0.0f;
		*right = 0.0f;
		return;
	}
	const double whole = std::floor(absolutePos);
	const auto frac = static_cast<float>(absolutePos - whole);
	const size_t i0 = ringIndex(whole, len);
	const size_t i1 = (i0 + 1u) % len;
	float l0 = 0.0f;
	float r0 = 0.0f;
	float l1 = 0.0f;
	float r1 = 0.0f;
	ring.readAt(i0, &l0, &r0);
	ring.readAt(i1, &l1, &r1);
	*left = l0 + (l1 - l0) * frac;
	*right = r0 + (r1 - r0) * frac;
}

} // namespace feathers::fx
