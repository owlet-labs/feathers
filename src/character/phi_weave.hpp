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
#include <cstdint>

namespace owlet::dsp {

// ============================================================================
// PHI_WEAVE — scanned-physics oscillator: THE LAWS
//
// A ring of 32 masses on springs, simulated at CONTROL rate and scanned at
// audio rate as a wavetable (Mathews/Verplank scanned synthesis, on a closed
// loop so the scanned cycle is seam-free by topology). Pitch = scan speed;
// timbre = the physics. The phase doesn't select a waveform — it selects the
// LAWS: where the ring is stiff, where it's damp, what shape it relaxes toward,
// and how it's continuously bowed.
//
// This file is the float port of the firmware's bank table and
// buildPhiWeaveParams (reference firmware 2567b04c, dsp/phi_weave.{hpp,cpp}). It is
// stateless; the ring that obeys these laws is src/dsp/weave_string.h.
//
// Literature:
//   Verplank, B., Mathews, M., Shaw, R., "Scanned Synthesis", Proc. ICMC 2000.
//   Boulanger, R., Smaragdis, P., ffitch, J., "Scanned Synthesis: An
//     Introduction and Demonstration...", Proc. ICMC 2000.
//   Verlet, L., "Computer 'Experiments' on Classical Fluids. I.",
//     Phys. Rev. 159(1), 1967 (the integration scheme).
// ============================================================================

inline constexpr int32_t kPhiWeaveNumNodes = 32;

// Physics safety rails. Leapfrog with dt=1 tick is stable while
// (stiffness + 4*coupling) < 4; the clamps below keep it under 1.92.
inline constexpr float kPhiWeaveMaxVelocity = 0.5f;
inline constexpr float kPhiWeaveMaxDisplacement = 2.0f;

// ============================================================================
// Phi Triangle Bank Configurations
// ============================================================================

// --- Global physics scalars ---

// Base stiffness (unipolar, exponentially mapped 0.0008..0.06):
// how strongly nodes are pulled toward the home shape (slack rubber -> taut wire)
inline constexpr phi::PhiTriConfig kPhiWeaveStiffBase = {phi::kPhi125, 0.6f, 0.000f, false};

// Base neighbor coupling (unipolar, 0.01..0.29): wave propagation speed around
// the ring — low = slow smearing ripples, high = fast glassy travel
inline constexpr phi::PhiTriConfig kPhiWeaveCoupleBase = {phi::kPhiN050, 0.7f, 0.210f, false};

// Base damping (unipolar, 0.0015..0.045): how quickly motion dies back to the
// home shape — breathing organ vs quickly-settling pluck body
inline constexpr phi::PhiTriConfig kPhiWeaveDampBase = {phi::kPhi250, 0.5f, 0.420f, false};

// Spatial cycle counts (1..4 bumps around the ring). Inline literals in the
// firmware's builder; named here so the test can restate them.
inline constexpr phi::PhiTriConfig kPhiWeaveStiffCycles = {phi::kPhi050, 1.0f, 0.470f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveDampCycles = {phi::kPhi325, 1.0f, 0.720f, false};

// --- Spatial landscapes (evaluated per node with phase-dependent placement) ---

inline constexpr phi::PhiTriConfig kPhiWeaveStiffLand1 = {phi::kPhi075, 0.8f, 0.060f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveStiffLand2 = {phi::kPhi300, 0.5f, 0.530f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveDampLand1 = {phi::kPhi175, 0.7f, 0.140f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveDampLand2 = {phi::kPhiN025, 0.6f, 0.660f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveCoupleLand = {phi::kPhi225, 0.6f, 0.310f, true};

// --- Home shape: partial amounts and phases for the string's rest pose ---

inline constexpr phi::PhiTriConfig kPhiWeaveHomeP2 = {phi::kPhi150, 0.6f, 0.480f, true};  // 2nd partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomeP3 = {phi::kPhi325, 0.5f, 0.760f, true};  // 3rd partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomeP5 = {phi::kPhi050, 0.35f, 0.900f, true}; // 5th partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomePh2 = {phi::kPhi025, 1.0f, 0.320f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveHomePh3 = {phi::kPhi275, 1.0f, 0.610f, false};

// --- Bow: continuous micro-excitation keeping the ring alive ---

inline constexpr phi::PhiTriConfig kPhiWeaveBowDepth = {phi::kPhi350, 0.55f, 0.190f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowRate = {phi::kPhi100, 0.8f, 0.370f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowPos = {phi::kPhi200, 1.0f, 0.740f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowSpread = {phi::kPhiN100, 0.7f, 0.880f, false};

// Bow mode: the phase selects the excitation character (evaluated per bank;
// forces crossfade under morph, so mixed-mode morphs are click-free). See WeaveBow.
inline constexpr phi::PhiTriConfig kPhiWeaveBowMode = {phi::kPhi150, 0.9f, 0.680f, false};

// --- Travel: slow rotation of the ring under the scan head (chorus-like drift) ---

inline constexpr phi::PhiTriConfig kPhiWeaveTravel = {phi::kPhi067, 0.5f, 0.250f, true};

// --- Pluck: note-on displacement injection ---

inline constexpr phi::PhiTriConfig kPhiWeavePluckPos = {phi::kPhi375, 1.0f, 0.090f, false};
inline constexpr phi::PhiTriConfig kPhiWeavePluckWidth = {phi::kPhi033, 0.7f, 0.440f, false};
inline constexpr phi::PhiTriConfig kPhiWeavePluckAmp = {phi::kPhi250, 0.6f, 0.820f, false};

// Scan focus: cutoff of the per-node smoother between the physics and the
// scan head. Scanned synthesis wants the scanned shape moving at HAPTIC rates
// (Verplank/Mathews: hand speeds); the raw string's fast modes (up to ~100Hz
// in stiff zones) read as pitch-independent inharmonic hash. Low = focused
// slow morph, high = shimmery (fast-mode sidebands up to ~60Hz let through).
inline constexpr phi::PhiTriConfig kPhiWeaveShimmer = {phi::kPhi067, 0.6f, 0.240f, false};

// --- Morph bow: how strongly crossfade MOTION excites the ring ---

inline constexpr phi::PhiTriConfig kPhiWeaveMorphBow = {phi::kPhi175, 0.7f, 0.150f, false};

// --- Output gain trim ---

inline constexpr phi::PhiTriConfig kPhiWeaveOutGain = {phi::kPhiN025, 0.9f, 0.560f, false};

// ============================================================================
// Types
// ============================================================================

/// The bow's excitation character.
enum class WeaveBow : uint8_t {
	Triangle,  ///< smooth push-pull
	StickSlip, ///< slow drag, fast snap (Helmholtz friction at haptic rate)
	Noise,     ///< lowpassed turbulence (rate = colour)
	Pulses,    ///< alternating-sign taps
	Walk,      ///< Brownian pressure wander
};
inline constexpr int kWeaveBowModeCount = 5;

struct PhiWeaveParams {
	float stiffness[kPhiWeaveNumNodes];
	float coupling[kPhiWeaveNumNodes];
	float damping[kPhiWeaveNumNodes];
	// Bow-force energy balance: 0.6 * min(1, sqrt(damping/ref)) per node. A bow
	// pumping an underdamped node reaches equilibrium energy ~ F^2/d, which
	// explodes where damping bottoms out (that WAS the mid-Silk hash: churn
	// energy rivaling the carrier). Scaling force by sqrt(d) bounds it.
	float bowBalance[kPhiWeaveNumNodes];
	WeaveBow bowMode;
	float home[kPhiWeaveNumNodes]; ///< The rest pose, normalised to ±1 peak.
	float bowDepth;
	float bowRate;
	float bowPos;
	float bowSpread;
	float travelRate;
	float pluckPos;
	float pluckWidth;
	float pluckAmp;
	float morphBowGain;
	float outGain;
	float shimmerAlpha; ///< Per-sub-tick one-pole coefficient for the scan smoother
};

/// The laws at one phase. The firmware's caller passed a zone and an offset and computed
/// `zone / 1023 + phaseOffset` inside; here the caller computes the phase, so any continuous
/// control can walk it. Stateless — runs only when the phase moves, never per sample.
PhiWeaveParams buildPhiWeaveLaws(double phase);

} // namespace owlet::dsp
