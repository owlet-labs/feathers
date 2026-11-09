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

// Behavioural tests for the plugin's own float renderer. The character layer (buildPhiMorphWavetable)
// is pinned bit-for-bit by the golden test; nothing here re-checks it. These assert the properties
// the renderer must hold no matter which zone it is handed: bounded, non-silent, continuous, and a
// crossfade that collapses to its endpoints while doing something non-trivial in between.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/morph_renderer.h"
#include "character/phi_morph.hpp"

#include <array>
#include <cmath>
#include <vector>

namespace {

using owlet::dsp::buildPhiMorphWavetable;
using owlet::dsp::PhiMorphWavetable;

constexpr int kSweepPoints = 4096;
constexpr uint16_t kZones[] = {1, 17, 64, 100};

// The subset whose gain drives them into the clip, so the rail's sign is observable.
constexpr uint16_t kClippedZones[] = {64, 100};

std::vector<float> sweep(const PhiMorphWavetable& table, int points = kSweepPoints) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(points));
	for (int i = 0; i < points; i++) {
		out.push_back(feathers::evalMorphTable(table, static_cast<double>(i) / static_cast<double>(points)));
	}
	return out;
}

float peak(const std::vector<float>& samples) {
	float m = 0.0f;
	for (float s : samples) {
		m = std::max(m, std::abs(s));
	}
	return m;
}

float rms(const std::vector<float>& samples) {
	double acc = 0.0;
	for (float s : samples) {
		acc += static_cast<double>(s) * static_cast<double>(s);
	}
	return static_cast<float>(std::sqrt(acc / static_cast<double>(samples.size())));
}

float maxDeviation(const std::vector<float>& x, const std::vector<float>& y) {
	REQUIRE(x.size() == y.size());
	float m = 0.0f;
	for (size_t i = 0; i < x.size(); i++) {
		m = std::max(m, std::abs(x[i] - y[i]));
	}
	return m;
}

float maxDeviation(const PhiMorphWavetable& x, const PhiMorphWavetable& y) {
	return maxDeviation(sweep(x), sweep(y));
}

// What a naive crossfade would produce: mix the two rendered waveforms instead of their waypoints.
std::vector<float> sampleMix(const PhiMorphWavetable& a, const PhiMorphWavetable& b, float xf) {
	const std::vector<float> sa = sweep(a);
	const std::vector<float> sb = sweep(b);
	std::vector<float> out;
	out.reserve(sa.size());
	for (size_t i = 0; i < sa.size(); i++) {
		out.push_back((1.0f - xf) * sa[i] + xf * sb[i]);
	}
	return out;
}

// The waveform is periodic, so the step from the last sample back to the first counts too.
float maxAdjacentStep(const std::vector<float>& samples) {
	float m = 0.0f;
	for (size_t i = 0; i < samples.size(); i++) {
		const size_t prev = (i == 0) ? samples.size() - 1 : i - 1;
		m = std::max(m, std::abs(samples[i] - samples[prev]));
	}
	return m;
}

} // namespace

TEST_CASE("evalMorphTable output is bounded and non-silent for every zone") {
	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const std::vector<float> samples = sweep(buildPhiMorphWavetable(zone, 0.0f));

		for (float s : samples) {
			REQUIRE(std::isfinite(s));
		}
		// The clip is part of eval: the hardware saturates at Q31 full scale, which normalises to 1.0.
		CHECK(peak(samples) <= 1.0f + 1e-6f);
		CHECK(rms(samples) > 0.05f);
	}
}

TEST_CASE("a clipped zone flat-tops on the positive rail, not just the negative one") {
	// This pins the sign of the clip, which is a different claim from |s| <= 1. The character layer used
	// to clamp waypoint amplitudes against a float literal that rounds one past INT32_MAX, making the
	// conversion out of range: ARM saturated, x86 wrapped to INT32_MIN. A peak-magnitude assertion sees
	// nothing wrong with that, because a waveform diving to -1.0 has the same peak as one flat-topping
	// at +1.0. Before the firmware clamped in double, zone 100 never rose above +0.192 and sat pinned
	// at -1.0 for 730 of these 4096 samples, and zone 64 never reached +1.0 at all.
	for (uint16_t zone : kClippedZones) {
		CAPTURE(zone);
		const std::vector<float> samples = sweep(buildPhiMorphWavetable(zone, 0.0f));

		int atPositiveRail = 0;
		for (float s : samples) {
			if (s >= 1.0f - 1e-6f) {
				atPositiveRail++;
			}
		}
		// Observed 481 for zone 64 and 248 for zone 100 — a plateau, not one grazing sample.
		CHECK(atPositiveRail > 50);
	}
}

TEST_CASE("evalMorphTable is continuous across the cycle") {
	// Segment boundaries break the slope, never the value: a segment ends at exactly the amplitude the
	// next one starts from, and the curvature term vanishes at both ends of a segment. So the step
	// between adjacent samples is limited only by the steepest segment's slope.
	//
	// Zone 17 at offset 0 peaks at 0.585 (asserted below), so it never reaches the clip and the bound
	// measures real slope rather than a clipped plateau. Its observed maximum step over 4096 points is
	// 0.0079. 0.02 leaves ~2.5x headroom — enough for a character-layer nudge that steepens the
	// narrowest segment, not enough to hide a segment that has come adrift from its neighbour.
	constexpr float kMaxStep = 0.02f;

	const PhiMorphWavetable table = buildPhiMorphWavetable(17, 0.0f);
	const std::vector<float> samples = sweep(table);

	REQUIRE(peak(samples) < 0.99f);
	CHECK(maxAdjacentStep(samples) < kMaxStep);

	// A small step is not the same claim as continuity: a jump of 0.015 would pass the bound above and
	// stay that size however finely the cycle is sampled. Quadrupling the sample count has to quarter
	// the step (observed: 0.0079 → 0.0020) — only a continuous waveform does that.
	const float denser = maxAdjacentStep(sweep(table, kSweepPoints * 4));
	CHECK(denser < 0.4f * maxAdjacentStep(samples));
}

TEST_CASE("buildMorphXfade collapses to its source tables at the endpoints") {
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);
	const PhiMorphWavetable b = buildPhiMorphWavetable(17, 0.0f);

	const PhiMorphWavetable atA = feathers::buildMorphXfade(a, b, 0.0f);
	const PhiMorphWavetable atB = feathers::buildMorphXfade(a, b, 1.0f);

	// Absolute, not relative: the waveform crosses zero, where a relative tolerance means nothing.
	for (int i = 0; i < 64; i++) {
		const double p = static_cast<double>(i) / 64.0;
		CAPTURE(p);
		CHECK(std::abs(feathers::evalMorphTable(atA, p) - feathers::evalMorphTable(a, p)) <= 1e-6f);
		CHECK(std::abs(feathers::evalMorphTable(atB, p) - feathers::evalMorphTable(b, p)) <= 1e-6f);
	}
}

TEST_CASE("buildMorphXfade midpoint is not a linear mix of the two waveforms") {
	// Zone 17 at offset 0 has morphAmpOvershoot 0.487 and morphCurvBoost 1.069, so as bank B it drives
	// the excite machinery. A midpoint that merely averaged the two waveforms would mean the
	// waypoint-space crossfade had done nothing. Observed deviation: 0.45.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);
	const PhiMorphWavetable b = buildPhiMorphWavetable(17, 0.0f);

	CHECK(maxDeviation(sweep(feathers::buildMorphXfade(a, b, 0.5f)), sampleMix(a, b, 0.5f)) > 0.1f);
}

TEST_CASE("buildMorphXfade excitation changes the midpoint waveform") {
	// The test above would still pass with the excite machinery deleted, because interpolating
	// waypoints is already not the same as mixing two renders. These two isolate the excitation
	// itself: same bank pair, same crossfade, only the excite factors zeroed.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);

	SUBCASE("amplitude overshoot and curvature boost") {
		// Zone 17 as bank B: overshoot 0.487, curvature boost 1.069, no phase distortion.
		const PhiMorphWavetable b = buildPhiMorphWavetable(17, 0.0f);
		PhiMorphWavetable inert = b;
		inert.morphAmpOvershoot = 0.0f;
		inert.morphCurvBoost = 0.0f;

		// Observed 0.487 at the midpoint, and it has to vanish at the endpoints.
		CHECK(maxDeviation(feathers::buildMorphXfade(a, b, 0.5f), feathers::buildMorphXfade(a, inert, 0.5f)) > 0.1f);
		CHECK(maxDeviation(feathers::buildMorphXfade(a, b, 0.0f), feathers::buildMorphXfade(a, inert, 0.0f)) == 0.0f);
		CHECK(maxDeviation(feathers::buildMorphXfade(a, b, 1.0f), feathers::buildMorphXfade(a, inert, 1.0f)) == 0.0f);
	}

	SUBCASE("phase distortion") {
		// Zone 127 as bank B is the clean isolation: overshoot and curvature boost are both zero, so
		// morphPhaseDistort 0.0414 is the only mechanism left. Without a case like this the phase-warp
		// term goes untested — none of the zones above distort phase at offset 0. Observed 0.560.
		const PhiMorphWavetable b = buildPhiMorphWavetable(127, 0.0f);
		REQUIRE(b.morphAmpOvershoot == 0.0f);
		REQUIRE(b.morphCurvBoost == 0.0f);
		REQUIRE(b.morphPhaseDistort != 0.0f);

		PhiMorphWavetable inert = b;
		inert.morphPhaseDistort = 0.0f;

		CHECK(maxDeviation(feathers::buildMorphXfade(a, b, 0.5f), feathers::buildMorphXfade(a, inert, 0.5f)) > 0.1f);
	}
}

TEST_CASE("buildMorphXfade keeps waypoints ordered inside the cycle") {
	// The phase warp is what could break this: the builder clamps morphPhaseDistort to ±0.15 precisely
	// so the warp stays monotonic. If it ever did not, waypoints would cross and produce a segment with
	// a negative width, whose reciprocal-width interpolation is garbage. Every zone, across the fade.
	// Observed tightest gap: 0.0266.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);

	for (uint16_t zone = 0; zone < 1024; zone++) {
		for (int step = 0; step <= 20; step++) {
			const PhiMorphWavetable m =
			    feathers::buildMorphXfade(a, buildPhiMorphWavetable(zone, 0.0f), static_cast<float>(step) / 20.0f);
			for (int i = 0; i < 7; i++) {
				const float prev = (i == 0) ? 0.0f : m.phases[i - 1];
				if (!(m.phases[i] > prev) || !(m.phases[i] < 1.0f)) {
					// Reporting per waypoint would mean 143k assertions; only a failure needs the detail.
					FAIL("zone " << zone << " xf " << (static_cast<float>(step) / 20.0f) << " waypoint " << i
					             << " out of order: " << prev << " -> " << m.phases[i]);
				}
			}
		}
	}
}

TEST_CASE("buildMorphXfade and evalMorphTable are deterministic") {
	const PhiMorphWavetable first =
	    feathers::buildMorphXfade(buildPhiMorphWavetable(64, 0.0f), buildPhiMorphWavetable(17, 0.0f), 0.375f);
	const PhiMorphWavetable second =
	    feathers::buildMorphXfade(buildPhiMorphWavetable(64, 0.0f), buildPhiMorphWavetable(17, 0.0f), 0.375f);

	const std::vector<float> a = sweep(first);
	const std::vector<float> b = sweep(second);

	REQUIRE(a.size() == b.size());
	for (size_t i = 0; i < a.size(); i++) {
		CAPTURE(i);
		REQUIRE(a[i] == b[i]);
	}
}

TEST_CASE("blending N recipes generalises the two-sound crossfade") {
	// THE DECISION: recipe blending. One oscillator still plays the result, so what you
	// hear between sounds is a shape that is neither rather than both at once.
	//
	// WHAT REDUCES IS THE RECIPE AVERAGE, AND THAT IS THE CLAIM WORTH GUARDING: a weighted average of
	// waypoints at weights (1-t, t) IS the lerp the crossfade performs. Checked against a pair whose
	// EXCITE FACTORS ARE ZERO, so the comparison is about the blend rather than about the shaping —
	// zone 600 has overshoot, curvature boost and phase distortion all at zero, which is why it is
	// here.
	//
	// The excite shaping deliberately does NOT reduce; see the case below for what it does instead.
	// BOTH sounds have to be excite-free, not just the target: the blend weights the factors across
	// everything present, so a partner with a live curvature boost puts shaping back in. That caught a
	// draft of this test which used one plain zone and one lively one, and reported a mechanism fault
	// that was really a fixture fault. Zones 588-593 are a contiguous run with all three at zero.
	const PhiMorphWavetable a = buildPhiMorphWavetable(588, 0.0f);
	const PhiMorphWavetable plain = buildPhiMorphWavetable(593, 0.0f);
	for (const PhiMorphWavetable& t : { a, plain }) {
		REQUIRE(t.morphAmpOvershoot == 0.0f);
		REQUIRE(t.morphCurvBoost == 0.0f);
		REQUIRE(t.morphPhaseDistort == 0.0f);
	}
	const std::array<PhiMorphWavetable, 2> pair{a, plain};

	for (int step = 0; step <= 20; ++step) {
		const double t = static_cast<double>(step) / 20.0;
		const std::array<double, 2> weights{1.0 - t, t};
		const PhiMorphWavetable blended = feathers::buildMorphBlend(pair, weights);
		const PhiMorphWavetable faded = feathers::buildMorphXfade(a, plain, static_cast<float>(t));
		CAPTURE(t);
		for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
			CHECK(blended.phases[i] == doctest::Approx(faded.phases[i]).epsilon(1e-6));
			CHECK(blended.amplitudes[i] == doctest::Approx(faded.amplitudes[i]).epsilon(1e-6));
		}
		for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
			CHECK(blended.curvatures[i] == doctest::Approx(faded.curvatures[i]).epsilon(1e-6));
		}
		CHECK(blended.endpointAmp == doctest::Approx(faded.endpointAmp).epsilon(1e-6));
	}
}

TEST_CASE("the excitation is present, smooth, and deliberately not the two-way rule") {
	// THE SHAPING DOES NOT REDUCE, ON PURPOSE, and this is where that is stated rather than discovered.
	//
	// The two-way rule takes its factors from the TARGET and pushes the amplitude from A past B — both
	// of which need a direction of travel that a constellation does not have. So: the factors are
	// weighted like everything else (smooth, no tie to break), and the directional amplitude overshoot
	// is not applied at all, because every candidate direction flips exactly on the midline between
	// two sounds, which is where excite is strongest and a jump would be loudest.
	//
	// Zone 300 carries all three factors, so a build that dropped the shaping entirely fails here.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);
	const PhiMorphWavetable b = buildPhiMorphWavetable(300, 0.0f);
	REQUIRE(b.morphCurvBoost > 1.0f);
	const std::array<PhiMorphWavetable, 2> pair{a, b};

	const std::array<double, 2> parked{1.0, 0.0};
	const std::array<double, 2> mixed{0.5, 0.5};
	const PhiMorphWavetable atA = feathers::buildMorphBlend(pair, parked);
	const PhiMorphWavetable middle = feathers::buildMorphBlend(pair, mixed);

	// Parked on a sound, the excitation is off and the recipe is that sound's.
	for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
		CHECK(atA.curvatures[i] == doctest::Approx(a.curvatures[i]).epsilon(1e-6));
	}

	// Mixed, the curvature is boosted past the plain average — the shapes that exist only while
	// morphing. Compared against the average rather than against either sound, so this is the BOOST
	// being measured and not the blend.
	double boosted = 0.0;
	double plain = 0.0;
	for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
		boosted += std::abs(middle.curvatures[i]);
		plain += std::abs(0.5 * a.curvatures[i] + 0.5 * b.curvatures[i]);
	}
	CHECK(boosted > plain * 1.5);
}

TEST_CASE("all of one sound is exactly that sound") {
	// Standing on an anchor. The excite term is what could break this — a blend that kept exciting at
	// full weight would mean no sound in the constellation was ever purely itself, and there would be
	// nowhere in the space to stand and hear what you placed.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);
	const PhiMorphWavetable b = buildPhiMorphWavetable(600, 0.0f);
	const PhiMorphWavetable c = buildPhiMorphWavetable(300, 0.25f);
	const std::array<PhiMorphWavetable, 3> three{a, b, c};

	for (size_t on = 0; on < 3; ++on) {
		std::array<double, 3> weights{0.0, 0.0, 0.0};
		weights[on] = 1.0;
		const PhiMorphWavetable blended = feathers::buildMorphBlend(three, weights);
		CAPTURE(on);
		for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
			CHECK(blended.phases[i] == doctest::Approx(three[on].phases[i]).epsilon(1e-6));
			CHECK(blended.amplitudes[i] == doctest::Approx(three[on].amplitudes[i]).epsilon(1e-6));
		}
	}
}

TEST_CASE("three sounds evenly shared is none of them") {
	// The point of a constellation rather than a list: the middle is its own place. If an even blend
	// came out as one of its neighbours, the space between three sounds would be a corridor rather
	// than a room.
	const PhiMorphWavetable a = buildPhiMorphWavetable(64, 0.0f);
	const PhiMorphWavetable b = buildPhiMorphWavetable(600, 0.0f);
	const PhiMorphWavetable c = buildPhiMorphWavetable(300, 0.25f);
	const std::array<PhiMorphWavetable, 3> three{a, b, c};
	const std::array<double, 3> even{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
	const PhiMorphWavetable middle = feathers::buildMorphBlend(three, even);

	for (const PhiMorphWavetable& one : three) {
		bool differs = false;
		for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
			if (std::abs(middle.amplitudes[i] - one.amplitudes[i]) > 1e-3f) {
				differs = true;
			}
		}
		CHECK(differs);
	}

	// And the excitation is real there: an even three-way share is more mixed than a two-way midpoint,
	// so the shapes that exist only while morphing are at their strongest in the middle of a room.
	const std::array<double, 3> lopsided{0.9, 0.05, 0.05};
	const PhiMorphWavetable corner = feathers::buildMorphBlend(three, lopsided);
	double middleSpread = 0.0;
	double cornerSpread = 0.0;
	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		middleSpread += std::abs(middle.amplitudes[i]);
		cornerSpread += std::abs(corner.amplitudes[i] - a.amplitudes[i]);
	}
	CHECK(cornerSpread < middleSpread);
}
