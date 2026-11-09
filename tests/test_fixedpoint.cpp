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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/fixedpoint.h"

#include <cstdint>
#include <limits>
#include <vector>

using namespace feathers::character;

// THE SIX OPERATIONS, PINNED DIRECTLY — because the golden capture only covers two of them.
//
// THIS FILE EXISTS BECAUSE A MEASUREMENT CONTRADICTED THE PLAN. The character goldens were held in
// place specifically so the fixed-point replacement could be proved bit-exact against them, and that
// was the right instinct — but "the golden is the venue" was assumed rather than checked. Broken
// deliberately, one symbol at a time, rebuilt and re-run:
//
//     multiply_32x32_rshift32                     -> returns 42, golden still PASSES
//     multiply_accumulate_32x32_rshift32_rounded  -> returns 42, golden still PASSES
//     add_saturate                                -> returns 42, golden still PASSES
//     q31_from_double_saturating                  -> returns 42, golden FAILS
//     u32_from_double_saturating                  -> returns 42, golden FAILS
//
// TWO OF FIVE. The other three are reached only from a noise/jitter path in phi_morph.cpp that
// `buildPhiMorphWavetable` never calls, so the capture cannot see them however wrong they are. A
// replacement "proved" by that capture alone would have been three-fifths unproven, and nobody would
// have known — the suite would have been green, the golden would have been green, and the claim in
// the commit message would have been false.
//
// THE REFERENCES BELOW ARE COMPUTED INDEPENDENTLY, not by restating any implementation: the
// expectations divide a 128-bit product and name boundary values in `if`s. An expectation that spells
// the code back at itself passes for any implementation including a wrong one.

/// The reference arithmetic's width. 128 bits where the compiler has them; otherwise 64, which still holds every value
/// these references form: the largest is INT32_MIN * INT32_MIN + 2^31 = 2^62 + 2^31, and a sum of two int32s needs 33 bits.
/// MSVC has no 128-bit integer, and the references must not borrow the shift or clamp under test either way.
#if defined(__SIZEOF_INT128__)
using Wide = __int128;
#else
using Wide = std::int64_t;
#endif

namespace {

/// floor(a*b / 2^32), computed in 128 bits so the reference does not reuse the shift under test.
int32_t referenceMulRshift32(int32_t a, int32_t b) {
	const Wide product = static_cast<Wide>(a) * b;
	const Wide divisor = static_cast<Wide>(1) << 32;
	Wide q = product / divisor;
	// C++ division truncates toward zero; the shift under test floors. They differ for negatives that
	// do not divide exactly, and that difference is the entire point of asserting it.
	if (product < 0 && q * divisor != product) {
		q -= 1;
	}
	return static_cast<int32_t>(q);
}

} // namespace

TEST_CASE("multiply_32x32_rshift32 is a floored 64-bit product shift") {
	// A floor, not a truncation, and negatives are where the two part company.
	CHECK(multiply_32x32_rshift32(0, 0) == 0);
	CHECK(multiply_32x32_rshift32(INT32_MAX, 0) == 0);

	// Half times half is a quarter, in a domain where 2^31 is one.
	CHECK(multiply_32x32_rshift32(1 << 30, 1 << 30) == referenceMulRshift32(1 << 30, 1 << 30));
	CHECK(multiply_32x32_rshift32(1 << 30, 1 << 30) == (1 << 28));

	const int32_t kCases[] = { 1,          -1,       2,          -2,      12345,   -12345,
		                       INT32_MAX,  INT32_MIN, 1 << 16,   -(1 << 16), 0x5A5A5A5A,
		                       static_cast<int32_t>(0xA5A5A5A5) };
	for (const int32_t a : kCases) {
		for (const int32_t b : kCases) {
			CAPTURE(a);
			CAPTURE(b);
			CHECK(multiply_32x32_rshift32(a, b) == referenceMulRshift32(a, b));
		}
	}
}

TEST_CASE("the rounded accumulate adds half before shifting, and that is not the same as truncating") {
	// THE ONE BIT THE NAME DOES NOT TELL YOU. "Rounded" describes several arithmetics; this one adds
	// 2^31 — half of the shift's divisor — BEFORE shifting, so it rounds to nearest rather than down.
	const auto reference = [](int32_t sum, int32_t a, int32_t b) {
		const Wide product = static_cast<Wide>(a) * b + (static_cast<Wide>(1) << 31);
		const Wide divisor = static_cast<Wide>(1) << 32;
		Wide q = product / divisor;
		if (product < 0 && q * divisor != product) {
			q -= 1;
		}
		return static_cast<int32_t>(sum + static_cast<int32_t>(q));
	};

	const int32_t kCases[] = { 0, 1, -1, 12345, -12345, 1 << 30, -(1 << 30), INT32_MAX, INT32_MIN };
	for (const int32_t a : kCases) {
		for (const int32_t b : kCases) {
			CAPTURE(a);
			CAPTURE(b);
			CHECK(multiply_accumulate_32x32_rshift32_rounded(0, a, b) == reference(0, a, b));
		}
	}

	// AND IT IS GENUINELY DIFFERENT FROM THE UNROUNDED FORM, or this case is pinning a distinction
	// that does not exist. A product whose low 32 bits are exactly half rounds up here and down there.
	// A product of EXACTLY 2^31 — the low half sits precisely on the halfway mark, so truncating
	// gives 0 and rounding carries to 1. A first draft used 3 * 0x55555556, where the two agree, and
	// the check failed 1 != 1: the case was wrong, not the arithmetic. A non-vacuity check needs its
	// own non-vacuity check, which is the joke this project keeps landing on.
	const int32_t a = 2;
	const int32_t b = 1 << 30; // a * b == 2^31
	CHECK(multiply_32x32_rshift32(a, b) == 0);
	CHECK(multiply_accumulate_32x32_rshift32_rounded(0, a, b) == 1);
}

TEST_CASE("add_saturate clamps at the ends instead of wrapping") {
	CHECK(add_saturate(1, 2) == 3);
	CHECK(add_saturate(-5, 5) == 0);

	// THE POINT OF IT. Plain int32 addition here is undefined behaviour and in practice wraps to a
	// large negative; saturation is what keeps a sum at the ceiling.
	CHECK(add_saturate(INT32_MAX, 1) == INT32_MAX);
	CHECK(add_saturate(INT32_MAX, INT32_MAX) == INT32_MAX);
	CHECK(add_saturate(INT32_MIN, -1) == INT32_MIN);
	CHECK(add_saturate(INT32_MIN, INT32_MIN) == INT32_MIN);

	// Just inside the boundary it must NOT clamp, or the check above is satisfied by a function that
	// always returns the extremes.
	CHECK(add_saturate(INT32_MAX - 1, 1) == INT32_MAX);
	CHECK(add_saturate(INT32_MAX - 10, 5) == INT32_MAX - 5);
}

TEST_CASE("add_saturate matches a 128-bit reference across every boundary neighbourhood") {
	// The reference widens to 128 bits and names the two ceilings in `if`s. The sweep is every pair drawn from windows around the five places the result can
	// change character -- both ends, zero, and the two points where one operand alone reaches an end --
	// plus a xorshift sample of the whole plane.
	const auto reference = [](int32_t a, int32_t b) -> int32_t {
		const Wide s = static_cast<Wide>(a) + static_cast<Wide>(b);
		if (s > static_cast<Wide>(2147483647)) {
			return 2147483647;
		}
		if (s < -static_cast<Wide>(2147483648LL)) {
			return static_cast<int32_t>(-2147483647 - 1);
		}
		return static_cast<int32_t>(s);
	};

	constexpr int64_t kWindow = 64;
	const int64_t kCentres[] = { INT32_MIN, -(int64_t(1) << 30), -1, 0, 1, int64_t(1) << 30, INT32_MAX };
	std::vector<int32_t> values;
	for (const int64_t c : kCentres) {
		for (int64_t d = -kWindow; d <= kWindow; ++d) {
			const int64_t v = c + d;
			if (v >= INT32_MIN && v <= INT32_MAX) {
				values.push_back(static_cast<int32_t>(v));
			}
		}
	}

	uint64_t mismatches = 0;
	uint64_t pairs = 0;
	for (const int32_t a : values) {
		for (const int32_t b : values) {
			++pairs;
			if (add_saturate(a, b) != reference(a, b)) {
				++mismatches;
			}
		}
	}
	uint32_t x = 2463534242u;
	for (int i = 0; i < (1 << 20); ++i) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		const int32_t a = static_cast<int32_t>(x);
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		const int32_t b = static_cast<int32_t>(x);
		++pairs;
		if (add_saturate(a, b) != reference(a, b)) {
			++mismatches;
		}
	}
	CAPTURE(pairs);
	CHECK(mismatches == 0u);
	// NON-VACUITY: the sweep really does reach both clamps and the pass-through.
	CHECK(add_saturate(INT32_MAX - 3, 64) == INT32_MAX);
	CHECK(add_saturate(INT32_MIN + 3, -64) == INT32_MIN);
	CHECK(add_saturate(INT32_MAX - 64, 63) == INT32_MAX - 1);
}

TEST_CASE("the double converters clamp at their own type's ends") {
	CHECK(q31_from_double_saturating(0.0) == 0);
	CHECK(q31_from_double_saturating(1.0) == 1);
	CHECK(q31_from_double_saturating(-1.0) == -1);

	// THE CONSTANTS ARE EXACTLY REPRESENTABLE AS DOUBLES, which is why the original compared against
	// literals rather than against INT32_MAX — and why replacing them with the macros would be a
	// change rather than a tidy-up at some other width.
	CHECK(q31_from_double_saturating(1e18) == INT32_MAX);
	CHECK(q31_from_double_saturating(-1e18) == INT32_MIN);
	CHECK(q31_from_double_saturating(2147483647.0) == INT32_MAX);
	CHECK(q31_from_double_saturating(-2147483648.0) == INT32_MIN);

	CHECK(u32_from_double_saturating(0.0) == 0u);
	CHECK(u32_from_double_saturating(-1.0) == 0u); // NEGATIVES CLAMP TO ZERO, not to the top
	CHECK(u32_from_double_saturating(1e18) == 4294967295u);
	CHECK(u32_from_double_saturating(4294967295.0) == 4294967295u);
	CHECK(u32_from_double_saturating(12345.0) == 12345u);

	// A NaN FAILS BOTH CLAMP ARMS AND REACHED THE CAST — undefined, in the two functions whose contract
	// is to make the conversion safe (backlog EH, shape 3). Zero, and every finite value as before.
	const double nan = std::numeric_limits<double>::quiet_NaN();
	CHECK(q31_from_double_saturating(nan) == 0);
	CHECK(u32_from_double_saturating(nan) == 0u);
	CHECK(q31_from_double_saturating(std::numeric_limits<double>::infinity()) == 0);
	CHECK(u32_from_double_saturating(-std::numeric_limits<double>::infinity()) == 0u);
}

TEST_CASE("q31_t is the width the character layer assumes") {
	// The phi sources index and mask on the assumption that this is exactly 32 bits and signed. It is
	// a typedef, so nothing would fail to compile if it moved — it would just quietly compute
	// something else.
	CHECK(sizeof(q31_t) == 4);
	CHECK(std::numeric_limits<q31_t>::is_signed);
    CHECK(std::numeric_limits<q31_t>::max() == INT32_MAX);
}

//--------------------------------------------------------------------------------------------------//
// CLEAN-ROOM ORACLE: input -> output vectors, and a whole-plane digest of each operation.            //
//--------------------------------------------------------------------------------------------------//
//
// Recorded from the implementation these cases were written against. The vectors name the edges (the
// rounding carry, the wrap of the accumulate, both clamps); the digests cover a million pseudo-random
// argument tuples each, so a replacement that differs anywhere in the plane is very unlikely to pass.

namespace {

struct Fnv64 {
	uint64_t h = 1469598103934665603ull;
	void add(int32_t v) {
		const auto u = static_cast<uint32_t>(v);
		for (int k = 0; k < 4; ++k) {
			h ^= (u >> (8 * k)) & 0xFFu;
			h *= 1099511628211ull;
		}
	}
};

struct XorShift32 {
	uint32_t x = 2463534242u;
	int32_t next() {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return static_cast<int32_t>(x);
	}
};

struct Pair {
	int32_t a, b, expected;
};
struct Triple {
	int32_t sum, a, b, expected;
};

constexpr Pair kMulVectors[] = {
	{ 0, 1, 0 },
	{ 0, 1073741824, 0 },
	{ 0, INT32_MIN, 0 },
	{ 0, 65536, 0 },
	{ 1, 2, 0 },
	{ 1, INT32_MAX, 0 },
	{ 1, -305419896, -1 },
	{ -1, -1, 0 },
	{ -1, -1073741824, 0 },
	{ -1, 305419896, -1 },
	{ 3, 1, 0 },
	{ 3, 1073741824, 0 },
	{ 3, INT32_MIN, -2 },
	{ 3, 65536, 0 },
	{ -3, 2, -1 },
	{ -3, INT32_MAX, -2 },
	{ -3, -305419896, 0 },
	{ 32767, -1, -1 },
	{ 32767, -1073741824, -8192 },
	{ 32767, 305419896, 2330 },
	{ 65536, 1, 0 },
	{ 65536, 1073741824, 16384 },
	{ 65536, INT32_MIN, -32768 },
	{ 65536, 65536, 1 },
	{ -65536, 2, -1 },
	{ -65536, INT32_MAX, -32768 },
	{ -65536, -305419896, 4660 },
	{ 1073741824, -1, -1 },
	{ 1073741824, -1073741824, -268435456 },
	{ 1073741824, 305419896, 76354974 },
	{ -1073741824, 1, -1 },
	{ -1073741824, 1073741824, -268435456 },
	{ -1073741824, INT32_MIN, 536870912 },
	{ -1073741824, 65536, -16384 },
	{ 1073741825, 2, 0 },
	{ 1073741825, INT32_MAX, 536870912 },
	{ 1073741825, -305419896, -76354975 },
	{ 1431655765, -1, -1 },
	{ 1431655765, -1073741824, -357913942 },
	{ 1431655765, 305419896, 101806631 },
	{ -1431655766, 1, -1 },
	{ -1431655766, 1073741824, -357913942 },
	{ -1431655766, INT32_MIN, 715827883 },
	{ -1431655766, 65536, -21846 },
	{ 123456789, 2, 0 },
	{ 123456789, INT32_MAX, 61728394 },
	{ 123456789, -305419896, -8779150 },
	{ -987654321, -1, 0 },
	{ -987654321, -1073741824, 246913580 },
	{ -987654321, 305419896, -70233197 },
	{ INT32_MAX, 1, 0 },
	{ INT32_MAX, 1073741824, 536870911 },
	{ INT32_MAX, INT32_MIN, -1073741824 },
	{ INT32_MAX, 65536, 32767 },
	{ INT32_MIN, 2, -1 },
	{ INT32_MIN, INT32_MAX, -1073741824 },
	{ INT32_MIN, -305419896, 152709948 },
	{ -1, 1, -1 },
	{ 1, -1, -1 },
	{ -1, -1, 0 },
	{ INT32_MIN, INT32_MIN, 1073741824 },
	{ INT32_MIN, INT32_MAX, -1073741824 },
	{ INT32_MAX, INT32_MAX, 1073741823 },
	{ -3, 1073741824, -1 },
	{ 3, 1073741824, 0 },
	{ 2, 1073741824, 0 },
	{ -2, 1073741824, -1 },
};

constexpr Triple kMacVectors[] = {
	{ 0, 0, 1, 0 },
	{ 0, 1, 305419896, 0 },
	{ 0, 3, -1073741824, -1 },
	{ 0, 32767, -1, 0 },
	{ 0, 65536, -305419896, -4660 },
	{ 0, 1073741824, INT32_MAX, 536870912 },
	{ 0, 1073741825, 2, 1 },
	{ 0, 1431655765, 65536, 21845 },
	{ 0, 123456789, INT32_MIN, -61728394 },
	{ 0, INT32_MAX, 1073741824, 536870912 },
	{ 7, 0, 1, 7 },
	{ 7, 1, 305419896, 7 },
	{ 7, 3, -1073741824, 6 },
	{ 7, 32767, -1, 7 },
	{ 7, 65536, -305419896, -4653 },
	{ 7, 1073741824, INT32_MAX, 536870919 },
	{ 7, 1073741825, 2, 8 },
	{ 7, 1431655765, 65536, 21852 },
	{ 7, 123456789, INT32_MIN, -61728387 },
	{ 7, INT32_MAX, 1073741824, 536870919 },
	{ -7, 0, 1, -7 },
	{ -7, 1, 305419896, -7 },
	{ -7, 3, -1073741824, -8 },
	{ -7, 32767, -1, -7 },
	{ -7, 65536, -305419896, -4667 },
	{ -7, 1073741824, INT32_MAX, 536870905 },
	{ -7, 1073741825, 2, -6 },
	{ -7, 1431655765, 65536, 21838 },
	{ -7, 123456789, INT32_MIN, -61728401 },
	{ -7, INT32_MAX, 1073741824, 536870905 },
	{ 1000000, 0, 1, 1000000 },
	{ 1000000, 1, 305419896, 1000000 },
	{ 1000000, 3, -1073741824, 999999 },
	{ 1000000, 32767, -1, 1000000 },
	{ 1000000, 65536, -305419896, 995340 },
	{ 1000000, 1073741824, INT32_MAX, 537870912 },
	{ 1000000, 1073741825, 2, 1000001 },
	{ 1000000, 1431655765, 65536, 1021845 },
	{ 1000000, 123456789, INT32_MIN, -60728394 },
	{ 1000000, INT32_MAX, 1073741824, 537870912 },
	{ INT32_MAX, 0, 1, INT32_MAX },
	{ INT32_MAX, 1, 305419896, INT32_MAX },
	{ INT32_MAX, 3, -1073741824, 2147483646 },
	{ INT32_MAX, 32767, -1, INT32_MAX },
	{ INT32_MAX, 65536, -305419896, 2147478987 },
	{ INT32_MAX, 1073741824, INT32_MAX, -1610612737 },
	{ INT32_MAX, 1073741825, 2, INT32_MIN },
	{ INT32_MAX, 1431655765, 65536, -2147461804 },
	{ INT32_MAX, 123456789, INT32_MIN, 2085755253 },
	{ INT32_MAX, INT32_MAX, 1073741824, -1610612737 },
	{ INT32_MIN, 0, 1, INT32_MIN },
	{ INT32_MIN, 1, 305419896, INT32_MIN },
	{ INT32_MIN, 3, -1073741824, INT32_MAX },
	{ INT32_MIN, 32767, -1, INT32_MIN },
	{ INT32_MIN, 65536, -305419896, 2147478988 },
	{ INT32_MIN, 1073741824, INT32_MAX, -1610612736 },
	{ INT32_MIN, 1073741825, 2, -2147483647 },
	{ INT32_MIN, 1431655765, 65536, -2147461803 },
	{ INT32_MIN, 123456789, INT32_MIN, 2085755254 },
	{ INT32_MIN, INT32_MAX, 1073741824, -1610612736 },
	{ 2147483645, 0, 1, 2147483645 },
	{ 2147483645, 1, 305419896, 2147483645 },
	{ 2147483645, 3, -1073741824, 2147483644 },
	{ 2147483645, 32767, -1, 2147483645 },
	{ 2147483645, 65536, -305419896, 2147478985 },
	{ 2147483645, 1073741824, INT32_MAX, -1610612739 },
	{ 2147483645, 1073741825, 2, 2147483646 },
	{ 2147483645, 1431655765, 65536, -2147461806 },
	{ 2147483645, 123456789, INT32_MIN, 2085755251 },
	{ 2147483645, INT32_MAX, 1073741824, -1610612739 },
	{ 0, 2, 1073741824, 1 },
	{ 0, -2, 1073741824, 0 },
	{ 0, 1, 1, 0 },
	{ 0, -1, 1, 0 },
	{ 0, 3, 1073741824, 1 },
	{ 0, -3, 1073741824, -1 },
	{ 0, INT32_MIN, INT32_MIN, 1073741824 },
	{ INT32_MAX, 1073741824, 1073741824, -1879048193 },
	{ INT32_MAX, INT32_MIN, INT32_MIN, -1073741825 },
	{ INT32_MIN, INT32_MIN, 1, INT32_MIN },
	{ INT32_MIN, -1073741824, 1073741824, 1879048192 },
	{ 5, 2, 1073741824, 6 },
	{ 0, INT32_MAX, INT32_MAX, 1073741823 },
	{ 0, INT32_MIN, INT32_MAX, -1073741823 },
};

constexpr Pair kAddVectors[] = {
	{ 0, 0, 0 },
	{ 0, -65536, -65536 },
	{ 0, -987654321, -987654321 },
	{ 1, -3, -2 },
	{ 1, 1431655765, 1431655766 },
	{ -1, 1, 0 },
	{ -1, 1073741824, 1073741823 },
	{ -1, INT32_MAX, 2147483646 },
	{ 3, 32767, 32770 },
	{ 3, -1431655766, -1431655763 },
	{ -3, -1, -4 },
	{ -3, -1073741824, -1073741827 },
	{ -3, INT32_MIN, INT32_MIN },
	{ 32767, 65536, 98303 },
	{ 32767, 123456789, 123489556 },
	{ 65536, 3, 65539 },
	{ 65536, 1073741825, 1073807361 },
	{ -65536, 0, -65536 },
	{ -65536, -65536, -131072 },
	{ -65536, -987654321, -987719857 },
	{ 1073741824, -3, 1073741821 },
	{ 1073741824, 1431655765, INT32_MAX },
	{ -1073741824, 1, -1073741823 },
	{ -1073741824, 1073741824, 0 },
	{ -1073741824, INT32_MAX, 1073741823 },
	{ 1073741825, 32767, 1073774592 },
	{ 1073741825, -1431655766, -357913941 },
	{ 1431655765, -1, 1431655764 },
	{ 1431655765, -1073741824, 357913941 },
	{ 1431655765, INT32_MIN, -715827883 },
	{ -1431655766, 65536, -1431590230 },
	{ -1431655766, 123456789, -1308198977 },
	{ 123456789, 3, 123456792 },
	{ 123456789, 1073741825, 1197198614 },
	{ -987654321, 0, -987654321 },
	{ -987654321, -65536, -987719857 },
	{ -987654321, -987654321, -1975308642 },
	{ INT32_MAX, -3, 2147483644 },
	{ INT32_MAX, 1431655765, INT32_MAX },
	{ INT32_MIN, 1, -2147483647 },
	{ INT32_MIN, 1073741824, -1073741824 },
	{ INT32_MIN, INT32_MAX, -1 },
};

} // namespace

TEST_CASE("CLEAN-ROOM ORACLE: multiply_32x32_rshift32 vectors and plane digest") {
	for (const Pair& v : kMulVectors) {
		CAPTURE(v.a);
		CAPTURE(v.b);
		CHECK(multiply_32x32_rshift32(v.a, v.b) == v.expected);
	}
	Fnv64 f;
	XorShift32 r;
	for (int i = 0; i < (1 << 20); ++i) {
		const int32_t a = r.next();
		const int32_t b = r.next();
		f.add(multiply_32x32_rshift32(a, b));
	}
	CHECK(f.h == 0xd778849618ae07cdull);
}

TEST_CASE("CLEAN-ROOM ORACLE: multiply_accumulate_32x32_rshift32_rounded vectors and plane digest") {
	for (const Triple& v : kMacVectors) {
		CAPTURE(v.sum);
		CAPTURE(v.a);
		CAPTURE(v.b);
		CHECK(multiply_accumulate_32x32_rshift32_rounded(v.sum, v.a, v.b) == v.expected);
	}
	Fnv64 f;
	XorShift32 r;
	for (int i = 0; i < (1 << 20); ++i) {
		const int32_t sum = r.next();
		const int32_t a = r.next();
		const int32_t b = r.next();
		f.add(multiply_accumulate_32x32_rshift32_rounded(sum, a, b));
	}
	CHECK(f.h == 0xdb94f596f2f6f40eull);
}

TEST_CASE("CLEAN-ROOM ORACLE: add_saturate vectors and plane digest") {
	for (const Pair& v : kAddVectors) {
		CAPTURE(v.a);
		CAPTURE(v.b);
		CHECK(add_saturate(v.a, v.b) == v.expected);
	}
	Fnv64 f;
	XorShift32 r;
	for (int i = 0; i < (1 << 20); ++i) {
		const int32_t a = r.next();
		const int32_t b = r.next();
		f.add(add_saturate(a, b));
	}
	CHECK(f.h == 0x1a7f5ce6c25d6dcdull);
}
