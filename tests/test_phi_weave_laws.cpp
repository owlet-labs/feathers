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

// The weave LAWS (src/character/phi_weave): the float port of the firmware's buildPhiWeaveParams.
// The scalar case restates each formula from the firmware's text (reference firmware 2567b04c,
// dsp/phi_weave.cpp:53-136) with the bank configs written out as literals from its header — NOT
// from the port's constants — so a transcription slip in either the formula or a config shows here.
// Comparisons are EXACT (==), not Approx: the restatement runs the same float operations as the port,
// and doctest's Approx adds a scale of 1.0 to its epsilon, which would pass a 0.0015 damping against
// anything under 1e-3.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/portable_trig.h"

#include <algorithm>
#include <cmath>

namespace {

using owlet::dsp::buildPhiWeaveLaws;
using owlet::dsp::kPhiWeaveNumNodes;
using owlet::dsp::PhiWeaveParams;
namespace phi = owlet::dsp::phi;

constexpr int kSweep = 4096;

float tri(double phase, float freq, float duty, float offset, bool bipolar) {
	return phi::evalTriangle(phase, 1.0f, phi::PhiTriConfig { freq, duty, offset, bipolar });
}

} // namespace

// Nine phases chosen so that every scalar's triangle is MID-RANGE (0.25 < |t| < 0.8) at one of them at
// least — at a phase where a triangle sits at 0 or at its floor, a slipped coefficient multiplies
// nothing and cannot show. The coverage itself is asserted below, so a future edit to the configs that
// empties it goes red here rather than silently weakening the case.
constexpr double kScalarPhases[] = { 0.0, 0.05, 0.137, 0.21, 0.33, 0.5, 0.618, 0.77, 0.9 };

TEST_CASE("weave laws: scalars match the firmware formulas at nine phases") {
	// Every triangle the scalars and the home shape read, as literals from phi_weave.hpp:72-136.
	const phi::PhiTriConfig cfgs[] = {
		{ phi::kPhi125, 0.6f, 0.000f, false },  { phi::kPhiN050, 0.7f, 0.210f, false },
		{ phi::kPhi250, 0.5f, 0.420f, false },  { phi::kPhi350, 0.55f, 0.190f, false },
		{ phi::kPhi100, 0.8f, 0.370f, false },  { phi::kPhi067, 0.5f, 0.250f, true },
		{ phi::kPhi250, 0.6f, 0.820f, false },  { phi::kPhiN025, 0.9f, 0.560f, false },
		{ phi::kPhi067, 0.6f, 0.240f, false },  { phi::kPhi200, 1.0f, 0.740f, false },
		{ phi::kPhiN100, 0.7f, 0.880f, false }, { phi::kPhi375, 1.0f, 0.090f, false },
		{ phi::kPhi033, 0.7f, 0.440f, false },  { phi::kPhi175, 0.7f, 0.150f, false },
		{ phi::kPhi150, 0.6f, 0.480f, true },   { phi::kPhi325, 0.5f, 0.760f, true },
		{ phi::kPhi050, 0.35f, 0.900f, true },  { phi::kPhi025, 1.0f, 0.320f, false },
		{ phi::kPhi275, 1.0f, 0.610f, false },
	};
	for (const phi::PhiTriConfig& c : cfgs) {
		bool mid = false;
		for (double phase : kScalarPhases) {
			const float t = std::abs(phi::evalTriangle(phase, 1.0f, c));
			mid = mid || (t > 0.25f && t < 0.8f);
		}
		CAPTURE(c.phiFreq);
		CAPTURE(c.phaseOffset);
		CHECK(mid);
	}

	for (double phase : kScalarPhases) {
		CAPTURE(phase);
		const PhiWeaveParams p = buildPhiWeaveLaws(phase);

		// Restated from the firmware text: configs from phi_weave.hpp:72-136, formulas from
		// phi_weave.cpp:59-133. EXACT equality: the port and this restatement perform the same float
		// operations in the same order under -ffp-contract=off, so any difference is a transcription
		// difference, not rounding.
		const float stiffT = tri(phase, phi::kPhi125, 0.6f, 0.000f, false);
		const float stiffBase = 0.0008f * std::pow(75.0f, stiffT);
		const float coupleT = tri(phase, phi::kPhiN050, 0.7f, 0.210f, false);
		const float coupleBase = 0.010f + coupleT * coupleT * 0.28f;
		const float dampT = tri(phase, phi::kPhi250, 0.5f, 0.420f, false);
		const float dampBase = 0.0015f + dampT * dampT * 0.0435f;
		const float bowDepthT = tri(phase, phi::kPhi350, 0.55f, 0.190f, false);
		const float bowDepth = 0.0015f + bowDepthT * bowDepthT * 0.030f;
		const float bowRate = 0.002f + tri(phase, phi::kPhi100, 0.8f, 0.370f, false) * 0.045f;
		const float travelT = tri(phase, phi::kPhi067, 0.5f, 0.250f, true);
		const float travelRate = travelT * travelT * travelT * 0.004f;
		const float pluckAmp = 0.4f + tri(phase, phi::kPhi250, 0.6f, 0.820f, false) * 0.6f;
		const float outGain = 0.70f + tri(phase, phi::kPhiN025, 0.9f, 0.560f, false) * 0.55f;
		const float shimmerAlpha = 0.07f * std::pow(4.3f, tri(phase, phi::kPhi067, 0.6f, 0.240f, false));
		const float bowPos = tri(phase, phi::kPhi200, 1.0f, 0.740f, false);
		const float bowSpread = 0.06f + tri(phase, phi::kPhiN100, 0.7f, 0.880f, false) * 0.20f;
		const float pluckPos = tri(phase, phi::kPhi375, 1.0f, 0.090f, false);
		const float pluckWidth = 0.03f + tri(phase, phi::kPhi033, 0.7f, 0.440f, false) * 0.20f;
		const float morphBowGain = 0.5f + tri(phase, phi::kPhi175, 0.7f, 0.150f, false) * 4.0f;
		const int bowMode =
		    static_cast<int>(std::min(4.0f, tri(phase, phi::kPhi150, 0.9f, 0.680f, false) * 5.0f));

		CHECK(p.bowDepth == bowDepth);
		CHECK(p.bowRate == bowRate);
		CHECK(p.travelRate == travelRate);
		CHECK(p.pluckAmp == pluckAmp);
		CHECK(p.outGain == outGain);
		CHECK(p.shimmerAlpha == shimmerAlpha);
		CHECK(p.bowPos == bowPos);
		CHECK(p.bowSpread == bowSpread);
		CHECK(p.pluckPos == pluckPos);
		CHECK(p.pluckWidth == pluckWidth);
		CHECK(p.morphBowGain == morphBowGain);
		CHECK(static_cast<int>(p.bowMode) == bowMode);

		// The three bases are not stored; they reach the per-node laws through the landscapes and the
		// clamps. Restate the node laws in full at EVERY node so the bases are pinned too.
		const float stiffCycles = 1.0f + tri(phase, phi::kPhi050, 1.0f, 0.470f, false) * 3.0f;
		const float dampCycles = 1.0f + tri(phase, phi::kPhi325, 1.0f, 0.720f, false) * 3.0f;
		auto spatial = [&](float nf, float cycles, float freq, float duty, float offset) {
			const float w = phi::wrapPhase(phase * static_cast<double>(freq) + static_cast<double>(nf * cycles)
			                               + static_cast<double>(offset));
			return owlet::dsp::triangleFloat(w, duty);
		};
		for (int i = 0; i < kPhiWeaveNumNodes; ++i) {
			CAPTURE(i);
			const float nf = static_cast<float>(i) / 32.0f;
			const float stiffLand = 1.0f + 0.8f * spatial(nf, stiffCycles, phi::kPhi075, 0.8f, 0.060f)
			                        + 0.5f * spatial(nf, stiffCycles * 2.0f, phi::kPhi300, 0.5f, 0.530f);
			const float dampLand = 1.0f + 0.85f * spatial(nf, dampCycles, phi::kPhi175, 0.7f, 0.140f)
			                       + 0.5f * spatial(nf, dampCycles * 3.0f, phi::kPhiN025, 0.6f, 0.660f);
			const float coupleLand = 1.0f + 0.6f * spatial(nf, stiffCycles, phi::kPhi225, 0.6f, 0.310f);
			const float k = std::clamp(stiffBase * stiffLand, 0.016f, 0.12f);
			const float d = std::clamp(dampBase * dampLand, 0.006f, 0.09f);
			const float c = std::clamp(coupleBase * coupleLand, 0.004f, 0.45f);
			CHECK(p.stiffness[i] == k);
			CHECK(p.damping[i] == d);
			CHECK(p.coupling[i] == c);
			CHECK(p.bowBalance[i] == 0.60f * std::min(1.0f, std::sqrt(d * (1.0f / 0.0255f))));
		}
	}
}

TEST_CASE("weave laws: the home shape is the firmware's normalised partial sum at nine phases") {
	constexpr float kTwoPi = 6.283185307f;
	for (double phase : kScalarPhases) {
		CAPTURE(phase);
		const PhiWeaveParams p = buildPhiWeaveLaws(phase);
		// phi_weave.cpp:67-110: fundamental + weighted 2nd, 3rd and 5th partials, the 2nd and 3rd at
		// phase offsets, then normalised to a peak of 1 when the raw peak is above 0.01.
		const float p2 = tri(phase, phi::kPhi150, 0.6f, 0.480f, true) * 0.7f;
		const float p3 = tri(phase, phi::kPhi325, 0.5f, 0.760f, true) * 0.55f;
		const float p5 = tri(phase, phi::kPhi050, 0.35f, 0.900f, true) * 0.4f;
		const float ph2 = tri(phase, phi::kPhi025, 1.0f, 0.320f, false) * kTwoPi;
		const float ph3 = tri(phase, phi::kPhi275, 1.0f, 0.610f, false) * kTwoPi;
		float raw[kPhiWeaveNumNodes];
		float peak = 0.0f;
		for (int i = 0; i < kPhiWeaveNumNodes; ++i) {
			const float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);
			// The platform-exact sine, as phi_weave.cpp uses since backlog LP (2026-10-06): the C library's sinf rounds
			// differently on macOS, so a restatement through it would pin one platform's rounding, not the law.
			using feathers::dsp::portableSin;
			raw[i] = portableSin(kTwoPi * nf) + p2 * portableSin(2.0f * kTwoPi * nf + ph2)
			         + p3 * portableSin(3.0f * kTwoPi * nf + ph3) + p5 * portableSin(5.0f * kTwoPi * nf);
			peak = std::max(peak, std::abs(raw[i]));
		}
		REQUIRE(peak > 0.01f);
		const float norm = 1.0f / peak;
		for (int i = 0; i < kPhiWeaveNumNodes; ++i) {
			CAPTURE(i);
			CHECK(p.home[i] == raw[i] * norm);
		}
	}
}

TEST_CASE("weave laws: every per-node law stays inside the firmware clamps across 4096 phases") {
	for (int s = 0; s < kSweep; ++s) {
		const double phase = static_cast<double>(s) / kSweep;
		CAPTURE(phase);
		const PhiWeaveParams p = buildPhiWeaveLaws(phase);
		float peak = 0.0f;
		for (int i = 0; i < kPhiWeaveNumNodes; ++i) {
			REQUIRE(p.stiffness[i] >= 0.016f);
			REQUIRE(p.stiffness[i] <= 0.12f);
			REQUIRE(p.damping[i] >= 0.006f);
			REQUIRE(p.damping[i] <= 0.09f);
			REQUIRE(p.coupling[i] >= 0.004f);
			REQUIRE(p.coupling[i] <= 0.45f);
			REQUIRE(std::isfinite(p.home[i]));
			peak = std::max(peak, std::abs(p.home[i]));
		}
		// Normalised when the raw peak is above 0.01. The raw peak is never that small in practice (the
		// fundamental alone reaches 1 at node 8), so every phase lands here.
		REQUIRE(std::abs(peak - 1.0f) < 1e-5f);
	}
}

TEST_CASE("weave laws: the leapfrog stability headroom is documented") {
	// DOCUMENTS HEADROOM; IT CANNOT FAIL while the clamps hold: stiffness <= 0.12 and coupling <= 0.45
	// bound stiffness + 4 * coupling at 1.92, under the dt = 1 leapfrog's limit of 4 (the firmware header's
	// condition). What it records is how much of that the laws actually use — MEASURED 2026-09-25 over
	// 4096 phases x 32 nodes: 1.816. The clamp case above is what would catch a laws change.
	float worst = 0.0f;
	for (int s = 0; s < kSweep; ++s) {
		const PhiWeaveParams p = buildPhiWeaveLaws(static_cast<double>(s) / kSweep);
		for (int i = 0; i < kPhiWeaveNumNodes; ++i) {
			worst = std::max(worst, p.stiffness[i] + 4.0f * p.coupling[i]);
		}
	}
	MESSAGE("max stiffness + 4*coupling = " << worst);
	CHECK(worst < 4.0f);
}

TEST_CASE("weave laws: all five bow modes occur across the phase sweep") {
	int counts[owlet::dsp::kWeaveBowModeCount] = {};
	for (int s = 0; s < kSweep; ++s) {
		const int m = static_cast<int>(buildPhiWeaveLaws(static_cast<double>(s) / kSweep).bowMode);
		REQUIRE(m >= 0);
		REQUIRE(m < owlet::dsp::kWeaveBowModeCount);
		++counts[m];
	}
	for (int m = 0; m < owlet::dsp::kWeaveBowModeCount; ++m) {
		CAPTURE(m);
		CHECK(counts[m] > 0);
	}
}
