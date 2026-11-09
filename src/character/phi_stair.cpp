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

#include "character/phi_stair.hpp"
#include <algorithm>
#include <cmath>

namespace owlet::dsp {

// ============================================================================
// Zone -> staircase builder (runs only on zone/gamma change)
// ============================================================================

namespace {


float stairSpatial(double zonePhase, float stepFrac, float cycles, const phi::PhiTriConfig& cfg) {
	float wrapped = phi::wrapPhase(zonePhase * static_cast<double>(cfg.phiFreq) + static_cast<double>(stepFrac * cycles)
	                               + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return owlet::dsp::triangleFloat(wrapped, cfg.duty);
	}
	return owlet::dsp::triangleSimpleUnipolar(wrapped, cfg.duty);
}

// Height pattern families per zone: Brick, Terrace, Ramp, Mesa, Pylon,
// Glyph, Shard, Teeth
float stairPattern(int32_t family, float t, double phase) {
	switch (family) {
	case 0: // BRICK: alternation (square-class)
		return (static_cast<int32_t>(t * 16.0f) & 1) ? -0.8f : 0.8f;
	case 1: // TERRACE: monotonic climb (saw-stair)
		return t * 2.0f - 1.0f;
	case 2: // RAMP: gentle climb, meant for high slopes
		return t * 1.6f - 0.8f;
	case 3: // MESA: up, plateau, down
		return (t < 0.3f) ? (t / 0.3f) : ((t < 0.7f) ? 1.0f : (1.0f - (t - 0.7f) / 0.3f)) * 1.6f - 0.8f;
	case 4: // PYLON: tall center
		return (std::abs(t - 0.5f) < 0.2f) ? 0.9f : -0.45f;
	case 5: // GLYPH: phi-random quantized levels
		return std::round(stairSpatial(phase, t, 7.0f, {phi::kPhi300, 0.9f, 0.230f, true}) * 3.0f) * (1.0f / 3.0f);
	case 6: // SHARD: big asymmetric jumps
		return stairSpatial(phase, t, 3.0f, {phi::kPhi200, 0.5f, 0.510f, true});
	default: // TEETH: rapid pair alternation with lean
		return ((static_cast<int32_t>(t * 16.0f) & 2) ? 0.7f : -0.7f) + t * 0.5f - 0.25f;
	}
}

} // namespace

PhiStairParams buildPhiStairParams(uint16_t zone, float phaseOffset) {
	PhiStairParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);
	int32_t family = std::min(static_cast<int32_t>(7), static_cast<int32_t>(zone >> 7));

	// Continuous step count: the last step's width fades in fractionally
	float countF = 2.0f + phi::evalTriangle(phase, 1.0f, kPhiStairCount) * 14.0f;
	p.slope = phi::evalTriangle(phase, 1.0f, kPhiStairSlope);
	p.slope = p.slope * p.slope * 0.9f; // Squared: most of the map stays steppy
	if (family == 2) {
		p.slope = 0.3f + p.slope * 0.6f; // RAMP zone is meant to be smooth
	}

	float tilt = phi::evalTriangle(phase, 1.0f, kPhiStairTilt) * 0.6f;
	float asym = phi::evalTriangle(phase, 1.0f, kPhiStairAsym);
	float cycles = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiStairLandCycles) * 3.0f;

	float peak = 0.0001f;
	for (int32_t i = 0; i < kPhiStairSteps; i++) {
		float t = (static_cast<float>(i) + 0.5f) / countF;
		float h = stairPattern(family, std::min(t, 1.0f), phase)
		          + 0.35f * stairSpatial(phase, t, cycles, kPhiStairHeightLand) + tilt * (t * 2.0f - 1.0f);
		p.heights[i] = h;
		peak = std::max(peak, std::abs(h));

		// Width: even split warped by the asymmetry landscape, with the
		// fractional-count fade keeping the step count CONTINUOUS
		float wLand = 1.0f + asym * 0.85f * stairSpatial(phase, t, cycles * 2.0f, kPhiStairAsymLand);
		float countFade = std::clamp(countF - static_cast<float>(i), 0.0f, 1.0f);
		p.widths[i] = std::max(wLand, 0.1f) * countFade;
	}
	float norm = 1.0f / peak;
	for (float& h : p.heights) {
		h *= norm;
	}

	return p;
}

} // namespace owlet::dsp
