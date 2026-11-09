/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "character/phi_triangle.hpp"

#include <algorithm>
#include <cmath>

#include <array>
#include <cstddef>

// **THE SHAPER'S OWN N-SPACE — eight phi phases, and the plane a pad travels in.**
//
// `automod_phase_space.h` is this file for thirty-nine triangles. The shaper always had the same
// shape and nobody had built the plane over it: `deriveCurve` computes eight phases as
// `wrapPhase(gamma * kPhiNNN)` and reads its six curve weights out of them.
//
// **WHICH IS WHY GAMMA CANNOT BE BOUNDED.** By design, gamma is never bounded. It is not a
// preference — it is what the construction MEANS. Gamma is the phase driver,
// and `shaper_curve.h`'s own comment says it out loud: *"the quantity being reduced mod 1 grows with
// gamma while the answer stays in [0,1)"*. Its `0..1` declaration is a DRAG WINDOW, exactly as
// automod's `0..2^24` is, and the author of this file once read that declaration as a bound and built
// a whole wrong design on it — backlog BB's trap, which is that an endpoint is a value for a bounded
// control and a SENTINEL for an unbounded one, and a `ParamDesc` cannot tell you which.
//
// **AND WHY THE PAD IS A PLANE RATHER THAN TWO KNOBS ON A PICTURE.** x and y are ABSTRACT axes of a
// plane sitting in this space. `Rotate` turns its basis and leaves the puck; a drag decodes into a
// vector on the plane; the vector adds to the integrated trajectory. The device consumes however many
// dimensions it consumes, and the pad neither knows nor cares which — which is what lets one widget
// serve a device with eight and a device with thirty-nine.
//
// **NOTHING NAMED LIVES HERE.** By design, unnamed and unbounded unless there is a really good reason
// otherwise. There are no zones in this space and no regions of it are special.
// That is not tidiness: `Rotate` makes the map from plane position to these phases VARIABLE, so a name
// attached to a position would be false the moment the basis turned. The bands that used to be painted
// on the terrain were claiming exactly that fixed relationship.

namespace feathers::fx::shaper::phase_space {

namespace phi = owlet::dsp::phi;

/// THE DEVICE'S N — the curve's eight, then the extras' ten (backlog II, 2026-09-23). Against automod's
/// thirty-nine, and the only place the number is stated.
///
/// **THE EXTRAS JOINED BECAUSE THE FIRMWARE'S ZONE KNOB MOVES THEM.** I thought in the firmware
/// they were attached to the zone knob which ties back to the pad. They were: `deriveParametersWith
/// Phase` derives every extra from `shapeY` and the gamma phase, exactly as it derives the weights. The
/// pad reached them too until it became a joystick (bebb30e1, 2026-09-03), which gave travel to the
/// curve's eight phases only — so a drag changed the curve and left the comb, sub, rotation and slew
/// wherever Colour had put them. The extras' phases are indices `kCurvePhaseCount` onward.
inline constexpr size_t kCurvePhaseCount = 8;
inline constexpr size_t kPhaseCount = kCurvePhaseCount + 10;

/// **WHERE EACH EXTRA'S PHASE SITS** — `deriveCurve` adds `travel[k...]` to the phase it names.
inline constexpr size_t kDeadzoneWidthPhase = 8;
inline constexpr size_t kDeadzoneCentrePhase = 9;
inline constexpr size_t kHysteresisPhase = 10;
inline constexpr size_t kSubDepthPhase = 11;
inline constexpr size_t kSubIntervalPhase = 12; ///< the extras bank's four, in its order: interval,
inline constexpr size_t kCombStridePhase = 13;  ///< comb stride,
inline constexpr size_t kCombDepthPhase = 14;   ///< comb depth,
inline constexpr size_t kRotationPhase = 15;    ///< and bit rotation
inline constexpr size_t kSlewPhase = 16;
inline constexpr size_t kPreStagePhase = 17;
static_assert(kPreStagePhase + 1 == kPhaseCount);

/// **THE φ MULTIPLIERS, IN THE ORDER `deriveCurve` READS THEM.** Order is not cosmetic here: the
/// folded-travel blob stores one float per entry BY INDEX, so reordering this array silently
/// re-points every saved trajectory. Append only, and only with a state-block conversation.
inline constexpr std::array<float, kCurvePhaseCount> kPhi {
	phi::kPhi225, phi::kPhi200, phi::kPhi175, phi::kPhi250,
	phi::kPhi150, phi::kPhi125, phi::kPhi275, phi::kPhi100,
};

using Phases = std::array<double, kPhaseCount>;

/// **THE FIELD'S ACCELERATION IN COLOUR.** `deriveCurve`: `freqMult = 1 + yNorm^2 * 3`.
inline constexpr double kAccelFactor = 3.0;

/// **EACH BASIS HAS ITS OWN CONSTANT OFFSET, and dropping them was this file's first bug.** They are
/// what spreads the eight triangles around the cycle instead of stacking them; the order matches
/// `kPhi` and both match the order `deriveCurve` assigns its weights in.
inline constexpr std::array<double, kCurvePhaseCount> kOffset {
	0.0, 0.167, 0.333, 0.5, 0.667, 0.833, 0.25, 0.0,
};

/// **AND ASYMMETRY IS GENUINELY UNCORRELATED FROM THE OTHER SEVEN.** It does not merely carry a
/// different offset — `deriveCurve` gives it its OWN acceleration, `asymFreqMult = 1 + yNorm^2 *
/// (3 * 0.5)`, so it moves along a different curve in colour and the eight phases do not stay in fixed
/// proportion as colour changes.
///
/// **THE FIRST DRAFT OF THIS FILE CLAIMED THEY DID** — that all eight were one scalar times their own
/// φ, so the home was a LINE. That was read off two of the eight lines and was wrong. A review caught it:
/// each transfer basis has its own uncorrelated term.
inline constexpr double kAsymAccelScale = 0.5;

/// **WHERE THE FIELD SITS BEFORE ANY TRAVEL.**
///
/// **COLOUR AND GAMMA ARE THE SAME DIRECTION, AND THAT IS BY DESIGN.** color is by definition
/// in the same direction as gamma, gamma being the coarsest step. Both terms are `(scalar) * kPhi[i]`,
/// so they push along the same vector — MEASURED, the angle between `d(home)/d(colour)` and
/// `d(home)/d(gamma)` is **0 degrees at colour 0 and 5.5 degrees at colour 1**, never more. Gamma is
/// unbounded and linear (the coarse step); colour is clamped and accelerated (the fine one); and the
/// 5.5 degrees is bought entirely by asymmetry having its own acceleration.
///
/// **SO THE SHAPER HAS ONE COORDINATE DIRECTION AT THREE GRANULARITIES — colour, its phase, and gamma
/// — AND NO SECOND AXIS AT ALL.** The pad it had put gamma on x and colour on y, which is a LINE drawn
/// as a square: two axes 5.5 degrees apart. That is the nonconformance, exactly.
///
/// **AND IT IS WHY ONLY A PLANE CAN FIX IT.** A second dimension cannot be had by choosing different
/// parameters, because the parameters do not contain one. It has to come from travel through a basis,
/// which is the thing that moves the eight phases independently.
///
/// The home is where colour and gamma put the eight phases; travel then moves them independently,
/// because `SliceBasis::travelTerm` adds a different amount to each. Colour and gamma are not
/// themselves independent directions in this space — they are two ways of driving a largely shared
/// term — which is exactly why a pad wired straight to them cannot reach most of the space, and why
/// the plane exists.
///
/// **COLOUR IS CLAMPED AND GAMMA IS NOT, WHICH IS THE WHOLE DIFFERENCE BETWEEN THEM.** `deriveCurve`
/// clamps `yNorm` and leaves `ph = gamma` alone, so colour genuinely has ends and gamma genuinely does
/// not. That is readable from the code five lines apart, where the two `ParamDesc`s both say `0..1`
/// and cannot tell you which is which.
///
/// READ FRESH, never cached: the plane is a function of the home, so a knob edit between two drags
/// legitimately changes which plane the second one moves in.
[[nodiscard]] inline Phases homeFor(double colour, double gamma) {
	const double yNorm = std::clamp(colour, 0.0, 1.0);
	const double freqMult = 1.0 + yNorm * yNorm * kAccelFactor;
	const double asymMult = 1.0 + yNorm * yNorm * (kAccelFactor * kAsymAccelScale);
	Phases home {};
	// THE CURVE'S EIGHT: the extras' entries stay 0 — the home only anchors the plane, through
	// `planeHomeOf`, which reads the curve's eight (see there). The extras ride the travel alone.
	for (size_t i = 0; i < kCurvePhaseCount; ++i) {
		const double phi = static_cast<double>(kPhi[i]);
		// THE LAST ENTRY IS ASYMMETRY AND IT USES ITS OWN MULTIPLIER. Indexed rather than named
		// because the blob is indexed; `kPhi`'s order is the contract and this follows it.
		const double mult = (i + 1 == kCurvePhaseCount) ? asymMult : freqMult;
		// UNWRAPPED, DELIBERATELY. `deriveCurve` wraps at the point of USE; the home carries the
		// unwrapped sum so travel can accumulate past a turn without it folding underneath — which is
		// the whole of what "unbounded" buys.
		home[i] = yNorm * phi * mult + gamma * phi + kOffset[i];
	}
	return home;
}



/// **THE ACCUMULATOR, REDUCED FOR STORAGE — modulo ONE, not modulo a per-triangle period.**
///
/// Automod wraps each entry by its own triangle's period because its offsets are consumed as
/// `wrapPhase(offset * kPhiNNN)`, so an offset's period is `1/kPhiNNN`. The shaper's travel is added
/// to a phase that is ALREADY the product — `deriveCurve` does `wrapPhase(base + ph + travel)` — so
/// one whole turn is one, for every entry.
///
/// **IT IS REDUCED AT ALL BECAUSE A FLOAT WITH A LARGE INTEGER PART HAS NO FRACTION LEFT.** The
/// trajectory is unbounded by design and a player can keep pushing; stored raw, the value would climb
/// until its precision went into the integer part and the phase stopped moving smoothly. Reducing on
/// the way to storage keeps the fraction, which is the only part anybody hears.
inline void wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored) {
	for (size_t i = 0; i < kPhaseCount; ++i) {
		// NON-FINITE BECOMES ZERO rather than propagating: this crosses into a saved patch and into a
		// drawing, and a NaN in either is a thing that reappears every time the patch is loaded.
		const double v = std::isfinite(accumulator[i]) ? accumulator[i] : 0.0;
		stored[i] = static_cast<float>(v - std::floor(v));
	}
}

/// **THE SCALAR THE PLANE IS ANCHORED AT — the mean of the phases.** `SliceBasis` is tuned around one
/// number standing for "where the field is", and the mean is the honest one for a set whose entries
/// are all φ-scaled versions of a shared position. Automod's `planeHomeOf` does the same for its
/// thirty-nine.
[[nodiscard]] inline double planeHomeOf(const Phases& home) {
	double sum = 0.0;
	// **THE CURVE'S EIGHT ONLY.** `sliceCoefficient` depends on the index and not on N, so the first
	// eight directions of an 18-wide basis are the 8-wide basis's bit for bit — PROVIDED the anchor is
	// the same scalar. Averaged over the curve's eight it is, so the pad already known moves the
	// curve exactly as it did, and the extras move with it.
	for (size_t i = 0; i < kCurvePhaseCount; ++i) {
		const double v = home[i];
		sum += v;
	}
	return sum / static_cast<double>(kCurvePhaseCount);
}
} // namespace feathers::fx::shaper::phase_space
