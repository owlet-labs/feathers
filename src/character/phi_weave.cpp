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

#include "character/phi_weave.hpp"
#include "character/util.hpp"
#include "dsp/portable_trig.h"

#include <algorithm>
#include <cmath>

namespace owlet::dsp {

namespace {

constexpr float kTwoPi = 6.283185307f;

// Spatial landscape: a phi triangle evaluated around the ring. The phase places
// the landscape (every phase paints different terrain); spatialCycles sets how
// many bumps fit around the loop. NOTE the form differs from evalTriangle's
// (phase + offset) * freq: here the offset and the node term are added AFTER the
// frequency multiply. That is the firmware's arithmetic and is kept.
float evalSpatial(double phase, float nodeFrac, float spatialCycles, const phi::PhiTriConfig& cfg) {
	float wrapped =
	    phi::wrapPhase(phase * static_cast<double>(cfg.phiFreq) + static_cast<double>(nodeFrac * spatialCycles)
	                   + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return triangleFloat(wrapped, cfg.duty);
	}
	return triangleSimpleUnipolar(wrapped, cfg.duty);
}

} // namespace

PhiWeaveParams buildPhiWeaveLaws(double phase) {
	PhiWeaveParams p{};

	// Global scalars
	float stiffT = phi::evalTriangle(phase, 1.0f, kPhiWeaveStiffBase);
	float stiffBase = 0.0008f * std::pow(75.0f, stiffT); // 0.0008..0.06, exponential
	float coupleT = phi::evalTriangle(phase, 1.0f, kPhiWeaveCoupleBase);
	float coupleBase = 0.010f + coupleT * coupleT * 0.28f;
	float dampT = phi::evalTriangle(phase, 1.0f, kPhiWeaveDampBase);
	float dampBase = 0.0015f + dampT * dampT * 0.0435f;

	// Home shape partials
	float p2 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP2) * 0.7f;
	float p3 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP3) * 0.55f;
	float p5 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP5) * 0.4f;
	float ph2 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomePh2) * kTwoPi;
	float ph3 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomePh3) * kTwoPi;

	// Spatial cycle counts derived from the phase too (1..4 bumps around the ring)
	float stiffCycles = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiWeaveStiffCycles) * 3.0f;
	float dampCycles = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiWeaveDampCycles) * 3.0f;

	float homePeak = 0.0f;
	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);

		float stiffLand = 1.0f + 0.8f * evalSpatial(phase, nf, stiffCycles, kPhiWeaveStiffLand1)
		                  + 0.5f * evalSpatial(phase, nf, stiffCycles * 2.0f, kPhiWeaveStiffLand2);
		// Floors raised from sim-validated full-map sweep: stiffness anchors
		// the home-shape carrier (without it, low-k zones wander so far the
		// churn sidebands rival the carrier); damping bounds the pluck's
		// free-ring to well under a second (tau = 1/d was ~6s at the old floor)
		p.stiffness[i] = std::clamp(stiffBase * stiffLand, 0.016f, 0.12f);

		float dampLand = 1.0f + 0.85f * evalSpatial(phase, nf, dampCycles, kPhiWeaveDampLand1)
		                 + 0.5f * evalSpatial(phase, nf, dampCycles * 3.0f, kPhiWeaveDampLand2);
		p.damping[i] = std::clamp(dampBase * dampLand, 0.006f, 0.09f);
		p.bowBalance[i] = 0.60f * std::min(1.0f, std::sqrt(p.damping[i] * (1.0f / 0.0255f)));

		float coupleLand = 1.0f + 0.6f * evalSpatial(phase, nf, stiffCycles, kPhiWeaveCoupleLand);
		p.coupling[i] = std::clamp(coupleBase * coupleLand, 0.004f, 0.45f);

		// THE PLATFORM-EXACT SINE (backlog LP, 2026-10-06): the home shape is the ring's rest, and a C library's sinf
		// rounding one of these arguments the other way gave macOS a ring a few ulps off Linux's, which its chaotic bow
		// grew into a different buzz. portableSin is the correctly rounded float on every host.
		float h = feathers::dsp::portableSin(kTwoPi * nf) + p2 * feathers::dsp::portableSin(2.0f * kTwoPi * nf + ph2)
		          + p3 * feathers::dsp::portableSin(3.0f * kTwoPi * nf + ph3) + p5 * feathers::dsp::portableSin(5.0f * kTwoPi * nf);
		p.home[i] = h;
		homePeak = std::max(homePeak, std::abs(h));
	}

	// Normalize the home shape to +/-1 peak — it is the baseline timbre the
	// string relaxes toward, so it sets the resting loudness of every phase
	if (homePeak > 0.01f) {
		float norm = 1.0f / homePeak;
		for (float& h : p.home) {
			h *= norm;
		}
	}

	float bowDepthT = phi::evalTriangle(phase, 1.0f, kPhiWeaveBowDepth);
	p.bowDepth = 0.0015f + bowDepthT * bowDepthT * 0.030f;
	// Bow oscillation capped ~16 Hz: faster bowing stamped inharmonic
	// sidebands on every partial (excitation must be haptic-rate too)
	p.bowRate = 0.002f + phi::evalTriangle(phase, 1.0f, kPhiWeaveBowRate) * 0.045f;
	const float modeCount = static_cast<float>(kWeaveBowModeCount);
	p.bowMode = static_cast<WeaveBow>(static_cast<uint8_t>(
	    std::min(modeCount - 1.0f, phi::evalTriangle(phase, 1.0f, kPhiWeaveBowMode) * modeCount)));
	p.bowPos = phi::evalTriangle(phase, 1.0f, kPhiWeaveBowPos);
	p.bowSpread = 0.06f + phi::evalTriangle(phase, 1.0f, kPhiWeaveBowSpread) * 0.20f;

	float travelT = phi::evalTriangle(phase, 1.0f, kPhiWeaveTravel);
	p.travelRate = travelT * travelT * travelT * 0.004f; // Cubic: mostly near-still, occasionally drifting

	p.pluckPos = phi::evalTriangle(phase, 1.0f, kPhiWeavePluckPos);
	p.pluckWidth = 0.03f + phi::evalTriangle(phase, 1.0f, kPhiWeavePluckWidth) * 0.20f;
	p.pluckAmp = 0.4f + phi::evalTriangle(phase, 1.0f, kPhiWeavePluckAmp) * 0.6f;

	p.morphBowGain = 0.5f + phi::evalTriangle(phase, 1.0f, kPhiWeaveMorphBow) * 4.0f;
	p.outGain = 0.70f + phi::evalTriangle(phase, 1.0f, kPhiWeaveOutGain) * 0.55f;

	// 0.07..0.30 exponential: scan-smoother cutoff ~8..33 Hz at the sub-tick rate
	float shimmerT = phi::evalTriangle(phase, 1.0f, kPhiWeaveShimmer);
	p.shimmerAlpha = 0.07f * std::pow(4.3f, shimmerT);

	return p;
}

} // namespace owlet::dsp
