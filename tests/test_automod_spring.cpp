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

#include "fx/automod/automod_spring.h"

#include <cmath>
#include <limits>

using namespace feathers::fx::automod;

namespace {

constexpr double kSr = 48000.0;

Spring make(float hz, float zeta) {
	Spring s;
	s.prepare(kSr);
	s.setResponse(hz, zeta);
	return s;
}

/// Drive a spring at a constant target and report the furthest it ever got.
float peakOf(Spring& s, float target, double seconds) {
	const int n = static_cast<int>(seconds * kSr);
	float peak = 0.0f;
	for (int i = 0; i < n; ++i) {
		peak = std::max(peak, s.tick(target));
	}
	return peak;
}

} // namespace

TEST_CASE("automod spring: critically damped reaches its target without ever passing it") {
	// THE DEFINITION OF CRITICAL DAMPING, asserted as the absence of overshoot rather than as a
	// tolerance. This is the comb spring's setting, and an overshoot here would be a delay time
	// swinging past where the LFO asked — audible as a chirp on every LFO corner.
	Spring s = make(10.0f, kCriticalDamping);
	const float peak = peakOf(s, 1.0f, 1.0);

	CHECK(peak <= 1.0f + 1e-4f);
	CHECK(s.position() == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(std::fabs(s.velocity()) < 1e-4f);
}

TEST_CASE("automod spring: under-damped overshoots, and by the ratio the damping predicts") {
	// THE VALUE, NOT "IT BOUNCED". A second-order step response overshoots by exp(-pi*z/sqrt(1-z^2)),
	// which is a closed form — so the test computes what the overshoot MUST be from the damping ratio
	// and checks that number, rather than checking that the peak exceeded the target.
	//
	// This is what makes the case able to tell a correct spring from any bouncing thing.
	constexpr float kZeta = 0.3f;
	Spring s = make(10.0f, kZeta);
	const float peak = peakOf(s, 1.0f, 2.0);

	const float predicted =
	    1.0f + std::exp(-3.14159265f * kZeta / std::sqrt(1.0f - kZeta * kZeta));
	CHECK(peak == doctest::Approx(predicted).epsilon(0.02));

	// It still settles — a bounce that never stops is an oscillator, not a smoother.
	CHECK(s.position() == doctest::Approx(1.0f).epsilon(1e-2));
}

TEST_CASE("automod spring: a lighter damping overshoots further, in the direction the formula says") {
	// THE ORDERING, as a second and independent check on the closed form above. Three damping ratios,
	// three overshoots, monotonically decreasing — which a single-point check cannot see.
	auto overshoot = [](float zeta) {
		Spring s = make(10.0f, zeta);
		return peakOf(s, 1.0f, 2.0) - 1.0f;
	};

	const float light = overshoot(0.15f);
	const float middle = overshoot(0.4f);
	const float heavy = overshoot(0.8f);

	CHECK(light > middle);
	CHECK(middle > heavy);
	// NON-VACUITY: the lightest really does bounce a long way, so the ordering is not three numbers
	// near zero.
	CHECK(light > 0.4f);
	CHECK(heavy < 0.05f);
}

TEST_CASE("automod spring: over-damped crawls — it neither overshoots nor arrives as fast") {
	// THE THIRD REGIME, and the one a case checking only "no overshoot" would confuse with critical.
	// Over-damped is SLOWER than critical, which is the whole reason critical is the useful default.
	Spring critical = make(10.0f, kCriticalDamping);
	Spring over = make(10.0f, 3.0f);

	const int n = static_cast<int>(0.05 * kSr);
	for (int i = 0; i < n; ++i) {
		critical.tick(1.0f);
		over.tick(1.0f);
	}

	CHECK(over.position() < critical.position());
	CHECK(over.position() > 0.0f); // it is moving, just slowly
}

TEST_CASE("automod spring: the natural frequency sets how fast it gets there") {
	// THE FREQUENCY REACHES THE ARITHMETIC. A spring that ignored it would satisfy every case above.
	Spring slow = make(2.0f, kCriticalDamping);
	Spring fast = make(20.0f, kCriticalDamping);

	const int n = static_cast<int>(0.05 * kSr);
	for (int i = 0; i < n; ++i) {
		slow.tick(1.0f);
		fast.tick(1.0f);
	}

	CHECK(fast.position() > slow.position());
	// VALUES, not just an ordering: 50 ms is many cycles for a 20 Hz spring and a fraction of one
	// for a 2 Hz spring, so the two are on opposite sides of the target's midpoint.
	CHECK(fast.position() > 0.9f);
	CHECK(slow.position() < 0.5f);
}

TEST_CASE("automod spring: the response is the same at any sample rate") {
	// THE FIX FOR THE FIRMWARE'S PER-BUFFER COEFFICIENTS, asserted the same way the envelope
	// follower's is: fix the TIME and vary the RATE. A coefficient that had lost its dt would give
	// two different answers here.
	auto after = [](double sr) {
		Spring s;
		s.prepare(sr);
		s.setResponse(10.0f, 0.3f);
		const int n = static_cast<int>(0.05 * sr);
		float peak = 0.0f;
		for (int i = 0; i < n; ++i) {
			peak = std::max(peak, s.tick(1.0f));
		}
		return peak;
	};

	const float at48 = after(48000.0);
	const float at44 = after(44100.0);
	const float at96 = after(96000.0);

	CHECK(at48 == doctest::Approx(at44).epsilon(5e-3));
	CHECK(at48 == doctest::Approx(at96).epsilon(5e-3));
	// NON-VACUITY: it overshot at every rate, so agreement is about a real trajectory.
	CHECK(at48 > 1.2f);
}

TEST_CASE("automod spring: snapTo places it with no momentum, so a rebuilt shape causes no swoop") {
	// WHAT THE FIRMWARE DOES WHEN THE LFO'S WAVEFORM IS REBUILT, and it is not an optimisation: a
	// spring left pointing at the old shape's value lunges across to the new one, so turning a knob
	// produces a sweep the knob did not ask for.
	Spring s = make(10.0f, 0.3f);
	for (int i = 0; i < 2000; ++i) {
		s.tick(1.0f);
	}
	REQUIRE(std::fabs(s.velocity()) > 0.0f);

	s.snapTo(-0.5f);
	CHECK(s.position() == -0.5f);
	CHECK(s.velocity() == 0.0f);

	// And held at its new target it STAYS — no lunge from the snap itself.
	float maxExcursion = 0.0f;
	for (int i = 0; i < 2000; ++i) {
		maxExcursion = std::max(maxExcursion, std::fabs(s.tick(-0.5f) + 0.5f));
	}
	CHECK(maxExcursion < 1e-5f);
}

TEST_CASE("automod spring: it is stable at the extremes of its own declared range") {
	// STABILITY AS A VALUE. Semi-implicit Euler is stable while omega*dt stays well under 1; the
	// range this class declares should be entirely inside that, and the case is what says so rather
	// than the comment. Driven with a square wave, which is the worst input for a resonant system.
	for (float hz : { kSpringFreqMin, 10.0f, kSpringFreqMax }) {
		for (float zeta : { kSpringDampingMin, kCriticalDamping, kSpringDampingMax }) {
			Spring s = make(hz, zeta);
			float peak = 0.0f;
			for (int i = 0; i < 200000; ++i) {
				const float target = ((i / 512) % 2 == 0) ? 1.0f : -1.0f;
				peak = std::max(peak, std::fabs(s.tick(target)));
			}
			CAPTURE(hz);
			CAPTURE(zeta);
			CHECK(std::isfinite(peak));
			// A resonant spring driven at its own frequency can legitimately exceed the target, but
			// not without bound — this is a generous ceiling that a diverging spring blows past
			// immediately.
			CHECK(peak < 100.0f);
		}
	}
}

TEST_CASE("automod spring: a non-finite target or state cannot become permanent") {
	// A SPRING REMEMBERS, so one bad value would be forever. Same hazard, same guard, same reasoning
	// as the envelope follower's.
	// LONG ENOUGH TO HAVE ARRIVED, and the first draft was not: 1000 samples is 20 ms, which a 10 Hz
	// critically damped spring covers only to 0.377. The REQUIRE then failed as a FATAL premise and
	// ended the case before a single assertion about non-finite input could run — the playbook's 8d,
	// where a false premise hides the thing it is a premise for. 8000 samples is 167 ms, comfortably
	// settled.
	Spring s = make(10.0f, kCriticalDamping);
	for (int i = 0; i < 8000; ++i) {
		s.tick(1.0f);
	}
	const float before = s.position();
	REQUIRE(before > 0.5f);

	// A non-finite TARGET is ignored rather than absorbed.
	s.tick(std::numeric_limits<float>::quiet_NaN());
	CHECK(std::isfinite(s.position()));
	s.tick(std::numeric_limits<float>::infinity());
	CHECK(std::isfinite(s.position()));

	// And it still tracks afterwards rather than being stuck. AN ABSOLUTE BOUND, NOT `Approx(0)`:
	// a relative comparison against zero is the trap this suite hit once already — doctest's Approx
	// tests |a-b| < epsilon*(scale + max|.|), which for a zero expectation is a bar that a correct
	// answer can still miss. 250 ms is four cycles of a 10 Hz critically damped spring, which puts
	// the residual around 2.5e-6.
	for (int i = 0; i < 12000; ++i) {
		s.tick(0.0f);
	}
	CHECK(std::fabs(s.position()) < 1e-3f);
}

TEST_CASE("automod spring: the frequency and damping are clamped into the declared range") {
	// THE VALUES, READ BACK. A clamp that silently did nothing would leave the stability case above
	// testing a range the class does not actually enforce.
	Spring s = make(1.0e9f, 1.0e9f);
	CHECK(s.naturalHz() == doctest::Approx(kSpringFreqMax));
	CHECK(s.dampingRatio() == doctest::Approx(kSpringDampingMax));

	s.setResponse(-5.0f, -5.0f);
	CHECK(s.naturalHz() == doctest::Approx(kSpringFreqMin));
	CHECK(s.dampingRatio() == doctest::Approx(kSpringDampingMin));

	// A non-finite damping lands on CRITICAL rather than on a limit — the one sensible default for
	// a smoother, and different from what the numeric clamp would give.
	s.setResponse(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN());
	CHECK(s.naturalHz() == doctest::Approx(kSpringFreqMin));
	CHECK(s.dampingRatio() == doctest::Approx(kCriticalDamping));
}
