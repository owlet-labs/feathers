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

#include "dsp/slice_build.h"

#include "dsp/flagship.h"
#include "dsp/phase_warp.h"
#include "dsp/portable_trig.h"
#include "dsp/vox_anchors.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// The assembly below is the firmware's, restated. Read the header for why it is here rather than
// there; read the identity test for what stops it drifting. Comments that explain the CHARACTER are
// kept from the original so the two read as the same code — the ones that are new say what the slice
// does differently, and there are only a handful of those.

namespace feathers {

namespace {

using namespace owlet::dsp;

// The counts in slice_basis.h are what the tests and the audition tools index against; these are
// what the engine actually has. If a bank grows, this stops the build instead of leaving the new
// triangle sharing a coefficient with an old one.
static_assert(kMorphTriangles
                  == static_cast<size_t>(kPhiMorphMaxSegments) + static_cast<size_t>(kPhiMorphMovableWaypoints)
                         + static_cast<size_t>(kPhiMorphMaxSegments) + 12,
              "morph triangle count must match the banks phi_morph.hpp declares");
static_assert(morph_slot::kAmplitude == static_cast<size_t>(kPhiMorphMaxSegments));
static_assert(morph_slot::kCurvature
              == static_cast<size_t>(kPhiMorphMaxSegments) + static_cast<size_t>(kPhiMorphMovableWaypoints));
static_assert(morph_slot::kAsymGain == kMorphTriangles - 1);

/// The vox bank, IN SLOT ORDER, and the tie that keeps it honest: every entry is the firmware
/// header's own constant, so a config renamed or removed there stops this build here, and a retune
/// there flows straight through with no copy to go stale. The count assert is what catches a bank
/// ADDED there — a thirteenth config would leave kVoxTriangles a triangle short, exactly the
/// silent-unsliced failure the morph assert above exists for.
constexpr std::array<phi::PhiTriConfig, kVoxTriangles> kVoxBank {
    kPhiVoxF1Wander,     // vox_slot::kF1Wander
    kPhiVoxF2Wander,     // vox_slot::kF2Wander
    kPhiVoxN1,           // vox_slot::kPulses1
    kPhiVoxN2,           // vox_slot::kPulses2
    kPhiVoxDecay1,       // vox_slot::kDecay1
    kPhiVoxDecay2,       // vox_slot::kDecay2
    kPhiVoxBalance,      // vox_slot::kBalance
    kPhiVoxPolarity1,    // vox_slot::kPolarity1
    kPhiVoxPolarity2,    // vox_slot::kPolarity2
    kPhiVoxShift,        // vox_slot::kShift
    kPhiVoxBreath,       // vox_slot::kBreath
    kPhiVoxArticulation, // vox_slot::kArticulation
};
static_assert(kVoxBank.size() == kVoxTriangles,
              "vox triangle count must match the banks phi_vox.hpp declares");
static_assert(vox_slot::kArticulation == kVoxTriangles - 1);

/// The Resonate bank, IN SLOT ORDER, on the vox bank's rule: every entry is phi_weave.hpp's own
/// constant, so a retune there flows straight through and a config removed there stops this build.
constexpr std::array<phi::PhiTriConfig, kWeaveTriangles> kWeaveBank {
    kPhiWeaveStiffBase,   // weave_slot::kStiffBase
    kPhiWeaveCoupleBase,  // weave_slot::kCoupleBase
    kPhiWeaveDampBase,    // weave_slot::kDampBase
    kPhiWeaveStiffCycles, // weave_slot::kStiffCycles
    kPhiWeaveDampCycles,  // weave_slot::kDampCycles
    kPhiWeaveStiffLand1,  // weave_slot::kStiffLand1
    kPhiWeaveStiffLand2,  // weave_slot::kStiffLand2
    kPhiWeaveDampLand1,   // weave_slot::kDampLand1
    kPhiWeaveDampLand2,   // weave_slot::kDampLand2
    kPhiWeaveCoupleLand,  // weave_slot::kCoupleLand
    kPhiWeaveHomeP2,      // weave_slot::kHomeP2
    kPhiWeaveHomeP3,      // weave_slot::kHomeP3
    kPhiWeaveHomeP5,      // weave_slot::kHomeP5
    kPhiWeaveHomePh2,     // weave_slot::kHomePh2
    kPhiWeaveHomePh3,     // weave_slot::kHomePh3
    kPhiWeaveBowDepth,    // weave_slot::kBowDepth
    kPhiWeaveBowRate,     // weave_slot::kBowRate
    kPhiWeaveBowPos,      // weave_slot::kBowPos
    kPhiWeaveBowSpread,   // weave_slot::kBowSpread
    kPhiWeaveBowMode,     // weave_slot::kBowMode
    kPhiWeaveTravel,      // weave_slot::kTravel
    kPhiWeavePluckPos,    // weave_slot::kPluckPos
    kPhiWeavePluckWidth,  // weave_slot::kPluckWidth
    kPhiWeavePluckAmp,    // weave_slot::kPluckAmp
    kPhiWeaveShimmer,     // weave_slot::kShimmer
    kPhiWeaveMorphBow,    // weave_slot::kMorphBow
    kPhiWeaveOutGain,     // weave_slot::kOutGain
};
static_assert(kWeaveBank.size() == kWeaveTriangles,
              "weave triangle count must match the banks phi_weave.hpp declares");
static_assert(weave_slot::kOutGain == kWeaveTriangles - 1);

/// The firmware TU's vowel anchors (F1, F2 per zone) live in dsp/vox_anchors.h, shared with the spectral drawer's
/// Formant operator; see buildVoxSliced's header comment for the parity boundary they create.

/// One triangle, read where the basis says triangle `slot` should be read.
///
/// `freqMult` is the firmware's own parameter and it means exactly what it means there — the rate at
/// which this triangle's landscape passes under the point. The default basis returns 1.0f for it, so
/// this call is the firmware's call.
template <size_t N>
float evalSlot(const SliceBasis<N>& basis, size_t slot, double home, double tx, double ty,
               const phi::PhiTriConfig& cfg) {
	return phi::evalTriangle(basis.phaseFor(slot, home, tx, ty), basis.rateFor(slot, tx, ty), cfg);
}

} // namespace

namespace {

/// The blue shift's two rates, DERIVED rather than transcribed so their ratio is exactly φ.
///
/// §2b.1 chose φ^-1.75 for the Doppler reactivity and φ^-0.75 for the overshoot reactivity, and said
/// what makes the pair right: their ratio is φ^1, the canonical incommensurate choice in this codebase
/// and the same trick the character layer uses to keep its landscapes from ever realigning. Writing
/// the two decimals out by hand would have made that ratio approximate — φ to four places divided by φ
/// to four places is not φ — so the slower one is the firmware's own φ^1.75 inverted and the faster one
/// is that times φ. The periods land where the plan measured them, ~1470 and ~2370 zones, and their
/// place in the ladder is worth stating exactly: these are the two FASTEST of the long components.
/// They clear the character banks (190–1300 zones) and then interleave with the quickest orientation
/// angle rather than sitting wholly below the orientation field — φ^-1 is about 1650 zones, so the
/// overshoot is faster than every orientation component and the Doppler sits between the first two.
/// Both are still far too slow to flicker per zone, which is what makes them regions of
/// weather-character; they are simply the near end of "long", not the far one.
constexpr float kDopplerRate = 1.0f / phi::kPhi175;
constexpr float kOvershootRate = kDopplerRate * phi::kPhi100;

/// THE COEFFICIENTS ARE MIXING CHOICES, reported rather than derived and meant to be tuned by ear —
/// the same status as the orientation bank's. Unipolar because a reactivity is an amount rather than a
/// direction, and the two offsets are far apart so the maps do not both peak at the origin, which is
/// where an audition starts and would be the one place they looked like one field.
///
/// WIDE DUTIES, and that is a correction rather than a preference. A unipolar triangle reads exactly
/// zero outside its duty, so the 0.6 this started at left two fifths of the space with NO Doppler at
/// all — and a sound parked in one of those regions makes the feature look broken rather than looking
/// like a quiet neighbourhood. It was found by a test that reasonably expected a signed approach and
/// got a signed zero. The aperture triangle carries 0.9 for the same reason, and these follow it: what
/// varies is how strongly a region reacts, not whether it reacts at all.
constexpr std::array<phi::PhiTriConfig, 2> kBlueShiftBank = { {
    { kOvershootRate, 0.9f, 0.230f, false },
    { kDopplerRate, 0.85f, 0.660f, false },
} };

} // namespace

BlueShiftFields blueShiftFieldsAt(double position) {
	BlueShiftFields fields;
	// freqMult 1.0f, as the orientation passes and for the same reason: the rate half of the slice
	// belongs to a sound's travel, and a field over the space is not travelling.
	fields.overshoot = phi::evalTriangle(position, 1.0f, kBlueShiftBank[0]);
	fields.doppler = phi::evalTriangle(position, 1.0f, kBlueShiftBank[1]);
	return fields;
}

owlet::dsp::PhiMorphWavetable buildMorphSliced(uint16_t zone, float phaseOffset, double travelX,
                                               double travelY, const MorphBasis& basis,
                                               double gammaBase, float gritScale) {
	const AngleKindScope sites(AngleKind::Morph); // the phase warp's Morph sites (dsp/phase_warp.h)
	PhiMorphWavetable table{};
	table.numSegments = kPhiMorphMaxSegments;

	// Home: the firmware's own line, computed the firmware's own way, plus gamma's unbounded half.
	// Travel departs from it. At a zero base this is the firmware's number to the bit.
	const double home = sliceHome(zone, phaseOffset, gammaBase);

	float rawPhaseDeltas[kPhiMorphMaxSegments];
	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		rawPhaseDeltas[i] = evalSlot(basis, morph_slot::kPhaseDelta + static_cast<size_t>(i), home, travelX, travelY,
		                             kPhiMorphPhaseBank[static_cast<size_t>(i)]);
	}
	float rawAmplitudes[kPhiMorphMovableWaypoints];
	for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
		rawAmplitudes[i] = evalSlot(basis, morph_slot::kAmplitude + static_cast<size_t>(i), home, travelX, travelY,
		                            kPhiMorphAmplitudeBank[static_cast<size_t>(i)]);
	}
	float rawCurvatures[kPhiMorphMaxSegments];
	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		rawCurvatures[i] = evalSlot(basis, morph_slot::kCurvature + static_cast<size_t>(i), home, travelX, travelY,
		                            kPhiMorphCurvatureBank[static_cast<size_t>(i)]);
	}
	const float rawGain = evalSlot(basis, morph_slot::kGain, home, travelX, travelY, kPhiMorphGainBank[0]);
	const float rawEndpoint = evalSlot(basis, morph_slot::kEndpoint, home, travelX, travelY, kPhiMorphEndpointBank[0]);
	const float rawAmpOvershoot = evalSlot(basis, morph_slot::kAmpOvershoot, home, travelX, travelY, kPhiMorphAmpOvershootBank[0]);
	const float rawCurvBoost = evalSlot(basis, morph_slot::kCurvBoost, home, travelX, travelY, kPhiMorphCurvBoostBank[0]);
	const float rawPhaseDistort = evalSlot(basis, morph_slot::kPhaseDistort, home, travelX, travelY, kPhiMorphPhaseDistortBank[0]);
	const float rawSineBlend = evalSlot(basis, morph_slot::kSineBlend, home, travelX, travelY, kPhiMorphSineBlendBank[0]);
	const float rawOddSym = evalSlot(basis, morph_slot::kOddSym, home, travelX, travelY, kPhiMorphOddSymBank[0]);
	const float rawWindow = evalSlot(basis, morph_slot::kWindow, home, travelX, travelY, kPhiMorphWindowBank[0]);
	const float rawSlopeMatch = evalSlot(basis, morph_slot::kSlopeMatch, home, travelX, travelY, kPhiMorphSlopeMatchBank[0]);
	const float rawPhaseJitter = evalSlot(basis, morph_slot::kPhaseJitter, home, travelX, travelY, kPhiMorphPhaseJitterBank[0]);
	const float rawAmpNoise = evalSlot(basis, morph_slot::kAmpNoise, home, travelX, travelY, kPhiMorphAmpNoiseBank[0]);
	const float rawAsymGain = evalSlot(basis, morph_slot::kAsymGain, home, travelX, travelY, kPhiMorphAsymGainBank[0]);

	// Phase deltas: take abs of bipolar, add floor
	float deltas[kPhiMorphMaxSegments];
	for (int i = 0; i < kPhiMorphMaxSegments; i++) {
		deltas[i] = 0.3f + std::abs(rawPhaseDeltas[i]) * 0.7f;
	}

	// Accumulate phases WITHOUT normalizing — absolute delta magnitude determines
	// how many waypoints fit. Short segments → all 8 fit (complex shape).
	// Long segments → fewer fit, simpler shape with longer return-to-zero tail.
	float phaseAccum = kPhiMorphPhaseMin;
	int activeWaypoints = 0;

	for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
		float nextPhase = phaseAccum + deltas[i] * kPhiMorphPhaseScale;
		if (nextPhase >= kPhiMorphPhaseMax) {
			break;
		}
		table.phases[i] = nextPhase;
		phaseAccum = nextPhase;
		activeWaypoints = i + 1;
	}

	// Guarantee at least 2 waypoints so normalization always has +/- peaks
	if (activeWaypoints < 2) {
		constexpr float kRange = kPhiMorphPhaseMax - kPhiMorphPhaseMin;
		table.phases[0] = kPhiMorphPhaseMin + kRange * 0.33f;
		table.phases[1] = kPhiMorphPhaseMin + kRange * 0.66f;
		phaseAccum = table.phases[1];
		activeWaypoints = 2;
	}

	// Spread remaining waypoints from truncation to kPhiMorphPhaseMax with zero amplitude.
	if (activeWaypoints < kPhiMorphMovableWaypoints) {
		int remaining = kPhiMorphMovableWaypoints - activeWaypoints;
		float spreadRange = kPhiMorphPhaseMax - phaseAccum;
		for (int i = activeWaypoints; i < kPhiMorphMovableWaypoints; i++) {
			int idx = i - activeWaypoints + 1;
			table.phases[i] = phaseAccum + spreadRange * static_cast<float>(idx) / static_cast<float>(remaining + 1);
		}
	}

	// Peak-symmetric normalization: all waypoints from phi bank, centered and scaled
	// so max → +1, min → -1. Guarantees no DC asymmetry in the fundamental.
	float minAmp = rawAmplitudes[0], maxAmp = rawAmplitudes[0];
	for (int i = 1; i < activeWaypoints; i++) {
		minAmp = std::min(minAmp, rawAmplitudes[i]);
		maxAmp = std::max(maxAmp, rawAmplitudes[i]);
	}
	float ampRange = maxAmp - minAmp;
	if (ampRange > 0.01f) {
		float scale = 2.0f / ampRange;
		float center = (maxAmp + minAmp) * 0.5f;
		for (int i = 0; i < activeWaypoints; i++) {
			table.amplitudes[i] = std::clamp((rawAmplitudes[i] - center) * scale, -1.0f, 1.0f);
		}
	}
	else {
		// Degenerate: all similar values → alternating ±1 for a basic waveform
		for (int i = 0; i < activeWaypoints; i++) {
			table.amplitudes[i] = (i % 2 == 0) ? 1.0f : -1.0f;
		}
	}

	for (int i = activeWaypoints; i < kPhiMorphMovableWaypoints; i++) {
		table.amplitudes[i] = 0.0f;
	}

	// Odd symmetry: blend waypoints toward f(x) = -f(1-x) to eliminate even harmonics.
	float oddSym = rawOddSym;
	if (oddSym > 0.01f && activeWaypoints >= 2) {
		int pairs = activeWaypoints / 2;
		for (int i = 0; i < pairs; i++) {
			int j = activeWaypoints - 1 - i;
			float symPhase = (table.phases[i] + (1.0f - table.phases[j])) * 0.5f;
			table.phases[i] += (symPhase - table.phases[i]) * oddSym;
			table.phases[j] += ((1.0f - symPhase) - table.phases[j]) * oddSym;
			float antiSym = (table.amplitudes[i] - table.amplitudes[j]) * 0.5f;
			table.amplitudes[i] += (antiSym - table.amplitudes[i]) * oddSym;
			table.amplitudes[j] += (-antiSym - table.amplitudes[j]) * oddSym;
		}
		if (activeWaypoints % 2 == 1) {
			int mid = activeWaypoints / 2;
			table.phases[mid] += (0.5f - table.phases[mid]) * oddSym;
			table.amplitudes[mid] *= (1.0f - oddSym);
		}
	}

	// Sine blend: lerp active waypoint amplitudes toward sin(2π × phase).
	constexpr float kTwoPi = 6.283185307f;
	float sineBlend = rawSineBlend;
	if (sineBlend > 0.01f) {
		for (int i = 0; i < activeWaypoints; i++) {
			float sineTarget = std::sin(kTwoPi * table.phases[i]);
			table.amplitudes[i] += (sineTarget - table.amplitudes[i]) * sineBlend;
		}
	}

	// Amplitude windowing: raised cosine taper toward cycle extremes.
	float windowAmt = rawWindow;
	if (windowAmt > 0.01f) {
		for (int i = 0; i < activeWaypoints; i++) {
			float w = 0.5f * (1.0f + std::cos(kTwoPi * (table.phases[i] - 0.5f)));
			table.amplitudes[i] *= (1.0f - windowAmt) + windowAmt * w;
		}
	}

	// Endpoint amplitude: 30% duty cycle (non-zero only for ~30% of zone positions).
	float endpointRaw = std::max(0.0f, rawEndpoint - 0.7f) / 0.3f;
	table.endpointAmp = endpointRaw * 0.8f;

	// Morph excitation factors: zone-dependent strengths for crossfade effects
	table.morphAmpOvershoot = rawAmpOvershoot * 1.5f;
	table.morphCurvBoost = rawCurvBoost * 6.0f;
	table.morphPhaseDistort = std::clamp(rawPhaseDistort * 0.15f, -0.15f, 0.15f);

	// Per-sample render modifiers
	table.phaseJitter = rawPhaseJitter;
	// THE GRIT KNOB (2026-08-25) BIASES THE TRIANGLE, NOT THE SCALED RESULT. fieldWithOverride is
	// defined on [0,1] at both ends — its upper half lifts toward 1 — and rawAmpNoise is that
	// triangle, while `* 0.25f` is the range the FIRMWARE's arithmetic gives (see modifiers.h: the
	// builder's 0.25 is why ampNoise reaches 0.249997 and not the 0.5 phi_morph.hpp advertises).
	// Applying the curve after the 0.25 would lift a [0, 0.25] quantity toward 1.0 and hand the
	// renderer four times the grit the hardware can make.
	//
	// AND THIS IS THE ONE FIELD MODIFIER THAT IS NOT FLAGSHIP-ONLY. ampNoise rides the morph table,
	// so it reaches the Morph engine as well as V5 — which is why the knob in front of it must not be
	// greyed on a non-V5 patch.
	table.ampNoise = fieldWithOverride(rawAmpNoise, gritScale) * 0.25f;

	// Energy normalization: equalize perceived loudness across zone positions.
	float energy = 0.0f;
	{
		float prevPhase = 0.0f;
		float prevAmp = table.endpointAmp;
		for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
			float segWidth = table.phases[i] - prevPhase;
			float a0 = prevAmp;
			float a1 = table.amplitudes[i];
			energy += segWidth * (a0 * a0 + a0 * a1 + a1 * a1);
			prevPhase = table.phases[i];
			prevAmp = a1;
		}
		float lastWidth = 1.0f - prevPhase;
		float aEnd = table.endpointAmp;
		energy += lastWidth * (prevAmp * prevAmp + prevAmp * aEnd + aEnd * aEnd);
		energy /= 3.0f;
	}

	constexpr float kTargetEnergy = 0.333f;
	if (energy > 0.001f) {
		float normScale = std::sqrt(kTargetEnergy / energy);
		normScale = std::min(normScale, 2.0f);
		for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
			table.amplitudes[i] *= normScale;
		}
		table.endpointAmp *= normScale;
	}

	// Gain: strictly positive [1.0, 2.0] — drives normalized waveform into the clipper.
	float gainValue = 1.0f + rawGain;
	table.gain = gainValue;
	for (int i = 0; i < activeWaypoints; i++) {
		table.amplitudes[i] *= gainValue;
	}
	table.endpointAmp *= gainValue;

	// Asymmetric gain: break waveform symmetry to introduce even harmonics.
	float asymGain = rawAsymGain;
	if (asymGain < -0.01f) {
		float posFactor = 1.0f + asymGain;
		for (int i = 0; i < activeWaypoints; i++) {
			if (table.amplitudes[i] > 0.0f) {
				table.amplitudes[i] *= posFactor;
			}
		}
		if (table.endpointAmp > 0.0f) {
			table.endpointAmp *= posFactor;
		}
	}
	else if (asymGain > 0.01f) {
		float allFactor = 1.0f + asymGain * 0.5f;
		for (int i = 0; i < activeWaypoints; i++) {
			table.amplitudes[i] *= allFactor;
		}
		table.endpointAmp *= allFactor;
	}

	// Curvatures: blend between phi-triangle curvatures and C1 slope-matched curvatures.
	float slopeMatch = rawSlopeMatch;
	{
		float bPhase[kPhiMorphMaxSegments + 1];
		float bAmp[kPhiMorphMaxSegments + 1];
		bPhase[0] = 0.0f;
		bAmp[0] = table.endpointAmp;
		for (int i = 0; i < kPhiMorphMovableWaypoints; i++) {
			bPhase[i + 1] = table.phases[i];
			bAmp[i + 1] = table.amplitudes[i];
		}
		bPhase[kPhiMorphMaxSegments] = 1.0f;
		bAmp[kPhiMorphMaxSegments] = table.endpointAmp;

		for (int i = 0; i < kPhiMorphMaxSegments; i++) {
			float w = bPhase[i + 1] - bPhase[i];
			float ampDelta = bAmp[i + 1] - bAmp[i];
			float linearSlope = (w > 0.001f) ? ampDelta / w : 0.0f;

			float tEntry;
			if (i == 0) {
				tEntry = linearSlope;
			}
			else {
				float dp = bPhase[i + 1] - bPhase[i - 1];
				tEntry = (dp > 0.001f) ? (bAmp[i + 1] - bAmp[i - 1]) / dp : linearSlope;
			}

			float tExit;
			if (i >= kPhiMorphMaxSegments - 1) {
				tExit = linearSlope;
			}
			else {
				float dp = bPhase[i + 2] - bPhase[i];
				tExit = (dp > 0.001f) ? (bAmp[i + 2] - bAmp[i]) / dp : linearSlope;
			}

			float naturalCurvAmp = (tEntry - tExit) * w / 4.0f;
			float naturalCurv = std::clamp(naturalCurvAmp * 0.5f, -1.0f, 1.0f);

			float phiCurv = (i <= activeWaypoints) ? rawCurvatures[i] : 0.0f;
			table.curvatures[i] = phiCurv * (1.0f - slopeMatch) + naturalCurv * slopeMatch;
		}
	}

	buildSegmentsFromWaypoints(table.phases, table.amplitudes, table.curvatures, table);

	return table;
}

namespace {

/// A landscape read, sliced — Stair's staircase landscapes and Resonate's per-node ones alike (the
/// firmware's stairSpatial and phi_weave's evalSpatial are the same arithmetic).
///
/// The firmware's spatial reads take no freqMult — the plane's rate coefficient therefore scales the
/// zone-phase term, which is the same quantity `freqMult` scales in evalTriangle: how fast this
/// landscape passes under the point as it travels. `position * cycles` is a place WITHIN the staircase
/// or round the ring and is not the plane's business; the offset and it are added AFTER the frequency
/// multiply, the firmware's arithmetic, kept. At rate 1 the extra factor is an exact multiply by 1.0,
/// so the value is the firmware's to the bit.
float spatialSliced(double zonePhase, float rate, float position, float cycles, const phi::PhiTriConfig& cfg) {
	float wrapped = phi::wrapPhase(zonePhase * static_cast<double>(cfg.phiFreq) * static_cast<double>(rate)
	                               + static_cast<double>(position * cycles) + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return owlet::dsp::triangleFloat(wrapped, cfg.duty);
	}
	return owlet::dsp::triangleSimpleUnipolar(wrapped, cfg.duty);
}

/// Height pattern families per zone: Brick, Terrace, Ramp, Mesa, Pylon, Glyph, Shard, Teeth.
/// Families 5 and 6 read a landscape, so they take their own slice coordinates.
float stairPatternSliced(int32_t family, float t, const StairBasis& basis, double home, double tx, double ty) {
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
		return std::round(spatialSliced(basis.phaseFor(stair_slot::kGlyph, home, tx, ty),
		                                     basis.rateFor(stair_slot::kGlyph, tx, ty), t, 7.0f,
		                                     {phi::kPhi300, 0.9f, 0.230f, true})
		                  * 3.0f)
		       * (1.0f / 3.0f);
	case 6: // SHARD: big asymmetric jumps
		return spatialSliced(basis.phaseFor(stair_slot::kShard, home, tx, ty),
		                          basis.rateFor(stair_slot::kShard, tx, ty), t, 3.0f,
		                          {phi::kPhi200, 0.5f, 0.510f, true});
	default: // TEETH: rapid pair alternation with lean
		return ((static_cast<int32_t>(t * 16.0f) & 2) ? 0.7f : -0.7f) + t * 0.5f - 0.25f;
	}
}

} // namespace

owlet::dsp::PhiStairParams buildStairSliced(uint16_t zone, float phaseOffset, double travelX,
                                            double travelY, const StairBasis& basis,
                                            double gammaBase) {
	const AngleKindScope sites(AngleKind::Stair); // the phase warp's Stair sites (dsp/phase_warp.h)
	PhiStairParams p{};

	const double home = sliceHome(zone, phaseOffset, gammaBase);
	int32_t family = std::min(static_cast<int32_t>(7), static_cast<int32_t>(zone >> 7));

	// Continuous step count: the last step's width fades in fractionally
	float countF = 2.0f + evalSlot(basis, stair_slot::kCount, home, travelX, travelY, kPhiStairCount) * 14.0f;
	p.slope = evalSlot(basis, stair_slot::kSlope, home, travelX, travelY, kPhiStairSlope);
	p.slope = p.slope * p.slope * 0.9f; // Squared: most of the map stays steppy
	if (family == 2) {
		p.slope = 0.3f + p.slope * 0.6f; // RAMP zone is meant to be smooth
	}

	float tilt = evalSlot(basis, stair_slot::kTilt, home, travelX, travelY, kPhiStairTilt) * 0.6f;
	float asym = evalSlot(basis, stair_slot::kAsym, home, travelX, travelY, kPhiStairAsym);
	float cycles = 1.0f + evalSlot(basis, stair_slot::kLandCycles, home, travelX, travelY, kPhiStairLandCycles) * 3.0f;

	const double heightPhase = basis.phaseFor(stair_slot::kHeightLand, home, travelX, travelY);
	const float heightRate = basis.rateFor(stair_slot::kHeightLand, travelX, travelY);
	const double asymPhase = basis.phaseFor(stair_slot::kAsymLand, home, travelX, travelY);
	const float asymRate = basis.rateFor(stair_slot::kAsymLand, travelX, travelY);

	float peak = 0.0001f;
	for (int32_t i = 0; i < kPhiStairSteps; i++) {
		float t = (static_cast<float>(i) + 0.5f) / countF;
		float h = stairPatternSliced(family, std::min(t, 1.0f), basis, home, travelX, travelY)
		          + 0.35f * spatialSliced(heightPhase, heightRate, t, cycles, kPhiStairHeightLand)
		          + tilt * (t * 2.0f - 1.0f);
		p.heights[i] = h;
		peak = std::max(peak, std::abs(h));

		// Width: even split warped by the asymmetry landscape, with the
		// fractional-count fade keeping the step count CONTINUOUS
		float wLand = 1.0f + asym * 0.85f * spatialSliced(asymPhase, asymRate, t, cycles * 2.0f, kPhiStairAsymLand);
		float countFade = std::clamp(countF - static_cast<float>(i), 0.0f, 1.0f);
		p.widths[i] = std::max(wLand, 0.1f) * countFade;
	}
	float norm = 1.0f / peak;
	for (float& h : p.heights) {
		h *= norm;
	}

	return p;
}

VoxParams buildVoxSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                         const VoxBasis& basis, double gammaBase) {
	const AngleKindScope sites(AngleKind::Vox); // the phase warp's vowel sites (dsp/phase_warp.h)
	VoxParams p {};

	const double home = sliceHome(zone, phaseOffset, gammaBase);
	const auto vox = [&](size_t slot) {
		return evalSlot(basis, slot, home, travelX, travelY, kVoxBank[slot]);
	};

	// Anchor interpolation: continuous position across the 8 vowel anchors — on the raw zone alone,
	// firmware-identical (see the header: zone stays the vowel selector; the plane carries the φ)
	float zonePos = static_cast<float>(zone) / 1023.0f * 7.0f;
	int32_t zi = std::min(static_cast<int32_t>(zonePos), static_cast<int32_t>(6));
	float zf = zonePos - static_cast<float>(zi);
	float f1Anchor = kVoxAnchorF1[zi] + (kVoxAnchorF1[zi + 1] - kVoxAnchorF1[zi]) * zf;
	float f2Anchor = kVoxAnchorF2[zi] + (kVoxAnchorF2[zi + 1] - kVoxAnchorF2[zi]) * zf;

	// Phi wander in octaves around the anchors, plus a slow COHERENT shift of both formants
	// together (the "same vowel, moved" dimension)
	float shiftMul = std::exp2(vox(vox_slot::kShift) * 0.6f);
	float f1 = f1Anchor * std::exp2(vox(vox_slot::kF1Wander) * 0.6f) * shiftMul;
	float f2 = f2Anchor * std::exp2(vox(vox_slot::kF2Wander) * 0.8f) * shiftMul;
	p.formant[0].frequencyHz = std::clamp(f1, 60.0f, 8000.0f);
	p.formant[1].frequencyHz = std::clamp(f2, 60.0f, 8000.0f);

	// FRACTIONAL pulse counts: the last pulse fades in continuously, so sweeping through phi space
	// never pops a whole pulse in/out of the burst (that integer step was an audible discontinuity
	// at specific zone positions)
	float n1 = 1.0f + vox(vox_slot::kPulses1) * 11.0f;
	float n2 = 1.0f + vox(vox_slot::kPulses2) * 11.0f;
	float b1 = 0.30f + vox(vox_slot::kDecay1) * 0.88f;
	float b2 = 0.30f + vox(vox_slot::kDecay2) * 0.88f;
	float pol1 = vox(vox_slot::kPolarity1);
	float pol2 = vox(vox_slot::kPolarity2);

	// F1/F2 balance: 0 -> F1 only, 1 -> equal
	float balance = 0.15f + vox(vox_slot::kBalance) * 0.85f;
	float gains[kVoxNumFormants] = {0.62f, 0.62f * balance};
	float counts[kVoxNumFormants] = {n1, n2};
	float decays[kVoxNumFormants] = {b1, b2};
	float pols[kVoxNumFormants] = {pol1, pol2};

	for (size_t f = 0; f < kVoxNumFormants; f++) {
		float count = std::min(counts[f], static_cast<float>(kVoxMaxPulses));
		int32_t nFull = static_cast<int32_t>(count);
		float partial = count - static_cast<float>(nFull);
		float mag = 1.0f;
		float peak = 0.0f;
		for (int32_t k = 0; k < nFull + (partial > 0.0f ? 1 : 0) && k < static_cast<int32_t>(kVoxMaxPulses);
		     k++) {
			float sign = 1.0f - 2.0f * static_cast<float>(k & 1) * pols[f];
			float frac = (k < nFull) ? 1.0f : partial;
			p.formant[f].pulseGain[static_cast<size_t>(k)] = mag * sign * frac;
			peak = std::max(peak, std::abs(mag) * frac);
			mag *= decays[f];
		}
		// Peak-normalize the burst (decay > 1 makes the LAST pulse loudest)
		float norm = (peak > 0.0001f) ? gains[f] / peak : 0.0f;
		for (size_t k = 0; k < kVoxMaxPulses; k++) {
			p.formant[f].pulseGain[k] *= norm;
		}
		// Remaining entries stay zero: the silent gap until the cycle wraps
	}

	p.breath = vox(vox_slot::kBreath);
	p.breath = p.breath * p.breath * 0.30f;
	p.articulation = 0.5f + vox(vox_slot::kArticulation) * 3.5f;

	return p;
}

PhiWeaveParams buildWeaveSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                                const WeaveBasis& basis, double gammaBase) {
	const AngleKindScope sites(AngleKind::Weave); // the phase warp's Resonate sites (dsp/phase_warp.h)
	constexpr float kTwoPi = 6.283185307f;
	PhiWeaveParams p{};

	const double home = sliceHome(zone, phaseOffset, gammaBase);
	const auto weave = [&](size_t slot) {
		return evalSlot(basis, slot, home, travelX, travelY, kWeaveBank[slot]);
	};
	// One landscape's placement, read once and shared by every node — see weave_slot.
	struct Land {
		double phase;
		float rate;
	};
	const auto land = [&](size_t slot) {
		return Land { basis.phaseFor(slot, home, travelX, travelY), basis.rateFor(slot, travelX, travelY) };
	};

	// Global scalars
	float stiffT = weave(weave_slot::kStiffBase);
	float stiffBase = 0.0008f * std::pow(75.0f, stiffT); // 0.0008..0.06, exponential
	float coupleT = weave(weave_slot::kCoupleBase);
	float coupleBase = 0.010f + coupleT * coupleT * 0.28f;
	float dampT = weave(weave_slot::kDampBase);
	float dampBase = 0.0015f + dampT * dampT * 0.0435f;

	// Home shape partials
	float p2 = weave(weave_slot::kHomeP2) * 0.7f;
	float p3 = weave(weave_slot::kHomeP3) * 0.55f;
	float p5 = weave(weave_slot::kHomeP5) * 0.4f;
	float ph2 = weave(weave_slot::kHomePh2) * kTwoPi;
	float ph3 = weave(weave_slot::kHomePh3) * kTwoPi;

	// Spatial cycle counts derived from the phase too (1..4 bumps around the ring)
	float stiffCycles = 1.0f + weave(weave_slot::kStiffCycles) * 3.0f;
	float dampCycles = 1.0f + weave(weave_slot::kDampCycles) * 3.0f;

	const Land stiffLand1 = land(weave_slot::kStiffLand1);
	const Land stiffLand2 = land(weave_slot::kStiffLand2);
	const Land dampLand1 = land(weave_slot::kDampLand1);
	const Land dampLand2 = land(weave_slot::kDampLand2);
	const Land coupleLandAt = land(weave_slot::kCoupleLand);

	float homePeak = 0.0f;
	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);

		float stiffLand =
		    1.0f + 0.8f * spatialSliced(stiffLand1.phase, stiffLand1.rate, nf, stiffCycles, kPhiWeaveStiffLand1)
		    + 0.5f * spatialSliced(stiffLand2.phase, stiffLand2.rate, nf, stiffCycles * 2.0f, kPhiWeaveStiffLand2);
		p.stiffness[i] = std::clamp(stiffBase * stiffLand, 0.016f, 0.12f);

		float dampLand =
		    1.0f + 0.85f * spatialSliced(dampLand1.phase, dampLand1.rate, nf, dampCycles, kPhiWeaveDampLand1)
		    + 0.5f * spatialSliced(dampLand2.phase, dampLand2.rate, nf, dampCycles * 3.0f, kPhiWeaveDampLand2);
		p.damping[i] = std::clamp(dampBase * dampLand, 0.006f, 0.09f);
		p.bowBalance[i] = 0.60f * std::min(1.0f, std::sqrt(p.damping[i] * (1.0f / 0.0255f)));

		float coupleLand =
		    1.0f + 0.6f * spatialSliced(coupleLandAt.phase, coupleLandAt.rate, nf, stiffCycles, kPhiWeaveCoupleLand);
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

	float bowDepthT = weave(weave_slot::kBowDepth);
	p.bowDepth = 0.0015f + bowDepthT * bowDepthT * 0.030f;
	// Bow oscillation capped ~16 Hz: faster bowing stamped inharmonic
	// sidebands on every partial (excitation must be haptic-rate too)
	p.bowRate = 0.002f + weave(weave_slot::kBowRate) * 0.045f;
	p.bowMode = static_cast<WeaveBow>(static_cast<uint8_t>(std::min(4.0f, weave(weave_slot::kBowMode) * 5.0f)));
	p.bowPos = weave(weave_slot::kBowPos);
	p.bowSpread = 0.06f + weave(weave_slot::kBowSpread) * 0.20f;

	float travelT = weave(weave_slot::kTravel);
	p.travelRate = travelT * travelT * travelT * 0.004f; // Cubic: mostly near-still, occasionally drifting

	p.pluckPos = weave(weave_slot::kPluckPos);
	p.pluckWidth = 0.03f + weave(weave_slot::kPluckWidth) * 0.20f;
	p.pluckAmp = 0.4f + weave(weave_slot::kPluckAmp) * 0.6f;

	p.morphBowGain = 0.5f + weave(weave_slot::kMorphBow) * 4.0f;
	p.outGain = 0.70f + weave(weave_slot::kOutGain) * 0.55f;

	// 0.07..0.30 exponential: scan-smoother cutoff ~8..33 Hz at the sub-tick rate
	float shimmerT = weave(weave_slot::kShimmer);
	p.shimmerAlpha = 0.07f * std::pow(4.3f, shimmerT);

	return p;
}

// ------------------------------------------------------------------------------------------------
// THE ADDITIVE ENGINE'S TERRAIN (design 2026-10-08 §2): thirteen shape fields and 320 detail triangles.
//
// EVERY NUMBER BELOW IS TASTE, to be set by ear (design §7). They are written once and plainly so
// they can be: the SHAPE is agreed (the fields sit ON V5's band so a field changes across the window a
// hand can reach, the duties follow §2.1's table, the offsets keep the origin clear of every gated
// field's peak and spread the peaks apart); the values are a first guess nobody has listened to.
// ------------------------------------------------------------------------------------------------

namespace {

/// Each non-Smoothness field's place on the band: rate = kFlagshipQuantizeRate · φ^(−0.15 · rank),
/// periods ≈ 3.0 (rank 0) to 6.5 homes (rank 11). ON V5's band rather than below its slowest, because
/// a field whose period is longer than the reachable window (home −8..+11, about 19 homes) barely
/// changes anywhere a hand can go. The SLOWEST ranks are the LARGEST regions, so the fields that
/// define a country's character (rolloff, the formant centres) get the widest ones.
constexpr std::array<int, partial_slot::kFieldCount> kPartialFieldRank {
	11, // kRolloff
	10, // kFormant1Centre
	1,  // kFormant1Width
	6,  // kFormant1Gain
	9,  // kFormant2Centre
	0,  // kFormant2Width
	5,  // kFormant2Gain
	4,  // kOddEven
	3,  // kCombSpacing
	2,  // kCombDepth
	8,  // kDetail
	7,  // kOffGrid
	-1, // kSmoothness: V5's own field, by reference (below)
};

/// Each field's offset as a fraction of ITS OWN PERIOD: where on the triangle the origin (home 0) sits.
/// evalTriangle adds the offset to the PHASE before the rate multiplies it, so the stored offset is the
/// fraction over the rate (kFlagshipSmoothField's spelling). The gated fields' fractions sit past their
/// duty, so the origin is in every gated field's dead zone; they were picked by a search over a 0.05
/// grid for the widest spacing between gated peaks over home −9..+12 with every peak at least half a
/// home from the origin (MEASURED: closest pair 0.243 homes). Rolloff's 0.25 puts the origin at
/// mid-range (0.5), not the brightest end. The duty-1 fractions are a spread by hand.
constexpr std::array<float, partial_slot::kFieldCount> kPartialFieldOrigin {
	0.25f, // kRolloff: mid-range at the origin
	0.62f, // kFormant1Centre
	0.91f, // kFormant1Width
	0.95f, // kFormant1Gain (duty 0.45: dead at the origin)
	0.08f, // kFormant2Centre
	0.17f, // kFormant2Width
	0.60f, // kFormant2Gain (duty 0.45)
	0.95f, // kOddEven (duty 0.35)
	0.37f, // kCombSpacing
	0.25f, // kCombDepth (duty 0.20)
	0.81f, // kDetail
	0.45f, // kOffGrid (duty 0.35)
	0.0f,  // kSmoothness: unused, V5's own offset
};

/// What each duty buys (design §2.1, §2.3, §4.1).
constexpr std::array<float, partial_slot::kFieldCount> kPartialFieldDuty {
	1.0f,  // kRolloff: always a value, every spectrum has a slope
	1.0f,  // kFormant1Centre: read only where formant 1's gain is live,
	1.0f,  // kFormant1Width:   so centre and width need no off-region
	0.45f, // kFormant1Gain: moderate, formant country rather than everywhere
	1.0f,  // kFormant2Centre: as formant 1's
	1.0f,  // kFormant2Width
	0.45f, // kFormant2Gain
	0.35f, // kOddEven: moderate, hollow country is a region
	1.0f,  // kCombSpacing: read only where comb depth is live
	0.20f, // kCombDepth: SPARSE, dramatic (§5.3's grey-goo rule)
	1.0f,  // kDetail: always a value; 0 is "no detail" (its rest)
	0.35f, // kOffGrid: moderate, phase-2 §5.3's rank for inharmonicity
	0.0f,  // kSmoothness: unused, V5's own duty (0.20)
};

/// The bank, built once at load (std::pow is not constexpr). SMOOTHNESS IS V5's FIELD, kFlagshipSmoothField
/// itself: smooth country is the same place on both engines, by construction rather than by copying.
std::array<phi::PhiTriConfig, partial_slot::kFieldCount> makePartialFieldBank() {
	std::array<phi::PhiTriConfig, partial_slot::kFieldCount> bank {};
	for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
		if (r == partial_slot::kSmoothness) {
			bank[r] = kFlagshipSmoothField;
			continue;
		}
		const auto rate = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate)
		                                     * std::pow(std::numbers::phi, -0.15 * kPartialFieldRank[r]));
		bank[r] = { rate, kPartialFieldDuty[r], kPartialFieldOrigin[r] / rate, false };
	}
	return bank;
}

/// NOTHING MAY CALL THE PARTIAL BUILDERS DURING STATIC INITIALISATION: these two banks are dynamically
/// initialised, so a static in another TU reading them could see zeros.
const std::array<phi::PhiTriConfig, partial_slot::kFieldCount> kPartialFieldBank = makePartialFieldBank();
static_assert(partial_slot::kSmoothness == partial_slot::kFieldCount - 1);

/// Harmonic 1's detail rate, φ^-2.75 (V5's FM width rate, φ^-6.25, four and a half rungs up). Taste.
constexpr float kPartialDetailRate = kFlagshipFmWidthRate / phi::kPhi050 * phi::kPhi400;

/// The 320 detail configs, built once at load (not lazily: a lazily built static would build on the
/// first caller's thread, which may be the audio thread).
std::array<phi::PhiTriConfig, partials::kMaxPartials> makePartialDetailBank() {
	std::array<phi::PhiTriConfig, partials::kMaxPartials> bank {};
	constexpr double kPhiSquared = std::numbers::phi * std::numbers::phi;
	for (int k = 1; k <= partials::kMaxPartials; ++k) {
		// β = 0.5 (taste): harmonic 320's rate is φ^4.16 times harmonic 1's.
		const double rate = static_cast<double>(kPartialDetailRate)
		                    * std::pow(std::numbers::phi, 0.5 * std::log2(static_cast<double>(k)));
		const double periods = static_cast<double>(k) * kPhiSquared;
		const double offset = (periods - std::floor(periods)) / rate; // frac(k·φ²) of its own period
		bank[static_cast<size_t>(k - 1)] = { static_cast<float>(rate), 1.0f, static_cast<float>(offset), true };
	}
	return bank;
}

/// Dynamically initialised like the field bank: nothing may call the partial builders during static initialisation.
const std::array<phi::PhiTriConfig, partials::kMaxPartials> kPartialDetailBank = makePartialDetailBank();

/// **PAL STAGE 1'S FIELDS (design §3.7, 2026-10-19), READ THROUGH EXISTING ROWS** — each a triangle of its own (rate, duty, origin) read
/// at a HOST row's placement, as every detail harmonic reads its group's row: so the knee, the shelf, the body and sparse country
/// add no basis row, no residue value and no stance member (a row is also stored residue: see partial_slot). A host row's own field
/// and its guest share a travel direction but not a rate, so over the reachable window they are independent patterns. Ranks between
/// the existing ones (rate = V5's quantize rate · φ^(−0.15·rank), periods ~3 to 6.5 homes); the gated guests' origins sit past their
/// duty, so the origin is in their dead zone; the duties: knee and shelf MODERATE (0.35), the body and SPARSE COUNTRY SPARSE
/// (0.30: a region you find, as the comb's), the position fields always valued.
struct PartialExtraField {
	size_t host;
	double rank;
	float duty;
	float origin; ///< the origin's place on the triangle, as a fraction of its period
};
constexpr std::array<PartialExtraField, 7> kPartialExtraFields { {
	{ partial_slot::kCombSpacing, 7.5, 0.35f, 0.90f },    // knee
	{ partial_slot::kFormant1Width, 4.5, 0.35f, 0.85f },  // shelf
	{ partial_slot::kFormant2Width, 9.5, 1.0f, 0.40f },   // shelfAt
	{ partial_slot::kFormant2Centre, 6.5, 0.30f, 0.70f }, // body
	{ partial_slot::kFormant1Centre, 10.5, 1.0f, 0.30f }, // bodyAt
	{ partial_slot::kDetail, 8.5, 0.30f, 0.60f },         // sparsity
	{ partial_slot::kRolloff, 11.5, 1.0f, 0.15f },        // sparseSet: the slowest, so a set changes over regions
} };
std::array<phi::PhiTriConfig, kPartialExtraFields.size()> makePartialExtraBank() {
	std::array<phi::PhiTriConfig, kPartialExtraFields.size()> bank {};
	for (size_t i = 0; i < bank.size(); ++i) {
		const PartialExtraField& e = kPartialExtraFields[i];
		const auto rate = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate) * std::pow(std::numbers::phi, -0.15 * e.rank));
		bank[i] = { rate, e.duty, e.origin / rate, false };
	}
	return bank;
}
const std::array<phi::PhiTriConfig, kPartialExtraFields.size()> kPartialExtraBank = makePartialExtraBank();

/// **THE STRING'S FIVE FIELDS (KS design §2.1)**, guests of the Additive rows on PAL's road: each its own triangle at its host
/// row's placement, ranks between the existing ones. Hosts chosen so no two of the string's guests share a row with each other
/// or with a PAL guest. Brightness, Sustain and Pick always have a value; Noise is MODERATE (0.45) and Stretch SPARSE (0.25,
/// the grey-goo rule), their origins in their dead zones so the default coordinate plucks its own tone on a harmonic string.
constexpr std::array<PartialExtraField, 5> kStringExtraFields { {
	{ partial_slot::kOddEven, 3.5, 1.0f, 0.55f },       // brightness
	{ partial_slot::kCombDepth, 5.5, 1.0f, 0.35f },     // sustain
	{ partial_slot::kFormant1Gain, 2.5, 1.0f, 0.20f },  // pick
	{ partial_slot::kFormant2Gain, 8.0, 0.45f, 0.80f }, // noise
	{ partial_slot::kOffGrid, 6.0, 0.25f, 0.85f },      // stretch
} };
std::array<phi::PhiTriConfig, kStringExtraFields.size()> makeStringExtraBank() {
	std::array<phi::PhiTriConfig, kStringExtraFields.size()> bank {};
	for (size_t i = 0; i < bank.size(); ++i) {
		const PartialExtraField& e = kStringExtraFields[i];
		const auto rate = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate) * std::pow(std::numbers::phi, -0.15 * e.rank));
		bank[i] = { rate, e.duty, e.origin / rate, false };
	}
	return bank;
}
const std::array<phi::PhiTriConfig, kStringExtraFields.size()> kStringExtraBank = makeStringExtraBank();

/// **THE DRUM'S SEVEN FIELDS (drum proposal §3.1)**, guests of the Additive rows as the string's are. A drum sound never
/// shares a voice with a string or an Additive spectrum, so its guests may sit on rows theirs use; their ranks and origins are
/// their own. Pitch, Length, Tone and Snap always have a value (duty 1); Sweep and Noise are moderate; Metal SPARSE (0.25, the
/// grey-goo rule: most of the map rings as a skin).
constexpr std::array<PartialExtraField, 7> kDrumExtraFields { {
	{ partial_slot::kFormant1Centre, 3.0, 1.0f, 0.25f }, // pitch
	{ partial_slot::kRolloff, 4.0, 1.0f, 0.40f },        // length
	{ partial_slot::kCombSpacing, 5.0, 0.6f, 0.30f },    // sweep
	{ partial_slot::kOddEven, 2.0, 1.0f, 0.60f },        // tone
	{ partial_slot::kFormant2Gain, 6.5, 0.5f, 0.85f },   // noise
	{ partial_slot::kFormant1Width, 3.5, 1.0f, 0.45f },  // snap
	{ partial_slot::kOffGrid, 7.0, 0.25f, 0.85f },       // metal
} };
std::array<phi::PhiTriConfig, kDrumExtraFields.size()> makeDrumExtraBank() {
	std::array<phi::PhiTriConfig, kDrumExtraFields.size()> bank {};
	for (size_t i = 0; i < bank.size(); ++i) {
		const PartialExtraField& e = kDrumExtraFields[i];
		const auto rate = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate) * std::pow(std::numbers::phi, -0.15 * e.rank));
		bank[i] = { rate, e.duty, e.origin / rate, false };
	}
	return bank;
}
const std::array<phi::PhiTriConfig, kDrumExtraFields.size()> kDrumExtraBank = makeDrumExtraBank();

} // namespace

const std::array<phi::PhiTriConfig, partial_slot::kFieldCount>& partialFieldBank() {
	return kPartialFieldBank;
}

phi::PhiTriConfig partialDetailConfig(int k) {
	return kPartialDetailBank[static_cast<size_t>(std::clamp(k, 1, partials::kMaxPartials) - 1)];
}

float partialDetailBaseRate() {
	return kPartialDetailRate;
}

partials::PartialFields readPartialFields(const PartialBasis& frame, double home, double travelX, double travelY) {
	const auto field = [&](size_t row) {
		return phi::evalTriangle(frame.phaseFor(row, home, travelX, travelY), frame.rateFor(row, travelX, travelY),
		                         kPartialFieldBank[row]);
	};
	partials::PartialFields f;
	f.rolloff = field(partial_slot::kRolloff);
	f.formant1Centre = field(partial_slot::kFormant1Centre);
	f.formant1Width = field(partial_slot::kFormant1Width);
	f.formant1Gain = field(partial_slot::kFormant1Gain);
	f.formant2Centre = field(partial_slot::kFormant2Centre);
	f.formant2Width = field(partial_slot::kFormant2Width);
	f.formant2Gain = field(partial_slot::kFormant2Gain);
	f.oddEven = field(partial_slot::kOddEven);
	f.combSpacing = field(partial_slot::kCombSpacing);
	f.combDepth = field(partial_slot::kCombDepth);
	f.detail = field(partial_slot::kDetail);
	f.offGrid = field(partial_slot::kOffGrid);
	f.smoothness = field(partial_slot::kSmoothness);
	// PAL STAGE 1: the guests, each at its host row's placement and its own triangle.
	const auto guest = [&](size_t i) {
		const size_t row = kPartialExtraFields[i].host;
		return phi::evalTriangle(frame.phaseFor(row, home, travelX, travelY), frame.rateFor(row, travelX, travelY),
		                         kPartialExtraBank[i]);
	};
	f.knee = guest(0);
	f.shelf = guest(1);
	f.shelfAt = guest(2);
	f.body = guest(3);
	f.bodyAt = guest(4);
	f.sparsity = guest(5);
	f.sparseSet = guest(6);
	return f;
}

void readPartialDetail(const PartialBasis& frame, double home, double travelX, double travelY,
                       std::array<float, partials::kMaxPartials>& detail) {
	// Each group's placement read once and shared by every harmonic in it — the arithmetic of reading
	// it per harmonic, done four times instead of 320.
	std::array<double, partial_slot::kDetailGroups> phase {};
	std::array<float, partial_slot::kDetailGroups> rate {};
	for (size_t g = 0; g < partial_slot::kDetailGroups; ++g) {
		phase[g] = frame.phaseFor(partial_slot::kDetailGroup + g, home, travelX, travelY);
		rate[g] = frame.rateFor(partial_slot::kDetailGroup + g, travelX, travelY);
	}
	for (int k = 1; k <= partials::kMaxPartials; ++k) {
		const size_t g = partialDetailRow(k) - partial_slot::kDetailGroup;
		const auto i = static_cast<size_t>(k - 1);
		detail[i] = phi::evalTriangle(phase[g], rate[g], kPartialDetailBank[i]);
	}
}

StringFields readStringFields(const PartialBasis& frame, double home, double travelX, double travelY, const PhaseWarp* warp) {
	// THE PHASE WARP (P-MÖBIUS): this sound's warp on the five String sites, in the order read below.
	const AngleWarpScope likes(warp);
	const AngleKindScope sites(AngleKind::String);
	const auto guest = [&](size_t i) {
		const size_t row = kStringExtraFields[i].host;
		return phi::evalTriangle(frame.phaseFor(row, home, travelX, travelY), frame.rateFor(row, travelX, travelY),
		                         kStringExtraBank[i]);
	};
	StringFields f;
	f.brightness = guest(0);
	f.sustain = guest(1);
	f.pick = guest(2);
	f.noise = guest(3);
	f.stretch = guest(4);
	return f;
}

StringFields stringFieldsWithOverrides(const StringFields& raw, const StringBias& bias) {
	StringFields f;
	f.brightness = fieldWithOverride(raw.brightness, bias.brightness);
	f.sustain = fieldWithOverride(raw.sustain, bias.sustain);
	f.pick = fieldWithOverride(raw.pick, bias.pick);
	f.noise = fieldWithOverride(raw.noise, bias.noise);
	f.stretch = fieldWithOverride(raw.stretch, bias.stretch);
	return f;
}

drum::Fields readDrumFields(const PartialBasis& frame, double home, double travelX, double travelY, const PhaseWarp* warp) {
	// THE PHASE WARP (P-MÖBIUS): this sound's warp on the seven Drum sites, in the order read below.
	const AngleWarpScope likes(warp);
	const AngleKindScope sites(AngleKind::Drum);
	const auto guest = [&](size_t i) {
		const size_t row = kDrumExtraFields[i].host;
		return phi::evalTriangle(frame.phaseFor(row, home, travelX, travelY), frame.rateFor(row, travelX, travelY),
		                         kDrumExtraBank[i]);
	};
	drum::Fields f;
	f.pitch = guest(0);
	f.length = guest(1);
	f.sweep = guest(2);
	f.tone = guest(3);
	f.noise = guest(4);
	f.snap = guest(5);
	f.metal = guest(6);
	return f;
}

drum::Fields drumFieldsWithOverrides(const drum::Fields& raw, const DrumBias& bias) {
	drum::Fields f;
	f.pitch = fieldWithOverride(raw.pitch, bias.pitch);
	f.length = fieldWithOverride(raw.length, bias.length);
	f.sweep = fieldWithOverride(raw.sweep, bias.sweep);
	f.tone = fieldWithOverride(raw.tone, bias.tone);
	f.noise = fieldWithOverride(raw.noise, bias.noise);
	f.snap = fieldWithOverride(raw.snap, bias.snap);
	f.metal = fieldWithOverride(raw.metal, bias.metal);
	return f;
}

partials::PartialFields partialFieldsWithOverrides(const partials::PartialFields& raw, float smoothnessScale,
                                                   const PartialBias& bias) {
	partials::PartialFields f = raw;
	f.smoothness = fieldWithOverride(raw.smoothness, smoothnessScale);
	f.rolloff = fieldWithOverride(raw.rolloff, bias.rolloff);
	f.formant1Gain = fieldWithOverride(raw.formant1Gain, bias.formant);
	f.formant2Gain = fieldWithOverride(raw.formant2Gain, bias.formant);
	f.oddEven = fieldWithOverride(raw.oddEven, bias.oddEven);
	f.combDepth = fieldWithOverride(raw.combDepth, bias.comb);
	f.detail = fieldWithOverride(raw.detail, bias.detail);
	f.offGrid = fieldWithOverride(raw.offGrid, bias.offGrid);
	return f;
}

void buildPartialSound(const PartialBasis& frame, double home, double travelX, double travelY,
                       partials::PartialSpectrum& out, float smoothnessScale, const PartialBias& bias,
                       const PhaseWarp* warp) {
	// THE PHASE WARP (P-MÖBIUS): this sound's warp on every one of its Partials sites (320 harmonics, then 20 fields).
	const AngleWarpScope likes(warp);
	const AngleKindScope sites(AngleKind::Partials);
	std::array<float, partials::kMaxPartials> detail;
	readPartialDetail(frame, home, travelX, travelY, detail);
	partials::buildSound(
	    partialFieldsWithOverrides(readPartialFields(frame, home, travelX, travelY), smoothnessScale, bias), detail,
	    out);
}

} // namespace feathers
