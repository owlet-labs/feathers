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

#include "dsp/morph_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace feathers {

using owlet::dsp::kPhiMorphMaxSegments;
using owlet::dsp::kPhiMorphMovableWaypoints;
using owlet::dsp::PhiMorphSegment;
using owlet::dsp::PhiMorphWavetable;

namespace {

// buildSegmentsFromWaypoints scales waypoints by kPhiMorphRefAmplitude and clamps them at Q31 full
// scale; the hardware evaluator saturates the curvature sum at the same rail. Normalising by that
// rail therefore puts a nominal waypoint at ±0.5 and the clip at exactly ±1.0.
constexpr double kQ31FullScale = 2147483647.0;

// One less than the 2^32 the builder scales its boundaries by, which costs nothing and buys an
// unconditional range guarantee: phase is wrapped into [0,1) before scaling, so the product is
// strictly below 2^32-1 and the uint32 conversion below is in range no matter what. Scaling by 2^32
// would also be safe here, but only via the subtler argument that multiplying a double by a power of
// two is exact and so can never round up to 2^32 — a guarantee that would evaporate if the scale
// ever stopped being a power of two.
//
// The cost is that a phase of exactly phases[i] can land one unit short of the boundary the builder
// wrote, picking the previous segment at frac 1 instead of the next at frac 0. That is 2.3e-10 of a
// cycle and produces the same sample either way, because a segment ends at exactly the amplitude the
// next one starts from.
constexpr double kPhaseToU32 = 4294967295.0;

} // namespace

float evalMorphTable(const PhiMorphWavetable& table, double phase01) {
	const double wrapped = phase01 - std::floor(phase01);
	const double phaseF = wrapped * kPhaseToU32;
	const auto phase = static_cast<uint32_t>(phaseF);

	// Segment lookup, same rule as the hardware: the first segment ending past this phase, else the
	// last one. Zero-width segments from coincident waypoints drop out of this for free. So does the
	// final segment's endPhase on a host where the builder's 1.0f * 4294967295.0f wrapped it to 0 —
	// the fallback catches it, which is why endPhase is never used as an arithmetic bound below.
	int seg = kPhiMorphMaxSegments - 1;
	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		if (phase < table.segments[i].endPhase) {
			seg = i;
			break;
		}
	}

	const PhiMorphSegment& s = table.segments[seg];

	// invWidth came from the uint32 difference, which wraps to the correct width even for that
	// endPhase-of-0 final segment. Unlike the hardware the offset keeps its fraction, so phase
	// resolution is the double's rather than the 1/2^32 grid.
	const double frac = (phaseF - static_cast<double>(s.startPhase)) * static_cast<double>(s.invWidth);

	// Linear ramp across the segment plus a quadratic bend that vanishes at both ends — which is why
	// the waveform stays continuous at segment boundaries and only its slope breaks there.
	const double linear = static_cast<double>(s.startAmp) + static_cast<double>(s.ampDeltaF) * frac;
	const double quadTerm = frac * (1.0 - frac) * 4.0;
	const double value = linear + static_cast<double>(s.curvatureF) * quadTerm;

	return static_cast<float>(std::clamp(value / kQ31FullScale, -1.0, 1.0));
}

PhiMorphWavetable buildMorphXfade(const PhiMorphWavetable& a, const PhiMorphWavetable& b, float xf01) {
	const float cf = std::clamp(xf01, 0.0f, 1.0f);
	const float cfInv = 1.0f - cf;

	// 0 at both endpoints, 1.0 at the midpoint: the excitation exists only while morphing, so a
	// crossfade parked at either end renders exactly that bank. Defined in the header because the
	// editor's scope prints it, and two copies of it could disagree about what the engine is doing.
	const float excite = feathers::morphExcite(cf);

	// Bank B alone: the morph target defines the character of the transition into it.
	const float ampOvershoot = b.morphAmpOvershoot;
	const float curvBoost = b.morphCurvBoost;
	const float pd = b.morphPhaseDistort * excite;

	float effPhases[kPhiMorphMovableWaypoints];
	float effAmplitudes[kPhiMorphMovableWaypoints];
	float effCurvatures[kPhiMorphMaxSegments];

	for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
		const float p = cfInv * a.phases[i] + cf * b.phases[i];
		// Quadratic warp, monotonic because the builder clamped morphPhaseDistort to ±0.15.
		effPhases[i] = p + pd * p * (1.0f - p) * 4.0f;

		const float ampLerp = cfInv * a.amplitudes[i] + cf * b.amplitudes[i];
		// Overshoot pushes past both banks in the direction of travel, driving the clipper mid-morph.
		const float ampDelta = b.amplitudes[i] - a.amplitudes[i];
		effAmplitudes[i] = ampLerp + excite * ampDelta * ampOvershoot;
	}

	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		const float curvLerp = cfInv * a.curvatures[i] + cf * b.curvatures[i];
		effCurvatures[i] = curvLerp * (1.0f + excite * curvBoost);
	}

	PhiMorphWavetable out{};
	out.endpointAmp = cfInv * a.endpointAmp + cf * b.endpointAmp;
	out.phaseJitter = cfInv * a.phaseJitter + cf * b.phaseJitter;
	out.ampNoise = cfInv * a.ampNoise + cf * b.ampNoise;

	// Descriptive only — the interpolated amplitudes already carry each bank's gain. Kept so the
	// field agrees with what the waypoints are worth instead of reading as a silent 0.
	out.gain = cfInv * a.gain + cf * b.gain;

	// The firmware's effective table leaves these zeroed; it only ever evaluates the segments. Storing
	// them costs nothing and keeps the struct honest for anything that reads the waypoints back.
	std::copy(std::begin(effPhases), std::end(effPhases), std::begin(out.phases));
	std::copy(std::begin(effAmplitudes), std::end(effAmplitudes), std::begin(out.amplitudes));
	std::copy(std::begin(effCurvatures), std::end(effCurvatures), std::begin(out.curvatures));

	// The character layer's own segment builder, called rather than reimplemented: same waypoints in,
	// bit-identical segments out. It reads endpointAmp for the two fixed cycle-edge waypoints, so that
	// has to be set before this call.
	owlet::dsp::buildSegmentsFromWaypoints(effPhases, effAmplitudes, effCurvatures, out);
	out.numSegments = kPhiMorphMaxSegments;

	// morphAmpOvershoot/morphCurvBoost/morphPhaseDistort stay zero, as they do in the firmware's
	// effective table: they describe a transition *into* a zone, and a crossfade result is not a zone.
	return out;
}

PhiMorphWavetable buildMorphBlend(std::span<const PhiMorphWavetable> tables,
                                  std::span<const double> weights, const BlueShift& blue) {
	PhiMorphWavetable out{};
	const size_t n = std::min(tables.size(), weights.size());
	if (n == 0) {
		return out;
	}

	// DOPPLER MOVES THE SHARES BEFORE ANYTHING ELSE READS THEM: approaching sounds count for more,
	// receding sounds for less, and everything downstream — the recipe average, the excite spread, the
	// weighted factors — then follows from a blend that has already been tilted the way you are
	// travelling. That is what makes it navigation rather than a tone control.
	//
	// AT REST THIS BRANCH IS NOT TAKEN AT ALL, and it must not be. blendWeights normalises, so the sum
	// it produces is 1.0 to fifteen places rather than exactly 1.0; renormalising an untilted set of
	// weights would therefore move every one of them in the last bit, and a build with the scales down
	// would not be the build without any of this in it. Silence has to be exact.
	double tilted[kMaxSounds]{};
	if (n <= kMaxSounds && tiltWeights(weights, blue, tilted, n)) {
		weights = { tilted, n };
	}

	// HOW MIXED IS THIS. Zero when one sound has all the weight — standing on an anchor, where the
	// character must be exactly that sound's — and greatest when they share evenly. The same quantity
	// morphExcite measures for a two-way fade, asked in a way that does not need there to be two, and
	// taken from the header so the scope that prints it cannot print a different number.
	const float excite = blendExcite(weights.first(n));

	// WHOSE EXCITATION FACTORS. With two sounds the firmware uses the TARGET's, because a fade has a
	// direction and the thing you are arriving at defines the character of the arrival. A
	// constellation has no target, so these are weighted like everything else — symmetric, continuous
	// as the puck moves, and with no tie to break. Taking the nearest sound's instead would reduce to
	// the two-way rule above the midpoint, and would jump as the puck crossed any midline; smooth is
	// worth more here than an exact reduction on a path this function does not serve.
	//
	// That is a SEAM at the moment a third sound is switched on: the factors jump from B's to a
	// weighted average. It is masked by the bank's rebuild crossfade and it happens exactly when the
	// user adds a sound, which is a discrete act rather than something they can slide through.
	float curvBoost = 0.0f;
	float phaseDistort = 0.0f;
	for (size_t k = 0; k < n; ++k) {
		const auto w = static_cast<float>(weights[k]);
		curvBoost += w * tables[k].morphCurvBoost;
		phaseDistort += w * tables[k].morphPhaseDistort;
	}
	const float pd = phaseDistort * excite;

	float effPhases[kPhiMorphMovableWaypoints]{};
	float effAmplitudes[kPhiMorphMovableWaypoints]{};
	float effCurvatures[kPhiMorphMaxSegments]{};

	for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
		float phase = 0.0f;
		float amplitude = 0.0f;
		for (size_t k = 0; k < n; ++k) {
			const auto w = static_cast<float>(weights[k]);
			phase += w * tables[k].phases[i];
			amplitude += w * tables[k].amplitudes[i];
		}
		effPhases[i] = phase + pd * phase * (1.0f - phase) * 4.0f;

		// AMPLITUDE OVERSHOOT GETS ITS DIRECTION FROM WHERE YOU ARE GOING — §2b, and it is the answer to
		// the thing this function had to leave undone. The two-way rule pushes the amplitude from A past
		// B, `excite · (b − a) · overshoot`, which needs a DIRECTION OF TRAVEL that a constellation does
		// not have on its own. Every direction invented from the geometry alone flips on a midline, and
		// excite is at its maximum there, so the character would jump at the busiest place in the space.
		//
		// The velocity has no such problem. `weightRate` is how much of each sound's share would arrive
		// next if you kept going, so this sum is the direction the blended waypoints are ALREADY moving
		// — pushed further along it, which is what an overshoot is. Singular nowhere, because the
		// weights are smooth away from the anchors and excite vanishes at them. At two sounds with the
		// puck travelling A→B it is exactly the (b − a) the crossfade uses.
		//
		// Zero when parked, and zero when the audition has the scale down, in which case `overshoot` is
		// zero and this whole term is +0.0 added to the average.
		float push = 0.0f;
		if (blue.overshoot != 0.0f && blue.weightRate.size() >= n) {
			double along = 0.0;
			for (size_t k = 0; k < n; ++k) {
				along += blue.weightRate[k] * static_cast<double>(tables[k].amplitudes[i]);
			}
			push = excite * static_cast<float>(along) * blue.overshoot;
		}
		effAmplitudes[i] = amplitude + push;
	}

	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		float curvature = 0.0f;
		for (size_t k = 0; k < n; ++k) {
			// APPROACHING SOUNDS BRIGHTEN AND RECEDING ONES DIM, in the recipe's own currency: curvature
			// is what makes a morph waveform edgy, so a sound you are closing on contributes a sharper
			// version of itself. Not a filter, and deliberately not — the blue shift lives entirely in
			// recipe space, so what changes is the waveform being built rather than a treatment of it.
			const double lean = blue.approach.size() > k ? 1.0 + blue.approach[k] : 1.0;
			curvature += static_cast<float>(weights[k] * lean) * tables[k].curvatures[i];
		}
		effCurvatures[i] = curvature * (1.0f + excite * curvBoost);
	}

	for (size_t k = 0; k < n; ++k) {
		const auto w = static_cast<float>(weights[k]);
		out.endpointAmp += w * tables[k].endpointAmp;
		out.phaseJitter += w * tables[k].phaseJitter;
		out.ampNoise += w * tables[k].ampNoise;
		out.gain += w * tables[k].gain;
	}

	std::copy(std::begin(effPhases), std::end(effPhases), std::begin(out.phases));
	std::copy(std::begin(effAmplitudes), std::end(effAmplitudes), std::begin(out.amplitudes));
	std::copy(std::begin(effCurvatures), std::end(effCurvatures), std::begin(out.curvatures));

	owlet::dsp::buildSegmentsFromWaypoints(effPhases, effAmplitudes, effCurvatures, out);
	out.numSegments = kPhiMorphMaxSegments;
	return out;
}

} // namespace feathers
