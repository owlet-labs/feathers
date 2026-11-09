/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/decisions.h"

#include <cmath>

//--------------------------------------------------------------------------------------------------//
// THE SECOND HALF OF THE VERBATIM FOUNDATION                                                         //
//--------------------------------------------------------------------------------------------------//
//
// `hash_random.h` supplies the bits; this file supplies what Sklitter DOES with them before any mode
// logic gets involved. Both are ported verbatim for the same reason and pinned the same way, and the
// same warning applies: if one of these goes red, the port stopped being a copy. Do not update the
// constant to match — find the line that drifted.
//
// THE SEEDS ARE THE PART MOST WORTH PINNING AND THE LEAST OBVIOUS. They exist to DECORRELATE
// decisions: reverse, skip, length and delay each draw from the same base hash through a different
// seed, and it is only the seeds that keep them independent. Mistype one into another's value and two
// decisions move together forever — a Sklitter that reverses exactly when it skips. That is audible as
// "less random", it is not a crash, and no probability-shaped test would catch it.

namespace sklitter = feathers::fx::sklitter;
using namespace feathers::fx::sklitter;

TEST_CASE("the hash seeds are the decorrelation, and they are all distinct") {
	CHECK(HashSeed::ReverseDecision == 0x12345678u);
	CHECK(HashSeed::SkipDecision == 0x9ABCDEF0u);
	CHECK(HashSeed::SkipTarget == 0x11223344u);
	CHECK(HashSeed::DryMix == 0x2468ACE0u);
	CHECK(HashSeed::BinarySubdiv == 0x13579BDFu);
	CHECK(HashSeed::TripletSubdiv == 0xFEDCBA98u);
	CHECK(HashSeed::SliceOffset == 0xAABBCCDDu);
	CHECK(HashSeed::LengthMult == 0x55667788u);
	CHECK(HashSeed::DelayRatio == 0xDEADBEEFu);
	CHECK(HashSeed::DelayDecision == 0xBAADF00Du);
	CHECK(HashSeed::PitchDecision == 0xCAFEBABEu);
	CHECK(HashSeed::RepeatSlice == 0xFACEFEEDu);
	CHECK(HashSeed::LongGrain == 0xBEEFCAFEu);

	// AND DISTINCTNESS AS A PROPERTY, not just as thirteen literals. The literals catch a changed
	// seed; this catches a DUPLICATED one, which is the failure that actually costs something and
	// which thirteen independent equality checks would happily pass.
	const uint32_t seeds[] = {
	    HashSeed::ReverseDecision, HashSeed::SkipDecision,   HashSeed::SkipTarget,
	    HashSeed::DryMix,          HashSeed::BinarySubdiv,   HashSeed::TripletSubdiv,
	    HashSeed::SliceOffset,     HashSeed::LengthMult,     HashSeed::DelayRatio,
	    HashSeed::DelayDecision,   HashSeed::PitchDecision,  HashSeed::RepeatSlice,
	    HashSeed::LongGrain,
	};
	constexpr size_t n = sizeof(seeds) / sizeof(seeds[0]);
	CHECK(n == 13u);
	for (size_t i = 0; i < n; ++i) {
		for (size_t j = i + 1; j < n; ++j) {
			CAPTURE(i);
			CAPTURE(j);
			CHECK(seeds[i] != seeds[j]);
		}
	}
}

TEST_CASE("computeDelayTimeRatio is two bits selecting a power of two") {
	// All four branches explicitly, because the switch is the whole function and a wrong arm is a
	// delay at the wrong subdivision rather than an error.
	CHECK(computeDelayTimeRatio(64u, 0u) == 16u); // 1/4x
	CHECK(computeDelayTimeRatio(64u, 1u) == 32u); // 1/2x
	CHECK(computeDelayTimeRatio(64u, 2u) == 64u); // 1x
	CHECK(computeDelayTimeRatio(64u, 3u) == 128u); // 2x

	// ONLY the low two bits are read. A port that hashed the whole word, or masked a different pair,
	// would pass the four above and fail here.
	CHECK(computeDelayTimeRatio(64u, 4u) == 16u);
	CHECK(computeDelayTimeRatio(64u, 0xFFFFFFFDu) == 32u); // ...01
	CHECK(computeDelayTimeRatio(64u, 0xDEADBEEFu) == computeDelayTimeRatio(64u, 3u));
}

TEST_CASE("kLengthMultLevels is eight steps from a half to unity") {
	CHECK(kLengthMultLevels[0] == 0.5f);
	CHECK(kLengthMultLevels[1] == 0.5625f);
	CHECK(kLengthMultLevels[2] == 0.625f);
	CHECK(kLengthMultLevels[3] == 0.6875f);
	CHECK(kLengthMultLevels[4] == 0.75f);
	CHECK(kLengthMultLevels[5] == 0.8125f);
	CHECK(kLengthMultLevels[6] == 0.875f);
	CHECK(kLengthMultLevels[7] == 1.0f);

	// MONOTONIC AND BOUNDED, as a property. A slice length multiplier above 1 would read past the
	// slice it was given.
	for (int i = 1; i < 8; ++i) {
		CAPTURE(i);
		CHECK(kLengthMultLevels[i] > kLengthMultLevels[i - 1]);
	}
	CHECK(kLengthMultLevels[7] <= 1.0f);
}

TEST_CASE("sliceIndexFromTriangle clamps at the top, which is where it matters") {
	CHECK(sliceIndexFromTriangle(0.0f, 8) == 0);
	CHECK(sliceIndexFromTriangle(0.5f, 8) == 4);
	CHECK(sliceIndexFromTriangle(0.999f, 8) == 7);

	// **THE CASE THE CLAMP EXISTS FOR.** tri == 1.0 gives idx == numSlices, one past the end. Without
	// the clamp this reads a slice that does not exist, and a triangle legitimately reaches 1.0.
	CHECK(sliceIndexFromTriangle(1.0f, 8) == 7);

	// Degenerate counts, and out-of-range input, both guarded.
	CHECK(sliceIndexFromTriangle(0.5f, 1) == 0);
	CHECK(sliceIndexFromTriangle(0.5f, 0) == 0);
	CHECK(sliceIndexFromTriangle(-0.5f, 8) == 0);
	CHECK(sliceIndexFromTriangle(2.0f, 8) == 7);
}

TEST_CASE("gateFromTriangle never fully mutes") {
	// The floor is the point: a gate of zero is a slice you cannot hear, and the triangle reaches 0.
	CHECK(gateFromTriangle(0.0f) == 0.125f);
	CHECK(gateFromTriangle(1.0f) == 1.0f);
	CHECK(gateFromTriangle(0.5f) == 0.5625f);
	CHECK(gateFromTriangle(0.0f) > 0.0f);
}

TEST_CASE("reverseFromTriangle compares strictly greater") {
	CHECK(reverseFromTriangle(0.6f, 0.5f) == true);
	CHECK(reverseFromTriangle(0.4f, 0.5f) == false);
	// The boundary, both sides. A >= here doubles the reverse rate at a threshold of 0.
	CHECK(reverseFromTriangle(0.5f, 0.5f) == false);
	CHECK(reverseFromTriangle(0.0f, 0.0f) == false);
	CHECK(reverseFromTriangle(1.0f, 0.0f) == true);
}

TEST_CASE("pitchFromTriangle centres on unity") {
	// The centre is exact and must be: a pitch ratio of anything but 1.0 at the midpoint means the
	// unmodulated case is detuned.
	CHECK(pitchFromTriangle(0.5f, 12.0f) == 1.0f);
	CHECK(pitchFromTriangle(1.0f, 12.0f) == 2.0f);  // +12 semitones, an octave up
	CHECK(pitchFromTriangle(0.0f, 12.0f) == 0.5f);  // -12 semitones, an octave down
	CHECK(pitchFromTriangle(0.75f, 12.0f) == doctest::Approx(std::exp2(0.5f)).epsilon(1e-6));
	CHECK(pitchFromTriangle(0.5f, 0.0f) == 1.0f);   // no range, no shift
}

TEST_CASE("tapeSpeedFromTriangle and filterFreqFromTriangle hit their endpoints") {
	CHECK(tapeSpeedFromTriangle(0.0f) == 0.5f);
	CHECK(tapeSpeedFromTriangle(1.0f) == 2.0f);
	CHECK(tapeSpeedFromTriangle(0.5f) == 1.25f); // LINEAR, not geometric -- 1.25, not 1.0
	CHECK(tapeSpeedFromTriangle(0.5f, 1.0f, 3.0f) == 2.0f);

	CHECK(filterFreqFromTriangle(0.0f) == doctest::Approx(200.0f).epsilon(1e-5));
	CHECK(filterFreqFromTriangle(1.0f) == doctest::Approx(8000.0f).epsilon(1e-5));
	// EXPONENTIAL, not linear: the midpoint is the geometric mean, sqrt(200*8000) == 1264.9, not the
	// arithmetic 4100. This is the assertion that distinguishes the two mappings.
	CHECK(filterFreqFromTriangle(0.5f) == doctest::Approx(std::sqrt(200.0f * 8000.0f)).epsilon(1e-4));
}

TEST_CASE("the pitch tables are what the reference's are") {
	SUBCASE("kPitchRatioFP is 16.16 fixed point of 2^(n/12)") {
		CHECK(kPitchRatioFP[0] == 65536u);   // 1.0
		CHECK(kPitchRatioFP[12] == 131072u); // exactly an octave, exactly double
		CHECK(kPitchRatioFP[17] == 174959u);
		// **ENTRY 17 IS ROUNDED WRONG IN THE REFERENCE BY ONE UNIT, AND WE KEEP IT.** MEASURED: the
		// exact value is 174960.129, so correct rounding gives 174960 and the reference says
		// 174959. Every other entry rounds correctly (worst of the other seventeen: 0.494). It is
		// 0.00029 of a semitone and inaudible, and it is PARITY -- pinned here by value so that the
		// next reader who recomputes the table finds the discrepancy already known rather than
		// "correcting" it and silently diverging from the device this is a port of.
		CHECK(kPitchRatioFP[17] == 174959u);
		CHECK(std::abs(static_cast<double>(kPitchRatioFP[17]) - std::exp2(17 / 12.0) * 65536.0)
		      == doctest::Approx(1.129).epsilon(1e-3));

		// AND AS A PROPERTY, computed independently rather than transcribed: every entry is
		// 2^(n/12) scaled by 65536. The bound is the MEASURED worst case, not a guessed one.
		for (int n = 0; n < 18; ++n) {
			CAPTURE(n);
			const double want = std::exp2(n / 12.0) * 65536.0;
			CHECK(std::abs(static_cast<double>(kPitchRatioFP[n]) - want) < 1.2);
		}
		// The other seventeen are correctly rounded, which is what makes 17 a slip rather than a
		// different intent.
		for (int n = 0; n < 17; ++n) {
			CAPTURE(n);
			const double want = std::exp2(n / 12.0) * 65536.0;
			CHECK(std::abs(static_cast<double>(kPitchRatioFP[n]) - want) < 0.5);
		}
	}
	SUBCASE("kScaleSemitones: the scales, then thirteen constant rows") {
		CHECK(kScaleSemitones[0][0] == 0);
		CHECK(kScaleSemitones[0][7] == 12); // every scale row ends on the octave
		CHECK(kScaleSemitones[1][2] == 4);  // Major has a major third
		CHECK(kScaleSemitones[2][2] == 3);  // Minor has a minor third
		CHECK(kScaleSemitones[5][3] == 6);  // Blues has the flat five

		// ROWS 12..24 ARE FIXED SEMITONE SHIFTS, each row a single repeated value n-11. Asserted as
		// the rule rather than as 104 literals, so a typo anywhere in the block is caught.
		for (int row = 12; row < 25; ++row) {
			for (int col = 0; col < 8; ++col) {
				CAPTURE(row);
				CAPTURE(col);
				CHECK(kScaleSemitones[row][col] == row - 11);
			}
		}
	}
}

TEST_CASE("the phi banks are the reference's, config for config") {
	// These select WHICH phi triangle drives which structural or timbral decision. A swapped entry
	// is a Sklitter whose slice offset moves at the rate its length multiplier should.
	REQUIRE(kStructuralBank.size() == 3u);
	CHECK(kStructuralBank[0].phiFreq == owlet::dsp::phi::kPhi100);
	CHECK(kStructuralBank[0].phaseOffset == 0.00f);
	CHECK(kStructuralBank[1].phiFreq == owlet::dsp::phi::kPhi150);
	CHECK(kStructuralBank[1].phaseOffset == 0.33f);
	CHECK(kStructuralBank[2].phiFreq == owlet::dsp::phi::kPhi175);
	CHECK(kStructuralBank[2].phaseOffset == 0.67f);

	REQUIRE(kTimbraBank.size() == 4u);
	CHECK(kTimbraBank[0].phiFreq == owlet::dsp::phi::kPhiN050);
	CHECK(kTimbraBank[1].phiFreq == owlet::dsp::phi::kPhi067);
	CHECK(kTimbraBank[2].phiFreq == owlet::dsp::phi::kPhi125);
	CHECK(kTimbraBank[3].phiFreq == owlet::dsp::phi::kPhi200);
	// The timbral bank spreads by a quarter turn; the structural one by thirds.
	for (size_t i = 0; i < kTimbraBank.size(); ++i) {
		CAPTURE(i);
		CHECK(kTimbraBank[i].phaseOffset == doctest::Approx(0.25f * static_cast<float>(i)));
	}
	// NONE of them are bipolar. Every consumer treats these as [0,1].
	for (const auto& c : kStructuralBank) { CHECK(c.bipolar == false); }
	for (const auto& c : kTimbraBank) { CHECK(c.bipolar == false); }
}
