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

#include "character/phi_lfo_shape.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace feathers::character;

// THE φ-AUTHORED LFO SHAPE — every case here asserts a VALUE rather than comparing two renders,
// which is CLAUDE.md's oldest rule and the one this particular port is most exposed to: the whole
// device is "one knob redraws the waveform", so ANY two coordinates give different output and a
// difference check would pass for a generator that had been replaced by noise.
//
// AND THE PORT IS A DELIBERATE DIVERGENCE, WHICH IS WHY THE CONTINUITY CASE IS HERE. The firmware
// evaluates this shape in q31 and stores each run's slope as `slope * 2^31` — but the slopes are
// amplitude-per-unit-phase and run up to 18.9, so the store overflows for 89.9% of segments
// (MEASURED, 1024 coordinates x 5 segments = 5120, 4602 saturating). The consequence is not
// rounding: the q31 output snaps to each corner's level and then ramps at a slope capped near 2.0,
// so its largest step between adjacent samples at 1/8192 of a cycle is 1.999992 where the float
// shape's is 0.002302. Mean |difference| between the two over the coordinate range is 0.32 on a
// signal that spans ±1. So this is not the firmware's waveform reproduced in float — it is the
// waveform the firmware's own float half was written to produce, which its integer half discards.
// `test_fixedpoint.cpp` is the precedent for pinning a fixed-point contract; this is the opposite
// case, and the continuity case below is what pins the divergence.

namespace {

/// How finely the sweeps walk the coordinate. Enough that a bank retune moves at least one of them.
constexpr int kCoords = 1024;

constexpr float coordAt(int i) { return static_cast<float>(i) / static_cast<float>(kCoords - 1); }

/// The narrowest run the generator can produce, DERIVED rather than measured, so the case fails if
/// somebody retunes the width bank rather than merely drifting.
///
/// An interior run is `phaseRange * width[i] / total`, with `width[i] >= kLfoSegmentWidthFloor` and
/// `total <= kLfoSegments * (floor + span)`. The two outer runs carry `kLfoPhaseMin` on top of that,
/// so the interior bound is the binding one.
constexpr float kNarrowestRun = (kLfoPhaseMax - kLfoPhaseMin) * kLfoSegmentWidthFloor
                                / (static_cast<float>(kLfoSegments)
                                   * (kLfoSegmentWidthFloor + kLfoSegmentWidthSpan));

float maxAbsSlope(const LfoShape& s) {
	float m = 0.0f;
	for (std::size_t i = 0; i < kLfoSegments; ++i) {
		m = std::max(m, std::abs(s.segSlope[i]));
	}
	return m;
}

} // namespace

TEST_CASE("phi lfo shape: the corners come out ordered and spaced, with no run narrow enough to click") {
	// THE ORDERING IS BY CONSTRUCTION AND THIS IS WHAT CHECKS THE CONSTRUCTION. The generator
	// accumulates strictly-positive widths rather than sorting, so a corner that arrived out of order
	// would mean a width had gone non-positive — which is the anti-aliasing floor having stopped
	// working, not a cosmetic problem.
	float narrowestSeen = 1.0f;
	float widestSeen = 0.0f;

	for (int i = 0; i < kCoords; ++i) {
		const LfoShape s = lfoShapeAt(coordAt(i), 0.0f);

		CAPTURE(i);
		CHECK(s.phase[0] >= kLfoPhaseMin);
		CHECK(s.phase[kLfoWaypoints - 1] <= kLfoPhaseMax);

		float previous = 0.0f;
		for (std::size_t k = 0; k < kLfoWaypoints; ++k) {
			CAPTURE(k);
			CHECK(s.phase[k] > previous);
			const float run = s.phase[k] - previous;
			narrowestSeen = std::min(narrowestSeen, run);
			widestSeen = std::max(widestSeen, run);
			previous = s.phase[k];
		}
		const float lastRun = 1.0f - previous;
		CHECK(lastRun > 0.0f);
		narrowestSeen = std::min(narrowestSeen, lastRun);
		widestSeen = std::max(widestSeen, lastRun);
	}

	// THE BOUND THE WIDTH FLOOR BUYS. `phi_lfo_shape.hpp` leans on this twice: the unreachable
	// span guard says so in a comment, and the continuity case below uses the slope it implies.
	CHECK(narrowestSeen >= kNarrowestRun);

	// NON-VACUITY, and it is the assertion that matters most here: a generator stuck at one shape
	// would satisfy every ordering check above. The runs must actually MOVE as the coordinate turns.
	CHECK(widestSeen > narrowestSeen * 1.5f);
}

TEST_CASE("phi lfo shape: the levels are renormalised so every waveform travels the same distance") {
	// The claim is a VALUE — peak-to-peak is exactly 2 — not "the levels are within range". A
	// generator that clamped instead of scaling would satisfy a range check and fail this.
	int normalisedCount = 0;
	int collapsedCount = 0;

	for (int i = 0; i < kCoords; ++i) {
		const LfoShape s = lfoShapeAt(coordAt(i), 0.0f);
		CAPTURE(i);

		// The pinned endpoints are part of the waveform, so they are part of the span.
		float lo = 0.0f;
		float hi = 0.0f;
		for (std::size_t k = 0; k < kLfoWaypoints; ++k) {
			lo = std::min(lo, s.amplitude[k]);
			hi = std::max(hi, s.amplitude[k]);
		}

		if (hi - lo > 0.5f) {
			CHECK(hi - lo == doctest::Approx(2.0f).epsilon(1e-5));
			CHECK(lo >= -1.0f - 1e-5f);
			CHECK(hi <= 1.0f + 1e-5f);
			++normalisedCount;
		}
		else {
			// The collapse fallback: a waveform with no travel is flattened rather than amplified.
			for (std::size_t k = 0; k < kLfoWaypoints; ++k) {
				CHECK(s.amplitude[k] == 0.0f);
			}
			++collapsedCount;
		}
	}

	// BOTH BRANCHES MUST HAVE RUN, or one of them is untested and the tally above is decoration.
	CHECK(normalisedCount > 0);
	CHECK(collapsedCount > 0);
}

TEST_CASE("phi lfo shape: the origin is the one coordinate whose waveform collapses to rest") {
	// THIS IS THE COLLAPSE BRANCH'S ONLY REACHABLE INPUT, found by sweeping 200,000 coordinates: the
	// four level triangles agree at field phase 0 and nowhere else, so `kLfoFlatThreshold` fires
	// exactly at the origin. Pinned rather than merely noted, because a bank retune that moved the
	// degenerate point somewhere else would otherwise be silent.
	//
	// IT IS NOT A DEFECT AND IT IS NOT THIS FILE'S TO FIX. The default coordinate is chosen by ear
	// later — the standing decision is not to fit the field's offsets to protect the origin — so what
	// this case owns is that the collapse is HANDLED (flat, not amplified noise), and where it is.
	const LfoShape origin = lfoShapeAt(0.0f, 0.0f);
	for (std::size_t k = 0; k < kLfoWaypoints; ++k) {
		CAPTURE(k);
		CHECK(origin.amplitude[k] == 0.0f);
		CHECK(origin.segSlope[k] == 0.0f);
	}
	CHECK(evalLfoShape(origin, 0.25f) == 0.0f);
	CHECK(evalLfoShape(origin, 0.75f) == 0.0f);

	// AND IT IS A POINT, NOT A REGION — a neighbour a thousandth away is already at full travel, so
	// the collapse cannot be quietly widening into the range a hand would use.
	const LfoShape neighbour = lfoShapeAt(0.001f, 0.0f);
	float lo = 0.0f;
	float hi = 0.0f;
	for (std::size_t k = 0; k < kLfoWaypoints; ++k) {
		lo = std::min(lo, neighbour.amplitude[k]);
		hi = std::max(hi, neighbour.amplitude[k]);
	}
	CHECK(hi - lo == doctest::Approx(2.0f).epsilon(1e-5));
}

TEST_CASE("phi lfo shape: the waveform is continuous, which the firmware's integer evaluator is not") {
	// THE CASE THAT PINS THE DELIBERATE DIVERGENCE — see the file header for the measurement. A
	// piecewise-linear shape read at a step of `dt` cannot move further than `maxSlope * dt` between
	// two reads; the firmware's q31 evaluator moves 1.999992 at dt = 1/8192, because its stored slope
	// has overflowed and its corners are reached by a jump rather than by a run.
	//
	// SAMPLED FINELY ENOUGH TO STRADDLE EVERY CORNER: the narrowest run is bounded below by
	// kNarrowestRun (~0.072), so 8192 steps put at least 500 samples inside the narrowest run and a
	// jump at a corner cannot hide between two samples.
	constexpr int kSteps = 8192;
	constexpr float dt = 1.0f / static_cast<float>(kSteps);

	float worstStepSeen = 0.0f;
	float worstSlopeSeen = 0.0f;

	for (int i = 0; i < kCoords; ++i) {
		const LfoShape s = lfoShapeAt(coordAt(i), 0.0f);
		const float slope = maxAbsSlope(s);
		worstSlopeSeen = std::max(worstSlopeSeen, slope);

		// The bound is this shape's own maximum slope, so the case cannot be satisfied by a
		// generator that simply produced a flatter waveform — that would lower both sides together.
		const float bound = slope * dt + 1e-5f;

		float previous = evalLfoShape(s, 0.0f);
		for (int p = 1; p <= kSteps; ++p) {
			const float here = evalLfoShape(s, static_cast<float>(p) * dt);
			const float step = std::abs(here - previous);
			if (step > worstStepSeen) {
				worstStepSeen = step;
			}
			CAPTURE(i);
			CAPTURE(p);
			CHECK(step <= bound);
			previous = here;
		}
	}

	// NON-VACUITY: the shapes really are steep somewhere, so the bound above is a constraint rather
	// than a formality satisfied by a flat line. 18.9 was the measured maximum; the bar is set well
	// under it so a retune does not fail this for the wrong reason.
	CHECK(worstSlopeSeen > 5.0f);
	CHECK(worstStepSeen > 0.0f);
}

TEST_CASE("phi lfo shape: the cycle begins and ends at rest, so a loop has nothing to click on") {
	for (int i = 0; i < kCoords; i += 7) {
		const LfoShape s = lfoShapeAt(coordAt(i), 0.0f);
		CAPTURE(i);
		// The pinned endpoints, asserted as values at both ends of the cycle. A bare `== 0.0f`, NOT
		// an Approx: `Approx(0).epsilon(e)` compares |lhs-rhs| against e*(scale+max|.|), which is
		// e*0 = 0 for a zero expectation, so it rejects even an exact match. The playbook's entry on
		// Approx below 1 is the same trap one step further on. The value here is exactly zero by
		// construction — segment 0 starts at phase 0 with level 0 — so exact is the honest test.
		CHECK(evalLfoShape(s, 0.0f) == 0.0f);
		CHECK(std::abs(evalLfoShape(s, 1.0f - 1e-6f)) < 1e-3f);
		// Wrapping past the end lands back at the start rather than running off the last run.
		CHECK(evalLfoShape(s, 1.0f) == evalLfoShape(s, 0.0f));
	}
}

TEST_CASE("phi lfo shape: a phase outside the cycle wraps, which is what lets one shape be tapped at several offsets") {
	// THE PROPERTY THE WHOLE STATELESS DESIGN RESTS ON. The automodulator reads one waveform at four
	// offset phases at once; if an offset past 1.0 did not wrap, three of those four taps would be
	// reading the last run forever.
	const LfoShape s = lfoShapeAt(0.37f, 0.0f);

	CHECK(evalLfoShape(s, 1.25f) == doctest::Approx(evalLfoShape(s, 0.25f)));
	CHECK(evalLfoShape(s, 7.25f) == doctest::Approx(evalLfoShape(s, 0.25f)));
	CHECK(evalLfoShape(s, -0.25f) == doctest::Approx(evalLfoShape(s, 0.75f)));

	// THE NEGATIVE-SIDE EDGE, pinned because it is REACHABLE and was nearly deleted as waste. A tiny
	// negative phase wraps to exactly 1.0f — one past the last segment — and the shape must read it
	// as the start of the cycle rather than as a point off the end. Found by mutating the two guards
	// in `evalLfoShape` separately: each alone was GREEN and only both together went red, which is
	// the masked pair the playbook's 8b describes. This assertion is what breaks the masking, so the
	// fixup guard now has a case of its own instead of an alibi.
	CHECK(evalLfoShape(s, -1e-30f) == evalLfoShape(s, 0.0f));
	CHECK(evalLfoShape(s, -1e-45f) == evalLfoShape(s, 0.0f));

	// NON-VACUITY: the two phases compared above are not the same value by accident.
	CHECK(evalLfoShape(s, 0.25f) != doctest::Approx(evalLfoShape(s, 0.75f)));
}

TEST_CASE("phi lfo shape: a non-finite phase reads as rest rather than propagating") {
	// std::clamp(NaN, lo, hi) RETURNS NaN — the playbook's 8a trap — so this guard cannot be a clamp
	// and a case is what proves it is not one.
	const LfoShape s = lfoShapeAt(0.61f, 0.0f);
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	CHECK(evalLfoShape(s, nan) == 0.0f);
	CHECK(evalLfoShape(s, inf) == 0.0f);
	CHECK(evalLfoShape(s, -inf) == 0.0f);

	// A good neighbour in the same block, so "it returned zero for everything" cannot pass for a fix.
	CHECK(evalLfoShape(s, 0.3f) != 0.0f);
}

TEST_CASE("phi lfo shape: the offset moves the coordinate along the same field, exactly") {
	// WHAT A ZONE CONTROL'S PHASE OFFSET IS — the same line, further along it. Asserted as an
	// identity rather than as "the shape changed", because "it changed" is satisfied by an offset
	// that scrambles the field instead of translating it.
	// COORDINATES ON AN EXACT BINARY GRID, and that is a statement about what is being claimed rather
	// than a convenience. The identity is exact in the FIELD, which is read in double; the sum the
	// caller hands over is computed in float. On a 1/64 grid both offsets below are exactly
	// representable, so the two roads agree bit for bit and the case can assert equality rather than
	// nearness. Off the grid they differ in the last bit of the float sum — which is the caller's
	// rounding, not the generator's, and asserting through it would be testing float addition.
	for (int i = 0; i < 64; ++i) {
		const float coord = static_cast<float>(i) / 64.0f;
		CAPTURE(i);
		CHECK(lfoShapeAt(coord, 0.25f) == lfoShapeAt(coord + 0.25f, 0.0f));
		CHECK(lfoShapeAt(coord, -0.5f) == lfoShapeAt(coord - 0.5f, 0.0f));
	}

	// NON-VACUITY: an offset that did nothing would satisfy the identity above trivially.
	CHECK_FALSE(lfoShapeAt(0.3f, 0.25f) == lfoShapeAt(0.3f, 0.0f));
}

TEST_CASE("phi lfo shape: one coordinate names one waveform, and here is which one") {
	// THE PINNED VALUES. Everything above is a property; this is the case that fails when the bank is
	// retuned or the accumulate/normalise arithmetic drifts, and it is the reason a property suite is
	// not enough on its own — every property here is satisfied by a DIFFERENT valid generator.
	const LfoShape s = lfoShapeAt(0.5f, 0.0f);

	CHECK(s.phase[0] == doctest::Approx(0.18760476f).epsilon(1e-6));
	CHECK(s.phase[1] == doctest::Approx(0.32520953f).epsilon(1e-6));
	CHECK(s.phase[2] == doctest::Approx(0.57540631f).epsilon(1e-6));
	CHECK(s.phase[3] == doctest::Approx(0.71301109f).epsilon(1e-6));

	CHECK(s.amplitude[0] == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(s.amplitude[1] == doctest::Approx(0.88825333f).epsilon(1e-6));
	CHECK(s.amplitude[2] == doctest::Approx(0.95190376f).epsilon(1e-6));
	CHECK(s.amplitude[3] == doctest::Approx(-1.0f).epsilon(1e-6));

	// And what it READS AS, which is the number anything downstream will actually see.
	CHECK(evalLfoShape(s, 0.25f) == doctest::Approx(0.94932979f).epsilon(1e-6));
	CHECK(evalLfoShape(s, 0.50f) == doctest::Approx(0.93272030f).epsilon(1e-6));
	CHECK(evalLfoShape(s, 0.75f) == doctest::Approx(-0.87111378f).epsilon(1e-6));

	// DETERMINISM, stated because the generator reads a field and a field is the kind of thing that
	// grows a cache. Two calls, same answer, bit for bit.
	CHECK(lfoShapeAt(0.5f, 0.0f) == s);
}
