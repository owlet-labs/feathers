/*
 * Copyright © 2026 Owlet Records
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
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"

#include "device/registry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <limits>

using namespace feathers;
using namespace feathers::mod::lfo;

TEST_CASE("lfo: skew remap is identity at neutral") {
	SkewMap skew(0.5f, 0.5f, 0.5f); // fractions now, not 0..100 — see the descriptors
	// At neutral, each quadrant has equal weight (0.25).
	CHECK(skew.remap(0.0f) == doctest::Approx(0.0f));
	CHECK(skew.remap(0.25f) == doctest::Approx(0.25f));
	CHECK(skew.remap(0.5f) == doctest::Approx(0.5f));
	CHECK(skew.remap(0.75f) == doctest::Approx(0.75f));
	CHECK(skew.remap(1.0f) == doctest::Approx(1.0f));
}

TEST_CASE("lfo: the three skews NEST — the cross sets the halves and the other two divide them") {
	// **THE DESIGNED PARTITION**, asserted as weights rather than described. Each control is
	// checked for the thing its own NAME claims, which the old implementation did not do for the
	// middle one: "how much comes before the cross" is a statement about the whole cycle, and it was
	// built as one quadrant's share of four.
	SUBCASE("the cross alone moves the half-way point, all the way to either end") {
		CHECK(SkewMap(0.5f, 0.75f, 0.5f).weight[0] + SkewMap(0.5f, 0.75f, 0.5f).weight[1]
		      == doctest::Approx(0.75f));
		CHECK(SkewMap(0.5f, 0.10f, 0.5f).weight[0] + SkewMap(0.5f, 0.10f, 0.5f).weight[1]
		      == doctest::Approx(0.10f));
		// THE ENDS, WHICH ARE THE WHOLE POINT: the old build stopped at 0.75 whatever it was given.
		CHECK(SkewMap(0.5f, 1.0f, 0.5f).weight[0] + SkewMap(0.5f, 1.0f, 0.5f).weight[1]
		      == doctest::Approx(1.0f));
		CHECK(SkewMap(0.5f, 0.0f, 0.5f).weight[0] + SkewMap(0.5f, 0.0f, 0.5f).weight[1]
		      == doctest::Approx(0.0f));
	}

	SUBCASE("the side skews divide the half they belong to, at every setting of the cross") {
		// **THE "RE SCALING" HALF OF THE REQUEST.** A side skew is a proportion OF ITS HALF, so it
		// keeps its meaning as the cross moves instead of competing with it for one budget.
		for (const float cross : { 0.2f, 0.5f, 0.9f }) {
			CAPTURE(cross);
			const SkewMap skew(0.25f, cross, 0.75f);
			const float first = skew.weight[0] + skew.weight[1];
			const float second = skew.weight[2] + skew.weight[3];
			CHECK(first == doctest::Approx(cross));
			CHECK(second == doctest::Approx(1.0f - cross));
			CHECK(skew.weight[0] == doctest::Approx(first * 0.25f));   // a quarter of the first half
			CHECK(skew.weight[2] == doctest::Approx(second * 0.75f));  // three quarters of the second
		}
	}

	SUBCASE("and the weights always sum to one, by construction rather than by a normalising pass") {
		for (const float a : { 0.0f, 0.3f, 1.0f }) {
			for (const float b : { 0.0f, 0.6f, 1.0f }) {
				for (const float c : { 0.0f, 0.4f, 1.0f }) {
					CAPTURE(a); CAPTURE(b); CAPTURE(c);
					const SkewMap skew(a, b, c);
					CHECK(skew.weight[0] + skew.weight[1] + skew.weight[2] + skew.weight[3]
					      == doctest::Approx(1.0f));
				}
			}
		}
	}
}

TEST_CASE("lfo: the cross alone turns a triangle into a sawtooth, which the old partition could not do") {
	// **THE FEATURE'S OWN TEST** (rescaling the side skews so that a triangle properly becomes a
	// sawtooth), asserted on the OUTPUT rather than on the weights — a partition that summed
	// correctly and still drew the wrong wave would pass the case above.
	Settings s;
	s.wave = Waveform::Triangle;

	SUBCASE("at the top of the cross the rise fills the cycle: a rising saw") {
		s.skewPreCross = 1.0f;
		// A TRIANGLE'S RISE IS LINEAR, so a rising sawtooth is `y == x` across the whole cycle. That is
		// the strongest statement available here and it is an independent one: it comes from what a
		// sawtooth IS, not from re-running the remap.
		for (int i = 0; i <= 20; ++i) {
			const float x = static_cast<float>(i) / 20.0f;
			CAPTURE(x);
			// The last sample is the reset itself — the cycle's end, which is also its start.
			const float want = (i == 20) ? 0.0f : x;
			CHECK(eval(s, x) == doctest::Approx(want).epsilon(0.02));
		}
	}

	SUBCASE("at the bottom it is the mirror: a falling saw") {
		s.skewPreCross = 0.0f;
		for (int i = 1; i <= 19; ++i) {
			const float x = static_cast<float>(i) / 20.0f;
			CAPTURE(x);
			CHECK(eval(s, x) == doctest::Approx(1.0f - x).epsilon(0.02));
		}
	}

	SUBCASE("and neutral is still a triangle, or the two above prove nothing about the control") {
		s.skewPreCross = 0.5f;
		CHECK(eval(s, 0.25f) == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(eval(s, 0.5f) == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(eval(s, 0.75f) == doctest::Approx(0.5f).epsilon(0.02));
	}
}

TEST_CASE("lfo: waveforms evaluate correctly") {
	// Sine
	CHECK(evalWaveform(Waveform::Sine, 0.0f) == doctest::Approx(0.5f));
	CHECK(evalWaveform(Waveform::Sine, 0.25f) == doctest::Approx(1.0f));
	CHECK(evalWaveform(Waveform::Sine, 0.5f) == doctest::Approx(0.5f));
	CHECK(evalWaveform(Waveform::Sine, 0.75f) == doctest::Approx(0.0f));

	// Triangle: peak at 0.5, zero at 0 and 1.
	CHECK(evalWaveform(Waveform::Triangle, 0.0f) == doctest::Approx(0.0f));
	CHECK(evalWaveform(Waveform::Triangle, 0.25f) == doctest::Approx(0.5f));
	CHECK(evalWaveform(Waveform::Triangle, 0.5f) == doctest::Approx(1.0f));
	CHECK(evalWaveform(Waveform::Triangle, 0.75f) == doctest::Approx(0.5f));

	// Square
	CHECK(evalWaveform(Waveform::Square, 0.0f) == doctest::Approx(1.0f));
	CHECK(evalWaveform(Waveform::Square, 0.49f) == doctest::Approx(1.0f));
	CHECK(evalWaveform(Waveform::Square, 0.5f) == doctest::Approx(0.0f));
	CHECK(evalWaveform(Waveform::Square, 0.99f) == doctest::Approx(0.0f));
}

TEST_CASE("lfo: bipolar remaps 0..1 to -1..1") {
	Settings s;
	s.wave = Waveform::Triangle;
	s.bipolar = true;
	// Triangle at phase 0 = 0, bipolar → -1
	CHECK(eval(s, 0.0f) == doctest::Approx(-1.0f));
	// Triangle at phase 0.5 = 1, bipolar → 1
	CHECK(eval(s, 0.5f) == doctest::Approx(1.0f));
}

TEST_CASE("lfo: random walk is deterministic") {
	// Same seed and step always produces the same value.
	const float v1 = randomWalkValue(42, 7);
	const float v2 = randomWalkValue(42, 7);
	CHECK(v1 == v2);

	// Different seeds produce different values (with high probability).
	const float v3 = randomWalkValue(43, 7);
	CHECK(v1 != v3);

	// Values are in [0, 1].
	for (int seed = 0; seed < 100; ++seed) {
		for (int step = 0; step < 100; ++step) {
			const float v = randomWalkValue(seed, step);
			CHECK(v >= 0.0f);
			CHECK(v < 1.0f);
		}
	}
}


TEST_CASE("lfo: once mode runs one cycle then stops") {
	Settings s;
	s.mode = Mode::Once;
	s.wave = Waveform::Triangle;
	s.phase = 0.0f;
	s.bipolar = false;

	Shape shape;
	shape.prepare(48000.0);

	// Tick for a while at 1 Hz. One cycle = 1 second = 48000 samples / 64 samples per tick = 750 ticks.
	float lastValue = 0.0f;
	for (int i = 0; i < 800; ++i) {
		lastValue = shape.tick(s, true, false, device::TransportClock::unknown());
	}

	// After one cycle, the shape should be finished and holding at the end value.
	// Triangle at phase 1.0 = 0.
	CHECK(lastValue == doctest::Approx(0.0f).epsilon(0.01));

	// **AND IT GOT THERE BY RUNNING, WHICH THE LINE ABOVE CANNOT SAY.** A triangle reads 0 at phase 1
	// and ALSO at phase 0, so an LFO that never advanced at all — a dead `advance`, a rate of zero, a
	// `finished` flag stuck on from the start — passes that assertion exactly as a working one does.
	// It is the shape CLAUDE.md names: a value that several different worlds produce.
	//
	// Three statements that only the working world satisfies: the cycle ENDED (the flag, read through
	// the playhead, which is the only seam that exposes it), it PASSED THROUGH THE PEAK of the triangle on
	// the way, and it stays put once finished.
	CHECK(shape.playhead().phase == doctest::Approx(0.0f));  // a finished shape reports no position

	Shape again;
	again.prepare(48000.0);
	float peak = 0.0f;
	for (int i = 0; i < 800; ++i) {
		peak = std::max(peak, again.tick(s, true, false, device::TransportClock::unknown()));
	}
	INFO("the highest value the cycle reached: " << peak);
	CHECK(peak > 0.9f);

	// AND ONCE MEANS ONCE: a thousand further ticks do not start a second cycle.
	for (int i = 0; i < 1000; ++i) {
		CHECK(again.tick(s, true, false, device::TransportClock::unknown())
		      == doctest::Approx(0.0f).epsilon(0.01));
	}
}

TEST_CASE("lfo: settings round-trip through rowOf/settingsOf") {
	Settings s;
	s.wave = Waveform::Square;
	s.skewPrePeak = 0.25f;
	s.skewPreCross = 0.75f;
	s.skewPreTrough = 0.5f;
	s.seed = 42;

	s.timeBase = 5;
	s.timeScale = 1.5f;
	s.smoothing = -5.643856189774724f; // 20 ms — a smoothing is now a TIME and nothing else
	s.mode = Mode::Loop;
	s.phase = 0.3f;
	s.pingPong = true;
	s.bipolar = true;

	Row row = rowOf(s);
	Settings s2 = settingsOf(row, row.size());

	CHECK(s2.wave == Waveform::Square);
	CHECK(s2.skewPrePeak == doctest::Approx(0.25f));
	CHECK(s2.skewPreCross == doctest::Approx(0.75f));
	CHECK(s2.skewPreTrough == doctest::Approx(0.5f));
	CHECK(s2.seed == 42);

	CHECK(s2.timeBase == 5);
	CHECK(s2.phase == doctest::Approx(0.3f));
	CHECK(s2.pingPong == true);
	CHECK(s2.bipolar == true);

	// **THE THREE THIS CASE SET AND DID NOT READ BACK.** A round-trip case is a claim about the WHOLE
	// row, and one that writes thirteen fields and checks ten is silent about exactly the three that a
	// column-order mistake would land on. `timeScale` and `mode` are the rate and the retrigger
	// behaviour — the two settings a patch most obviously has to remember — and `smoothing` is the one
	// filter in here. (It was `smoothingOn`, the switch in front of that filter, until the switch was
	// deleted and the time took over saying whether it runs.)
	CHECK(s2.timeScale == doctest::Approx(1.5f));
	CHECK(s2.smoothing == doctest::Approx(-5.643856189774724f));
	CHECK(s2.mode == Mode::Loop);
}

//------------------------------------------------------------------------------------------------//
// THE FREE-RUNNING RANDOM WALK, which is where the Rate knob went                                  //
//------------------------------------------------------------------------------------------------//

namespace {

/// How many times a free-running Random LFO changes its output over `ticks`, at a given rate.
/// The count IS the rate, which is the whole point: a random walk that steps once per second and one
/// that steps once per tick are the same code path and differ only here.
int randomStepsOver(float log2Hz, int ticks) {
	Settings s;
	s.wave = Waveform::Random;
	s.mode = Mode::Free;
	s.timeBase = 0;  // un-synced: free-running, rate read from the scale
	s.timeScale = log2Hz;
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin;  // OFF: smoothing would blur the steps into a ramp and hide the count
	s.seed = 7;

	Shape shape;
	shape.prepare(48000.0);
	int changes = 0;
	float previous = shape.tick(s, true, false, device::TransportClock::unknown());
	for (int i = 1; i < ticks; ++i) {
		const float now = shape.tick(s, true, false, device::TransportClock::unknown());
		if (now != previous) {
			++changes;
		}
		previous = now;
	}
	return changes;
}

}  // namespace

TEST_CASE("lfo: a free-running random walk steps at its rate, not once per tick") {
	// **THE RATE KNOB DID NOTHING HERE.** `advanceRandom` took `hz` and `rateMul` and named them both
	// out — `float /*hz*/, float /*rateMul*/` — then, in the un-synced branch, did `step = randomStep_
	// + 1` on EVERY call. So a free-running Random LFO drew a fresh value every 64-sample tick, which
	// at 48 kHz is 750 Hz: not a modulator, white noise, and the same noise at every Rate setting.
	//
	// The synced branch was right all along (it derives the step from the song position), so this is
	// the one path, and it is the path that runs with no host transport.
	//
	// 750 ticks is one second at 48 kHz with the 64-sample control tick, so the counts below read
	// directly as "steps per second" — which is what the knob claims to set.
	// FOUR SECONDS, AND NOT ONE, because a whole number of seconds puts every wrap of a 4 Hz walk on
	// a tick boundary at once and the last one lands on the closing tick — the first draft of this
	// case measured one second, expected 4 and read 3, which says more about the window than about
	// the walk. A longer window costs nothing and makes the count the rate rather than the phase of
	// the sampling against it.
	constexpr int kFourSeconds = 3000;

	const int atOneHz = randomStepsOver(0.0f, kFourSeconds);    // exp2(0) = 1 Hz  -> about 4
	const int atFourHz = randomStepsOver(2.0f, kFourSeconds);   // exp2(2) = 4 Hz  -> about 16
	INFO("steps in four seconds: 1 Hz -> " << atOneHz << ", 4 Hz -> " << atFourHz);

	// THE COUNT IS THE RATE, within the one step a tick boundary can swallow at either end.
	CHECK(atOneHz >= 3);
	CHECK(atOneHz <= 5);
	CHECK(atFourHz >= 15);
	CHECK(atFourHz <= 17);

	// NOT VACUOUS, AND THIS IS THE ASSERTION THAT WOULD HAVE CAUGHT IT: the two rates must produce
	// DIFFERENT counts. Under the old code both read 749, so every bound above could have been met by
	// a build where Rate was ignored, if the bounds had been loose enough.
	CHECK(atFourHz > atOneHz);
}

TEST_CASE("lfo: a free-running random walk holds its value between steps") {
	// The other half of the same claim, and the one a count cannot make: BETWEEN steps the output is
	// held, not re-drawn. A walk that changed on most ticks but not all would satisfy a loose count.
	Settings s;
	s.wave = Waveform::Random;
	s.mode = Mode::Free;
	s.timeBase = 0;
	s.timeScale = 0.0f;  // 1 Hz: one step per 750 ticks, so the hold is 749 of them
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	s.seed = 3;

	Shape shape;
	Shape other;
	shape.prepare(48000.0);
	other.prepare(48000.0);

	const float first = shape.tick(s, true, false, device::TransportClock::unknown());
	int held = 0;
	for (int i = 1; i < 300; ++i) {
		if (shape.tick(s, true, false, device::TransportClock::unknown()) == first) {
			++held;
		}
	}
	INFO("held the first value for " << held << " of the next 299 ticks");
	CHECK(held == 299);

	// AND IT IS THE SEED'S VALUE, not a constant: a different seed holds a different number, so
	// "held" is not passing because the walk outputs the same thing for everybody.
	Settings t = s;
	t.seed = 4;
	const float otherFirst = other.tick(t, true, false, device::TransportClock::unknown());
	CHECK(otherFirst != first);
}

TEST_CASE("lfo: bipolar reaches the random walk, as it reaches every other wave") {
	// **A DEAD CONTROL ON ONE WAVEFORM.** Every other wave leaves the engine through `eval`, which ends
	// with the bipolar rescale; Random took a different exit — `raw_ = randomValue_` — and skipped it.
	// So turning Bipolar on did nothing at all on Random: the output stayed 0..1 while the card, the
	// dropdown and every other wave said -1..1. The honesty rule, broken at the engine rather than at
	// the card, which is the harder place to see it.
	const auto extremesOver = [](bool bipolar) {
		Settings s;
		s.wave = Waveform::Random;
		s.mode = Mode::Free;
		s.timeBase = 0;
		s.timeScale = 6.0f;  // 64 Hz: many steps in a short run, so the walk visits its range
		s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
		s.bipolar = bipolar;
		s.seed = 11;

		Shape shape;
		shape.prepare(48000.0);
		float low = 1e9f;
		float high = -1e9f;
		for (int i = 0; i < 4000; ++i) {
			const float v = shape.tick(s, true, false, device::TransportClock::unknown());
			low = std::min(low, v);
			high = std::max(high, v);
		}
		return std::pair<float, float> { low, high };
	};

	const auto unipolar = extremesOver(false);
	const auto bipolar = extremesOver(true);
	INFO("unipolar " << unipolar.first << ".." << unipolar.second << ", bipolar " << bipolar.first
	                 << ".." << bipolar.second);

	// Unipolar is the 0..1 the walk draws in, and it reaches most of it over this many steps.
	CHECK(unipolar.first >= 0.0f);
	CHECK(unipolar.second <= 1.0f);
	CHECK(unipolar.second - unipolar.first > 0.5f);

	// AND BIPOLAR IS THE SAME WALK, DOUBLED AND CENTRED: it must go NEGATIVE, which is the whole of
	// what the switch claims and exactly what it did not do.
	CHECK(bipolar.first < 0.0f);
	CHECK(bipolar.second > 0.0f);
	CHECK(bipolar.first == doctest::Approx(2.0f * unipolar.first - 1.0f).epsilon(0.001));
	CHECK(bipolar.second == doctest::Approx(2.0f * unipolar.second - 1.0f).epsilon(0.001));
}

TEST_CASE("lfo: the random scope draws the output the engine is producing, at the step it is on") {
	// **WHAT THIS CARD IS, BY DESIGN:** the random card is an oscilloscope of what the actual output
	// is. That is a claim a picture cannot make for itself —
	// a drawn curve is evidence that SOMETHING was drawn — so it is asserted here as values: the trace
	// and the engine are compared sample against sample, and they have to agree because they are the
	// same function.
	Settings s;
	s.wave = Waveform::Random;
	s.mode = Mode::Free;
	s.timeBase = 0;
	s.timeScale = 2.0f;  // 4 Hz
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	s.seed = 19;

	// Run the engine a while so it is somewhere a long way from its own beginning — the old curve
	// always drew steps 0..7 whatever the walk was doing, and starting at step 0 would hide that.
	Shape shape;
	shape.prepare(48000.0);
	float engineValue = 0.0f;
	for (int i = 0; i < 2000; ++i) {
		engineValue = shape.tick(s, true, false, device::TransportClock::unknown());
	}
	const mod::Playhead where = shape.playhead();
	REQUIRE(where.running());
	const int step = static_cast<int>(where.ordinal);
	INFO("the engine is on step " << step);
	// NOT VACUOUS: the walk really has moved off its own start, so "draws from step 0" is a DIFFERENT
	// picture from "draws from here" and the case can tell them apart.
	CHECK(step >= 8);

	// **THE FRACTION IS PART OF "WHERE THE ENGINE IS", so the case passes it.** Asking for the ordinal
	// alone was what made the picture scroll in lurches — and a case that also asked
	// for the ordinal alone could not have told the difference.
	const auto trace = randomTrace(s, step, where.phase);
	REQUIRE(trace.size() >= 2);

	// THE RIGHT-HAND EDGE IS NOW. The last sample is the value the engine is putting out this instant.
	CHECK(trace.back().second == doctest::Approx(engineValue));

	// AND EVERY DIVISION IS THE STEP IT CLAIMS. Walk the window and check each sample against the
	// walk's own value for the step that sample falls in — the engine's function, called independently
	// here, which is the restatement rather than a second reading of the same call.
	//
	// THE WINDOW IS `(now - kScopeSteps, now]` IN STEP UNITS and `now` is fractional, so the sample's
	// position is computed the same way rather than from a count of whole divisions.
	const double now = static_cast<double>(step) + static_cast<double>(where.phase);
	for (const auto& point : trace) {
		const double at = now - kScopeSteps + static_cast<double>(point.first) * kScopeSteps;
		CAPTURE(at);
		CHECK(point.second
		      == doctest::Approx(randomWalkValue(s.seed, static_cast<int>(std::floor(at)))));
	}

	// AND THE SCOPE FOLLOWS THE WALK. A later step draws a different window; a scope showing a fixed
	// eight steps from the beginning of time would return the same trace forever.
	const auto later = randomTrace(s, step + 3, where.phase);
	bool differs = false;
	for (size_t i = 0; i < trace.size() && i < later.size(); ++i) {
		differs = differs || (trace[i].second != later[i].second);
	}
	CHECK(differs);

	SUBCASE("and it follows the walk WITHIN a step too, which is what stops it scrolling in lurches") {
		// **THE REGRESSION GUARD FOR THE 2026-09-19 DEFECT:** the oscilloscope scrolled in chunks by
		// waypoint, and with smoothing on it fed in chunks. The window was built from the
		// ordinal alone, so it was quantised to step boundaries — eight WHOLE steps, the current one
		// drawn complete before it had been played, and a jump sideways when the ordinal ticked.
		//
		// **ASKED AS A DISPLACEMENT RATHER THAN AS "THEY DIFFER".** Two traces half a step apart differ
		// under the broken version too, the moment anything else moves; what only a sliding window can
		// do is move the SAME PICTURE sideways by exactly that fraction. So the claim is that the trace
		// at half a step in, read at x, equals the trace at the step's start read half a division to
		// the left — which is a statement about the amount of the shift, not merely its existence.
		// **ASKED OF THE GLIDE, because a displacement smaller than a sample cannot be asserted on a
		// staircase.** The shift is half a step; a stepped trace is flat everywhere except at the edges
		// and vertical AT them, so interpolating the earlier trace across an edge compares a number to
		// the jump it is in the middle of. The glide is continuous, so the claim is exact everywhere.
		// The window's behaviour is a property of the WINDOW and not of the wave inside it.
		Settings glide = s;
		glide.wave = Waveform::RandomGlide;
		const auto atStart = randomTrace(glide, step, 0.0f);
		const auto halfIn = randomTrace(glide, step, 0.5f);
		REQUIRE(atStart.size() == halfIn.size());

		const double shift = 0.5 / static_cast<double>(kScopeSteps); // half a step, in window widths
		int compared = 0;
		for (size_t i = 0; i < halfIn.size(); ++i) {
			// **PLUS, NOT MINUS — the picture scrolls LEFT as time advances.** A feature sitting at `x`
			// in the newer trace was at `x + shift` in the older one, because the window has moved on
			// and the feature has moved back through it. The first draft of this had the sign the other
			// way and reddened at 0.407 against 0.664, which is the sort of thing a difference check
			// would have passed in silence.
			const double want = static_cast<double>(halfIn[i].first) + shift;
			if (want > 1.0) {
				continue; // newer than the earlier window: no counterpart to compare against
			}
			// The earlier trace read at the shifted x, by the same interpolation a reader would do.
			const double at = want * static_cast<double>(atStart.size() - 1);
			const size_t lo = static_cast<size_t>(std::floor(at));
			const size_t hi = std::min(lo + 1, atStart.size() - 1);
			const double f = at - static_cast<double>(lo);
			const double expected =
			    atStart[lo].second + (atStart[hi].second - atStart[lo].second) * f;
			CAPTURE(i);
			CHECK(halfIn[i].second == doctest::Approx(expected).epsilon(0.02));
			++compared;
		}
		// NOT VACUOUS: most of the window really was compared, rather than every sample falling in the
		// `continue` above and the loop proving nothing.
		CHECK(compared > static_cast<int>(halfIn.size()) * 3 / 4);
	}
}

TEST_CASE("lfo: the random scope carries the bipolar switch, like the sound it is drawing") {
	// The card drew the walk's raw 0..1 while the engine — once Bipolar reached it — puts out -1..1.
	// A scope with a different vertical range from its signal is the same lie in the other direction.
	Settings s;
	s.wave = Waveform::Random;
	s.timeScale = 2.0f;
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	s.seed = 5;

	s.bipolar = false;
	const auto unipolar = randomTrace(s, 40);
	s.bipolar = true;
	const auto bipolar = randomTrace(s, 40);
	REQUIRE(unipolar.size() == bipolar.size());

	bool sawNegative = false;
	for (size_t i = 0; i < unipolar.size(); ++i) {
		CHECK(bipolar[i].second == doctest::Approx(2.0f * unipolar[i].second - 1.0f));
		sawNegative = sawNegative || (bipolar[i].second < 0.0f);
	}
	// NOT VACUOUS: the window actually contains a value below the centre, so the rescale had something
	// to prove rather than agreeing trivially on a trace that never goes low.
	CHECK(sawNegative);
}


TEST_CASE("Random Glide is the same walk, always moving and always arriving — which is what the smoothing knob could not do") {
	// **WHY THIS WAVE EXISTS, ASSERTED RATHER THAN DESCRIBED.** The random walk with
	// smoothing was smooth and steppy at the same time. The mechanism is that a one-pole
	// on a STAIRCASE settles early in each step and then sits — a corner and then a plateau — and that
	// the only cure available on that knob, a longer tau, stops the pole reaching its targets and so
	// trades the plateau for DEPTH. There is no setting of it that is both smooth and full.
	//
	// So the three claims here are the three halves of that sentence: the glide covers the same range
	// as the steps (it arrives), it never holds still (no plateau), and a heavily smoothed stepped
	// walk does neither — which is the known-bad standing inside the case rather than beside it.
	const double kRate = 48000.0;
	const float kLog2Hz = 3.0f; // 8 steps a second, so a second of ticks holds several
	const int kSeed = 7;

	// ONE SETTINGS, ONE SEED, ONE RATE — only the waveform and the smoothing differ between runs, or
	// a difference in span would be a difference in which walk was playing.
	const auto run = [&](Waveform wave, float smoothing) {
		Shape shape;
		shape.prepare(kRate);
		Settings s;
		s.wave = wave;
		s.mode = Mode::Free;
		s.timeBase = 0; // un-synced: free-running, the rate read from the scale
		s.timeScale = kLog2Hz;
		s.seed = kSeed;
		s.smoothing = smoothing;
		std::vector<float> out;
		out.reserve(750);
		for (int i = 0; i < 750; ++i) {
			out.push_back(shape.tick(s, true, false, feathers::mod::TransportClock::unknown(), 1.0f));
		}
		return out;
	};
	const auto span = [](const std::vector<float>& v) {
		const auto lo = *std::min_element(v.begin(), v.end());
		const auto hi = *std::max_element(v.begin(), v.end());
		return hi - lo;
	};
	// How much of the run is spent NOT MOVING — the "steppy" half, as a fraction of ticks.
	const auto held = [](const std::vector<float>& v) {
		int flat = 0;
		for (size_t i = 1; i < v.size(); ++i) {
			flat += (std::abs(v[i] - v[i - 1]) < 1e-4f) ? 1 : 0;
		}
		return static_cast<double>(flat) / static_cast<double>(v.size() - 1);
	};

	const float kOff = feathers::mod::kSmoothingLog2SecondsMin;
	const std::vector<float> stepped = run(Waveform::Random, kOff);
	const std::vector<float> glided = run(Waveform::RandomGlide, kOff);
	const std::vector<float> smoothed = run(Waveform::Random, -1.0f); // tau = 500 ms, the heavy end

	// 1. THE GLIDE ARRIVES: it covers the walk's own range, because a ramp between two known values
	//    ends ON the second one whatever the rate. Not "close to" the stepped span — the SAME span, to
	//    a tick's worth of sampling, since both are reading the same `hash01(seed, step)` sequence.
	CHECK(span(glided) == doctest::Approx(span(stepped)).epsilon(0.02));

	// 2. THE ONE-POLE DOES NOT, AND THIS IS THE KNOWN-BAD. Same walk, same rate, smoothing wound up
	//    far enough to remove the plateau — and a third of the movement is gone with it. Stated as a
	//    strict inequality against the glide rather than as a number, so it is a claim about the
	//    MECHANISM and does not have to be re-tuned when the seed or the rate changes.
	CHECK(span(smoothed) < 0.75f * span(glided));

	// 3. AND THE GLIDE NEVER HOLDS. The stepped walk is flat almost everywhere by construction — that
	//    is what a staircase is — and the glide is flat nowhere, which is the whole difference a
	//    listener hears. Both bounds are asserted: without the first, a glide that had silently
	//    become a staircase would still pass the second.
	CHECK(held(stepped) > 0.90);
	CHECK(held(glided) < 0.02);

	SUBCASE("and it is the SAME sequence, not merely a similar-looking one") {
		// THE VALUES, NOT THE SHAPE (CLAUDE.md: a claim about a value cannot be inferred from the
		// buffer it came out of). A glide passes through every step value the stepped wave holds, so
		// each of the stepped run's distinct plateau levels must appear in the glided run — and they
		// are `randomWalkValue(seed, step)` in both, which is the independent restatement.
		std::vector<float> levels;
		for (size_t i = 0; i < stepped.size(); ++i) {
			if (levels.empty() || std::abs(stepped[i] - levels.back()) > 1e-4f) {
				levels.push_back(stepped[i]);
			}
		}
		REQUIRE(levels.size() >= 4); // the run really did step several times, or this proves nothing

		for (const float level : levels) {
			CAPTURE(level);
			// SOMEWHERE in the glided run, because a ramp reaches every value between its ends and the
			// step values ARE its ends.
			bool found = false;
			for (const float v : glided) {
				found = found || std::abs(v - level) < 2e-2f;
			}
			CHECK(found);
			// AND IT IS THE WALK'S OWN VALUE rather than any number the two runs happen to share.
			bool isWalk = false;
			for (int step = 0; step < 32; ++step) {
				isWalk = isWalk || std::abs(randomWalkValue(kSeed, step) - level) < 1e-4f;
			}
			CHECK(isWalk);
		}
	}
}

TEST_CASE("lfo: the scope draws the GLIDE for Random Glide — the ramp between step values, not the staircase") {
	// THE SAME DESIGN AS THE CASE ABOVE (the random card is an oscilloscope of what the actual output
	// is), asked of the second walk. It is worth asking separately because the
	// trace's smoothing arithmetic is CHANGED by this wave rather than reused: a one-pole on a
	// constant is `target + (start - target)·exp(-t/tau)`, and on a ramp it is
	// `a + b·t - b·tau + (y0 - a + b·tau)·exp(-t/tau)` — two closed forms, one function, and a card
	// that took the first road for a ramping input would draw a shape the engine never produces.
	Settings s;
	s.wave = Waveform::RandomGlide;
	s.mode = Mode::Free;
	s.timeBase = 0;
	s.timeScale = 2.0f;                                     // 4 Hz
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin;   // off: the ramp alone, nothing on top
	s.seed = 19;

	const int step = 40; // a long way from the walk's own beginning, as the stepped case insists
	const auto trace = randomTrace(s, step);
	REQUIRE(trace.size() >= 2);

	// **EVERY SAMPLE AGAINST AN INDEPENDENT RESTATEMENT OF THE INTENDED PATH** — the lerp written out
	// here, from `randomWalkValue` called directly, rather than a second reading of the trace.
	// THE WINDOW IS `(now - kScopeSteps, now]` in step units — see `randomTrace`, which slides it by
	// the fraction rather than quantising to whole steps. Here `intoStep` is left at its default 0, so
	// `now` is the start of `step`.
	int moved = 0;
	for (size_t i = 0; i < trace.size(); ++i) {
		const double pos =
		    static_cast<double>(step) - kScopeSteps + static_cast<double>(trace[i].first) * kScopeSteps;
		const int at = static_cast<int>(std::floor(pos));
		const double into = pos - std::floor(pos);
		const float from = randomWalkValue(s.seed, at);
		const float to = randomWalkValue(s.seed, at + 1);
		CAPTURE(i);
		CHECK(trace[i].second == doctest::Approx(from + (to - from) * static_cast<float>(into)));
		if (i > 0 && std::abs(trace[i].second - trace[i - 1].second) > 1e-5f) {
			++moved;
		}
	}

	// AND THE CHECK ABOVE IS NOT VACUOUS IN THE WAY THAT MATTERS: a staircase would satisfy a lerp
	// whose two ends happened to be equal, so the trace is required to be MOVING nearly everywhere —
	// which is the one thing that tells this picture apart from the stepped one at a glance.
	CHECK(moved > static_cast<int>(trace.size()) - 12);

	SUBCASE("and the stepped wave in the same window is flat nearly everywhere, from the same seed") {
		// THE OTHER HALF OF THE COMPARISON, so "moving" above is a property of the WAVE rather than of
		// the window or the seed.
		Settings stepped = s;
		stepped.wave = Waveform::Random;
		const auto other = randomTrace(stepped, step);
		REQUIRE(other.size() == trace.size());
		int flat = 0;
		for (size_t i = 1; i < other.size(); ++i) {
			flat += (std::abs(other[i].second - other[i - 1].second) < 1e-5f) ? 1 : 0;
		}
		CHECK(flat > static_cast<int>(other.size()) - 12);
	}
}


TEST_CASE("Once is one shot per PHRASE on this device: a second note under a held gate does not re-fire it") {
	// **DECIDED 2026-09-19, and the reason it needed deciding.** This device was the only one of the
	// three that restarted Once on a second note — MEASURED at phase 0.267 → 0.003 — while the DAHDSR
	// and the MSEG refused and `mod::Mode` said in as many words that Once is *"a stab that cannot be
	// re-fired by the next note in a held chord"*.
	//
	// THE ARGUMENT FOR KEEPING IT WAS REAL AND IS WORTH RECORDING HERE. This modulator is MONO — one
	// shape, no voices, and the rack's gate is global — so `retrigger` is the only evidence a second
	// note exists at all, and refusing it means a chord fires the shot on the first note and not again
	// until every key is up. The DAHDSR gets one shot per NOTE only because it has a per-note table to
	// give each note a shape of its own. The decision is that a mono modulator gets one shot per PHRASE
	// rather than pretending: same sentence, applied to what the device actually has. A per-note
	// option for this device is open — backlog GA.
	Settings s;
	s.mode = Mode::Once;
	s.wave = Waveform::Triangle;
	s.timeBase = 0;    // free-running
	s.timeScale = 1.0f; // 2 Hz, so one cycle is 500 ms and 100 ticks is well inside it

	const auto phaseAfterSecondNote = [&](Mode mode) {
		Settings m = s;
		m.mode = mode;
		Shape shape;
		shape.prepare(48000.0);
		for (int i = 0; i < 100; ++i) {
			shape.tick(m, true, false, feathers::mod::TransportClock::unknown(), 1.0f);
		}
		const float before = shape.playhead().phase;
		// THE SECOND NOTE: the gate is still up, so this is the retrigger road and nothing else.
		shape.tick(m, true, true, feathers::mod::TransportClock::unknown(), 1.0f);
		return std::pair { before, shape.playhead().phase };
	};

	const auto [onceBefore, onceAfter] = phaseAfterSecondNote(Mode::Once);
	REQUIRE(onceBefore > 0.1f); // the shape really is mid-flight, or there is nothing to restart
	// IT CARRIED ON, rather than going back to the start: one tick further along and no more.
	CHECK(onceAfter > onceBefore);

	SUBCASE("while Gate, with the same note, does re-fire — which is what makes the refusal a finding") {
		// THE NON-VACUITY GUARD. Without it, a `retrigger` flag that had stopped reaching this device
		// at all would satisfy the check above, and the case would report a rule it never tested.
		const auto [gateBefore, gateAfter] = phaseAfterSecondNote(Mode::Gate);
		REQUIRE(gateBefore > 0.1f);
		CHECK(gateAfter < 0.01f);
	}

	SUBCASE("and a gate that falls and rises IS a new phrase, so the shot fires again") {
		// THE OTHER HALF OF "PER PHRASE" — refusing every restart forever would be a different mode.
		Shape shape;
		shape.prepare(48000.0);
		for (int i = 0; i < 100; ++i) {
			shape.tick(s, true, false, feathers::mod::TransportClock::unknown(), 1.0f);
		}
		REQUIRE(shape.playhead().phase > 0.1f);
		shape.tick(s, false, false, feathers::mod::TransportClock::unknown(), 1.0f); // all keys up
		shape.tick(s, true, false, feathers::mod::TransportClock::unknown(), 1.0f);  // a new phrase
		CHECK(shape.playhead().phase < 0.01f);
	}
}

TEST_CASE("smoothPeriodic is the steady state a long filter run converges to, in two passes instead of thousands") {
	// **THE CLAIM IS THAT A CLOSED FORM EQUALS A LIMIT**, so the reference is the limit itself: run the
	// ordinary one-pole recursion over the same wave for hundreds of periods and compare the last one.
	// That is an independent restatement in the strongest sense available — the brute force shares no
	// line of code with `smoothPeriodic`, and it is how anyone would check the algebra by hand.
	constexpr size_t kM = 127;                 // the card's own period resolution
	const double period = 0.25;                // 4 Hz
	for (const double tau : { 0.002, 0.02, 0.08, 0.5 }) {
		CAPTURE(tau);
		// A SQUARE, deliberately: it is the shape a one-pole changes most, so an error in the steady
		// state has the most room to show. A sine would be nearly its own answer.
		std::vector<float> wave(kM);
		for (size_t i = 0; i < kM; ++i) {
			wave[i] = (i < kM / 2) ? 1.0f : 0.0f;
		}
		const std::vector<float> raw = wave;

		smoothPeriodic(wave, period, tau);

		// THE BRUTE FORCE: the same recursion, run until it has forgotten where it started. 400
		// periods is far past any tau in the range — the worst case here decays by exp(-0.25/0.5)
		// per period, so 400 of them is e^-200.
		const double r = std::exp(-(period / static_cast<double>(kM)) / tau);
		const double a = 1.0 - r;
		double y = 0.0;
		for (int rep = 0; rep < 400; ++rep) {
			for (size_t i = 0; i < kM; ++i) {
				y = r * y + a * static_cast<double>(raw[i]);
			}
		}
		for (size_t i = 0; i < kM; ++i) {
			y = r * y + a * static_cast<double>(raw[i]);
			CAPTURE(i);
			CHECK(wave[i] == doctest::Approx(static_cast<float>(y)).epsilon(1e-4));
		}
	}

	SUBCASE("a filter far slower than the wave passes its MEAN, which is the 0/0 limit of the formula") {
		// THE CASE THE CLOSED FORM CANNOT EVALUATE DIRECTLY: as tau grows, the numerator and the
		// denominator both vanish. The answer is the input's average, and it is asserted as a VALUE
		// rather than as "something sensible".
		std::vector<float> wave(kM);
		for (size_t i = 0; i < kM; ++i) {
			wave[i] = (i < kM / 2) ? 1.0f : 0.0f;
		}
		double sum = 0.0;
		for (const float v : wave) {
			sum += v;
		}
		const auto mean = static_cast<float>(sum / static_cast<double>(kM));
		smoothPeriodic(wave, 0.25, 1.0e9);
		for (size_t i = 0; i < kM; ++i) {
			CAPTURE(i);
			CHECK(wave[i] == doctest::Approx(mean));
		}
		// AND NOT VACUOUS: the mean is not what the wave already was.
		CHECK(mean > 0.4f);
		CHECK(mean < 0.6f);
	}

	SUBCASE("and it is inert when there is nothing to smooth, rather than nearly inert") {
		// OFF MUST BE EXACT. A card that drew a barely-filtered wave at the bottom of the travel would
		// be the `Smoothing On` confusion again, in pixels.
		std::vector<float> wave(kM);
		for (size_t i = 0; i < kM; ++i) {
			wave[i] = static_cast<float>(i) / static_cast<float>(kM);
		}
		const std::vector<float> before = wave;
		smoothPeriodic(wave, 0.25, 0.0);            // tau 0: off
		smoothPeriodic(wave, 0.0, 0.02);            // no period: nothing to smooth over
		for (size_t i = 0; i < kM; ++i) {
			CAPTURE(i);
			CHECK(wave[i] == before[i]); // bit-exact, not approximately
		}
	}
}


TEST_CASE("lfo: a FREE synced wave under a Rate route follows the song's step, and a STOPPED transport does not freeze it") {
	// The MSEG's case beside this one has the story. The LFO placed its phase at
	// `fmod(beats, window) / window` every tick, so a Rate route scrubbed it, and it never asked whether
	// the transport was PLAYING (the MSEG always has), so a stopped host freezing on one position froze
	// the wave. Option A: follow the song's step (`mod::TransportLock`); stopped, free-run.
	Settings s;
	s.wave = Waveform::Sine;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = 13; // 1/4
	s.timeScale = 0.0f;
	constexpr double kSampleRate = 48000.0;
	const double tickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;
	constexpr double kSecondsPerBeat = 0.5;
	const double tickBeats = tickSeconds / kSecondsPerBeat;
	const double division = device::kDivisionBeats[s.timeBase - 1];
	const auto windowOf = [&](float scale) { return division * feathers::mod::timeSeconds(scale); };
	const auto circular = [](double a, double b) {
		double d = (a - std::floor(a)) - (b - std::floor(b));
		return std::abs(d - std::round(d));
	};
	feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);

	Shape shape;
	shape.prepare(kSampleRate);

	SUBCASE("playing, with the Rate moving") {
		clock.playing = true;
		double expected = 0.0;
		double worst = 0.0;
		double quotientMiss = 0.0;
		for (int k = 0; k < 3000; ++k) {
			clock.songPositionBeats = 64.0 + k * tickBeats;
			s.timeScale = 0.4f * static_cast<float>(std::sin(2.0 * M_PI * k / 700.0));
			const double w = windowOf(s.timeScale);
			expected = k == 0 ? clock.songPositionBeats / w : expected + tickBeats / w;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(shape.playhead().phase, expected));
			quotientMiss = std::max(quotientMiss, circular(clock.songPositionBeats / w, expected));
		}
		CAPTURE(worst);
		CAPTURE(quotientMiss);
		CHECK(worst < 1e-4);
		REQUIRE(quotientMiss > 0.3);
	}

	SUBCASE("playing and unmodulated, the phase IS the song divided by the window") {
		clock.playing = true;
		double worst = 0.0;
		for (int k = 0; k < 3000; ++k) {
			clock.songPositionBeats = 64.0 + k * tickBeats;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(shape.playhead().phase, clock.songPositionBeats / windowOf(s.timeScale)));
		}
		CHECK(worst < 1e-5);
	}

	SUBCASE("STOPPED at a position the host still reports: the wave runs at its rate") {
		clock.playing = false;
		clock.songPositionBeats = 10.6;
		constexpr int kTicks = 100;
		for (int k = 0; k < kTicks; ++k) {
			shape.tick(s, false, false, clock);
		}
		// Free restarts at its first tick (phase 0) and then advances every tick at 1 / (window × spb) Hz.
		const double hz = 1.0 / (windowOf(s.timeScale) * kSecondsPerBeat);
		CHECK(circular(shape.playhead().phase, kTicks * hz * tickSeconds) < 1e-4);
		// THE FROZEN READING THE OLD RULE GAVE, which this must not be.
		REQUIRE(circular(kTicks * hz * tickSeconds, 10.6 / windowOf(s.timeScale)) > 0.1);
	}
}

TEST_CASE("backlog NJ: FREE on a synced base offsets the song-locked phase by the Phase control, as the Hz base does") {
	// The lead's follow-up, 2026-10-17: Phase was live on a synced Free LFO and moved nothing — the honesty rule. Now
	// the phase is the song in beats over the window, plus Phase: 1/4 is one beat, so at 120 bpm the phase is
	// beats + Phase. Two Phase settings, so the case is about the control and not about one offset that happens to fit.
	constexpr double kSampleRate = 48000.0;
	constexpr double kSecondsPerBeat = 0.5;
	const double tickBeats = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate / kSecondsPerBeat;
	const auto circular = [](double a, double b) {
		double d = (a - std::floor(a)) - (b - std::floor(b));
		return std::abs(d - std::round(d));
	};
	for (const float phase : { 0.3f, 0.75f }) {
		CAPTURE(phase);
		Settings s;
		s.wave = Waveform::Sine;
		s.mode = feathers::mod::Mode::Free;
		s.timeBase = 13; // 1/4: one beat
		s.timeScale = 0.0f;
		s.phase = phase;
		REQUIRE(device::kDivisionBeats[s.timeBase - 1] == 1.0);
		Shape shape;
		shape.prepare(kSampleRate);
		feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
		clock.playing = true;
		double worst = 0.0;
		for (int k = 0; k < 1500; ++k) {
			clock.songPositionBeats = 17.2 + k * tickBeats;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(shape.playhead().phase, clock.songPositionBeats + phase));
		}
		CHECK(worst < 1e-5);
	}
}

TEST_CASE("backlog NJ: FREE in Hz follows the song's SECONDS while playing — phase = Rate x seconds + Phase") {
	// THE DESIGN: in Hz, Free mode's phase follows song time while the transport plays — Rate times the
	// song position in SECONDS, plus the Phase control; stopped, it runs free; a playhead jump moves it with the song.
	// Restated from the design rather than from the code: Rate's stored number is log2 of the frequency, so 1 is 2 Hz.
	Settings s;
	s.wave = Waveform::Sine;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = 0; // Hz
	s.timeScale = 1.0f;
	s.phase = 0.25f;
	constexpr double kHz = 2.0;
	constexpr double kSampleRate = 48000.0;
	const double tickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;
	const auto circular = [](double a, double b) {
		double d = (a - std::floor(a)) - (b - std::floor(b));
		return std::abs(d - std::round(d));
	};
	const auto expectedAt = [&](double seconds) { return kHz * seconds + static_cast<double>(s.phase); };
	REQUIRE(Param::TimeBase == static_cast<Param>(5));
	REQUIRE(std::string(kTimeBaseNames[0]) == "Hz");

	Shape shape;
	shape.prepare(kSampleRate);
	feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(0.5);
	clock.playing = true;

	SUBCASE("playing: every tick's phase is the song's seconds times the rate, plus Phase") {
		double worst = 0.0;
		for (int k = 0; k < 2000; ++k) {
			clock.songPositionSeconds = 7.3 + k * tickSeconds;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(shape.playhead().phase, expectedAt(clock.songPositionSeconds)));
		}
		CHECK(worst < 1e-4);
		// AND THE VALUE IS THE WAVE AT THAT PHASE — the published number, not only the playhead.
		CHECK(shape.value() == doctest::Approx(eval(s, static_cast<float>(expectedAt(clock.songPositionSeconds)
		                                                                  - std::floor(expectedAt(clock.songPositionSeconds)))))
		                           .epsilon(1e-3));
	}

	SUBCASE("a tempo change mid-song does not move it: the beats lurch, the seconds do not") {
		double worst = 0.0;
		double beats = 30.0;
		for (int k = 0; k < 2000; ++k) {
			const double spb = k < 1000 ? 0.5 : 0.37; // 120 bpm, then about 162
			clock = feathers::mod::TransportClock::fromSecondsPerBeat(spb);
			clock.playing = true;
			clock.songPositionSeconds = 15.0 + k * tickSeconds;
			beats += tickSeconds / spb;
			clock.songPositionBeats = beats;
			shape.tick(s, false, false, clock);
			worst = std::max(worst, circular(shape.playhead().phase, expectedAt(clock.songPositionSeconds)));
		}
		CHECK(worst < 1e-4);
	}

	SUBCASE("a playhead jump moves the phase with the song, either way") {
		for (int k = 0; k < 50; ++k) {
			clock.songPositionSeconds = 3.0 + k * tickSeconds;
			shape.tick(s, false, false, clock);
		}
		for (const double to : { 41.137, 1.009 }) {
			CAPTURE(to);
			clock.songPositionSeconds = to;
			shape.tick(s, false, false, clock);
			CHECK(circular(shape.playhead().phase, expectedAt(to)) < 1e-5);
		}
	}

	SUBCASE("stopped at a position the host still reports: it runs free at its rate") {
		clock.playing = false;
		clock.songPositionSeconds = 10.45;
		constexpr int kTicks = 100;
		for (int k = 0; k < kTicks; ++k) {
			shape.tick(s, false, false, clock);
		}
		// Free restarts at its first tick (at Phase) and advances every tick by Rate x a tick.
		const double free = static_cast<double>(s.phase) + kTicks * kHz * tickSeconds;
		CHECK(circular(shape.playhead().phase, free) < 1e-4);
		REQUIRE(circular(free, expectedAt(10.45)) > 0.1); // the song's reading, which this must not be
	}

	SUBCASE("the random walk in Hz steps with the song's seconds too") {
		s.wave = Waveform::Random;
		for (int k = 0; k < 500; ++k) {
			clock.songPositionSeconds = 2.0 + k * tickSeconds;
			shape.tick(s, false, false, clock);
			const double walked = kHz * clock.songPositionSeconds;
			CHECK(shape.playhead().ordinal == static_cast<uint32_t>(std::floor(walked)));
		}
	}

	SUBCASE("only FREE follows: LFO Retrig in Hz still runs from its own start") {
		s.mode = feathers::mod::Mode::LfoRetrig;
		clock.songPositionSeconds = 10.6;
		shape.tick(s, false, false, clock);
		CHECK(circular(shape.playhead().phase, static_cast<double>(s.phase) + kHz * tickSeconds) < 1e-4);
	}
}

TEST_CASE("lfo: a synced RANDOM walk follows the song's step under a Rate route, and walks when the transport is stopped") {
	// **THE SHAPED WAVES' RULE, ON THE WALK** (option A, 2026-09-25). The step was
	// `floor(position / stepBeats)` every tick: a Rate route scrubbed the walk through steps, and with no
	// `playing` test a stopped host that still reports its position froze it on one. Asserted on the
	// playhead, whose ordinal is the step and whose phase is the fraction into it.
	Settings s;
	s.wave = Waveform::RandomGlide;
	s.mode = feathers::mod::Mode::Gate;
	s.timeBase = 13; // 1/4
	s.timeScale = 0.0f;
	constexpr double kSampleRate = 48000.0;
	const double tickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;
	constexpr double kSecondsPerBeat = 0.5;
	const double tickBeats = tickSeconds / kSecondsPerBeat;
	const double division = device::kDivisionBeats[s.timeBase - 1];
	const auto windowOf = [&](float scale) { return division * feathers::mod::timeSeconds(scale); };
	feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
	Shape shape;
	shape.prepare(kSampleRate);
	const auto walked = [&] {
		return static_cast<double>(shape.playhead().ordinal) + static_cast<double>(shape.playhead().phase);
	};

	SUBCASE("playing, with the Rate moving") {
		clock.playing = true;
		double expected = 0.0;
		double worst = 0.0;
		double quotientMiss = 0.0;
		for (int k = 0; k < 3000; ++k) {
			clock.songPositionBeats = 64.0 + k * tickBeats;
			s.timeScale = 0.4f * static_cast<float>(std::sin(2.0 * M_PI * k / 700.0));
			const double w = windowOf(s.timeScale);
			expected = k == 0 ? clock.songPositionBeats / w : expected + tickBeats / w;
			shape.tick(s, true, k == 0, clock);
			worst = std::max(worst, std::abs(walked() - expected));
			quotientMiss = std::max(quotientMiss, std::abs(clock.songPositionBeats / w - expected));
		}
		CAPTURE(worst);
		CAPTURE(quotientMiss);
		CHECK(worst < 1e-4);
		REQUIRE(quotientMiss > 1.0); // the old rule would have been whole steps away
	}

	SUBCASE("STOPPED at a position the host still reports: the walk steps at its rate") {
		clock.playing = false;
		clock.songPositionBeats = 10.6;
		constexpr int kTicks = 1000;
		for (int k = 0; k < kTicks; ++k) {
			shape.tick(s, true, k == 0, clock);
		}
		const double hz = 1.0 / (windowOf(s.timeScale) * kSecondsPerBeat);
		CHECK(std::abs(walked() - kTicks * hz * tickSeconds) < 1e-3);
		// THE FROZEN READING THE OLD RULE GAVE: step 21 of the song, not the few the rate has walked.
		REQUIRE(std::abs(kTicks * hz * tickSeconds - 10.6 / windowOf(s.timeScale)) > 1.0);
	}
}

// SMOOTHING CLOSES — the DAHDSR's bug (in smooth mode it never fully closed) asked of this device. Only Once ENDS: the gate modes loop, so a gate falling leaves the wave running.
// A finished Once holds the value it ended on, so that is its rest, and the smoothed output must ARRIVE
// there exactly. The rest is taken from the SAME settings run with smoothing off — a road with no
// one-pole in it — rather than from anything the smoothed run reports.
TEST_CASE("lfo: smoothing closes — a finished Once's smoothed output reaches the value the shape ended on EXACTLY, within the cycle + tau ln(1e6)") {
	constexpr double kSnapBound = 1e-6; // a millionth of full scale, -120 dB, stated rather than read
	constexpr double kTick = 64.0 / 48000.0;
	for (const float smoothing : { mod::kSmoothingLog2SecondsMin, -9.0f, -5.0f, -2.0f,
	                               mod::kSmoothingLog2SecondsMax }) {
		for (const Waveform wave : { Waveform::Sine, Waveform::Triangle, Waveform::Square, Waveform::Random,
		                             Waveform::RandomGlide }) {
			for (const bool bipolar : { false, true }) {
				CAPTURE(smoothing);
				CAPTURE(static_cast<int>(wave));
				CAPTURE(bipolar);
				Settings s;
				s.mode = Mode::Once;
				s.wave = wave;
				s.phase = 0.0f;
				s.bipolar = bipolar;
				const double tau = mod::smoothingTau(smoothing);
				// THIS DEVICE'S RAW IS ALREADY BIPOLAR (-1..1), so the last jump the one-pole settles from can be
				// 2 wide rather than 1 — a bipolar Square jumps -1 to +1 half way through.
				const double span = bipolar ? 2.0 : 1.0;
				const int bound = static_cast<int>((1.0 + tau * std::log(span / kSnapBound) + 0.1) / kTick);

				Settings off = s;
				off.smoothing = mod::kSmoothingLog2SecondsMin;
				Shape reference;
				reference.prepare(48000.0);
				for (int i = 0; i < bound; ++i) {
					(void)reference.tick(off, true, false, device::TransportClock::unknown());
				}
				const float rest = reference.value();
				REQUIRE_FALSE(reference.playhead().running()); // the cycle did finish

				s.smoothing = smoothing;
				Shape shape;
				shape.prepare(48000.0);
				float was = 0.0f;
				int closedAt = -1;
				double cutOff = 0.0;
				for (int i = 0; i < bound; ++i) {
					const float v = shape.tick(s, true, false, device::TransportClock::unknown());
					if (v == rest && closedAt < 0 && !shape.playhead().running()) {
						// WHAT THE SNAP TOOK: the residue an un-snapped one-pole, restated, would still carry.
						const double keep = tau > 0.0 ? std::exp(-kTick / tau) : 0.0;
						cutOff = std::abs(static_cast<double>(was) - static_cast<double>(rest)) * keep / (bipolar ? 2.0 : 1.0);
						closedAt = i;
					}
					was = v;
				}
				CAPTURE(closedAt);
				CAPTURE(rest);
				CAPTURE(static_cast<double>(shape.value()) - static_cast<double>(rest));
				CHECK(shape.value() == rest);
				CHECK(cutOff <= kSnapBound + static_cast<double>(std::numeric_limits<float>::epsilon()));
				for (int i = 0; i < 100; ++i) {
					(void)shape.tick(s, true, false, device::TransportClock::unknown());
				}
				CHECK(shape.value() == rest);
			}
		}
	}
}

namespace {

/// A 1 Hz wave: 750 ticks of 64 samples a cycle at 48 kHz.
constexpr int kTicksPerCycle = 750;

/// Ticks `shape` `ticks` times under `gate`, returning every value.
std::vector<float> run(Shape& shape, const Settings& s, bool gate, int ticks) {
	std::vector<float> out;
	for (int i = 0; i < ticks; ++i) {
		out.push_back(shape.tick(s, gate, false, device::TransportClock::unknown()));
	}
	return out;
}

} // namespace

TEST_CASE("lfo Gate mode: at the key the output goes to NEUTRAL at once and the phase stops — unipolar and bipolar, smoothing off; and a new note starts it again") {
	// MEASURED before: the wave went on cycling after the key, 0..1, for ever — the gate was heard only as a restart.
	for (const bool bipolar : { false, true }) {
		CAPTURE(bipolar);
		Settings s;
		s.mode = Mode::Gate;
		s.wave = Waveform::Triangle;
		s.bipolar = bipolar;
		Shape shape;
		shape.prepare(48000.0);
		const auto held = run(shape, s, true, 225); // 0.3 s
		// NOT VACUOUS: it was running and away from neutral when the key went up (triangle at 0.3 = 0.6).
		REQUIRE(std::abs(held.back() - applyBipolar(s, 0.6f)) < 0.01f);
		const auto after = run(shape, s, false, kTicksPerCycle + 375);
		for (size_t i = 0; i < after.size(); ++i) {
			CAPTURE(i);
			REQUIRE(after[i] == 0.0f);
		}
		CHECK_FALSE(shape.playhead().running());
		CHECK(shape.idle());
		// A NEW NOTE STARTS IT AGAIN, from its start.
		const auto again = run(shape, s, true, 75); // 0.1 s: triangle 0.2
		CHECK(std::abs(again.back() - applyBipolar(s, 0.2f)) < 0.01f);
	}
}

TEST_CASE("lfo Gate mode reaches neutral through ITS OWN smoothing, not by a step") {
	Settings s;
	s.mode = Mode::Gate;
	s.wave = Waveform::Triangle;
	s.smoothing = -4.0f; // 1/16 s
	Shape shape;
	shape.prepare(48000.0);
	const float before = run(shape, s, true, 225).back();
	REQUIRE(before > 0.3f);
	const auto after = run(shape, s, false, 3 * kTicksPerCycle);
	// FALLS, never rises, and the first step down is the one-pole's, not the whole distance.
	CHECK(after.front() > 0.5f * before);
	for (size_t i = 1; i < after.size(); ++i) {
		CAPTURE(i);
		REQUIRE(after[i] <= after[i - 1]);
	}
	CHECK(after.back() == 0.0f); // and ARRIVES — `mod::smoothingStep`'s snap
	CHECK(shape.idle());
}

TEST_CASE("lfo Loop mode: at the key it FINISHES THE CYCLE it is in, then rests at neutral — and a cycle ends where it began, a phase offset included") {
	for (const float start : { 0.0f, 0.25f }) {
		CAPTURE(start);
		Settings s;
		s.mode = Mode::Loop;
		s.wave = Waveform::Triangle;
		s.phase = start;
		Shape released;
		released.prepare(48000.0);
		Shape held;
		held.prepare(48000.0);
		(void)run(released, s, true, 225); // key up 0.3 s into the cycle
		(void)run(held, s, true, 225);
		const auto out = run(released, s, false, kTicksPerCycle);
		const auto own = run(held, s, true, kTicksPerCycle);
		// THE REST OF THE CYCLE IS ITS OWN TIMELINE: 0.7 s more, exactly what a held key plays.
		for (int i = 0; i < 520; ++i) {
			CAPTURE(i);
			REQUIRE(out[static_cast<size_t>(i)] == own[static_cast<size_t>(i)]);
		}
		// NOT VACUOUS: the held one is still cycling over that stretch and well away from neutral in it.
		REQUIRE(*std::max_element(own.begin(), own.begin() + 520) > 0.5f);
		// AND FROM THE CYCLE'S END (1 s from the start), neutral. Smoothing off, so neutral exactly.
		for (size_t i = 530; i < out.size(); ++i) {
			CAPTURE(i);
			REQUIRE(out[i] == 0.0f);
		}
		CHECK(own.back() != 0.0f); // the held one goes on
		CHECK_FALSE(released.playhead().running());
	}
}

TEST_CASE("lfo Loop's random walk runs out its STEP after the key, then rests at neutral") {
	Settings s;
	s.mode = Mode::Loop;
	s.wave = Waveform::Random;
	Shape shape;
	shape.prepare(48000.0);
	(void)run(shape, s, true, kTicksPerCycle + 225); // 0.3 s into its second step
	const float step = shape.value();
	REQUIRE(step != 0.0f);
	const auto out = run(shape, s, false, kTicksPerCycle);
	for (int i = 0; i < 520; ++i) {
		CAPTURE(i);
		REQUIRE(out[static_cast<size_t>(i)] == step); // the same step, held to its end
	}
	CHECK(out.back() == 0.0f);
}

TEST_CASE("lfo Free and LFO Retrig keep running after the key — only Gate and Loop stop") {
	for (const Mode mode : { Mode::Free, Mode::LfoRetrig }) {
		CAPTURE(static_cast<int>(mode));
		Settings s;
		s.mode = mode;
		s.wave = Waveform::Triangle;
		Shape shape;
		shape.prepare(48000.0);
		(void)run(shape, s, true, 225);
		const auto after = run(shape, s, false, kTicksPerCycle);
		CHECK(*std::max_element(after.begin(), after.end()) - *std::min_element(after.begin(), after.end()) > 0.9f);
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG PA: STEREO PHASE — the right ear is the same deterministic shape read at phase + offset,
// 0–180°; Random and S&H read the SAME walk that far along, not a second draw. At 0 it is today's LFO.
namespace {
namespace pa {
/// A TICK OF 64 SAMPLES AT 8192 Hz IS 2^-7 s, so an 8 Hz wave (Rate 3, log2 of the Hz) moves EXACTLY 1/16 of a cycle a
/// tick and every phase it visits is exact. "90° later" is then exactly four ticks later, which is what lets the right
/// ear be checked against the LEFT EAR'S OWN FUTURE, to the bit, rather than against a restatement of the arithmetic.
constexpr double kSampleRate = 8192.0;
constexpr int kTicksPerCycle = 16;

Settings freeAt8Hz(Waveform wave, double degrees) {
	Settings s;
	s.wave = wave;
	s.mode = Mode::Free;
	s.timeBase = 0; // Hz
	s.timeScale = 3.0f;
	s.stereoPhase = static_cast<float>(degrees * M_PI / 180.0);
	return s;
}

struct Ears {
	std::vector<float> left;
	std::vector<float> right;
};

Ears earsOver(const Settings& s, int ticks, bool gate = true) {
	Shape shape;
	shape.prepare(kSampleRate);
	Ears e;
	for (int k = 0; k < ticks; ++k) {
		shape.tick(s, gate, false, device::TransportClock::unknown());
		e.left.push_back(shape.value());
		e.right.push_back(shape.valueRight());
	}
	return e;
}
} // namespace pa
} // namespace

TEST_CASE("backlog PA: Stereo Phase — the right ear NOW is exactly what the left ear reads that far around the cycle LATER, on every wave, the walks included") {
	REQUIRE(feathers::mod::timeSeconds(3.0f) == 8.0);
	for (const Waveform wave : { Waveform::Sine, Waveform::Triangle, Waveform::Square, Waveform::Random, Waveform::RandomGlide }) {
		for (const double degrees : { 90.0, 180.0 }) {
			CAPTURE(static_cast<int>(wave));
			CAPTURE(degrees);
			const Settings s = pa::freeAt8Hz(wave, degrees);
			// A STEP IS A WALK'S CYCLE (the playhead's phase is the step's fraction), so the walks' "later" is in steps.
			const int lead = static_cast<int>(pa::kTicksPerCycle * degrees / 360.0);
			const pa::Ears e = pa::earsOver(s, 20 * pa::kTicksPerCycle + lead);
			size_t apart = 0;
			for (size_t k = 0; k + static_cast<size_t>(lead) < e.left.size(); ++k) {
				CAPTURE(k);
				// THE SAME SEQUENCE: the right ear's value is the left ear's, `lead` ticks on — a second draw could not be.
				CHECK(e.right[k] == e.left[k + static_cast<size_t>(lead)]);
				apart += e.right[k] != e.left[k] ? 1u : 0u;
			}
			// NOT VACUOUS: the ears really are apart — a fifth of the time at least (a stepped walk 90° on differs only in the
			// last quarter of each step, where the right ear has already stepped).
			CHECK(apart > e.left.size() / 5);
		}
	}
}

TEST_CASE("backlog PA: Stereo Phase restated — the right ear is the wave evaluated at the playhead's phase + offset, and a walk's is its seed's value that many steps on") {
	for (const double degrees : { 45.0, 90.0, 135.0, 180.0 }) {
		CAPTURE(degrees);
		const double lead = degrees / 360.0;
		{
			// A SINE, written out: 0.5 + 0.5·sin(2π·(phase + lead)).
			Settings s = pa::freeAt8Hz(Waveform::Sine, degrees);
			s.timeScale = 2.3f; // an inexact rate: the restatement does not lean on the exact grid above
			Shape shape;
			shape.prepare(48000.0);
			for (int k = 0; k < 400; ++k) {
				shape.tick(s, true, false, device::TransportClock::unknown());
				const double p = shape.playhead().phase + lead;
				CHECK(std::abs(shape.valueRight() - (0.5 + 0.5 * std::sin(2.0 * M_PI * (p - std::floor(p))))) < 1e-5);
			}
		}
		{
			// THE WALK: `randomWalkValue(seed, step)` IS the sequence, so the right ear's step is the left's plus however
			// many whole steps the lead carries the step's fraction past.
			Settings s = pa::freeAt8Hz(Waveform::Random, degrees);
			s.seed = 417;
			s.timeScale = 3.1f;
			Shape shape;
			shape.prepare(48000.0);
			int crossed = 0;
			for (int k = 0; k < 1500; ++k) {
				shape.tick(s, true, false, device::TransportClock::unknown());
				const feathers::mod::Playhead where = shape.playhead();
				const int ahead = static_cast<int>(std::floor(static_cast<double>(where.phase) + lead));
				crossed += ahead;
				CHECK(shape.valueRight() == randomWalkValue(s.seed, static_cast<int>(where.ordinal) + ahead));
				CHECK(shape.value() == randomWalkValue(s.seed, static_cast<int>(where.ordinal)));
			}
			CHECK(crossed > 0); // NOT VACUOUS: the lead did carry the right ear into the next step
		}
	}
}

TEST_CASE("backlog PA: Stereo Phase on a song-locked Free wave is transport-deterministic — the right ear at song position x is the left ear at x + offset × window") {
	constexpr double kSecondsPerBeat = 0.5;
	for (const double degrees : { 90.0, 180.0 }) {
		CAPTURE(degrees);
		const double lead = degrees / 360.0;
		Settings s;
		s.wave = Waveform::Sine;
		s.mode = Mode::Free;
		s.stereoPhase = static_cast<float>(degrees * M_PI / 180.0);
		SUBCASE("synced: the window in beats") {
			s.timeBase = 13; // 1/4
			const double window = device::kDivisionBeats[s.timeBase - 1] * feathers::mod::timeSeconds(s.timeScale);
			for (const double x : { 3.1, 17.37, 64.9 }) {
				CAPTURE(x);
				feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
				clock.playing = true;
				clock.songPositionBeats = x;
				Shape here;
				here.prepare(48000.0);
				here.tick(s, false, false, clock);
				clock.songPositionBeats = x + lead * window;
				Shape later;
				later.prepare(48000.0);
				later.tick(s, false, false, clock);
				CHECK(std::abs(here.valueRight() - later.value()) < 1e-5);
				CHECK(std::abs(here.valueRight() - here.value()) > 1e-3); // NOT VACUOUS
			}
		}
		SUBCASE("in Hz: the window in seconds") {
			s.timeBase = 0;
			s.timeScale = 1.0f; // 2 Hz
			for (const double t : { 1.13, 7.3, 22.05 }) {
				CAPTURE(t);
				feathers::mod::TransportClock clock = feathers::mod::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
				clock.playing = true;
				clock.songPositionSeconds = t;
				Shape here;
				here.prepare(48000.0);
				here.tick(s, false, false, clock);
				clock.songPositionSeconds = t + lead / 2.0;
				Shape later;
				later.prepare(48000.0);
				later.tick(s, false, false, clock);
				CHECK(std::abs(here.valueRight() - later.value()) < 1e-5);
				CHECK(std::abs(here.valueRight() - here.value()) > 1e-3);
			}
		}
	}
}

TEST_CASE("backlog PA: at Stereo Phase 0 the right ear IS the left, bit for bit, on every wave and mode — and turning it back to 0 rejoins them at once") {
	for (const Waveform wave : { Waveform::Sine, Waveform::Triangle, Waveform::Square, Waveform::Random, Waveform::RandomGlide }) {
		for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::Free, Mode::LfoRetrig }) {
			CAPTURE(static_cast<int>(wave));
			CAPTURE(static_cast<int>(mode));
			Settings s = pa::freeAt8Hz(wave, 0.0);
			s.mode = mode;
			s.smoothing = 0.0f; // smoothing on: the right ear's own pole must agree with the left's too
			const pa::Ears e = pa::earsOver(s, 300);
			for (size_t k = 0; k < e.left.size(); ++k) {
				CAPTURE(k);
				REQUIRE(e.right[k] == e.left[k]);
			}
		}
	}
	// AND BACK: a wave run apart, then set to 0, has equal ears on the very next tick.
	Settings s = pa::freeAt8Hz(Waveform::Triangle, 90.0);
	Shape shape;
	shape.prepare(pa::kSampleRate);
	for (int k = 0; k < 37; ++k) {
		shape.tick(s, true, false, device::TransportClock::unknown());
	}
	REQUIRE(shape.valueRight() != shape.value());
	s.stereoPhase = 0.0f;
	shape.tick(s, true, false, device::TransportClock::unknown());
	CHECK(shape.valueRight() == shape.value());
}

TEST_CASE("backlog PA: Stereo Phase under the key — Gate rests both ears at neutral, and a finished Once holds both where it ended") {
	{
		Settings s = pa::freeAt8Hz(Waveform::Sine, 90.0);
		s.mode = Mode::Gate;
		Shape shape;
		shape.prepare(pa::kSampleRate);
		for (int k = 0; k < 21; ++k) {
			shape.tick(s, true, false, device::TransportClock::unknown());
		}
		REQUIRE(shape.valueRight() != shape.value());
		shape.tick(s, false, false, device::TransportClock::unknown());
		CHECK(shape.value() == 0.0f);
		CHECK(shape.valueRight() == 0.0f);
	}
	{
		Settings s = pa::freeAt8Hz(Waveform::Triangle, 90.0);
		s.mode = Mode::Once;
		Shape shape;
		shape.prepare(pa::kSampleRate);
		for (int k = 0; k < 3 * pa::kTicksPerCycle; ++k) {
			shape.tick(s, true, false, device::TransportClock::unknown());
		}
		CHECK(shape.valueRight() == shape.value());
		CHECK(shape.value() == eval(s, 1.0f));
	}
}

TEST_CASE("backlog PA: Stereo Phase is stored by its own key, in radians read as degrees, and round-trips the row") {
	const device::ParamDesc& p = kParamDescs[static_cast<size_t>(Param::StereoPhase)];
	CHECK(std::string(p.key) == "stereo_phase");
	CHECK(p.unit == device::Unit::Degrees);
	CHECK(p.min == 0.0);
	CHECK(p.max == doctest::Approx(M_PI));
	CHECK(p.def == 0.0);
	CHECK(kOutputs[0].pairWhile == static_cast<int>(Param::StereoPhase));
	Settings s;
	CHECK(s.stereoPhase == 0.0f);
	s.stereoPhase = 1.25f;
	CHECK(settingsOf(rowOf(s), rowOf(s).size()).stereoPhase == 1.25f);
	// A ROW FROM BEFORE IT (one field short) reads the default: mono.
	CHECK(settingsOf(rowOf(s), rowOf(s).size() - 1).stereoPhase == 0.0f);
}
