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

#include "mod/mseg/mseg.h"
#include "mod/mseg/mseg_geometry.h"

// The OTHER devices, for the cross-device traits case. Headers only — kTraits is constexpr, so this
// costs no link dependency and the case can see every device without a target that pulls all three
// libraries in.
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace feathers::mod;
namespace mseg = feathers::mod::mseg;
namespace device = feathers::device;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;

/// A shape with the segments a case asks for and nothing else in the way: no smoothing, no loop, no
/// sustain point, and a start level of zero. The same idea as the DAHDSR suite's bare().
mseg::Settings bare(std::vector<std::pair<double, float>> segments) {
	mseg::Settings s;
	s.startLevel = 0.0f;
	s.count = segments.size();
	s.sustainSegment = mseg::kNoSustain;
	s.mode = Mode::Gate;
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	for (size_t i = 0; i < segments.size() && i < mseg::kMaxSegments; ++i) {
		s.segment[i].share = timeValueFor(segments[i].first);
		s.segment[i].level = segments[i].second;
		s.segment[i].shape = 0.0f;
	}
	// AND THE WINDOW IS THE SUM OF WHAT WAS ASKED FOR, which is what keeps every case in this file
	// meaning what it meant before segments became SHARES of a window.
	//
	// A share is a fraction of the window and the engine spends `share_i / sum(shares) * W` on each
	// segment. So setting W to the sum makes segment i take exactly the number of seconds the caller
	// named — the old absolute-duration behaviour, reproduced exactly, from the new arithmetic. Cases
	// about steps, loops, sustain and depth are about the SHAPE and should not have to care that the
	// time base exists; cases that ARE about the window say so and set it themselves.
	double sum = 0.0;
	for (const auto& seg : segments) {
		sum += seg.first;
	}
	s.timeScale = timeValueFor(sum > 0.0 ? sum : 1.0);

	// SANITISED, so the fixture hands back settings whose invariants already hold. Without this the
	// default loopEnd of 2 survives onto a two-segment shape, and a round-trip case comparing against
	// the fixture fails on a field the fixture never meant to set — which is the fixture being wrong
	// rather than the round trip.
	mseg::sanitize(&s);
	return s;
}

/// Run a shape for `seconds` with the gate held, returning one reading per control tick.
std::vector<float> run(mseg::Shape& shape, const mseg::Settings& s, double seconds, bool gate = true) {
	std::vector<float> out;
	const int ticks = static_cast<int>(seconds / kTickSeconds);
	out.reserve(static_cast<size_t>(ticks));
	for (int i = 0; i < ticks; ++i) {
		out.push_back(shape.tick(s, gate, false, feathers::mod::TransportClock::unknown()));
	}
	return out;
}

} // namespace

TEST_CASE("the playhead says which segment is running and how far through it") {
	// **THE TRANSLATION, ASSERTED AGAINST THE THING IT TRANSLATES.** `playhead()` is a second way to
	// read `position()`, and two ways to read one fact is exactly the redundancy this project checks
	// rather than trusts — so every reading is compared with the Position it came from, on the same
	// tick, rather than with a remembered expectation.
	mseg::Shape shape;
	shape.prepare(kSampleRate);
	// THREE SEGMENTS OF DIFFERENT LENGTHS, so "which segment" is a real question: with equal segments
	// an off-by-one in the ordinal lands at the same phase and the case would agree with it.
	const mseg::Settings s = bare({ { 0.05, 1.0f }, { 0.20, 0.25f }, { 0.10, 0.75f } });

	// A FRESH SHAPE HAS NOT STARTED, so it is IDLE rather than at segment 0 phase 0. Those are
	// different claims and only one of them is true of a shape nobody has gated.
	CHECK_FALSE(shape.playhead().running());
	CHECK(shape.playhead().ordinal == feathers::mod::Playhead::kIdle);

	size_t sawSegments = 0;
	size_t lastOrdinal = feathers::mod::Playhead::kIdle;
	// **RUN PAST THE END RATHER THAN UP TO IT.** MEASURED: this shape finishes at 0.3493 s, 262
	// ticks — and the first draft ran exactly 0.35 s, which is 262 ticks, so it stopped ON the tick
	// that finishes and the idle check below was a coin toss on a rounding. A case that ends exactly
	// at the boundary it is about is testing the boundary's arithmetic, not the thing it meant to.
	const int ticks = static_cast<int>(0.40 / kTickSeconds);
	for (int i = 0; i < ticks; ++i) {
		(void)shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
		const mseg::Position pos = shape.position();
		const feathers::mod::Playhead head = shape.playhead();
		CAPTURE(i);
		if (pos.finished || pos.segment >= mseg::kMaxSegments) {
			CHECK_FALSE(head.running());
			continue;
		}
		// THE VALUE, both halves of it.
		CHECK(head.running());
		CHECK(head.ordinal == static_cast<uint32_t>(pos.segment));
		CHECK(head.phase == pos.phase);
		CHECK(head.phase >= 0.0f);
		CHECK(head.phase <= 1.0f);
		if (head.ordinal != lastOrdinal) {
			++sawSegments;
			lastOrdinal = head.ordinal;
		}
	}
	// NON-VACUITY, AND IT IS THE HALF THAT MATTERS: every check above is satisfied by a shape that
	// never moved off segment 0, and by one that was idle the whole time.
	CHECK(sawSegments == 3);

	// AND WHEN IT RUNS OFF THE END IT IS NOWHERE, not parked on the last segment. `Position` already
	// says so by holding kMaxSegments; the playhead must not quietly re-point that at a real segment.
	REQUIRE(shape.idle());
	CHECK_FALSE(shape.playhead().running());
}

TEST_CASE("the playhead's phase climbs across a segment rather than jumping at its end") {
	// **A BOUND IS NOT A VALUE, AND "BETWEEN 0 AND 1" IS A BOUND** — the case above checks the range
	// and would pass against a phase pinned at 0 for a whole segment and snapping to 1 at its edge,
	// which is exactly what a marker must not do. So the SHAPE of the climb is pinned here.
	mseg::Shape shape;
	shape.prepare(kSampleRate);
	// ONE LONG SEGMENT, so there are many ticks inside it to look at.
	const mseg::Settings s = bare({ { 0.40, 1.0f } });

	std::vector<float> insideFirst;
	// THE WHOLE SEGMENT, not part of it — see the bounds below for what running short did to them.
	const int ticks = static_cast<int>(0.45 / kTickSeconds);
	for (int i = 0; i < ticks; ++i) {
		(void)shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
		const feathers::mod::Playhead head = shape.playhead();
		if (head.running() && head.ordinal == 0) {
			insideFirst.push_back(head.phase);
		}
	}
	REQUIRE(insideFirst.size() > 20);
	for (size_t i = 1; i < insideFirst.size(); ++i) {
		CAPTURE(i);
		CHECK(insideFirst[i] > insideFirst[i - 1]);
	}
	// AND IT COVERS THE SEGMENT rather than crawling in one corner of it.
	//
	// **BOTH NUMBERS ARE FROM THE INSTRUMENT AND THE FIRST DRAFT'S WERE NOT.** I wrote "measured …
	// reaches past 0.9" into this comment before measuring anything, and the case failed: run for
	// 0.30 s of a 0.40 s segment it only reaches 0.75, so the bound was wrong AND the comment was
	// claiming a measurement that had not happened. Run to completion the readings are
	// first = 0.003333 (one tick in) and last = 0.999999, both measured, 300 readings.
	CHECK(insideFirst.front() < 0.02f);
	CHECK(insideFirst.back() > 0.99f);
	CHECK(insideFirst.size() > 250);
}

TEST_CASE("a segment reaches its declared level in its declared time") {
	// THE VALUE, NOT A DIFFERENCE. This project's standing rule, and an MSEG is the case that makes it
	// unavoidable: a shape can pass through the same level three times, so "the output changed" says
	// nothing about which segment produced it or whether it got there on time.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	// One segment: half a second, ending at 1.0, from a start of 0.
	const mseg::Settings s = bare({ { 0.5, 1.0f } });
	const std::vector<float> v = run(shape, s, 1.0);
	REQUIRE(v.size() > 100);

	// A LINEAR SEGMENT IS LINEAR, so its value at a quarter of the way is a quarter of the travel.
	// Asserted at three points rather than one, because a single point is satisfied by any curve
	// through it.
	const auto at = [&](double seconds) { return v[static_cast<size_t>(seconds / kTickSeconds) - 1]; };
	CHECK(at(0.125) == doctest::Approx(0.25f).epsilon(0.02));
	CHECK(at(0.250) == doctest::Approx(0.50f).epsilon(0.02));
	CHECK(at(0.375) == doctest::Approx(0.75f).epsilon(0.02));

	// And it ARRIVES: at the declared duration it is at the declared level, and it stays there,
	// because a finished shape holds where its last segment left it rather than snapping to zero.
	CHECK(at(0.5) == doctest::Approx(1.0f).epsilon(0.02));
	CHECK(v.back() == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("a segment with no duration is a step rather than a divide by zero") {
	// The convention device.h states for every LogSeconds range: the bottom means NO TIME AT ALL. The
	// DAHDSR's card paid three separate bugs for forgetting it, so the engine states it as a case.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	mseg::Settings s = bare({ { 0.0, 0.8f }, { 0.25, 0.0f } });
	s.segment[0].share = kTimeLog2SecondsMin; // exactly the bottom

	const float first = shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
	CHECK(std::isfinite(first));

	// THE STEP HAPPENS AND THE TICK CARRIES ON, which is what changed when advance() started spending
	// SECONDS rather than phase. It used to charge a whole tick for a segment declared to take no
	// time, so the first reading was 0.8 exactly and the shape was a tick behind for the rest of its
	// life. Now the step costs nothing and the following segment — 250 ms from 0.8 down to 0 — gets
	// the whole of the tick.
	//
	// The reference is computed from the declared numbers rather than read off the run: one tick of
	// 250 ms is 1/187.5 of the way down an 0.8 drop. Measured 0.795733; predicted 0.795733.
	const double intoNext = kTickSeconds / 0.25;
	CHECK(static_cast<double>(first) == doctest::Approx(0.8 - 0.8 * intoNext).epsilon(1e-4));

	// AND IT IS STILL A STEP RATHER THAN A RAMP — it arrived at 0.8 within this tick and is on its way
	// down from there, not creeping up towards it. Without this the check above is satisfied by any
	// shape that happens to pass through 0.7957.
	CHECK(first > 0.79f);
	CHECK(shape.position().segment == 1);
}

TEST_CASE("the loop markers repeat the middle and leave the run-in alone") {
	// THIS IS THE REASON THIS DEVICE EXISTS BESIDE THE DAHDSR. The rack plan records what a plain
	// looping flag cannot say: "Bitwig's Segments loops between two points INSIDE the shape, so an
	// attack plays once and a middle repeats under it — the sampler's sustain loop. On the MSEG that
	// is two draggable markers rather than a flag."
	//
	// So the claim is not "it repeats" — the DAHDSR's flag does that. It is that the segments BEFORE
	// the markers play exactly ONCE and the ones between them repeat.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	// Segment 0 is the run-in: a slow rise to 1.0 that happens once.
	// Segments 1 and 2 are the loop: down to 0.4 and back to 0.9, repeatedly.
	mseg::Settings s = bare({ { 0.4, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f } });
	s.mode = Mode::Loop;
	s.loopStart = 1;
	s.loopEnd = 2;
	s.sustainSegment = mseg::kNoSustain;

	const std::vector<float> v = run(shape, s, 3.0);
	REQUIRE(v.size() > 500);

	const auto at = [&](double seconds) { return v[static_cast<size_t>(seconds / kTickSeconds) - 1]; };

	// The run-in happened: halfway through segment 0 the shape is halfway up.
	CHECK(at(0.2) == doctest::Approx(0.5f).epsilon(0.05));
	CHECK(at(0.4) == doctest::Approx(1.0f).epsilon(0.05));

	// THE LOOP RUNS. One cycle is segments 1 and 2 — 0.4 s — so from 0.4 s the shape is periodic.
	// Sampled a whole number of cycles apart, the readings must agree.
	CHECK(at(0.5) == doctest::Approx(at(0.9)).epsilon(0.05));
	CHECK(at(0.5) == doctest::Approx(at(1.3)).epsilon(0.05));
	CHECK(at(0.7) == doctest::Approx(at(1.1)).epsilon(0.05));

	// AND THE RUN-IN NEVER COMES BACK, which is the half a flag could not express. Segment 0 is the
	// only place the shape is below 0.4, so after the loop starts it must never go there again — if
	// the whole shape were repeating, it would return to the start level every cycle.
	size_t belowAfterLoop = 0;
	for (size_t i = static_cast<size_t>(0.45 / kTickSeconds); i < v.size(); ++i) {
		if (v[i] < 0.35f) {
			++belowAfterLoop;
		}
	}
	CAPTURE(belowAfterLoop);
	CHECK(belowAfterLoop == 0);

	SUBCASE("and the run-in DID go there, or the check above is vacuous") {
		// The other half: segment 0 really does pass below 0.35, so "never again" is a claim about
		// something that happens rather than about something impossible.
		size_t belowDuringRunIn = 0;
		for (size_t i = 0; i < static_cast<size_t>(0.3 / kTickSeconds) && i < v.size(); ++i) {
			if (v[i] < 0.35f) {
				++belowDuringRunIn;
			}
		}
		CAPTURE(belowDuringRunIn);
		CHECK(belowDuringRunIn > 10);
	}
}

TEST_CASE("a sustain breakpoint holds the shape until the gate falls") {
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	// Rise to 1.0, fall to 0.5, then release to 0. Sustain at the end of segment 1.
	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.5f }, { 0.2, 0.0f } });
	s.sustainSegment = 1;

	// Held: it arrives at 0.5 and STAYS, however long the gate is up.
	const std::vector<float> held = run(shape, s, 1.5, true);
	CHECK(held.back() == doctest::Approx(0.5f).epsilon(0.01));
	// Sampled well apart, so "it stays" is about a plateau rather than about one instant.
	CHECK(held[static_cast<size_t>(0.5 / kTickSeconds)] == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(held[static_cast<size_t>(1.0 / kTickSeconds)] == doctest::Approx(0.5f).epsilon(0.01));

	// Gate falls: the shape carries on into the segment after the breakpoint and reaches 0.
	const std::vector<float> released = run(shape, s, 0.5, false);
	CHECK(released.back() == doctest::Approx(0.0f).epsilon(0.02));

	SUBCASE("and with no sustain point it runs straight through") {
		// The other kind of MSEG, and the reason kNoSustain is a value rather than a second boolean.
		mseg::Shape other;
		other.prepare(kSampleRate);
		mseg::Settings once = s;
		once.sustainSegment = mseg::kNoSustain;
		const std::vector<float> v = run(other, once, 1.0, true);
		// It reached the end despite the gate never falling.
		CHECK(v.back() == doctest::Approx(0.0f).epsilon(0.02));
	}
}

TEST_CASE("a looping shape does not hold at its sustain breakpoint — the loop wins") {
	// THE RULE: an MSEG's sustain segment does not apply in looping mode.
	//
	// AND IT WAS A SOUND BUG, NOT A DRAWING ONE. The sustain branch in `advance` ran BEFORE the loop
	// branch and returned, and it consulted `sustainSegment` without ever asking `loop` — so on a
	// looping shape the breakpoint took first refusal and the loop NEVER RAN. A marker that silently
	// disabled a mode the player turned on, which is worse than a marker drawn where it does nothing.
	//
	// WHY THE LOOP WINS AND NOT THE OTHER WAY: see `sustainInForce`. The two are answers to the same
	// question and there is no arrangement of the indices in which both do something, so one of them
	// has to be dead; the DAHDSR already kills the hold rather than the loop, and two modulators in
	// one rack disagreeing about that would be worse than either choice.
	//
	// POSITIONS OVER TIME, NOT A DIFFERENCE. `Position` exists for exactly this: a value alone cannot
	// say which segment produced it, and the broken build and the fixed one both sit at levels this
	// shape genuinely visits.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	// Run-in 0.4 s to 1.0; then the loop, segments 1 and 2 at 0.2 s each; then a release tail.
	mseg::Settings s = bare({ { 0.4, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f }, { 0.2, 0.0f } });
	s.mode = Mode::Loop;
	s.loopStart = 1;
	s.loopEnd = 2;
	s.sustainSegment = 1; // the breakpoint the loop must overrule

	// THE PREDICATE THE ENGINE AND THE PICTURE SHARE, asserted on its own so the cases below are
	// about behaviour rather than about re-deriving it.
	CHECK(mseg::sustainInForce(s) == mseg::kNoSustain);

	std::vector<size_t> segmentAt;
	std::vector<float> value;
	bool everSustained = false;
	const int ticks = static_cast<int>(1.6 / kTickSeconds);
	for (int i = 0; i < ticks; ++i) {
		value.push_back(shape.tick(s, true, false, feathers::mod::TransportClock::unknown()));
		segmentAt.push_back(shape.position().segment);
		everSustained = everSustained || shape.position().sustaining;
	}
	const auto tickOf = [](double seconds) { return static_cast<size_t>(seconds / kTickSeconds) - 1; };

	// IT NEVER HOLDS. The broken build sets `sustaining` at the end of segment 1 and never clears it.
	CHECK_FALSE(everSustained);

	// AND THE VALUES ARE THE INTENDED PATH RESTATED INDEPENDENTLY, not a comparison between runs.
	// Every segment is linear here (shape 0), so the level is a plain interpolation:
	//   t = 0.2  — halfway up segment 0:            0.0 + (1.0 - 0.0) * 0.5  = 0.50
	//   t = 0.5  — halfway through segment 1:       1.0 + (0.4 - 1.0) * 0.5  = 0.70
	//   t = 0.7  — halfway through segment 2:       0.4 + (0.9 - 0.4) * 0.5  = 0.65
	//   t = 0.9  — halfway through segment 1 again: 0.70, the loop's second pass
	//   t = 1.1  — halfway through segment 2 again: 0.65
	// The broken build reads 0.40 at every one of the last four — pinned at segment[1].level.
	CHECK(value[tickOf(0.2)] == doctest::Approx(0.50f).epsilon(0.03));
	CHECK(value[tickOf(0.5)] == doctest::Approx(0.70f).epsilon(0.03));
	CHECK(value[tickOf(0.7)] == doctest::Approx(0.65f).epsilon(0.03));
	CHECK(value[tickOf(0.9)] == doctest::Approx(0.70f).epsilon(0.03));
	CHECK(value[tickOf(1.1)] == doctest::Approx(0.65f).epsilon(0.03));

	// AND THE POSITION SAYS THE SAME THING FROM THE OTHER SIDE. After the run-in the shape is only
	// ever in segment 1 or 2, and it is in BOTH many times — a shape frozen at the breakpoint would
	// report segment 1 forever and never segment 2.
	size_t inOne = 0;
	size_t inTwo = 0;
	size_t elsewhere = 0;
	for (size_t i = tickOf(0.45); i < segmentAt.size(); ++i) {
		if (segmentAt[i] == 1) { ++inOne; }
		else if (segmentAt[i] == 2) { ++inTwo; }
		else { ++elsewhere; }
	}
	CAPTURE(inOne);
	CAPTURE(inTwo);
	CAPTURE(elsewhere);
	CHECK(elsewhere == 0);
	CHECK(inOne > 100);
	CHECK(inTwo > 100);

	SUBCASE("and the gate falling still releases past the loop") {
		// The half that says the loop is a SUSTAIN loop rather than a permanent one: segment 3 is
		// unreachable while the gate is up and must be reached the moment it falls.
		const std::vector<float> released = run(shape, s, 0.6, false);
		CHECK(released.back() == doctest::Approx(0.0f).epsilon(0.02));
		CHECK(shape.position().finished);
	}

	SUBCASE("and with the loop off the SAME settings hold at the breakpoint") {
		// THE CONTROL FOR THE CLAIM. If the sustain point were simply broken, this would fail too —
		// so this is what makes "the loop overrules it" different from "it stopped working".
		mseg::Shape other;
		other.prepare(kSampleRate);
		mseg::Settings held = s;
		held.mode = Mode::Gate;
		CHECK(mseg::sustainInForce(held) == 1);

		const std::vector<float> v = run(other, held, 1.5, true);
		CHECK(other.position().sustaining);
		CHECK(other.position().segment == 1);
		CHECK(v.back() == doctest::Approx(0.4f).epsilon(0.01));
		CHECK(v[static_cast<size_t>(1.0 / kTickSeconds)] == doctest::Approx(0.4f).epsilon(0.01));
	}
}

TEST_CASE("turning the loop on under a held note ends the hold it was already in") {
	// THE HOLD IS A STATE, AND A STATE OUTLIVES THE CONDITION THAT CREATED IT. `advance` returns
	// early while `sustaining`, so fixing only the branch that ENTERS the hold would leave a shape
	// that was already parked at its breakpoint frozen there forever — the loop would start working
	// on the next note and not on this one. A hand toggling Loop under a held key is the ordinary way
	// to find out what Loop does.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.4f }, { 0.1, 0.9f } });
	s.sustainSegment = 1;
	s.mode = Mode::Gate;
	s.loopStart = 1;
	s.loopEnd = 2;

	// Parked at the breakpoint: segment 1, holding, at its level.
	const std::vector<float> before = run(shape, s, 0.6, true);
	REQUIRE(shape.position().sustaining);
	REQUIRE(shape.position().segment == 1);
	CHECK(before.back() == doctest::Approx(0.4f).epsilon(0.01));

	// The chip goes on, the gate never falls.
	mseg::Settings looping = s;
	looping.mode = Mode::Loop;
	const std::vector<float> after = run(shape, looping, 0.6, true);

	CHECK_FALSE(shape.position().sustaining);
	// AND IT IS ACTUALLY CYCLING, asserted as a level the hold cannot produce: the loop is segments 1
	// and 2, so the shape must reach segment 2's level of 0.9 — 0.4 is the only level a frozen shape
	// can show.
	float highest = 0.0f;
	for (const float v : after) {
		highest = std::max(highest, v);
	}
	CAPTURE(highest);
	CHECK(highest > 0.85f);
}

TEST_CASE("the row round-trips, and a short payload keeps the declared defaults") {
	mseg::Settings s = bare({ { 0.3, 0.7f }, { 0.1, 0.2f } });
	s.timeScale = 0.5f;
	s.bipolar = true;
	s.mode = Mode::Once;
	s.segment[1].shape = 0.6f;

	const mseg::Row row = mseg::rowOf(s);
	const mseg::Settings back = mseg::settingsOf(row, row.size());
	CHECK(back == s);

	SUBCASE("a payload that stops short fills the rest from the declarations, not from zero") {
		// THE POINT OF CARRYING fieldsPresent. A zero-length segment at level zero is a REAL shape, so
		// a short payload read as zeroes would be a silently different patch rather than an obviously
		// broken one — which is the failure mode a flattened row makes invisible.
		const size_t shortLength = static_cast<size_t>(mseg::Param::Seg2Time);
		const mseg::Settings partial = mseg::settingsOf(row, shortLength);

		// What the payload reached is what it said.
		CHECK(partial.segment[0].level == doctest::Approx(0.7f));
		CHECK(partial.segment[1].shape == doctest::Approx(0.6f));

		// What it did not reach is the DECLARED DEFAULT — 200 ms and level 1.0 — rather than zero.
		const auto& timeDesc = mseg::kParamDescs[static_cast<size_t>(mseg::Param::Seg2Time)];
		const auto& levelDesc = mseg::kParamDescs[static_cast<size_t>(mseg::Param::Seg2Level)];
		CHECK(partial.segment[2].share == doctest::Approx(static_cast<float>(timeDesc.def)));
		CHECK(partial.segment[2].level == doctest::Approx(static_cast<float>(levelDesc.def)));
		CHECK(timeSeconds(partial.segment[2].share) == doctest::Approx(0.2).epsilon(0.01));
	}
}

TEST_CASE("sanitize keeps the loop inside the shape it belongs to") {
	// The three relationships a per-field clamp cannot state, because a range is a fact about ONE
	// control. Every field below is individually in range and the combination is still nonsense.
	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.0f } });
	s.count = 2;
	s.loopStart = 6; // past the live segments
	s.loopEnd = 1;
	s.sustainSegment = 5; // also past them
	mseg::sanitize(&s);

	CAPTURE(s.loopStart);
	CAPTURE(s.loopEnd);
	CAPTURE(s.sustainSegment);
	CHECK(s.loopStart <= s.count - 1);
	CHECK(s.loopEnd <= s.count - 1);
	CHECK(s.loopEnd >= s.loopStart);
	// A sustain point past the end is NOT clamped to the last segment — it is "no sustain". Clamping
	// would invent a hold the user never asked for on a shape that had none.
	CHECK(s.sustainSegment == mseg::kNoSustain);

	SUBCASE("and a non-number goes to the declaration rather than to an end of the range") {
		mseg::Settings bad = bare({ { 0.1, 1.0f } });
		bad.segment[0].level = std::numeric_limits<float>::quiet_NaN();
		mseg::sanitize(&bad);
		const auto& desc = mseg::kParamDescs[static_cast<size_t>(mseg::Param::Seg0Level)];
		CHECK(bad.segment[0].level == doctest::Approx(static_cast<float>(desc.def)));
	}
}

TEST_CASE("a retrigger sends the shape back to its start level, mid-flight or not") {
	// THE DEFECT: retrigger did not work if the shape had not completed — a new note arriving before
	// the cycle completed left the output at its last level instead of starting at the beginning.
	//
	// THE CASE THIS REPLACES ASSERTED THE OPPOSITE, ON PURPOSE. "A retrigger continues from where the
	// output is, not from the start level" pinned a soft restart — `segmentFrom_ = raw_` — as click
	// avoidance. What that bought was a restart nobody could hear: on a FALLING segment a shape
	// re-fired at 0.5 heads for 0 from 0.5, which is exactly what it was doing before the note arrived,
	// and the only thing that changed was the time it takes to get there. That is the defect, exactly.
	// A retrigger means the shape plays again from the start; the smoothing switch is
	// where a step's edge goes for anyone who minds it.
	//
	// SO THE SHAPE HERE FALLS, because a falling first segment is where the old behaviour was
	// invisible — on a rising one the two restarts differ only in slope.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	mseg::Settings s = bare({ { 0.4, 0.0f }, { 0.4, 1.0f } });
	s.startLevel = 1.0f;

	// Run to about halfway DOWN the first segment.
	std::vector<float> v = run(shape, s, 0.2);
	const float before = v.back();
	REQUIRE(before > 0.3f);
	REQUIRE(before < 0.7f);

	SUBCASE("the control: with no note, the next tick goes on falling") {
		// Without this the subcases below could be watching a shape that returns to the top on its
		// own, and the retrigger would be getting credit for the shape's own behaviour.
		const float next = shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
		CAPTURE(before);
		CAPTURE(next);
		CHECK(next < before);
		CHECK(before - next < 0.01f);
	}

	SUBCASE("a note under the held gate: the very next tick is at the top again") {
		const float after = shape.tick(s, true, true, feathers::mod::TransportClock::unknown());
		CAPTURE(before);
		CAPTURE(after);
		// THE VALUE: the start level less one tick's travel down a 0.4 s segment, about 0.003. The
		// old soft restart reads `before` here to within a few thousandths — and "it changed" would
		// have passed that too, since the slope changes.
		CHECK(after > 0.99f);
		CHECK(shape.position().segment == 0);
		CHECK(shape.position().phase < 0.01f);
	}

	SUBCASE("a note after the gate fell, while the tail is still running") {
		// No sustain point, so the gate falling changes nothing about the shape's course — the tail
		// is still on its way down when the next note arrives. This is the case the user can play: a
		// staccato note whose successor lands inside the previous one's shape.
		const float released = shape.tick(s, false, false, feathers::mod::TransportClock::unknown());
		REQUIRE(released < before);
		const float after = shape.tick(s, true, true, feathers::mod::TransportClock::unknown());
		CAPTURE(released);
		CAPTURE(after);
		CHECK(after > 0.99f);
		CHECK(shape.position().segment == 0);
	}
}

TEST_CASE("a retrigger from a sustain hold leaves the hold and starts over") {
	// The same claim from the other resting place a shape has: parked at its sustain breakpoint
	// under a held note, a second note must send it back to the start level rather than leave it
	// sitting on the breakpoint — which is what the old restart-from-the-output did, since the
	// output WAS the breakpoint.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	mseg::Settings s = bare({ { 0.1, 0.0f }, { 0.1, 0.5f }, { 0.1, 1.0f } });
	s.startLevel = 1.0f;
	s.sustainSegment = 1;

	run(shape, s, 0.5);
	REQUIRE(shape.position().sustaining);
	REQUIRE(shape.value() == doctest::Approx(0.5f));

	const float after = shape.tick(s, true, true, feathers::mod::TransportClock::unknown());
	CAPTURE(after);
	// One tick down a 0.1 s segment is about 0.013.
	CHECK(after > 0.97f);
	CHECK(shape.position().segment == 0);
	CHECK_FALSE(shape.position().sustaining);
}

TEST_CASE("the declared control count differs from the DAHDSR's, which is the whole point") {
	// The MSEG is not a bonus — it is the TEST. A second modulator type with a different control count
	// is the only thing that demonstrates pin #1 is actually gone rather than merely widened.
	//
	// So the count is asserted, and asserted as DIFFERENT rather than as a number, because the number
	// is allowed to move and the difference is the claim.
	CHECK(mseg::kParamDescs.size() == static_cast<size_t>(mseg::Param::kCount));
	CHECK(static_cast<size_t>(mseg::Param::kCount) != 15u); // the DAHDSR's

	// Every descriptor is declared: a name, and a range that is a range.
	for (size_t i = 0; i < mseg::kParamDescs.size(); ++i) {
		CAPTURE(i);
		const auto& desc = mseg::kParamDescs[i];
		CAPTURE(std::string(desc.name == nullptr ? "<null>" : desc.name));
		REQUIRE(desc.name != nullptr);
		CHECK(std::string(desc.name).size() > 0);
		CHECK(desc.max >= desc.min);
		CHECK(desc.def >= desc.min);
		CHECK(desc.def <= desc.max);
	}

	SUBCASE("and the breakpoints are patch state rather than modulation destinations") {
		// A DELIBERATE DIVERGENCE FROM THE DAHDSR, where every control is modulatable. Modulating one
		// breakpoint out of eight reaches almost nothing musically and costs eight destinations per
		// device; modulating Rate and Amount reaches everything a hand wants from a shape under
		// modulation. Recorded as a case so the choice is visible rather than implied.
		for (size_t seg = 0; seg < mseg::kMaxSegments; ++seg) {
			CAPTURE(seg);
			for (const auto field :
			     { mseg::SegmentField::Time, mseg::SegmentField::Level, mseg::SegmentField::Shape }) {
				const auto p = mseg::segmentParam(seg, field);
				CHECK(mseg::kParamDescs[static_cast<size_t>(p)].modulatable == false);
			}
		}
		// And the two that ARE the handles.
		CHECK(mseg::kParamDescs[static_cast<size_t>(mseg::Param::TimeScale)].modulatable == true);
	}
}

TEST_CASE("the free trigger runs without a gate at all") {
	// What makes a looped shape an LFO rather than a repeating envelope. Free substitutes an always-up
	// gate at ONE place, so everything downstream still reasons about a single boolean.
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.0f } });
	s.mode = Mode::Free;
	s.loopStart = 0;

	// The gate is NEVER up, and it runs anyway.
	const std::vector<float> v = run(shape, s, 1.0, false);
	float lo = 9.0f;
	float hi = -9.0f;
	for (const float x : v) {
		lo = std::min(lo, x);
		hi = std::max(hi, x);
	}
	CAPTURE(lo);
	CAPTURE(hi);
	CHECK(hi > 0.9f);
	CHECK(lo < 0.1f);

	SUBCASE("where a retrigger-mode shape with the gate down does nothing at all") {
		// The half that makes the above a claim about Free rather than about looping.
		mseg::Shape gated;
		gated.prepare(kSampleRate);
		mseg::Settings retrig = s;
		retrig.mode = Mode::Gate;
		const std::vector<float> still = run(gated, retrig, 1.0, false);
		for (const float x : still) {
			CHECK(x == doctest::Approx(0.0f).epsilon(0.001));
		}
	}
}

TEST_CASE("the editor's x axis is duration and not a knob position") {
	// THE INHERITED LESSON, and the reason this file exists separately from a widget. The DAHDSR's
	// card originally sized each stage by its control's POSITION IN ITS RANGE — which is log2 seconds
	// — so a stage taking fifty times as long drew 1.66x as wide, and an evening went on a
	// ten-second period the picture could not show.
	//
	// A shape editor whose x axis is not time is not a shape editor, and here that is checkable
	// without a window.
	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.5f } });
	const mseg::Layout even = mseg::layoutOf(s);
	REQUIRE(even.segmentEnd.size() == 2);
	// Two equal durations, two equal widths.
	CHECK(even.segmentEnd[0] == doctest::Approx(0.5f).epsilon(0.02));
	CHECK(even.segmentEnd[1] == doctest::Approx(1.0f).epsilon(0.001));

	SUBCASE("a segment ten times longer is drawn about ten times wider") {
		mseg::Settings lopsided = bare({ { 0.1, 1.0f }, { 1.0, 0.5f } });
		const mseg::Layout l = mseg::layoutOf(lopsided);
		const float first = l.segmentEnd[0];
		const float second = l.segmentEnd[1] - l.segmentEnd[0];
		CAPTURE(first);
		CAPTURE(second);
		// PROPORTIONAL, not merely monotone: on the log axis this pair drew 1.3x, which is the whole
		// defect. Loose bounds because the visibility floor perturbs the ratio slightly, and tight
		// enough that a log axis fails by a mile.
		CHECK(second / first > 6.0f);
	}

	SUBCASE("and a segment that does not happen takes no width, while a short one stays grabbable") {
		mseg::Settings mixed = bare({ { 0.5, 1.0f }, { 0.001, 0.5f } });
		const mseg::Layout live = mseg::layoutOf(mixed);
		const float shortOne = live.segmentEnd[1] - live.segmentEnd[0];
		CAPTURE(shortOne);
		// A THOUSANDTH OF THE TIME, BUT STILL A HANDLE. Proportional alone gives 0.2% of the width,
		// which is not something a hand can grab — the floor is a concession about pixels, and it is
		// asserted so nobody removes it as an inconsistency.
		CHECK(shortOne > 0.02f);
		CHECK(shortOne < 0.20f);

		// But a segment declared OFF is a vertical step and gets nothing, which is the convention
		// device.h states for the bottom of a LogSeconds range.
		mseg::Settings stepped = mixed;
		stepped.segment[1].share = kTimeLog2SecondsMin;
		const mseg::Layout withStep = mseg::layoutOf(stepped);
		CHECK(withStep.segmentEnd[1] - withStep.segmentEnd[0] == doctest::Approx(0.0f).epsilon(1e-4));
	}
}

TEST_CASE("dragging a breakpoint TRADES time with its neighbour, and leaves every other segment where it was — in the window and on the screen") {
	// **THIS CASE USED TO ASSERT THE OPPOSITE, AND ITS OWN TITLE WAS THE TELL.** It was called
	// "changes its OWN segment and leaves the others alone", and three lines from the end it checked
	// `fractionOfWindow(moved, 2) < fractionOfWindow(s, 2)` with the comment "squeezed, because the
	// window is fixed". Both of those cannot be true. "Leaves the others alone" was a claim about the
	// stored FIELD; "squeezed" was the truth about the DURATION, which is `share / total × window` and
	// therefore moves whenever the total does. It was found from the other end — dragging points
	// upset other points — because the picture was reporting the retiming faithfully.
	//
	// **THE OLD RULE'S OWN OBJECTION IS WHAT DECIDES IT.** It argued against trading because "you
	// cannot lengthen anything without shortening something you were not touching". Under a fixed
	// window you cannot: it is arithmetic. The only question is whose time is taken, and the old
	// answer took it from EVERY segment in proportion. Taking it from the neighbour is the local and
	// predictable answer; taking it from everyone is the surprising one.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.2, 0.5f }, { 0.2, 0.0f } });
	const double before = mseg::totalSeconds(s);
	const float otherBefore = s.segment[2].share;

	const mseg::Layout layout = mseg::layoutOf(s);
	REQUIRE(layout.handle.size() == 4); // a start plus one per segment

	// Handle 1 ends segment 0. Drag it right, from a third of the box to two thirds.
	const mseg::Settings moved = mseg::withHandleMoved(s, 1, 0.66f, 1.0f);

	CAPTURE(timeSeconds(s.segment[0].share));
	CAPTURE(timeSeconds(moved.segment[0].share));
	// ITS OWN SEGMENT GREW.
	CHECK(timeSeconds(moved.segment[0].share) > timeSeconds(s.segment[0].share) * 1.4);
	// AND THE NEIGHBOUR PAID FOR IT, EXACTLY — their sum is what is held, which is the mechanism.
	CHECK(timeSeconds(moved.segment[0].share) + timeSeconds(moved.segment[1].share)
	      == doctest::Approx(timeSeconds(s.segment[0].share) + timeSeconds(s.segment[1].share)));
	CHECK(timeSeconds(moved.segment[1].share) < timeSeconds(s.segment[1].share));
	// THE ONE BEYOND THE PAIR DID NOT MOVE AT ALL — the value, not "roughly".
	CHECK(moved.segment[2].share == doctest::Approx(otherBefore));
	// AND THE SHAPE IS EXACTLY AS LONG AS IT WAS, WHICH IS THE OPPOSITE OF WHAT THIS ASSERTED.
	//
	// It used to read "the shape is LONGER, which is the consequence that makes the rule visible" —
	// true while a segment carried an absolute duration and the shape's length was their sum. Under
	// the time base it is false BY DESIGN: the window is what the division and the scale knob decide,
	// and a segment says only what fraction of it to take. A drag REDISTRIBUTES the window; it cannot
	// lengthen it. That is what "the whole curve fits inside (time_base) * scale" means, and it is a
	// better rule — a hand adjusting the shape no longer silently changes how long the shape takes.
	CHECK(mseg::totalSeconds(moved) == doctest::Approx(before));
	// THE REDISTRIBUTION IS THE CLAIM NOW, stated as the fraction rather than as the raw share, since
	// the raw share is only meaningful against the sum.
	const auto fractionOfWindow = [](const mseg::Settings& shape, size_t segment) {
		double sum = 0.0;
		for (size_t i = 0; i < shape.count; ++i) {
			sum += timeSeconds(shape.segment[i].share);
		}
		return sum > 0.0 ? timeSeconds(shape.segment[segment].share) / sum : 0.0;
	};
	CHECK(fractionOfWindow(moved, 0) > fractionOfWindow(s, 0) * 1.4);
	// **AND THIS IS THE LINE THE WHOLE CHANGE IS ABOUT.** It read
	// `fractionOfWindow(moved, 2) < fractionOfWindow(s, 2)` — "squeezed, because the window is fixed"
	// — which is a segment nobody touched being retimed by a drag two handles away. It is EQUAL now,
	// and equal is what "leaves the others alone" has to mean if it is to mean anything: not that the
	// stored number survived, but that the segment still plays for as long as it did.
	CHECK(fractionOfWindow(moved, 2) == doctest::Approx(fractionOfWindow(s, 2)));

	SUBCASE("a handle cannot cross its neighbours, because past them there is no time left to take") {
		// THE COST OF THE TRADE, ASSERTED RATHER THAN LEFT TO BE DISCOVERED. Dragging handle 1 far
		// right stops where handle 2 is: it can take all of segment 1's time and no more.
		const mseg::Settings far = mseg::withHandleMoved(s, 1, 0.99f, 1.0f);
		CHECK(timeSeconds(far.segment[1].share) == doctest::Approx(0.0));
		CHECK(timeSeconds(far.segment[0].share)
		      == doctest::Approx(timeSeconds(s.segment[0].share) + timeSeconds(s.segment[1].share)));
		// AND STILL NOTHING BEYOND THE PAIR MOVED, at the extreme where it would matter most.
		CHECK(far.segment[2].share == doctest::Approx(otherBefore));
		CHECK(mseg::totalSeconds(far) == doctest::Approx(before));
	}

	SUBCASE("dragging it onto the previous handle makes a step, not an error") {
		const mseg::Settings step = mseg::withHandleMoved(s, 1, 0.0f, 1.0f);
		CHECK(timeSeconds(step.segment[0].share) == doctest::Approx(0.0));
		// And the level it was dragged to still took.
		CHECK(step.segment[0].level == doctest::Approx(1.0f));
	}

	SUBCASE("the start handle has no duration before it, so only its level moves") {
		const double totalBefore = mseg::totalSeconds(s);
		const mseg::Settings start = mseg::withHandleMoved(s, 0, 0.5f, 0.3f);
		CHECK(start.startLevel == doctest::Approx(0.3f));
		CHECK(mseg::totalSeconds(start) == doctest::Approx(totalBefore));
	}
}

TEST_CASE("a bipolar shape's handles round-trip through the output domain") {
	// The y a drag arrives with is in the OUTPUT's domain, which for a bipolar shape is -1..1. A
	// breakpoint stores the level the user set, so the rescale has to be undone on the way in and
	// applied on the way out — and if those two disagree, dragging a handle and letting go moves it.
	mseg::Settings s = bare({ { 0.2, 1.0f } });
	s.bipolar = true;

	// Drop the handle at the middle of a bipolar box, which is level 0.5.
	const mseg::Settings moved = mseg::withHandleMoved(s, 1, 1.0f, 0.0f);
	CHECK(moved.segment[0].level == doctest::Approx(0.5f));

	// And the layout puts it back where it was dropped.
	const mseg::Layout l = mseg::layoutOf(moved);
	REQUIRE(l.handle.size() == 2);
	const float plottedY = 2.0f * l.handle[1].level - 1.0f;
	CHECK(plottedY == doctest::Approx(0.0f).epsilon(0.001));

	SUBCASE("and the drawn curve uses the whole of the bipolar range") {
		// The DAHDSR's card clamped -1..1 into 0..1 and flattened half the signal onto the floor. This
		// exists so that mistake has one place it could be made.
		mseg::Settings swing = bare({ { 0.1, 1.0f }, { 0.1, 0.0f } });
		swing.bipolar = true;
		const auto pts = mseg::polyline(swing);
		float lo = 9.0f;
		float hi = -9.0f;
		for (const auto& p : pts) {
			lo = std::min(lo, p.second);
			hi = std::max(hi, p.second);
		}
		CAPTURE(lo);
		CAPTURE(hi);
		CHECK(hi > 0.9f);
		CHECK(lo < -0.9f);
	}
}

TEST_CASE("a loop marker snaps to a segment boundary because that is what it is") {
	// A marker IS a segment index — the engine cannot start a loop halfway through a segment — so an
	// editor that let one be dropped there would be offering a position it then silently rounded
	// away. The honesty rule, applied to a drag.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f } });
	s.mode = Mode::Loop;
	s.loopStart = 0;
	s.loopEnd = 2;

	// Drop the opening marker at 0.4 — nearest boundary is the start of segment 1, at 1/3.
	const mseg::Settings moved = mseg::withLoopMarkerMoved(s, true, 0.4f);
	CHECK(moved.loopStart == 1);

	// And the layout puts the marker exactly on the boundary rather than where it was dropped.
	const mseg::Layout l = mseg::layoutOf(moved);
	CHECK(l.loopVisible == true);
	CHECK(l.loopStartX == doctest::Approx(l.segmentEnd[0]));

	SUBCASE("and the closing marker cannot be dragged before the opening one") {
		// The relationship sanitize enforces, reached through the gesture that would break it.
		const mseg::Settings crossed = mseg::withLoopMarkerMoved(moved, false, 0.0f);
		CAPTURE(crossed.loopStart);
		CAPTURE(crossed.loopEnd);
		CHECK(crossed.loopEnd >= crossed.loopStart);
	}

	SUBCASE("the opening marker sits at its segment's LEFT edge and the closing one at the right") {
		// Getting these the same way round is the difference between a loop that includes its first
		// segment and one that skips it — and the engine's own loop runs FROM loopStart's beginning
		// TO loopEnd's end, so the picture has to agree.
		mseg::Settings one = s;
		one.loopStart = 1;
		one.loopEnd = 1;
		const mseg::Layout l1 = mseg::layoutOf(one);
		CHECK(l1.loopStartX == doctest::Approx(l1.segmentEnd[0]));
		CHECK(l1.loopEndX == doctest::Approx(l1.segmentEnd[1]));
		CHECK(l1.loopEndX > l1.loopStartX);
	}
}

TEST_CASE("the sustain marker leaves the picture when the shape loops") {
	// THE HONESTY RULE, WEARING A MARKER INSTEAD OF A KNOB. `layoutOf` used to decide the marker's
	// visibility from `sustainSegment` alone, so a looping shape drew a breakpoint the gate never
	// reaches. It is asked of `sustainInForce` now — the SAME function `Shape::advance` asks — which
	// is what makes the picture and the sound agree by construction rather than by two conditions
	// being kept in step.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f } });
	s.sustainSegment = 1;

	const mseg::Layout resting = mseg::layoutOf(s);
	CHECK(resting.sustainVisible);
	CHECK(resting.sustainActive);
	CHECK(resting.sustainX == doctest::Approx(resting.segmentEnd[1]));

	mseg::Settings looping = s;
	looping.mode = Mode::Loop;
	looping.loopStart = 0;
	looping.loopEnd = 2;
	const mseg::Layout laid = mseg::layoutOf(looping);
	CHECK_FALSE(laid.sustainVisible);
	CHECK_FALSE(laid.sustainActive);

	// AND THE STORED INDEX SURVIVES, which is why this is a predicate rather than a sanitise. A hand
	// auditioning the loop and turning it back off must find its breakpoint where it left it.
	CHECK(looping.sustainSegment == 1);
	mseg::Settings back = looping;
	back.mode = Mode::Gate;
	const mseg::Layout again = mseg::layoutOf(back);
	CHECK(again.sustainVisible);
	CHECK(again.sustainActive);
	CHECK(again.sustainX == doctest::Approx(resting.sustainX));

	SUBCASE("and the loop's own markers are the ones that appear instead") {
		// The two are exact complements, which is what lets the card grab either without a precedence
		// between them: `loopVisible` is `loop` and `sustainVisible` is its negation.
		CHECK(laid.loopVisible);
		CHECK_FALSE(resting.loopVisible);
	}
}

TEST_CASE("the sustain marker drags to a segment end, and off the left wall to none") {
	// THE SUSTAIN MARKER DRAGS TO A DIFFERENT LOCATION. There was no drag road at all — the
	// marker was drawn and `SustainSegment` was settable by nothing on the card, which the
	// reachability guard had recorded as a known gap.
	//
	// THE VALUE, NOT THE PICTURE. What the engine reads is `sustainSegment`, so that is what is
	// asserted; the layout is checked afterwards to show the marker followed the number rather than
	// the other way round.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f } });
	s.sustainSegment = 1;
	const mseg::Layout laid = mseg::layoutOf(s);
	REQUIRE(laid.segmentEnd.size() == 3);

	// Dropped near the end of segment 0 — a third across — it snaps there rather than staying where
	// the pointer was. A sustain point halfway through a segment is not something the engine can run.
	const mseg::Settings toFirst = mseg::withSustainMarkerMoved(s, 0.30f);
	CHECK(toFirst.sustainSegment == 0);
	CHECK(mseg::layoutOf(toFirst).sustainX == doctest::Approx(laid.segmentEnd[0]));

	// And to the end of the last segment, which is a legal place to hold.
	const mseg::Settings toLast = mseg::withSustainMarkerMoved(s, 0.95f);
	CHECK(toLast.sustainSegment == 2);
	CHECK(mseg::layoutOf(toLast).sustainX == doctest::Approx(laid.segmentEnd[2]));

	SUBCASE("the left wall means kNoSustain, and the marker parks there rather than vanishing") {
		// THE ONLY WAY TO CLEAR IT, and the reason "none" needs a POSITION. No segment's right edge
		// is ever x = 0, so the wall is unambiguous — and a marker that disappeared when cleared
		// would make the gesture one-way.
		const mseg::Settings cleared = mseg::withSustainMarkerMoved(s, 0.02f);
		CHECK(cleared.sustainSegment == mseg::kNoSustain);

		const mseg::Layout parked = mseg::layoutOf(cleared);
		CHECK(parked.sustainVisible);      // still on the picture, still grabbable
		CHECK_FALSE(parked.sustainActive); // and honest that it holds nothing
		CHECK(parked.sustainX == doctest::Approx(0.0f));

		// AND BACK, from the parked position — the half that makes it a gesture rather than a trap.
		const mseg::Settings replaced = mseg::withSustainMarkerMoved(cleared, 0.65f);
		CHECK(replaced.sustainSegment == 1);
		CHECK(mseg::layoutOf(replaced).sustainActive);
	}

	SUBCASE("and a looping shape refuses the drag rather than writing an index nothing reads") {
		// There is no marker to move; writing one would be the gesture pretending to have acted. The
		// card does not offer the grab there either, but the refusal lives in the geometry so the two
		// cannot drift apart.
		mseg::Settings looping = s;
		looping.mode = Mode::Loop;
		looping.loopStart = 0;
		looping.loopEnd = 2;
		const mseg::Settings after = mseg::withSustainMarkerMoved(looping, 0.95f);
		CHECK(after.sustainSegment == 1);
		CHECK(after == looping);
	}
}

TEST_CASE("a dragged sustain point keeps its place in the shape when a waypoint is inserted") {
	// THE REINDEXING WARNING ON `withWaypointAdded`, REACHED THROUGH THE NEW GESTURE. The drag writes
	// an INDEX, and inserting a segment changes what an index refers to — so a breakpoint set by hand
	// and then split in front of would silently name a different part of the shape.
	//
	// THE STRONG CLAIM IS ABOUT THE PICTURE'S x, NOT THE INDEX. An index that shifts by one is only
	// correct if it still names the same instant of the curve, and the x is where that is visible.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.2, 0.4f }, { 0.2, 0.9f } });
	s.sustainSegment = mseg::kNoSustain;

	// Placed by the gesture rather than by hand, so this case is about the road a hand takes.
	const mseg::Settings placed = mseg::withSustainMarkerMoved(s, 0.65f);
	REQUIRE(placed.sustainSegment == 1);
	const float wasX = mseg::layoutOf(placed).sustainX;

	// Split segment 0, which is entirely BEFORE the breakpoint. The total duration is preserved, so
	// the instant the marker names is unmoved even though its index is not.
	const mseg::Settings split = mseg::withWaypointAdded(placed, 0.15f);
	REQUIRE(split.count == 4);
	CHECK(split.sustainSegment == 2);
	CHECK(mseg::layoutOf(split).sustainX == doctest::Approx(wasX).epsilon(0.01));

	SUBCASE("and it comes back when the waypoint is removed again") {
		const mseg::Settings merged = mseg::withWaypointRemoved(split, 1);
		REQUIRE(merged.count == 3);
		CHECK(merged.sustainSegment == 1);
		CHECK(mseg::layoutOf(merged).sustainX == doctest::Approx(wasX).epsilon(0.01));
	}

	SUBCASE("a split AFTER the breakpoint leaves the index alone") {
		// The other side of the same rule, and the reason the guard is `>= seg` rather than
		// unconditional: a segment added past the marker names nothing the marker cares about.
		const mseg::Settings later = mseg::withWaypointAdded(placed, 0.9f);
		REQUIRE(later.count == 4);
		CHECK(later.sustainSegment == 1);
	}
}

TEST_CASE("coincident breakpoints stay individually grabbable") {
	// Breakpoints coincide exactly whenever a segment has no duration, and a hit test that preferred
	// the EARLIER one would make the second of a coincident pair permanently unreachable — a control
	// that exists and cannot be touched.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.0, 0.2f }, { 0.2, 0.6f } });
	s.segment[1].share = kTimeLog2SecondsMin; // no duration: handles 2 and 1 share an x

	const mseg::Layout l = mseg::layoutOf(s);
	REQUIRE(l.handle.size() == 4);
	CHECK(l.handle[1].x == doctest::Approx(l.handle[2].x));
	// They are at DIFFERENT levels, which is what makes them distinguishable at all.
	CHECK(l.handle[1].level != doctest::Approx(l.handle[2].level));

	// Asked at each one's own level, each is found.
	const int atFirst = mseg::handleAt(l, l.handle[1].x, l.handle[1].level, 0.05f, false);
	const int atSecond = mseg::handleAt(l, l.handle[2].x, l.handle[2].level, 0.05f, false);
	CAPTURE(atFirst);
	CAPTURE(atSecond);
	CHECK(atFirst == 1);
	CHECK(atSecond == 2);

	SUBCASE("and a point far from every handle finds none") {
		// Or the checks above are satisfied by a function that always answers.
		CHECK(mseg::handleAt(l, 0.5f, 0.5f, 0.01f, false) == -1);
	}

	SUBCASE("two handles at the SAME point resolve to the later one") {
		// THE CASE ABOVE DOES NOT TEST THE TIE-BREAK AND ITS KNOWN-BAD SAID SO. Those handles share an
		// x but differ in level, so the nearest-wins search never sees an exact tie — flipping the
		// comparison from `<=` to `<` left it green. The code's comment claimed to be preventing an
		// ungrabbable handle; nothing was checking that claim.
		//
		// A genuine tie needs both coordinates equal, which happens when a zero-duration segment ends
		// at the level it started from — a collapsed breakpoint sitting exactly on its neighbour. The
		// LATER one wins, because it is the one a hand has just collapsed and the one they will want
		// to drag back out.
		mseg::Settings tied = bare({ { 0.2, 0.7f }, { 0.0, 0.7f }, { 0.2, 0.3f } });
		tied.segment[1].share = kTimeLog2SecondsMin;
		const mseg::Layout t = mseg::layoutOf(tied);
		REQUIRE(t.handle.size() == 4);
		REQUIRE(t.handle[1].x == doctest::Approx(t.handle[2].x));
		REQUIRE(t.handle[1].level == doctest::Approx(t.handle[2].level));

		CHECK(mseg::handleAt(t, t.handle[2].x, t.handle[2].level, 0.05f, false) == 2);
	}
}

TEST_CASE("the drawn curve is the shape the engine plays") {
	// The picture and the sound come from the same arithmetic — asserted rather than assumed, because
	// "the card runs the real envelope" is exactly the claim the filter card could not make and
	// needed a case to hold together.
	mseg::Settings s = bare({ { 0.25, 1.0f }, { 0.25, 0.0f } });
	s.segment[0].shape = 0.5f; // a curve, so a straight-line drawing would disagree

	mseg::Shape shape;
	shape.prepare(kSampleRate);
	const std::vector<float> played = run(shape, s, 0.5);
	const auto drawn = mseg::polyline(s, 32);
	REQUIRE(played.size() > 100);
	REQUIRE(drawn.size() > 32);

	// Sample the drawing at a quarter across and the playing at a quarter of the way through, and
	// they must agree — through the CURVE, which is where a second formula would show up.
	const auto drawnAt = [&](float x) {
		float best = 9.0f;
		float value = 0.0f;
		for (const auto& p : drawn) {
			if (std::abs(p.first - x) < best) {
				best = std::abs(p.first - x);
				value = p.second;
			}
		}
		return value;
	};
	const auto playedAt = [&](double seconds) {
		return played[static_cast<size_t>(seconds / kTickSeconds) - 1];
	};

	CHECK(drawnAt(0.25f) == doctest::Approx(playedAt(0.125)).epsilon(0.05));
	CHECK(drawnAt(0.50f) == doctest::Approx(playedAt(0.250)).epsilon(0.05));
	CHECK(drawnAt(0.75f) == doctest::Approx(playedAt(0.375)).epsilon(0.05));

	SUBCASE("and the shaped segment is not a straight line, or the agreement is trivial") {
		// A curve of 0.5 must actually bend, or the case above would pass against any implementation
		// that happened to be linear in both places.
		const float quarter = drawnAt(0.25f);
		CAPTURE(quarter);
		CHECK(std::abs(quarter - 0.5f) > 0.08f);
	}
}

TEST_CASE("every device declares a whole Traits, and no two share a tag") {
	// A NULL TAG SHIPPED, AND THE ONLY REASON ANYONE LOOKED WAS A WARNING ABOUT A DIFFERENT FIELD.
	//
	// mseg::kTraits omitted `.tag`, so it was a null pointer. audition.cpp reads a device's tag as
	// four bytes without asking — `chain.device[0].tag[i] = tag[i]` — so the first commit to put an
	// MSEG in a slot would have read through null. A landmine timed to go off in somebody else's
	// commit, presenting as a fault in the rack wiring rather than in a declaration merged hours
	// earlier. `-Wmissing-field-initializers` named `hasExpanded`, the harmless half; cc followed it
	// to the dangerous one.
	//
	// ONE DEVICE CHECKING ITSELF IS THE HAND-KEPT-LIST SHAPE. test_fx_filter already asserted its own
	// tag was four bytes, and that did nothing for the device added afterwards — each device restates
	// its traits and nothing compares them. So this walks ALL of them, and the next device's omission
	// is a failing case rather than a warning somebody may or may not chase.
	struct Device {
		const char* what;
		const feathers::device::Traits* traits;
	};
	const std::array<Device, 3> kDevices { {
	    { "dahdsr", &feathers::mod::dahdsr::kTraits },
	    { "mseg", &feathers::mod::mseg::kTraits },
	    { "filter", &feathers::fx::filter::kTraits },
	} };

	for (const Device& d : kDevices) {
		CAPTURE(std::string(d.what));

		// THE TAG. Not null, and exactly four bytes — the width audition.cpp reads and compares.
		REQUIRE(d.traits->tag != nullptr);
		CHECK(std::string(d.traits->tag).size() == 4);

		// THE NAME. What a card's header says; a device with no name is a card with a blank heading.
		REQUIRE(d.traits->name != nullptr);
		CHECK(std::string(d.traits->name).size() > 0);
	}

	SUBCASE("and the tags are distinct, which nothing else enforces") {
		// A rack of instances identifies a device's record by its tag. Two devices sharing one is not
		// a compile error, not a warning, and not something any existing case would notice — the
		// symptom would be a patch loading one device's settings into another, which reads as
		// corruption rather than as a collision.
		for (size_t i = 0; i < kDevices.size(); ++i) {
			for (size_t j = i + 1; j < kDevices.size(); ++j) {
				CAPTURE(std::string(kDevices[i].what));
				CAPTURE(std::string(kDevices[j].what));
				CHECK(std::string(kDevices[i].traits->tag) != std::string(kDevices[j].traits->tag));
			}
		}
	}

	SUBCASE("hasExpanded is a decision, and every device has made one") {
		// It cannot be asserted to a VALUE — the right answer differs per device and the MSEG's will
		// change the day an expanded surface exists. What can be asserted is that nothing reads an
		// expanded view from a device that has not got one, which is the honesty rule for this field:
		// today no device declares one, so no host code can be relying on it.
		//
		// If this case ever fails, it is because a device declared true — at which point the question
		// is whether the surface exists, not whether to update this line.
		for (const Device& d : kDevices) {
			CAPTURE(std::string(d.what));
			CHECK(d.traits->hasExpanded == false);
		}
	}
}

TEST_CASE("a looping shape's period is the sum of its segments, at either sample rate") {
	// THE DAHDSR HAS THIS CASE AND THIS DEVICE DID NOT — every timing case in this file ran at 48 kHz
	// alone, which is exactly the rate at which a conversion between seconds and ticks can be wrong
	// and look right. What it found was not the conversion: the shape was spending PHASE where it
	// should have spent SECONDS, and a remainder carried across a segment boundary meant a different
	// number of seconds on the other side of it.
	//
	// THE INSTRUMENT IS THE POSITION, NOT THE OUTPUT. A first draft counted upward crossings of a
	// level only the top of the cycle reaches, and it reported a 30 ms shape as running at HALF its
	// rate — the crossing lives for under two ticks and the reader stepped over it. A shape passes
	// through the same level several times a cycle by construction, so a level is not evidence about
	// where it is; `position().segment` is, which is why it is exposed.
	//
	// THE RATE LOOP IS INSIDE EACH SUBCASE AND NOT AROUND THEM. Written the other way round — one
	// `for` over the rates with SUBCASEs inside — doctest enters a given subcase only once per run,
	// so the second rate silently did not execute and half of what this case claims was never tested.
	// The tell was its own warning: "DEEPEST SUBCASE STACK REACHED" naming the same subcase twice.
	const auto periodOf = [](double rate, const mseg::Settings& s, int cycles) {
		mseg::Shape shape;
		shape.prepare(rate);
		const double tick = static_cast<double>(feathers::mod::kBlockSamples) / rate;
		size_t was = 0;
		int wraps = 0;
		int ticks = 0;
		int first = -1;
		while (wraps <= cycles && ticks < 2000000) {
			shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
			++ticks;
			const size_t now = shape.position().segment;
			if (now < was) { // fell back towards the loop start: one cycle done
				if (first < 0) {
					first = ticks; // the run-in is not a cycle; start the clock at the first wrap
				}
				else {
					++wraps;
				}
			}
			was = now;
		}
		REQUIRE(first > 0);
		REQUIRE(wraps > cycles);
		return static_cast<double>(ticks - first) * tick / static_cast<double>(wraps);
	};

	SUBCASE("segments of unequal length") {
		// Three different durations, so a boundary that carried its remainder in the wrong segment's
		// units gets three chances per cycle to show it.
		mseg::Settings s = bare({ { 0.100, 1.0f }, { 0.150, 0.2f }, { 0.050, 0.8f } });
		s.mode = Mode::Loop;
		s.loopStart = 0;
		s.loopEnd = 2;
		mseg::sanitize(&s);

		for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const double tick = static_cast<double>(feathers::mod::kBlockSamples) / rate;
			const double period = periodOf(rate, s, 8);
			CAPTURE(period);
			// A QUARTER OF A TICK, ON A MEAN OVER EIGHT CYCLES — and the divisor is the point. The
			// reading quantises to the tick, but it does so ONCE across the whole span, so its
			// contribution to the mean is a tick over the cycle count and not a tick per cycle. A
			// per-cycle tick would be 1.45 ms at 44.1 kHz, which is looser than the fault this case
			// exists to catch: the old advance() measured 298.96 ms at 44.1 kHz and 299.26 at 48, and
			// a per-cycle tick would have let both through.
			// MEASURED with the fix: 0.06 ticks of error at 44.1 kHz and exactly zero at the other two.
			CHECK(std::abs(period - 0.300) <= tick / 4.0);
		}
	}

	SUBCASE("a short segment beside a long one, where the remainder is worth the most") {
		// THE SAME DEFECT, SIZED UP UNTIL IT IS UNMISTAKABLE. The error a mis-converted remainder
		// makes is the overshoot times the DIFFERENCE between the two segments' lengths, so it is
		// nearly invisible on segments of similar length and enormous on 2 ms next to 20 ms: an
		// overshoot worth most of a 2 ms segment was spent as that fraction of a 20 ms one, and the
		// cycle skipped most of the long segment.
		//
		// WHICH IS ALSO WHY THE FIRST DRAFT OF THIS SUBCASE PASSED AGAINST THE KNOWN-BAD. It used
		// three EQUAL 1 ms segments, on the theory that segments shorter than a tick were the
		// vulnerable ones. They are not: when every segment is the same length, phase is the same
		// currency in all of them and carrying a remainder unconverted is exactly right. Equal
		// segments are the one shape the old code got perfectly.
		mseg::Settings s = bare({ { 0.002, 1.0f }, { 0.020, 0.2f } });
		s.mode = Mode::Loop;
		s.loopStart = 0;
		s.loopEnd = 1;
		mseg::sanitize(&s);

		for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const double tick = static_cast<double>(feathers::mod::kBlockSamples) / rate;
			const double period = periodOf(rate, s, 20);
			CAPTURE(period);
			// MEASURED against the old advance(), where the knobs say 22 ms: 13.89 at 44.1 kHz,
			// 16.00 at 48, and exactly 22 at 96 — where 2 ms and 20 ms are 3 and 30 whole ticks, so
			// there is no remainder for it to mis-convert. Wrong at two rates and exact at the one
			// whose arithmetic comes out even is the mechanism's own signature.
			CHECK(std::abs(period - 0.022) <= tick / 4.0);
			// MEASURED with the fix: 0.02 ticks of error, at every rate.
			REQUIRE(tick / 4.0 < 0.022);
		}
	}

	SUBCASE("and the scale knob sets the window the period fills") {
		// THE KNOB'S MEANING MOVED AND SO DID THIS SUBCASE. It read "TimeScale is the MSEG's rate
		// control... doubling it doubles the period", and set `timeScale = 2.0f` expecting twice the
		// 80 ms the segments named. Under the time base the knob IS the window — log2 seconds when
		// there is no tempo — so 2.0f now means a four-SECOND window, and the period would be four
		// seconds rather than 160 ms.
		//
		// The claim survives in a better form: the period is the WINDOW, whatever the segments say.
		// A shape whose segments name 50 ms and 30 ms loops in 160 ms because the window is 160 ms,
		// not because 50 and 30 add to 80 and the knob doubled it. Set the window and the period
		// follows it exactly — which is the whole of what "the curve fits inside the window" buys.
		mseg::Settings s = bare({ { 0.050, 1.0f }, { 0.030, 0.2f } });
		s.mode = Mode::Loop;
		s.loopStart = 0;
		s.loopEnd = 1;
		s.timeScale = timeValueFor(0.160);
		mseg::sanitize(&s);

		for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const double tick = static_cast<double>(feathers::mod::kBlockSamples) / rate;
			const double period = periodOf(rate, s, 8);
			CAPTURE(period);
			CHECK(std::abs(period - 0.160) <= tick / 4.0);
		}
	}
}

TEST_CASE("a waypoint can be added, and adding one does not move the shape") {
	// ON THE INSTALLED BUILD, new waypoints could not be added. The device declares eight segments
	// and the editor could only ever drag the ones a patch already had.
	//
	// THE CONTRACT IS THAT NOTHING CHANGES BUT THE HANDLE COUNT. A gesture that adds a breakpoint and
	// also bends the sound is a gesture nobody can use to prepare an edit, so this asserts the SHAPE
	// either side of the split rather than that a segment appeared.
	mseg::Settings s = bare({ { 0.200, 1.0f }, { 0.200, 0.0f } });
	const mseg::Layout before = mseg::layoutOf(s);
	REQUIRE(before.segmentEnd.size() == 2);

	// SEVEN TENTHS THROUGH THE FIRST SEGMENT, DELIBERATELY NOT ITS MIDDLE. The first draft split at
	// x = 0.25, which on two equal segments is exactly segment 0's midpoint — so a withWaypointAdded
	// that ignored the pointer entirely and always halved the segment PASSED, and the case read as
	// obviously correct. Aim the probe where the right answer and the lazy one differ.
	const mseg::Settings split = mseg::withWaypointAdded(s, 0.35f);
	CHECK(split.count == 3);

	// THE SHAPE IS THE SAME CURVE. Sampled at twenty places across the whole width and compared
	// against the original — which is the assertion that catches a split that moved a level, took the
	// wrong duration, or dropped the segment's bend.
	const auto sampleAt = [](const mseg::Settings& shape, float x) {
		const mseg::Layout layout = mseg::layoutOf(shape);
		const int seg = mseg::segmentAt(layout, x);
		REQUIRE(seg >= 0);
		const size_t i = static_cast<size_t>(seg);
		const float left = i == 0 ? 0.0f : layout.segmentEnd[i - 1];
		const float span = layout.segmentEnd[i] - left;
		const float t = span > 0.0f ? (x - left) / span : 1.0f;
		// The polyline is the drawn curve; reading it at x is what the eye would compare.
		const std::vector<std::pair<float, float>> line = mseg::polyline(shape, 64);
		float best = line.front().second;
		float bestDx = 9.0f;
		for (const auto& p : line) {
			const float dx = std::abs(p.first - x);
			if (dx < bestDx) {
				bestDx = dx;
				best = p.second;
			}
		}
		(void)t;
		return best;
	};
	for (int i = 0; i <= 20; ++i) {
		const float x = static_cast<float>(i) / 20.0f;
		CAPTURE(x);
		CHECK(sampleAt(split, x) == doctest::Approx(sampleAt(s, x)).epsilon(0.02));
	}

	// AND THE NEW HANDLE IS WHERE THE HAND POINTED, which the sameness check above cannot say: a
	// split that did nothing at all would pass every one of those.
	const mseg::Layout after = mseg::layoutOf(split);
	REQUIRE(after.segmentEnd.size() == 3);
	CHECK(after.segmentEnd[0] == doctest::Approx(0.35f).epsilon(0.05));
	CHECK(after.handle.size() == before.handle.size() + 1);

	SUBCASE("and the loop markers follow the segments they named") {
		// A marker is an INDEX, so inserting below it moves what it points at. A loop over the second
		// segment must still be over that segment's music after a split in the first.
		mseg::Settings looped = bare({ { 0.100, 1.0f }, { 0.100, 0.5f }, { 0.100, 0.0f } });
		looped.mode = Mode::Loop;
		looped.loopStart = 1;
		looped.loopEnd = 2;
		mseg::sanitize(&looped);

		const mseg::Settings cut = mseg::withWaypointAdded(looped, 0.15f); // inside segment 0
		REQUIRE(cut.count == 4);
		CHECK(cut.loopStart == 2);
		CHECK(cut.loopEnd == 3);
		// The marked span is the same music: it still starts where the old segment 1 started.
		const mseg::Layout was = mseg::layoutOf(looped);
		const mseg::Layout now = mseg::layoutOf(cut);
		CHECK(now.loopStartX == doctest::Approx(was.loopStartX).epsilon(0.02));
		CHECK(now.loopEndX == doctest::Approx(was.loopEndX).epsilon(0.02));
	}

	SUBCASE("and a full shape refuses rather than dropping a segment to make room") {
		mseg::Settings full;
		full.count = mseg::kMaxSegments;
		for (size_t i = 0; i < mseg::kMaxSegments; ++i) {
			full.segment[i].share = feathers::mod::timeValueFor(0.1);
			full.segment[i].level = static_cast<float>(i) / static_cast<float>(mseg::kMaxSegments);
		}
		mseg::sanitize(&full);
		const mseg::Settings same = mseg::withWaypointAdded(full, 0.5f);
		CHECK(same.count == mseg::kMaxSegments);
		CHECK(same == full); // untouched, not merely the same length
	}
}

TEST_CASE("an opt-drag bends what it caught, and a waypoint bends both its sides in opposite ways") {
	// THE DESIGN: select a waypoint, which applies an inverse curve on either side, or select a segment;
	// the affected segments are highlighted while editing.
	//
	// THE VALUE, NOT THE PICTURE. A bend is a number on a segment; a rendered curve carries every
	// segment's number at once and cannot say which one moved.
	mseg::Settings s = bare({ { 0.100, 1.0f }, { 0.100, 0.0f }, { 0.100, 1.0f } });
	const mseg::Layout layout = mseg::layoutOf(s);
	REQUIRE(layout.segmentEnd.size() == 3);

	SUBCASE("a segment takes the bend alone") {
		// A point in the middle of segment 1, well away from either breakpoint.
		const float x = 0.5f * (layout.segmentEnd[0] + layout.segmentEnd[1]);
		const mseg::CurveTarget target = mseg::curveTargetAt(layout, x, 0.5f, 0.02f, false);
		REQUIRE(target.kind == mseg::CurveTarget::Kind::Segment);
		CHECK(target.index == 1);
		CHECK(target.first == 1);
		CHECK(target.count == 1); // and the highlight is that one segment

		const mseg::Settings bent = mseg::withCurvature(s, target, 0.25f);
		CHECK(bent.segment[1].shape == doctest::Approx(0.25f));
		// ITS NEIGHBOURS ARE UNTOUCHED, which is the half that separates this from the waypoint case.
		CHECK(bent.segment[0].shape == doctest::Approx(s.segment[0].shape));
		CHECK(bent.segment[2].shape == doctest::Approx(s.segment[2].shape));
	}

	SUBCASE("a waypoint bends the segment before it one way and the one after it the other") {
		// The breakpoint between segment 0 and segment 1 — handle 1, at the end of segment 0.
		const float x = layout.segmentEnd[0];
		const mseg::CurveTarget target = mseg::curveTargetAt(layout, x, layout.handle[1].level, 0.05f, false);
		REQUIRE(target.kind == mseg::CurveTarget::Kind::Waypoint);
		CHECK(target.index == 1);
		CHECK(target.first == 0);
		CHECK(target.count == 2); // both sides highlighted, which is the disambiguation

		const mseg::Settings bent = mseg::withCurvature(s, target, 0.3f);
		CHECK(bent.segment[0].shape == doctest::Approx(0.3f));
		CHECK(bent.segment[1].shape == doctest::Approx(-0.3f));
		// AND THE OPPOSITE SIGNS ARE THE CLAIM, stated so it cannot pass by both being 0.3.
		CHECK(bent.segment[0].shape == doctest::Approx(-bent.segment[1].shape));
		CHECK(bent.segment[2].shape == doctest::Approx(s.segment[2].shape));
	}

	SUBCASE("a waypoint at either end has one neighbour, and bends it") {
		// A CHOSEN BEHAVIOUR, not a specified one — see curveTargetAt. The alternative was to refuse
		// the end handles a curvature gesture, which would make two of the handles silently different
		// from every other.
		const mseg::CurveTarget start =
		    mseg::curveTargetAt(layout, 0.0f, layout.handle.front().level, 0.05f, false);
		REQUIRE(start.kind == mseg::CurveTarget::Kind::Waypoint);
		CHECK(start.index == 0);
		CHECK(start.count == 1);
		const mseg::Settings fromStart = mseg::withCurvature(s, start, 0.2f);
		CHECK(fromStart.segment[0].shape == doctest::Approx(-0.2f));

		const mseg::CurveTarget end =
		    mseg::curveTargetAt(layout, 1.0f, layout.handle.back().level, 0.05f, false);
		REQUIRE(end.kind == mseg::CurveTarget::Kind::Waypoint);
		CHECK(end.index == 3);
		CHECK(end.count == 1);
		const mseg::Settings toEnd = mseg::withCurvature(s, end, 0.2f);
		CHECK(toEnd.segment[2].shape == doctest::Approx(0.2f));
	}

	SUBCASE("and a bend cannot be driven past the declared limit") {
		const float x = 0.5f * (layout.segmentEnd[0] + layout.segmentEnd[1]);
		const mseg::CurveTarget target = mseg::curveTargetAt(layout, x, 0.5f, 0.02f, false);
		mseg::Settings bent = s;
		for (int i = 0; i < 50; ++i) {
			bent = mseg::withCurvature(bent, target, 0.5f);
		}
		CHECK(bent.segment[1].shape == doctest::Approx(feathers::mod::kShapeMax));
		for (int i = 0; i < 100; ++i) {
			bent = mseg::withCurvature(bent, target, -0.5f);
		}
		CHECK(bent.segment[1].shape == doctest::Approx(-feathers::mod::kShapeMax));
	}
}

TEST_CASE("a breakpoint's grab region is a circle on the glass, not an ellipse in the unit box") {
	// ON THE INSTALLED BUILD, waypoints were hard to grab — and the tolerance was only half
	// the story. The card asked for ONE radius and computed it as `max(9/width, 9/height)`, which
	// takes the LARGER of two pixel-to-unit conversions and then applies it to both axes. On the real
	// editor that is 24.7px of reach sideways and 9px vertically, from a number written as "9".
	//
	// THE NUMBERS BELOW ARE THE REAL CARD'S. 216px card, 9px padding each side, 72px editor:
	// see MsegCard::kPadX and kEditorHeight. A fixture that invented its own box would be asserting
	// about a shape editor nobody has.
	const float kEditorW = 216.0f - 2.0f * 9.0f; // 198
	const float kEditorH = 72.0f;
	const float kGrabPx = 13.0f;
	const float rx = kGrabPx / kEditorW;
	const float ry = kGrabPx / kEditorH;

	// A flat shape, so every breakpoint sits at the same height and only the geometry is under test.
	mseg::Settings s = bare({ { 0.200, 0.5f }, { 0.200, 0.5f }, { 0.200, 0.5f } });
	s.startLevel = 0.5f;
	mseg::sanitize(&s);
	const mseg::Layout layout = mseg::layoutOf(s);
	REQUIRE(layout.handle.size() == 4);

	const mseg::Handle& target = layout.handle[1];
	REQUIRE(target.level == doctest::Approx(0.5f));

	// TWELVE PIXELS EACH WAY IS A HIT, AND FIFTEEN IS A MISS — on BOTH axes, which is the whole
	// claim. A one-radius hit test cannot make both of these true at once on a box that is 198 by 72.
	const float in = 12.0f;
	const float out = 15.0f;
	CHECK(mseg::handleAt(layout, target.x + in / kEditorW, target.level, rx, ry, false) == 1);
	CHECK(mseg::handleAt(layout, target.x - in / kEditorW, target.level, rx, ry, false) == 1);
	CHECK(mseg::handleAt(layout, target.x, target.level + in / kEditorH, rx, ry, false) == 1);
	CHECK(mseg::handleAt(layout, target.x, target.level - in / kEditorH, rx, ry, false) == 1);

	CHECK(mseg::handleAt(layout, target.x, target.level + out / kEditorH, rx, ry, false) != 1);
	CHECK(mseg::handleAt(layout, target.x, target.level - out / kEditorH, rx, ry, false) != 1);

	// SIDEWAYS, THE MISS HAS TO BE STATED AGAINST A SHAPE WITH ROOM FOR IT. Handles here sit a third
	// of 198px apart — 66px — so 15px to the right of handle 1 is still nearer handle 1 than handle
	// 2, and "not handle 1" would be false for the wrong reason. The claim is the DISTANCE, so ask
	// for a point that is outside handle 1's circle and check nothing at all is caught: 15px out in
	// x AND 12px out in y is 1.32 tolerances away and inside no handle's region.
	CHECK(mseg::handleAt(layout, target.x + out / kEditorW, target.level + in / kEditorH, rx, ry,
	                     false)
	      == -1);

	SUBCASE("and the old one-radius reading really does differ, or the two radii buy nothing") {
		// THE CONTROL FOR THIS WHOLE CASE. If `max(rx, ry)` on both axes happened to agree with the
		// ellipse at these points, the assertions above would pass on a build that never took the
		// second radius — the mutation would be inert and the case decoration. It does not agree:
		// the old radius is ry = 0.1806, so 15px up (0.2083) misses either way, but 24px SIDEWAYS
		// (0.1212 in x) is inside the old circle and outside the new one.
		const float old = std::max(rx, ry);
		CHECK(old == doctest::Approx(ry));
		CHECK(mseg::handleAt(layout, target.x + 24.0f / kEditorW, target.level, old, false) == 1);
		CHECK(mseg::handleAt(layout, target.x + 24.0f / kEditorW, target.level, rx, ry, false) == -1);
	}
}

TEST_CASE("a waypoint can be removed, and the segment it ended dissolves into the next") {
	// NOT ASKED FOR — see the commit message. An editor that adds and cannot remove fills to
	// kMaxSegments in eight double-clicks and then refuses every further gesture, which is a worse
	// place to leave a hand than the one that was reported.
	//
	// THE TOTAL LENGTH IS PRESERVED, which is the contract that makes removal the inverse of adding
	// rather than a second kind of edit. Adding split a duration in two; removing puts the two back.
	mseg::Settings s = bare({ { 0.100, 1.0f }, { 0.300, 0.4f }, { 0.200, 0.0f } });
	const double totalBefore = mseg::totalSeconds(s);
	REQUIRE(totalBefore == doctest::Approx(0.600));

	// Handle 1 ends segment 0. Removing it merges segment 0 into segment 1.
	const mseg::Settings cut = mseg::withWaypointRemoved(s, 1);
	REQUIRE(cut.count == 2);

	// THE VALUES, NOT THE SHAPE OF THE ANSWER. The merged segment takes the two durations and the
	// FOLLOWING segment's level and bend — 0.1 + 0.3 seconds arriving at 0.4.
	CHECK(feathers::mod::timeSeconds(cut.segment[0].share) == doctest::Approx(0.400).epsilon(0.01));
	CHECK(cut.segment[0].level == doctest::Approx(0.4f));
	CHECK(feathers::mod::timeSeconds(cut.segment[1].share) == doctest::Approx(0.200).epsilon(0.01));
	CHECK(cut.segment[1].level == doctest::Approx(0.0f));
	CHECK(mseg::totalSeconds(cut) == doctest::Approx(totalBefore).epsilon(0.01));

	SUBCASE("and adding then removing at the same breakpoint is a round trip") {
		// THE INVERSE CLAIM, STATED AS ONE. A split at 0.35 of a two-segment shape puts a handle in
		// segment 0; removing that handle must give the durations back.
		mseg::Settings two = bare({ { 0.200, 1.0f }, { 0.200, 0.0f } });
		const mseg::Settings split = mseg::withWaypointAdded(two, 0.35f);
		REQUIRE(split.count == 3);
		const mseg::Settings back = mseg::withWaypointRemoved(split, 1);
		REQUIRE(back.count == 2);
		CHECK(feathers::mod::timeSeconds(back.segment[0].share)
		      == doctest::Approx(feathers::mod::timeSeconds(two.segment[0].share)).epsilon(0.02));
		CHECK(back.segment[0].level == doctest::Approx(two.segment[0].level));
		CHECK(back.segment[1].level == doctest::Approx(two.segment[1].level));
	}

	SUBCASE("and the markers follow the segment they named") {
		// THE FIRST DRAFT OF THIS SUBCASE PASSED ITS OWN KNOWN-BAD, and the reason is worth keeping.
		// It marked segments 2 and 3 while removing handle 1, so every marker sat STRICTLY ABOVE the
		// removal — and `segment >= handleIndex` and `segment > handleIndex` give the same answer for
		// every one of those. The mutation was absorbed by slack: the probe was aimed exactly where
		// the right rule and the off-by-one rule agree.
		//
		// So the markers below sit on all three sides of the removal. Handle 1 dissolves segment 0
		// into segment 1, and the merged segment lands at index 0 — so a marker naming segment 0 AND
		// a marker naming segment 1 both end up naming 0, and only a marker above them slides down.
		mseg::Settings looped = bare({ { 0.100, 1.0f }, { 0.100, 0.5f }, { 0.100, 0.2f },
		                               { 0.100, 0.0f } });
		looped.mode = Mode::Loop;
		looped.loopStart = 1; // == the removal, the case the off-by-one gets wrong
		looped.loopEnd = 3;   // above it
		looped.sustainSegment = 1;
		mseg::sanitize(&looped);

		const mseg::Settings cutLoop = mseg::withWaypointRemoved(looped, 1);
		REQUIRE(cutLoop.count == 3);
		CHECK(cutLoop.loopStart == 0);
		CHECK(cutLoop.loopEnd == 2);
		CHECK(cutLoop.sustainSegment == 0);

		// AND A MARKER BELOW THE REMOVAL DOES NOT MOVE — the third side, and the one an unconditional
		// decrement gets wrong. It needs a marker strictly between 0 and the removal, which is why
		// loopStart is 1 here and not 0: `remap` and `segment == 0 ? 0 : segment - 1` agree on 0 and
		// on everything at or above the removal, and disagree only in the gap between them. Removing
		// handle 3 dissolves segment 2, so segment 1 is inside that gap. Sustain stays at 0 to pin the
		// floor as well, which is a different claim and a cheap one.
		mseg::Settings below = bare({ { 0.100, 1.0f }, { 0.100, 0.5f }, { 0.100, 0.2f },
		                              { 0.100, 0.0f } });
		below.mode = Mode::Loop;
		below.loopStart = 1;
		below.loopEnd = 3;
		below.sustainSegment = 0;
		mseg::sanitize(&below);
		const mseg::Settings cutBelow = mseg::withWaypointRemoved(below, 3); // dissolves segment 2
		REQUIRE(cutBelow.count == 3);
		CHECK(cutBelow.loopStart == 1);
		CHECK(cutBelow.loopEnd == 2);
		CHECK(cutBelow.sustainSegment == 0);
	}

	SUBCASE("and the two handles with no merge partner refuse") {
		// Handle 0 is the shape's START — not a segment boundary at all. The last handle is its END,
		// and there is no following segment to give the time to, so dissolving it would silently
		// shorten the shape.
		CHECK(mseg::withWaypointRemoved(s, 0) == s);
		CHECK(mseg::withWaypointRemoved(s, 3) == s);
		CHECK(mseg::withWaypointRemoved(s, 99) == s);

		// AND A ONE-SEGMENT SHAPE CANNOT BE EMPTIED. Its only interior handle is its last one.
		mseg::Settings one = bare({ { 0.200, 1.0f } });
		CHECK(mseg::withWaypointRemoved(one, 1) == one);
	}
}

TEST_CASE("a breakpoint lands where the pointer is, in one event, and stops at a wall it can name") {
	// ON THE INSTALLED BUILD, MSEG waypoints at the far right extreme seemed to become ungrabbable.
	//
	// IT IS NOT THE HIT TEST, AND THAT WAS MEASURED BEFORE ANY CODE MOVED. The selectable pixel area
	// of the leftmost, middle and rightmost handles on the real card geometry is 536, 606 and 530
	// square pixels — symmetric, and the extremes within 13% of the middle. Nothing there is
	// edge-dependent. The reported feel is the DRAG.
	//
	// `withHandleMoved` used to set the duration to `span * totalSeconds(s)` — a span measured in the
	// unit box, times the shape's current length. Writing it changes the length, and the box is
	// normalised BY the length, so the handle landed short and the next mouse event repeated from
	// there: a fixed-point iteration wearing the clothes of an assignment.
	mseg::Settings s = bare({ { 0.200, 1.0f }, { 0.200, 0.5f }, { 0.200, 0.0f } });
	const auto handleX = [](const mseg::Settings& shape, size_t h) {
		const mseg::Layout layout = mseg::layoutOf(shape);
		REQUIRE(h < layout.handle.size());
		return layout.handle[h].x;
	};
	REQUIRE(handleX(s, 1) == doctest::Approx(1.0f / 3.0f));

	// ONE EVENT, AND THE VALUE IS THE POINTER'S OWN x. Not "closer than it was" — the number.
	// MEASURED BEFORE THE FIX for the same single event: 0.4286 against a pointer at 0.5.
	//
	// **WITHIN REACH, WHICH SINCE 2026-09-20 MEANS "NOT PAST THE NEIGHBOUR".** This used to also ask
	// for 0.70 and 0.90 and got them, because a drag could inflate the shape and travel as far as the
	// duration cap allowed. It trades with its neighbour now (see `withHandleMoved`), so handle 1 can
	// reach handle 2 and no further — on three even segments that is 2/3, and 0.70 and 0.90 are both
	// on the other side of it. They are asserted below as the wall rather than deleted, because
	// "where does it stop" is the more useful question and it now has a nameable answer.
	CHECK(handleX(mseg::withHandleMoved(s, 1, 0.50f, 1.0f), 1) == doctest::Approx(0.50f).epsilon(0.002));
	CHECK(handleX(mseg::withHandleMoved(s, 1, 0.60f, 1.0f), 1) == doctest::Approx(0.60f).epsilon(0.002));

	SUBCASE("and a second event on a parked pointer changes nothing, which is what 'solved' means") {
		// THE HALF THAT SEPARATES A SOLVE FROM A FASTER ITERATION. A drag delivers events for as long
		// as the hand holds still; if the answer moved on each one the handle would creep. Asked at a
		// pointer BEYOND the neighbour, which is where a creep would be easiest to hide.
		const mseg::Settings once = mseg::withHandleMoved(s, 1, 0.90f, 1.0f);
		const mseg::Settings twice = mseg::withHandleMoved(once, 1, 0.90f, 1.0f);
		CHECK(handleX(twice, 1) == doctest::Approx(handleX(once, 1)));
		CHECK(twice.segment[0].share == doctest::Approx(once.segment[0].share));
	}

	SUBCASE("and the wall is the NEIGHBOUR, which is a wall a hand can see") {
		// **THIS SUBCASE USED TO NAME TWO WALLS AND MEASURE A THIRD NUMBER**, and the arithmetic was
		// right about all of it: reaching x = 0.98 needed 19.6 s in one segment against a 10 s cap, and
		// then `shares()`'s visibility floor put the drawn stop at 1 - 2 * kMinLiveShare = 0.92 rather
		// than where the cap alone implied. It was a careful account of a wall made of two numbers a
		// player could not see, at a place nothing on screen marks.
		//
		// **TRADING REPLACES IT WITH A WALL THAT IS DRAWN.** A handle can take all of the next
		// segment's time and no more, so it stops ON its neighbour — and the neighbour is a dot on the
		// card. The stop is where the hand can see it should be, which is the whole improvement; the
		// duration cap and the floor are still there and are simply no longer reachable by this
		// gesture.
		const float neighbour = handleX(s, 2);
		REQUIRE(neighbour == doctest::Approx(2.0f / 3.0f));

		for (const float aim : { 0.70f, 0.90f, 0.98f }) {
			CAPTURE(aim);
			const mseg::Settings pushed = mseg::withHandleMoved(s, 1, aim, 1.0f);
			// IT PARKS ON THE NEIGHBOUR, wherever the pointer went past it.
			CHECK(handleX(pushed, 1) == doctest::Approx(neighbour).epsilon(0.002));
			// AND THE NEIGHBOUR DID NOT RETREAT — it is a wall, not a thing being pushed along.
			CHECK(handleX(pushed, 2) == doctest::Approx(neighbour).epsilon(0.002));
			// THE SEGMENT BETWEEN THEM IS SPENT, which is what "took all of it" means.
			CHECK(timeSeconds(pushed.segment[1].share) == doctest::Approx(0.0));
			// AND THE SHAPE IS THE LENGTH IT WAS, at the extreme where the old model inflated it most.
			CHECK(mseg::totalSeconds(pushed) == doctest::Approx(mseg::totalSeconds(s)));
		}

		// ALREADY THERE AFTER ONE EVENT. Measured before the solve: 0.5951 after one, 0.9200 only
		// after thirty — the creep this case was written for, asserted against the new wall.
		const mseg::Settings pushed = mseg::withHandleMoved(s, 1, 0.98f, 1.0f);
		const mseg::Settings again = mseg::withHandleMoved(pushed, 1, 0.98f, 1.0f);
		CHECK(handleX(again, 1) == doctest::Approx(handleX(pushed, 1)));
	}
}

TEST_CASE("the last breakpoint takes no horizontal move, and no longer destroys its segment for one") {
	// THE OTHER HALF OF "ungrabbable at the far right extreme", and the destructive half.
	//
	// The last handle's x is 1.0 BY CONSTRUCTION: the box is normalised to the shape's own length and
	// the last breakpoint IS the end of the shape. So a horizontal drag on it cannot move it, and a
	// hand pulling at it correctly reports that nothing happens.
	//
	// EXCEPT SOMETHING DID. MEASURED on the installed build: dragging the last handle to ANY x below
	// the right edge drove the final segment's duration to zero — 0.200 s to 0.000 s, the shape from
	// 0.60 s to 0.40 s — while the handle did not move a pixel. A gesture that looks dead and is not
	// is worse than one that is dead, so the horizontal component is now genuinely ignored.
	mseg::Settings s = bare({ { 0.200, 1.0f }, { 0.200, 0.5f }, { 0.200, 0.0f } });
	const size_t last = mseg::layoutOf(s).handle.size() - 1;
	REQUIRE(last == 3);
	const double before = mseg::totalSeconds(s);
	REQUIRE(before == doctest::Approx(0.600));

	for (const float pointer : { 0.20f, 0.50f, 0.80f, 1.00f }) {
		CAPTURE(pointer);
		const mseg::Settings dragged = mseg::withHandleMoved(s, last, pointer, 0.0f);
		// THE VALUE, NOT "ROUGHLY UNCHANGED": the final segment keeps the duration it had.
		CHECK(feathers::mod::timeSeconds(dragged.segment[2].share) == doctest::Approx(0.200).epsilon(0.01));
		CHECK(mseg::totalSeconds(dragged) == doctest::Approx(before).epsilon(0.01));
		CHECK(mseg::layoutOf(dragged).handle[last].x == doctest::Approx(1.0f));
	}

	SUBCASE("and its LEVEL still moves, or the handle would be inert rather than horizontal-proof") {
		// The claim is about the x component alone. A last handle that had stopped responding
		// entirely would pass every assertion above and be a worse bug than the one being fixed.
		const mseg::Settings lifted = mseg::withHandleMoved(s, last, 0.5f, 0.75f);
		CHECK(lifted.segment[2].level == doctest::Approx(0.75f));
		CHECK(feathers::mod::timeSeconds(lifted.segment[2].share) == doctest::Approx(0.200).epsilon(0.01));
	}
}

TEST_CASE("every division has a dotted and a triplet, and they mean what they say") {
	// The MSEG's sync had no dotted or triplet divisions.
	//
	// THE ARITHMETIC IS THE ASSERTION, because that is what dotted and triplet MEAN: a dotted note is itself
	// plus half again, and three triplets occupy the space of two straight notes. A case that only
	// checked the table had 27 entries would pass for a table of 27 wrong numbers.
	using namespace feathers::mod::mseg;
	REQUIRE(kDivisionCount % 3 == 0);
	REQUIRE(std::size(kDivisionNames) == kDivisionCount);

	for (size_t base = 0; base < kDivisionCount; base += 3) {
		CAPTURE(base);
		const double straight = kDivisionBeats[base];
		CHECK(kDivisionBeats[base + 1] == doctest::Approx(straight * 1.5));
		CHECK(kDivisionBeats[base + 2] == doctest::Approx(straight * 2.0 / 3.0));

		// AND THE NAMES MATCH THE NUMBERS. A table where the arithmetic is right and the labels are
		// off by one is worse than one that is simply wrong, because the control lies quietly.
		const std::string s = kDivisionNames[base];
		CHECK(std::string(kDivisionNames[base + 1]) == s + ".");
		CHECK(std::string(kDivisionNames[base + 2]) == s + "T");
	}

	// THE STRAIGHT DIVISIONS STILL DOUBLE, which is what says the bases were not disturbed while the
	// flavours were inserted between them.
	for (size_t base = 3; base < kDivisionCount; base += 3) {
		CAPTURE(base);
		CHECK(kDivisionBeats[base] == doctest::Approx(kDivisionBeats[base - 3] * 2.0));
	}

	// AND THE DEFAULT IS STILL ONE BAR -- pinned by MEANING rather than by index, so a future
	// reordering fails here instead of silently defaulting the device to a dotted sixteenth.
	CHECK(kDivisionBeats[kDefaultDivision] == doctest::Approx(4.0));
	CHECK(std::string(kDivisionNames[kDefaultDivision]) == "1 bar");
}

TEST_CASE("the MSEG's Enabled survives a round trip through its own Settings") {
	// **THE MSEG'S ENABLE GOT WEDGED, STUCK UNTIL RELOAD.** This is why.
	//
	// `Param::Enabled` was added to the descriptor so the shared head draws a bypass square and
	// `Traits::enableControl` points at it — and NOTHING in `mseg.cpp` was told. `Settings` has no
	// `enabled` member, `rowOf` never writes that index, and `settingsOf` never reads it.
	//
	// **`rowOf` OPENS WITH `Row row {}`, SO THE UNWRITTEN INDEX IS ZERO — WHICH IS "OFF".** Any road
	// that round-trips the device through its own settings therefore does not merely lose the switch,
	// it actively turns it OFF; and the mod panel round-trips on every edit. So the square could be
	// pressed, would light, and would snap back the moment anything else about the MSEG was touched.
	// A control that is drawn, pressed and does not stick is the honesty rule's own failure case.
	//
	// THE DAHDSR DOES ROUND-TRIP ITS ENABLE (`dahdsr.cpp` puts `s.enabled`), which is exactly why
	// it was seen on the MSEG and not on the envelope.
	mseg::Settings s;
	s.enabled = false;

	SUBCASE("off survives") {
		const mseg::Row row = mseg::rowOf(s);
		CHECK(row[static_cast<size_t>(mseg::Param::Enabled)] == doctest::Approx(0.0f));
		CHECK_FALSE(mseg::settingsOf(row, row.size()).enabled);
	}

	SUBCASE("and so does on — which is the half that was broken") {
		s.enabled = true;
		const mseg::Row row = mseg::rowOf(s);
		// THE VALUE, NOT MERELY "DIFFERENT FROM OFF": the field was zero because nothing wrote it, so
		// an assertion that the two round trips differ would have passed on a build that wrote 0 and
		// 0.5. This is the index the head reads through `enableControl`.
		CHECK(row[static_cast<size_t>(mseg::Param::Enabled)] == doctest::Approx(1.0f));
		CHECK(mseg::settingsOf(row, row.size()).enabled);
	}

	SUBCASE("a row too short to carry it reads the declared default, not zero") {
		// `settingsOf`'s own contract: a field the row does not reach is at its DECLARED default. The
		// declared default is 1.0 — a modulator added to a rack should modulate — so a patch saved by
		// a narrower build must come back ON rather than silently bypassed.
		mseg::Row row = mseg::rowOf(s);
		const size_t shortLength = static_cast<size_t>(mseg::Param::Enabled);
		CHECK(mseg::settingsOf(row, shortLength).enabled);
	}
}

TEST_CASE("a FREE shape is positioned by the song, on the timeline its own window uses") {
	// **BACKLOG DQ (2026-08-24).** `mod/modulator.h` recorded that
	// `Trigger::Free` had never been transport-anchored — its doc claimed *"the shape stays where the
	// transport put it"* and the correction says the device *"HAS NEVER HAD"* it, because nothing
	// passed a song position. The clock carries one now.
	//
	// **ASSERTED ON THE POSITION, NOT ON THE OUTPUT.** One level stands for a segment, a phase and a
	// curve at once, so a case reading `tick()`'s return could not tell "the song put us two-thirds
	// through segment 1" from any other way of arriving at the same number.
	namespace md = feathers::mod;
	const mseg::Settings base = bare({ { 0.10, 1.0f }, { 0.10, 0.0f } });
	mseg::Settings s = base;
	s.mode = md::Mode::Free;

	SUBCASE("with no tempo it reads SECONDS, because the window IS the scale in seconds") {
		mseg::Shape shape;
		shape.prepare(kSampleRate);

		md::TransportClock clock {};
		clock.playing = true;
		// Two equal segments, so a quarter of the way through the window is halfway through segment 0.
		const double window = feathers::mod::timeSeconds(s.timeScale);
		REQUIRE(window > 0.0);
		clock.songPositionSeconds = window * 0.25;

		// **THE CLOCK IS HELD BY `tick`, so the reading is asked AFTER one.** `lockedTurns` answers
		// from the clock the shape was last given, which is the same arrangement `effectiveRateHz`
		// has on automod: a device reports about the block it was told about, not about a block that
		// has not happened.
		shape.tick(s, false, false, clock);
		const std::optional<double> turns = shape.lockedTurns(s);
		REQUIRE(turns.has_value());
		CHECK(*turns == doctest::Approx(0.25));

		CHECK(shape.position().segment == 0);
		CHECK(shape.position().phase == doctest::Approx(0.5).epsilon(0.05));

		// AND THE SECOND SEGMENT IS REACHED BY THE SONG rather than by waiting — three quarters
		// through the window is halfway through segment 1.
		clock.songPositionSeconds = window * 0.75;
		shape.tick(s, false, false, clock);
		CHECK(shape.position().segment == 1);
		CHECK(shape.position().phase == doctest::Approx(0.5).epsilon(0.05));
	}

	SUBCASE("with a tempo it reads BEATS, so a tempo change does not teleport the shape") {
		// **THIS IS WHY THE UNIT FOLLOWS THE WINDOW.** `advance`'s own comment warns that a phase
		// "derived from an absolute elapsed time divided by a window that has since changed"
		// teleports a shape when a tempo ramp crosses a block. A beat-denominated window read against
		// a beat position moves neither term, so the hazard cannot arise.
		mseg::Shape shape;
		shape.prepare(kSampleRate);

		const double windowBeats =
		    feathers::device::kDivisionBeats[std::min(s.timeBase, feathers::device::kDivisionCount - 1)]
		    * feathers::mod::timeSeconds(s.timeScale);
		REQUIRE(windowBeats > 0.0);

		md::TransportClock slow = md::TransportClock::fromSecondsPerBeat(0.5); // 120 bpm
		slow.playing = true;
		slow.songPositionBeats = windowBeats * 0.25;
		slow.songPositionSeconds = 1234.0; // nonsense on purpose: a beats road must not reach for it
		const std::optional<double> atSlow = shape.lockedTurns(s);
		REQUIRE_FALSE(atSlow.has_value()); // no clock held yet — the shape has not ticked

		shape.tick(s, false, false, slow);
		const std::optional<double> turns = shape.lockedTurns(s);
		REQUIRE(turns.has_value());
		CHECK(*turns == doctest::Approx(0.25));

		// **THE SAME BEAT AT DOUBLE TEMPO IS THE SAME PLACE IN THE SHAPE.** A seconds-derived lock
		// could not promise this, and it is the whole reason a synced window reads beats.
		md::TransportClock fast = md::TransportClock::fromSecondsPerBeat(0.25); // 240 bpm
		fast.playing = true;
		fast.songPositionBeats = slow.songPositionBeats;
		fast.songPositionSeconds = 4321.0;
		shape.tick(s, false, false, fast);
		CHECK(*shape.lockedTurns(s) == doctest::Approx(0.25));
	}

	SUBCASE("a STOPPED transport does not lock, and a RETRIGGER shape is never locked") {
		mseg::Shape shape;
		shape.prepare(kSampleRate);
		md::TransportClock stopped {};
		stopped.playing = false;
		stopped.songPositionSeconds = 5.0;
		shape.tick(s, false, false, stopped);
		CHECK_FALSE(shape.lockedTurns(s).has_value());

		// **AND THE MODE IS THE GATE, which is what stops this reaching an envelope that is meant to
		// start where the note starts.** `lockedTurns` answers for any settings — the caller decides —
		// so the claim asserted here is the CALLER's: a Retrigger shape gated at a song position that
		// would have locked it stays at its own beginning.
		mseg::Settings retrig = base;
		retrig.mode = Mode::Gate;
		mseg::Shape other;
		other.prepare(kSampleRate);
		md::TransportClock rolling {};
		rolling.playing = true;
		rolling.songPositionSeconds = feathers::mod::timeSeconds(retrig.timeScale) * 0.75;
		other.tick(retrig, true, false, rolling);
		CHECK(other.position().segment == 0);
		CHECK(other.position().phase < 0.2f);
	}
}

TEST_CASE("ping-pong reverses at the loop boundaries instead of wrapping") {
	// A THREE-SEGMENT LOOP (0, 1, 2) with ping-pong should play 0 → 1 → 2 → 1 → 0 → 1 → 2 → 1 → 0 ...
	// rather than the forward-only 0 → 1 → 2 → 0 → 1 → 2 ...
	mseg::Settings s = bare({ { 0.1, 1.0f }, { 0.1, 0.5f }, { 0.1, 0.0f } });
	s.mode = Mode::Loop;
	s.loopStart = 0;
	s.loopEnd = 2;
	s.pingPong = true;
	mseg::Shape shape;
	shape.prepare(kSampleRate);

	// Each segment takes 0.1s at scale 1.0; at 64 samples/tick and 48kHz, that's ~75 ticks per segment.
	// A full ping-pong cycle (0→1→2→1→0) is ~375 ticks; we sample 500 to capture it.
	std::vector<size_t> segments;
	for (int i = 0; i < 500; ++i) {
		shape.tick(s, true, false, TransportClock::unknown());
		segments.push_back(shape.position().segment);
	}

	// FIND THE TRANSITIONS between segments. Each segment lasts ~75 ticks, so consecutive
	// same-segment entries are the shape working through that segment. We want the points where
	// the segment index changes.
	std::vector<size_t> transitions;
	for (size_t i = 1; i < segments.size(); ++i) {
		if (segments[i] != segments[i - 1]) {
			transitions.push_back(segments[i]);
		}
	}

	// THE PING-PONG SEQUENCE: 0 → 1 → 2 → 1 → 0 → 1 → 2 → 1 → 0 ...
	// Verify the first several transitions match this pattern.
	REQUIRE(transitions.size() >= 6);
	CHECK(transitions[0] == 1); // 0 → 1
	CHECK(transitions[1] == 2); // 1 → 2
	CHECK(transitions[2] == 1); // 2 → 1 (ping-pong reversal, NOT 2 → 0)
	CHECK(transitions[3] == 0); // 1 → 0
	CHECK(transitions[4] == 1); // 0 → 1 (ping-pong reversal)
	CHECK(transitions[5] == 2); // 1 → 2
}


TEST_CASE("levelAtX is the DRAWN line: it agrees with the polyline at every point the editor plots") {
	// A shape used as its picture (Featherverb's IR envelope) must apply exactly the line it draws, so
	// this checks the evaluator against the drawing itself, curves and a step included.
	mseg::Settings s = bare({ { 0.2, 1.0f }, { 0.0, 0.3f }, { 0.5, 0.8f }, { 0.05, 0.1f } });
	s.segment[0].shape = 0.7f;
	s.segment[2].shape = -0.5f;
	s.startLevel = 0.2f;
	const auto pts = mseg::polyline(s, 24);
	double worst = 0.0;
	for (size_t i = 0; i < pts.size(); ++i) {
		// A step draws two points at one x; the evaluator answers the level AFTER it, as the sound does.
		const bool stepFoot = i + 1 < pts.size() && pts[i + 1].first == pts[i].first;
		if (stepFoot) {
			continue;
		}
		worst = std::max(worst, static_cast<double>(std::fabs(mseg::levelAtX(s, pts[i].first) - pts[i].second)));
	}
	CAPTURE(worst);
	CHECK(worst < 1.0e-5);
	CHECK(mseg::levelAtX(s, -1.0f) == doctest::Approx(0.2f));
	CHECK(mseg::levelAtX(s, 2.0f) == doctest::Approx(0.1f));
}

TEST_CASE("a FREE shape under a Rate route FOLLOWS the song's step rather than dividing its position — it is not thrown between segments") {
	// **FREE SHAPES WERE RETRIGGERING AFTER ONE SEGMENT.** The lock placed the shape at
	// `position / window` every tick, so a route into Rate moved it by `turns × Δwindow / window` — MEASURED
	// from beat 64, a 5% route read segments 1, 2, 0, 2, 1, 0, 2 at 67 ms intervals. Option A (the
	// decision): the song's STEP is followed against the window as it is now, and the shape re-anchors to the
	// quotient only where the song jumped. See `mod::TransportLock`.
	//
	// ASSERTED AS THE POSITION, RESTATED INDEPENDENTLY: an accumulator `T += Δbeats / W_now` anchored at
	// `beats / W` on the first tick, plus the one tick `advance` spends after the lock. Three equal
	// segments, so the position in turns is `(segment + phase) / 3`.
	mseg::Settings s = bare({ { 1.0, 1.0f }, { 1.0, 0.0f }, { 1.0, 1.0f } });
	s.mode = Mode::Free;
	s.timeScale = 0.0f; // one division
	const double division = feathers::device::kDivisionBeats[std::min(s.timeBase, feathers::device::kDivisionCount - 1)];
	const auto windowOf = [&](float scale) { return division * timeSeconds(scale); };
	const auto turnsOf = [](const mseg::Shape& shape) {
		return (static_cast<double>(shape.position().segment) + static_cast<double>(shape.position().phase)) / 3.0;
	};
	const auto circular = [](double a, double b) {
		double d = (a - std::floor(a)) - (b - std::floor(b));
		return std::abs(d - std::round(d));
	};

	constexpr double kSecondsPerBeat = 0.5; // 120 bpm
	const double tickBeats = kTickSeconds / kSecondsPerBeat;
	TransportClock clock = TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
	clock.playing = true;

	mseg::Shape shape;
	shape.prepare(kSampleRate);

	SUBCASE("unmodulated, the position IS the song divided by the window — backlog DQ holds") {
		double worst = 0.0;
		for (int k = 0; k < 3000; ++k) {
			clock.songPositionBeats = 64.0 + k * tickBeats;
			shape.tick(s, false, false, clock);
			const double w = windowOf(s.timeScale);
			worst = std::max(worst, circular(turnsOf(shape), clock.songPositionBeats / w + tickBeats / w));
		}
		CHECK(worst < 1e-5);
	}

	SUBCASE("with the Rate moving, the position is the song's steps summed against each window") {
		double expected = 0.0;
		double worst = 0.0;
		double quotientMiss = 0.0; // how far the OLD rule would have put it: the case's non-vacuity
		for (int k = 0; k < 3000; ++k) {
			clock.songPositionBeats = 64.0 + k * tickBeats;
			s.timeScale = 0.4f * static_cast<float>(std::sin(2.0 * M_PI * k / 700.0)); // a route, as it resolves
			const double w = windowOf(s.timeScale);
			expected = k == 0 ? clock.songPositionBeats / w : expected + tickBeats / w;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(turnsOf(shape), expected + tickBeats / w));
			quotientMiss = std::max(quotientMiss, circular(clock.songPositionBeats / w, expected));
		}
		CAPTURE(worst);
		CAPTURE(quotientMiss);
		CHECK(worst < 1e-4);
		// THE ROUTE IS STRONG ENOUGH THAT THE OLD RULE WOULD HAVE LANDED ELSEWHERE — a third of a cycle and
		// more — so the check above is not passing because both rules agree.
		REQUIRE(quotientMiss > 0.3);

		SUBCASE("and a jump in the song — a loop wrapping back — re-anchors to the quotient") {
			clock.songPositionBeats -= 16.0;
			shape.tick(s, false, false, clock);
			const double w = windowOf(s.timeScale);
			CHECK(circular(turnsOf(shape), clock.songPositionBeats / w + tickBeats / w) < 1e-5);
			// AND FOLLOWS FROM THERE: the next ordinary step moves it by one tick's worth.
			const double before = turnsOf(shape);
			clock.songPositionBeats += tickBeats;
			shape.tick(s, false, false, clock);
			CHECK(circular(turnsOf(shape), before + tickBeats / w) < 1e-5);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// SMOOTHING CLOSES — the DAHDSR's bug (in smooth mode it never fully closed) asked of this       //
// device. A finished shape HOLDS its last level by design, so its rest is                          //
// that level; the smoothed output must ARRIVE there exactly, and `idle()` must not say finished     //
// while the one-pole is still carrying a tail.                                                     //
//------------------------------------------------------------------------------------------------//

namespace {

/// A millionth of full scale, -120 dB: the most a snap onto rest may cut off. Stated here, not read
/// from the device.
constexpr double kSnapBound = 1e-6;

} // namespace

TEST_CASE("smoothing closes: a finished shape's smoothed output reaches its last level EXACTLY, within the shape + tau ln(1e6), and idle() waits for it") {
	for (const float smoothing : { feathers::mod::kSmoothingLog2SecondsMin, -9.0f, -5.0f, -2.0f,
	                               feathers::mod::kSmoothingLog2SecondsMax }) {
		for (const float last : { 0.0f, 0.3f, 1.0f }) {
			for (const Mode mode : { Mode::Gate, Mode::Once }) {
				for (const bool bipolar : { false, true }) {
					CAPTURE(smoothing);
					CAPTURE(last);
					CAPTURE(static_cast<int>(mode));
					CAPTURE(bipolar);
					// Up to 0.8 over 50 ms, then to `last` over 50 ms: the shape moves, then holds.
					mseg::Settings s = bare({ { 0.05, 0.8f }, { 0.05, last } });
					s.smoothing = smoothing;
					s.mode = mode;
					s.bipolar = bipolar;
					// THE REST, restated from the declaration: the last level, rescaled when bipolar.
					const float rest = bipolar ? 2.0f * last - 1.0f : last;
					const double tau = feathers::mod::smoothingTau(smoothing);

					mseg::Shape shape;
					shape.prepare(kSampleRate);
					const int bound = static_cast<int>((0.1 + tau * std::log(1.0 / kSnapBound) + 0.05) / kTickSeconds);
					int closedAt = -1;
					int idleWithATail = 0;
					float was = shape.value();
					double cutOff = 0.0;
					for (int i = 0; i < bound; ++i) {
						const float v = shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
						if (shape.idle() && v != rest) {
							++idleWithATail;
						}
						if (v == rest && closedAt < 0 && shape.idle()) {
							// WHAT THE SNAP TOOK: the residue an un-snapped one-pole, restated, would still carry.
							const double keep = tau > 0.0 ? std::exp(-kTickSeconds / tau) : 0.0;
							cutOff = std::abs(static_cast<double>(was) - static_cast<double>(rest)) * keep / (bipolar ? 2.0 : 1.0);
							closedAt = i;
						}
						was = v;
					}
					CAPTURE(closedAt);
					CAPTURE(static_cast<double>(shape.value()) - static_cast<double>(rest));
					CHECK(shape.value() == rest);
					CHECK(shape.idle());
					CHECK(idleWithATail == 0);
					// THE SNAP IS INAUDIBLE (plus one float epsilon for reading it back through a float).
					CHECK(cutOff <= kSnapBound + static_cast<double>(std::numeric_limits<float>::epsilon()));
					// AND IT STAYS THERE.
					(void)run(shape, s, 0.1);
					CHECK(shape.value() == rest);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG PA ON THE MSEG: STEREO PHASE — the right ear is the same shape read `stereoPhase / 2π` of the WINDOW further
// on (360° is the window, the unit Phase is in), through the same loop, hold and end as the left ear. At 0 it is
// today's MSEG to the bit.
namespace {
namespace pa {
/// A TICK OF 64 SAMPLES AT 8192 Hz IS 2^-7 s, and the window is one second (no tempo, Rate 0): 128 ticks a window, so
/// 90° is exactly 32 ticks and 180° exactly 64, and every segment below is a whole number of ticks long. "Later" can then
/// be read off the LEFT EAR'S OWN FUTURE rather than off a restatement of the walk.
constexpr double kSampleRate = 8192.0;
constexpr int kTicksPerWindow = 128;

/// Three segments of a quarter, a quarter and a half of the window, bent and at three levels, so no two moments of the
/// shape agree by accident. Shares -2, -2, -1 (log2 seconds): 0.25 + 0.25 + 0.5 = the one-second window exactly.
mseg::Settings shapeAt(Mode mode, double degrees) {
	mseg::Settings s;
	s.startLevel = 0.1f;
	s.count = 3;
	s.segment[0] = { -2.0f, 1.0f, 0.6f };
	s.segment[1] = { -2.0f, 0.3f, -0.5f };
	s.segment[2] = { -1.0f, 0.8f, 0.4f };
	s.timeBase = 0;
	s.timeScale = 0.0f;
	s.sustainSegment = mseg::kNoSustain;
	s.loopStart = 0;
	s.loopEnd = 2;
	s.mode = mode;
	s.smoothing = kSmoothingLog2SecondsMin;
	s.stereoPhase = static_cast<float>(degrees * M_PI / 180.0);
	mseg::sanitize(&s);
	return s;
}

struct Ears {
	std::vector<float> left;
	std::vector<float> right;
};

/// The gate up for `held` ticks, then down, for `ticks` in all.
Ears earsOver(const mseg::Settings& s, int ticks, int held) {
	mseg::Shape shape;
	shape.prepare(kSampleRate);
	Ears e;
	for (int k = 0; k < ticks; ++k) {
		shape.tick(s, k < held, false, TransportClock::unknown());
		e.left.push_back(shape.value());
		e.right.push_back(shape.valueRight());
	}
	return e;
}

struct Variant {
	const char* name;
	Mode mode;
	int sustain;
	size_t loopStart;
	bool pingPong;
};
} // namespace pa
} // namespace

TEST_CASE("backlog PA (MSEG): Stereo Phase — the right ear NOW is what the left ear reads that share of the window LATER, in every mode, held and after the key") {
	REQUIRE(timeSeconds(0.0f) == 1.0);
	REQUIRE(mseg::windowSeconds(device::kDivisionBeats[0], 1.0, TransportClock::unknown()) == 1.0);
	const pa::Variant variants[] = {
		{ "Gate, no sustain", Mode::Gate, mseg::kNoSustain, 0, false },
		{ "Gate, sustain at 1", Mode::Gate, 1, 0, false },
		{ "Loop, whole shape", Mode::Loop, mseg::kNoSustain, 0, false },
		{ "Loop, a sub-loop after an attack", Mode::Loop, mseg::kNoSustain, 1, false },
		{ "Loop, ping-pong", Mode::Loop, mseg::kNoSustain, 0, true },
		{ "Once", Mode::Once, mseg::kNoSustain, 0, false },
		{ "Free, no transport", Mode::Free, mseg::kNoSustain, 1, false },
		{ "LFO Retrig", Mode::LfoRetrig, mseg::kNoSustain, 0, false },
	};
	for (const pa::Variant& v : variants) {
		for (const double degrees : { 90.0, 180.0 }) {
			CAPTURE(std::string(v.name));
			CAPTURE(degrees);
			mseg::Settings s = pa::shapeAt(v.mode, degrees);
			s.sustainSegment = v.sustain;
			s.loopStart = v.loopStart;
			s.pingPong = v.pingPong;
			mseg::sanitize(&s);
			const int lead = static_cast<int>(pa::kTicksPerWindow * degrees / 360.0);
			// THE KEY GOES UP FOR FIVE WINDOWS, THEN DOWN FOR TWO: the right ear is the left's future wherever the gate
			// holds still across the lead — while held, and after the key — and only the lead's width around the
			// release itself is a future the right ear could not know.
			constexpr int kHeld = 5 * pa::kTicksPerWindow;
			const int ticks = kHeld + 2 * pa::kTicksPerWindow;
			const pa::Ears e = pa::earsOver(s, ticks, kHeld);
			size_t apart = 0;
			size_t compared = 0;
			for (int k = 0; k + lead < ticks; ++k) {
				if (k < kHeld && k + lead >= kHeld) {
					continue; // the gate falls inside the lead
				}
				CAPTURE(k);
				const auto at = static_cast<size_t>(k);
				CHECK(std::abs(e.right[at] - e.left[at + static_cast<size_t>(lead)]) < 1e-6f);
				apart += std::abs(e.right[at] - e.left[at]) > 1e-3f ? 1u : 0u;
				++compared;
			}
			REQUIRE(compared > 500);
			// NOT VACUOUS: the ears really are apart, for at least the lead's width (a held sustain and a finished one-shot
			// are where they agree, by design).
			CHECK(apart >= static_cast<size_t>(lead) / 2);
		}
	}
}

TEST_CASE("backlog PA (MSEG): Stereo Phase restated — on straight segments looping the whole window, the right ear is the shape at the left ear's place + offset, wrapped") {
	for (const double degrees : { 45.0, 90.0, 135.0, 180.0 }) {
		CAPTURE(degrees);
		const double lead = degrees / 360.0;
		mseg::Settings s = pa::shapeAt(Mode::Loop, degrees);
		for (size_t i = 0; i < 3; ++i) {
			s.segment[i].shape = 0.0f;
		}
		// AN INEXACT RATE AND A SAMPLE RATE OFF THE GRID: the restatement does not lean on the exact ticks above.
		s.timeScale = -0.37f;
		mseg::sanitize(&s);
		// THE SHAPE AS A FUNCTION OF ITS PLACE IN THE WINDOW, written out: segment edges at 1/4 and 1/2, straight lines.
		const auto shapeAtX = [&](double x) {
			const double edge[] = { 0.0, 0.25, 0.5, 1.0 };
			const double from[] = { s.startLevel, s.segment[0].level, s.segment[1].level };
			for (size_t i = 0; i < 3; ++i) {
				if (x < edge[i + 1] || i == 2) {
					const double t = (x - edge[i]) / (edge[i + 1] - edge[i]);
					return from[i] + (s.segment[i].level - from[i]) * t;
				}
			}
			return 0.0;
		};
		const double edge[] = { 0.0, 0.25, 0.5, 1.0 };
		mseg::Shape shape;
		shape.prepare(44100.0);
		size_t apart = 0;
		for (int k = 0; k < 900; ++k) {
			shape.tick(s, true, false, TransportClock::unknown());
			const Playhead where = shape.playhead();
			REQUIRE(where.running());
			const double x = edge[where.ordinal] + (edge[where.ordinal + 1] - edge[where.ordinal]) * where.phase;
			CHECK(std::abs(shape.value() - shapeAtX(x)) < 1e-5);
			const double ahead = x + lead - std::floor(x + lead);
			CHECK(std::abs(shape.valueRight() - shapeAtX(ahead)) < 1e-4);
			apart += std::abs(shape.valueRight() - shape.value()) > 1e-3f ? 1u : 0u;
		}
		CHECK(apart > 300);
	}
}

TEST_CASE("backlog PA (MSEG): Stereo Phase on a song-locked Free shape is transport-deterministic — the right ear at song position x is the left ear at x + offset × window") {
	for (const double degrees : { 90.0, 180.0 }) {
		CAPTURE(degrees);
		const double lead = degrees / 360.0;
		// A SUB-LOOP, which a locked Free shape runs through as the window (the song places it), so the lead is the window's.
		mseg::Settings s = pa::shapeAt(Mode::Free, degrees);
		s.loopStart = 1;
		mseg::sanitize(&s);
		SUBCASE("no tempo: the window in seconds") {
			const double window = timeSeconds(s.timeScale);
			for (const double t : { 0.13, 3.71, 22.05 }) {
				CAPTURE(t);
				TransportClock clock {};
				clock.playing = true;
				clock.songPositionSeconds = t;
				mseg::Shape here;
				here.prepare(48000.0);
				here.tick(s, false, false, clock);
				clock.songPositionSeconds = t + lead * window;
				mseg::Shape later;
				later.prepare(48000.0);
				later.tick(s, false, false, clock);
				CHECK(std::abs(here.valueRight() - later.value()) < 1e-5);
				CHECK(std::abs(here.valueRight() - here.value()) > 1e-3); // NOT VACUOUS
			}
		}
		SUBCASE("synced: the window in beats") {
			s.timeBase = 13;
			mseg::sanitize(&s);
			const double windowBeats = device::kDivisionBeats[s.timeBase] * timeSeconds(s.timeScale);
			for (const double x : { 3.1, 17.37, 64.9 }) {
				CAPTURE(x);
				TransportClock clock = TransportClock::fromSecondsPerBeat(0.5);
				clock.playing = true;
				clock.songPositionBeats = x;
				mseg::Shape here;
				here.prepare(48000.0);
				here.tick(s, false, false, clock);
				clock.songPositionBeats = x + lead * windowBeats;
				mseg::Shape later;
				later.prepare(48000.0);
				later.tick(s, false, false, clock);
				CHECK(std::abs(here.valueRight() - later.value()) < 1e-5);
				CHECK(std::abs(here.valueRight() - here.value()) > 1e-3);
			}
		}
	}
}

TEST_CASE("backlog PA (MSEG): at Stereo Phase 0 the right ear IS the left, bit for bit, in every mode — and turning it back to 0 rejoins them at once") {
	for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::Free, Mode::LfoRetrig }) {
		CAPTURE(static_cast<int>(mode));
		mseg::Settings s = pa::shapeAt(mode, 0.0);
		s.smoothing = -4.0f; // smoothing on: the right ear's own pole must agree with the left's too
		s.sustainSegment = 1;
		const pa::Ears e = pa::earsOver(s, 600, 400);
		for (size_t k = 0; k < e.left.size(); ++k) {
			CAPTURE(k);
			REQUIRE(std::bit_cast<uint32_t>(e.right[k]) == std::bit_cast<uint32_t>(e.left[k]));
		}
	}
	mseg::Settings s = pa::shapeAt(Mode::Loop, 90.0);
	s.smoothing = -4.0f;
	mseg::Shape shape;
	shape.prepare(pa::kSampleRate);
	for (int k = 0; k < 37; ++k) {
		shape.tick(s, true, false, TransportClock::unknown());
	}
	REQUIRE(shape.valueRight() != shape.value());
	s.stereoPhase = 0.0f;
	shape.tick(s, true, false, TransportClock::unknown());
	CHECK(shape.valueRight() == shape.value());
}

TEST_CASE("backlog PA (MSEG): a finished Once holds both ears where the shape ended, and the device is idle only when both have arrived") {
	mseg::Settings s = pa::shapeAt(Mode::Once, 90.0);
	s.smoothing = -5.0f;
	mseg::Shape shape;
	shape.prepare(pa::kSampleRate);
	bool apart = false;
	for (int k = 0; k < 4 * pa::kTicksPerWindow; ++k) {
		shape.tick(s, k < 10, false, TransportClock::unknown());
		apart = apart || std::abs(shape.valueRight() - shape.value()) > 1e-3f;
	}
	CHECK(apart);
	CHECK(shape.position().finished);
	CHECK(shape.value() == s.segment[2].level);
	CHECK(shape.valueRight() == s.segment[2].level);
	CHECK(shape.idle());
}

TEST_CASE("backlog PA (MSEG): the card's right-ear reading is the engine's — Shape::rawLeadingAt at the left ear's place is the running right ear") {
	// THE PICTURE'S ROAD (`MsegCard::curveRight`) asked against the sound's: a looping shape, the right ear read off the
	// running engine and off the static helper at the left ear's place in the window. Smoothing off, so raw is the value.
	const pa::Variant variants[] = {
		{ "Loop, whole shape", Mode::Loop, mseg::kNoSustain, 0, false },
		{ "Loop, ping-pong", Mode::Loop, mseg::kNoSustain, 0, true },
		{ "Loop, a sub-loop", Mode::Loop, mseg::kNoSustain, 1, false },
		{ "Gate, sustain at 1", Mode::Gate, 1, 0, false },
		{ "Once", Mode::Once, mseg::kNoSustain, 0, false },
	};
	for (const pa::Variant& v : variants) {
		CAPTURE(std::string(v.name));
		mseg::Settings s = pa::shapeAt(v.mode, 135.0);
		s.sustainSegment = v.sustain;
		s.loopStart = v.loopStart;
		s.pingPong = v.pingPong;
		mseg::sanitize(&s);
		mseg::Shape shape;
		shape.prepare(pa::kSampleRate);
		size_t apart = 0;
		// THE FIRST PASS, up to the end of the window: ping-pong's return leg is a direction the static picture does not
		// carry, and the card draws one pass.
		for (int k = 0; k + 1 < pa::kTicksPerWindow; ++k) {
			shape.tick(s, true, false, TransportClock::unknown());
			const Playhead where = shape.playhead();
			if (!where.running() || shape.position().sustaining) {
				continue; // held at the sustain point: the left ear is not running through the picture
			}
			CAPTURE(k);
			CHECK(std::abs(shape.valueRight()
			               - mseg::Shape::rawLeadingAt(s, where.ordinal, where.phase, s.stereoLead(), true))
			      < 1e-5f);
			apart += std::abs(shape.valueRight() - shape.value()) > 1e-3f ? 1u : 0u;
		}
		CHECK(apart > 10);
	}
}

TEST_CASE("backlog PA (MSEG): the right ear's polyline is drawn at the left one's x, from the engine's walk, and only while Stereo Phase is not zero") {
	mseg::Settings s = pa::shapeAt(Mode::Loop, 0.0);
	CHECK(mseg::polylineRight(s).empty());
	s.stereoPhase = static_cast<float>(M_PI / 2.0);
	const auto left = mseg::polyline(s);
	const auto right = mseg::polylineRight(s);
	// THE SAME XS AS THE LEFT EAR'S LINE, so the two traces are read against one axis.
	REQUIRE(right.size() == left.size());
	size_t apart = 0;
	for (size_t i = 0; i < right.size(); ++i) {
		CAPTURE(i);
		CHECK(right[i].first == left[i].first);
		apart += std::abs(right[i].second - left[i].second) > 1e-3f ? 1u : 0u;
	}
	CHECK(apart > right.size() / 2);
	// A QUARTER OF THE WINDOW ON FROM THE START IS THE END OF SEGMENT 0 (its share is a quarter): the right ear's first
	// point is the first breakpoint's level, the left ear's is the start level.
	CHECK(left.front().second == s.startLevel);
	CHECK(std::abs(right.front().second - s.segment[0].level) < 1e-5f);
	// AND BIPOLAR IS THE PLOT'S DOMAIN, as the left line's.
	s.bipolar = true;
	const auto bipolar = mseg::polylineRight(s);
	REQUIRE(bipolar.size() == right.size());
	CHECK(std::abs(bipolar.front().second - (2.0f * s.segment[0].level - 1.0f)) < 1e-5f);
}

TEST_CASE("backlog PA (MSEG): Stereo Phase is stored by its own key, in radians read as degrees, makes the output a pair while non-zero, and round-trips the row") {
	const device::ParamDesc& p = mseg::kParamDescs[static_cast<size_t>(mseg::Param::StereoPhase)];
	CHECK(std::string(p.key) == "stereo_phase");
	CHECK(p.unit == device::Unit::Degrees);
	CHECK(p.min == 0.0);
	CHECK(p.max == doctest::Approx(M_PI));
	CHECK(p.def == 0.0);
	CHECK_FALSE(p.modulatable);
	CHECK(mseg::kOutputs[0].pairWhile == static_cast<int>(mseg::Param::StereoPhase));
	mseg::Settings s;
	CHECK(s.stereoPhase == 0.0f);
	s.stereoPhase = 1.25f;
	const mseg::Row row = mseg::rowOf(s);
	CHECK(row[static_cast<size_t>(mseg::Param::StereoPhase)] == 1.25f);
	CHECK(mseg::settingsOf(row, row.size()).stereoPhase == 1.25f);
	// A ROW TOO SHORT TO REACH IT reads the default: mono.
	CHECK(mseg::settingsOf(row, static_cast<size_t>(mseg::Param::StereoPhase)).stereoPhase == 0.0f);
	// CLAMPED TO ITS RANGE by the declaration.
	s.stereoPhase = 7.0f;
	mseg::sanitize(&s);
	CHECK(s.stereoPhase == static_cast<float>(M_PI));
}

TEST_CASE("mseg: settings are sanitised once at set time, so a hostile row still ticks finite and in range") {
	// `Shape::tick` no longer copies and re-sanitises per tick; the road in (`settingsOf`, through `setFieldsOf`) does it.
	mseg::Row row = mseg::rowOf(mseg::Settings {});
	for (float& v : row) {
		v = std::numeric_limits<float>::quiet_NaN();
	}
	const mseg::Settings s = mseg::settingsOf(row, row.size());
	CHECK(mseg::isSanitisedForDebug(s));
	mseg::Shape shape;
	shape.prepare(48000.0);
	for (int i = 0; i < 2000; ++i) {
		const float v = shape.tick(s, true, false, feathers::mod::TransportClock::unknown());
		REQUIRE(std::isfinite(v));
	}
	// AND THE PREDICATE IS NOT VACUOUS: a hand-built out-of-range value is reported.
	mseg::Settings raw = s;
	raw.startLevel = 1.0e9f;
	CHECK_FALSE(mseg::isSanitisedForDebug(raw));
}
