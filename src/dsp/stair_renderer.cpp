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

#include "dsp/stair_renderer.h"

#include "dsp/morph_renderer.h"

#include <algorithm>
#include <cmath>

namespace feathers {

using owlet::dsp::kPhiStairSlots;
using owlet::dsp::kPhiStairSteps;
using owlet::dsp::PhiStairParams;

namespace {

// The original fixed-point Stair table build scales its finished slots by 2^30-1 and the render then doubles the amplitude,
// so a zone that reaches the rule's peak cap sits at 0.95 of HALF q31 full scale. Normalising against
// the full scale — the same rail the morph evaluator uses — therefore puts a loud zone at ±0.475 and
// leaves the ±1.0 clip as headroom, which is the convention both evaluators share.
constexpr float kSlotScale = 1073741823.0f;
constexpr float kQ31FullScale = 2147483647.0f;
constexpr float kSlotToFullScale = kSlotScale / kQ31FullScale;

// The threshold the original fixed-point Stair table build uses to decide a slope is worth ramping. Below it every step is
// a hard edge, which is the pulse-class end of the character rather than a rounding detail, so it has
// to be the same number and not merely a small one.
constexpr float kMinSlope = 0.001f;

/// The staircase itself, before the loudness rule: the value the firmware would write into a slot.
float rawEval(const StairTable& t, double u) {
	// The first step whose interval ends past u. The original fixed-point Stair table build walks this incrementally because
	// its slots arrive in order; evaluation is random-access, so it scans. Sixteen comparisons is
	// cheaper than the branch misprediction a search over so few entries would trade them for.
	int seg = kPhiStairSteps - 1;
	for (int i = 0; i < kPhiStairSteps; i++) {
		if (u < t.boundaries[i + 1]) {
			seg = i;
			break;
		}
	}

	const double width = t.boundaries[seg + 1] - t.boundaries[seg];
	if (width <= 0.0 || t.slope <= kMinSlope) {
		return t.heights[seg];
	}

	// The riser sits at the START of the step and ramps from where the PREVIOUS step was holding to
	// this step's own height, taking `slope` of the width to get there; the rest of the step holds.
	// Step 0 rises from the last step that still has width, which is what joins the cycle end to end.
	const double frac = (u - t.boundaries[seg]) / width;
	if (frac >= static_cast<double>(t.slope)) {
		return t.heights[seg];
	}
	const float from = (seg == 0) ? t.heights[t.lastActive] : t.heights[seg - 1];
	return from + (t.heights[seg] - from) * static_cast<float>(frac / static_cast<double>(t.slope));
}

/// One circular (1, 2, 1)/4 pass across the 128-slot grid, then the guard slot.
///
/// This is the original fixed-point Stair table build's binomial in float. That one works in q31 and shifts right by
/// two, so it loses a bit or so of resolution per pass where this does not; the shape is identical and
/// the difference is far below where the table's own quantisation sits.
///
/// The response of one pass on this grid is cos²(pi k / 128) at harmonic k, which is a hair under
/// unity for any fundamental and about -14 dB by the harmonics that were doing the aliasing.
void binomialPass(const float* src, float* dst) {
	for (int j = 0; j < kPhiStairSlots; j++) {
		const int prev = (j == 0) ? kPhiStairSlots - 1 : j - 1;
		// src[j + 1] leans on the guard slot exactly as the firmware's does, so slot 127 reads slot 0.
		dst[j] = 0.25f * (src[prev] + 2.0f * src[j] + src[j + 1]);
	}
	dst[kPhiStairSlots] = dst[0];
}

} // namespace

StairTable buildStairTable(const PhiStairParams& p) {
	StairTable t{};
	t.slope = p.slope;
	std::copy(std::begin(p.heights), std::end(p.heights), std::begin(t.heights));

	double widthSum = 0.0;
	for (float w : p.widths) {
		widthSum += static_cast<double>(w);
	}

	t.boundaries[0] = 0.0;
	t.lastActive = 0;
	if (widthSum > 0.0) {
		const double invSum = 1.0 / widthSum;
		for (int i = 0; i < kPhiStairSteps; i++) {
			t.boundaries[i + 1] = t.boundaries[i] + static_cast<double>(p.widths[i]) * invSum;
			if (p.widths[i] > 0.0f) {
				t.lastActive = i;
			}
		}
	}
	else {
		// buildPhiStairParams cannot produce this — step 0's fractional-count fade is always 1 and its
		// width floor is 0.1 — but a hand-built or crossfaded params could, and a zero-width cycle would
		// otherwise divide by zero. One step across the whole cycle is the only sensible reading.
		for (int i = 1; i <= kPhiStairSteps; i++) {
			t.boundaries[i] = 1.0;
		}
	}
	// Exact rather than accumulated, so the last step ends where the cycle does.
	t.boundaries[kPhiStairSteps] = 1.0;

	// The loudness rule, lifted from the original fixed-point Stair table build: peak-normalised heights alone left sparse
	// patterns far quieter than dense ones, so the finished waveform is DC-stripped and then scaled to
	// a saw-class RMS with a hard peak cap. It is measured where the firmware measures it, on 128
	// evenly spaced samples — that is the character. Measuring it continuously instead would be a
	// different, arguably better normalisation, and would make every zone quietly disagree with the
	// hardware. The cost is that a step narrower than 1/128 of the cycle can escape the measurement;
	// evalStairTable's clip catches the one zone where that matters.
	float slots[kPhiStairSlots];
	float mean = 0.0f;
	for (int j = 0; j < kPhiStairSlots; j++) {
		slots[j] = rawEval(t, static_cast<double>(j) / static_cast<double>(kPhiStairSlots));
		mean += slots[j];
	}
	mean *= 1.0f / static_cast<float>(kPhiStairSlots);
	t.mean = mean;

	float peak = 0.0001f;
	float sumSq = 0.0f;
	for (float slot : slots) {
		const float d = slot - mean;
		peak = std::max(peak, std::abs(d));
		sumSq += d * d;
	}
	const float rms = std::sqrt(sumSq * (1.0f / static_cast<float>(kPhiStairSlots)));
	t.scale = std::min(0.55f / std::max(rms, 0.05f), 0.95f / peak) * kSlotToFullScale;

	// Keep the normalised slots: they are the grid the voice path actually renders through, and they
	// are already sitting here. The firmware clamps at 2147483000 rather than q31 full scale to guard
	// its float-to-int conversion; ±1.0 is the same rail expressed in the plugin's units.
	for (int j = 0; j < kPhiStairSlots; j++) {
		t.slots[j] = std::clamp((slots[j] - mean) * t.scale, -1.0f, 1.0f);
	}
	t.slots[kPhiStairSlots] = t.slots[0]; // The scan's idx+1 guard, exactly as the fixed-point slot table carried it.

	// The anti-alias mips, which the firmware labels the WEAVE pattern: a circular three-tap binomial
	// across the grid, once for mip1 and again for mip2. No decimation — the grid stays 128 slots and
	// only gets duller.
	binomialPass(t.slots, t.mip1);
	binomialPass(t.mip1, t.mip2);

	return t;
}

float evalStairTable(const StairTable& table, double phase01) {
	const double wrapped = phase01 - std::floor(phase01);
	// The firmware rails at 2147483000 rather than at q31 full scale, guarding its float-to-int32
	// conversion; 3e-7 of full scale below 1.0 is not a rail anyone can hear, and clipping where the
	// morph evaluator clips keeps one meaning of "±1.0" across the plugin.
	return std::clamp((rawEval(table, wrapped) - table.mean) * table.scale, -1.0f, 1.0f);
}

float evalStairTable(const PhiStairParams& p, double phase01) {
	return evalStairTable(buildStairTable(p), phase01);
}

float evalStairGrid(const float* slots, double phase01) {
	const double wrapped = phase01 - std::floor(phase01);
	const double u = wrapped * static_cast<double>(kPhiStairSlots);

	// The firmware gets its index by shifting a uint32 phase down by 25 bits, so the index can never
	// leave range however the phase rounded. Coming from a double, a wrapped phase of 1 - 1e-17 rounds
	// up to exactly 128.0, so the clamp stands in for what the shift gives the firmware for free.
	int idx = static_cast<int>(u);
	if (idx >= kPhiStairSlots) {
		idx = kPhiStairSlots - 1;
	}

	const float frac = static_cast<float>(u - static_cast<double>(idx));
	return slots[idx] + (slots[idx + 1] - slots[idx]) * frac;
}

float evalStairSlots(const StairTable& table, double phase01) {
	return evalStairGrid(table.slots, phase01);
}

int stairMipForFrequency(double frequencyHz) {
	// The firmware's own constants, converted once rather than rounded to the 700 and 1800 its comment
	// names. A phase increment is frequency over sample rate times 2^32 and the hardware runs at 44100,
	// which puts these at 699.999989 Hz and 1799.999994 Hz — so the round numbers were the intent, and
	// keeping the constants means the switch lands where the hardware's lands rather than 1e-5 Hz off.
	constexpr double kPhaseScale = 4294967296.0;
	constexpr double kHardwareRate = 44100.0;
	constexpr double kMip1Hz = 68174083.0 / kPhaseScale * kHardwareRate;
	constexpr double kMip2Hz = 175304787.0 / kPhaseScale * kHardwareRate;

	if (frequencyHz > kMip2Hz) {
		return 2;
	}
	if (frequencyHz > kMip1Hz) {
		return 1;
	}
	return 0;
}

const float* stairMipSlots(const StairTable& table, int mipLevel) {
	switch (mipLevel) {
	case 2:
		return table.mip2;
	case 1:
		return table.mip1;
	default:
		return table.slots;
	}
}

PhiStairParams buildStairXfade(const PhiStairParams& a, const PhiStairParams& b, float xf01) {
	const float cf = std::clamp(xf01, 0.0f, 1.0f);
	const float cfInv = 1.0f - cf;

	PhiStairParams out{};
	for (int i = 0; i < kPhiStairSteps; i++) {
		out.heights[i] = cfInv * a.heights[i] + cf * b.heights[i];
		out.widths[i] = cfInv * a.widths[i] + cf * b.widths[i];
	}
	out.slope = cfInv * a.slope + cf * b.slope;
	return out;
}

PhiStairParams buildStairBlend(std::span<const PhiStairParams> params, std::span<const double> weights,
                               const BlueShift& blue) {
	PhiStairParams out{};
	const size_t n = std::min(params.size(), weights.size());
	if (n == 0) {
		return out;
	}

	// The shares leaned the way you are travelling, before anything reads them — the same call the
	// morph blend makes, because the tilt is about the constellation and not about either recipe.
	double tilted[kMaxSounds]{};
	if (n <= kMaxSounds && tiltWeights(weights, blue, tilted, n)) {
		weights = { tilted, n };
	}

	for (size_t k = 0; k < n; ++k) {
		const auto w = static_cast<float>(weights[k]);
		for (int i = 0; i < kPhiStairSteps; i++) {
			out.heights[i] += w * params[k].heights[i];
			out.widths[i] += w * params[k].widths[i];
		}
		// BRIGHTNESS LEANS THE OTHER WAY HERE, and it is not a sign error. `slope` is the fraction of a
		// step spent ramping to its height, so a SHORTER ramp is a sharper edge and a brighter sound —
		// where the morph side brightens by taking MORE curvature, this brightens by taking LESS slope.
		// A sound you are closing on contributes a crisper version of itself either way.
		//
		// WHERE THE RECIPE HAS NO RAMP THERE IS NOTHING TO SHARPEN, and that is a real limit rather than
		// a gap to paper over: 39% of zones come out of the builder with slope exactly 0 — hard-edged
		// staircases — and a multiplicative lean leaves them hard-edged in both directions. The other
		// two halves of blue shift (the weight tilt and the height overshoot) still reach those zones,
		// so the feature is live everywhere; it is this one flavour that has less to say on a step with
		// no ramp in it. Inventing a ramp for it would be turning a hard edge soft while you move,
		// which is a bigger change than the rule asks for.
		const double lean = blue.approach.size() > k ? 1.0 - blue.approach[k] : 1.0;
		out.slope += static_cast<float>(static_cast<double>(w) * std::max(0.15, lean))
		             * params[k].slope;
	}

	// THE VELOCITY-AIMED OVERSHOOT, on the quantity that carries a staircase's amplitude. Pushed
	// DIFFERENTIALLY — each step by its own weighted rate — so what moves is the shape of the pattern.
	// A uniform push would be spent for nothing: buildStairTable strips DC and normalises RMS, so a
	// staircase scaled bodily comes out where it went in.
	//
	// Gated by the spread of the weights, exactly as the morph excitation is, so the push vanishes when
	// one sound has all of the blend — standing on an anchor is that sound and nothing else, however
	// fast you arrived.
	// Held inside the range the builder itself produces. Above 1 the fraction stops meaning anything —
	// a step whose ramp is longer than the step never arrives at its own height — and the widest zone
	// the character layer makes is 0.894, so this only ever catches a lean pushed past the recipe.
	out.slope = std::clamp(out.slope, 0.0f, 0.95f);

	if (blue.overshoot != 0.0f && blue.weightRate.size() >= n) {
		const float spread = blendExcite(weights.first(n));
		for (int i = 0; i < kPhiStairSteps; i++) {
			double along = 0.0;
			for (size_t k = 0; k < n; ++k) {
				along += blue.weightRate[k] * static_cast<double>(params[k].heights[i]);
			}
			out.heights[i] += spread * static_cast<float>(along) * blue.overshoot;
		}
	}
	return out;
}

StairTable buildStairXfadeTable(const PhiStairParams& a, const PhiStairParams& b, float xf01) {
	return buildStairTable(buildStairXfade(a, b, xf01));
}

} // namespace feathers
