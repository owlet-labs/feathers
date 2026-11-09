/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_triangle.hpp"
#include "character/util.hpp"
#include "fx/sklitter/macro_push.h"
#include "fx/sklitter/perform.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/slice_plan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <string>

using namespace feathers::fx::sklitter;
namespace phi = owlet::dsp::phi;
using owlet::dsp::triangleFloat;
using owlet::dsp::triangleSimpleUnipolar;

namespace {

// 0.01f is the gate itself; the configs' fractional parts all differ, so no two share a position.
constexpr std::array kMacros { 0.0f, 0.005f, 0.01f, 0.02f, 0.4f, 1.0f };
constexpr std::array kConfigs { 0.0f, 0.3f, 0.62f, 3.85f, -1.7f };
constexpr std::array kGammas { 0.0f, 0.37f, 100.0f };

} // namespace

TEST_CASE("the macro's push is its eight terms, each restated from where slice_plan.h reads it") {
	// **EXACT FLOAT EQUALITY IS THE POINT.** The perform circle replaces eight inline expressions in
	// `planFor` and `offsetsFor` with this struct, and a plan drawn from it must be the plan drawn today
	// to the bit. Each expected value is restated from its site — `offsetsFor`'s two zone influences,
	// `planFor`'s dry threshold, three probability scales, gate influence and subdivision influence — in
	// the site's own product order and type, not by calling `macroPushOf`.
	for (float macro : kMacros) {
		for (float cfg : kConfigs) {
			for (float gamma : kGammas) {
				const Coordinate mc = splitCoordinate(cfg);
				const MacroPush p = macroPushOf(macro, mc, gamma);
				// planFor's own sum, in its own order and type: `ofs.macroConfig` is the lap, the
				// position arrives as `macroConfig`
				const double norm = static_cast<double>(mc.position)
				                    + (static_cast<double>(mc.lap) * 0.1 + static_cast<double>(gamma));
				const auto tri = [&](float k, float duty) {
					return triangleSimpleUnipolar(phi::wrapPhase(norm * static_cast<double>(k)), duty);
				};
				const auto bip = [&](float k) {
					return triangleFloat(phi::wrapPhase(norm * static_cast<double>(k)), 0.6f);
				};
				INFO("macro=" << macro << " cfg=" << cfg << " gamma=" << gamma);
				CHECK(p.dryThreshold == macro * tri(phi::kPhi, 0.5f));
				const bool pushes = macro > 0.01f; // the reference's gate on the three probabilities
				CHECK(p.dReverse == (pushes ? macro * bip(phi::kPhi125) * 0.5f : 0.0f));
				CHECK(p.dPitch == (pushes ? macro * bip(phi::kPhi200) * 0.3f : 0.0f));
				CHECK(p.dDelay == (pushes ? macro * bip(phi::kPhi075) * 0.4f : 0.0f));
				CHECK(p.gatePull == macro * tri(phi::kPhi100, 0.6f) * 0.75f);
				CHECK(p.subdivMix == macro * tri(phi::kPhi225, 0.5f));
				// offsetsFor reads the POSITION only — no lap, no gamma, no wrap
				CHECK(p.zoneAPush == macro * triangleSimpleUnipolar(mc.position * phi::kPhi050, 0.5f) * 0.3f);
				CHECK(p.zoneBPush == macro * triangleSimpleUnipolar(mc.position * phi::kPhi075, 0.5f) * 0.3f);
			}
		}
	}
}

TEST_CASE("the macro's push has the properties a blend relies on, not only the expressions it restates") {
	// The restatement above is nearly the builder's own code, so it proves wiring more than meaning. These
	// are what the perform circle actually leans on, each checked without repeating an expression.

	SUBCASE("the zone pushes read the position alone; the plan's terms read the lap and gamma too") {
		// 0.25, 2.25 and -1.75 are exact binary fractions with one position and three laps.
		const MacroPush atLap0 = macroPushOf(1.0f, splitCoordinate(0.25f), 0.0f);
		const MacroPush atLap2 = macroPushOf(1.0f, splitCoordinate(2.25f), 0.0f);
		const MacroPush atLapM2 = macroPushOf(1.0f, splitCoordinate(-1.75f), 0.0f);
		const MacroPush atGamma = macroPushOf(1.0f, splitCoordinate(0.25f), 100.0f);
		CHECK(atLap0.zoneAPush == atLap2.zoneAPush);
		CHECK(atLap0.zoneBPush == atLap2.zoneBPush);
		CHECK(atLap0.zoneAPush == atLapM2.zoneAPush);
		CHECK(atLap0.zoneBPush == atLapM2.zoneBPush);
		CHECK(atLap0.zoneAPush == atGamma.zoneAPush);
		CHECK(atLap0.zoneBPush == atGamma.zoneBPush);
		CHECK(atLap0.zoneAPush != 0.0f); // the equalities above are not two zeros
		CHECK(atLap0.zoneBPush != 0.0f);
		// And NOT vacuous: the same lap change does move a term `planFor` reads.
		CHECK((atLap0.dryThreshold != atLap2.dryThreshold || atLap0.gatePull != atLap2.gatePull
		       || atLap0.subdivMix != atLap2.subdivMix));
		CHECK((atLap0.dryThreshold != atGamma.dryThreshold || atLap0.gatePull != atGamma.gatePull
		       || atLap0.subdivMix != atGamma.subdivMix));
	}

	SUBCASE("above the gate, every term scales with the macro") {
		for (float m : { 0.02f, 0.1f, 0.25f, 0.5f }) {
			for (float cfg : { 0.3f, 3.85f }) {
				for (float gamma : { 0.0f, 0.37f }) {
					INFO("m=" << m << " cfg=" << cfg << " gamma=" << gamma);
					const MacroPush one = macroPushOf(m, splitCoordinate(cfg), gamma);
					const MacroPush two = macroPushOf(2.0f * m, splitCoordinate(cfg), gamma);
					const auto twice = [](float v) { return doctest::Approx(2.0f * v).epsilon(1e-5); };
					CHECK(two.dryThreshold == twice(one.dryThreshold));
					CHECK(two.dReverse == twice(one.dReverse));
					CHECK(two.dPitch == twice(one.dPitch));
					CHECK(two.dDelay == twice(one.dDelay));
					CHECK(two.gatePull == twice(one.gatePull));
					CHECK(two.subdivMix == twice(one.subdivMix));
					CHECK(two.zoneAPush == twice(one.zoneAPush));
					CHECK(two.zoneBPush == twice(one.zoneBPush));
				}
			}
		}
	}

	SUBCASE("the three probability pushes are exactly zero up to 0.01 and live just past it") {
		for (float macro : { 0.009f, 0.01f }) {
			for (float cfg : kConfigs) {
				INFO("macro=" << macro << " cfg=" << cfg);
				const MacroPush p = macroPushOf(macro, splitCoordinate(cfg), 0.37f);
				CHECK(p.dReverse == 0.0f);
				CHECK(p.dPitch == 0.0f);
				CHECK(p.dDelay == 0.0f);
			}
		}
		const MacroPush past = macroPushOf(0.0101f, splitCoordinate(0.3f), 0.37f);
		CHECK((past.dReverse != 0.0f || past.dPitch != 0.0f || past.dDelay != 0.0f));
	}

	SUBCASE("across the restatement's grid, each of the eight terms is nonzero somewhere") {
		std::array<bool, 8> live {};
		for (float macro : kMacros) {
			for (float cfg : kConfigs) {
				for (float gamma : kGammas) {
					const MacroPush p = macroPushOf(macro, splitCoordinate(cfg), gamma);
					const std::array v { p.dryThreshold, p.dReverse, p.dPitch,    p.dDelay,
					                     p.gatePull,     p.subdivMix, p.zoneAPush, p.zoneBPush };
					for (size_t i = 0; i < v.size(); ++i) {
						live[i] = live[i] || v[i] != 0.0f;
					}
				}
			}
		}
		for (size_t i = 0; i < live.size(); ++i) {
			INFO("field " << i << " (declaration order)");
			CHECK(live[i]);
		}
	}
}

TEST_CASE("a plan drawn from the push is the plan drawn from the pair, bit for bit") {
	// **THIS CASE HOLDS THE TWO ROADS TO ONE ANSWER. IT DOES NOT PROVE THE PUSH ROAD IS THE OLD PLAN.**
	// The pair overloads forward to the push overloads, so a wrong `macroPushOf` agrees with itself here.
	// What it DOES catch is a forward built with the wrong coordinate: the known-bad that dropped the
	// config's lap in `planFor`'s forward went red here and nowhere else.
	//
	// Identity with the old inline plan was shown ONCE, by an uncommitted byte-compare of 483,840 plans
	// dumped before and after the change. Of the committed cases, only `test_fx_sklitter.cpp`'s parity case (the zone
	// push) caught a mutation of the push road; `test_fx_sklitter_modes.cpp`'s pinned values caught
	// neither known-bad.
	//
	// **GAMMA 0 AT CONFIG 0.65 IS WHAT REACHES THE GATE PULL.** `planFor` reads `gatePull` only on the
	// discrete zone-B path, which needs `phRawB == 0` — no gamma and no zone B push — and a zone B
	// position past 0.02. At 0.65 the zone B push's triangle is in its dead zone (0.65 · kPhi075 wraps
	// past 0.5) while the gate pull's is live (0.65 · kPhi100 wraps under 0.6), so both hold at once.
	// The first counter below proves the grid got there rather than trusting this arithmetic; the second
	// proves the path then PRODUCED a gate ratio, not only that its conditions held.
	int discreteWithGatePull = 0;
	int gateRatioMoved = 0;
	for (float macro : { 0.0f, 0.005f, 0.4f, 1.0f }) {
		for (float cfg : { 0.0f, 0.3f, 0.65f, 2.3f }) {
			for (float gamma : { 0.37f, 0.0f }) {
				for (float zoneB : { 0.6f, 0.2f }) {
					for (bool repeat : { false, true }) {
						for (float zoneA : { 0.0f, 0.2f, 0.9f }) {
							for (int32_t slice : { 0, 3, 7 }) {
								const Coordinate a = splitCoordinate(zoneA), b = splitCoordinate(zoneB),
								                 mc = splitCoordinate(cfg);
								const PhaseOffsets byPair = offsetsFor(a, b, mc, macro, gamma, 1, repeat);
								const MacroPush push = macroPushOf(macro, mc, gamma);
								const PhaseOffsets byPush = offsetsFor(a, b, mc, push, gamma, 1, repeat);
								INFO("macro=" << macro << " cfg=" << cfg << " gamma=" << gamma << " zoneB=" << zoneB
								              << " repeat=" << repeat << " zoneA=" << zoneA << " slice=" << slice);
								CHECK(byPair.zoneA == byPush.zoneA);
								CHECK(byPair.zoneB == byPush.zoneB);
								CHECK(byPair.macroPushesDelay == byPush.macroPushesDelay);
								const SlicePlan planByPush =
								    planFor(a.position, b.position, mc.position, push, slice, byPush, nullptr);
								CHECK(planFor(a.position, b.position, mc.position, macro, slice, byPair, nullptr) == planByPush);
								if (byPush.gamma == 0.0f && byPush.zoneB == 0.0f && push.gatePull != 0.0f) {
									++discreteWithGatePull;
									gateRatioMoved += planByPush.gateRatio != 1.0f ? 1 : 0;
								}
							}
						}
					}
				}
			}
		}
	}
	CHECK(discreteWithGatePull > 0);
	CHECK(gateRatioMoved > 0);
}

TEST_CASE("the plan cache is keyed on the push, so a different push is never handed the old plan") {
	// zoneB 0.2 is inside the stride dead zone and slice 0 has no weight: the cache is live. With the
	// default offsets `phRawB` is 0, so this is the discrete zone-B path (zone 1, position 0.6), where
	// the gate pull is read; zone A 0.5 puts the plan in the ratchet branch.
	//
	// **EVERY TERM `planFor` READS IS VARIED ALONE**, each to a value that changes something in this
	// plan: a key that ignored any one of them would hand back `first`. **THE TWO ZONE PUSHES ARE NOT
	// HERE, AND CANNOT BE:** `planFor` never reads them — they reach the plan through `ofs.zoneA` and
	// `ofs.zoneB`, which are in the key in their own right (`test_fx_sklitter_modes.cpp` pins that).
	const PhaseOffsets ofs;
	const Coordinate mc = splitCoordinate(0.3f);
	const MacroPush one = macroPushOf(0.4f, mc, 0.0f);
	PlanCache primed;
	const SlicePlan first = planFor(0.5f, 0.2f, 0.3f, one, 0, ofs, &primed);
	// the subdivision mix is read only when the plan does not hold for several slices
	REQUIRE(first.repeatSlices == 1);

	const auto differsOnlyIn = [&](const char* field, MacroPush other) {
		INFO("varied: " << std::string(field));
		PlanCache cache;
		(void)planFor(0.5f, 0.2f, 0.3f, one, 0, ofs, &cache); // prime with `one`
		PlanCache cold;
		const SlicePlan fresh = planFor(0.5f, 0.2f, 0.3f, other, 0, ofs, &cold);
		CHECK(planFor(0.5f, 0.2f, 0.3f, other, 0, ofs, &cache) == fresh);
		CHECK_FALSE(fresh == first); // not vacuous: this term matters here
	};
	// Each `other` is equal to `one` in every field but the one named — so a key that ignored the push
	// would hand back `first`.
	MacroPush other = one;
	other.dryThreshold = first.macroWantsDry ? 0.0f : 0.9f;
	differsOnlyIn("dryThreshold", other);
	other = one;
	other.dReverse = first.shouldReverse ? -1.0f : 1.0f; // clamps the probability to the far end
	differsOnlyIn("dReverse", other);
	other = one;
	other.dPitch = first.shouldPitchUp ? -1.0f : 1.0f;
	differsOnlyIn("dPitch", other);
	other = one;
	other.dDelay = first.shouldDelay ? -1.0f : 1.0f; // `ofs.macroPushesDelay` defaults true
	differsOnlyIn("dDelay", other);
	other = one;
	other.gatePull = one.gatePull + 0.25f;
	differsOnlyIn("gatePull", other);
	other = one;
	other.subdivMix = first.subdivisions > 1 ? 0.0f : 1.0f;
	differsOnlyIn("subdivMix", other);
}

TEST_CASE("perform weights: the centre keeps its share, a rim waypoint is itself, and they always sum to one") {
	// **WHAT THIS CASE USED TO BE CALLED AND WHY IT CHANGED.** It read "the centre is the knobs", and
	// the weights it asserts have not moved by one bit — but `PerformWeights::knob` is `centre` now
	// (2026-09-19) and there are no knobs for the centre to BE. The numbers are the same, the sentence
	// they support is "the share the puck has not spent", and that share multiplies nothing.
	const auto sum = [](const PerformWeights& w) { return w.centre + w.corner[0] + w.corner[1] + w.corner[2] + w.corner[3]; };

	const PerformWeights c = performWeightsOf(0.0f, 0.0f);
	CHECK(c.centre == 1.0f);
	CHECK(c.corner == std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f });

	// **THE WAYPOINTS ARE ON THE DIAGONALS SINCE 2026-09-19**, so this no longer reads
	// `performWeightsOf(0, -1)` for N: the new cardinals are rotated 45 degrees to match the corner boxes.
	// **THE POSITION IS ASKED OF `waypointAxis` RATHER THAN SPELLED HERE** —
	// a case that hard-codes a rim point is a case that goes on asserting the country the engine left,
	// and the four `perfX = 1.0f` fixtures in `test_fx_sklitter.cpp` did exactly that until this rotation
	// turned seven of its cases red.
	const auto onWaypoint = [](Corner c) {
		const auto [wx, wy] = waypointAxis(c);
		return performWeightsOf(wx, wy);
	};
	for (const Corner c : { kCfg1, kCfg2, kCfg3, kCfg4 }) {
		CAPTURE(static_cast<int>(c));
		const PerformWeights w = onWaypoint(c);
		// **`== 1.0f`, NOT `Approx`, AND THAT IS THE WHOLE POINT OF HOW THE ROTATION IS WRITTEN.**
		// `perform.h` drops the two `/√2` divides and scales the centre constant instead, which leaves
		// `fl(√2)` and `2·fl(1/√2)` bit-identical: the centre share clamps to a hard zero, the total IS
		// this one corner, and `c / total` is exact. The honest divide form gives 0.99999997 here — a
		// red on this line means somebody put the cosine back.
		CHECK(w.corner[c] == 1.0f);
		CHECK(w.centre == 0.0f);
		for (const Corner other : { kCfg1, kCfg2, kCfg3, kCfg4 }) {
			if (other != c) {
				CAPTURE(static_cast<int>(other));
				CHECK(w.corner[other] == 0.0f);
			}
		}
	}

	// **INSIDE THE CENTRE REGION: THE CENTRE KEEPS THE REST — checked against the HONEST ROTATION.**
	//
	// `perform.h` computes `u = x − y` and measures the centre against √2; the arithmetic it is claiming
	// to be is `u = (x − y)/√2` measured against 1, whose five weights already sum to one and need no
	// normalise at all. **That claim is the thing worth a case**, because it is what says the blend is
	// today's blend read at a rotated point rather than a new partition — so the reference below is the
	// divide form, written out, and not the implementation typed twice.
	const auto honestlyRotated = [](float x, float y) {
		const float inv = 1.0f / std::sqrt(2.0f);
		const float u = (x - y) * inv;
		const float v = (x + y) * inv;
		std::array<float, 5> w {}; // four corners by `Corner`, then the centre
		w[kCfg1] = std::max(0.0f, -v);
		w[kCfg2] = std::max(0.0f, u);
		w[kCfg3] = std::max(0.0f, v);
		w[kCfg4] = std::max(0.0f, -u);
		w[4] = std::max(0.0f, 1.0f - (std::fabs(u) + std::fabs(v)));
		return w;
	};
	for (const auto& [x, y] : { std::pair { 0.25f, -0.5f }, std::pair { -0.6f, 0.1f },
		                       std::pair { 0.0f, 0.0f }, std::pair { 0.45f, 0.45f },
		                       std::pair { -0.2f, -0.65f } }) {
		INFO("x=" << x << " y=" << y);
		const PerformWeights got = performWeightsOf(x, y);
		const std::array<float, 5> want = honestlyRotated(x, y);
		for (const Corner c : { kCfg1, kCfg2, kCfg3, kCfg4 }) {
			CAPTURE(static_cast<int>(c));
			CHECK(got.corner[c] == doctest::Approx(want[c]).epsilon(1e-6f));
		}
		CHECK(got.centre == doctest::Approx(want[4]).epsilon(1e-6f));
	}
	// NOT VACUOUS: the sweep above includes a point where the centre keeps a real share and two
	// waypoints split the rest, so "they agree" is a claim about live numbers rather than about zeros.
	const PerformWeights d = performWeightsOf(0.25f, -0.5f);
	CHECK(d.centre > 0.25f);
	CHECK(d.corner[kCfg1] > 0.1f);
	CHECK(d.corner[kCfg2] > 0.1f);
	CHECK(d.corner[kCfg3] == 0.0f);
	CHECK(d.corner[kCfg4] == 0.0f);

	// **A CARDINAL IS NOW A CROSSFADE, AND THE HALVES ARE EXACT.** Screen-east is exactly between `kCfg2`
	// (top-right) and `kCfg3` (bottom-right) — where under the cardinal arrangement it WAS `kCfg2` whole. This
	// is the one line a reader of the old numbers will trip over, so it is asserted rather than implied.
	const PerformWeights due = performWeightsOf(1.0f, 0.0f);
	CHECK(due.centre == 0.0f);
	CHECK(due.corner[kCfg2] == 0.5f);
	CHECK(due.corner[kCfg3] == 0.5f);
	CHECK(due.corner[kCfg1] == 0.0f);
	CHECK(due.corner[kCfg4] == 0.0f);

	// **OPPOSITE WAYPOINTS CANCEL ON THE AXIS BETWEEN THE OTHER TWO — which is a DIAGONAL now.** `kCfg2` and
	// `kCfg4` are `max(0, ±(x − y))`, so both vanish exactly where `x == y`: the `kCfg1`–`kCfg3` axis, top-left to
	// bottom-right.
	CHECK(performWeightsOf(-0.3f, -0.3f).corner[kCfg2] == 0.0f);
	CHECK(performWeightsOf(-0.3f, -0.3f).corner[kCfg4] == 0.0f);
	CHECK(performWeightsOf(0.3f, 0.3f).corner[kCfg2] == 0.0f);
	CHECK(performWeightsOf(0.3f, 0.3f).corner[kCfg4] == 0.0f);
	// and the other diagonal is where N and S cancel
	CHECK(performWeightsOf(0.3f, -0.3f).corner[kCfg1] == 0.0f);
	CHECK(performWeightsOf(0.3f, -0.3f).corner[kCfg3] == 0.0f);

	// modulation can push past the square; the weights stay defined
	for (float x : { -3.0f, -1.0f, -0.3f, 0.0f, 0.6f, 1.0f, 5.0f }) {
		for (float y : { -2.0f, -0.8f, 0.0f, 0.4f, 1.0f, 9.0f }) {
			INFO("x=" << x << " y=" << y);
			const PerformWeights w = performWeightsOf(x, y);
			CHECK(sum(w) == doctest::Approx(1.0f));
			CHECK(w.centre >= 0.0f);
		}
	}
}

namespace {

MacroPush pushOf(float macro, float cfg) {
	return macroPushOf(macro, splitCoordinate(cfg), 0.0f);
}

/// THE FOUR WAYPOINTS AT THEIR DECLARED CONFIGURATIONS, EACH AT MACRO 1.0 — which is what
/// `performPushOf` builds since the waypoint macros were deleted (2026-09-19). It took
/// {0.9, 0.6, 0.2, 0.6} as its four macros while those controls existed.
/// **THE CENTRE'S PUSH, RESTATED FROM THE DESIGN RATHER THAN READ FROM `centrePush()`** (2026-09-23,
/// provisional, pending a decision on whether it stays): zero in every term but `subdivMix`, which is 0.5.
constexpr float kRestSubdivMix = 0.5f;
MacroPush restCentre() {
	MacroPush p;
	p.subdivMix = kRestSubdivMix;
	return p;
}

std::array<MacroPush, kCorners> fourCorners() {
	return { pushOf(1.0f, 0.1875f), pushOf(1.0f, 0.4375f), pushOf(1.0f, 0.6875f), pushOf(1.0f, 0.9375f) };
}

/// Every one of the eight terms, approximately — for the places the blend is equal in value but not promised
/// to the bit (see the rim case for why).
///
/// **THE TOLERANCE IS THE ROUNDING IT DOCUMENTS, NOT doctest's DEFAULT.** MEASURED over all four rim points
/// at depth 1, worst over the eight terms: 7.45e-09 absolute, 1.005e-07 relative. doctest's default float
/// epsilon is ~1.19e-05 — about 1600x that slack, enough to pass a blend wrong in the fifth decimal. 1e-6
/// leaves an order of magnitude over the measurement and no more. A lerp form with a different error budget
/// must move this number DELIBERATELY, with its own measurement, rather than find it already loose enough.
void checkPushApprox(const MacroPush& got, const MacroPush& want) {
	CHECK(got.dryThreshold == doctest::Approx(want.dryThreshold).epsilon(1e-6f));
	CHECK(got.dReverse == doctest::Approx(want.dReverse).epsilon(1e-6f));
	CHECK(got.dPitch == doctest::Approx(want.dPitch).epsilon(1e-6f));
	CHECK(got.dDelay == doctest::Approx(want.dDelay).epsilon(1e-6f));
	CHECK(got.gatePull == doctest::Approx(want.gatePull).epsilon(1e-6f));
	CHECK(got.subdivMix == doctest::Approx(want.subdivMix).epsilon(1e-6f));
	CHECK(got.zoneAPush == doctest::Approx(want.zoneAPush).epsilon(1e-6f));
	CHECK(got.zoneBPush == doctest::Approx(want.zoneBPush).epsilon(1e-6f));
}

} // namespace

TEST_CASE("the blend at the centre IS THE CENTRE'S PUSH, bit for bit, at every depth") {
	// **AMENDED 2026-09-23: THE CENTRE CARRIES `subdivMix` 0.5** (the provisional ratchet decision), zero in
	// every other term — so "the zero push" below is now `restCentre()`, and the memcmp says the 0.5 is
	// exact at every depth, which is what "depth scales the displacement from the centre" promises.
	// **WHAT THIS CASE USED TO ASSERT AND WHY IT DOES NOT ANY MORE.** It was "the blend at the centre IS
	// the knobs' push, bit for bit, at every depth" — the promise that shipping the circle could not move
	// an existing patch's sound, which rested on the centre being the `Macro` / `Macro Cfg` pair. Those
	// two controls are DELETED (2026-09-19: the centre macro is gone and the centre is zero), so the centre is
	// NO MACRO AT ALL and the claim is the stronger, simpler one: a
	// centred puck produces `MacroPush {}`.
	//
	// **EXACT `==` AND A memcmp, FOR THE SAME REASON THE OLD CASE HAD BOTH.** `==` is VALUE identity,
	// which is all a consumer of the push can observe and which counts -0.0 equal to +0.0; the memcmp is
	// the stronger reading and says whether a zero's sign moves. Every term must come back +0.0f, which
	// `performWeightsOf`'s exact corner zeros are what buy (`perform.h`).
	const auto corners = fourCorners();
	const PerformWeights centre = performWeightsOf(0.0f, 0.0f);
	const MacroPush rest = restCentre(); // the CENTRE's push: zero but for subdivMix (2026-09-23)
	int bitsDiffer = 0;
	for (float depth : { 0.0f, 0.3f, 0.5f, 0.77f, 1.0f }) {
		INFO("depth=" << depth);
		const MacroPush got = blendPush(corners, centre, depth);
		CHECK(got == rest);
		bitsDiffer += std::memcmp(&got, &rest, sizeof(MacroPush)) != 0 ? 1 : 0;
	}
	CHECK(bitsDiffer == 0);
	// NOT VACUOUS WHEN THIS CASE IS RUN ALONE BY NAME: the corners are not themselves zero, so the
	// identity above is the CENTRE's doing — a puck barely off the origin already draws a live push.
	CHECK_FALSE(blendPush(corners, performWeightsOf(0.02f, 0.0f), 1.0f) == rest);
}

TEST_CASE("at depth 1 a rim point IS that waypoint's push") {
	// **EXACT NOW, AND IT WAS APPROXIMATE — THE OLD LERP HAD TO CHOOSE AND THIS DOES NOT.** The blend was
	// `knob + (corner - knob) · 1`, which in floating point need not give `corner` back to the bit, so
	// this case used `checkPushApprox` and its note recorded the trade: exact at the centre (today's
	// sound) and approximate here. With the centre a zero push the blend is `(0 + 1·corner + three signed
	// zeros) · 1`, and every step of that is exact — so BOTH ends are bit-exact and the tolerance is gone
	// rather than merely unused. `checkPushApprox` survives for the half-way case, which genuinely rounds.
	//
	// **AND THE RIM POINT IS A DIAGONAL SINCE 2026-09-19, WITHOUT COSTING THE IDENTITY.** Rotating the
	// input by an honest `/√2` would have: `fl(1/√2)²·2` is 0.99999997, so the puck would leak a 3e-8
	// centre share and this `==` would have had to become `checkPushApprox`. `perform.h` keeps the √2 in
	// the axes and scales the centre's constant to match, which makes `x ± y` land exactly on
	// `kWaypointAxis` — so the tolerance is still gone rather than merely unused.
	const auto corners = fourCorners();
	const auto atWaypoint = [](Corner c) {
		const auto [wx, wy] = waypointAxis(c);
		return performWeightsOf(wx, wy);
	};
	SUBCASE("N") { CHECK(blendPush(corners, atWaypoint(kCfg1), 1.0f) == corners[kCfg1]); }
	SUBCASE("E") { CHECK(blendPush(corners, atWaypoint(kCfg2), 1.0f) == corners[kCfg2]); }
	SUBCASE("S") { CHECK(blendPush(corners, atWaypoint(kCfg3), 1.0f) == corners[kCfg3]); }
	SUBCASE("W") { CHECK(blendPush(corners, atWaypoint(kCfg4), 1.0f) == corners[kCfg4]); }
	// not vacuous: the four waypoints are not all one push, so "it is that waypoint's" says something
	CHECK_FALSE(corners[kCfg1] == corners[kCfg2]);
	CHECK(corners[kCfg1].zoneAPush != doctest::Approx(corners[kCfg2].zoneAPush));
}

TEST_CASE("half-way to a waypoint is HALF that waypoint's push — the radius is the amount") {
	// **THE CLAIM THE DELETED WAYPOINT MACROS LEFT BEHIND.** `Macro N/E/S/W` are gone (2026-09-19: no macro
	// knobs, the pad alone controls); each waypoint's push is built at macro
	// 1.0 and the puck's DISTANCE from the middle is what supplies the amount. On a waypoint's OWN axis
	// inside the centre region that distance IS the weight, so the blended push must be half of E's,
	// term by term, and the restatement below is the half rather than a second call to the blend.
	//
	// **HALF-WAY IS ON THE DIAGONAL NOW, AND THE HALF IS STILL EXACT.** It read `(0.5, 0)` while the
	// waypoints were cardinal; E's axis is the top-right diagonal since 2026-09-19, so the point is half
	// of `waypointAxis(kCfg2)`. Halving a float is exact, `x − y` is then exactly `fl(1/√2)` and `x + y`
	// exactly 0, and the centre keeps `√2 − fl(1/√2)` which is the same `fl(1/√2)` again — so the total
	// is twice the corner and the weight is exactly 0.5, not 0.5 within rounding.
	const auto c = fourCorners();
	const auto halfWayTo = [](Corner corner) {
		const auto [wx, wy] = waypointAxis(corner);
		return std::pair { wx * 0.5f, wy * 0.5f };
	};
	const auto [hx, hy] = halfWayTo(kCfg2);
	CHECK(performWeightsOf(hx, hy).corner[kCfg2] == 0.5f); // exactly, and the case below leans on it
	const MacroPush got = blendPush(c, performWeightsOf(hx, hy), 1.0f);
	// THE RESTATEMENT: half of E's push, term by term, built here rather than asked of the blend — and in
	// `subdivMix` the other half is the CENTRE's (the provisional 0.5, 2026-09-23), since the share the
	// puck has not spent now carries it.
	const MacroPush want { 0.5f * c[kCfg2].dryThreshold, 0.5f * c[kCfg2].dReverse,  0.5f * c[kCfg2].dPitch,
		                   0.5f * c[kCfg2].dDelay,       0.5f * c[kCfg2].gatePull,
		                   0.5f * c[kCfg2].subdivMix + 0.5f * kRestSubdivMix,
		                   0.5f * c[kCfg2].zoneAPush,    0.5f * c[kCfg2].zoneBPush };
	checkPushApprox(got, want);
	// NOT VACUOUS: E's push is not itself zero, so "half of it" is a claim about a real number — and the
	// half is not the whole, so this could not pass against the rim case's answer.
	//
	// **IT NAMED `zoneAPush` AND THAT WAS WRONG, MEASURED BY THIS CASE GOING RED.** At E's declared
	// configuration of 0.4375 that term is EXACTLY ZERO — `0.4375 · kPhi050` lands past the 0.5 duty of
	// its `triangleSimpleUnipolar` — so the assertion asked for a number the arrangement does not
	// produce. The honest non-vacuity claim is about the push as a whole, which is what a reader of
	// "half of E" actually needs: some of E is nonzero, and half of it is not all of it.
	CHECK_FALSE(c[kCfg2] == MacroPush {});
	CHECK_FALSE(got == MacroPush {});
	CHECK_FALSE(got == c[kCfg2]);
	// AND THE OTHER THREE WAYPOINTS CONTRIBUTE NOTHING HERE, which is what makes the restatement above a
	// restatement of the intended path rather than an accident of these four configurations.
	CHECK(performWeightsOf(hx, hy).corner[kCfg1] == 0.0f);
	CHECK(performWeightsOf(hx, hy).corner[kCfg3] == 0.0f);
	CHECK(performWeightsOf(hx, hy).corner[kCfg4] == 0.0f);
	// AND THE CENTRE KEEPS THE OTHER HALF, which is what "the radius is the amount" means — half-way out
	// is half the waypoint and half nothing.
	CHECK(performWeightsOf(hx, hy).centre == 0.5f);
}

TEST_CASE("Perf Depth's travel is CURVED, and both ends of it are exact") {
	// **PLAYED IN ITS LINEAR VERSION, 2026-09-19:** full macro strength was usually too strong, and the
	// useful region of the strength knob was concentrated in its first 1% or perhaps 5-10%. So the curve
	// goes on the knob: `Perf Depth` — the strength knob — and NOT the puck's radius, which stays linear
	// (the half-way case above still holds).
	//
	// **THE EXPECTED VALUES ARE RESTATED FROM `kDepthCurve`, NOT READ BACK FROM THE FUNCTION.** Calling
	// `performAmountOf` to build the expectation would assert that it equals itself. `std::pow` here is
	// the restatement of "raise the knob to k" in the test's own words; what it is checking is that the
	// device's amount IS that, rather than the knob, and that the exponent in force is the declared one.
	const auto want = [](double knob) { return std::pow(knob, static_cast<double>(kDepthCurve)); };

	SUBCASE("the ends are exact, which is what every other case leans on") {
		// **ASSERTED RATHER THAN ARGUED.** `0^k` and `1^k` are exact in IEEE and the implementation
		// skips the call at both anyway — but "depth 0 is inert" and "at depth 1 a rim point IS that
		// waypoint's push" are the two properties the rest of this file is built on, and an exponent
		// that quietly moved either would make those cases lie rather than fail.
		CHECK(performAmountOf(0.0f) == 0.0f);
		CHECK(performAmountOf(1.0f) == 1.0f);
		// AND CLAMPED, because a routed depth can arrive outside its declared range even though
		// `stateOf` clamps the one the device decodes.
		CHECK(performAmountOf(-0.5f) == 0.0f);
		CHECK(performAmountOf(4.0f) == 1.0f);
	}

	SUBCASE("in between it is the knob raised to the curve") {
		for (const float knob : { 0.1f, 0.25f, 0.5f, 0.7f, 0.9f, 0.99f }) {
			CAPTURE(knob);
			CHECK(performAmountOf(knob) == doctest::Approx(want(knob)).epsilon(1e-6));
		}
		// **THE DESCRIBED SHAPE, SPELLED OUT AS NUMBERS** so a later exponent change has to face
		// them: the knob at half is about 1.6% of full and at seven tenths about 12%. These are the
		// arithmetic behind `kDepthCurve`'s starting value of 6 and they are NOT a measurement of
		// anything — `perform.h` and backlog FP both say it is left to be set by ear.
		CHECK(performAmountOf(0.5f) == doctest::Approx(0.015625f).epsilon(1e-4));
		CHECK(performAmountOf(0.7f) == doctest::Approx(0.117649f).epsilon(1e-4));
		// NOT VACUOUS: the amount is emphatically not the knob, which is the whole complaint.
		CHECK(performAmountOf(0.5f) < 0.5f * 0.25f);
	}

	SUBCASE("and it reaches the PUSH, which is the claim a knob's own value cannot make") {
		// The arithmetic above is about one function; this is about the device's road through it. At a
		// rim point the blend is that waypoint's push whole, so the pushes at two depths stand in
		// exactly the ratio the curve says — restated here rather than asked of `performAmountOf`.
		Settings s;
		s.perfX = 1.0f; // the E rim, so the push is E's and nothing else's
		const MacroPush full = performPushOf(s, performWeightsOf(s.perfX, s.perfY), 1.0f);
		const MacroPush half = performPushOf(s, performWeightsOf(s.perfX, s.perfY), 0.5f);
		REQUIRE(full.subdivMix != 0.0f); // E's largest live term — see backlog FP's table
		// `subdivMix` MEASURED FROM THE CENTRE'S 0.5 (2026-09-23): depth scales the displacement from the
		// centre, so half-depth is the centre plus the curved share of the way to E.
		CHECK(half.subdivMix
		      == doctest::Approx(kRestSubdivMix + static_cast<float>(want(0.5)) * (full.subdivMix - kRestSubdivMix))
		             .epsilon(1e-5));
		CHECK(half.dPitch == doctest::Approx(static_cast<float>(want(0.5)) * full.dPitch).epsilon(1e-5));
		// AND A LINEAR DEPTH WOULD HAVE GIVEN SOMETHING ELSE ENTIRELY — the known-bad this case exists
		// to fail is `kDepthCurve = 1`, and this line is what makes the difference a value rather than
		// a rounding.
		CHECK_FALSE(half.subdivMix
		            == doctest::Approx(kRestSubdivMix + 0.5f * (full.subdivMix - kRestSubdivMix)).epsilon(1e-3));
	}
}

TEST_CASE("depth 0 is inert wherever the puck is") {
	// INERT NOW MEANS THE CENTRE'S PUSH (zero but for subdivMix 0.5 since 2026-09-23) — the sentence is the same and the value
	// it names changed with the centre (2026-09-19).
	const MacroPush rest = restCentre(); // the CENTRE's push: zero but for subdivMix (2026-09-23)
	for (float x : { -1.0f, 0.3f, 1.0f }) {
		for (float y : { -1.0f, 0.0f, 0.8f }) {
			INFO("x=" << x << " y=" << y);
			CHECK(blendPush(fourCorners(), performWeightsOf(x, y), 0.0f) == rest);
		}
	}
	// not vacuous: the same pucks at depth 1 are emphatically not zero
	CHECK_FALSE(blendPush(fourCorners(), performWeightsOf(1.0f, 0.0f), 1.0f) == rest);
}

TEST_CASE("between two waypoints each term is the weighted sum of theirs — restated, not re-called") {
	const auto c = fourCorners();
	// **DUE NORTH IS THE BISECTOR OF N AND E SINCE THE ROTATION.** `(√½, −√½)` used to be the point
	// half-way round the rim between two cardinal waypoints; it is now `kCfg2`'s own axis, whole. The point
	// that splits N and E evenly is the direction between their two diagonals, which is straight up —
	// and the halves are exact there (`u` and `−v` are both 1, the centre clamps to 0, the total is 2).
	const PerformWeights w = performWeightsOf(0.0f, -1.0f); // half N, half E
	REQUIRE(w.corner[kCfg1] == 0.5f);
	REQUIRE(w.corner[kCfg2] == 0.5f);
	const MacroPush got = blendPush(c, w, 1.0f);
	// The tolerance is `checkPushApprox`'s and set from the same measurement — see its note. MEASURED here,
	// the three restatements below come back at 0 error exactly, so 1e-6 is slack this point does not use.
	CHECK(got.zoneAPush == doctest::Approx(0.5f * c[kCfg1].zoneAPush + 0.5f * c[kCfg2].zoneAPush).epsilon(1e-6f));
	CHECK(got.dReverse == doctest::Approx(0.5f * c[kCfg1].dReverse + 0.5f * c[kCfg2].dReverse).epsilon(1e-6f));
	CHECK(got.dryThreshold
	      == doctest::Approx(0.5f * c[kCfg1].dryThreshold + 0.5f * c[kCfg2].dryThreshold).epsilon(1e-6f));
	// not vacuous: the two waypoints genuinely differ on the term that matters most
	CHECK(c[kCfg1].zoneAPush != doctest::Approx(c[kCfg2].zoneAPush));
}

TEST_CASE("the plan's dry vote flips ONCE, at the puck position the blended number crosses 0.5") {
	// **THE CLAIM IS ABOUT `planFor`'s BOOLEAN, SO IT IS READ FROM A PLAN**, not from `blendPush`'s float:
	// design §1.2 is that the threshold happens once, at the end, on the blended number — and the number is
	// all `blendPush` can show. Sweeping the puck west to east with the knobs at dry 0 and the E waypoint at
	// dry 1 is the shape that would break if a future blend thresholded per waypoint and averaged booleans:
	// five steps instead of one edge.
	//
	// **AND THE EDGE'S POSITION IS ASSERTED, NOT ONLY ITS COUNT** — a single flip anywhere would pass a bare
	// crossing count. DERIVED: at depth 1 the blended `dryThreshold` is `wE·1 = wE` (a sum of `wE` and four
	// signed zeros, so exactly `wE`), and on a waypoint's OWN axis inside the centre region `wE` is the
	// distance out. `planFor` votes dry at `> 0.5f`, so the first dry step is the first above 0.5 on a
	// 0.01 grid: i = 51.
	//
	// **THE SWEEP RUNS ALONG `kCfg2`'s AXIS, WHICH IS THE TOP-RIGHT DIAGONAL SINCE 2026-09-19.** It used to
	// run due east with `y = 0`. **That sweep can no longer cross at all** — due east is the boundary
	// between `kCfg2` and `kCfg3`, so `wE` rises to exactly 0.5 and stops there, `> 0.5f` never fires, and the
	// case reported `crossings == 0`. A sweep that cannot express the outcome is not evidence about it.
	//
	// **AND THE INDEX IS CROSS-CHECKED AGAINST THE WEIGHT rather than only pinned**, because the divide
	// now runs everywhere (the unnormalised total is `kWaypointAxis`, not 1) and a 1-ulp question at the
	// i = 50 sample is a real one. `firstCross` below is where the blended NUMBER first exceeds 0.5,
	// computed without going near `planFor`; that the plan's boolean flips at the same step is the claim
	// — design §1.2's "the threshold happens once, at the end, on the blended number". A blend that
	// thresholded per waypoint and averaged booleans would give several steps, which `crossings` catches.
	//
	// **THE DERIVATION LOST A TERM AND NOT A STEP.** It read `wKnob·0 + wE·1` while the centre was a knob
	// push this case deliberately set to dryThreshold 0; with the centre a constant zero there is no first
	// term to write down, and the number the sweep crosses is unchanged — which is why `kFirstDry` did not
	// move when the knobs were deleted.
	constexpr int kFirstDry = 51;
	std::array<MacroPush, kCorners> c {};
	c[kCfg2].dryThreshold = 1.0f;
	const PhaseOffsets ofs;
	int crossings = 0;
	int firstDry = -1;
	int firstCross = -1;
	bool was = false;
	const auto [ex, ey] = waypointAxis(kCfg2);
	const auto puckAt = [&](float out) { return performWeightsOf(out * ex, out * ey); };
	for (int i = 0; i <= 100; ++i) {
		const float out = static_cast<float>(i) / 100.0f;
		const PerformWeights w = puckAt(out);
		if (w.corner[kCfg2] > 0.5f && firstCross < 0) {
			firstCross = i;
		}
		const MacroPush blended = blendPush(c, w, 1.0f);
		const bool dry = planFor(0.5f, 0.2f, 0.3f, blended, 0, ofs, nullptr).macroWantsDry;
		if (dry && firstDry < 0) {
			firstDry = i;
		}
		crossings += (dry != was) ? 1 : 0;
		was = dry;
	}
	CHECK(crossings == 1);
	CHECK(firstDry == kFirstDry);
	// THE PLAN'S EDGE IS THE NUMBER'S EDGE — the independent half, which does not consult `planFor`.
	CHECK(firstDry == firstCross);
	// not vacuous: the sweep really did visit both verdicts
	CHECK(planFor(0.5f, 0.2f, 0.3f, blendPush(c, performWeightsOf(0.0f, 0.0f), 1.0f), 0, ofs, nullptr)
	          .macroWantsDry
	      == false);
	CHECK(planFor(0.5f, 0.2f, 0.3f, blendPush(c, puckAt(1.0f), 1.0f), 0, ofs, nullptr).macroWantsDry
	      == true);
}

TEST_CASE("a non-finite puck axis reads as the centre, and an infinite one still reaches the rim") {
	// **THE HOLE THIS CLOSES IS NOT A ROUNDING ONE.** `std::clamp` passes a NaN through and `std::max(0.0f,
	// NaN)` returns its first argument, so without the de-NaN in `performWeightsOf` every weight is 0, the
	// total is 0, and the normalise makes all five NaN — and the blend then carries NaN into every term at
	// ANY depth, because `NaN · 0` is NaN too. `Perf X` / `Perf Y` are modulation destinations, which is
	// how a NaN can arrive.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const MacroPush rest = restCentre(); // the CENTRE's push: zero but for subdivMix (2026-09-23)
	const auto corners = fourCorners();

	for (auto puck : { std::pair { nan, 0.0f }, std::pair { 0.0f, nan }, std::pair { nan, nan } }) {
		INFO("x=" << puck.first << " y=" << puck.second);
		const PerformWeights w = performWeightsOf(puck.first, puck.second);
		CHECK(w.centre == 1.0f);
		CHECK(w.corner == std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f });
		CHECK(w.centre + w.corner[0] + w.corner[1] + w.corner[2] + w.corner[3] == 1.0f);
		// the centre's whole point: the circle is inert — NO MACRO, not "the knobs' macro", since
		// 2026-09-19 — at the depth that pulls furthest as well as at 0
		CHECK(blendPush(corners, w, 0.0f) == rest);
		CHECK(blendPush(corners, w, 1.0f) == rest);
	}

	// **THE GUARANTEE IS PER AXIS, AND THIS IS WHERE THAT SHOWS.** A NaN X with a live Y is not the centre —
	// it is the puck with its X centred, which is the same answer a zero X would give. Asserting the centre
	// here instead would be asserting more than the header promises, and it is how the first draft of this
	// case went red.
	CHECK(performWeightsOf(nan, 0.5f).corner == performWeightsOf(0.0f, 0.5f).corner);
	CHECK(performWeightsOf(nan, 0.5f).centre == performWeightsOf(0.0f, 0.5f).centre);
	CHECK(performWeightsOf(-1.0f, nan).corner == performWeightsOf(-1.0f, 0.0f).corner);
	// **NOT VACUOUS: THAT AXIS IS STILL LIVE.** It read `== 0.5f` while the waypoints were cardinal and a
	// puck at (0, 0.5) was half-way to S on S's own axis. Since the rotation that point is evenly between
	// `kCfg3` and `kCfg4` with the centre keeping the rest, so the share is the rotation's business and the
	// claim here is only that the Y axis reached the weights at all — asserted as a live number and as
	// the SYMMETRY that point must have, which a stuck zero or a stuck one could not fake.
	CHECK(performWeightsOf(nan, 0.5f).corner[kCfg3] > 0.3f);
	CHECK(performWeightsOf(nan, 0.5f).corner[kCfg3] == performWeightsOf(nan, 0.5f).corner[kCfg4]);

	// **NOT OVER-BROAD: an infinity is an ordinary out-of-square puck and clamps to the square as one**,
	// so (+inf, −inf) is the square's top-right corner and not the centre. **SINCE THE ROTATION THAT
	// CORNER IS `kCfg2`'s OWN AXIS**, so it is that waypoint WHOLE — and exactly, because the clamp lands on
	// (1, −1) and `x − y` is then exactly 2, which is past `kWaypointAxis`: the centre clamps hard and the
	// total is the one corner. It was half N and half E while the waypoints were cardinal.
	const PerformWeights ne = performWeightsOf(inf, -inf);
	CHECK(ne.centre == 0.0f);
	CHECK(ne.corner[kCfg2] == 1.0f);
	CHECK(ne.corner[kCfg1] == 0.0f);
	CHECK(ne.corner[kCfg3] == 0.0f);
	CHECK(ne.corner[kCfg4] == 0.0f);
	// AND A SINGLE INFINITY IS THE CARDINAL IT POINTS AT, which is a crossfade rather than a waypoint:
	// (−inf, 0) clamps to (−1, 0), due west, evenly between `kCfg4` (bottom-left) and `kCfg1` (top-left).
	CHECK(performWeightsOf(-inf, 0.0f).corner[kCfg4] == 0.5f);
	CHECK(performWeightsOf(-inf, 0.0f).corner[kCfg1] == 0.5f);
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE'S SEVEN AUX MOD OUTPUTS — design §4, the `kModOps` road.                                //
//--------------------------------------------------------------------------------------------------//
//
// SKLITTER is the registry's first device that is BOTH an audio device and a modulation source. The
// engine builds one object through `kAudioOps` and hands the SAME resolved row to both op tables every
// block: `kAudioOps.setFields` -> `setTarget` (the audio road, which owns `target_`) and
// `kModOps.setFields` -> `setPerformFields` (the mod road, which owns the puck and nothing else).
//
// **WHAT THESE CASES ARE GUARDING IS THAT THE TWO ROADS AGREE.** One pure function (`performWeightsOf`)
// over two pieces of state is the pointer-identity family with the shared thing being the FUNCTION, so
// nothing about calling it twice forces the answers to match. The agreement case below is the one that
// says they do after the same row; the rest pin each road's own behaviour.

namespace {

/// A ROW WITH THE PUCK PUT SOMEWHERE — the shape a host hands `setFields`, built from the declared
/// defaults so every field but the two named ones is what the descriptor says it is.
Row puckRow(float x, float y) {
	Row row = rowOf(Settings {});
	row[static_cast<size_t>(Param::PerfX)] = x;
	row[static_cast<size_t>(Param::PerfY)] = y;
	return row;
}

/// **DRIVES THE DEVICE THE WAY THE RACK DOES**: `setPerformFields` once per host block,
/// `tickPerform()` every `kPerformTickSamples`. Moves the puck along x at `unitsPerSecond` for
/// `seconds`, delivering a new position every `stepSeconds` (0 = every tick), and returns the speed
/// reading at the end.
///
/// IT STARTS AT -1 SO A SECOND OF TRAVEL AT THE RATES BELOW STAYS INSIDE THE SQUARE — `performWeightsOf`
/// clamps, and a drive that ran into the clamp would measure the clamp rather than the hand.
float driveSpeed(Sklitter& dev, double sampleRate, double unitsPerSecond, double seconds, double stepSeconds) {
	const double tick = static_cast<double>(kPerformTickSamples) / sampleRate;
	// **THE STEP IS COUNTED, AND THE TEST IS `>` AGAINST THE NEXT BOUNDARY** — the first draft held the
	// last delivery TIME and compared it with `!=`, which asks two computed doubles to be bit-equal for
	// the helper to deliver the right number of times. `std::floor(t / stepSeconds) * stepSeconds`
	// reproduces a boundary by two roundings, so the comparison was answering a question about floating
	// point rather than about the drive. A count and a `>` do not care.
	long delivered = -1;
	for (double t = 0.0; t < seconds; t += tick) {
		const long step = stepSeconds > 0.0 ? static_cast<long>(std::floor(t / stepSeconds)) : -1;
		const bool due = stepSeconds > 0.0 ? step > delivered : true;
		if (due) {
			delivered = step;
			const double at = stepSeconds > 0.0 ? static_cast<double>(step) * stepSeconds : t;
			const Row row = puckRow(static_cast<float>(-1.0 + at * unitsPerSecond), 0.0f);
			dev.setPerformFields(row.data(), row.size());
		}
		dev.tickPerform();
	}
	return dev.performOutput(outIndex(Out::Speed));
}

/// THE NORMALISATION, RESTATED — design §4.5's `v / (v + vHalf)`, so the cases assert a number rather
/// than compare two runs of the same code.
float saturate(double v) { return static_cast<float>(v / (v + kSpeedHalfUnitsPerSecond)); }

} // namespace

TEST_CASE("outputs 1-6 are the puck and the weights the blend used, published after normalisation") {
	Sklitter dev;
	dev.prepare(48000.0, 4096);

	SUBCASE("inside the centre region, where the centre still keeps a share") {
		const Row row = puckRow(0.25f, -0.5f);
		dev.setPerformFields(row.data(), row.size());
		const PerformWeights w = performWeightsOf(0.25f, -0.5f);
		CHECK(dev.performOutput(outIndex(Out::X)) == 0.25f);
		CHECK(dev.performOutput(outIndex(Out::Y)) == -0.5f);
		CHECK(dev.performOutput(outIndex(Out::Weight1)) == w.corner[kCfg1]);
		CHECK(dev.performOutput(outIndex(Out::Weight2)) == w.corner[kCfg2]);
		CHECK(dev.performOutput(outIndex(Out::Weight3)) == w.corner[kCfg3]);
		CHECK(dev.performOutput(outIndex(Out::Weight4)) == w.corner[kCfg4]);
	}

	SUBCASE("OUTSIDE the centre region, where the divide is what makes the published pair right") {
		// **THIS IS THE SUBCASE THAT CAN TELL NORMALISED FROM RAW**, because the numbers below are
		// written out by hand rather than taken from the function under test. The puck at (0.8, 0.6) is
		// outside the centre region (it needs `max(|x|, |y|) < √2/2` to be inside), so the centre share
		// has clamped to 0 and the two live corners are the rotated axes themselves: `u = x − y = 0.2`
		// on `kCfg2` and `v = x + y = 1.4` on `kCfg3`, against a total of 1.6. **RAW AND NORMALISED THEREFORE
		// DIFFER BY 1.6** — E publishes 0.125 against 0.2 raw.
		//
		// **AND THE PAIR IS NOT (0.8, 0.6) ANY MORE, WHICH IS THE ROTATION ITSELF.** Before 2026-09-19
		// the live corners WERE the screen axes and this subcase read `0.8/1.4` and `0.6/1.4`; a device
		// still publishing those would now fail here, which is the point of writing them out.
		const Row row = puckRow(0.8f, 0.6f);
		dev.setPerformFields(row.data(), row.size());
		CHECK(dev.performOutput(outIndex(Out::X)) == 0.8f);
		CHECK(dev.performOutput(outIndex(Out::Y)) == 0.6f);
		CHECK(dev.performOutput(outIndex(Out::Weight1)) == 0.0f); // N: x + y is positive
		CHECK(dev.performOutput(outIndex(Out::Weight2)) == doctest::Approx((0.8f - 0.6f) / 1.6f).epsilon(1e-6f));
		CHECK(dev.performOutput(outIndex(Out::Weight3)) == doctest::Approx((0.8f + 0.6f) / 1.6f).epsilon(1e-6f));
		CHECK(dev.performOutput(outIndex(Out::Weight4)) == 0.0f); // W: x - y is positive
		// AND THE RAW PAIR IS NOT WHAT CAME OUT, said directly so the claim cannot be read as
		// "something in the right area".
		CHECK(dev.performOutput(outIndex(Out::Weight2)) != doctest::Approx(0.8f - 0.6f).epsilon(1e-6f));
		CHECK(dev.performOutput(outIndex(Out::Weight3)) != doctest::Approx(0.8f + 0.6f).epsilon(1e-6f));
	}

	SUBCASE("a puck outside the square is published where the weights put it") {
		// ONE SANITISER: `performWeightsOf` clamps, and the axes go out as IT left them — not as the row
		// delivered them, and not through a second clamp at the publish.
		const Row row = puckRow(3.0f, -7.5f);
		dev.setPerformFields(row.data(), row.size());
		CHECK(dev.performOutput(outIndex(Out::X)) == 1.0f);
		CHECK(dev.performOutput(outIndex(Out::Y)) == -1.0f);
	}

	SUBCASE("past the declared outputs there is nothing, rather than whatever is next in memory") {
		const Row row = puckRow(0.25f, -0.5f);
		dev.setPerformFields(row.data(), row.size());
		CHECK(dev.performOutput(kPerformOutputs) == 0.0f);
		CHECK(dev.performOutput(9999) == 0.0f);
	}

	SUBCASE("a row the host cut short, or did not send at all, decodes to the centre") {
		// **`AudioOps::setFields` DOCUMENTS A SHORT ROW AS THE ORDINARY CASE** — "absent means put your
		// default here" — and `ModOps::setFields` is deliberately the same shape. The mod road's own
		// `at` lambda answers 0 for a field past `fieldsPresent`, which for both puck axes IS the
		// declared default, so a host that stops before `PerfX` gets a centred puck rather than one
		// built out of whatever the row happened not to contain.
		const Row far = puckRow(0.9f, -0.9f);
		dev.setPerformFields(far.data(), far.size());
		REQUIRE(dev.performOutput(outIndex(Out::X)) == 0.9f); // moved, so the centre below is a change

		// CUT BEFORE THE PUCK. `PerfX` is not among the first few controls, so this row is short of it.
		dev.setPerformFields(far.data(), static_cast<size_t>(Param::PerfX));
		CHECK(dev.performOutput(outIndex(Out::X)) == 0.0f);
		CHECK(dev.performOutput(outIndex(Out::Y)) == 0.0f);
		CHECK(dev.performOutput(outIndex(Out::Weight1)) == 0.0f); // the centre spends nothing on any corner
		CHECK(dev.performOutput(outIndex(Out::Weight2)) == 0.0f);

		// AND NO ROW AT ALL. The null guard in that lambda was written and never exercised; a host
		// between states can hand over a null buffer, and the answer must be the centre and not a fault.
		dev.setPerformFields(far.data(), far.size());
		REQUIRE(dev.performOutput(outIndex(Out::X)) == 0.9f);
		dev.setPerformFields(nullptr, far.size());
		CHECK(dev.performOutput(outIndex(Out::X)) == 0.0f);
		CHECK(dev.performOutput(outIndex(Out::Y)) == 0.0f);
	}
}

TEST_CASE("THE RULE: after ONE resolved row both roads agree — the published axes and weights are the ones the blend used") {
	// **THE CASE THE TASK 6 QUALITY REVIEW ASKED FOR, and it is the only thing holding the two copies of
	// the puck together.** `performPushOf` takes the weights as an argument now, so there is one shape of
	// call rather than one hidden build — but the audio road passes weights built from `target_` and the
	// mod road publishes weights built from its own `puckX_`/`puckY_`, and nothing in the types forces
	// those to be the same puck. Here the SAME row goes to both, and the published sextet is checked
	// against the weights the audio road's settings produce.
	Sklitter dev;
	dev.prepare(48000.0, 4096);

	// PAST THE RIM ON PURPOSE: inside the diamond the divide does not run, so a disagreement about
	// normalisation would be invisible. And the pair is asymmetric so no two of the six coincide.
	for (const std::pair<float, float>& puck :
	     { std::pair { 0.8f, 0.6f }, std::pair { -0.35f, 0.4f }, std::pair { 0.0f, 0.0f },
	       std::pair { 1.0f, -1.0f }, std::pair { 5.0f, 0.125f } }) {
		INFO("x=" << puck.first << " y=" << puck.second);
		const Row row = puckRow(puck.first, puck.second);
		dev.setTarget(stateOf(row));               // the audio road
		dev.setPerformFields(row.data(), row.size()); // the mod road, same row

		const PerformWeights w = performWeightsOf(dev.targetForTest().perfX, dev.targetForTest().perfY);
		// THE AXES THE WEIGHTS CAME FROM, not the axes the row carried — they differ whenever the row is
		// outside the square, which the last pair above is.
		CHECK(dev.performOutput(outIndex(Out::X)) == w.x);
		CHECK(dev.performOutput(outIndex(Out::Y)) == w.y);
		CHECK(dev.performOutput(outIndex(Out::Weight1)) == w.corner[kCfg1]);
		CHECK(dev.performOutput(outIndex(Out::Weight2)) == w.corner[kCfg2]);
		CHECK(dev.performOutput(outIndex(Out::Weight3)) == w.corner[kCfg3]);
		CHECK(dev.performOutput(outIndex(Out::Weight4)) == w.corner[kCfg4]);
	}
}

TEST_CASE("the mod road does not touch the audio road's settings") {
	// **A SECOND WRITER IS A SECOND WRITER HOWEVER EQUAL THE VALUES LOOK.** `kModOps.setFields` receives
	// the whole resolved row and could decode all of it; it decodes the puck. If it ever called
	// `setTarget`, the device would have two writers of `target_` running in one block with no rule about
	// which lands last — and this case is what says it does not, by handing the mod road a row whose
	// every other field disagrees with what the audio road was told.
	Sklitter dev;
	dev.prepare(48000.0, 4096);

	Settings audio;
	audio.mode = Mode::Grain;
	audio.cornerCfg[kCfg2] = 0.75f; // stood in for the deleted `macro` field: an audio-road-only value
	audio.gamma = 1.3f;
	audio.perfX = 0.1f;
	audio.perfY = -0.2f;
	audio.perfDepth = 0.25f;
	dev.setTarget(audio);

	Row row = rowOf(Settings {}); // mode Shuffle, the declared waypoint configs, gamma at its default
	row[static_cast<size_t>(Param::PerfX)] = -0.9f;
	row[static_cast<size_t>(Param::PerfY)] = 0.4f;
	dev.setPerformFields(row.data(), row.size());

	// THE SETTINGS ARE UNMOVED, INCLUDING THE PUCK — which is the field a careless mod road would be
	// most tempted to "helpfully" keep in step.
	CHECK(dev.targetForTest().mode == Mode::Grain);
	CHECK(dev.targetForTest().cornerCfg[kCfg2] == 0.75f);
	CHECK(dev.targetForTest().gamma == 1.3f);
	CHECK(dev.targetForTest().perfX == 0.1f);
	CHECK(dev.targetForTest().perfY == -0.2f);
	CHECK(dev.targetForTest().perfDepth == 0.25f);

	// AND THE MOD ROAD DID ITS OWN JOB, so the case is not passing because `setPerformFields` did nothing
	// at all — the published puck is the one the ROW carried, not the one the settings hold.
	CHECK(dev.performOutput(outIndex(Out::X)) == -0.9f);
	CHECK(dev.performOutput(outIndex(Out::Y)) == 0.4f);
}

TEST_CASE("Perf Speed reads zero at rest and does not spike on the first tick after prepare") {
	// DESIGN §4.5: *"a patch load that teleports the puck is not a gesture."* A patch that opens with the
	// puck at 0.9 has not moved it 0.9 units in one tick, and the first tick is the only place that
	// distinction can be made.
	Sklitter dev;
	dev.prepare(48000.0, 4096);
	const Row row = puckRow(0.9f, 0.0f);
	dev.setPerformFields(row.data(), row.size());
	dev.tickPerform();
	CHECK(dev.performOutput(outIndex(Out::Speed)) == 0.0f);
	for (int i = 0; i < 1000; ++i) {
		dev.tickPerform();
	}
	CHECK(dev.performOutput(outIndex(Out::Speed)) == 0.0f);
}

TEST_CASE("Perf Speed is the mean speed, whatever size of step the puck arrives in") {
	// **THE WHOLE REASON IT IS AN AVERAGE AND NOT A PEAK.** The puck arrives in steps — a UI drag event
	// every ~16 ms, host automation once a block — so the per-tick distance is a spike followed by zeros.
	// A peak-follower turns that granularity into loudness and reads a coarser host as a FASTER hand.
	Sklitter smooth;
	Sklitter stepped;
	smooth.prepare(48000.0, 4096);
	stepped.prepare(48000.0, 4096);
	const float a = driveSpeed(smooth, 48000.0, 1.25, 1.0, 0.0);
	const float b = driveSpeed(stepped, 48000.0, 1.25, 1.0, 0.016); // a UI drag event rate

	// THE VALUE, RESTATED rather than compared against another run of the same code. One second is 12.5
	// time constants, so the average has arrived.
	CHECK(a == doctest::Approx(saturate(1.25)).epsilon(0.01));
	// **AND GRANULARITY IS NOT LOUDNESS.** MEASURED on this build rather than modelled — a first draft of
	// this comment predicted the two would sit about 8% apart, from a sawtooth argument about the average
	// being kicked up by each step and decaying by exp(-0.016/0.080) = 0.82 between them. **They sit
	// 0.40% apart: 0.386165 stepped against 0.384614 smooth**, at `kPerformTickSamples` = 64. (At 32 it
	// was 0.16%, 0.385218 against the same 0.384614 — the smooth reading did not move when the tick rate
	// halved, which is the Δt-based coefficient doing what it claims.) The shape was right and the
	// magnitude was not, which is why the band below is generous rather than tight: what it has to
	// survive is the PHASE of that ripple, which depends on where in a step the drive happens to stop,
	// and one reading does not measure a phase. What 0.1 still kills is the thing the case is for, and
	// that number is measured too: the peak-follower known-bad reads this drive at 0.873423 against
	// 0.384615 (0.931909 at a 32-sample tick).
	CHECK(b == doctest::Approx(a).epsilon(0.1));
}

TEST_CASE("Perf Speed agrees across sample rates after the same elapsed time") {
	// Δt is `kPerformTickSamples / sampleRate` and the coefficient is derived from τ, so these two run a
	// different NUMBER of ticks over the same second and must still land in the same place.
	Sklitter lo;
	Sklitter hi;
	lo.prepare(44100.0, 4096);
	hi.prepare(96000.0, 4096);
	const float slow = driveSpeed(lo, 44100.0, 1.0, 1.0, 0.0);
	const float fast = driveSpeed(hi, 96000.0, 1.0, 1.0, 0.0);
	CHECK(slow == doctest::Approx(saturate(1.0)).epsilon(0.01)); // not vacuous: both are the right value
	CHECK(fast == doctest::Approx(slow).epsilon(0.01));
}

TEST_CASE("Perf Speed falls away once the puck stops, and reset forgets where it was") {
	Sklitter dev;
	dev.prepare(48000.0, 4096);
	REQUIRE(driveSpeed(dev, 48000.0, 2.0, 0.5, 0.0) > 0.4f); // not vacuous: it was moving

	// FIVE TIME CONSTANTS OF STILLNESS. exp(-5) is 0.0067, so 2 units/s decays to about 0.0135 and the
	// saturation of that is 0.0067 — comfortably under the bar below, which is what makes the bar a
	// statement about the decay rather than about the arithmetic being close to it.
	const int ticks = static_cast<int>(5.0 * kSpeedTauSeconds * 48000.0 / kPerformTickSamples);
	for (int i = 0; i < ticks; ++i) {
		dev.tickPerform();
	}
	CHECK(dev.performOutput(outIndex(Out::Speed)) < 0.01f);

	// **AND A RESET FORGETS THE POSITION, NOT ONLY THE READING.** A reset that zeroed `speed_` and kept
	// the last position would publish 0 for exactly one tick and then read the whole teleport as travel.
	dev.reset();
	const Row row = puckRow(-0.9f, 0.0f);
	dev.setPerformFields(row.data(), row.size());
	dev.tickPerform();
	CHECK(dev.performOutput(outIndex(Out::Speed)) == 0.0f);
	// ONE MORE TICK WITH THE PUCK STILL: the remembered position is the new one, so there is nothing to
	// measure. Without this the case could pass on a reset that merely delays the spike by a tick.
	dev.tickPerform();
	CHECK(dev.performOutput(outIndex(Out::Speed)) == 0.0f);
}

//--------------------------------------------------------------------------------------------------//
// ANCHOR — backlog HT: bake where the puck has taken the sound into the patch, and centre the puck.   //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE PUSH THE PLAN TAKES FOR A SETTINGS — the device's own composition, restated from its two public parts.
MacroPush planPushOf(const Settings& s) {
	return totalPushOf(s, performPushOf(s, performWeightsOf(s.perfX, s.perfY), s.perfDepth));
}

/// AN ANCHOR PRESS APPLIED to a settings, the way the panel writes it.
Settings anchoredFrom(const Settings& base) {
	const AnchorPress press = anchorPressOf(base);
	Settings after = base;
	after.anchor = press.anchor;
	after.perfX = press.perfX;
	after.perfY = press.perfY;
	return after;
}

bool sameBits(const MacroPush& a, const MacroPush& b) { return std::memcmp(&a, &b, sizeof(MacroPush)) == 0; }

} // namespace

TEST_CASE("HT: an anchor press keeps the push the plan takes, term for term, and centres the puck") {
	// THE VALUE, NOT THE SOUND: the plan is a function of its push, so the same push to the bit is the same plan.
	struct Place {
		float x, y, depth;
	};
	for (const Place p : { Place { 0.6f, -0.3f, 1.0f }, Place { -0.9f, 0.9f, 0.7f }, Place { 0.05f, 0.0f, 0.4f },
	                       Place { 0.0f, -1.0f, 1.0f } }) {
		CAPTURE(p.x);
		CAPTURE(p.y);
		CAPTURE(p.depth);
		Settings base;
		base.perfX = p.x;
		base.perfY = p.y;
		base.perfDepth = p.depth;
		base.cornerCfg = { 0.31f, 1.7f, 2.45f, 0.83f };
		REQUIRE(anchorWouldMove(base));
		const MacroPush before = planPushOf(base);
		// NOT VACUOUS: the puck's push here is not the centre's, so a press that dropped it would differ.
		REQUIRE_FALSE(before == centrePush());

		const Settings after = anchoredFrom(base);
		CHECK(after.perfX == 0.0f);
		CHECK(after.perfY == 0.0f);
		CHECK(anchored(after.anchor));
		CHECK(sameBits(planPushOf(after), before));
		CHECK_FALSE(anchorWouldMove(after)); // the button dims: a second press would change nothing

		// AND AGAIN FROM THERE: the puck moved off the new home and anchored once more keeps THAT push.
		Settings moved = after;
		moved.perfX = -0.4f;
		moved.perfY = 0.5f;
		const MacroPush second = planPushOf(moved);
		REQUIRE_FALSE(sameBits(second, before));
		CHECK(sameBits(planPushOf(anchoredFrom(moved)), second));
	}
}

TEST_CASE("HT: after an anchor the puck moves AWAY FROM the anchored push — the anchored push plus the puck's departure from the centre") {
	Settings base;
	base.perfX = 0.7f;
	base.perfY = 0.2f;
	const Settings after = anchoredFrom(base);
	Settings east = after;
	east.perfX = 1.0f;
	east.perfY = 0.0f;
	const MacroPush puck = performPushOf(east, performWeightsOf(east.perfX, east.perfY), east.perfDepth);
	const MacroPush home = anchorPushOf(after.anchor);
	const MacroPush centre = centrePush();
	const MacroPush got = planPushOf(east);
	CHECK(got.dryThreshold == home.dryThreshold + (puck.dryThreshold - centre.dryThreshold));
	CHECK(got.dReverse == home.dReverse + (puck.dReverse - centre.dReverse));
	CHECK(got.dPitch == home.dPitch + (puck.dPitch - centre.dPitch));
	CHECK(got.dDelay == home.dDelay + (puck.dDelay - centre.dDelay));
	CHECK(got.gatePull == home.gatePull + (puck.gatePull - centre.gatePull));
	CHECK(got.subdivMix == home.subdivMix + (puck.subdivMix - centre.subdivMix));
	CHECK(got.zoneAPush == home.zoneAPush + (puck.zoneAPush - centre.zoneAPush));
	CHECK(got.zoneBPush == home.zoneBPush + (puck.zoneBPush - centre.zoneBPush));
}

TEST_CASE("HT: a device never anchored takes the puck's push UNTOUCHED, to the bit, so the default is today's sound") {
	for (const float x : { 0.0f, 0.3f, -1.0f }) {
		Settings s;
		s.perfX = x;
		s.perfY = -0.25f;
		const MacroPush puck = performPushOf(s, performWeightsOf(s.perfX, s.perfY), s.perfDepth);
		CHECK(sameBits(totalPushOf(s, puck), puck));
	}
	// AND THE DESCRIPTOR DECLARES THE ROW STATE the anchor is stored in, under its own key.
	CHECK(kDescriptor.traits.extraState == kAnchorFloats);
	CHECK(std::string(kDescriptor.traits.extraStateKey) == "perform_anchor");
}

TEST_CASE("HT: the device reads the anchored push from the row past its params, and a row without it is unanchored") {
	Settings base;
	base.perfX = 0.5f;
	base.perfY = 0.5f;
	const Settings after = anchoredFrom(base);
	std::vector<float> fields(kAnchorFirst + kAnchorFloats);
	const Row row = rowOf(after);
	std::copy(row.begin(), row.end(), fields.begin());
	std::copy(after.anchor.begin(), after.anchor.end(), fields.begin() + static_cast<std::ptrdiff_t>(kAnchorFirst));

	Sklitter device;
	Sklitter* dev = &device;
	dev->prepare(48000.0, 512);
	kAudioOps.setFields(dev, fields.data(), fields.size());
	CHECK(sameBits(dev->targetPushForTest(), planPushOf(base)));
	// THE SAME ROW WITHOUT THE NINE FLOATS (a patch from before the anchor): the centred puck's own push.
	kAudioOps.setFields(dev, fields.data(), kAnchorFirst);
	CHECK(sameBits(dev->targetPushForTest(), centrePush()));
}
