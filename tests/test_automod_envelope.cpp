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

#include "fx/automod/automod_envelope.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace feathers::fx::automod;

namespace {

constexpr double kSr = 48000.0;

EnvelopeFollower make(float attackSeconds, float releaseSeconds) {
	EnvelopeFollower e;
	e.prepare(kSr);
	e.setTimes(attackSeconds, releaseSeconds);
	return e;
}

/// Feed a constant level for a duration, and report where the left channel ended up.
float driveFor(EnvelopeFollower& e, float value, double seconds) {
	const int n = static_cast<int>(seconds * kSr);
	for (int i = 0; i < n; ++i) {
		e.process(value, value);
	}
	return e.level(Channel::Left);
}

} // namespace

TEST_CASE("automod envelope: one time constant of a step covers 1 - 1/e of the distance") {
	// THE DEFINING VALUE OF A ONE-POLE, computed from the maths rather than from a reference render.
	// "It rose" is satisfied by any coefficient; 63.2% after exactly one time constant is satisfied
	// only by the right one, which is what makes this a test of the TIME rather than of the motion.
	constexpr float kAttack = 0.010f; // 10 ms
	EnvelopeFollower e = make(kAttack, 1.0f);

	const float after = driveFor(e, 1.0f, kAttack);
	const float expected = 1.0f - std::exp(-1.0f);

	CHECK(after == doctest::Approx(expected).epsilon(2e-3));

	// And it keeps converging on the input rather than stopping partway.
	const float settled = driveFor(e, 1.0f, 10.0 * kAttack);
	CHECK(settled == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("automod envelope: the release time is a different time constant, which is what makes it a follower") {
	// ASYMMETRY, ASSERTED AS TWO VALUES. A lowpass would satisfy "it goes up and it comes down"; only
	// the two different time constants say this is an envelope follower.
	constexpr float kAttack = 0.001f;  // 1 ms
	constexpr float kRelease = 0.050f; // 50 ms
	EnvelopeFollower e = make(kAttack, kRelease);

	driveFor(e, 1.0f, 0.2); // settle at 1

	// One release time constant of silence leaves 1/e of the level behind.
	const float after = driveFor(e, 0.0f, kRelease);
	CHECK(after == doctest::Approx(std::exp(-1.0f)).epsilon(2e-3));

	// NON-VACUITY, and it is the assertion that catches a follower using one coefficient for both:
	// the same span of the ATTACK time would have taken it far further down if release == attack.
	EnvelopeFollower symmetric = make(kAttack, kAttack);
	driveFor(symmetric, 1.0f, 0.2);
	const float symAfter = driveFor(symmetric, 0.0f, kRelease);
	CHECK(symAfter < after * 0.01f);
}

TEST_CASE("automod envelope: a time constant means the same thing at any sample rate") {
	// THIS CASE REPLACED A VACUOUS ONE, AND THE MUTATION SWEEP IS WHAT EXPOSED IT.
	//
	// The first draft asserted block invariance by running identical audio in blocks of 1, 37 and 512
	// and comparing. It passed — and SEVEN known-bads later, not one of them had reddened it. That is
	// the tell: `process()` takes ONE SAMPLE, so this object never sees a block boundary and there is
	// nothing a chunking could change. The case could not fail, which makes it decoration rather than
	// a check, however true the property it named.
	//
	// THE PROPERTY WORTH GUARDING IS THE ONE THE FIRMWARE ACTUALLY GETS WRONG, and it is this: its
	// coefficient is applied once per BUFFER, so its time constant is measured in buffers and moves
	// with the host's block size. Ours is measured in seconds. The way to catch a regression to that
	// is to fix the TIME and vary the RATE — a coefficient that had lost its `sampleRate_` term, or
	// gained a block length, gives two different answers here.
	auto levelAfterOneTimeConstant = [](double sr) {
		constexpr float kAttack = 0.010f;
		EnvelopeFollower e;
		e.prepare(sr);
		e.setTimes(kAttack, 1.0f);
		const int n = static_cast<int>(kAttack * sr);
		for (int i = 0; i < n; ++i) {
			e.process(1.0f, 1.0f);
		}
		return e.level(Channel::Left);
	};

	const float expected = 1.0f - std::exp(-1.0f);
	const float at48 = levelAfterOneTimeConstant(48000.0);
	const float at44 = levelAfterOneTimeConstant(44100.0);
	const float at96 = levelAfterOneTimeConstant(96000.0);

	CHECK(at48 == doctest::Approx(expected).epsilon(2e-3));
	CHECK(at44 == doctest::Approx(expected).epsilon(2e-3));
	CHECK(at96 == doctest::Approx(expected).epsilon(2e-3));

	// And they agree with EACH OTHER more tightly than any of them agrees with a wrong answer, which
	// is what makes this a statement about rate-independence rather than three loose checks.
	CHECK(at48 == doctest::Approx(at96).epsilon(1e-3));
}

TEST_CASE("automod envelope: the derivative is positive while swelling and negative while decaying") {
	// THE SIGN IS THE WHOLE POINT — it is what tells the effect a note was just struck rather than
	// merely that it is loud. Asserted at both signs, plus zero in the steady state.
	EnvelopeFollower e = make(0.005f, 0.020f);

	// Rising.
	for (int i = 0; i < static_cast<int>(0.002 * kSr); ++i) {
		e.process(1.0f, 1.0f);
	}
	CHECK(e.derivative(Channel::Left) > 0.0f);

	// Settled: the level has stopped moving, so the derivative decays toward nothing.
	driveFor(e, 1.0f, 0.5);
	CHECK(std::fabs(e.derivative(Channel::Left)) < 1.0f);

	// Falling.
	for (int i = 0; i < static_cast<int>(0.002 * kSr); ++i) {
		e.process(0.0f, 0.0f);
	}
	CHECK(e.derivative(Channel::Left) < 0.0f);
}

TEST_CASE("automod envelope: the derivative is in level per second, so it does not change with the sample rate") {
	// THE UNIT, ASSERTED. A per-SAMPLE delta would be 48000/44100 different between these two runs
	// for the identical musical gesture, which is exactly the trap the per-second scaling exists to
	// avoid — and it is invisible until somebody runs the plugin at a second rate.
	auto peakDerivative = [](double sr) {
		EnvelopeFollower e;
		e.prepare(sr);
		e.setTimes(0.005f, 0.050f);
		float peak = 0.0f;
		const int n = static_cast<int>(0.05 * sr);
		for (int i = 0; i < n; ++i) {
			e.process(1.0f, 1.0f);
			peak = std::max(peak, e.derivative(Channel::Left));
		}
		return peak;
	};

	const float at48 = peakDerivative(48000.0);
	const float at44 = peakDerivative(44100.0);

	CHECK(at48 == doctest::Approx(at44).epsilon(0.02));

	// NON-VACUITY: it is a real number rather than zero at both rates, so agreement means something.
	CHECK(at48 > 1.0f);
}

TEST_CASE("automod envelope: it follows magnitude, so a negative signal is just as loud") {
	// |input|, NOT input. A follower that tracked the raw sample would sit near zero on any
	// symmetric waveform — which is every musical signal — and the effect would never open.
	EnvelopeFollower positive = make(0.001f, 0.100f);
	EnvelopeFollower negative = make(0.001f, 0.100f);

	const float p = driveFor(positive, 0.7f, 0.05);
	const float n = driveFor(negative, -0.7f, 0.05);

	CHECK(p == doctest::Approx(n).epsilon(1e-6));
	CHECK(p == doctest::Approx(0.7f).epsilon(1e-3));
}

TEST_CASE("automod envelope: the two channels are independent") {
	// A follower that shared state would make a hard-panned note open the filter on both ears, which
	// is the opposite of what the stereo offset elsewhere in this effect is for.
	EnvelopeFollower e = make(0.001f, 0.100f);
	for (int i = 0; i < static_cast<int>(0.05 * kSr); ++i) {
		e.process(1.0f, 0.0f);
	}
	CHECK(e.level(Channel::Left) == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(e.level(Channel::Right) < 1e-4f);
}

TEST_CASE("automod envelope: a non-finite sample cannot become a permanent level") {
	// THE HAZARD A FOLLOWER HAS AND A STATELESS STAGE DOES NOT: it REMEMBERS. One NaN would otherwise
	// poison every block after it, and the symptom is "the filter stopped moving" rather than a
	// crash — invisible until somebody goes looking.
	EnvelopeFollower e = make(0.001f, 0.100f);
	driveFor(e, 0.5f, 0.05);
	const float before = e.level(Channel::Left);
	REQUIRE(before > 0.4f);

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	e.process(nan, inf);
	e.process(inf, nan);

	CHECK(std::isfinite(e.level(Channel::Left)));
	CHECK(std::isfinite(e.level(Channel::Right)));
	CHECK(std::isfinite(e.derivative(Channel::Left)));

	// And it recovers — the follower still tracks after the bad samples rather than being stuck.
	const float after = driveFor(e, 0.5f, 0.05);
	CHECK(after == doctest::Approx(0.5f).epsilon(1e-2));
}

TEST_CASE("automod envelope: a zero attack time tracks the input exactly rather than needing a special case") {
	// THE RANGE COLLAPSES GRACEFULLY. Below kEnvMinSeconds the coefficient is 1, which IS "no
	// smoothing" — so the bottom of the range is a usable setting and not a guard against one.
	EnvelopeFollower e = make(0.0f, 0.0f);
	e.process(0.6f, 0.6f);
	CHECK(e.level(Channel::Left) == doctest::Approx(0.6f).epsilon(1e-6));
	e.process(0.1f, 0.1f);
	CHECK(e.level(Channel::Left) == doctest::Approx(0.1f).epsilon(1e-6));
}

TEST_CASE("automod envelope: preparing at a new rate resets rather than carrying a level across") {
	EnvelopeFollower e = make(0.001f, 1.0f);
	driveFor(e, 1.0f, 0.05);
	REQUIRE(e.level(Channel::Left) > 0.9f);

	e.prepare(44100.0);
	CHECK(e.level(Channel::Left) == 0.0f);
	CHECK(e.derivative(Channel::Left) == 0.0f);
}
