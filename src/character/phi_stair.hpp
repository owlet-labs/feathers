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

#pragma once

#include "character/phi_triangle.hpp"
#include "character/fixedpoint.h"
#include <cstdint>

// THE FIXED-POINT NAMES, BROUGHT IN EXPLICITLY. These were unqualified here because the firmware's
// util/fixedpoint.h put them at global scope; ours does not, and should not. src/shims/util/fixedpoint.h
// stood in during the replacement step and is deleted with the rest of the shims — this is what
// replaces it, in the file that actually uses the names rather than in a shadow of somebody else's path.
using feathers::character::q31_t;
using feathers::character::add_saturate;
using feathers::character::multiply_32x32_rshift32;
using feathers::character::multiply_accumulate_32x32_rshift32_rounded;
using feathers::character::q31_from_double_saturating;
using feathers::character::u32_from_double_saturating;

namespace owlet::dsp {

// ============================================================================
// PHI_STAIR — staircase wavetable oscillator
//
// The family's digital primitive: the waveform is a STAIRCASE of up to 16
// steps with zone-derived HEIGHTS (pattern families: brick alternation,
// terraced ramps, mesas, phi-random glyph levels, teeth...), WIDTHS
// (asymmetry landscapes), and a SLOPE dimension sweeping the risers from
// hard edges (pulse-class -6 dB/oct brightness) to fully ramped polygons.
// Step count is CONTINUOUS: the last step's width fades in fractionally, so
// zone sweeps and morphs never pop a step in or out.
//
// Architecture reuses the family's proven parts: MORPH's zone/wave/gamma
// interface, a 128-slot scan table rebuilt only when the morph moves
// (epsilon-guarded; a one-buffer smoothstep crossfade de-zippers rebuilds),
// WEAVE's pitch-adaptive anti-alias mips and dual-tap stereo zones, and
// halved-difference lerps throughout (steps sign-flip at full scale by
// design - the raw q31 difference would wrap).
// ============================================================================

inline constexpr int32_t kPhiStairSteps = 16;
inline constexpr int32_t kPhiStairSlots = 128;
inline constexpr int32_t kPhiStairSlotShift = 25; // 32 - log2(128)

// ============================================================================
// Phi Triangle Bank Configurations (per zone)
// ============================================================================

// Step count (continuous 2..16) and riser slope (0 = hard edge .. ~0.9 of a
// step width = almost triangle)
inline constexpr phi::PhiTriConfig kPhiStairCount = {phi::kPhi125, 0.7f, 0.000f, false};
inline constexpr phi::PhiTriConfig kPhiStairSlope = {phi::kPhi250, 0.6f, 0.140f, false};

// Height wander (per-step spatial landscape over the pattern) and tilt
// (monotonic lean added to any pattern - saw-ness)
inline constexpr phi::PhiTriConfig kPhiStairHeightLand = {phi::kPhi275, 0.6f, 0.290f, true};
inline constexpr phi::PhiTriConfig kPhiStairTilt = {phi::kPhiN050, 0.7f, 0.410f, true};

// Width asymmetry: spatial landscape over the steps (0 = even, 1 = extreme)
inline constexpr phi::PhiTriConfig kPhiStairAsym = {phi::kPhi175, 0.6f, 0.550f, false};
inline constexpr phi::PhiTriConfig kPhiStairAsymLand = {phi::kPhi325, 0.5f, 0.660f, true};

// Spatial cycle counts for the landscapes
inline constexpr phi::PhiTriConfig kPhiStairLandCycles = {phi::kPhi050, 1.0f, 0.780f, false};

// ============================================================================
// Types
// ============================================================================

struct PhiStairParams {
	float heights[kPhiStairSteps]; // Normalized to +/-1 peak
	float widths[kPhiStairSteps];  // Relative; includes the fractional-count fade; renormalized at table build
	float slope;                   // Riser length as a fraction of each step's width
};

// ============================================================================
// Function declarations
// ============================================================================

PhiStairParams buildPhiStairParams(uint16_t zone, float phaseOffset = 0.0f);

} // namespace owlet::dsp
