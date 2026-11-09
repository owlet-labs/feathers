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

// Behavioural tests for the plugin's float staircase evaluator. The character layer
// (buildPhiStairParams) is pinned field-for-field by the golden test; nothing here re-checks it.
// These are the first coverage anywhere of what those params MEAN as a waveform — the firmware's own
// render half has none — so several of them pin geometry that is easy to get subtly wrong: which end
// of a step the riser sits at, what the first step rises from, and which of the discontinuities in a
// zone sweep belong to the evaluator and which are the character layer's own.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_stair.hpp"
#include "dsp/stair_renderer.h"

#include <cmath>
#include <vector>

namespace {

using owlet::dsp::buildPhiStairParams;
using owlet::dsp::kPhiStairSlots;
using owlet::dsp::kPhiStairSteps;
using owlet::dsp::PhiStairParams;
using feathers::buildStairTable;
using feathers::evalStairSlots;
using feathers::evalStairTable;
using feathers::StairTable;

constexpr int kSweepPoints = 4096;

// The five zones the task named, plus one per pattern family. family = min(7, zone >> 7), so the
// named five are all BRICK; without the second half, Terrace, Ramp, Mesa, Pylon, Glyph, Shard and
// Teeth would go unrendered by every test in this file.
constexpr uint16_t kZones[] = {0, 17, 64, 100, 127, 200, 300, 450, 600, 750, 900, 1023};

// Zones whose own slope is well clear of the 0.001 hard-edge threshold, so the waveform they
// describe is continuous and a step bound measures riser steepness rather than a step edge.
constexpr uint16_t kSlopedZones[] = {300, 600, 900};

std::vector<float> sweep(const StairTable& table, int points = kSweepPoints) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(points));
	for (int i = 0; i < points; i++) {
		out.push_back(evalStairTable(table, static_cast<double>(i) / static_cast<double>(points)));
	}
	return out;
}

std::vector<float> sweep(const PhiStairParams& p, int points = kSweepPoints) {
	return sweep(buildStairTable(p), points);
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

// The waveform is periodic, so the step from the last sample back to the first counts too.
float maxAdjacentStep(const std::vector<float>& samples) {
	float m = 0.0f;
	for (size_t i = 0; i < samples.size(); i++) {
		const size_t prev = (i == 0) ? samples.size() - 1 : i - 1;
		m = std::max(m, std::abs(samples[i] - samples[prev]));
	}
	return m;
}

// Mean |difference| between two waveforms, sampled off-grid so a feature narrower than the sample
// spacing still contributes its share rather than being hit or missed by luck. This is the metric
// that answers "did the sound jump", and for a staircase it is the only one that can: a step of
// vanishing width is a full-height excursion however finely you look at it, so the largest
// difference between two waveforms stays large no matter how close their params are.
double meanAbsDiff(const StairTable& a, const StairTable& b, int points = 1 << 18) {
	double acc = 0.0;
	for (int i = 0; i < points; i++) {
		const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(points);
		acc += std::abs(static_cast<double>(evalStairTable(a, u)) - static_cast<double>(evalStairTable(b, u)));
	}
	return acc / static_cast<double>(points);
}

// Largest move of any single param, the scalar this file means by "the params delta".
float paramsDelta(const PhiStairParams& a, const PhiStairParams& b) {
	float m = std::abs(a.slope - b.slope);
	for (int i = 0; i < kPhiStairSteps; i++) {
		m = std::max(m, std::abs(a.heights[i] - b.heights[i]));
		m = std::max(m, std::abs(a.widths[i] - b.widths[i]));
	}
	return m;
}

// First step the character layer has faded out, i.e. the one that fades back in as the step count
// rises through the next integer.
int firstInactiveStep(const PhiStairParams& p) {
	for (int i = 0; i < kPhiStairSteps; i++) {
		if (p.widths[i] == 0.0f) {
			return i;
		}
	}
	return -1;
}

} // namespace

TEST_CASE("evalStairTable output is bounded and non-silent for every zone") {
	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const std::vector<float> samples = sweep(buildPhiStairParams(zone, 0.0f));

		for (float s : samples) {
			REQUIRE(std::isfinite(s));
		}
		// The table build's loudness rule caps the slot peak at 0.95 of its half-scale ceiling, which
		// normalises to 0.475. Only a step too narrow for the 128-slot grid to sample can push a
		// continuously evaluated waveform past that, and then the clip catches it — see the dedicated
		// case below. Observed peak across these zones: 0.475.
		CHECK(peak(samples) <= 1.0f + 1e-6f);
		// The same rule targets an RMS of 0.55 of that ceiling, so a zone that reaches it sits at 0.275.
		// Zones the peak cap binds first sit lower; observed minimum 0.215 at zone 300.
		CHECK(rms(samples) > 0.05f);
	}
}

TEST_CASE("the staircase is continuous in phase wherever the risers have length") {
	// A riser ramps from the previous step's level to this one's, and the hold that follows sits at the
	// level the next riser starts from, so with a slope every step edge is joined and only the slope
	// breaks. The bound therefore measures the steepest riser, which is set by the narrowest step.
	//
	// Observed maxima over 4096 points: 0.00245 (zone 300), 0.00222 (zone 600), 0.00263 (zone 900).
	// 0.01 leaves ~4x for a character-layer nudge that narrows a step, and is still 200x below the
	// 0.55-0.93 a hard edge produces, so it cannot be passed by a waveform that has come apart.
	constexpr float kMaxStep = 0.01f;

	for (uint16_t zone : kSlopedZones) {
		CAPTURE(zone);
		const PhiStairParams p = buildPhiStairParams(zone, 0.0f);
		REQUIRE(p.slope > 0.001f);
		REQUIRE(firstInactiveStep(p) != 0);

		const std::vector<float> samples = sweep(p);
		CHECK(maxAdjacentStep(samples) < kMaxStep);

		// A small step is not the same claim as continuity: a jump of 0.005 would pass the bound above
		// and stay that size however finely the cycle is sampled. Quadrupling the sample count has to
		// quarter the step — observed ratio 0.2500 for all three, which is a ramp and nothing else.
		const float denser = maxAdjacentStep(sweep(p, kSweepPoints * 4));
		CHECK(denser < 0.4f * maxAdjacentStep(samples));
	}
}

TEST_CASE("slope sweeps the risers from hard edges to fully ramped") {
	// The slope dimension is the whole point of the character, and nothing else in this file pins its
	// direction: an evaluator that put the riser at the END of a step, or ramped towards the previous
	// level instead of away from it, would still be continuous and still be bounded.
	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const PhiStairParams base = buildPhiStairParams(zone, 0.0f);

		PhiStairParams hard = base;
		hard.slope = 0.0f;
		PhiStairParams ramped = base;
		ramped.slope = 0.9f; // The ceiling buildPhiStairParams can reach.

		// Every zone has at least one step edge worth 0.37 or more of full scale at slope 0; the
		// smallest observed here is 0.371 at zone 200 and the largest 0.930 at zone 750.
		CHECK(maxAdjacentStep(sweep(hard)) > 0.3f);

		// At the other end no edge survives at all. Observed maximum 0.0267, at zone 127 — the 11-step
		// zone whose narrowest step is 0.006 of the cycle, so its risers are the steepest here. Every
		// other zone lands under 0.004. 0.05 keeps ~1.9x over that worst case.
		CHECK(maxAdjacentStep(sweep(ramped)) < 0.05f);
	}
}

TEST_CASE("a slope below the build's own threshold is a hard edge, not a very steep ramp") {
	// The original fixed-point Stair table build ramps only when slope exceeds 0.001, so that constant is part of the
	// character rather than a guard against dividing by something small. It is live: 74 of the 4096
	// zone x {0, 0.25, 1.618, 0.5} pairs have a slope in (0, 0.001], among them zone 39 at offset 0
	// (0.000464). Treating those as ramps would cost nothing audible, but it would put the plugin and
	// the hardware on opposite sides of a documented threshold, so it is worth one case.
	{
		const PhiStairParams real = buildPhiStairParams(39, 0.0f);
		REQUIRE(real.slope > 0.0f);
		REQUIRE(real.slope <= 0.001f);
	}

	// Sampling has to be dense enough to resolve a riser 0.0011 of a step wide, or a real ramp and a
	// real edge look identical. At 4096 points both of these read as ~0.4-0.58; it takes 2^20 to tell
	// them apart, which is the point — an edge does not care how closely you look.
	constexpr int kDense = 1 << 20;
	const PhiStairParams base = buildPhiStairParams(600, 0.0f);

	PhiStairParams below = base;
	below.slope = 0.0009f;
	PhiStairParams above = base;
	above.slope = 0.0011f;

	// Observed 0.583250 at both 4096 and 2^20 points: the same jump however finely it is sampled.
	CHECK(maxAdjacentStep(sweep(below, kDense)) > 0.3f);
	// Observed 0.002444, down from 0.396945 at 4096 — the signature of a ramp being resolved.
	CHECK(maxAdjacentStep(sweep(above, kDense)) < 0.01f);
}

TEST_CASE("the first step rises from the last, so the cycle wraps like any interior edge") {
	// The wrap is the one edge with no neighbour to read: the original fixed-point Stair table build sources step 0's riser
	// from the last step that still has width, which is what makes the cycle join. Reading heights[15]
	// (dead, faded out) or heights[0] (a riser to itself, i.e. no riser) instead would both leave a
	// hard edge at phase 0 while every interior edge stayed smooth — invisible to a bound that takes a
	// maximum over the whole cycle if some interior edge happens to be steeper.
	constexpr double kEps = 1e-9;

	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const PhiStairParams p = buildPhiStairParams(zone, 0.0f);
		if (p.slope <= 0.001f) {
			continue; // Hard-edged zone: every edge is a jump, including this one, by design.
		}
		const StairTable table = buildStairTable(p);

		// Observed 0 for four of the six sloped zones and 4.5e-8 for the other two, which is the float
		// scale factor's last bit, not a seam.
		const float before = evalStairTable(table, 1.0 - kEps);
		const float after = evalStairTable(table, kEps);
		CHECK(std::abs(before - after) < 1e-6f);
	}
}

TEST_CASE("a step fading in at zero width is silent only when there is no riser") {
	// The fractional-count fade is the character layer's promise that a step never pops in. It keeps
	// that promise for the waveform's own body — a step of width w occupies w of the cycle, so the
	// sound it adds vanishes with it — but only while slope is 0. The moment there is a riser, the
	// promise breaks at the wrap: step 0 rises from the last step that has ANY width, and "any" is a
	// discrete test, so the instant the fading step's width leaves 0 the riser's origin jumps from one
	// height to the next. The jump is in the original fixed-point Stair table build, not here, and this case pins both halves
	// so a future firmware bump that fixes it fails loudly rather than silently changing the sound.
	const PhiStairParams base = buildPhiStairParams(600, 0.0f);
	const int fading = firstInactiveStep(base);
	REQUIRE(fading > 0);

	SUBCASE("hard edges: the fade is continuous") {
		PhiStairParams hard = base;
		hard.slope = 0.0f;
		const StairTable at0 = buildStairTable(hard);

		// Observed mean |difference| per unit of added width: 0.1294, 0.1299, 0.1304 — flat, so the
		// difference is proportional to the width and goes to zero with it.
		double previous = 0.0;
		for (double w : {1e-2, 1e-3, 1e-4}) {
			PhiStairParams grown = hard;
			grown.widths[fading] = static_cast<float>(w);
			const double diff = meanAbsDiff(at0, buildStairTable(grown));
			CAPTURE(w);
			CHECK(diff < 0.2 * w);
			if (previous > 0.0) {
				CHECK(diff < 0.2 * previous); // Ten times narrower has to be far quieter, not equally loud.
			}
			previous = diff;
		}
	}

	SUBCASE("with a riser: the wrap origin jumps and the difference does not vanish") {
		REQUIRE(base.slope > 0.001f);
		const StairTable at0 = buildStairTable(base);

		// Converges to 0.00751 rather than to 0 as the added width shrinks by four decades. The size is
		// the wrap riser's origin moving by |heights[fading] - heights[fading - 1]| = 0.292, spread over
		// the riser's share of step 0.
		REQUIRE(std::abs(base.heights[fading] - base.heights[fading - 1]) > 0.2f);
		for (double w : {1e-3, 1e-4, 1e-5}) {
			PhiStairParams grown = base;
			grown.widths[fading] = static_cast<float>(w);
			CAPTURE(w);
			CHECK(meanAbsDiff(at0, buildStairTable(grown)) > 0.005);
		}
	}
}

TEST_CASE("a step-count crossing pops in the character layer, not in the evaluator") {
	// Task brief expected the waveform difference across a step-count fade to shrink with the params
	// difference. It does not, and the reason is upstream of this file: at zone 64 the step count
	// crosses 7 between phase offsets 0.0059042275 and 0.0059042280 (adjacent floats), and BRICK's
	// height pattern quantises t * 16 to an integer, which for step 3 lands on 8 at exactly that count.
	// So heights[3] flips between -0.8 and +0.8 there. Closing in on the crossing from both sides, the
	// params difference converges
	// to 1.607 instead of to 0. Pattern families with a threshold in t (Brick, Mesa, Pylon, Glyph,
	// Teeth) all behave this way; the fade only ever made the step WIDTH continuous.
	//
	// What is testable about the evaluator is that it does not amplify that: the waveform difference
	// stays proportional to the params difference rather than blowing up.
	//
	// Bisected rather than hard-coded, because the two offsets that straddle the crossing are adjacent
	// floats — decimal literals for them round to the same side — and because bisecting on the width
	// itself keeps the case pointed at the crossing if a firmware bump moves it.
	float lo = 0.00585f; // countF 6.996 here...
	float hi = 0.00605f; // ...and 7.011 here, so step 7 comes alive somewhere between.
	REQUIRE(buildPhiStairParams(64, lo).widths[7] == 0.0f);
	REQUIRE(buildPhiStairParams(64, hi).widths[7] > 0.0f);
	while (true) {
		const float mid = 0.5f * (lo + hi);
		if (mid == lo || mid == hi) {
			break;
		}
		if (buildPhiStairParams(64, mid).widths[7] > 0.0f) {
			hi = mid;
		}
		else {
			lo = mid;
		}
	}

	const PhiStairParams below = buildPhiStairParams(64, lo);
	const PhiStairParams above = buildPhiStairParams(64, hi);

	const float delta = paramsDelta(below, above);
	CHECK(delta > 1.5f); // The character layer's own jump, pinned so a firmware fix is visible here.

	const double diff = meanAbsDiff(buildStairTable(below), buildStairTable(above));
	CHECK(diff < static_cast<double>(delta)); // Observed 0.135 against a params delta of 1.607.
}

TEST_CASE("buildStairXfade collapses to its source params at the endpoints") {
	const PhiStairParams a = buildPhiStairParams(64, 0.0f);
	const PhiStairParams b = buildPhiStairParams(600, 0.0f);

	const StairTable atA = buildStairTable(feathers::buildStairXfade(a, b, 0.0f));
	const StairTable atB = buildStairTable(feathers::buildStairXfade(a, b, 1.0f));
	const StairTable justA = buildStairTable(a);
	const StairTable justB = buildStairTable(b);

	// Absolute, not relative: the waveform crosses zero, where a relative tolerance means nothing.
	for (int i = 0; i < 64; i++) {
		const double p = static_cast<double>(i) / 64.0;
		CAPTURE(p);
		CHECK(std::abs(evalStairTable(atA, p) - evalStairTable(justA, p)) <= 1e-6f);
		CHECK(std::abs(evalStairTable(atB, p) - evalStairTable(justB, p)) <= 1e-6f);
	}
}

TEST_CASE("buildStairXfade interpolates every field linearly") {
	// The original fixed-point Stair table build crossfades the two banks in float, plainly, before it quantises anything —
	// the halved-difference lerps in that file are all in the q31 scan, where a raw difference of two
	// full-scale slots would wrap. There is nothing to guard against here, so a plain lerp is not a
	// simplification of the firmware's rule, it IS the firmware's rule, and this pins that.
	const PhiStairParams a = buildPhiStairParams(64, 0.0f);
	const PhiStairParams b = buildPhiStairParams(600, 0.0f);
	const PhiStairParams mid = feathers::buildStairXfade(a, b, 0.375f);

	CHECK(mid.slope == doctest::Approx(0.625f * a.slope + 0.375f * b.slope));
	for (int i = 0; i < kPhiStairSteps; i++) {
		CAPTURE(i);
		CHECK(mid.heights[i] == doctest::Approx(0.625f * a.heights[i] + 0.375f * b.heights[i]));
		CHECK(mid.widths[i] == doctest::Approx(0.625f * a.widths[i] + 0.375f * b.widths[i]));
	}
}

TEST_CASE("buildStairXfadeTable is the crossfade and the preparation in one call") {
	// A composition, so the only thing that can go wrong is composing the wrong two things — passing
	// the crossfade position through unmangled, and preparing the crossfade result rather than one of
	// the banks. Mid-fade positions catch both; the endpoints alone would not, since a table prepared
	// from bank a matches at xf 0 whatever the function did with it.
	const PhiStairParams a = buildPhiStairParams(64, 0.0f);
	const PhiStairParams b = buildPhiStairParams(600, 0.0f);

	for (float xf : {0.0f, 0.125f, 0.5f, 0.875f, 1.0f}) {
		CAPTURE(xf);
		const StairTable composed = buildStairTable(feathers::buildStairXfade(a, b, xf));
		const StairTable direct = feathers::buildStairXfadeTable(a, b, xf);
		for (int i = 0; i < 64; i++) {
			const double p = static_cast<double>(i) / 64.0;
			REQUIRE(evalStairTable(direct, p) == evalStairTable(composed, p));
		}
	}
}

TEST_CASE("the prepared table and the params overload evaluate identically") {
	// The params overload exists for tests and one-shots; the voice path prepares a table once and
	// evaluates it per sample. They have to be the same function or the cheap one is a different
	// oscillator.
	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const PhiStairParams p = buildPhiStairParams(zone, 0.0f);
		const StairTable table = buildStairTable(p);
		for (int i = 0; i < 64; i++) {
			const double u = static_cast<double>(i) / 64.0;
			REQUIRE(evalStairTable(table, u) == evalStairTable(p, u));
		}
	}
}

TEST_CASE("phase wraps, so any real offset is legal") {
	const StairTable table = buildStairTable(buildPhiStairParams(600, 0.0f));
	for (int i = 0; i < 64; i++) {
		const double u = static_cast<double>(i) / 64.0;
		CAPTURE(u);
		CHECK(evalStairTable(table, u + 7.0) == evalStairTable(table, u));
		CHECK(evalStairTable(table, u - 3.0) == evalStairTable(table, u));
	}
}

TEST_CASE("buildStairXfade and evalStairTable are deterministic") {
	const PhiStairParams first =
	    feathers::buildStairXfade(buildPhiStairParams(64, 0.0f), buildPhiStairParams(600, 0.0f), 0.375f);
	const PhiStairParams second =
	    feathers::buildStairXfade(buildPhiStairParams(64, 0.0f), buildPhiStairParams(600, 0.0f), 0.375f);

	const std::vector<float> a = sweep(first);
	const std::vector<float> b = sweep(second);

	REQUIRE(a.size() == b.size());
	for (size_t i = 0; i < a.size(); i++) {
		CAPTURE(i);
		REQUIRE(a[i] == b[i]);
	}
}

TEST_CASE("a step narrower than the loudness rule's own sample grid drives the clip") {
	// The original fixed-point Stair table build measures peak and RMS from 128 slots and scales the waveform by what it
	// found. Evaluating continuously renders steps those 128 slots can miss, and a missed step is a
	// missed peak, so the scale comes out too large and the step overshoots. Zone 108 at offset 0.25 is
	// the case: its last live step is 0.0078 of the cycle against a slot spacing of 0.0078125, and its
	// height is the only positive one in an otherwise negative staircase. Unclipped it would reach
	// 1.27. This is the only zone/offset pair in the whole 1024 x {0, 0.25, 1.618, 0.5} grid that
	// clips, and one more that reaches 0.985 without clipping, so the clip is a rail and not a
	// character — but it has to be there, and it has to hold its sign.
	const std::vector<float> samples = sweep(buildPhiStairParams(108, 0.25f), 1 << 16);

	CHECK(peak(samples) <= 1.0f + 1e-6f);

	int atPositiveRail = 0;
	for (float s : samples) {
		if (s >= 1.0f - 1e-6f) {
			atPositiveRail++;
		}
	}
	CHECK(atPositiveRail > 100); // A plateau across the narrow step, not one grazing sample.
}

TEST_CASE("the 128-slot grid holds the character and drops only what falls between slots") {
	// The voice path renders evalStairSlots, not the continuous evaluator, because the original fixed-point Stair renderer
	// resamples onto a 128-slot grid and interpolates rather than evaluating the staircase per sample.
	// The risk in that swap is losing the character rather than losing treble, so this pins the
	// dividing line: at the slot phases the two must agree exactly, and between them they may differ
	// only by what linear interpolation across one slot can hide.
	for (uint16_t zone : kZones) {
		for (float offset : {0.0f, 0.25f, 1.618f}) {
			CAPTURE(zone);
			CAPTURE(offset);
			const StairTable t = buildStairTable(buildPhiStairParams(zone, offset));

			// Exactly, not approximately: the slots ARE the continuous evaluator sampled at j/128 and
			// normalised by the same mean and scale, so any difference at all means the grid was built
			// from something other than the waveform it is supposed to carry.
			for (int j = 0; j < kPhiStairSlots; j++) {
				const double u = static_cast<double>(j) / static_cast<double>(kPhiStairSlots);
				REQUIRE(evalStairSlots(t, u) == evalStairTable(t, u));
			}

			// Between slots the grid is a chord across whatever the waveform does, so it can never leave
			// the range its two endpoints span. A staircase edge inside a slot is exactly the content
			// that gets traded away, on hardware as here.
			for (int j = 0; j < kPhiStairSlots; j++) {
				const float lo = std::min(t.slots[j], t.slots[j + 1]);
				const float hi = std::max(t.slots[j], t.slots[j + 1]);
				for (int s = 1; s < 8; s++) {
					const double u = (static_cast<double>(j) + static_cast<double>(s) / 8.0)
					                 / static_cast<double>(kPhiStairSlots);
					const float v = evalStairSlots(t, u);
					REQUIRE(v >= lo - 1e-6f);
					REQUIRE(v <= hi + 1e-6f);
				}
			}
		}
	}
}

TEST_CASE("the slot grid wraps rather than running off its own end") {
	// The guard slot exists so the scan's idx+1 never needs a wrap test. If it were ever out of step
	// with slot 0, the last fraction of the cycle would interpolate towards garbage — audible as a
	// click once per cycle, and invisible to every test that samples on slot boundaries.
	const StairTable t = buildStairTable(buildPhiStairParams(600, 0.0f));
	CHECK(t.slots[kPhiStairSlots] == t.slots[0]);

	// Approaching the wrap from below lands on slot 0, and phase 1.0 is phase 0.
	CHECK(evalStairSlots(t, 1.0) == evalStairSlots(t, 0.0));
	CHECK(std::abs(evalStairSlots(t, 1.0 - 1e-9) - t.slots[0]) < 1e-5f);
}

TEST_CASE("the mips are the firmware's binomial pass, applied once and twice") {
	// The original fixed-point Stair table build filters the slot table into the first mip and that into the second with a circular
	// three-tap (1, 2, 1)/4 across the slots, no decimation. Recomputed here independently rather than
	// by calling the same helper, so this pins the recipe and not merely that the code is consistent
	// with itself.
	auto binomialOf = [](const float* src) {
		std::vector<float> dst(kPhiStairSlots + 1);
		for (int j = 0; j < kPhiStairSlots; j++) {
			const int prev = (j == 0) ? kPhiStairSlots - 1 : j - 1;
			dst[static_cast<size_t>(j)] = 0.25f * (src[prev] + 2.0f * src[j] + src[j + 1]);
		}
		dst[kPhiStairSlots] = dst[0];
		return dst;
	};

	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const StairTable t = buildStairTable(buildPhiStairParams(zone, 0.0f));

		const std::vector<float> expect1 = binomialOf(t.slots);
		const std::vector<float> expect2 = binomialOf(expect1.data());
		for (int j = 0; j <= kPhiStairSlots; j++) {
			REQUIRE(t.mip1[j] == expect1[static_cast<size_t>(j)]);
			REQUIRE(t.mip2[j] == expect2[static_cast<size_t>(j)]);
		}
	}
}

TEST_CASE("the mip ladder takes treble and leaves the rest alone") {
	bool sawHardEdgedZone = false;
	for (uint16_t zone : kZones) {
		CAPTURE(zone);
		const StairTable t = buildStairTable(buildPhiStairParams(zone, 0.0f));

		auto meanOf = [](const float* s) {
			double acc = 0.0;
			for (int j = 0; j < kPhiStairSlots; j++) {
				acc += static_cast<double>(s[j]);
			}
			return acc / static_cast<double>(kPhiStairSlots);
		};
		auto roughnessOf = [](const float* s) {
			float m = 0.0f;
			for (int j = 0; j < kPhiStairSlots; j++) {
				m = std::max(m, std::abs(s[j + 1] - s[j]));
			}
			return m;
		};

		// A binomial pass has unity gain at DC, so the level the waveform sits at cannot move. If this
		// drifted, a mip switch mid-note would step the DC and thump.
		CHECK(meanOf(t.mip1) == doctest::Approx(meanOf(t.slots)).epsilon(1e-6));
		CHECK(meanOf(t.mip2) == doctest::Approx(meanOf(t.slots)).epsilon(1e-6));

		// And it is a lowpass, so a pass can never make the grid rougher.
		CHECK(roughnessOf(t.mip1) <= roughnessOf(t.slots));
		CHECK(roughnessOf(t.mip2) <= roughnessOf(t.mip1));

		// Only <= , because a symmetric three-tap smoother reproduces a straight line exactly. In a zone
		// with risers long enough to span several slots the steepest place on the grid IS a straight
		// line, so its roughness is preserved and that is the filter behaving correctly. Where the zone
		// has hard edges instead there is nothing linear to preserve, and the pass has to bite.
		if (buildPhiStairParams(zone, 0.0f).slope <= 0.001f) {
			CHECK(roughnessOf(t.mip1) < roughnessOf(t.slots));
			CHECK(roughnessOf(t.mip2) < roughnessOf(t.mip1));
			sawHardEdgedZone = true;
		}

		// Nothing leaves the range the bright table already occupied — a lowpass of a bounded signal
		// cannot overshoot when its taps are all positive.
		for (int j = 0; j <= kPhiStairSlots; j++) {
			REQUIRE(std::abs(t.mip1[j]) <= 1.0f + 1e-6f);
			REQUIRE(std::abs(t.mip2[j]) <= 1.0f + 1e-6f);
		}
	}

	// Otherwise the strict half of this test would be vacuous and nobody would notice.
	CHECK(sawHardEdgedZone);
}

TEST_CASE("the mip switches where the firmware switches") {
	// The original fixed-point Stair renderer compares its phase increment against 68174083 and 175304787. A phase increment is
	// frequency over sample rate times 2^32 and the hardware runs at 44100, which puts the switch
	// points at 699.999989 Hz and 1799.999994 Hz. Bracketed by a hundredth of a Hz either side and
	// written out as literals, so a change to the conversion has to disagree with something here.
	CHECK(feathers::stairMipForFrequency(20.0) == 0);
	CHECK(feathers::stairMipForFrequency(699.99) == 0);
	CHECK(feathers::stairMipForFrequency(700.01) == 1);
	CHECK(feathers::stairMipForFrequency(1799.99) == 1);
	CHECK(feathers::stairMipForFrequency(1800.01) == 2);
	CHECK(feathers::stairMipForFrequency(20000.0) == 2);

	// Middle C is below the first threshold, so an ordinary note plays the bright table and the ladder
	// is not something every note is quietly paying for.
	CHECK(feathers::stairMipForFrequency(261.63) == 0);

	const StairTable t = buildStairTable(buildPhiStairParams(108, 0.25f));
	CHECK(feathers::stairMipSlots(t, 0) == t.slots);
	CHECK(feathers::stairMipSlots(t, 1) == t.mip1);
	CHECK(feathers::stairMipSlots(t, 2) == t.mip2);
}

TEST_CASE("a shorter slope is a brighter staircase") {
	// THE DIRECTION CLAIM BEHIND STAIR'S BLUE SHIFT, measured rather than reasoned. The morph engine
	// brightens by taking MORE curvature; stair brightens by taking LESS slope, because slope is the
	// fraction of a step spent ramping and a shorter ramp is a faster edge. That inversion looks like a
	// sign error to anyone reading the two side by side, so it is worth a number.
	//
	// Measured as high-frequency energy in the rendered cycle: sharper edges put more of the waveform's
	// content above the fundamental's first few harmonics.
	// Zone 291 has a slope worth measuring (0.509). Chosen because 39% of zones come out with slope
	// exactly 0 — hard-edged staircases, where there is no ramp to shorten and this flavour has nothing
	// to do. A fixture on one of those would have proved the opposite of what it looked like.
	const PhiStairParams base = owlet::dsp::buildPhiStairParams(291, 0.0f);

	const auto brightness = [](const PhiStairParams& p) {
		StairTable table = buildStairTable(p);
		constexpr int kPoints = 4096;
		std::vector<float> cycle(kPoints);
		for (int i = 0; i < kPoints; ++i) {
			cycle[i] = evalStairTable(table, static_cast<double>(i) / static_cast<double>(kPoints));
		}
		// Plain difference energy: a proxy for edge content that needs no window and no transform, and
		// one that cannot be argued with — a waveform with faster transitions has larger steps between
		// adjacent samples.
		double energy = 0.0;
		for (int i = 1; i < kPoints; ++i) {
			const double d = static_cast<double>(cycle[i]) - static_cast<double>(cycle[i - 1]);
			energy += d * d;
		}
		return energy;
	};

	PhiStairParams sharp = base;
	PhiStairParams dull = base;
	sharp.slope = base.slope * 0.5f;
	dull.slope = std::min(0.95f, base.slope * 1.8f);
	REQUIRE(base.slope > 0.02f); // a zone whose slope is already nothing could not show the difference

	CAPTURE(base.slope);
	CHECK(brightness(sharp) > brightness(base));
	CHECK(brightness(dull) < brightness(base));
}

TEST_CASE("no zone asks for a slope the blend would clamp") {
	// THE CLAMP AT buildStairBlend IS INERT TODAY, and this is what would notice if it stopped being.
	// The leaned slope is held at 0.95 because above 1 a ramp is longer than its own step and never
	// arrives at its height; the widest slope the character layer actually produces is 0.894, so the
	// clamp never fires on any zone this engine can build.
	//
	// It is worth pinning because the failure mode is silent: a future widening of the slope range
	// would start clipping character at the top end with no golden to catch it — the goldens pin the
	// firmware's own builders, and this clamp lives on our side of that line.
	float widest = 0.0f;
	int widestZone = -1;
	for (int zone = 0; zone < 1024; ++zone) {
		const PhiStairParams p = owlet::dsp::buildPhiStairParams(static_cast<uint16_t>(zone), 0.0f);
		if (p.slope > widest) {
			widest = p.slope;
			widestZone = zone;
		}
	}
	CAPTURE(widestZone);
	CAPTURE(widest);
	// Zone 563 is the widest the builder makes, at 0.894. THE KNOWN-BAD FOR THIS ONE IS A FIRMWARE
	// BUMP, which is not mine to stage — the character layer is a submodule this repo does not modify.
	// What was verified instead is that the assertion is live rather than vacuous: lowered to 0.80 it
	// fails and reports 0.894 from zone 563, so it is reading the real builder and not an empty loop.
	CHECK(widest < 0.95f);
	// And a sanity floor, so a build whose slopes all collapsed to zero could not pass this by being
	// uniformly silent about it.
	CHECK(widest > 0.5f);
}
