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


#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/constellation.h"
#include "dsp/puck_motion.h"

#include <array>
#include <cmath>
#include <vector>

// The blend's geometry, with no sound anywhere near it. Weights and positions are arithmetic, and
// arithmetic is the part that can be wrong in a way nobody hears until it is a patch that used to
// work.

using namespace feathers;

namespace {

std::vector<double> weightsAt(const std::vector<AnchorPoint>& anchors, AnchorPoint puck) {
	std::vector<double> out(anchors.size(), 0.0);
	blendWeights(anchors, puck, out);
	return out;
}

const std::vector<AnchorPoint> kTwo = { { -0.5, 0.0 }, { 0.5, 0.0 } };

} // namespace

TEST_CASE("two sounds on a segment blend exactly as Path always has") {
	// THE COMPATIBILITY CLAIM, and the reason the falloff exponent is 1 rather than the 2 the design
	// doc names. Path over two anchors has been a linear crossfade since v1, and normalised
	// inverse-distance on a segment IS that crossfade — not approximately, but as an identity:
	// weights (1-t, t) for a puck t of the way along.
	//
	// If this fails, every existing patch with Path anywhere but its ends has changed character.
	for (int step = 0; step <= 100; ++step) {
		const double path = static_cast<double>(step) / 100.0;
		const AnchorPoint puck = tourPoint(kTwo, path);
		const std::vector<double> w = weightsAt(kTwo, puck);
		CAPTURE(path);
		CHECK(w[0] == doctest::Approx(1.0 - path).epsilon(1e-12));
		CHECK(w[1] == doctest::Approx(path).epsilon(1e-12));
	}
}

TEST_CASE("the ends of the tour are the sounds themselves, exactly") {
	// Path 0 and Path 1 have to put the puck ON an anchor to the bit, not near it. Everything about a
	// sound being purely itself somewhere rests on this: its weight is 1 there, and — once travel is
	// wired — its distance from home is zero there, which is what makes it the character it says it is.
	CHECK(tourPoint(kTwo, 0.0) == kTwo[0]);
	CHECK(tourPoint(kTwo, 1.0) == kTwo[1]);

	const std::vector<AnchorPoint> four = { { -1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 0.0 }, { 0.0, -1.0 } };
	CHECK(tourPoint(four, 0.0) == four[0]);
	CHECK(tourPoint(four, 1.0) == four[3]);
	// And every interior anchor is reached exactly, at its own even share of the knob.
	CHECK(tourPoint(four, 1.0 / 3.0) == four[1]);
	CHECK(tourPoint(four, 2.0 / 3.0) == four[2]);

	for (size_t i = 0; i < four.size(); ++i) {
		const std::vector<double> w = weightsAt(four, four[i]);
		CAPTURE(i);
		CHECK(w[i] == 1.0);
		double others = 0.0;
		for (size_t j = 0; j < four.size(); ++j) {
			if (j != i) {
				others += w[j];
			}
		}
		CHECK(others == 0.0);
	}
}

TEST_CASE("weights are a share of one, wherever the puck is") {
	// Anything else is a volume change disguised as a blend. Checked off the tour as well as on it,
	// since Pos X/Y take the puck places the tour never goes.
	const std::vector<AnchorPoint> five = {
		{ -0.5, 0.0 }, { 0.5, 0.0 }, { 0.0, 0.75 }, { -0.75, -0.3 }, { 0.6, -0.6 },
	};
	for (int x = -20; x <= 20; ++x) {
		for (int y = -20; y <= 20; ++y) {
			const AnchorPoint puck { static_cast<double>(x) * 0.1, static_cast<double>(y) * 0.1 };
			const std::vector<double> w = weightsAt(five, puck);
			double total = 0.0;
			for (double one : w) {
				CHECK(one >= 0.0);
				total += one;
			}
			CAPTURE(puck.x);
			CAPTURE(puck.y);
			CHECK(total == doctest::Approx(1.0).epsilon(1e-12));
		}
	}
}

TEST_CASE("the nearest sound dominates, and it is the nearest one that does") {
	// Local support, which is the whole reason the design chose distance weighting over a tour
	// parameter: standing next to a sound should mean hearing that sound, whichever one it happens to
	// be and wherever the tour thinks you are.
	const std::vector<AnchorPoint> five = {
		{ -0.5, 0.0 }, { 0.5, 0.0 }, { 0.0, 0.75 }, { -0.75, -0.3 }, { 0.6, -0.6 },
	};
	for (size_t i = 0; i < five.size(); ++i) {
		const AnchorPoint puck { five[i].x + 0.02, five[i].y - 0.01 };
		const std::vector<double> w = weightsAt(five, puck);
		const auto loudest = static_cast<size_t>(std::max_element(w.begin(), w.end()) - w.begin());
		CAPTURE(i);
		CHECK(loudest == i);
		CHECK(w[i] > 0.8);
	}
}

TEST_CASE("a sound switched off is not there at all") {
	// Enabling is the caller's business — this file only ever sees the sounds that are on — so the
	// claim being checked is that the geometry says nothing about how many there are. Two anchors
	// blend the same whether they are the only two or the survivors of eight.
	const std::vector<AnchorPoint> survivors = { { -0.5, 0.0 }, { 0.5, 0.0 } };
	for (int step = 0; step <= 20; ++step) {
		const double path = static_cast<double>(step) / 20.0;
		CHECK(weightsAt(survivors, tourPoint(survivors, path))
		      == weightsAt(kTwo, tourPoint(kTwo, path)));
	}
}

TEST_CASE("coincident anchors share rather than divide by zero") {
	// A user can drag two sounds onto each other, and the honest answer is half each. The alternative
	// is an infinity into a normalisation, which comes out as a NaN per voice — silence at best.
	const std::vector<AnchorPoint> stacked = { { 0.25, -0.4 }, { 0.25, -0.4 }, { -0.8, 0.1 } };
	const std::vector<double> w = weightsAt(stacked, { 0.25, -0.4 });
	CHECK(w[0] == 0.5);
	CHECK(w[1] == 0.5);
	CHECK(w[2] == 0.0);
}

TEST_CASE("one sound, no sounds") {
	// The degenerate constellations, which a user reaches by switching things off rather than by doing
	// anything strange.
	const std::vector<AnchorPoint> one = { { 0.3, -0.2 } };
	CHECK(tourPoint(one, 0.0) == one[0]);
	CHECK(tourPoint(one, 0.5) == one[0]);
	CHECK(tourPoint(one, 1.0) == one[0]);
	CHECK(weightsAt(one, { -0.9, 0.9 })[0] == 1.0);

	CHECK(tourPoint({}, 0.5) == AnchorPoint {});
	std::array<double, 1> untouched { 42.0 };
	blendWeights({}, { 0.0, 0.0 }, untouched);
	CHECK(untouched[0] == 42.0); // nothing to weigh, so nothing written
}

TEST_CASE("path is clamped rather than extrapolated") {
	// A host can automate past the ends, and a tour that ran on would put the puck outside the
	// constellation entirely — every sound quiet, which is not a sound anyone asked for.
	CHECK(tourPoint(kTwo, -5.0) == kTwo[0]);
	CHECK(tourPoint(kTwo, 5.0) == kTwo[1]);
}

// ---------------------------------------------------------------------------
// The one quantity in this engine that is not a function of position.
// ---------------------------------------------------------------------------

TEST_CASE("a parked puck has exactly no velocity, forever") {
	// THE CLAIM EVERY BLUE-SHIFT EFFECT RESTS ON, and it has to be exact rather than small: §2b.1's
	// static purity says a parked patch is a pure function of position, and "the velocity term is
	// 0.0000001" would make that a tolerance instead of a guarantee. The difference of a position with
	// itself is ±0.0 and a pole fed exact zeros stays where it started, so this is arithmetic rather
	// than a threshold.
	PuckMotion motion;
	motion.setSampleRate(48000.0);
	motion.setPosition({ 0.4, -0.7 }); // parked away from the origin, which is where a lurch would show

	for (int i = 0; i < 500; ++i) {
		motion.advance(64);
		CHECK(motion.velocity().x == 0.0);
		CHECK(motion.velocity().y == 0.0);
	}
}

TEST_CASE("a moving puck has a velocity, and it points where the puck is going") {
	PuckMotion motion;
	motion.setSampleRate(48000.0);
	motion.setPosition({ 0.0, 0.0 });
	motion.advance(PuckMotion::kTickSamples); // the first tick only learns where the puck is

	// A tenth of a unit rightwards every tick: 0.1 / (64/48000) = 75 units per second, which the
	// smoother approaches rather than reaching, since that is what smoothing is.
	double x = 0.0;
	for (int i = 0; i < 400; ++i) {
		x += 0.1;
		motion.setPosition({ x, 0.0 });
		motion.advance(PuckMotion::kTickSamples);
	}
	CHECK(motion.velocity().x == doctest::Approx(75.0).epsilon(0.01));
	CHECK(motion.velocity().y == 0.0); // a gesture in one axis stays in that axis

	// AND IT DECAYS BACK TO REST once the gesture stops — which is what makes blue shift something you
	// hear while you move rather than a mode you leave switched on behind you. Half a second is about
	// seven time constants, so what is left is a thousandth of the gesture; a second and a half is
	// enough that nothing measurable is.
	for (int i = 0; i < 400; ++i) { // 400 ticks ≈ 0.53 s
		motion.advance(PuckMotion::kTickSamples);
	}
	CHECK(std::abs(motion.velocity().x) < 75.0 * 1e-3);
	for (int i = 0; i < 1100; ++i) {
		motion.advance(PuckMotion::kTickSamples);
	}
	CHECK(std::abs(motion.velocity().x) < 1e-6);
}

TEST_CASE("the velocity does not depend on how the host chopped its buffer") {
	// The hard constraint (plan §6), and time enters this engine nowhere else — so this class is the
	// one place a buffer size could reach the audio. BIT-EXACT, not close: the same automation has to
	// render the same samples, and a velocity a few ulps apart builds a different wavetable.
	//
	// This is why the smoother steps on a fixed sample grid instead of once per block with a dt taken
	// from the block: exp(-n₁/τ)·exp(-n₂/τ) and exp(-(n₁+n₂)/τ) are one number in algebra and two in
	// floating point.
	constexpr long long kTotal = 40960; // the same DURATION for every chopping, or the comparison is
	                                    // between two different performances rather than two chops of one
	const auto run = [](const std::vector<int>& chopping) {
		PuckMotion motion;
		motion.setSampleRate(48000.0);
		// A gesture written in absolute sample positions, so every chopping sees the same one. The
		// position changes only at multiples of 512, which is what a parameter event looks like from
		// here: constant between events.
		long long at = 0;
		size_t next = 0;
		while (at < kTotal) {
			const auto n = static_cast<int>(std::min<long long>(chopping[next++ % chopping.size()],
			                                                   kTotal - at));
			motion.setPosition({ static_cast<double>(at / 512) * 0.05,
			                     static_cast<double>(at / 512) * -0.02 });
			motion.advance(n);
			at += n;
		}
		return motion.velocity();
	};

	const AnchorPoint reference = run({ 512 });
	CHECK(reference.x != 0.0); // a gesture that never moved would pass every chopping trivially

	for (const std::vector<int>& chopping : { std::vector<int> { 1 }, std::vector<int> { 37 },
		                                      std::vector<int> { 128, 384 },
		                                      std::vector<int> { 1, 511, 63, 449 } }) {
		const AnchorPoint got = run(chopping);
		CHECK(got.x == reference.x);
		CHECK(got.y == reference.y);
	}
}

TEST_CASE("the first tick learns the position rather than lurching towards it") {
	// A patch loading with the puck away from centre must not read as an infinite swipe from the
	// origin — the loudest possible sound at the quietest possible moment.
	PuckMotion motion;
	motion.setSampleRate(48000.0);
	motion.setPosition({ -0.9, 0.8 });
	motion.advance(PuckMotion::kTickSamples * 4);
	CHECK(motion.velocity().x == 0.0);
	CHECK(motion.velocity().y == 0.0);

	// And a reset forgets the gesture without forgetting the puck: the next tick does not read the
	// reset itself as a jump.
	motion.setPosition({ 0.0, 0.0 });
	motion.advance(PuckMotion::kTickSamples);
	REQUIRE(motion.velocity().x != 0.0);
	motion.reset();
	CHECK(motion.velocity().x == 0.0);
	motion.advance(PuckMotion::kTickSamples * 2);
	CHECK(motion.velocity().x == 0.0);
}

TEST_CASE("the gesture is allowed to end") {
	// A GEOMETRIC DECAY DOES NOT REACH ZERO. Left alone, the velocity walks into the denormals and
	// sticks 28 of them above zero — `v * 0.982` rounds back to `v` once the step is below half the gap
	// between adjacent denormals — so every motion effect stays switched on for the rest of the
	// session, the character is rebuilt on every tick for about a minute, and the surviving bits depend
	// on whether the HOST runs its audio thread with flush-to-zero.
	//
	// The gate has to RE-CLOSE, exactly, and this is what says so.
	PuckMotion motion;
	motion.setSampleRate(48000.0);
	motion.setPosition({ 0.0, 0.0 });
	motion.advance(PuckMotion::kTickSamples);

	double x = 0.0;
	for (int i = 0; i < 200; ++i) {
		x += 0.1;
		motion.setPosition({ x, x * 0.5 });
		motion.advance(PuckMotion::kTickSamples);
	}
	REQUIRE(motion.velocity().x > 1.0); // a gesture happened, so there is something to decay

	// Two and a half seconds of standing still. The threshold is reached at about 1.9 s from a gesture
	// this size, so this is the settling time plus room rather than a number tuned to pass.
	const int ticks = static_cast<int>(2.5 * 48000.0 / PuckMotion::kTickSamples);
	for (int i = 0; i < ticks; ++i) {
		motion.advance(PuckMotion::kTickSamples);
	}
	CHECK(motion.velocity().x == 0.0);
	CHECK(motion.velocity().y == 0.0);

	// EXACTLY zero, not nearly — which is the whole difference, since every blue-shift term is this
	// multiplied by something and "almost zero" times a field is still not zero. A denormal would pass
	// any tolerance test ever written and fail this one.
	const bool exactlyZero = motion.velocity().x == 0.0 && motion.velocity().y == 0.0;
	CHECK(exactlyZero);
}
