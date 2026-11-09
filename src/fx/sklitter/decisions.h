/*
 * Copyright © 2024-2026 Owlet Records
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
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

//--------------------------------------------------------------------------------------------------//
// A VERBATIM PORT, LIKE hash_random.h. DO NOT TIDY IT.                                               //
//--------------------------------------------------------------------------------------------------//
//
// Lifted from the reference's `dsp/scatter.hpp` and `model/fx/stutterer.cpp` (reference firmware c1613cb55).
// Owlet Records wrote the originals. Only the licence header, the namespace, and `static constexpr`
// -> `inline constexpr` on the two tables (they were file-scope in a .cpp and are header-scope here)
// differ from the source.
//
// **WHAT BELONGS IN THIS FILE AND WHAT DOES NOT.** Everything here is a PURE function of hash bits, a
// triangle value, or nothing at all. Scatter's mode STRUCTURE is being re-expressed against Feathers'
// vocabulary; this layer is not, because it is where the character actually lives -- the exact
// thresholds, the exact seeds, the exact eight length steps. Re-expressing the structure is only
// survivable because this sits underneath it unchanged.
//
// Nothing that touches `q31_t`, `StereoSample` or NEON is here; that belongs to the re-expressed
// layer, which converts at its own edge.
//
// `tests/test_fx_sklitter_decisions.cpp` pins all of it. If that suite goes red, this file stopped
// being a copy -- do not update the constants to match it.

#include "character/phi_triangle.hpp"
#include "fx/sklitter/hash_random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::sklitter {

namespace phi = owlet::dsp::phi;

// Convenience aliases, as in the reference.
using HashBits = hash::Bits;
using HashContext = hash::Context;

namespace HashSeed {
constexpr uint32_t ReverseDecision = 0x12345678u; // Bool: should reverse this slice?
constexpr uint32_t SkipDecision = 0x9ABCDEF0u;    // Bool: should skip to non-adjacent?
constexpr uint32_t SkipTarget = 0x11223344u;      // Int: which slice to skip to
constexpr uint32_t DryMix = 0x2468ACE0u;          // Bool: use dry instead of grain?
constexpr uint32_t BinarySubdiv = 0x13579BDFu;    // Duty: binary subdivision level
constexpr uint32_t TripletSubdiv = 0xFEDCBA98u;   // Duty: triplet subdivision level
constexpr uint32_t SliceOffset = 0xAABBCCDDu;     // Int: offset added to slice index
constexpr uint32_t LengthMult = 0x55667788u;      // Nibble: length multiplier level
constexpr uint32_t DelayRatio = 0xDEADBEEFu;      // 2 bits: power-of-2 delay multiplier
constexpr uint32_t DelayDecision = 0xBAADF00Du;   // Bool: should apply delay this slice?
constexpr uint32_t PitchDecision = 0xCAFEBABEu;   // Bool: should pitch up (2x decimation)?
constexpr uint32_t RepeatSlice = 0xFACEFEEDu;     // Duty: repeat slice probability (inverse of ratchet)
constexpr uint32_t LongGrain = 0xBEEFCAFEu;       // Duty: combine consecutive slices into one grain
/// Bool: does this grain go down the GRAIN LANE? Backlog EW.
///
/// **ITS OWN SEED, DRAWN AGAINST THE SAME PROBABILITY AS `DelayDecision`.** The two destinations are
/// the same CHARACTER — how often phi sends a grain somewhere — and `Send Override` steers both,
/// which is what makes it one control rather than two. But they are not the same EVENT: sharing
/// `DelayDecision`'s draw would make every lane grain also an echo grain and vice versa, so the two
/// effects could never be heard apart. A separate seed costs nothing and keeps them independent at
/// the same density. (The device already does exactly this twice, in Grain mode: `grainDrawIsDry`
/// and `grainDrawWrites` hash the same index under different salts *"so the two draws do not move
/// together"*.)
constexpr uint32_t GrainLaneDecision = 0x5AFEC0DEu;
} // namespace HashSeed

/**
 * Compute delay time as power-of-2 multiple of base time (32nd note grid)
 * Uses 2 bits from hash: 0=1/4x, 1=1/2x, 2=1x, 3=2x
 * All bit shifts, no multiply or divide (~1 cycle)
 */
[[gnu::always_inline]] inline size_t computeDelayTimeRatio(size_t baseTime, uint32_t hashBits) {
	// 2 bits select power-of-2 multiplier: 1/4, 1/2, 1, 2
	switch (hashBits & 0x3) {
	case 0:
		return baseTime >> 2; // 1/4x (32nd if slice is 8th)
	case 1:
		return baseTime >> 1; // 1/2x (16th if slice is 8th)
	case 2:
		return baseTime; // 1x (same as slice)
	case 3:
		return baseTime << 1; // 2x (quarter if slice is 8th)
	default:
		return baseTime;
	}
}

// Length multiplier discrete levels (8 steps from 0.5 to 1.0)
constexpr float kLengthMultLevels[8] = {0.5f, 0.5625f, 0.625f, 0.6875f, 0.75f, 0.8125f, 0.875f, 1.0f};

/**
 * Phi triangle bank for structural scatter params (Zone A meta)
 *
 * Bank indices:
 *   [0] sliceOffset - Offset added to slice selection
 *   [1] lengthMult  - Slice length multiplier
 *   [2] skipProb    - Probability of skipping to non-adjacent slice
 *
 * Slower φ^n frequencies for gradual structural evolution.
 * Phase offsets spread for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 3> kStructuralBank = {{
    {phi::kPhi100, 0.80f, 0.00f, false}, // [0] sliceOffset
    {phi::kPhi150, 0.60f, 0.33f, false}, // [1] lengthMult
    {phi::kPhi175, 0.70f, 0.67f, false}, // [2] skipProb
}};

/**
 * Phi triangle bank for timbral scatter params (Zone B meta)
 *
 * Bank indices:
 *   [0] reverseProb - Probability of reversing slice
 *   [1] filterFreq  - Bandpass center frequency
 *   [2] delayFeed   - Per-grain delay send amount
 *   [3] envShape    - Envelope shape (percussive to reversed)
 *
 * Mix of slower and faster φ^n for varied timbral movement.
 * Phase offsets spread by 0.25 for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 4> kTimbraBank = {{
    {phi::kPhiN050, 0.50f, 0.00f, false}, // [0] reverseProb (slow)
    {phi::kPhi067, 0.70f, 0.25f, false},  // [1] filterFreq
    {phi::kPhi125, 0.60f, 0.50f, false},  // [2] delayFeed
    {phi::kPhi200, 0.80f, 0.75f, false},  // [3] envShape (faster)
}};

/**
 * Map triangle value [0,1] to slice index within a zone
 * @param tri Triangle value in [0,1]
 * @param numSlices Total number of slices in the zone
 * @return Slice index [0, numSlices-1]
 */
[[gnu::always_inline]] inline int32_t sliceIndexFromTriangle(float tri, int32_t numSlices) {
	if (numSlices <= 1) {
		return 0;
	}
	int32_t idx = static_cast<int32_t>(tri * static_cast<float>(numSlices));
	return std::clamp(idx, int32_t{0}, numSlices - 1);
}

/**
 * Map triangle value [0,1] to gate duty cycle
 * Never fully mutes - minimum gate is 12.5%
 * @param tri Triangle value in [0,1]
 * @return Gate ratio [0.125, 1.0]
 */
[[gnu::always_inline]] inline float gateFromTriangle(float tri) {
	return 0.125f + tri * 0.875f;
}

/**
 * Map triangle value [0,1] to reverse decision
 * @param tri Triangle value in [0,1]
 * @param threshold Probability threshold for reversing
 * @return true if slice should be reversed
 */
[[gnu::always_inline]] inline bool reverseFromTriangle(float tri, float threshold) {
	return tri > threshold;
}

/**
 * Map triangle value [0,1] to pitch ratio
 * Center is 1.0 (no pitch change), range depends on depth
 * @param tri Triangle value in [0,1]
 * @param semitoneRange Maximum semitone deviation (e.g., 12 for octave)
 * @return Pitch ratio
 */
[[gnu::always_inline]] inline float pitchFromTriangle(float tri, float semitoneRange) {
	// Map [0,1] to [-semitoneRange, +semitoneRange] semitones
	float semitones = (tri * 2.0f - 1.0f) * semitoneRange;
	// Convert semitones to pitch ratio: 2^(semitones/12)
	// Use fast approximation: exp2f(semitones / 12.0f)
	return std::exp2(semitones / 12.0f);
}

/**
 * Map triangle value [0,1] to tape speed ratio
 * @param tri Triangle value in [0,1]
 * @param minSpeed Minimum speed (e.g., 0.5 for half speed)
 * @param maxSpeed Maximum speed (e.g., 2.0 for double speed)
 * @return Speed ratio
 */
[[gnu::always_inline]] inline float tapeSpeedFromTriangle(float tri, float minSpeed = 0.5f, float maxSpeed = 2.0f) {
	return minSpeed + tri * (maxSpeed - minSpeed);
}

/**
 * Map triangle value [0,1] to filter frequency
 * Uses exponential mapping for perceptually linear sweep
 * @param tri Triangle value in [0,1]
 * @param minFreq Minimum frequency in Hz
 * @param maxFreq Maximum frequency in Hz
 * @return Frequency in Hz
 */
[[gnu::always_inline]] inline float filterFreqFromTriangle(float tri, float minFreq = 200.0f, float maxFreq = 8000.0f) {
	// Exponential mapping for perceptually linear sweep
	float logMin = std::log2(minFreq);
	float logMax = std::log2(maxFreq);
	return std::exp2(logMin + tri * (logMax - logMin));
}

// The pitch tables, from the reference's stutterer.cpp.

inline constexpr int8_t kScaleSemitones[25][8] = {
    {0, 0, 0, 3, 5, 7, 7, 12},        // 0: Chromatic: tonic-heavy with 5th and octave
    {0, 0, 4, 4, 7, 7, 0, 12},        // 1: Major: tonic (3x), 3rd (2x), 5th (2x), octave
    {0, 0, 3, 3, 7, 7, 0, 12},        // 2: Minor: tonic (3x), m3rd (2x), 5th (2x), octave
    {0, 0, 4, 7, 7, 0, 12, 12},       // 3: MajPent: tonic (3x), 3rd, 5th (2x), octave (2x)
    {0, 0, 3, 7, 7, 0, 10, 12},       // 4: MinPent: tonic (3x), m3rd, 5th (2x), b7, octave
    {0, 0, 3, 6, 7, 7, 0, 12},        // 5: Blues: tonic (3x), m3rd, b5, 5th (2x), octave
    {0, 0, 3, 5, 7, 7, 9, 12},        // 6: Dorian: tonic (2x), m3rd, 4th, 5th (2x), 6th, octave
    {0, 0, 4, 5, 7, 7, 10, 12},       // 7: Mixolydian: tonic (2x), 3rd, 4th, 5th (2x), b7, octave
    {0, 0, 0, 4, 4, 7, 7, 12},        // 8: MajTri: tonic (3x), 3rd (2x), 5th (2x), octave
    {0, 0, 0, 3, 3, 7, 7, 12},        // 9: MinTri: tonic (3x), m3rd (2x), 5th (2x), octave
    {0, 0, 0, 5, 5, 7, 7, 12},        // 10: Sus4: tonic (3x), 4th (2x), 5th (2x), octave
    {0, 0, 0, 3, 3, 6, 6, 12},        // 11: Dim: tonic (3x), m3rd (2x), b5 (2x), octave
    {1, 1, 1, 1, 1, 1, 1, 1},         // 12: +1 semitone (minor 2nd)
    {2, 2, 2, 2, 2, 2, 2, 2},         // 13: +2 semitones (major 2nd)
    {3, 3, 3, 3, 3, 3, 3, 3},         // 14: +3 semitones (minor 3rd)
    {4, 4, 4, 4, 4, 4, 4, 4},         // 15: +4 semitones (major 3rd)
    {5, 5, 5, 5, 5, 5, 5, 5},         // 16: +5 semitones (perfect 4th)
    {6, 6, 6, 6, 6, 6, 6, 6},         // 17: +6 semitones (tritone)
    {7, 7, 7, 7, 7, 7, 7, 7},         // 18: +7 semitones (perfect 5th)
    {8, 8, 8, 8, 8, 8, 8, 8},         // 19: +8 semitones (minor 6th)
    {9, 9, 9, 9, 9, 9, 9, 9},         // 20: +9 semitones (major 6th)
    {10, 10, 10, 10, 10, 10, 10, 10}, // 21: +10 semitones (minor 7th)
    {11, 11, 11, 11, 11, 11, 11, 11}, // 22: +11 semitones (major 7th)
    {12, 12, 12, 12, 12, 12, 12, 12}, // 23: +12 semitones (octave)
    {13, 13, 13, 13, 13, 13, 13, 13}, // 24: +13 semitones (octave + minor 2nd)
};

inline constexpr uint32_t kPitchRatioFP[18] = {
    65536,  // 0: 1.0000
    69433,  // 1: 1.0595
    73562,  // 2: 1.1225
    77936,  // 3: 1.1892
    82570,  // 4: 1.2599
    87480,  // 5: 1.3348
    92682,  // 6: 1.4142
    98193,  // 7: 1.4983
    104032, // 8: 1.5874
    110218, // 9: 1.6818
    116772, // 10: 1.7818
    123715, // 11: 1.8877
    131072, // 12: 2.0000 (octave)
    138866, // 13: 2.1189
    147123, // 14: 2.2449
    155872, // 15: 2.3784
    165140, // 16: 2.5198
    174959, // 17: 2.6697
};

//--------------------------------------------------------------------------------------------------//
// THE GRAIN ENVELOPE — also verbatim, also pure.                                                     //
//--------------------------------------------------------------------------------------------------//
//
// What shapes one slice on its way out: the gate cuts it short, `envShape` moves the peak from
// percussive (0) through hanning (0.5) to reversed (1), and `depth` blends the whole shape against a
// hard cut. Six multiplies and no trig, which is why the reference can afford it per sample.

/**
 * Compute grain envelope multiplier with configurable shape
 * Very cheap: ~6 multiplies, few branches, no trig
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param sliceLength Total length of slice in samples
 * @param gateRatio Gate duty cycle [0,1] - audio plays during this portion
 * @param depth Envelope depth [0,1] - 0=hard cut, 1=full smooth envelope
 * @param envShape Peak position [0,1] - 0=fade-out only, 0.5=symmetric, 1=fade-in only
 * @param envWidth Envelope region [0,1] - 1=full slice, 0.1=edges only (10% each end)
 * @return Amplitude multiplier [0,1]
 */
[[gnu::always_inline]] inline float grainEnvelope(int32_t positionInSlice, int32_t sliceLength, float gateRatio,
                                                  float depth, float envShape = 0.5f, float envWidth = 1.0f) {
	if (sliceLength <= 0) {
		return 1.0f;
	}

	// Anti-click now handled by zero-crossing mute in stutterer.cpp
	// Attack: mute until zero crossing, Release: mute after zero crossing

	// Normalized position within slice [0,1]
	float pos = static_cast<float>(positionInSlice) / static_cast<float>(sliceLength);

	// If past gate threshold, output is silent
	if (pos > gateRatio) {
		return 0.0f;
	}

	// Rescale position to [0,1] within the gated portion
	float gatedPos = pos / gateRatio;

	// Edge-only mode: flat middle with envelope only at edges
	// envWidth=1.0: full envelope, envWidth=0.1: only first/last 10%
	float envelope;
	if (envWidth < 1.0f && envWidth > 0.0f) {
		float edgeSize = envWidth * 0.5f; // Half at each end
		if (gatedPos < edgeSize) {
			// Attack region - remap to [0, envShape]
			float t = gatedPos / edgeSize;
			// Parabolic attack: t^2 for smooth start
			float attack = (envShape > 0.001f) ? t * t : 1.0f;
			envelope = attack;
		}
		else if (gatedPos > (1.0f - edgeSize)) {
			// Decay region - remap to [envShape, 1]
			float t = (gatedPos - (1.0f - edgeSize)) / edgeSize;
			// Parabolic decay: (1-t)^2 for smooth end
			float decay = (envShape < 0.999f) ? (1.0f - t) * (1.0f - t) : 1.0f;
			envelope = decay;
		}
		else {
			// Flat middle region
			envelope = 1.0f;
		}
	}
	else {
		// Full slice envelope with configurable peak position (envShape)
		// envShape=0: peak at start (fade-out only, preserves attack)
		// envShape=0.5: peak at middle (symmetric Hanning-like)
		// envShape=1: peak at end (fade-in only)
		if (envShape < 0.001f) {
			// Fade-out only: (1-pos)^2
			envelope = (1.0f - gatedPos) * (1.0f - gatedPos);
		}
		else if (envShape > 0.999f) {
			// Fade-in only: pos^2
			envelope = gatedPos * gatedPos;
		}
		else {
			// Asymmetric envelope with peak at envShape position
			if (gatedPos < envShape) {
				// Attack phase: parabolic ramp up to peak
				float t = gatedPos / envShape;
				envelope = t * t;
			}
			else {
				// Decay phase: parabolic ramp down from peak
				float t = (gatedPos - envShape) / (1.0f - envShape);
				envelope = (1.0f - t) * (1.0f - t);
			}
		}
	}

	// Depth-controlled envelope
	// depth=0: no envelope (passthrough)
	// depth=1: full envelope shape
	return 1.0f + depth * (envelope - 1.0f);
}

} // namespace feathers::fx::sklitter
