/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/slice_plan.h"

#include <algorithm>
#include <set>
#include <vector>

using namespace feathers::fx::sklitter;

//--------------------------------------------------------------------------------------------------//
// WHAT THIS FILE CAN HONESTLY CLAIM, AND WHAT IT DELIBERATELY DOES NOT.                              //
//--------------------------------------------------------------------------------------------------//
//
// `planFor` is a FAITHFUL PORT, not a re-expression, so the twenty-one expected numbers cannot be
// "transcribed from the reference" the way `hash_random.h`'s were -- they ARE the reference's
// arithmetic. Typing the port's own output back in as expected values would assert only that the
// code equals itself, which is the failure this repo's assert-the-value rule exists to name, and
// writing it at scale would make it look rigorous while proving nothing.
//
// SO THE CLAIMS HERE ARE ONES THAT CAN BE CHECKED WITHOUT A SECOND ORACLE:
//   - the FOUNDATION is already pinned against an independent restatement, one layer down
//     (`test_fx_sklitter_hash.cpp`, `test_fx_sklitter_decisions.cpp`), and `planFor` is a composition
//     of those verified pieces;
//   - every field lands in its declared domain, and the discrete ones on their declared LADDERS --
//     a wrong constant almost always lands off a ladder;
//   - the two zones drive DISJOINT halves of the plan, which is the design's own claim and the thing
//     a mis-wired phi constant breaks;
//   - the dead-zone cache returns what an uncached call returns, which is the one way to catch a
//     cache that is quietly lying;
//   - the slice-count ladder is exact, because that one IS transcribed.

namespace {

PhaseOffsets noOffsets() { return PhaseOffsets {}; }

SlicePlan planOf(float zoneA, float zoneB, float mc, float macro, int32_t slice,
                 const PhaseOffsets& ofs = PhaseOffsets {}) {
	PlanCache cache;
	return planFor(zoneA, zoneB, mc, macro, slice, ofs, &cache);
}

} // namespace

TEST_CASE("the slice-count ladder is the reference's, division for division") {
	// TRANSCRIBED: the reference's thresholds are 4/9/16/22/28 in its own 0..50 UI units, over a
	// -64..+64 knob. Ours is 0..1 across the same span.
	CHECK(sliceCountFor(Mode::Shuffle, 0.00f) == 1);  // one bar
	CHECK(sliceCountFor(Mode::Shuffle, 0.10f) == 2);  // halves
	CHECK(sliceCountFor(Mode::Shuffle, 0.25f) == 4);  // quarters
	CHECK(sliceCountFor(Mode::Shuffle, 0.38f) == 8);
	CHECK(sliceCountFor(Mode::Shuffle, 0.50f) == 16);
	CHECK(sliceCountFor(Mode::Shuffle, 1.00f) == 32);

	// **PITCH HALVES IT, and the reason is in the reference: a pitched grain needs time to be heard
	// AS a pitch.** The guard matters -- halving 1 would give 0 slices and divide by zero downstream.
	CHECK(sliceCountFor(Mode::Pitch, 1.00f) == 16);
	CHECK(sliceCountFor(Mode::Pitch, 0.50f) == 8);
	CHECK(sliceCountFor(Mode::Pitch, 0.00f) == 1); // NOT 0

	// Out of range is clamped rather than extrapolated.
	CHECK(sliceCountFor(Mode::Shuffle, -5.0f) == 1);
	CHECK(sliceCountFor(Mode::Shuffle, 99.0f) == 32);
}

TEST_CASE("Repeat's length is a continuous curve with a floor, not a ladder") {
	constexpr size_t kBar = 96000;
	// Squared, so the short end has the resolution: rate 0 is the whole bar, rate 1 the floor.
	CHECK(repeatSliceLength(0.0f, kBar) == kBar);
	CHECK(repeatSliceLength(1.0f, kBar) == 256u); // the ~6 ms floor
	// Halfway is a QUARTER of the bar, which is what "squared" means and what a linear curve would
	// have made a half.
	CHECK(repeatSliceLength(0.5f, kBar) == kBar / 4);
	// Never longer than what was captured, never shorter than the floor.
	CHECK(repeatSliceLength(0.99f, kBar) >= 256u);
	CHECK(repeatSliceLength(0.0f, 1000) == 1000u);
}

TEST_CASE("a coordinate's lap is its phase and its fraction is its position") {
	// None of these coordinates is bounded, by design, and `owltt.cpp` already splits its
	// Character the same way. Each whole turn is the same eight zones somewhere else in the field.
	CHECK(splitCoordinate(0.25f).position == doctest::Approx(0.25));
	CHECK(splitCoordinate(0.25f).lap == 0.0f);
	CHECK(splitCoordinate(3.25f).position == doctest::Approx(0.25));
	CHECK(splitCoordinate(3.25f).lap == 3.0f);
	// NEGATIVE COORDINATES TURN THE OTHER WAY AND MUST STILL LAND IN [0,1). floor(-0.25) is -1, so
	// the position is 0.75 -- which is the same region a positive quarter-turn back would reach.
	CHECK(splitCoordinate(-0.25f).position == doctest::Approx(0.75));
	CHECK(splitCoordinate(-0.25f).lap == -1.0f);
	CHECK(splitCoordinate(-0.25f).position >= 0.0f);
	CHECK(splitCoordinate(-0.25f).position < 1.0f);
}

TEST_CASE("every field of the plan lands in its declared domain, across the whole zone plane") {
	// A SWEEP RATHER THAN A SAMPLE. A wrong phi constant or a mistyped duty does not usually produce
	// a plausible-but-different number; it produces one off the end of a ladder, or a probability
	// outside [0,1], and that is visible without a second oracle.
	const std::set<float> lengths(std::begin(kLengthMultLevels), std::end(kLengthMultLevels));
	const std::set<int32_t> repeats { 1, 2, 4, 8 };
	const std::set<int32_t> grains { 1, 2, 4, 8 };

	// **BOTH BRANCHES, AND THE FIRST DRAFT ONLY REACHED ONE.** `planFor` splits on `phRawB`: with no
	// phase offset it reads the DISCRETE zones, and with one it ignores them and rides phi for every
	// timbral param. This sweep used default offsets, so it never entered the evolution half at all --
	// found when a known-bad deleting the gate FLOOR (which lives only in that half) stayed green.
	// Half the decision layer was untested and the suite looked thorough.
	const PhaseOffsets kDiscrete {};
	PhaseOffsets evolving {};
	evolving.zoneB = 0.37f; // any non-zero phase takes the other road
	evolving.zoneA = 0.21f;

	int examined = 0;
	for (const PhaseOffsets& ofs : { kDiscrete, evolving }) {
		const bool evolutionPath = ofs.zoneB != 0.0f;
		CAPTURE(evolutionPath);
	for (int a = 0; a <= 10; ++a) {
		for (int b = 0; b <= 10; ++b) {
			for (int s = 0; s < 16; ++s) {
				const float zoneA = static_cast<float>(a) / 10.0f;
				const float zoneB = static_cast<float>(b) / 10.0f;
				CAPTURE(zoneA);
				CAPTURE(zoneB);
				CAPTURE(s);
				const SlicePlan p = planOf(zoneA, zoneB, 0.4f, 0.6f, s, ofs);
				++examined;

				CHECK(lengths.count(p.lengthMult) == 1);
				CHECK(repeats.count(p.repeatSlices) == 1);
				CHECK(grains.count(p.grainLength) == 1);
				CHECK(p.subdivisions >= 1);
				CHECK(p.subdivisions <= 12);
				CHECK(p.sliceOffset >= 0);
				CHECK(p.sliceOffset <= 15);
				CHECK(p.skipTarget >= 0);
				CHECK(p.skipTarget <= 15);
				CHECK(p.filterFreq >= 0.0f);
				CHECK(p.filterFreq <= 1.0f);
				// THE DELAY PROBABILITY IS A PROBABILITY, across both branches and the whole plane —
				// added with the field, so the sweep covers it as it covers every other number here.
				CHECK(p.delayProb >= 0.0f);
				CHECK(p.delayProb <= 1.0f);
				CHECK(p.envShape >= 0.0f);
				CHECK(p.envShape <= 1.0f);
				CHECK(p.envDepth >= 0.0f);
				CHECK(p.envDepth <= 1.0f);
				CHECK(p.panAmount >= 0.0f);
				CHECK(p.panAmount <= 1.0f);
				CHECK(p.stereoWidth >= -1.0f);
				CHECK(p.stereoWidth <= 1.0f);
				CHECK(p.delaySendBits <= 3u);
				// **THE GATE NEVER FULLY MUTES, AND THE FLOOR IS 0.125 RATHER THAN "ABOVE ZERO".**
				// This first asserted `> 0.0f` and a known-bad that DELETED the floor stayed green:
				// without it the gate is `1 - gateRaw`, which only reaches zero if the triangle hits
				// exactly 1.0 and so almost never trips a `> 0` test. Asserting the floor itself is
				// what makes the known-bad red.
				CHECK(p.gateRatio >= 0.125f);
				CHECK(p.gateRatio <= 1.0f);
				// A ratchet and a repeat are mutually exclusive by construction.
				if (p.repeatSlices > 1) {
					CHECK(p.subdivisions == 1);
				}
			}
		}
	}
	}
	// NON-VACUITY: the sweep has to have run BOTH branches, or every CHECK above is true of an empty
	// loop -- or, as it was, of half the function.
	CHECK(examined == 2 * 11 * 11 * 16);
}

TEST_CASE("the plan is deterministic — the same inputs give the same plan") {
	// Every decision comes from a hash of the slice index and the controls, so two calls must agree
	// exactly. If they ever do not, something is reading state it should not.
	for (int s = 0; s < 8; ++s) {
		CAPTURE(s);
		CHECK(planOf(0.42f, 0.61f, 0.3f, 0.7f, s) == planOf(0.42f, 0.61f, 0.3f, 0.7f, s));
	}
}

TEST_CASE("Zone A and Zone B drive DISJOINT halves of the plan") {
	// **THE DESIGN'S OWN CLAIM, and the thing a mis-wired phi constant breaks silently.** Zone A is
	// structural -- which slice, its offset, its length, whether it skips. Zone B is timbral --
	// reverse, filter, delay, envelope. Crossing them is not a crash and not a wrong range; it is a
	// device whose two knobs have stopped meaning what the card says.
	//
	// Zone B is held in the DISCRETE region (below the 0.3 stride deadzone) so that moving Zone A
	// cannot change the timbral half through the stride, which is a real and intended coupling.
	const SlicePlan base = planOf(0.1f, 0.2f, 0.0f, 0.0f, 5);
	const SlicePlan movedA = planOf(0.9f, 0.2f, 0.0f, 0.0f, 5);
	const SlicePlan movedB = planOf(0.1f, 0.9f, 0.0f, 0.0f, 5);

	SUBCASE("moving Zone A moves the structural half") {
		const bool structuralMoved = movedA.sliceOffset != base.sliceOffset
		                             || movedA.lengthMult != base.lengthMult
		                             || movedA.shouldSkip != base.shouldSkip
		                             || movedA.grainLength != base.grainLength
		                             || movedA.repeatSlices != base.repeatSlices;
		CHECK(structuralMoved);
	}
	SUBCASE("and leaves the timbral half alone") {
		// The discrete Zone B path reads `zoneBInfo` and the macros only -- never Zone A.
		CHECK(movedA.filterFreq == base.filterFreq);
		CHECK(movedA.envShape == base.envShape);
		CHECK(movedA.delaySendBits == base.delaySendBits);
		CHECK(movedA.panAmount == base.panAmount);
		CHECK(movedA.stereoWidth == base.stereoWidth);
	}
	SUBCASE("moving Zone B moves the timbral half") {
		const bool timbralMoved = movedB.filterFreq != base.filterFreq || movedB.envShape != base.envShape
		                          || movedB.delaySendBits != base.delaySendBits
		                          || movedB.envDepth != base.envDepth;
		CHECK(timbralMoved);
	}
}

TEST_CASE("the dead-zone cache returns what an uncached call returns") {
	// **THE ONE WAY TO CATCH A CACHE THAT IS LYING.** In the dead zone the plan does not depend on
	// the slice index, so one answer serves every slice -- but only if the guard is right. A cache
	// that returned a stale plan after a control moved would be invisible to every other case here:
	// the values would still be in range, still deterministic, still disjoint.
	//
	// Zone B at 0.2 is inside the 0.3 stride deadzone, and slice 0 has no effective slice, so this
	// is the region the cache exists for.
	PlanCache cache;
	const PhaseOffsets ofs = noOffsets();

	const SlicePlan first = planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);
	const SlicePlan cached = planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);
	CHECK(cached == first);

	// AND AGAINST A COLD CACHE, which is what proves the hit is not merely self-consistent.
	PlanCache fresh;
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &fresh) == first);

	// A CONTROL MOVES: the cache must miss rather than hand back the old plan.
	const SlicePlan afterMove = planFor(0.9f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);
	PlanCache coldForMoved;
	CHECK(afterMove == planFor(0.9f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldForMoved));

	// AND MOVING BACK RETURNS THE ORIGINAL, not whatever was cached last.
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache) == first);

	// A NULL CACHE IS LEGAL and must compute rather than crash -- the plan seam is called without one.
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, nullptr) == first);
}

TEST_CASE("the dead-zone cache keys on the OFFSETS, which the reference's does not") {
	// **A FIFTH DEFECT IN THE REFERENCE, AND THE FIRST ONE THAT THIS PORT MAKES ORDINARY.** Its guard
	// reads `zoneAParam == cachedZoneA && zoneBParam == cachedZoneB && macroConfigParam ==
	// cachedMacroConfig && macroParam == cachedMacro && ofs.gamma == cachedGamma && ofs.barIndex ==
	// cachedBarIndex` — four params, gamma, the bar. The three PHASE OFFSETS are missing, and
	// `phRawA`, `phRawB` and `macroConfigOffset` all read them.
	//
	// ON THE HARDWARE IT NEEDS A SECRET GESTURE inside both dead zones, so it is nearly unreachable.
	// HERE THE OFFSETS ARE THE WHOLE PART OF THE THREE COORDINATES — turning Zone A from 0.5 to 1.5
	// leaves the position at 0.5 and moves the lap, which under the reference's key is a knob that
	// does nothing.
	PlanCache cache;
	PhaseOffsets ofs = noOffsets();

	const SlicePlan first = planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);

	// ONE LAP ON, SAME POSITION — the gesture the reference's key cannot see.
	ofs.zoneA = 1.0f;
	PlanCache coldA;
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache)
	      == planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldA));
	// AND IT IS GENUINELY A DIFFERENT PLAN, or the line above would hold for a cache that never missed.
	CHECK_FALSE(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldA) == first);

	// ZONE B'S LAP, the same way.
	ofs = noOffsets();
	ofs.zoneB = 1.0f;
	PlanCache coldB;
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache)
	      == planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldB));

	// AND MACRO CFG'S, which is the one that only became reachable when its phase param was folded
	// into the coordinate.
	ofs = noOffsets();
	ofs.macroConfig = 2.0f; // two laps: +0.2 on the macro config sum
	PlanCache coldC;
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache)
	      == planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldC));
	CHECK_FALSE(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &coldC) == first);
}

TEST_CASE("outside the dead zone the plan DOES depend on the slice index") {
	// The other side of the cache's premise. With Zone B past the stride deadzone the stride is
	// non-zero, so consecutive slices hash differently -- if they did not, the cache's guard would be
	// wrong about when it is safe and every slice in a bar would be identical.
	std::set<int32_t> offsets;
	for (int s = 0; s < 16; ++s) {
		offsets.insert(planOf(0.8f, 0.9f, 0.2f, 0.2f, s).sliceOffset);
	}
	CHECK(offsets.size() > 1u);
}

TEST_CASE("the macro's off-zero guard covers the PROBABILITIES and nothing else") {
	// The reference guards three probability adjustments with `macroNorm > 0.01f`.
	//
	// **IT DOES NOT MAKE A SMALL MACRO INERT, AND ASSERTING THAT WAS WRONG.** A first draft compared
	// the WHOLE plan below and above the guard and found 2,856 of 3,528 differing -- because
	// the macro is read UNGATED elsewhere: the discrete path's gate pull and the ratchet's subdivision
	// mix, which are what differed here; the dry vote, which is ungated too but cannot pass its 0.5 at
	// a macro of 0.005; and the two zone phase pushes in `offsetsFor`, which this case does not reach
	// because it hands `planFor` its offsets directly. All five are `MacroPush` terms now.
	// So a macro of 0.005 does change the sound; what the guard decides is
	// only whether it also nudges reverse, pitch and delay. The claim is now the narrow true one.
	// **THE THREE SCALES ARE NO LONGER SET FROM OUT HERE, and that is the point of the change that
	// removed them.** They were fields on `PhaseOffsets` that only this case ever wrote: nothing in
	// the device assigned them, so the macro's influence on reverse, pitch and delay probability was
	// inert in the plugin while this case handed itself a 1.0 and proved the guard over numbers the
	// device never produced. They are derived inside `macroPushOf` now, from the same sum `planFor` publishes, so the
	// sweep below exercises whatever the macro config actually yields — which is the arrangement the
	// guard has to be right about.
	const PhaseOffsets ofs = noOffsets();

	// **SWEPT, NOT SAMPLED.** One slice at one zone pair was not enough: a known-bad that deleted the
	// guard left that single comparison green, because a 0.25% shift in a probability rarely flips
	// one hash decision. MEASURED over the sweep below it flips plenty -- 14,112 plans digest
	// differently with the guard gone -- so the claim has to be made over the plane, not at a point.
	int examined = 0;
	int differing = 0;
	for (int a = 0; a <= 20; ++a) {
		for (int b = 0; b <= 20; ++b) {
			for (int s = 0; s < 8; ++s) {
				const float zoneA = static_cast<float>(a) / 20.0f;
				const float zoneB = static_cast<float>(b) / 20.0f;
				PlanCache c1;
				PlanCache c2;
				const SlicePlan atZero = planFor(zoneA, zoneB, 0.3f, 0.0f, s, ofs, &c1);
				const SlicePlan atTiny = planFor(zoneA, zoneB, 0.3f, 0.005f, s, ofs, &c2);
				const bool probabilitiesMoved = atTiny.shouldReverse != atZero.shouldReverse
				                                || atTiny.shouldPitchUp != atZero.shouldPitchUp
				                                || atTiny.shouldDelay != atZero.shouldDelay;
				if (probabilitiesMoved) {
					++differing;
					CAPTURE(zoneA);
					CAPTURE(zoneB);
					CAPTURE(s);
				}
				++examined;
			}
		}
	}
	CHECK(examined == 21 * 21 * 8);
	CHECK(differing == 0); // below the guard the three probability decisions never move
}

TEST_CASE("the send override is a lerp to an endpoint — and the endpoints cannot witness it") {
	// **READ THIS BEFORE WRITING ANOTHER CASE FOR THIS KNOB.** The obvious way to test a control whose
	// promise is "exactly 1" and "exactly 0" is to drive the PROBABILITY to its extremes and check the
	// two ends. That case passes with `overriddenProbability` deleted and `delayProb` left alone:
	//
	//     p = 1, k = +1 -> 1        and with no override at all -> 1
	//     p = 0, k = -1 -> 0        and with no override at all -> 0
	//
	// The error is ZERO AT BOTH BOUNDARIES BY CONSTRUCTION, so boundary testing here is not merely weak,
	// it is blind by design — a different axis from CLAUDE.md's "partial effect surviving where a total
	// one would be caught", and worth naming separately for that reason. **The witness is an
	// INTERMEDIATE `p` at an INTERMEDIATE `k`**, which is the block directly below; the endpoint
	// assertions after it pin "exactly 1.0" and "exactly 0.0" and are not the thing that catches a
	// deletion.
	//
	// **AND THE EXTREMES OF `k` ARE BLIND TOO, WHICH IS A SECOND AXIS AND WAS FOUND BY A KNOWN-BAD
	// RATHER THAN BY READING.** A mutation that made the override JOIN the bias — `clamp(p + k, 0, 1)`
	// in place of the lerp, which is precisely the arrangement backlog EW says a dedicated control has
	// to escape — reddened ONLY the two intermediate cases, this one and the plan's. At `k = ±1` a
	// clamped SUM also reaches 1 and 0, so an override and a bias are indistinguishable there for every
	// `p`. **So two independent blindnesses meet at exactly the four corners a normal test reaches
	// for**: the ends of `p` cannot see the feature at all, and the ends of `k` cannot tell it from the
	// thing it was built to replace. Nothing but an interior point separates them.

	// THE WITNESS. A half-strength override on a half-way probability, computed by hand rather than by
	// calling the function under test: half way from 0.5 to 1 is 0.75, and half way from 0.5 to 0 is
	// 0.25. Deleting the override makes all four of these read 0.5.
	CHECK(overriddenProbability(0.5f, 0.5f) == doctest::Approx(0.75f));
	CHECK(overriddenProbability(0.5f, -0.5f) == doctest::Approx(0.25f));
	CHECK(overriddenProbability(0.25f, 0.5f) == doctest::Approx(0.625f)); // 0.25 + 0.5 * 0.75
	CHECK(overriddenProbability(0.8f, -0.25f) == doctest::Approx(0.6f));  // 0.8 * 0.75

	// **AND THE IDENTITY IS EXACT, NOT APPROXIMATE** — `==` on the float, no epsilon, because a knob at
	// rest must leave phi's own value untouched bit for bit rather than nearly so. A mutation that
	// writes `p * (1 + k) * (1 + 1e-7f)` would pass an `Approx` here and change every decision.
	for (int i = 0; i <= 64; ++i) {
		const float p = static_cast<float>(i) / 64.0f;
		CHECK(overriddenProbability(p, 0.0f) == p);
		CHECK(overriddenProbability(p, -0.0f) == p); // the branch is `>= 0`, so negative zero lands left
	}

	// THE ENDPOINTS. Exactly one and exactly zero for every probability, which is the promise the
	// design makes — as high as 1.0 at full strength, or tied to zero — and they are `==` for
	// the same reason: "0.99999994 of the time" is not what a performer asked for.
	for (int i = 0; i <= 64; ++i) {
		const float p = static_cast<float>(i) / 64.0f;
		CHECK(overriddenProbability(p, 1.0f) == 1.0f);
		CHECK(overriddenProbability(p, -1.0f) == 0.0f);
	}

	// MONOTONIC IN `k`, AND CONTINUOUS ACROSS ZERO — the two branches meet there, so a hand sweeping the
	// knob through the middle hears no step. A first draft that wrote the negative half as `p + k * p`
	// is the same function; one that wrote it as `p - k` is not, and fails here at the crossing.
	for (const float p : { 0.0f, 0.2f, 0.5f, 0.9f, 1.0f }) {
		CAPTURE(p);
		float previous = -1.0f;
		for (int i = -200; i <= 200; ++i) {
			const float v = overriddenProbability(p, static_cast<float>(i) / 200.0f);
			CHECK(v >= previous);
			CHECK(v >= 0.0f);
			CHECK(v <= 1.0f);
			previous = v;
		}
	}

	// **CLAMPED, BECAUSE IT IS A MODULATION DESTINATION.** A route can hand this more than the declared
	// range; past the end it saturates rather than running the lerp out the far side (at k = 2 an
	// unclamped `p + k * (1 - p)` gives 1.5, which `evalBool` would take as "always" by accident and
	// `p * (1 + k)` at k = -2 gives a NEGATIVE probability).
	CHECK(overriddenProbability(0.5f, 4.0f) == 1.0f);
	CHECK(overriddenProbability(0.5f, -4.0f) == 0.0f);
}

TEST_CASE("the override reaches the PLAN's delay probability, at an intermediate value") {
	// The case above is about the arithmetic; this is about the WIRING — that `planFor` applies it, to
	// the delay probability, AFTER the push and the clamp, and to nothing else.
	//
	// **THE SAME BLINDNESS APPLIES AND IS AVOIDED THE SAME WAY.** The assertion that carries this is at
	// an intermediate `k` on a probability that is strictly between 0 and 1, and the REQUIREs below
	// refuse to let the case pass vacuously if a future tuning change moves that probability onto an
	// endpoint — at which point this case would silently become one that a deleted override passes.
	const PhaseOffsets base = noOffsets();
	PlanCache c0;
	const SlicePlan plain = planFor(0.35f, 0.75f, 0.0f, 0.0f, 3, base, &c0);
	REQUIRE(plain.delayProb > 0.0f); // not vacuous: the override has somewhere to move it FROM,
	REQUIRE(plain.delayProb < 1.0f); // in both directions

	// **RESTATED HERE RATHER THAN ASKED OF `overriddenProbability`.** Calling the function under test to
	// compute the expected value would assert only that `planFor` calls it, which is not the claim.
	for (const float k : { -0.75f, -0.5f, -0.25f, 0.25f, 0.5f, 0.75f }) {
		CAPTURE(k);
		PhaseOffsets ofs = base;
		ofs.sendOverride = k;
		PlanCache cache;
		const SlicePlan p = planFor(0.35f, 0.75f, 0.0f, 0.0f, 3, ofs, &cache);
		const float want = k >= 0.0f ? plain.delayProb + k * (1.0f - plain.delayProb)
		                             : plain.delayProb * (1.0f + k);
		CHECK(p.delayProb == doctest::Approx(want));
		// NOT VACUOUS, the second time: every one of these differs from the un-overridden value, so the
		// line above is comparing two different numbers rather than agreeing with itself.
		CHECK(p.delayProb != doctest::Approx(plain.delayProb));

		// **AND NOTHING ELSE IN THE PLAN MOVED — except the GRAIN LANE, which is supposed to.** The
		// override is the SEND's, and since backlog EW there are two destinations drawn against this one
		// probability: the internal echo and the grain lane. So `shouldSendToLane` is normalised away
		// here beside `shouldDelay`, and asserted separately below rather than silently forgiven — a
		// field excused from an equality check without a statement of its own is how a coupling stops
		// being tested.
		SlicePlan asPlain = p;
		asPlain.delayProb = plain.delayProb;
		asPlain.shouldDelay = plain.shouldDelay;
		asPlain.shouldSendToLane = plain.shouldSendToLane;
		CHECK(asPlain == plain);
	}

	// THE ENDPOINTS THROUGH THE PLAN, which is where the decision itself becomes deterministic: a
	// threshold of exactly 0 makes `evalBoolFast`'s `hash < 0` unsatisfiable, so -1 is literally never.
	PhaseOffsets never = base;
	never.sendOverride = -1.0f;
	PhaseOffsets always = base;
	always.sendOverride = 1.0f;
	for (int slice = 0; slice < 64; ++slice) {
		CAPTURE(slice);
		PlanCache cn;
		PlanCache ca;
		const SlicePlan off = planFor(0.35f, 0.75f, 0.0f, 0.0f, slice, never, &cn);
		const SlicePlan on = planFor(0.35f, 0.75f, 0.0f, 0.0f, slice, always, &ca);
		CHECK(off.delayProb == 0.0f);
		CHECK_FALSE(off.shouldDelay);
		CHECK(on.delayProb == 1.0f);
		// **AND THE GRAIN LANE OBEYS THE SAME OVERRIDE, WHICH IS WHAT MAKES IT ONE CONTROL** (backlog
		// EW). Force-down ties the lane's draw to zero exactly as it does the echo's, because both are
		// drawn against this one probability. Asserted at the bottom end only, for the reason the note
		// below gives about the top.
		CHECK_FALSE(off.shouldSendToLane);
		// **`shouldDelay` IS NOT ASSERTED AT THE TOP END, AND THAT IS NOT AN OVERSIGHT.**
		// `Context::toU32Threshold(1.0f)` is 0xFFFFFFFF and the comparison is `<`, so the single hash
		// value 0xFFFFFFFF declines — one slice in 2^32. The PROBABILITY is the claim; see `planFor`.
	}

	// **THE TWO DESTINATIONS ARE INDEPENDENT EVENTS AT THE SAME DENSITY** (backlog EW, and it is the
	// reason `HashSeed::GrainLaneDecision` is its own salt rather than a reuse of `DelayDecision`).
	// Sharing the draw would make every lane grain an echo grain, so the two effects could never be
	// heard apart; the claim is that over a population they DISAGREE sometimes while both firing at a
	// rate the probability explains. A count, not a coin: one slice proves nothing either way.
	{
		PhaseOffsets ofs = base;
		int lane = 0;
		int echo = 0;
		int disagreed = 0;
		constexpr int kSlices = 512;
		for (int slice = 0; slice < kSlices; ++slice) {
			PlanCache cache;
			const SlicePlan p = planFor(0.35f, 0.75f, 0.0f, 0.0f, slice, ofs, &cache);
			lane += p.shouldSendToLane ? 1 : 0;
			echo += p.shouldDelay ? 1 : 0;
			disagreed += (p.shouldSendToLane != p.shouldDelay) ? 1 : 0;
		}
		CAPTURE(lane);
		CAPTURE(echo);
		CAPTURE(disagreed);
		// BOTH FIRE, so neither draw is stuck; and they differ often, so they are not one draw wearing
		// two names. A shared seed would make `disagreed` exactly zero.
		CHECK(lane > 0);
		CHECK(echo > 0);
		CHECK(disagreed > kSlices / 10);
	}
}

TEST_CASE("the dead-zone cache keys on the send override") {
	// **THE CACHE HANDS BACK A WHOLE PLAN, `delayProb` INCLUDED.** The reference's key was short by
	// three offsets and the note in `slice_plan.h` calls that its fifth defect; a field added after
	// that fix and left out of the key is the same defect, newly made. In the dead zone (no stride, no
	// effective slice) one answer serves every slice, so this is exactly where a stale one survives.
	PhaseOffsets ofs = noOffsets();
	PlanCache cache;
	const SlicePlan first = planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);
	REQUIRE(cache.valid); // the dead zone was actually entered, or this case proves nothing
	REQUIRE(first.delayProb > 0.0f);
	REQUIRE(first.delayProb < 1.0f);

	ofs.sendOverride = 1.0f;
	const SlicePlan forced = planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cache);
	CHECK(forced.delayProb == 1.0f); // a hit here would have returned `first`

	// AND A COLD CACHE AGREES WITH THE WARM ONE, which is what says the entry was replaced rather than
	// merely bypassed.
	PlanCache cold;
	CHECK(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ofs, &cold) == forced);
}

TEST_CASE("the stride deadzone is where Zone B stops varying the slices") {
	// **WHAT THE 0.3 DEADZONE IS FOR.** Below it the stride is zero, so every slice hashes from the
	// same seed and a bar is uniform; above it the stride grows and consecutive slices diverge. That
	// boundary is the difference between "a steady stutter" and "a scattered one", and it is a tuning
	// decision -- there is no way to re-derive 0.3, so it is pinned.
	//
	// A known-bad moving it to 0.1 was green against every other case here: the ranges stay valid and
	// the timbral fields come from the zone rather than the hash, so nothing else could see it.
	const auto sliceOffsetsAcross = [](float zoneB) {
		std::set<int32_t> seen;
		for (int s = 0; s < 32; ++s) {
			seen.insert(planOf(0.8f, zoneB, 0.2f, 0.2f, s).sliceOffset);
		}
		return seen.size();
	};

	// INSIDE the deadzone: one answer for the whole bar.
	CHECK(sliceOffsetsAcross(0.10f) == 1u);
	CHECK(sliceOffsetsAcross(0.20f) == 1u);
	CHECK(sliceOffsetsAcross(0.29f) == 1u);
	// OUTSIDE it: the slices diverge.
	CHECK(sliceOffsetsAcross(0.50f) > 1u);
	CHECK(sliceOffsetsAcross(0.90f) > 1u);
}

//--------------------------------------------------------------------------------------------------//
// THE SLICE SCHEDULE — and here the expected values ARE transcribable.                               //
//--------------------------------------------------------------------------------------------------//
//
// Unlike the plan above, the patterns come with their intended sequences written out in the
// reference's own comments: "Weave: 0,N-1,1,N-2,2,N-3...", "Skip: evens then odds (0,2,4,6,1,3,5,7)",
// and so on. Those are an INDEPENDENT STATEMENT OF INTENT rather than the code's own output, so
// asserting them is a real check and not a tautology -- if the arithmetic disagrees with the comment,
// one of the two is wrong and this says so.

namespace {

std::vector<int32_t> orderUnder(SlicePattern pattern, int32_t n) {
	std::vector<int32_t> out;
	out.reserve(static_cast<size_t>(n));
	for (int32_t i = 0; i < n; ++i) {
		out.push_back(reorderFor(pattern, i, n));
	}
	return out;
}

} // namespace

TEST_CASE("each pattern plays the order its own comment promises") {
	using V = std::vector<int32_t>;
	CHECK(orderUnder(SlicePattern::Sequential, 8) == V { 0, 1, 2, 3, 4, 5, 6, 7 });
	CHECK(orderUnder(SlicePattern::Weave, 8) == V { 0, 7, 1, 6, 2, 5, 3, 4 });
	CHECK(orderUnder(SlicePattern::Skip, 8) == V { 0, 2, 4, 6, 1, 3, 5, 7 });
	CHECK(orderUnder(SlicePattern::Mirror, 8) == V { 0, 1, 2, 3, 3, 2, 1, 0 });
	CHECK(orderUnder(SlicePattern::Pairs, 8) == V { 1, 0, 3, 2, 5, 4, 7, 6 });
	CHECK(orderUnder(SlicePattern::Reverse, 8) == V { 7, 6, 5, 4, 3, 2, 1, 0 });
	CHECK(orderUnder(SlicePattern::Thirds, 8) == V { 0, 3, 6, 1, 4, 7, 2, 5 });
	// **SPIRAL IS THE ONE WHERE THE CODE AND ITS OWN COMMENT DISAGREE, AND THE CODE WINS HERE.**
	// The reference's comment promises "middle outward (3,4,2,5,1,6,0,7)" -- a proper permutation. Its
	// arithmetic is `mid = n/2` with `offset = (i+1)/2`, which walks 4,5,3,6,2,7,1 and then computes
	// 8 for the last step and CLAMPS it to 7. So at eight slices **slice 0 never plays and slice 7
	// plays twice**. The clamp's presence suggests the overshoot was known; the comment was not
	// updated to match.
	//
	// PORTED AS WRITTEN, because parity is the goal and this is audible character rather than
	// undefined behaviour -- unlike `delaySendBits`, where the divergence was UB and had to be fixed.
	// Recorded here so the choice is visible and so a future "fix" toward the comment is a decision.
	CHECK(orderUnder(SlicePattern::Spiral, 8) == V { 4, 5, 3, 6, 2, 7, 1, 7 });
	// The comment's sequence, for the record, is NOT what runs:
	CHECK(orderUnder(SlicePattern::Spiral, 8) != V { 3, 4, 2, 5, 1, 6, 0, 7 });
}

TEST_CASE("no pattern ever leaves the bar, at any slice count") {
	// **THE PROPERTY THAT MATTERS MORE THAN THE ORDERS.** An index off the end reads a slice that was
	// never captured -- silence dressed as audio, or a read past the ring. Thirds and Spiral both
	// compute indices that can overshoot and both clamp; this is what says the clamps are there.
	for (const int32_t n : { 1, 2, 4, 8, 16, 32 }) {
		for (int p = 0; p < static_cast<int>(SlicePattern::kCount); ++p) {
			for (int32_t i = 0; i < n; ++i) {
				CAPTURE(n);
				CAPTURE(p);
				CAPTURE(i);
				const int32_t at = reorderFor(static_cast<SlicePattern>(p), i, n);
				CHECK(at >= 0);
				CHECK(at < n);
			}
		}
	}
}

TEST_CASE("a permutation is a permutation — every slice played exactly once") {
	// Sequential, Weave, Skip, Pairs, Reverse and Thirds each visit the whole bar. Mirror and Spiral
	// deliberately do NOT (Mirror folds, Spiral can repeat an end), so they are named rather than
	// swept -- which is the same discipline as the exemption sets in `test_fx_devices.cpp`.
	// Spiral is excluded for a MEASURED reason rather than a suspected one: it clamps its last step
	// onto a slice it has already played, so slice 0 never sounds. See the pattern case above.
	for (const SlicePattern p : { SlicePattern::Sequential, SlicePattern::Weave, SlicePattern::Skip,
	                              SlicePattern::Pairs, SlicePattern::Reverse }) {
		CAPTURE(static_cast<int>(p));
		std::vector<int32_t> order = orderUnder(p, 16);
		std::sort(order.begin(), order.end());
		std::vector<int32_t> expected(16);
		for (int32_t i = 0; i < 16; ++i) {
			expected[static_cast<size_t>(i)] = i;
		}
		CHECK(order == expected);
	}
}

TEST_CASE("Zone A picks the pattern in eight steps across one lap") {
	CHECK(patternFor(0.00f) == SlicePattern::Sequential);
	CHECK(patternFor(0.20f) == SlicePattern::Weave);
	CHECK(patternFor(0.30f) == SlicePattern::Skip);
	CHECK(patternFor(0.99f) == SlicePattern::Spiral);
	// AND 1.0 IS SPIRAL, NOT ONE PAST IT. A coordinate legitimately reaches its top.
	CHECK(patternFor(1.00f) == SlicePattern::Spiral);
	CHECK(patternFor(5.00f) == SlicePattern::Spiral); // clamped, not wrapped
}

TEST_CASE("the schedule places a slice at a frame, asserted as numbers") {
	// A plain plan: no offset, no skip, one slice per grain.
	SlicePlan plan;
	plan.sliceOffset = 0;
	plan.shouldSkip = false;
	plan.grainLength = 1;
	plan.repeatSlices = 1;

	constexpr size_t kBar = 96000;
	constexpr int32_t kSlices = 8;
	const size_t each = kBar / kSlices; // 12000

	for (int32_t i = 0; i < kSlices; ++i) {
		CAPTURE(i);
		const SliceSchedule s = scheduleFor(Mode::Shuffle, 0.0f, i, kSlices, kBar, plan);
		CHECK(s.targetSlice == i);
		CHECK(s.startOffset == static_cast<size_t>(i) * each);
		CHECK(s.grainLength == 1);
		CHECK(s.sliceLength == each); // 96000/8 divides exactly, so no remainder anywhere
	}
}

TEST_CASE("the last grain of the bar swallows the remainder, so the loop does not rush") {
	// **INTEGER DIVISION LOSES UP TO numSlices-1 FRAMES PER BAR.** Dropping them shortens the loop
	// every time round, which drifts against the transport -- audible as a stutter that slowly runs
	// ahead of the beat. 100000/8 is 12500 exactly; 100001/8 is not.
	SlicePlan plan;
	constexpr size_t kBar = 100003; // deliberately prime-ish: 100003 = 8*12500 + 3
	constexpr int32_t kSlices = 8;
	size_t total = 0;
	for (int32_t i = 0; i < kSlices; ++i) {
		total += scheduleFor(Mode::Shuffle, 0.0f, i, kSlices, kBar, plan).sliceLength;
	}
	CHECK(total == kBar); // every frame of the bar accounted for, none twice

	const SliceSchedule last = scheduleFor(Mode::Shuffle, 0.0f, kSlices - 1, kSlices, kBar, plan);
	CHECK(last.sliceLength == 12500u + 3u);
}

TEST_CASE("a long grain is clamped by what is left of the bar, in both directions") {
	SlicePlan plan;
	plan.grainLength = 8;
	constexpr size_t kBar = 96000;
	constexpr int32_t kSlices = 8;

	// At step 6 only two steps remain, so an 8-slice grain becomes 2.
	CHECK(scheduleFor(Mode::Shuffle, 0.0f, 6, kSlices, kBar, plan).grainLength == 2);
	// At step 7, one.
	CHECK(scheduleFor(Mode::Shuffle, 0.0f, 7, kSlices, kBar, plan).grainLength == 1);
	// And a target near the end of the BUFFER clamps too, independently of the step.
	SlicePlan skipping;
	skipping.grainLength = 8;
	skipping.shouldSkip = true;
	skipping.skipTarget = 15; // -> slice 7
	const SliceSchedule s = scheduleFor(Mode::Shuffle, 0.0f, 0, kSlices, kBar, skipping);
	CHECK(s.targetSlice == 7);
	CHECK(s.grainLength == 1); // only one slice remains in the buffer from there
}

TEST_CASE("a skip REPLACES the target rather than nudging it") {
	// The difference between a jump to somewhere non-adjacent and a larger offset. A skip that added
	// would still be a walk; the reference assigns.
	SlicePlan plan;
	plan.sliceOffset = 4; // would move the target by (4*8)>>4 == 2
	plan.shouldSkip = true;
	plan.skipTarget = 8; // -> (8*8)>>4 == 4
	const SliceSchedule s = scheduleFor(Mode::Shuffle, 0.0f, 0, 8, 96000, plan);
	CHECK(s.targetSlice == 4); // not 0 + 2 + 4
}

TEST_CASE("Time mode holds one source slice for repeatSlices steps") {
	// The stretch: 1111,2222,3333 rather than 1234. Only Time does this -- Shuffle with the same plan
	// walks normally, which is what makes it a MODE difference rather than a plan one.
	SlicePlan plan;
	plan.repeatSlices = 4;
	std::vector<int32_t> timeOrder;
	std::vector<int32_t> shuffleOrder;
	for (int32_t i = 0; i < 8; ++i) {
		timeOrder.push_back(scheduleFor(Mode::Time, 0.0f, i, 8, 96000, plan).targetSlice);
		shuffleOrder.push_back(scheduleFor(Mode::Shuffle, 0.0f, i, 8, 96000, plan).targetSlice);
	}
	CHECK(timeOrder == std::vector<int32_t> { 0, 0, 0, 0, 1, 1, 1, 1 });
	CHECK(shuffleOrder == std::vector<int32_t> { 0, 1, 2, 3, 4, 5, 6, 7 });
}

TEST_CASE("a degenerate bar is inert rather than a divide by zero") {
	SlicePlan plan;
	CHECK(scheduleFor(Mode::Shuffle, 0.0f, 0, 0, 96000, plan) == SliceSchedule {});
	CHECK(scheduleFor(Mode::Shuffle, 0.0f, 0, 8, 0, plan) == SliceSchedule {});
	// One slice is the whole bar and every step plays it.
	const SliceSchedule whole = scheduleFor(Mode::Shuffle, 0.0f, 0, 1, 96000, plan);
	CHECK(whole.targetSlice == 0);
	CHECK(whole.sliceLength == 96000u);
}

TEST_CASE("gamma neither saturates nor wraps — the phi triangles wrap it, each at its own period") {
	// **BY DESIGN** gamma never saturates or wraps; if it does, that is a bug. Gamma drives phase
	// offsets, which drive the phi triangles periodically.
	//
	// Both earlier versions of this line were wrong, in opposite directions. The reference CLAMPS —
	// `clamp(macroConfigNorm + macroConfigOffset, 0, 1)` — which MEASURED pins at a gamma of about 0.7
	// against a Macro Cfg of 0.3 and freezes the three decisions downstream. I then WRAPPED it, which
	// is worse in a way that is harder to hear: every one of those three readers is
	// `triangleSimpleUnipolar(macroConfigNorm * <a DIFFERENT phi constant>)`, so wrapping before the
	// multiply forces all three to share a period of 1 in gamma. Raw, they wrap at 1/kPhi, 1/kPhi100
	// and 1/kPhi225 — incommensurate, so they never come back into register. That non-repetition is
	// what a phi bank is FOR.
	const auto normAt = [](float gamma, float macroConfig = 0.3f) {
		PhaseOffsets ofs = noOffsets();
		ofs.gamma = gamma;
		PlanCache cache;
		return planFor(0.5f, 0.2f, macroConfig, 0.4f, 0, ofs, &cache).macroConfigNorm;
	};

	// **LINEAR, AND WITHOUT END.** No clamp at 1.0, no wrap to 0. The value is the phase itself.
	CHECK(normAt(0.0f) == doctest::Approx(0.3));
	CHECK(normAt(0.5f) == doctest::Approx(0.8));
	CHECK(normAt(0.7f) == doctest::Approx(1.0));   // the reference's clamp began here
	CHECK(normAt(10.5f) == doctest::Approx(10.8)); // and an early wrap would have read 0.8
	CHECK(normAt(1000.0f) == doctest::Approx(1000.3));

	SUBCASE("and one full unit of gamma is NOT a repeat, which is what an early wrap would make it") {
		// Under `wrapPhase(macroConfig + gamma)` these two would be the same plan, because the wrap
		// gives the three triangles a shared period of 1. Raw, they disagree.
		PlanCache a;
		PlanCache b;
		PhaseOffsets oa = noOffsets();
		PhaseOffsets ob = noOffsets();
		oa.gamma = 0.25f;
		ob.gamma = 1.25f;
		CHECK_FALSE(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, oa, &a)
		            == planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, ob, &b));

		// AND STILL NOT AT TEN UNITS, which a period of 1 would also have made identical.
		PlanCache c;
		PhaseOffsets oc = noOffsets();
		oc.gamma = 10.25f;
		CHECK_FALSE(planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, oa, &a)
		            == planFor(0.5f, 0.2f, 0.3f, 0.4f, 0, oc, &c));
	}

	SUBCASE("the phase survives to the wrap in DOUBLE, or large gamma saturates by precision") {
		// `triangleSimpleUnipolar` wraps its own input with `floorf` — on a FLOAT — so a phase carried
		// as a float loses its fraction long before it is wrapped. **MEASURED at the primitive, 64
		// finely-spaced phases near 2^23: double yields 40 distinct triangle outputs and float yields
		// ONE.** That is saturation by another name, and 2^23 is not a corner — it is the top of the
		// coordinate's declared range. `PhiTriContext::gammaPhase` is a `double` in the reference
		// *"for precision at large values"*; the port had the value and not the type.
		//
		// **ASSERTED WHERE THE FRACTION IS VISIBLE.** `macroConfigNorm` is the phase itself, so the
		// claim is simply that 0.3 is still on the end of it — which a float at this magnitude (ulp
		// 0.5) cannot represent at all.
		CHECK(normAt(8000000.0f) == doctest::Approx(8000000.3).epsilon(1e-12));

		// **AND THROUGH THE TRIANGLES, using the one plan field that is computed from a phase and
		// nothing else.** `filterFreq` is a chain of `wrapPhase(phRawB * <phi>)` terms — it is dead in
		// the reference too and is never consumed, which is exactly what makes it a clean probe: no
		// hash reaches it, so it cannot differ for an unrelated reason the way a whole plan can.
		PlanCache e;
		PlanCache f;
		PhaseOffsets oe = noOffsets();
		PhaseOffsets of = noOffsets();
		oe.zoneB = 8000000.0f;
		of.zoneB = 8000001.0f;
		const float freqA = planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, oe, &e).filterFreq;
		const float freqB = planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, of, &f).filterFreq;
		CAPTURE(freqA);
		CAPTURE(freqB);
		CHECK(freqA != doctest::Approx(freqB));
	}

	SUBCASE("and the hash seed's cast is defined at a phase the knobs can reach") {
		// `static_cast<uint32_t>(phRawA * 65536.0f)` is undefined once the product passes 2^32, which
		// `phRawA = zoneALap + 1024 * gamma` does at a gamma of about 65535 — MEASURED at 6.7e9
		// against a 4.29e9 ceiling by gamma 100. That is a separate defect from the clamp and both
		// were in the way of the same control.
		PlanCache a;
		PlanCache b;
		PhaseOffsets oa = noOffsets();
		PhaseOffsets ob = noOffsets();
		oa.gamma = 100000.0f;
		ob.gamma = 100001.0f;
		CHECK_FALSE(planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, oa, &a)
		            == planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, ob, &b));

		// **INSIDE THE DECLARED RANGE, WHICH IS ALSO WHERE FLOATS ARE STILL EXACT.** A first draft
		// used 1e8 and 1e8+1 and they compared equal — at 1e8 a float's ulp is 8, so those are the
		// SAME NUMBER and the case was asserting that two identical inputs differ. The coordinate is
		// declared to 2^23, exactly the last integer a float counts by ones.
		PlanCache c;
		PlanCache d;
		PhaseOffsets oc = noOffsets();
		PhaseOffsets od = noOffsets();
		oc.zoneA = 8000000.0f;
		od.zoneA = 8000001.0f;
		CHECK_FALSE(planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, oc, &c)
		            == planFor(0.5f, 0.8f, 0.3f, 0.4f, 3, od, &d));
	}
}
