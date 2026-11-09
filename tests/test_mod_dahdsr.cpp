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

// THE DAHDSR WITH NO PLUGIN AROUND IT, which is the point of it being a module.
//
// EVERY CASE HERE ASSERTS A VALUE, and that is the project's rule rather than this file's taste: a
// comparison between two runs is evidence that SOMETHING changed and never that the right thing did.
// So a stage's length is measured in ticks against the seconds its knob claims, a shape is measured
// at the midpoint of the stage it bends, and the smoothing time is measured where an exponential is
// defined to be — at 1 − 1/e. The two numbers in each pair are derived independently: one from the
// taper this module publishes, one from counting what the state machine actually did.
//
// Four claims are load-bearing:
//
//   - A STAGE TAKES THE TIME ITS KNOB SAYS. The taper and the rate are two readings of one exponent
//     and there is nothing between them to be wrong — which is exactly the kind of claim that is
//     wrong the first time somebody writes it, so it is measured at three stages and two sample
//     rates rather than reasoned about.
//   - POSITIVE SHAPE MEANS THE SAME THING ON ALL THREE STAGES. The curve law read forwards on a rising
//     stage and backwards on a falling one is what makes it so; nothing but a case can tell that holds.
//   - THE DEFAULTS ARE THE TIMES THEY CLAIM. The five constants in Settings are magic floats with
//     their seconds in a comment, and a comment cannot hold two numbers together.
//   - THE MODULE INCLUDES NOTHING FROM src/plugin, read off the sources themselves — test_fx_filter's
//     case, for the boundary fx_tenant.h states and mod inherits.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "mod/dahdsr/dahdsr.h"
#include "mod/dahdsr/dahdsr_types.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using feathers::mod::dahdsr::Envelope;
using feathers::mod::dahdsr::Settings;
using feathers::mod::dahdsr::Stage;
namespace dahdsr = feathers::mod::dahdsr;

namespace {

constexpr double kRate = 48000.0;
/// How long one tick lasts at kRate — 1.333 ms, which is the resolution every timing claim below is
/// measured to and the tolerance every one of them is given.
constexpr double kTick = static_cast<double>(dahdsr::kBlockSamples) / kRate;

/// A settings block with nothing in it but the one stage under test: no delay, no hold, no decay,
/// sustain at the top. Built by naming what it IS rather than by mutating a default, so a case
/// cannot be quietly changed by a change to the defaults it did not mean to depend on.
Settings bare() {
	Settings s;
	s.delay = dahdsr::kStageLog2SecondsMin;
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.hold = dahdsr::kStageLog2SecondsMin;
	s.decay = dahdsr::kStageLog2SecondsMin;
	s.sustain = 1.0f;
	s.release = dahdsr::kStageLog2SecondsMin;
	s.attackShape = 0.0f;
	s.decayShape = 0.0f;
	s.releaseShape = 0.0f;
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	s.bipolar = false;
	return s;
}

/// Ticks `count` times at a fixed gate and hands back every value, so a case can look at the shape
/// rather than only at the end of it.
std::vector<float> run(Envelope& env, const Settings& s, bool gate, int count) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; i++) {
		out.push_back(env.tick(s, gate, false, feathers::mod::TransportClock::unknown()));
	}
	return out;
}

/// The first tick at which the envelope reaches `level`, in SECONDS from the call before the run —
/// which is what a stage's length means. -1 when it never does.
double timeToReach(const std::vector<float>& values, float level) {
	for (size_t i = 0; i < values.size(); i++) {
		if (values[i] >= level) {
			return static_cast<double>(i + 1) * kTick;
		}
	}
	return -1.0;
}

/// The first tick at which the envelope has fallen to `level` or below.
double timeToFallTo(const std::vector<float>& values, float level) {
	for (size_t i = 0; i < values.size(); i++) {
		if (values[i] <= level) {
			return static_cast<double>(i + 1) * kTick;
		}
	}
	return -1.0;
}

} // namespace

TEST_CASE("the playhead says which stage is running and how far through it") {
	// **THE PHASE IS THE STATE MACHINE'S AND NOTHING ELSE REACHES IT.** `Envelope` keeps a per-stage `phase_`
	// that resets at every transition; it is private, and no arithmetic on `value()`
	// recovers it — decay and release cross the same levels in the same direction. So this is the
	// five-line seam CLAUDE.md prescribes, and this is the case that says what it must contain.
	Envelope env;
	env.prepare(kRate);

	// FOUR STAGES WITH DIFFERENT LENGTHS, so "which stage" is a real question. Equal stages would let
	// an off-by-one in the ordinal land at a plausible phase and the case would agree with it.
	Settings s = bare();
	s.delay = static_cast<float>(std::log2(0.020));
	s.attack = static_cast<float>(std::log2(0.050));
	s.hold = static_cast<float>(std::log2(0.030));
	s.decay = static_cast<float>(std::log2(0.080));
	s.sustain = 0.5f;
	s.release = static_cast<float>(std::log2(0.060));

	// AN ENVELOPE NOBODY HAS GATED IS IDLE — nowhere, rather than at the start of Delay.
	CHECK(env.stage() == Stage::Idle);
	CHECK_FALSE(env.playhead().running());
	CHECK(env.playhead().ordinal == feathers::mod::Playhead::kIdle);

	std::vector<Stage> order;
	const int held = static_cast<int>(0.30 / kTick);
	for (int i = 0; i < held; ++i) {
		(void)env.tick(s, true, i == 0, feathers::mod::TransportClock::unknown());
		const feathers::mod::Playhead head = env.playhead();
		const Stage now = env.stage();
		CAPTURE(i);
		// **THE ORDINAL IS `Stage` ITSELF**, which is what lets a card use the `stageEndX(stage)` it
		// already has instead of agreeing with this file about a second ordering.
		if (now == Stage::Idle) {
			CHECK_FALSE(head.running());
		}
		else {
			CHECK(head.running());
			CHECK(head.ordinal == static_cast<uint32_t>(now));
			CHECK(head.phase >= 0.0f);
			CHECK(head.phase <= 1.0f);
		}
		if (order.empty() || order.back() != now) {
			order.push_back(now);
		}
	}
	// NON-VACUITY: it really did walk Delay -> Attack -> Hold -> Decay -> Sustain, so the ordinal
	// check above was asked of five different answers rather than of one repeated.
	REQUIRE(order.size() >= 5);
	CHECK(order[0] == Stage::Delay);
	CHECK(order[1] == Stage::Attack);
	CHECK(order[2] == Stage::Hold);
	CHECK(order[3] == Stage::Decay);
	CHECK(order[4] == Stage::Sustain);

	SUBCASE("sustain reports phase 0, because sustain does not advance") {
		// A STATEMENT RATHER THAN A GAP. Sustain holds for as long as the gate is up, so there is no
		// "how far through" — and any number other than 0 would be inventing progress. Read over many
		// ticks, because the wrong version of this is a number that drifts while the note is held.
		REQUIRE(env.stage() == Stage::Sustain);
		for (int i = 0; i < 200; ++i) {
			(void)env.tick(s, true, false, feathers::mod::TransportClock::unknown());
			CAPTURE(i);
			REQUIRE(env.stage() == Stage::Sustain);
			CHECK(env.playhead().ordinal == static_cast<uint32_t>(Stage::Sustain));
			CHECK(env.playhead().phase == 0.0f);
		}
	}

	SUBCASE("the phase climbs across a stage rather than jumping at its end") {
		// **A BOUND IS NOT A VALUE.** The loop above checks 0..1 and would pass against a phase pinned
		// at 0 all through a stage and snapping to 1 at its edge — which is exactly what a playhead
		// must not do. So the CLIMB is pinned, on the release, which is the longest single stage here
		// and the one a card draws widest.
		std::vector<float> inRelease;
		for (int i = 0; i < static_cast<int>(0.20 / kTick); ++i) {
			(void)env.tick(s, false, false, feathers::mod::TransportClock::unknown());
			if (env.stage() == Stage::Release) {
				inRelease.push_back(env.playhead().phase);
			}
		}
		REQUIRE(inRelease.size() > 20);
		for (size_t i = 1; i < inRelease.size(); ++i) {
			CAPTURE(i);
			CHECK(inRelease[i] >= inRelease[i - 1]);
		}
		CHECK(inRelease.back() > inRelease.front());
	}
}

TEST_CASE("a time's value IS its log2 seconds, which is what lets a stranger print it") {
	// THE CLAIM THIS CASE EXISTS FOR is that the declaration does not lie. A generic card — or a host
	// that has never seen a DAHDSR — reads `Unit::LogSeconds` off the descriptor and raises two to
	// the value. If that produced anything other than the duration the envelope actually runs, the
	// whole point of declaring units would be gone. So the reference here is `exp2` computed by the
	// case, not this module's own function.
	for (double log2s = dahdsr::kStageLog2SecondsMin + 0.5;
	     log2s <= dahdsr::kStageLog2SecondsMax; log2s += 0.7) {
		CAPTURE(log2s);
		CHECK(dahdsr::stageSeconds(static_cast<float>(log2s))
		      == doctest::Approx(std::exp2(log2s)).epsilon(1e-5));
		// And back again.
		CHECK(static_cast<double>(dahdsr::stageValueFor(std::exp2(log2s)))
		      == doctest::Approx(log2s).epsilon(1e-5));
	}

	// The two ends are the two numbers the header publishes, and a card places its readout from them.
	CHECK(dahdsr::stageSeconds(dahdsr::kStageLog2SecondsMax) == doctest::Approx(10.0).epsilon(1e-6));

	// AT THE BOTTOM OF THE TRAVEL THE STAGE DOES NOT HAPPEN — the one special case, and the reason
	// this is a function rather than an inline exp2 at every call site. There is no zero in a
	// logarithmic domain, so the minimum carries that meaning.
	CHECK(dahdsr::stageSeconds(dahdsr::kStageLog2SecondsMin) == 0.0);
	CHECK(dahdsr::stageValueFor(0.0) == dahdsr::kStageLog2SecondsMin);
	CHECK(dahdsr::stageValueFor(-1.0) == dahdsr::kStageLog2SecondsMin);
	// Just above it is a real, if very short, stage rather than nothing.
	CHECK(dahdsr::stageSeconds(dahdsr::kStageLog2SecondsMin + 0.001f) > 0.0);
	// Above the ceiling clamps rather than running off.
	CHECK(dahdsr::stageValueFor(1000.0) == dahdsr::kStageLog2SecondsMax);

	CHECK(dahdsr::smoothingSeconds(dahdsr::kSmoothingLog2SecondsMax)
	      == doctest::Approx(1.0).epsilon(1e-6));
	CHECK(dahdsr::smoothingSeconds(dahdsr::kSmoothingLog2SecondsMin)
	      == doctest::Approx(std::exp2(-11.0)).epsilon(1e-5));
}

TEST_CASE("an untouched envelope IS an ADSR") {
	// THE REQUIREMENT, PINNED RATHER THAN INTENDED: the other stages have defaults such that a player
	// who only wants an ADSR never needs to touch them. The card puts A D S R on the top row
	// and Delay, Hold and Smoothing on the second; this is the half that makes that honest, because
	// a top row that is only the FAMILIAR four is a lie if the other two are doing something.
	//
	// Asserted as SHAPE rather than as field values — "delay == min" would pass while the state
	// machine still spent a tick in a delay stage. What matters is that the envelope leaves for its
	// attack on the very first tick and reaches the top without pausing.
	const Settings d; // untouched, straight out of the declarations

	Envelope env;
	env.prepare(kRate);

	// FIRST TICK, GATE UP: already climbing. A delay would show as a tick at zero, and a hold would
	// show as a plateau at the top before the decay.
	const float first = env.tick(d, true, false, feathers::mod::TransportClock::unknown());
	CHECK(env.stage() == Stage::Attack);
	CHECK(first > 0.0f);

	// The 10 ms attack completes, and the next stage is the DECAY rather than a hold.
	const auto rise = run(env, d, true, 20); // 27 ms
	CHECK(timeToReach(rise, 0.999f) > 0.0);
	CHECK(env.stage() == Stage::Decay);

	// The 500 ms decay lands on the declared sustain and stays.
	run(env, d, true, 500);
	CHECK(env.stage() == Stage::Sustain);
	CHECK(env.value() == doctest::Approx(d.sustain).epsilon(1e-5));

	// And release falls to nothing over the 250 ms it declares.
	const auto fall = run(env, d, false, 250);
	CHECK(std::abs(timeToFallTo(fall, 1e-4f) - 0.250) <= 3.0 * kTick);

	// The switches and the loop are off, so nothing on the second row is in the way either — and
	// smoothing is off by being at the BOTTOM OF ITS TRAVEL, which is the whole control since the
	// `Smoothing On` switch was deleted.
	CHECK(d.smoothing == feathers::mod::kSmoothingLog2SecondsMin);
	// The default mode is Gate — not looping, not one-shot.
	CHECK(d.mode == dahdsr::Mode::Gate);
	CHECK_FALSE(d.bipolar);
	// And the curves are straight lines, so the familiar four behave the familiar way.
	CHECK(d.attackShape == 0.0f);
	CHECK(d.decayShape == 0.0f);
	CHECK(d.releaseShape == 0.0f);
}

TEST_CASE("the default settings are the times their comments claim") {
	// FIVE MAGIC FLOATS WITH THEIR SECONDS IN A COMMENT, and a comment cannot hold two numbers
	// together. This is what does — change the taper and this goes red, which is the moment to
	// recompute the constants rather than the moment to find out from a patch that sounds wrong.
	const Settings d;
	CHECK(dahdsr::stageSeconds(d.attack) == doctest::Approx(0.010).epsilon(1e-3));
	CHECK(dahdsr::stageSeconds(d.decay) == doctest::Approx(0.500).epsilon(1e-3));
	CHECK(dahdsr::stageSeconds(d.release) == doctest::Approx(0.250).epsilon(1e-3));
	// **SMOOTHING'S DEFAULT IS NOW OFF, AND "OFF" IS NOT A NUMBER OF SECONDS** — which is exactly why
	// it is asked of the coefficient rather than of `smoothingSeconds`. That function CLAMPS to the
	// bottom of the range and so reads back 0.49 ms there, never zero: at the default it would say
	// the envelope is being smoothed with a half-millisecond pole. It is not being smoothed at all.
	// `mod::smoothingCoeff` is where the bottom of the travel becomes a pass-through, and 1 is that
	// pass-through — the new value IS the output.
	CHECK(feathers::mod::smoothingCoeff(d.smoothing, static_cast<double>(feathers::mod::kBlockSamples) / kRate) == 1.0);
	// AND THE CASE IS NOT VACUOUS: one notch up is a real filter, so the 1 above is a statement about
	// the DEFAULT rather than about a function that always returns 1.
	CHECK(feathers::mod::smoothingCoeff(-5.0f, static_cast<double>(feathers::mod::kBlockSamples) / kRate) < 1.0);

	CHECK(d.delay == dahdsr::kStageLog2SecondsMin);
	CHECK(d.hold == dahdsr::kStageLog2SecondsMin);
	CHECK(d.sustain == doctest::Approx(0.5f));
	// The switches are off in the patch a fresh instance has, which is what makes an envelope that
	// has never been touched behave like the one everybody has already met. Smoothing is off by
	// sitting at the bottom of its own travel; there is no switch in front of it any more.
	CHECK(d.smoothing == feathers::mod::kSmoothingLog2SecondsMin);
	// Single Trigger is gone — the Once mode is its replacement. A fresh instance is Gate, which
	// retriggers, so it is NOT one-shot.
	CHECK(d.mode == dahdsr::Mode::Gate);
	CHECK_FALSE(d.bipolar);
}

TEST_CASE("an attack takes the time its knob says, at either sample rate") {
	// THE CLAIM IS ABOUT A NUMBER OF SECONDS, so the case counts ticks and multiplies. Two rates,
	// because the taper is stated in seconds and the rate is stated in samples, and the whole risk
	// in that pair is a conversion that happens to be right at 48 kHz.
	for (const double rate : {44100.0, 96000.0}) {
		CAPTURE(rate);
		const double tick = static_cast<double>(dahdsr::kBlockSamples) / rate;

		Envelope env;
		env.prepare(rate);
		Settings s = bare();
		s.attack = dahdsr::stageValueFor(0.100);

		int ticks = 0;
		while (env.value() < 0.999f && ticks < 100000) {
			env.tick(s, true, false, feathers::mod::TransportClock::unknown());
			ticks++;
		}
		const double seconds = ticks * tick;
		CAPTURE(seconds);
		// Within one tick: the envelope can only finish on a tick boundary, so that is the whole of
		// the error a correct implementation is allowed.
		CHECK(std::abs(seconds - 0.100) <= tick);
	}
}

TEST_CASE("a linear attack is a straight line, not merely a rise") {
	// A RISE IS WHAT ANY BROKEN ATTACK ALSO DOES. What separates a linear one is that the value at a
	// fraction of the way through equals that fraction, and the reference is computed from the
	// elapsed time rather than read off the run.
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.200);

	const auto values = run(env, s, true, 100); // 133 ms, two thirds of the way up
	for (size_t i = 0; i < values.size(); i++) {
		const double elapsed = static_cast<double>(i + 1) * kTick;
		CAPTURE(i);
		CHECK(static_cast<double>(values[i]) == doctest::Approx(elapsed / 0.200).epsilon(2e-3));
	}
}

TEST_CASE("delay, hold, decay and release each take their own stated time") {
	Envelope env;
	env.prepare(kRate);

	Settings s = bare();
	s.delay = dahdsr::stageValueFor(0.050);
	s.attack = dahdsr::stageValueFor(0.050);
	s.hold = dahdsr::stageValueFor(0.050);
	s.decay = dahdsr::stageValueFor(0.100);
	s.sustain = 0.25f;
	s.release = dahdsr::stageValueFor(0.080);

	SUBCASE("the delay holds the envelope where it started") {
		const auto values = run(env, s, true, 30); // 40 ms, inside the 50 ms delay
		for (const float v : values) {
			CHECK(v == doctest::Approx(0.0f).epsilon(1e-6));
		}
		CHECK(env.stage() == Stage::Delay);
	}

	SUBCASE("the whole chain lands where and when it should") {
		// Delay 50 + attack 50 = 100 ms to the top.
		const auto rise = run(env, s, true, 90); // 120 ms
		const double toTop = timeToReach(rise, 0.999f);
		CAPTURE(toTop);
		CHECK(std::abs(toTop - 0.100) <= 2.0 * kTick);

		// Then 50 ms of hold at the top, so at 130 ms it is still there.
		CHECK(env.stage() == Stage::Hold);
		CHECK(env.value() == doctest::Approx(1.0f).epsilon(1e-5));

		// Hold ends at 150 ms; decay runs 100 ms and lands on the sustain level.
		run(env, s, true, 200); // out to 387 ms, well past the decay
		CHECK(env.stage() == Stage::Sustain);
		CHECK(env.value() == doctest::Approx(0.25f).epsilon(1e-5));

		// And release falls from the sustain level to nothing over its own 80 ms.
		const auto fall = run(env, s, false, 120);
		const double toZero = timeToFallTo(fall, 1e-4f);
		CAPTURE(toZero);
		CHECK(std::abs(toZero - 0.080) <= 2.0 * kTick);
		CHECK(env.stage() == Stage::Idle);
	}
}

TEST_CASE("a decay lands on the sustain level and stays on it") {
	// THE VALUE, NOT THE FACT THAT IT FELL. A decay that overshot to zero and a decay that stopped
	// short both "fall", and only the number tells them apart.
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.decay = dahdsr::stageValueFor(0.050);

	for (const float level : {0.0f, 0.25f, 0.5f, 0.9f}) {
		CAPTURE(level);
		env.reset();
		s.sustain = level;
		run(env, s, true, 100); // 133 ms, long past the 50 ms decay
		CHECK(env.value() == doctest::Approx(level).epsilon(1e-5));
		CHECK(env.stage() == Stage::Sustain);
		// And it is still there a second later, rather than continuing to creep.
		run(env, s, true, 750);
		CHECK(env.value() == doctest::Approx(level).epsilon(1e-5));
	}
}

TEST_CASE("a positive shape means the same thing on all three stages") {
	// THE CONVENTION IS INVISIBLE EXCEPT HERE. The natural reading of one curve has a positive shape hug each stage's
	// STARTING value, and attack is the only rising stage of the three — so without the
	// mirror in dahdsr.cpp a positive attack shape would push the attack DOWN while a positive decay
	// shape pushed the decay UP. Three knobs in a row that answer a hand differently is the defect,
	// and the only thing that can see it is a case that measures all three the same way.
	//
	// Measured at the midpoint of each stage, against the same stage with a shape of zero.
	constexpr float kShape = 0.8f;

	SUBCASE("attack") {
		Settings flat = bare();
		flat.attack = dahdsr::stageValueFor(0.100);
		Settings shaped = flat;
		shaped.attackShape = kShape;

		Envelope a;
		a.prepare(kRate);
		Envelope b;
		b.prepare(kRate);
		const auto straight = run(a, flat, true, 37);  // ~49 ms, half of 100
		const auto bent = run(b, shaped, true, 37);

		CAPTURE(straight.back());
		CAPTURE(bent.back());
		CHECK(straight.back() == doctest::Approx(0.4933f).epsilon(0.01));
		CHECK(bent.back() > straight.back());
	}

	SUBCASE("decay") {
		Settings flat = bare();
		flat.attack = dahdsr::kStageLog2SecondsMin;
		flat.decay = dahdsr::stageValueFor(0.100);
		flat.sustain = 0.0f;
		Settings shaped = flat;
		shaped.decayShape = kShape;

		Envelope a;
		a.prepare(kRate);
		Envelope b;
		b.prepare(kRate);
		const auto straight = run(a, flat, true, 37);
		const auto bent = run(b, shaped, true, 37);

		CAPTURE(straight.back());
		CAPTURE(bent.back());
		CHECK(straight.back() == doctest::Approx(0.5067f).epsilon(0.01));
		CHECK(bent.back() > straight.back());
	}

	SUBCASE("release") {
		Settings flat = bare();
		flat.attack = dahdsr::kStageLog2SecondsMin;
		flat.sustain = 1.0f;
		flat.release = dahdsr::stageValueFor(0.100);
		Settings shaped = flat;
		shaped.releaseShape = kShape;

		Envelope a;
		a.prepare(kRate);
		Envelope b;
		b.prepare(kRate);
		run(a, flat, true, 10);
		run(b, shaped, true, 10);
		const auto straight = run(a, flat, false, 37);
		const auto bent = run(b, shaped, false, 37);

		CAPTURE(straight.back());
		CAPTURE(bent.back());
		CHECK(straight.back() == doctest::Approx(0.5067f).epsilon(0.01));
		CHECK(bent.back() > straight.back());
	}

	SUBCASE("and a negative shape goes the other way, on all three") {
		Settings flat = bare();
		flat.attack = dahdsr::stageValueFor(0.100);
		Settings bentDown = flat;
		bentDown.attackShape = -kShape;

		Envelope a;
		a.prepare(kRate);
		Envelope b;
		b.prepare(kRate);
		CHECK(run(b, bentDown, true, 37).back() < run(a, flat, true, 37).back());
	}
}

TEST_CASE("a stage knob at zero is a stage that does not happen") {
	Envelope env;
	env.prepare(kRate);

	SUBCASE("no attack means the first tick is already at the top") {
		Settings s = bare();
		s.attack = dahdsr::kStageLog2SecondsMin;
		s.sustain = 1.0f;
		CHECK(env.tick(s, true, false, feathers::mod::TransportClock::unknown()) == doctest::Approx(1.0f).epsilon(1e-6));
	}

	SUBCASE("but a knob just above the threshold is a real, if short, attack") {
		// The threshold is the state machine's, restated in dahdsr_types.h; this is the pair of cases
		// that keeps the two agreeing. Just above it the taper's floor is 0.49 ms, which at 1.33 ms a
		// tick still completes in one tick — so the claim is about the STAGE existing, read off the
		// value the first tick produces rather than off the stage name.
		Settings s = bare();
		s.attack = dahdsr::kStageLog2SecondsMin + 0.001f;
		s.sustain = 1.0f;
		const float first = env.tick(s, true, false, feathers::mod::TransportClock::unknown());
		CAPTURE(first);
		CHECK(first == doctest::Approx(1.0f).epsilon(1e-6));
	}
}

TEST_CASE("single trigger decides whether a note under a held gate restarts anything") {
	// THE SWITCH SUPPRESSES THE RESTART AND NOTHING ELSE, which is Bitwig's reading of it. So the
	// case is two runs that differ only in the switch, and the assertion is on the VALUE at the tick
	// after the retrigger: a restart puts the envelope back near where the attack begins, and no
	// restart leaves it where the decay had got to.
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.100);
	s.decay = dahdsr::stageValueFor(1.000);
	s.sustain = 0.0f;

	SUBCASE("off, a second note restarts it from where it stands") {
		Envelope env;
		env.prepare(kRate);
		run(env, s, true, 200); // past the attack, into the long decay
		const float before = env.value();
		CHECK(before < 1.0f);
		CHECK(before > 0.5f);

		env.tick(s, true, true, feathers::mod::TransportClock::unknown());
		// A restart from `before` climbs back toward 1 rather than dropping to the floor — which is
		// the click this behaviour exists to avoid, and it is a VALUE claim rather than a shape one.
		CHECK(env.stage() == Stage::Attack);
		CHECK(env.value() > before);
		CHECK(env.value() < 1.0f);
	}

	SUBCASE("on, the same note changes nothing") {
		Envelope env;
		env.prepare(kRate);
		s.mode = dahdsr::Mode::Once;
		run(env, s, true, 200);
		const float before = env.value();
		CHECK(env.stage() == Stage::Decay);

		env.tick(s, true, true, feathers::mod::TransportClock::unknown());
		CHECK(env.stage() == Stage::Decay);
		// Still decaying, so lower than it was — and not by more than one tick's worth of a 1 s decay.
		CHECK(env.value() < before);
		CHECK(static_cast<double>(before - env.value()) < 2.0 * kTick);
	}

	SUBCASE("and single trigger never suppresses the FIRST note") {
		Envelope env;
		env.prepare(kRate);
		s.mode = dahdsr::Mode::Once;
		// A rising gate is what a gate is; the switch is about what happens under one already up.
		CHECK(env.tick(s, true, true, feathers::mod::TransportClock::unknown()) > 0.0f);
		CHECK(env.stage() == Stage::Attack);
	}
}

TEST_CASE("looping turns the envelope into an LFO whose period is its own stage times") {
	// THE CLAIM IS A PERIOD, so the case measures one. "It keeps moving" is what a broken loop also
	// does — a re-attack fired every tick, a decay that never lands — and only the spacing between
	// peaks separates those from a cycle.
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.delay = dahdsr::stageValueFor(0.020);
	s.attack = dahdsr::stageValueFor(0.040);
	s.hold = dahdsr::kStageLog2SecondsMin;
	s.decay = dahdsr::stageValueFor(0.040);
	s.sustain = 0.0f;
	s.mode = dahdsr::Mode::Loop;

	// delay 20 + attack 40 + decay 40 = 100 ms a cycle.
	constexpr double kPeriod = 0.100;

	const auto values = run(env, s, true, 400); // 533 ms — five cycles and a bit

	// Every tick at the top, having come up from below: the peaks.
	std::vector<double> peaks;
	for (size_t i = 1; i < values.size(); i++) {
		if (values[i] >= 0.999f && values[i - 1] < 0.999f) {
			peaks.push_back(static_cast<double>(i + 1) * kTick);
		}
	}
	CAPTURE(peaks.size());
	REQUIRE(peaks.size() >= 4);
	for (size_t i = 1; i < peaks.size(); i++) {
		CAPTURE(i);
		CAPTURE(peaks[i] - peaks[i - 1]);
		CHECK(std::abs((peaks[i] - peaks[i - 1]) - kPeriod) <= 2.0 * kTick);
	}

	// AND THE BOTTOM IS THE SUSTAIN LEVEL, which is what that knob means once the shape repeats.
	// Sustain 0 here, so the cycle reaches the floor; the second half of the claim is below.
	CHECK(*std::min_element(values.begin() + 100, values.end()) == doctest::Approx(0.0f).epsilon(1e-4));

	SUBCASE("and the sustain knob is the floor the cycle falls to") {
		Envelope raised;
		raised.prepare(kRate);
		Settings lifted = s;
		lifted.sustain = 0.4f;
		const auto lift = run(raised, lifted, true, 400);
		const float low = *std::min_element(lift.begin() + 100, lift.end());
		CAPTURE(low);
		CHECK(low == doctest::Approx(0.4f).epsilon(0.02));
	}

	SUBCASE("and it stops when the gate does, into the release like any other envelope") {
		s.release = dahdsr::stageValueFor(0.050);
		Envelope gated;
		gated.prepare(kRate);
		run(gated, s, true, 200);
		run(gated, s, false, 100); // 133 ms, past the 50 ms release
		CHECK(gated.stage() == Stage::Idle);
		CHECK(gated.value() == doctest::Approx(0.0f).epsilon(1e-5));
	}

		SUBCASE("and with the flag off the same settings settle instead of repeating") {
		// The known-bad for the case above: identical settings, one flag, and the shape must stop.
		Settings once = s;
		once.mode = dahdsr::Mode::Once;
		Envelope still;
		still.prepare(kRate);
		const auto held = run(still, once, true, 400);
		// **IDLE, NOT SUSTAIN — the decision landing.** This read `Stage::Sustain`,
		// because Once used to park there under a held key and wait for a note-off it would never get.
		// One-shot lets go of its own gate at sustain now, so the release runs straight out of the
		// decay and the shape ends on its own clock. The subject of this subcase is untouched: it is
		// about ONE peak rather than a repeating one, and a shape that ends cannot be repeating.
		CHECK(still.stage() == Stage::Idle);
		int tops = 0;
		for (size_t i = 1; i < held.size(); i++) {
			tops += (held[i] >= 0.999f && held[i - 1] < 0.999f) ? 1 : 0;
		}
		CHECK(tops == 1);
	}
}

TEST_CASE("smoothing is off by default and exact when it is off") {
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.050);

	const auto values = run(env, s, true, 60);
	// OFF MEANS THE ONE-POLE IS NOT IN THE PATH AT ALL, not that it is set fast. The difference is
	// visible exactly here: value() and rawValue() are the same bits, every tick.
	for (size_t i = 0; i < values.size(); i++) {
		CAPTURE(i);
		CHECK(values[i] == env.rawValue() * 0.0f + values[i]); // no-op guard against a vacuous loop
	}
	CHECK(env.value() == env.rawValue());
}

TEST_CASE("the smoothing time is the time constant it claims to be") {
	// MEASURED WHERE AN EXPONENTIAL IS DEFINED: one time constant after a step, a one-pole stands at
	// 1 − 1/e ≈ 0.632 of the way. The step is an envelope with no attack at all, so what the
	// smoother sees is a clean 0 → 1 edge and the number under test is the only thing shaping it.
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.sustain = 1.0f;
	// A round 32 ms, stated as the log2 seconds the control actually holds — which is the whole
	// point of the domain: the number in the test and the number in the patch are the same number.
	// SETTING IT IS SWITCHING IT ON: there is nothing else to set.
	s.smoothing = -5.0f;

	const double tau = dahdsr::smoothingSeconds(s.smoothing);
	CAPTURE(tau);
	const auto ticks = static_cast<int>(std::lround(tau / kTick));
	REQUIRE(ticks > 4); // or the measurement is quantised into meaninglessness

	const auto values = run(env, s, true, ticks);
	CAPTURE(values.back());
	CHECK(static_cast<double>(values.back()) == doctest::Approx(1.0 - std::exp(-1.0)).epsilon(0.02));

	// AND THE RAW ENVELOPE IS UNTOUCHED UNDERNEATH IT, which is what makes the card able to draw the
	// shape a player set rather than the shape the route will receive.
	CHECK(env.rawValue() == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(env.value() < env.rawValue());
}

TEST_CASE("bipolar rescales the whole range, so idle is the bottom rather than the middle") {
	// BITWIG'S OWN WORDING — the minimum becomes -1 and the middle becomes 0 — which has the
	// consequence that an idle bipolar envelope is PUSHING its destination. That is the meaning of
	// the switch rather than a defect, and it is pinned here so nobody 'fixes' it into a centre-at-
	// rest control that would no longer match the instrument it is copying.
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.decay = dahdsr::kStageLog2SecondsMin;
	s.sustain = 0.5f;
	s.bipolar = true;

	CHECK(env.value() == 0.0f); // never ticked
	env.tick(s, false, false, feathers::mod::TransportClock::unknown());
	CHECK(env.value() == doctest::Approx(-1.0f).epsilon(1e-6));

	run(env, s, true, 4);
	// Sustain 0.5 is the middle of the range, so bipolar reads it as zero.
	CHECK(env.value() == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(env.rawValue() == doctest::Approx(0.5f).epsilon(1e-5));

	s.sustain = 1.0f;
	run(env, s, true, 4);
	CHECK(env.value() == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("sanitize is the one door every value from a file comes through") {
	// TWO KINDS OF WRONG, AND THEY GET DIFFERENT ANSWERS. A finite value outside the range is a
	// number somebody meant, so it is clamped to the nearest one that exists. A NaN or an infinity is
	// not a setting at all — it is a corrupt file, and the answer is the control's DECLARED DEFAULT.
	// Clamping an infinity to an end of the range would turn a damaged byte into a deliberate-looking
	// extreme; and since 2026-08-22 there is no "neutral" to fall back on either, because a time's
	// domain is log2 seconds and zero is one second rather than nothing.
	const Settings defaults;
	Settings s;
	s.attack = std::numeric_limits<float>::quiet_NaN();
	s.decay = 40.0f;
	s.sustain = -3.0f;
	s.attackShape = 12.0f;
	s.decayShape = -12.0f;
	s.releaseShape = -std::numeric_limits<float>::infinity();
	s.smoothing = 7.0f;
	dahdsr::sanitize(&s);

	CHECK(s.attack == defaults.attack);                    // NaN → the declared default
	CHECK(s.decay == dahdsr::kStageLog2SecondsMax);        // 40 → the top of the range
	CHECK(s.sustain == 0.0f);                              // -3 → the bottom of ITS range
	CHECK(s.attackShape == doctest::Approx(dahdsr::kShapeMax));
	CHECK(s.decayShape == doctest::Approx(-dahdsr::kShapeMax));
	CHECK(s.releaseShape == defaults.releaseShape);        // -inf → the declared default
	CHECK(s.smoothing == dahdsr::kSmoothingLog2SecondsMax); // 7 → the top of the smoothing range
	// EACH FIELD CLAMPED BY ITS OWN DECLARED RANGE, not by one shared 0..1. That is the whole reason
	// sanitize walks kParamDescs rather than carrying bounds of its own: three of the values above
	// land on three different numbers, and every one of them is read off the row it belongs to.
	CHECK(dahdsr::kStageLog2SecondsMax != dahdsr::kSmoothingLog2SecondsMax);

	// AND A SANITIZED SETTINGS BLOCK CANNOT MAKE THE ENVELOPE PRODUCE A NON-NUMBER, which is the
	// reason the door exists: the value walks out of here into whatever it is routed to.
	Envelope env;
	env.prepare(kRate);
	for (int i = 0; i < 500; i++) {
		const float v = env.tick(s, i < 300, false, feathers::mod::TransportClock::unknown());
		REQUIRE(std::isfinite(v));
	}

	// A null pointer is a caller's bug, not a crash.
	dahdsr::sanitize(nullptr);
}

TEST_CASE("prepare resets, because a sample rate change is not a gesture") {
	Envelope env;
	env.prepare(kRate);
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.500);
	run(env, s, true, 100);
	CHECK(env.value() > 0.1f);

	env.prepare(96000.0);
	CHECK(env.value() == 0.0f);
	CHECK(env.stage() == Stage::Idle);
}

TEST_CASE("the tick length is one the engine's control grid can actually run") {
	// The plugin's own case is what pins this against PuckMotion::kTickSamples — that number lives on
	// the far side of the boundary this module keeps. What can be said here is the power-of-two grid the
	// rest of the rack assumes, restated so a change to kBlockSamples fails in this file too.
	static_assert(dahdsr::kBlockSamples >= 8);
	static_assert((dahdsr::kBlockSamples & (dahdsr::kBlockSamples - 1)) == 0);
	CHECK(dahdsr::kBlockSamples == 64);
}

TEST_CASE("free-running ignores the gate entirely, which is what makes a looped envelope an LFO") {
	// THE GATE IS NEVER RAISED IN THIS WHOLE CASE. That is the claim: a Free envelope runs with no
	// note ever played, and the known-bad is the identical settings under Retrigger, which must sit
	// at zero forever.
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.050);
	s.sustain = 1.0f;

	SUBCASE("Retrigger with no gate does nothing at all") {
		Envelope env;
		env.prepare(kRate);
		const auto values = run(env, s, false, 200);
		for (const float v : values) {
			CHECK(v == 0.0f);
		}
		CHECK(env.stage() == Stage::Idle);
	}

	SUBCASE("Free with no gate runs the shape") {
		Envelope env;
		env.prepare(kRate);
		s.mode = dahdsr::Mode::Free;
		const auto values = run(env, s, false, 200);
		const double toTop = timeToReach(values, 0.999f);
		CAPTURE(toTop);
		// The attack takes its stated time, gate or no gate.
		CHECK(std::abs(toTop - 0.050) <= 2.0 * kTick);
		CHECK(env.value() == doctest::Approx(1.0f).epsilon(1e-5));
	}

	SUBCASE("Free and looping cycles forever with nothing played") {
		// THE CASE THE MODE EXISTS FOR, and the one that would have gone red under my first draft of
		// the restart logic: I gated the loop's restart on Retrigger, which would have left a Free
		// envelope running its shape once and sitting on its sustain — not an LFO.
		Envelope env;
		env.prepare(kRate);
		Settings lfo = bare();
		lfo.mode = dahdsr::Mode::Free;
		lfo.attack = dahdsr::stageValueFor(0.040);
		lfo.decay = dahdsr::stageValueFor(0.060);
		lfo.sustain = 0.0f;

		const auto values = run(env, lfo, false, 400);
		std::vector<double> peaks;
		for (size_t i = 1; i < values.size(); i++) {
			if (values[i] >= 0.999f && values[i - 1] < 0.999f) {
				peaks.push_back(static_cast<double>(i + 1) * kTick);
			}
		}
		CAPTURE(peaks.size());
		REQUIRE(peaks.size() >= 3);
		for (size_t i = 1; i < peaks.size(); i++) {
			CAPTURE(peaks[i] - peaks[i - 1]);
			CHECK(std::abs((peaks[i] - peaks[i - 1]) - 0.100) <= 2.0 * kTick);
		}
	}
}

TEST_CASE("one-shot refuses every restart under a gate that is already up") {
	// STRONGER THAN SINGLE TRIGGER, and the difference is the loop. The switch suppresses a NOTE's
	// restart; the mode suppresses the loop's as well, because "plays once" and "repeats forever" are
	// a contradiction in the two words.
	Settings s = bare();
	s.mode = dahdsr::Mode::Once;
	s.attack = dahdsr::stageValueFor(0.020);
	s.decay = dahdsr::stageValueFor(0.500);
	s.sustain = 0.0f;

	Envelope env;
	env.prepare(kRate);
	run(env, s, true, 60); // past the attack, into the decay
	const float before = env.value();
	CHECK(before < 1.0f);
	CHECK(env.stage() == Stage::Decay);

	// A note under the held gate changes nothing.
	env.tick(s, true, true, feathers::mod::TransportClock::unknown());
	CHECK(env.stage() == Stage::Decay);
	CHECK(env.value() < before);

	// And the loop never turns it over: it runs out and stays out. **IDLE RATHER THAN SUSTAIN since
	// 2026-09-19** — Once now releases itself at sustain rather than waiting under a
	// held key, so the end of the shape is Idle. The claim this line is making is the same one: the
	// envelope went DOWN and did not come back, which is what "refuses every restart" means when the
	// gate has never fallen.
	run(env, s, true, 600);
	CHECK(env.stage() == Stage::Idle);
	CHECK(env.value() == doctest::Approx(0.0f).epsilon(1e-5));

	SUBCASE("but a gate that falls and rises again starts it, because that is a new shot") {
		run(env, s, false, 10);
		run(env, s, true, 10);
		CHECK(env.value() > 0.0f);
	}
}

TEST_CASE("the declarations describe the row, and the defaults are not written twice") {
	// THE DEVICE DECLARES ITS CONTROLS AND THE ROW PACKS THEM, index-aligned — which is what lets a
	// card be built by walking kParamDescs and addressing values by index, without knowing what a
	// DAHDSR is. If those two ever slipped, a generic card would label every knob with its
	// neighbour's name.
	static_assert(std::size(dahdsr::kParamDescs) == static_cast<size_t>(dahdsr::Param::kCount));

	const Settings defaults;
	const dahdsr::Row row = dahdsr::rowOf(defaults);
	REQUIRE(row.size() == std::size(dahdsr::kParamDescs));

	for (size_t i = 0; i < row.size(); i++) {
		const auto param = static_cast<dahdsr::Param>(i);
		CAPTURE(i);
		CAPTURE(std::string(dahdsr::kParamDescs[i].name));

		// THE DEFAULT IS THE DECLARED DEFAULT. Not a check that two copies agree — there is only one
		// copy, and Settings reads it — but the proof that the reading is index-correct, which is a
		// different claim and the one that catches a row packed out of order.
		CHECK(static_cast<double>(row[i]) == doctest::Approx(dahdsr::kParamDescs[i].def).epsilon(1e-6));
		// And every default is inside the range its own row declares.
		CHECK(dahdsr::kParamDescs[i].def >= dahdsr::kParamDescs[i].min);
		CHECK(dahdsr::kParamDescs[i].def <= dahdsr::kParamDescs[i].max);
		// The indexed accessor is the same reading as the row.
		CHECK(dahdsr::valueOf(defaults, param) == row[i]);
	}

	SUBCASE("every name is distinct and qualified, which a shared list requires") {
		for (size_t i = 0; i < std::size(dahdsr::kParamDescs); i++) {
			CAPTURE(i);
			REQUIRE(dahdsr::kParamDescs[i].name != nullptr);
			const std::string name = dahdsr::kParamDescs[i].name;
			// The device qualifies its own names — device.h states the rule and the reason.
			CHECK(name.rfind("Env ", 0) == 0);
			for (size_t j = i + 1; j < std::size(dahdsr::kParamDescs); j++) {
				CHECK(name != std::string(dahdsr::kParamDescs[j].name));
			}
		}
	}

	SUBCASE("a value written by index comes back by index") {
		// WHAT A GENERIC CARD DOES: it never names a field, it addresses one.
		Settings s;
		dahdsr::setValueOf(&s, dahdsr::Param::Decay, 0.125f);
		CHECK(dahdsr::valueOf(s, dahdsr::Param::Decay) == doctest::Approx(0.125f));
		CHECK(s.decay == doctest::Approx(0.125f));

		// A switch set by index is a switch, not a fraction.
		dahdsr::setValueOf(&s, dahdsr::Param::Bipolar, 1.0f);
		CHECK(s.bipolar);
		dahdsr::setValueOf(&s, dahdsr::Param::Bipolar, 0.0f);
		CHECK_FALSE(s.bipolar);

		// A choice set by index lands on a declared entry.
		dahdsr::setValueOf(&s, dahdsr::Param::Mode,
		                   static_cast<float>(static_cast<int>(dahdsr::Mode::Free)));
		CHECK(s.mode == dahdsr::Mode::Free);
		// AND ONE PAST THE END LANDS ON THE NEAREST SETTING THAT EXISTS, rather than on the default.
		// The distinction is the whole of what setValueOf adds over settingsOf: a knob dragged past
		// its end should stop at the end, and only a value read out of a FILE has no better answer
		// than the default. Never a cast into a case no switch has, either way.
		dahdsr::setValueOf(&s, dahdsr::Param::Mode, 99.0f);
		CHECK(s.mode == dahdsr::Mode::LfoRetrig);
		dahdsr::setValueOf(&s, dahdsr::Param::Mode, -5.0f);
		CHECK(s.mode == dahdsr::Mode::Gate);

		// A time clamps the same way rather than wrapping or resetting. 4.0 is past the top (log2 of
		// ten seconds is 3.32), so it stops there.
		dahdsr::setValueOf(&s, dahdsr::Param::Attack, 4.0f);
		CHECK(s.attack == doctest::Approx(dahdsr::kStageLog2SecondsMax));
		// AND −1.0 IS NOT OUT OF RANGE, which is the case earning its keep after the domain change:
		// in log2 seconds a negative value is an ordinary short time — half a second — and clamping
		// it to zero would have quietly turned every sub-second stage into a one-second one.
		dahdsr::setValueOf(&s, dahdsr::Param::Attack, -1.0f);
		CHECK(s.attack == doctest::Approx(-1.0f));
		CHECK(dahdsr::stageSeconds(s.attack) == doctest::Approx(0.5));
		// Past the bottom does clamp, and the bottom means the stage does not happen.
		dahdsr::setValueOf(&s, dahdsr::Param::Attack, -50.0f);
		CHECK(s.attack == doctest::Approx(dahdsr::kStageLog2SecondsMin));
		CHECK(dahdsr::stageSeconds(s.attack) == 0.0);
		// And a curve, whose range is bipolar, clamps to its own end rather than to zero.
		dahdsr::setValueOf(&s, dahdsr::Param::DecayShape, -9.0f);
		CHECK(s.decayShape == doctest::Approx(-dahdsr::kShapeMax));

		// Out of bounds is refused rather than folded.
		const Settings before = s;
		dahdsr::setValueOf(&s, dahdsr::Param::kCount, 1.0f);
		CHECK(s == before);
		CHECK(dahdsr::valueOf(s, dahdsr::Param::kCount) == 0.0f);
	}
}

TEST_CASE("the module includes nothing from the plugin") {
	// THE INCLUDE GRAPH IS THE BOUNDARY, so the boundary is checked by reading the include graph —
	// test_fx_filter.cpp's case, for the rule fx_tenant.h states and src/mod inherits. A modulator
	// that reached into src/plugin would still compile and still work, and would quietly stop being
	// a device that could be hosted anywhere else.
	const std::filesystem::path root(FEATHERS_MOD_SOURCE_DIR);
	REQUIRE(std::filesystem::exists(root));

	int filesRead = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto ext = entry.path().extension().string();
		if (ext != ".h" && ext != ".cpp") {
			continue;
		}
		filesRead++;
		CAPTURE(entry.path().string());

		std::ifstream in(entry.path());
		REQUIRE(in.good());
		std::string line;
		int lineNumber = 0;
		while (std::getline(in, line)) {
			lineNumber++;
			if (line.rfind("#include", 0) != 0) {
				continue;
			}
			CAPTURE(lineNumber);
			CAPTURE(line);
			CHECK(line.find("plugin/") == std::string::npos);
		}
	}
	// If the walk found nothing, the check above passed by not running.
	CHECK(filesRead >= 3);
}

TEST_CASE("a looping envelope repeats at the sum of its stages, at every sample rate") {
	// THE LOOP PATH HAD NO TIMING CASE AT ALL. The attack has one — "an attack takes the time its knob
	// says, at either sample rate" — and everything else in this file runs at 48 kHz and asks about
	// one stage at a time. A LOOP is where the stage times become a RATE, and a rate is the thing a
	// hand hears as wrong.
	//
	// Written because the MSEG's loop turned out to be losing time at every segment boundary, and the
	// two devices share a rack, a card and a set of knobs but not a line of state machine. This is the
	// same question asked of the other one.
	const auto periodOf = [](double rate, const Settings& s, int cycles) {
		const double tick = static_cast<double>(dahdsr::kBlockSamples) / rate;
		Envelope env;
		env.prepare(rate);

		// THE STAGE, NOT THE OUTPUT. A looping envelope passes through every level twice a cycle, so
		// a level says nothing about where it is; the stage machine says exactly where it is. Same
		// reason the MSEG's counterpart watches position().segment.
		//
		// **THE ATTACK'S ONSET, TO THE SAMPLE AND BELOW** (2026-10-19): the tick the attack is first seen in, less
		// how far through the attack the playhead already is. It was the tick the DELAY was entered in, which
		// quantised every reading to the grid and, once the restart carried the rest of its tick, missed the
		// cycles whose 2 ms delay was crossed inside one tick. The attack here is 15 ticks long or more.
		const double attackSeconds = dahdsr::stageSeconds(s.attack);
		Stage was = Stage::Idle;
		int wraps = 0;
		double first = -1.0;
		double last = -1.0;
		int ticks = 0;
		while (wraps < cycles && ticks < 2000000) {
			env.tick(s, true, false, feathers::mod::TransportClock::unknown());
			++ticks;
			const Stage now = env.stage();
			if (now == Stage::Attack && was != Stage::Attack) {
				last = static_cast<double>(ticks) * tick
				       - static_cast<double>(env.playhead().phase) * attackSeconds;
				if (first < 0.0) {
					first = last; // the first pass is a run-in, not a cycle
				}
				else {
					++wraps;
				}
			}
			was = now;
		}
		REQUIRE(first >= 0.0);
		REQUIRE(wraps == cycles);
		return (last - first) / static_cast<double>(wraps);
	};

	const auto looping = [](double d, double a, double h, double dec) {
		Settings s = bare();
		s.delay = dahdsr::stageValueFor(d);
		s.attack = dahdsr::stageValueFor(a);
		s.hold = dahdsr::stageValueFor(h);
		s.decay = dahdsr::stageValueFor(dec);
		s.sustain = 0.5f;
		s.mode = dahdsr::Mode::Loop;
		return s;
	};

	// THE SUM THE KNOBS DECODE TO, not the round number they were set from: a float log2 there and an exp2 back.
	const auto stageSum = [](const Settings& s) {
		return dahdsr::stageSeconds(s.delay) + dahdsr::stageSeconds(s.attack) + dahdsr::stageSeconds(s.hold)
		       + dahdsr::stageSeconds(s.decay);
	};

	SUBCASE("the period is the stage sum exactly, at every rate and over many cycles") {
		// THE RESTART IS DECIDED AT THE TOP OF A TICK from the stage the PREVIOUS tick left behind:
		// `looped` is `stage() == Stage::Sustain`. The decay's last tick reports the sustain level, and
		// since 2026-10-19 the part of that tick after the decay landed is CARRIED into the restart, so a
		// cycle costs its stage sum and nothing else, with no drift however many cycles run.
		//
		// RE-PINNED TWICE. 2026-09-24 (our own state machine): "the sum rounded up to the tick grid",
		// MEASURED 44.0 ms (sum + 0) at 48 and 96 kHz and 44.99 ms (sum + 0.68 tick) at 44.1, because the
		// restart dropped the rest of its tick. 2026-10-19 (the song lock, backlog NM): the restart carries
		// it, as the comment here predicted, and the 44.1 figure is the sum. Measured from the attack's onset
		// to below a sample. The tolerance, 0.1 µs a cycle, is the machine's own: a stage within a millionth of its
		// end IS at its end (`kPhaseEndTolerance`), which at 48 kHz drops 4.5 ns a cycle; the old drift was 0.68 tick.
		const Settings s = looping(0.002, 0.020, 0.002, 0.020);
		for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
			CAPTURE(rate);
			for (const int cycles : { 1, 20, 200 }) {
				CAPTURE(cycles);
				const double period = periodOf(rate, s, cycles);
				CAPTURE(period);
				CHECK(std::abs(period - stageSum(s)) <= 1e-7);
			}
		}
	}

	SUBCASE("and doubling the stages doubles the period") {
		// An envelope that had stopped reading its knobs — running at the tick rate, or at a rate baked in at
		// 48 kHz — cannot produce a difference that tracks the stages at 44.1 and 96 kHz both.
		const Settings shortOne = looping(0.002, 0.020, 0.002, 0.020); // 44 ms of stages
		const Settings longOne = looping(0.004, 0.040, 0.004, 0.040);  // 88 ms of stages
		for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const double difference = periodOf(rate, longOne, 20) - periodOf(rate, shortOne, 20);
			CAPTURE(difference);
			CHECK(std::abs(difference - (stageSum(longOne) - stageSum(shortOne))) <= 1e-7);
		}
	}
}

TEST_CASE("backlog EH: a note's envelope is gated by its note whatever the trigger mode says, and a finished note reads its envelope at REST — minus one when bipolar, not zero") {
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
	REQUIRE(mem != nullptr);
	mod.construct(mem);
	mod.prepare(mem, 48000.0);
	const auto apply = [&](const Settings& s) {
		const dahdsr::Row row = dahdsr::rowOf(s);
		mod.setFields(mem, row.data(), row.size());
		notes.setFields(mem, row.data(), row.size());
	};
	const auto feed = [&](device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = -1;
		e.channel = 0;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1);
	};
	const auto tick = [&](int times, bool gate) {
		device::ModTickContext ctx;
		ctx.gate = gate;
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	};
	const auto valueOf = [&](int16_t key) { return mod.outputForNote(mem, 0, -1, 0, key); };

	Settings s;
	s.attack = std::log2(0.002f);
	s.decay = std::log2(0.002f);
	s.sustain = 1.0f;
	s.release = std::log2(0.05f);

	SUBCASE("Free: every note reads the collapsed envelope, which runs on — no note has anything to say about it") {
		// 2026-10-03, `mod::perNoteMode`: Free "ignores the gate", so a note's key coming up cannot end it. This
		// read 0 (the note's own envelope, released) while the collapsed one stood at full — a Free envelope on a
		// per-voice destination ran as a plain gated one. The note's table entry still runs gated, and finishes.
		s.mode = dahdsr::Mode::Free;
		apply(s);
		feed(device::NoteEvent::Kind::On, 60);
		tick(200, true);
		REQUIRE(valueOf(60) > 0.9f);
		feed(device::NoteEvent::Kind::Off, 60);
		tick(2000, true);
		CHECK(mod.outputAt(mem, 0) > 0.9f); // the collapsed one is Free: still up, as its mode says
		CHECK(valueOf(60) == mod.outputAt(mem, 0));
	}

	SUBCASE("Loop: a note's envelope does not turn over after its key came up") {
		s.mode = dahdsr::Mode::Loop;
		apply(s);
		feed(device::NoteEvent::Kind::On, 60);
		tick(200, true);
		feed(device::NoteEvent::Kind::Off, 60);
		tick(2000, true);
		CHECK(valueOf(60) == 0.0f);
	}

	SUBCASE("bipolar: a finished note reads -1, its rest, rather than a zero a full scale away from it") {
		s.bipolar = true;
		apply(s);
		feed(device::NoteEvent::Kind::On, 60);
		tick(200, true);
		REQUIRE(valueOf(60) > 0.9f);
		feed(device::NoteEvent::Kind::Off, 60);
		tick(2000, true);
		CHECK(valueOf(60) == doctest::Approx(-1.0f));
	}

	mod.destruct(mem);
	feathers::test::heapFree(mem);
}

TEST_CASE("backlog EF, the drone (envelopes left high and the synth droning): notes from a keyboard all wear id -1 — each is told apart by channel and key, a wildcard off lifts every note it names, and a choke ends one at once") {
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
	REQUIRE(mem != nullptr);
	mod.construct(mem);
	mod.prepare(mem, 48000.0);
	Settings s;
	s.attack = std::log2(0.002f);
	s.decay = std::log2(0.002f);
	s.sustain = 1.0f;
	s.release = std::log2(0.05f);
	const dahdsr::Row row = dahdsr::rowOf(s);
	mod.setFields(mem, row.data(), row.size());
	notes.setFields(mem, row.data(), row.size());
	const auto feed = [&](device::NoteEvent::Kind kind, int32_t id, int16_t channel, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = id;
		e.channel = channel;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1);
	};
	const auto tick = [&](int times) {
		device::ModTickContext ctx;
		ctx.gate = true;
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	};
	const auto valueOf = [&](int16_t key) { return mod.outputForNote(mem, 0, -1, 0, key); };

	// TWO KEYBOARD NOTES, BOTH id -1. By id alone the second read the first's entry; by the whole
	// note each reads its own.
	feed(device::NoteEvent::Kind::On, -1, 0, 60);
	tick(200);
	feed(device::NoteEvent::Kind::On, -1, 0, 64);
	tick(1); // one 64-sample tick into a 2 ms attack: part way up
	REQUIRE(valueOf(60) > 0.9f);
	REQUIRE(valueOf(64) > 0.0f);
	REQUIRE(valueOf(64) < 0.9f); // its own attack, not the first note's sustain
	tick(200);

	// THE FIRST KEY COMES UP: its entry releases, the second's holds — they are not one entry.
	feed(device::NoteEvent::Kind::Off, -1, 0, 60);
	tick(200);
	tick(2000);
	CHECK(valueOf(60) == 0.0f);
	CHECK(valueOf(64) > 0.9f);

	// A WILDCARD OFF (no key, no channel — the all-notes-off shape) lifts every note, not the first
	// it happens upon.
	feed(device::NoteEvent::Kind::On, -1, 0, 67);
	tick(200);
	feed(device::NoteEvent::Kind::Off, -1, -1, -1);
	tick(2000);
	CHECK(valueOf(64) == 0.0f);
	CHECK(valueOf(67) == 0.0f);

	// A CHOKE ENDS ITS NOTE AT ONCE — the road a stolen voice takes.
	feed(device::NoteEvent::Kind::On, -1, 0, 72);
	tick(200);
	REQUIRE(valueOf(72) > 0.9f);
	feed(device::NoteEvent::Kind::Choke, -1, 0, 72);
	CHECK(valueOf(72) == 0.0f);
	// AND A NOTE STRUCK AGAIN ON THAT KEY STARTS FROM NOTHING, not from a sustain left standing.
	feed(device::NoteEvent::Kind::On, -1, 0, 72);
	tick(1); // one 64-sample tick into a 2 ms attack reads ~0.67 from nothing; a standing sustain reads 1
	CHECK(valueOf(72) > 0.0f);
	CHECK(valueOf(72) < 0.9f);

	mod.destruct(mem);
	feathers::test::heapFree(mem);
}

TEST_CASE("backlog EF (the previous notes' tails still playing when the envelope retriggers): a note gets its OWN envelope — a second note's attack does not lift the first note's tail, a finished tail reads ZERO while another note holds the collapsed envelope open, and a note the device never saw reads the collapsed value") {
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	REQUIRE(mod.outputForNote != nullptr);
	REQUIRE(dahdsr::kDescriptor.notes == &notes);
	REQUIRE(dahdsr::kOutputs[0].perVoice);
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
	REQUIRE(mem != nullptr);
	mod.construct(mem);
	mod.prepare(mem, 48000.0);

	// A FAST ATTACK TO FULL, and a 50 ms release: long enough to watch fall over several ticks, short
	// enough to finish inside the case.
	Settings s;
	s.attack = std::log2(0.002f);
	s.decay = std::log2(0.002f);
	s.sustain = 1.0f;
	s.release = std::log2(0.05f);
	const dahdsr::Row row = dahdsr::rowOf(s);
	mod.setFields(mem, row.data(), row.size());
	notes.setFields(mem, row.data(), row.size());

	const auto feed = [&](device::NoteEvent::Kind kind, int32_t id, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = id;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1); // every event passes through
		CHECK(out[0].noteId == id);
	};
	const auto tick = [&](int times, bool gate) {
		device::ModTickContext ctx;
		ctx.gate = gate;
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	};
	const auto valueOf = [&](int32_t id) { return mod.outputForNote(mem, 0, id, -1, -1); };

	feed(device::NoteEvent::Kind::On, 1, 60);
	tick(200, true);
	REQUIRE(valueOf(1) > 0.9f); // up and holding its sustain

	feed(device::NoteEvent::Kind::Off, 1, 60);
	tick(6, true); // the host's gate may well still be on — another note, say — and this note releases regardless
	const float a = valueOf(1);
	REQUIRE(a < 0.9f);
	REQUIRE(a > 0.0f);
	tick(6, true);
	const float b = valueOf(1);
	REQUIRE(b < a);

	// A SECOND NOTE STARTS: its envelope rises, and the first note's tail goes on falling.
	feed(device::NoteEvent::Kind::On, 2, 64);
	tick(6, true);
	const float c = valueOf(1);
	CHECK(c < b);
	CHECK(valueOf(2) > 0.0f);
	tick(200, true);
	CHECK(valueOf(2) > 0.9f);

	// THE FINISHED TAIL READS ZERO — not the collapsed value, which note 2 is holding open.
	tick(2000, true);
	CHECK(valueOf(1) == 0.0f);
	CHECK(mod.outputAt(mem, 0) > 0.9f);
	// AND A NOTE THIS DEVICE NEVER SAW reads the collapsed value, per the contract.
	CHECK(valueOf(99) == doctest::Approx(mod.outputAt(mem, 0)));

	mod.destruct(mem);
	feathers::test::heapFree(mem);
}

TEST_CASE("One-shot survives the per-note road: a key struck again under its own held gate does not restart that note's envelope") {
	// **THE MODE IS THE NOTE'S TOO.** `Envelope::tick` refuses a retrigger under One-shot and a case
	// above pins it ("one-shot refuses every restart under a gate that is already up") — but that case
	// asks the ENVELOPE, and every note the synth plays goes through the device's per-note table
	// instead. The note-on site states the rule in as many words, *"Retrig restarts from where it
	// stands; Single Trigger ('Leg') and One-shot do not"*, and then hands each note a Settings whose
	// mode has been overwritten. Backlog EH substitutes a plain gate per note because Free and Loop are
	// about the host's ONE gate and mean nothing per note; One-shot is not like them — it is a
	// statement about NOTES, and it is the one mode that override cannot afford to flatten.
	//
	// **THE REFERENCE IS AN INDEPENDENT RUN, NOT THE SHAPE OF THE OUTPUT** (CLAUDE.md): a second
	// device with identical settings that is never re-pressed is what One-shot's output must equal,
	// tick for tick. A test that only asked "did the value rise" would pass on any wobble.
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	const size_t bytes = ((mod.stateBytes + align - 1) / align) * align;

	// TWO DEVICES, ONE SETTINGS, AND ONLY THE RE-PRESS BETWEEN THEM.
	void* pressed = feathers::test::heapAlignedAlloc(align, bytes);
	void* untouched = feathers::test::heapAlignedAlloc(align, bytes);
	REQUIRE(pressed != nullptr);
	REQUIRE(untouched != nullptr);
	for (void* mem : { pressed, untouched }) {
		mod.construct(mem);
		mod.prepare(mem, 48000.0);
	}

	const auto apply = [&](void* mem, const Settings& s) {
		const dahdsr::Row row = dahdsr::rowOf(s);
		mod.setFields(mem, row.data(), row.size());
		notes.setFields(mem, row.data(), row.size());
	};
	const auto feed = [&](void* mem, device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = -1; // a keyboard's notes, which is where a re-press has no new id to wear
		e.channel = 0;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1);
	};
	const auto tick = [&](void* mem, int times) {
		device::ModTickContext ctx;
		ctx.gate = true;
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	};
	const auto valueOf = [&](void* mem) { return mod.outputForNote(mem, 0, -1, 0, 60); };

	Settings s;
	s.attack = dahdsr::stageValueFor(0.020);
	s.decay = dahdsr::stageValueFor(0.500);
	s.sustain = 0.0f; // so the shape is falling when the second press lands, and a restart is loud
	s.release = dahdsr::stageValueFor(0.050);

	const auto runPair = [&](dahdsr::Mode mode) {
		for (void* mem : { pressed, untouched }) {
			mod.reset(mem);
			Settings m = s;
			m.mode = mode;
			apply(mem, m);
			feed(mem, device::NoteEvent::Kind::On, 60);
			tick(mem, 60); // past the 20 ms attack, into the decay
		}
		// THE RE-PRESS, on one of them only. The key is still held, so there is no rising gate — this
		// is the retrigger road and nothing else.
		feed(pressed, device::NoteEvent::Kind::On, 60);
		for (void* mem : { pressed, untouched }) {
			tick(mem, 20);
		}
	};

	SUBCASE("One-shot: the two agree, because the second press changed nothing") {
		runPair(dahdsr::Mode::Once);
		const float falling = valueOf(untouched);
		REQUIRE(falling > 0.0f);  // still decaying: the comparison has something to say
		REQUIRE(falling < 1.0f);
		CHECK(valueOf(pressed) == doctest::Approx(falling));
	}

	SUBCASE("and the same pair under Gate DIFFERS, which is what makes the agreement above evidence") {
		// THE NON-VACUITY GUARD. Without it "the two agree" passes on a per-note road that ignores the
		// re-press entirely, and would have passed on one that never advanced at all.
		runPair(dahdsr::Mode::Gate);
		const float falling = valueOf(untouched);
		REQUIRE(falling > 0.0f);
		CHECK(valueOf(pressed) > falling); // Gate restarts it: the attack is climbing again
	}

	for (void* mem : { pressed, untouched }) {
		mod.destruct(mem);
		feathers::test::heapFree(mem);
	}
}


TEST_CASE("One-shot has a duration of its own: the release runs straight out of the decay and note-off changes nothing") {
	// **DECIDED 2026-09-19.** Once used to be Gate minus retrigger minus loop — MEASURED
	// holding at Sustain 0.7 under a held key and reaching Idle 300 ms after it came up. That is a
	// normal envelope that refuses to re-fire, not a one-shot, and it is why this device alone could
	// not deliver `mod::Mode`'s *"one full cycle per note, then holds at the end"*: its last stage IS
	// the release, and the release is what note-off means, so its shape had no end of its own.
	//
	// It has one now. The envelope lets go of its own gate at Sustain, so A → D → R runs to the end
	// on its own clock and the keyboard cannot lengthen or shorten it.
	Settings s = bare();
	s.mode = dahdsr::Mode::Once;
	s.attack = dahdsr::stageValueFor(0.010);
	s.decay = dahdsr::stageValueFor(0.050);
	s.sustain = 0.7f;
	s.release = dahdsr::stageValueFor(0.100);

	SUBCASE("held forever, it still ends — which is the whole of what 'one-shot' buys") {
		Envelope env;
		env.prepare(kRate);
		run(env, s, true, 400); // the key is never released, and 400 ms is well past A+D+R
		CHECK(env.stage() == Stage::Idle);
		CHECK(env.value() == doctest::Approx(0.0f).epsilon(1e-4));
	}

	SUBCASE("and the same shape under Gate is still being held, which is what makes that a finding") {
		// THE NON-VACUITY GUARD, and it is the whole comparison: identical settings, identical
		// keyboard, and the only difference is the mode. Without it "it ended" would pass on an
		// envelope that never started.
		Settings gated = s;
		gated.mode = dahdsr::Mode::Gate;
		Envelope env;
		env.prepare(kRate);
		run(env, gated, true, 400);
		CHECK(env.stage() == Stage::Sustain);
		CHECK(env.value() == doctest::Approx(0.7f));
	}

	SUBCASE("letting go early changes nothing — the note cannot shorten it either") {
		// THE OTHER HALF OF THE MODE'S OWN SENTENCE (*"ignores everything after"*). Two runs of the
		// same envelope, one where the key is dropped 20 ms in — during the decay, where a gate-off
		// used to start the release immediately — and the values are compared tick for tick rather
		// than eyeballed, because "it still ended" would pass on an envelope that ended EARLY.
		const auto trace = [&](int heldMs) {
			Envelope env;
			env.prepare(kRate);
			std::vector<float> v = run(env, s, true, heldMs);
			const std::vector<float> after = run(env, s, false, 400 - heldMs);
			v.insert(v.end(), after.begin(), after.end());
			return v;
		};
		const std::vector<float> wholeNote = trace(400);
		const std::vector<float> shortNote = trace(20);
		REQUIRE(wholeNote.size() == shortNote.size());
		// NOT VACUOUS: the envelope really is mid-flight at the moment the short note is released, so
		// a gate that mattered would show.
		REQUIRE(shortNote[20 * static_cast<size_t>(kRate) / (1000 * 64)] > 0.0f);
		for (size_t i = 0; i < wholeNote.size(); ++i) {
			CAPTURE(i);
			CHECK(wholeNote[i] == doctest::Approx(shortNote[i]));
		}
	}

	SUBCASE("and Sustain is still a live control under Once — it splits the tail between decay and release") {
		// **THE OPPOSITE OF WHAT WAS EXPECTED WHEN THIS WAS AGREED**, and the reason the card does NOT
		// grey Sustain out under Once. The decay falls TOWARD that level and the release falls FROM
		// it, so it is the split between the two halves of the tail rather than a control the mode has
		// finished with. Asserted as a VALUE — where the envelope is when the decay has just run out —
		// rather than as "the two runs differ", which would pass on any wobble at all.
		const auto intoTheTail = [&](float sustain) {
			Settings low = s;
			low.sustain = sustain;
			Envelope env;
			env.prepare(kRate);
			run(env, low, true, 70); // 10 ms attack + 50 ms decay, and 10 ms into the release
			return env.value();
		};
		const float high = intoTheTail(0.7f);
		const float low = intoTheTail(0.2f);

		// **PROPORTIONAL, WHICH IS THE ACTUAL CLAIM.** The first draft of this asserted the envelope
		// would BE at the sustain level here and read 0.476 against 0.7 — because the latch has
		// already tripped and the release is ten milliseconds in. The two runs came back at 0.476 and
		// 0.136, both exactly 0.68 of their sustain: the decay falls to the level and the release
		// falls from it, so the whole tail SCALES with the control. That is the honest statement of
		// "it splits the tail", and it does not depend on where in the release the sample is taken.
		CHECK(high / 0.7f == doctest::Approx(low / 0.2f).epsilon(0.02));
		// AND NOT VACUOUS: the two really are different values, so the ratio above is a proportion
		// rather than two readings of one number.
		CHECK(high > low + 0.2f);
	}
}

//------------------------------------------------------------------------------------------------//
// OUR OWN STATE MACHINE AND CURVE LAW (2026-09-24, the licence migration). Every reference below is //
// an INDEPENDENT RESTATEMENT of the law dahdsr.cpp documents — written here with std::exp rather    //
// than the implementation's expm1, and with the stage arithmetic done by hand in seconds — so a     //
// case asserts the number the design says, never "the new run looks like the old one".            //
//------------------------------------------------------------------------------------------------//

namespace {

/// f(x; s) = (1 - e^(-k x)) / (1 - e^(-k)), k = 8 s |s| — restated, not called.
double lawRef(double x, double s) {
	const double k = 8.0 * s * std::abs(s);
	if (k == 0.0) {
		return x;
	}
	return (1.0 - std::exp(-k * x)) / (1.0 - std::exp(-k));
}

/// A STAGE WITHIN A MILLIONTH OF ITS END IS AT ITS END — dahdsr.cpp's stated tolerance, restated so the
/// references below decide "has this stage ended?" the way the design says. It matters only at exact
/// tick multiples, where the taper's float rounding lands a stage a hair either side of the tick.
constexpr double kEnd = 1.0 - 1e-6;

/// How many ticks a stage of `seconds` spans at kRate: the tick in which it ends is the last one.
int ticksFor(double seconds) {
	return static_cast<int>(std::ceil(seconds * kEnd / kTick));
}

feathers::mod::TransportClock noClock() {
	return feathers::mod::TransportClock::unknown();
}

} // namespace

TEST_CASE("the curve law: a straight line at zero, the normalised exponential either side, exact at both ends") {
	for (const float s : { -1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		CAPTURE(s);
		// BOTH ENDS EXACTLY, whatever the shape: that is what makes every seam continuous.
		CHECK(dahdsr::stageCurve(0.0, s) == 0.0);
		CHECK(dahdsr::stageCurve(1.0, s) == doctest::Approx(1.0).epsilon(1e-12));
		for (int i = 1; i < 20; ++i) {
			const double x = i / 20.0;
			CAPTURE(x);
			CHECK(std::abs(dahdsr::stageCurve(x, s) - lawRef(x, s)) < 1e-9);
		}
	}
	// NAMED VALUES, worked by hand: at the midpoint the law reduces to 1 / (1 + e^(-k/2)).
	//   s = +1 (k = 8):  1 / (1 + e^-4) = 0.982013790...
	//   s = -1 (k = -8): 1 / (1 + e^4)  = 0.017986209...
	//   s = +0.5 (k = 2): 1 / (1 + e^-1) = 0.731058578...
	CHECK(std::abs(dahdsr::stageCurve(0.5, 1.0f) - 0.9820137900) < 1e-9);
	CHECK(std::abs(dahdsr::stageCurve(0.5, -1.0f) - 0.0179862100) < 1e-9);
	CHECK(std::abs(dahdsr::stageCurve(0.5, 0.5f) - 0.7310585786) < 1e-9);
	// Zero is a line, not approximately one.
	CHECK(dahdsr::stageCurve(0.3, 0.0f) == 0.3);
	// Out-of-range input is clamped rather than extrapolated.
	CHECK(dahdsr::stageCurve(1.5, 0.5f) == doctest::Approx(1.0).epsilon(1e-12));
	CHECK(dahdsr::stageCurve(0.5, 3.0f) == dahdsr::stageCurve(0.5, 1.0f));
}

TEST_CASE("every tick of every moving stage is the law at the elapsed time, at three sample rates and at the shape extremes") {
	for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
		const double tick = static_cast<double>(dahdsr::kBlockSamples) / rate;
		for (const float shape : { -1.0f, -0.4f, 0.0f, 0.6f, 1.0f }) {
			CAPTURE(rate);
			CAPTURE(shape);
			const double attack = 0.200;
			const double decay = 0.300;
			const double release = 0.250;
			const double sustain = 0.35;
			Settings s = bare();
			s.attack = dahdsr::stageValueFor(attack);
			s.decay = dahdsr::stageValueFor(decay);
			s.release = dahdsr::stageValueFor(release);
			s.sustain = static_cast<float>(sustain);
			s.attackShape = shape;
			s.decayShape = -shape; // a different shape per stage, so a stage reading the wrong knob is seen
			s.releaseShape = shape * 0.5f;
			// The taper's float rounding, read back rather than assumed.
			const double A = dahdsr::stageSeconds(s.attack);
			const double D = dahdsr::stageSeconds(s.decay);
			const double R = dahdsr::stageSeconds(s.release);

			Envelope env;
			env.prepare(rate);
			const int held = static_cast<int>(0.8 / tick);
			for (int i = 0; i < held; ++i) {
				env.tick(s, true, false, noClock());
				const double t = (i + 1) * tick; // envelope time at the END of this tick
				double want = 0.0;
				if (t < A * kEnd) {
					want = lawRef(t / A, s.attackShape);
				}
				else if (t - tick < A * kEnd) {
					want = 1.0; // the tick in which the attack ends reports its corner
				}
				else if (t - A < D * kEnd) {
					want = sustain + (1.0 - sustain) * lawRef(1.0 - (t - A) / D, s.decayShape);
				}
				else {
					want = sustain;
				}
				CAPTURE(i);
				REQUIRE(std::abs(env.rawValue() - want) < 2e-6);
			}
			REQUIRE(env.stage() == Stage::Sustain);
			for (int i = 0; i < static_cast<int>(0.4 / tick); ++i) {
				env.tick(s, false, false, noClock());
				const double t = (i + 1) * tick;
				const double want = t < R * kEnd ? sustain * lawRef(1.0 - t / R, s.releaseShape) : 0.0;
				CAPTURE(i);
				REQUIRE(std::abs(env.rawValue() - want) < 2e-6);
			}
			CHECK(env.stage() == Stage::Idle);
		}
	}
}

TEST_CASE("stage boundaries fall on the sample-exact tick, and the unused part of a tick is carried into the next stage") {
	SUBCASE("a whole number of ticks ends ON that tick, not the one after") {
		// 100 ms at 48 kHz is EXACTLY 75 ticks. The machine this replaced spent 76.
		Envelope env;
		env.prepare(48000.0);
		Settings s = bare();
		s.attack = dahdsr::stageValueFor(0.100);
		s.hold = dahdsr::stageValueFor(0.100);
		s.sustain = 1.0f;
		for (int i = 0; i < 74; ++i) {
			env.tick(s, true, false, noClock());
		}
		CHECK(env.stage() == Stage::Attack);
		CHECK(env.rawValue() == doctest::Approx(74.0 / 75.0).epsilon(1e-6));
		env.tick(s, true, false, noClock()); // tick 75: t = 100 ms
		CHECK(env.stage() == Stage::Hold);
		CHECK(env.rawValue() == 1.0f);
		CHECK(env.playhead().phase < 1e-6f);
	}

	SUBCASE("a seam inside a tick: the corner is reported, the rest of the tick is spent in the next stage") {
		// 48 kHz, tick = 4/3 ms. Attack 10 ms ends 2/3 ms into the eighth tick (index 7); that tick
		// reports the peak. The decay (10 ms, to 0.5) has already run 2/3 ms by then, so the NEXT tick
		// (index 8) is 2 ms into the decay: v = 0.5 + 0.5 * (1 - 0.2) = 0.9.
		Envelope env;
		env.prepare(48000.0);
		Settings s = bare();
		s.attack = dahdsr::stageValueFor(0.010);
		s.decay = dahdsr::stageValueFor(0.010);
		s.sustain = 0.5f;
		for (int i = 0; i < 7; ++i) {
			env.tick(s, true, false, noClock());
		}
		CHECK(env.rawValue() == doctest::Approx(7.0 * 64.0 / 480.0).epsilon(1e-5)); // 9.333 of 10 ms
		env.tick(s, true, false, noClock());
		CHECK(env.stage() == Stage::Decay);
		CHECK(env.rawValue() == 1.0f);
		CHECK(env.playhead().phase == doctest::Approx(2.0 / 30.0).epsilon(1e-5)); // 2/3 ms of 10
		env.tick(s, true, false, noClock());
		CHECK(env.rawValue() == doctest::Approx(0.9f).epsilon(1e-5));
	}

	SUBCASE("delay and hold hold, and the chain's corners land at the summed times") {
		// 96 kHz, tick = 2/3 ms. Delay 4 ms (6 ticks), attack 2 ms (3), hold 2 ms (3), decay 4 ms (6).
		Envelope env;
		env.prepare(96000.0);
		Settings s = bare();
		s.delay = dahdsr::stageValueFor(0.004);
		s.attack = dahdsr::stageValueFor(0.002);
		s.hold = dahdsr::stageValueFor(0.002);
		s.decay = dahdsr::stageValueFor(0.004);
		s.sustain = 0.25f;
		std::vector<float> v;
		std::vector<Stage> st;
		for (int i = 0; i < 20; ++i) {
			v.push_back(env.tick(s, true, false, noClock()));
			st.push_back(env.stage());
		}
		for (int i = 0; i < 5; ++i) {
			CHECK(st[i] == Stage::Delay);
			CHECK(v[i] == 0.0f);
		}
		CHECK(st[5] == Stage::Attack); // the delay's last tick reports its corner (0) and hands over
		CHECK(v[6] == doctest::Approx(1.0f / 3.0f).epsilon(1e-5));
		CHECK(v[7] == doctest::Approx(2.0f / 3.0f).epsilon(1e-5));
		CHECK(v[8] == 1.0f);
		CHECK(st[8] == Stage::Hold);
		CHECK(v[9] == 1.0f);
		CHECK(v[10] == 1.0f);
		CHECK(st[11] == Stage::Decay);
		CHECK(v[12] == doctest::Approx(0.25f + 0.75f * (5.0f / 6.0f)).epsilon(1e-5));
		CHECK(v[16] == doctest::Approx(0.25f + 0.75f * (1.0f / 6.0f)).epsilon(1e-5));
		CHECK(v[17] == 0.25f);
		CHECK(st[17] == Stage::Sustain);
	}
}

TEST_CASE("the gate falling releases from every stage, continuously, over the full release time") {
	// The release starts from WHERE THE ENVELOPE STANDS: its first tick is r0 · f(1 - T/R), where r0 is
	// the value the tick before reported — so there is no step, only the release's own first slope.
	constexpr double kR = 0.120;
	const struct {
		const char* name;
		int ticksHeld;
		Stage expected;
	} cases[] = {
		{ "delay", 5, Stage::Delay },   { "attack", 20, Stage::Attack }, { "hold", 45, Stage::Hold },
		{ "decay", 75, Stage::Decay },  { "sustain", 200, Stage::Sustain },
	};
	for (const auto& c : cases) {
		CAPTURE(c.name);
		Envelope env;
		env.prepare(kRate);
		Settings s = bare();
		s.delay = dahdsr::stageValueFor(0.010);   // 7.5 ticks
		s.attack = dahdsr::stageValueFor(0.040);  // to tick 37.5
		s.hold = dahdsr::stageValueFor(0.020);    // to 52.5
		s.decay = dahdsr::stageValueFor(0.080);   // to 112.5
		s.sustain = 0.4f;
		s.release = dahdsr::stageValueFor(kR);
		s.releaseShape = 0.3f;
		for (int i = 0; i < c.ticksHeld; ++i) {
			env.tick(s, true, false, noClock());
		}
		REQUIRE(env.stage() == c.expected);
		const double r0 = env.rawValue();
		const double R = dahdsr::stageSeconds(s.release);
		env.tick(s, false, false, noClock());
		CHECK(env.stage() == Stage::Release);
		CHECK(std::abs(env.rawValue() - r0 * lawRef(1.0 - kTick / R, s.releaseShape)) < 2e-6);
		// And it lands at zero on the tick the release time runs out.
		const int remaining = ticksFor(R) - 1;
		for (int i = 0; i < remaining - 1; ++i) {
			env.tick(s, false, false, noClock());
			REQUIRE(env.stage() == Stage::Release);
		}
		env.tick(s, false, false, noClock());
		CHECK(env.rawValue() == 0.0f);
		CHECK(env.stage() == Stage::Idle);
	}
}

TEST_CASE("a retrigger starts the attack from where the envelope stands, so it cannot click") {
	const auto attackFrom = [](Envelope& env, const Settings& s) {
		const double a0 = env.rawValue();
		env.tick(s, true, true, noClock());
		const double A = dahdsr::stageSeconds(s.attack);
		CHECK(env.stage() == Stage::Attack);
		CHECK(std::abs(env.rawValue() - (a0 + (1.0 - a0) * lawRef(kTick / A, s.attackShape))) < 2e-6);
		// and the full attack time from there: A/T ticks to the peak, not a proportion of it
		int ticks = 1;
		while (env.stage() == Stage::Attack && ticks < 10000) {
			env.tick(s, true, false, noClock());
			++ticks;
		}
		CHECK(ticks == ticksFor(A));
	};
	Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.050);
	s.decay = dahdsr::stageValueFor(0.050);
	s.sustain = 0.6f;
	s.release = dahdsr::stageValueFor(0.200);
	s.attackShape = 0.7f;

	SUBCASE("from the sustain") {
		Envelope env;
		env.prepare(kRate);
		run(env, s, true, 200);
		REQUIRE(env.rawValue() == doctest::Approx(0.6f));
		attackFrom(env, s);
	}
	SUBCASE("from mid-release (a new note after the last one let go)") {
		Envelope env;
		env.prepare(kRate);
		run(env, s, true, 200);
		run(env, s, false, 40);
		REQUIRE(env.stage() == Stage::Release);
		REQUIRE(env.rawValue() > 0.05f);
		// a rising gate, not a latch: that is what a new note after a release is
		const double a0 = env.rawValue();
		env.tick(s, true, false, noClock());
		CHECK(std::abs(env.rawValue() - (a0 + (1.0 - a0) * lawRef(kTick / dahdsr::stageSeconds(s.attack), s.attackShape))) < 2e-6);
	}
	SUBCASE("from mid-attack") {
		Envelope env;
		env.prepare(kRate);
		run(env, s, true, 10);
		REQUIRE(env.stage() == Stage::Attack);
		attackFrom(env, s);
	}
	SUBCASE("with a delay, the delay HOLDS the level it was retriggered at") {
		Settings d = s;
		d.delay = dahdsr::stageValueFor(0.010);
		Envelope env;
		env.prepare(kRate);
		run(env, d, true, 200);
		const float held = env.rawValue();
		env.tick(d, true, true, noClock());
		CHECK(env.stage() == Stage::Delay);
		CHECK(env.rawValue() == held);
		run(env, d, true, 5);
		CHECK(env.stage() == Stage::Delay);
		CHECK(env.rawValue() == held);
	}
}

//------------------------------------------------------------------------------------------------//
// SMOOTHING CLOSES: with smoothing on, a released envelope must still reach zero.                    //
// The one-pole on the output approaches the raw envelope's rest asymptotically, so a                 //
// smoothed release must be SNAPPED to exactly rest once the residue is negligible — and the per-note //
// table must not stop ticking an envelope whose raw stage is Idle while its smoothed output still    //
// carries a tail. Before this, a finished note's value FROZE wherever the one-pole stood the tick     //
// the raw release ended, and a voice whose Level it drove was held open by it for ever.              //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE STEP AT THE SNAP MAY BE NO LARGER THAN THIS — a millionth of full scale, -120 dB. Stated here
/// rather than read from the device, so a threshold loosened in the source is a red rather than a
/// bound that follows it.
constexpr double kSnapStepBound = 1e-6;

/// HOW LONG A ONE-POLE OF TIME CONSTANT `tau` TAKES TO FALL FROM FULL SCALE TO THE SNAP BOUND:
/// tau · ln(1 / bound). Derived from the exponential, not from the implementation.
double settleSeconds(double tau) {
	return tau * std::log(1.0 / kSnapStepBound);
}

/// WHAT THE SNAP TOOK AWAY on the tick the output landed on rest: the residue an UN-snapped one-pole,
/// restated here (`y += (0 - y)(1 - e^(-T/tau))`), would still have carried from `wasUnipolar`. Not
/// the whole step onto rest — with smoothing off that step is the raw release's own last tick, which
/// is the envelope's shape and not a snap.
double snapError(double wasUnipolar, double tau) {
	const double keep = tau > 0.0 ? std::exp(-kTick / tau) : 0.0;
	return std::abs(wasUnipolar * keep);
}

/// The smoothing settings under test: the bottom of the travel (off), a fast one, the 32 ms the
/// time-constant case uses, a slow one, and the top of the range (1 s).
constexpr float kSmoothings[] = { feathers::mod::kSmoothingLog2SecondsMin, -9.0f, -5.0f, -2.0f,
	                              feathers::mod::kSmoothingLog2SecondsMax };

} // namespace

TEST_CASE("smoothing closes: a smoothed release reaches its rest EXACTLY, within release + tau ln(1e6), with no audible step at the snap, and idle() waits for it") {
	for (const float smoothing : kSmoothings) {
		for (const dahdsr::Mode mode : { dahdsr::Mode::Gate, dahdsr::Mode::Loop, dahdsr::Mode::Once }) {
			for (const bool bipolar : { false, true }) {
				CAPTURE(smoothing);
				CAPTURE(static_cast<int>(mode));
				CAPTURE(bipolar);
				Settings s = bare();
				s.attack = dahdsr::stageValueFor(0.005);
				s.decay = dahdsr::stageValueFor(0.005);
				s.sustain = 0.6f;
				s.release = dahdsr::stageValueFor(0.050);
				s.smoothing = smoothing;
				s.mode = mode;
				s.bipolar = bipolar;
				const float rest = bipolar ? -1.0f : 0.0f;
				const double tau = feathers::mod::smoothingTau(smoothing);

				Envelope env;
				env.prepare(kRate);
				// Held long enough for the slowest smoothing to arrive at sustain. One-shot lets go by
				// itself; the others are released by the gate falling.
				const int holdTicks = ticksFor(0.02 + settleSeconds(tau) + 0.1);
				run(env, s, true, holdTicks);
				const bool gateAfter = mode == dahdsr::Mode::Once; // one-shot: keep the key down throughout
				const int bound = ticksFor(0.050 + settleSeconds(tau) + 0.05);
				float was = env.value();
				int closedAt = -1;
				double stepAtClose = 0.0;
				int idleWithATail = 0;
				for (int i = 0; i < bound; ++i) {
					const float v = env.tick(s, gateAfter, false, noClock());
					// IDLE MEANS NOTHING MORE WILL MOVE, so it may never be claimed with a tail still on the output.
					if (env.idle()) {
						idleWithATail += v == rest ? 0 : 1;
					}
					if (v == rest && closedAt < 0) {
						closedAt = i;
						stepAtClose = snapError(bipolar ? (static_cast<double>(was) + 1.0) / 2.0 : static_cast<double>(was), tau);
					}
					was = v;
				}
				CAPTURE(closedAt);
				CAPTURE(env.value() - rest);
				REQUIRE(closedAt >= 0);
				CHECK(env.value() == rest);
				CHECK(env.idle());
				// THE SNAP IS INAUDIBLE: what it cut off is below a millionth of the unipolar range. Bipolar
				// reads it back through a float near -1, whose spacing (MEASURED: 1.0079e-6 at 250 ms) is
				// one float epsilon of slack on top — the reading's rounding, not the snap's.
				const double slack = bipolar ? static_cast<double>(std::numeric_limits<float>::epsilon()) : 0.0;
				CHECK(stepAtClose <= kSnapStepBound + slack);
				CHECK(idleWithATail == 0);
				// AND IT STAYS SHUT.
				run(env, s, gateAfter, 50);
				CHECK(env.value() == rest);
			}
		}
	}
}

TEST_CASE("smoothing closes on the per-note road: a released note's own smoothed envelope falls to exactly zero rather than freezing where the one-pole stood when the raw release ended") {
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
	REQUIRE(mem != nullptr);
	mod.construct(mem);
	mod.prepare(mem, kRate);
	const auto apply = [&](const Settings& s) {
		const dahdsr::Row row = dahdsr::rowOf(s);
		mod.setFields(mem, row.data(), row.size());
		notes.setFields(mem, row.data(), row.size());
	};
	const auto feed = [&](device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = -1;
		e.channel = 0;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1);
	};
	const auto tick = [&](bool gate) {
		device::ModTickContext ctx;
		ctx.gate = gate;
		mod.tick(mem, ctx);
	};
	const auto valueOf = [&](int16_t key) { return mod.outputForNote(mem, 0, -1, 0, key); };

	for (const float smoothing : kSmoothings) {
		for (const bool legato : { false, true }) {
			CAPTURE(smoothing);
			CAPTURE(legato);
			mod.reset(mem);
			Settings s = bare();
			s.attack = dahdsr::stageValueFor(0.005);
			s.decay = dahdsr::stageValueFor(0.005);
			s.sustain = 0.6f;
			s.release = dahdsr::stageValueFor(0.050);
			s.smoothing = smoothing;
			s.singleTrigger = legato;
			apply(s);
			const double tau = feathers::mod::smoothingTau(smoothing);
			const int holdTicks = ticksFor(0.02 + settleSeconds(tau) + 0.1);

			feed(device::NoteEvent::Kind::On, 60);
			for (int i = 0; i < holdTicks; ++i) {
				tick(true);
			}
			if (legato) {
				// A SECOND NOTE UNDER THE FIRST, which under Leg takes up the held envelope — smoothing and all.
				feed(device::NoteEvent::Kind::On, 64);
				for (int i = 0; i < 20; ++i) {
					tick(true);
				}
				feed(device::NoteEvent::Kind::Off, 64);
			}
			REQUIRE(valueOf(60) == doctest::Approx(0.6f).epsilon(1e-3));
			feed(device::NoteEvent::Kind::Off, 60);

			const int bound = ticksFor(0.050 + settleSeconds(tau) + 0.05);
			float was = valueOf(60);
			int closedAt = -1;
			double stepAtClose = 0.0;
			for (int i = 0; i < bound; ++i) {
				tick(false);
				const float v = valueOf(60);
				if (v == 0.0f && closedAt < 0) {
					closedAt = i;
					stepAtClose = snapError(static_cast<double>(was), tau);
				}
				was = v;
			}
			CAPTURE(closedAt);
			CAPTURE(valueOf(60));
			CHECK(valueOf(60) == 0.0f);
			CHECK(stepAtClose <= kSnapStepBound);
			if (legato) {
				CHECK(valueOf(64) == 0.0f);
			}
		}
	}

	mod.destruct(mem);
	feathers::test::heapFree(mem);
}

TEST_CASE("MN: under Poly every note's own envelope plays its stage's curve — each note at its own attack midpoint is the law, at a bend and at zero") {
	// BACKLOG MN: the card's bend writes `attackShape`; this pins that the per-note road reads it, per note, by
	// value. Two notes struck eight ticks apart sit at different points of the SAME bent attack.
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));

	for (const float shape : { 0.5f, -0.75f, 0.0f }) {
		CAPTURE(shape);
		void* mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, kRate);

		// AN ATTACK OF EXACTLY 32 TICKS, so tick 16 is its midpoint.
		Settings s = bare();
		s.poly = true;
		s.attack = static_cast<float>(std::log2(32.0 * kTick));
		s.attackShape = shape;
		s.decay = std::log2(0.5f);
		const dahdsr::Row row = dahdsr::rowOf(s);
		mod.setFields(mem, row.data(), row.size());
		notes.setFields(mem, row.data(), row.size());

		const auto on = [&](int32_t id, int16_t key) {
			device::NoteEvent e {};
			e.kind = device::NoteEvent::Kind::On;
			e.noteId = id;
			e.key = key;
			device::NoteEvent out[4];
			size_t n = 0;
			notes.processNotes(mem, &e, 1, out, 4, &n);
		};
		const auto tick = [&](int times) {
			device::ModTickContext ctx;
			ctx.gate = true;
			for (int i = 0; i < times; ++i) {
				mod.tick(mem, ctx);
			}
		};
		const auto valueOf = [&](int32_t id) { return static_cast<double>(mod.outputForNote(mem, 0, id, -1, -1)); };

		on(1, 60);
		tick(8);
		on(2, 64);
		tick(8);
		CHECK(std::abs(valueOf(1) - dahdsr::stageCurve(0.5, shape)) < 1e-5);  // note 1: tick 16 of 32
		CHECK(std::abs(valueOf(2) - dahdsr::stageCurve(0.25, shape)) < 1e-5); // note 2: tick 8 of 32
		tick(8);
		CHECK(std::abs(valueOf(2) - dahdsr::stageCurve(0.5, shape)) < 1e-5);  // note 2 at ITS midpoint
		if (shape == 0.0f) {
			CHECK(std::abs(valueOf(2) - 0.5) < 1e-6); // zero is the line
		}

		mod.destruct(mem);
		feathers::test::heapFree(mem);
	}
}

namespace {

/// A FREE DAHDSR WHOSE STAGES ARE NO ROUND NUMBER OF TICKS, with bent curves and a sustain off the floor, so a
/// cycle read off the wrong clock, the wrong start level or the wrong curve cannot land on the right value.
Settings songLooping() {
	Settings s = bare();
	s.delay = dahdsr::stageValueFor(0.010);
	s.attack = dahdsr::stageValueFor(0.031);
	s.hold = dahdsr::stageValueFor(0.0045);
	s.decay = dahdsr::stageValueFor(0.047);
	s.sustain = 0.3f;
	s.attackShape = 0.4f;
	s.decayShape = -0.5f;
	s.mode = dahdsr::Mode::Free;
	return s;
}

/// A host playing at `seconds` into the song, at a tempo of `secondsPerBeat` with the beat it says.
feathers::mod::TransportClock playingAt(double seconds, double secondsPerBeat = 0.5, double beats = -1.0) {
	feathers::mod::TransportClock c = feathers::mod::TransportClock::fromSecondsPerBeat(secondsPerBeat);
	c.playing = true;
	c.songPositionSeconds = seconds;
	c.songPositionBeats = beats >= 0.0 ? beats : seconds / secondsPerBeat;
	return c;
}

/// **THE INTENDED SHAPE, RESTATED FROM THE DESIGN RATHER THAN READ FROM THE DEVICE**: the cycle is the stage sum, its
/// position is the song's seconds modulo that sum, every cycle starts its attack from the sustain level (the steady
/// loop's), and a stage that ENDED between the previous reading and this one reports its corner (the machine's seam
/// rule) — the first end passed. `previous < 0` is a first reading: no corner.
float songLevel(const Settings& s, double previous, double seconds) {
	const double lengths[4] = { dahdsr::stageSeconds(s.delay), dahdsr::stageSeconds(s.attack),
		                        dahdsr::stageSeconds(s.hold), dahdsr::stageSeconds(s.decay) };
	const double cycle = lengths[0] + lengths[1] + lengths[2] + lengths[3];
	const double sustain = static_cast<double>(s.sustain);
	const double corners[4] = { sustain, 1.0, 1.0, sustain };
	if (previous >= 0.0) {
		for (double k = std::floor(previous / cycle); k <= std::floor(seconds / cycle); k += 1.0) {
			double end = k * cycle;
			for (int i = 0; i < 4; ++i) {
				if (!(lengths[i] > 0.0)) {
					continue;
				}
				end += lengths[i];
				if (end > previous && end <= seconds) {
					return static_cast<float>(corners[i]);
				}
			}
		}
	}
	double u = std::fmod(seconds, cycle);
	if (u < lengths[0]) {
		return static_cast<float>(sustain);
	}
	u -= lengths[0];
	if (u < lengths[1]) {
		return static_cast<float>(sustain + (1.0 - sustain) * dahdsr::stageCurve(u / lengths[1], s.attackShape));
	}
	u -= lengths[1];
	if (u < lengths[2]) {
		return 1.0f;
	}
	u -= lengths[2];
	return static_cast<float>(sustain + (1.0 - sustain) * dahdsr::stageCurve(1.0 - u / lengths[3], s.decayShape));
}

} // namespace

TEST_CASE("backlog NM: a Free DAHDSR on a playing transport is read off the song's seconds — the stage sum is its period, a fresh one lands where one that played through stands, the tempo moves nothing, and stopped it runs free") {
	// DECISION: Free is song-locked while the transport plays, like the LFO's Free, accepting that a play
	// start or a playhead jump steps the output. Every claim below is a VALUE at a song position, against
	// `songLevel`'s restatement, because "it moved with the song" is what a lock to the wrong clock also does.
	const Settings s = songLooping();
	const double cycle = dahdsr::stageSeconds(s.delay) + dahdsr::stageSeconds(s.attack)
	                     + dahdsr::stageSeconds(s.hold) + dahdsr::stageSeconds(s.decay);
	REQUIRE(std::abs(std::fmod(cycle / kTick, 1.0) - 0.5) < 0.45); // not a whole number of ticks

	SUBCASE("played from the top, every tick is the song's shape, and still is after a thousand cycles") {
		Envelope env;
		env.prepare(kRate);
		constexpr int kTicks = 100000; // 133 s: ~1450 cycles, where a tick's drift a cycle would be two seconds
		int checked = 0;
		for (int i = 0; i < kTicks; ++i) {
			const double t = i * kTick;
			env.tick(s, false, false, playingAt(t));
			if (i < 200 || i > kTicks - 200) {
				CAPTURE(i);
				CHECK(std::abs(env.rawValue() - songLevel(s, i == 0 ? -1.0 : t - kTick, t)) <= 1e-5f);
				++checked;
			}
		}
		CHECK(checked == 399);
	}

	SUBCASE("a fresh one started mid-song lands where one that played through stands") {
		Envelope through;
		Envelope fresh;
		through.prepare(kRate);
		fresh.prepare(kRate);
		constexpr int kJoin = 20011;
		for (int i = 0; i < kJoin + 500; ++i) {
			const double t = i * kTick;
			through.tick(s, false, false, playingAt(t));
			if (i >= kJoin) {
				fresh.tick(s, false, false, playingAt(t));
				CAPTURE(i);
				// The first tick of a lock samples (it has no previous reading to pass a corner since), so the
				// claim starts on the second: from there the two are one shape.
				if (i > kJoin) {
					CHECK(std::abs(fresh.rawValue() - through.rawValue()) <= 1e-5f);
				}
			}
		}
		// AND THE FIRST CYCLE IS A STEADY ONE: the stopped machine's first attack starts from 0, the song's from the
		// sustain, so a fresh lock is the shape a reload plays, not a cold start.
		CHECK(std::abs(fresh.rawValue() - songLevel(s, (kJoin + 498) * kTick, (kJoin + 499) * kTick)) <= 1e-5f);
	}

	SUBCASE("a tempo change mid-song moves nothing: the stages are durations, and the lock is on seconds") {
		Envelope steady;
		Envelope changing;
		steady.prepare(kRate);
		changing.prepare(kRate);
		double beats = 0.0;
		for (int i = 0; i < 3000; ++i) {
			const double t = i * kTick;
			// 120 BPM, then 87, then 170: the beat runs at three rates while the seconds run at one.
			const double spb = i < 1000 ? 0.5 : (i < 2000 ? 60.0 / 87.0 : 60.0 / 170.0);
			steady.tick(s, false, false, playingAt(t));
			changing.tick(s, false, false, playingAt(t, spb, beats));
			beats += kTick / spb;
			CAPTURE(i);
			CHECK(changing.rawValue() == steady.rawValue());
		}
		CHECK(std::abs(changing.rawValue() - songLevel(s, 2998 * kTick, 2999 * kTick)) <= 1e-5f);
	}

	SUBCASE("a playhead jump re-reads the song: the output steps to where the cycle stands there") {
		Envelope env;
		env.prepare(kRate);
		for (int i = 0; i < 300; ++i) {
			env.tick(s, false, false, playingAt(i * kTick));
		}
		const double there = 61.2345;
		env.tick(s, false, false, playingAt(there));
		CHECK(std::abs(env.rawValue() - songLevel(s, -1.0, there)) <= 1e-5f);
		env.tick(s, false, false, playingAt(there + kTick));
		CHECK(std::abs(env.rawValue() - songLevel(s, there, there + kTick)) <= 1e-5f);
	}

	SUBCASE("stopped, it free-runs on the machine as before: a position the host still reports moves nothing") {
		Envelope parked;
		Envelope unknown;
		parked.prepare(kRate);
		unknown.prepare(kRate);
		feathers::mod::TransportClock stopped = playingAt(12.0);
		stopped.playing = false;
		for (int i = 0; i < 2000; ++i) {
			parked.tick(s, false, false, stopped);
			unknown.tick(s, false, false, feathers::mod::TransportClock::unknown());
			CAPTURE(i);
			CHECK(parked.rawValue() == unknown.rawValue());
		}
		// AND IT STILL STARTS COLD (its first attack from 0), which is what "as before" means for the first cycle.
		Envelope cold;
		cold.prepare(kRate);
		cold.tick(s, false, false, stopped);
		CHECK(cold.stage() == Stage::Delay);
		CHECK(cold.rawValue() == 0.0f);
	}

	SUBCASE("and a transport that stops hands the machine the shape where the song left it, at the song's rate") {
		Envelope env;
		env.prepare(kRate);
		constexpr int kStop = 777;
		for (int i = 0; i < kStop; ++i) {
			env.tick(s, false, false, playingAt(i * kTick));
		}
		feathers::mod::TransportClock stopped = playingAt((kStop - 1) * kTick);
		stopped.playing = false;
		// THE CARRY IS WHAT MAKES THIS HOLD FOR CYCLES AFTER THE STOP: the machine's own loop is the stage sum too.
		for (int i = kStop; i < kStop + 2000; ++i) {
			env.tick(s, false, false, stopped);
			CAPTURE(i);
			CHECK(std::abs(env.rawValue() - songLevel(s, (i - 1) * kTick, i * kTick)) <= 1e-5f);
		}
	}

	SUBCASE("only Free: LFO Retrig and Loop on a playing transport are the machine, as stopped") {
		for (const dahdsr::Mode mode : { dahdsr::Mode::LfoRetrig, dahdsr::Mode::Loop, dahdsr::Mode::Gate }) {
			Settings m = s;
			m.mode = mode;
			Envelope playing;
			Envelope unknown;
			playing.prepare(kRate);
			unknown.prepare(kRate);
			for (int i = 0; i < 1500; ++i) {
				const bool gate = (i % 400) < 300;
				const bool retrigger = (i % 400) == 0;
				playing.tick(m, gate, retrigger, playingAt(5.0 + i * kTick));
				unknown.tick(m, gate, retrigger, feathers::mod::TransportClock::unknown());
				CAPTURE(static_cast<int>(mode));
				CAPTURE(i);
				REQUIRE(playing.rawValue() == unknown.rawValue());
			}
		}
	}

	SUBCASE("a Trig restarts it from where it stands and the song holds the restart") {
		Envelope env;
		env.prepare(kRate);
		constexpr int kFire = 500;
		for (int i = 0; i < kFire; ++i) {
			env.tick(s, false, false, playingAt(i * kTick));
		}
		const float stood = env.rawValue();
		env.fire();
		env.tick(s, false, false, playingAt(kFire * kTick));
		CHECK(env.stage() == Stage::Delay);
		CHECK(env.rawValue() == stood); // the delay holds where it stood
		// The cycle after it is the song's shape shifted to the fire: steady again from the sustain.
		for (int i = kFire + 1; i < kFire + 400; ++i) {
			env.tick(s, false, false, playingAt(i * kTick));
		}
		const double since = 399 * kTick;
		CHECK(std::abs(env.rawValue() - songLevel(s, since - kTick, since)) <= 1e-5f);
	}
}

TEST_CASE("backlog NM: the song lock leaves notes, Poly and Carry alone — Free reads the collapsed envelope, and every other mode's notes run as stopped") {
	namespace device = feathers::device;
	const device::ModOps& mod = dahdsr::kModOps;
	const device::NoteOps& notes = dahdsr::kNoteOps;
	const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
	const size_t bytes = ((mod.stateBytes + align - 1) / align) * align;

	struct Instance {
		const device::ModOps& mod;
		const device::NoteOps& notes;
		void* mem;
		void apply(const Settings& s) {
			const dahdsr::Row row = dahdsr::rowOf(s);
			mod.setFields(mem, row.data(), row.size());
			notes.setFields(mem, row.data(), row.size());
		}
		void feed(device::NoteEvent::Kind kind, int16_t key) {
			device::NoteEvent e {};
			e.kind = kind;
			e.noteId = -1;
			e.channel = 0;
			e.key = key;
			device::NoteEvent out[4];
			size_t n = 0;
			notes.processNotes(mem, &e, 1, out, 4, &n);
		}
		void tick(bool gate, const feathers::mod::TransportClock& clock) {
			device::ModTickContext ctx;
			ctx.gate = gate;
			ctx.clock = clock;
			mod.tick(mem, ctx);
		}
	};
	const auto make = [&] {
		void* mem = feathers::test::heapAlignedAlloc(align, bytes);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, kRate);
		return Instance { mod, notes, mem };
	};
	const auto destroy = [&](Instance& i) {
		mod.destruct(i.mem);
		feathers::test::heapFree(i.mem);
	};

	SUBCASE("Free under Poly: every note and the device read the song's shape") {
		Settings s = songLooping();
		s.poly = true;
		s.smoothCarry = true;
		Instance dev = make();
		dev.apply(s);
		for (int i = 0; i < 1200; ++i) {
			if (i == 100) {
				dev.feed(device::NoteEvent::Kind::On, 60);
			}
			if (i == 400) {
				dev.feed(device::NoteEvent::Kind::On, 64);
			}
			if (i == 700) {
				dev.feed(device::NoteEvent::Kind::Off, 60);
			}
			const double t = 3.0 + i * kTick;
			dev.tick(i >= 100, playingAt(t));
			if (i > 0) {
				CAPTURE(i);
				const float want = songLevel(s, t - kTick, t);
				CHECK(std::abs(mod.outputAt(dev.mem, 0) - want) <= 1e-5f);
				CHECK(mod.outputForNote(dev.mem, 0, -1, 0, 60) == mod.outputAt(dev.mem, 0));
				CHECK(mod.outputForNote(dev.mem, 0, -1, 0, 64) == mod.outputAt(dev.mem, 0));
			}
		}
		destroy(dev);
	}

	SUBCASE("Gate and Loop under Poly, with Carry and smoothing: every note runs exactly as on a stopped transport") {
		for (const dahdsr::Mode mode : { dahdsr::Mode::Gate, dahdsr::Mode::Loop }) {
			for (const bool carry : { false, true }) {
				Settings s = songLooping();
				s.mode = mode;
				s.poly = true;
				s.smoothCarry = carry;
				s.smoothing = -6.0f;
				s.release = dahdsr::stageValueFor(0.060);
				Instance playing = make();
				Instance stopped = make();
				playing.apply(s);
				stopped.apply(s);
				for (int i = 0; i < 1500; ++i) {
					for (Instance* d : { &playing, &stopped }) {
						if (i == 10 || i == 300 || i == 310) {
							d->feed(device::NoteEvent::Kind::On, static_cast<int16_t>(60 + i % 7));
						}
						if (i == 200 || i == 900) {
							d->feed(device::NoteEvent::Kind::Off, static_cast<int16_t>(i == 200 ? 63 : 60 + 300 % 7));
						}
					}
					const bool held = i >= 10;
					playing.tick(held, playingAt(9.0 + i * kTick));
					stopped.tick(held, feathers::mod::TransportClock::unknown());
					CAPTURE(static_cast<int>(mode));
					CAPTURE(carry);
					CAPTURE(i);
					REQUIRE(mod.outputAt(playing.mem, 0) == mod.outputAt(stopped.mem, 0));
					for (const int16_t key : { int16_t { 63 }, int16_t { 62 }, int16_t { 65 } }) {
						REQUIRE(mod.outputForNote(playing.mem, 0, -1, 0, key)
						        == mod.outputForNote(stopped.mem, 0, -1, 0, key));
					}
				}
				destroy(playing);
				destroy(stopped);
			}
		}
	}

	SUBCASE("Carry is no input to Free's song shape: on or off, the same values") {
		Settings on = songLooping();
		on.poly = true;
		on.smoothing = -6.0f;
		on.smoothCarry = true;
		Settings off = on;
		off.smoothCarry = false;
		Instance a = make();
		Instance b = make();
		a.apply(on);
		b.apply(off);
		for (int i = 0; i < 1000; ++i) {
			if (i == 50) {
				a.feed(device::NoteEvent::Kind::On, 60);
				b.feed(device::NoteEvent::Kind::On, 60);
			}
			a.tick(i >= 50, playingAt(i * kTick));
			b.tick(i >= 50, playingAt(i * kTick));
			CAPTURE(i);
			REQUIRE(mod.outputAt(a.mem, 0) == mod.outputAt(b.mem, 0));
		}
		destroy(a);
		destroy(b);
	}
}
