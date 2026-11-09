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

#include "fx/smooth/smooth_types.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

// THE SMOOTH SHAPER'S PRIMITIVES, PINNED BY VALUE.
//
// Every expected value here was recorded from the implementation the device shipped with, and each
// case checks a few exact entries plus a digest over a sweep, so a replacement that differs anywhere
// in a sweep fails. Floats are compared by their bits, never approximately: these functions feed a
// bit-identical integer path, and "close" is a different sound once it wraps.

namespace smooth = feathers::fx::smooth;

namespace {

struct Fnv64 {
	uint64_t h = 1469598103934665603ull;
	void add(uint32_t u) {
		for (int k = 0; k < 4; ++k) {
			h ^= (u >> (8 * k)) & 0xFFu;
			h *= 1099511628211ull;
		}
	}
	void add(int32_t v) { add(static_cast<uint32_t>(v)); }
	void add(float f) { add(std::bit_cast<uint32_t>(f)); }
};

struct XorShift32 {
	uint32_t x = 2463534242u;
	uint32_t next() {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	}
	int32_t nextInt() { return static_cast<int32_t>(next()); }
	/// A float in [lo, hi), from the top 24 bits.
	float nextIn(float lo, float hi) {
		return lo + (hi - lo) * (static_cast<float>(next() >> 8) * (1.0f / 16777216.0f));
	}
};

/// The table point i, recovered exactly: at a phase on a point the blend weight toward the next is zero,
/// so the result is the point times 65536.
int32_t tablePoint(uint32_t i) { return smooth::tableSine(i << 24) / 65536; }

void addCoefficients(Fnv64& f, const smooth::Coefficients& c) {
	for (const float v : { c.c1, c.c3, c.c5, c.c7, c.c9 }) {
		f.add(v);
	}
	for (const int32_t v : { c.c1q, c.c3q, c.c5q, c.c7q, c.c9q }) {
		f.add(v);
	}
}

void addTwist(Fnv64& f, const smooth::TwistParams& t) {
	for (const float v : { t.stereoWidth, t.stereoFreqMult, t.stereoPhaseOffset, t.stereoLfoRate, t.evenAmount,
	                       t.evenDryBlend, t.rectAmount, t.feedbackAmount, t.phaseHarmonic, t.phaseHarmonicFreqMod }) {
		f.add(v);
	}
}

} // namespace

TEST_CASE("VALUES: the sine table's 257 points") {
	// One cycle in 256 points of round(32767 sin(2 pi i / 256)), and a 257th equal to the first, which
	// only the last segment's interpolation reaches.
	CHECK(tablePoint(0) == 0);
	CHECK(tablePoint(1) == 804);
	CHECK(tablePoint(32) == 23170);
	CHECK(tablePoint(64) == 32767);
	CHECK(tablePoint(85) == 28510);
	CHECK(tablePoint(128) == 0);
	CHECK(tablePoint(192) == -32767);
	CHECK(tablePoint(255) == -804);
	// Halfway along the last segment: halfway from point 255 back to point 256, which is point 0.
	CHECK(smooth::tableSine(0xFF800000u) == (-804 * 32768 + 0 * 32768));
	Fnv64 f;
	for (uint32_t i = 0; i < 256; ++i) {
		f.add(tablePoint(i));
	}
	CHECK(f.h == 0xf9a84da78a6cf12eull);
}

TEST_CASE("VALUES: tableSine interpolates by the sixteen bits under the top byte") {
	CHECK(smooth::tableSine(0x40000000u) == 32767 * 65536);
	CHECK(smooth::tableSine(0xC0000000u) == -32767 * 65536);
	CHECK(smooth::tableSine(0x00008000u) == 804 * 128);  // a 512th of the way to point 1
	CHECK(smooth::tableSine(0x000000FFu) == 0);           // the low byte is ignored
	Fnv64 f;
	XorShift32 r;
	for (int i = 0; i < (1 << 20); ++i) {
		f.add(smooth::tableSine(r.next()));
	}
	for (uint32_t p = 0xFF000000u; p != 0u; p += 0x00000100u) {
		f.add(smooth::tableSine(p)); // every step of the last segment, into the guard point
	}
	CHECK(f.h == 0x894fb4e36a24d05ull);
}

TEST_CASE("VALUES: zoneOf, driveGainOf and saturatingQ31") {
	CHECK(smooth::zoneOf(0).index == 0);
	CHECK(smooth::zoneOf(smooth::kZoneWidthQ31).index == 1);
	CHECK(smooth::zoneOf(smooth::kOneQ31).index == 7);
	CHECK(smooth::driveGainOf(0) == 1.0f);
	CHECK(smooth::driveGainOf(INT32_MIN) == 0.0f);
	CHECK(smooth::driveGainOf(-(1 << 30)) == 0.25f);
	CHECK(smooth::driveGainOf(1 << 30) == 2.25f);
	CHECK(smooth::saturatingQ31(std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(smooth::saturatingQ31(3.0e9f) == INT32_MAX);
	CHECK(smooth::saturatingQ31(-3.0e9f) == INT32_MIN);
	CHECK(smooth::saturatingQ31(-1.75f) == -1);
	CHECK(smooth::saturatingQ31(2147483520.0f) == 2147483520);

	Fnv64 zone;
	Fnv64 drive;
	XorShift32 r;
	for (int i = 0; i < (1 << 18); ++i) {
		const int32_t v = r.nextInt();
		const smooth::Zone z = smooth::zoneOf(v);
		zone.add(z.index);
		zone.add(z.position);
		drive.add(smooth::driveGainOf(v));
	}
	for (int32_t k = 0; k <= 8; ++k) {
		for (int32_t d = -2; d <= 2; ++d) {
			const int64_t v = static_cast<int64_t>(k) * smooth::kZoneWidthQ31 + d;
			if (v >= INT32_MIN && v <= INT32_MAX) {
				const smooth::Zone z = smooth::zoneOf(static_cast<int32_t>(v));
				zone.add(z.index);
				zone.add(z.position);
			}
		}
	}
	CHECK(zone.h == 0xd8ec6d8e04ec80d9ull);
	CHECK(drive.h == 0x231cdf7382bd36c2ull);

	Fnv64 sat;
	for (int i = 0; i < (1 << 18); ++i) {
		sat.add(smooth::saturatingQ31(std::bit_cast<float>(r.next())));
	}
	CHECK(sat.h == 0x9b26f6abdf6f1edull);
}

TEST_CASE("VALUES: the blend weights and the Chebyshev coefficients over position and Gamma") {
	Fnv64 weights;
	Fnv64 coeffs;
	for (const double gamma : { 0.0, 0.5, 1.0, 2.75, -1.3, 17.25 }) {
		for (int i = 0; i <= 256; ++i) {
			const float pos = static_cast<float>(i) / 256.0f;
			for (const float w : smooth::blendWeightsOf(pos, gamma)) {
				weights.add(w);
			}
			addCoefficients(coeffs, smooth::chebyshevCoefficientsOf(pos, gamma));
		}
	}
	CHECK(weights.h == 0x2d8c7872fda60078ull);
	CHECK(coeffs.h == 0x6984c8133de268ddull);
}

TEST_CASE("VALUES: Twist's parameters over the knob and the three phases") {
	Fnv64 f;
	XorShift32 r;
	for (int k = 0; k <= 64; ++k) {
		const auto q = static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(k) * (int64_t(1) << 25), INT32_MAX));
		addTwist(f, smooth::twistParamsOf(q, 0.0f, 0.0f, 0.0f));
	}
	for (int i = 0; i < 4096; ++i) {
		const int32_t q = r.nextInt() & INT32_MAX;
		const float tph = r.nextIn(-3.0f, 3.0f);
		const float hph = r.nextIn(-3.0f, 3.0f);
		const float g = r.nextIn(-4.0f, 4.0f);
		addTwist(f, smooth::twistParamsOf(q, (i & 1) != 0 ? tph : 0.0f, (i & 2) != 0 ? hph : 0.0f, (i & 4) != 0 ? g : 0.0f));
	}
	CHECK(f.h == 0xa4785aa064b185d6ull);
}

TEST_CASE("VALUES: one sample through each of the eight zones") {
	Fnv64 f;
	Fnv64 probes;
	XorShift32 r;
	for (int zone = 0; zone < smooth::kZones; ++zone) {
		for (int i = 0; i < 8192; ++i) {
			const int32_t input = r.nextInt() >> (i % 12); // every level from full scale down
			const float drive = r.nextIn(0.0f, 4.0f);
			const float pos = r.nextIn(0.0f, 1.0f);
			smooth::Coefficients w = smooth::chebyshevCoefficientsOf(pos, static_cast<double>(r.nextIn(-2.0f, 2.0f)));
			if (zone == 7) {
				w.c1 = r.nextIn(0.0f, 1.0f); // the cascade blend
				w.c3 = r.nextIn(0.0f, 1.0f); // the self-multiplication blend
				w.updateQ31();
			}
			const float even = (i & 1) != 0 ? r.nextIn(0.0f, 1.0f) : 0.0f;
			const float evenDry = r.nextIn(0.0f, 1.0f);
			const float rect = (i & 2) != 0 ? r.nextIn(0.0f, 1.0f) : 0.0f;
			smooth::ShapeProbe probe;
			f.add(smooth::shapeSample(input, drive, zone, w, even, evenDry, rect, &probe));
			probes.add(probe.driven);
			probes.add(probe.zoneInput);
		}
	}
	CHECK(f.h == 0x1e763a8333dfc5c3ull);
	CHECK(probes.h == 0x2a1c0790c268e685ull);
}

TEST_CASE("VALUES: cascadePolynomial, exact entries and a digest") {
	struct V {
		int32_t x, expected;
	};
	constexpr V kVectors[] = {
		{ 0, 0 },
		{ 1, 0 },
		{ -1, -24 },
		{ 1000, 8968 },
		{ -1000, -9000 },
		{ 1048576, 9437104 },
		{ 16777216, 150749264 },
		{ 268435456, 1517879296 },
		{ -268435456, -1517879296 },
		{ 536870912, -167772112 }, // past the linear region the intermediate terms wrap
		{ -536870912, 167772160 },
		{ 1073741823, 1073741792 },
		{ 1073741824, 1073741840 },
		{ -1073741824, -1073741824 },
		{ 1610612736, -1577058176 },
		{ 2147483647, 2147483160 },
		{ -2147483647 - 1, -2147483647 - 1 },
		{ -2147483647, -2147483320 },
		{ 123456789, 1015495232 },
		{ 987654321, 136454440 },
		{ 1431655765, -1513265160 },
	};
	for (const V& v : kVectors) {
		CAPTURE(v.x);
		CHECK(smooth::cascadePolynomial(v.x) == v.expected);
	}
	Fnv64 f;
	for (uint32_t k = 0; k < (1u << 16); ++k) {
		f.add(smooth::cascadePolynomial(static_cast<int32_t>(k << 16)));
	}
	XorShift32 r;
	for (int i = 0; i < (1 << 20); ++i) {
		f.add(smooth::cascadePolynomial(r.nextInt()));
	}
	CHECK(f.h == 0xa05365b54d3de11eull);
}

TEST_CASE("VALUES: shiftSaturate, exact entries and a digest over every shift 0..31") {
	struct V {
		int32_t x;
		int shift;
		int32_t expected;
	};
	constexpr V kVectors[] = {
		{ -1, 0, -1 },
		{ -305419896, 0, -305419896 },
		{ 100, 1, 200 },
		{ 2147483647, 1, 2147483646 },
		{ -16777217, 1, -33554434 },
		{ 1, 7, 128 },
		{ -100, 7, -12800 },
		{ -8388608, 7, -1073741824 },
		{ 16777215, 7, 2147483520 },
		{ 16777216, 7, 2147483520 },
		{ 305419896, 7, 2147483520 },
		{ -16777216, 7, -2147483647 - 1 },
		{ -16777217, 7, -2147483647 - 1 },
		{ -1, 8, -256 },
		{ 8388607, 8, 2147483392 },
		{ 8388608, 8, 2147483392 },
		{ -8388608, 8, -2147483647 - 1 },
		{ -8388609, 8, -2147483647 - 1 },
		{ 100, 16, 6553600 },
		{ 8388608, 16, 2147418112 },
		{ -16777217, 16, -2147483647 - 1 },
		{ 1, 31, 0 },
		{ -100, 31, -2147483647 - 1 },
	};
	for (const V& v : kVectors) {
		CAPTURE(v.x);
		CAPTURE(v.shift);
		CHECK(smooth::shiftSaturate(v.x, v.shift) == v.expected);
	}
	Fnv64 f;
	XorShift32 r;
	for (int s = 0; s < 32; ++s) {
		for (int i = 0; i < 4096; ++i) {
			f.add(smooth::shiftSaturate(r.nextInt(), s));
		}
		for (const int64_t edge : { int64_t(1) << (31 - s), -(int64_t(1) << (31 - s)) }) {
			for (int64_t d = -3; d <= 3; ++d) {
				const int64_t v = edge + d;
				if (v >= INT32_MIN && v <= INT32_MAX) {
					f.add(smooth::shiftSaturate(static_cast<int32_t>(v), s));
				}
			}
		}
	}
	CHECK(f.h == 0x1b2be25266fa3bbcull);
}
