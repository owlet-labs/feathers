/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// THE FIXED-POINT OPERATIONS THE CHARACTER LAYER AND A FEW DEVICES USE.
//
// The three integer operations below are specified behaviourally in the project's private
// clean-room notes and are implemented from that specification alone. `tests/test_fixedpoint.cpp`
// pins every one of them by value (vectors at the edges and a digest over the whole plane).

namespace feathers::character {

/// A signed 32-bit fixed-point sample.
using q31_t = int32_t;

/// The high word of the signed 64-bit product a * b.
inline q31_t multiply_32x32_rshift32(q31_t a, q31_t b) {
	// floor(a * b / 2^32). The exact product needs at most 63 bits, so it is formed in int64; an
	// arithmetic right shift of a signed value floors (C++20 defines it so), which is the rounding
	// wanted, and the quotient always fits back into 32 bits.
	const int64_t product = static_cast<int64_t>(a) * static_cast<int64_t>(b);
	return static_cast<q31_t>(product >> 32);
}

/// sum plus the high word of a * b, rounded to nearest.
inline q31_t multiply_accumulate_32x32_rshift32_rounded(q31_t sum, q31_t a, q31_t b) {
	// sum + round(a * b / 2^32), a half rounding toward +infinity: half of 2^32 is added to the exact
	// product before flooring. The accumulation wraps modulo 2^32 rather than saturating; it is
	// carried out in uint32, where wrapping is defined, and the int32 conversion is modular in C++20.
	const int64_t product = static_cast<int64_t>(a) * static_cast<int64_t>(b);
	const int64_t rounded = (product + (int64_t { 1 } << 31)) >> 32;
	const uint32_t total = static_cast<uint32_t>(sum) + static_cast<uint32_t>(rounded);
	return static_cast<q31_t>(total);
}

/// a + b, clamped to the ends of int32 rather than wrapping.
inline int32_t add_saturate(int32_t a, int32_t b) {
	// The exact sum, taken in int64 where it cannot overflow, clamped to the int32 range.
	const int64_t total = static_cast<int64_t>(a) + static_cast<int64_t>(b);
	return static_cast<int32_t>(std::clamp<int64_t>(total, INT32_MIN, INT32_MAX));
}

/// A double into a q31, clamped at both ends.
///
/// THE COMPARISONS ARE IN `double`. `2147483647.0` and `-2147483648.0` are both exactly representable,
/// so the clamp lands on the intended side of each boundary.
inline q31_t q31_from_double_saturating(double value) {
	// A NaN FAILS BOTH ARMS AND REACHES THE CAST — undefined, in the function whose whole contract is to
	// make this conversion safe (backlog EH, shape 3). Zero for it; every finite value is as before.
	if (!std::isfinite(value)) {
		return 0;
	}
	return static_cast<q31_t>(value > 2147483647.0 ? 2147483647.0
	                                               : (value < -2147483648.0 ? -2147483648.0 : value));
}

/// A double into a uint32, clamped at both ends.
///
/// NOTE WHAT THIS DOES NOT FIX. `phi_morph.cpp` casts `pEnd * 4294967295.0f` to uint32_t elsewhere,
/// where the constant rounds UP to 2^32 as a float and the out-of-range cast is undefined — x86
/// wraps to 0, arm64 saturates. That is the reason the golden test is pinned to one platform, and it
/// is in the phi source rather than here. Routing it through this function would change the
/// character on x86, so it is left alone: this file's whole contract is that nothing sounds
/// different afterwards.
inline uint32_t u32_from_double_saturating(double value) {
	if (!std::isfinite(value)) {
		return 0u; // see q31_from_double_saturating
	}
	return static_cast<uint32_t>(value > 4294967295.0 ? 4294967295.0 : (value < 0.0 ? 0.0 : value));
}

} // namespace feathers::character
