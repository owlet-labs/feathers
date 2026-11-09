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
#include <array>
#include <cmath>
#include <cstddef>

// A PIECEWISE-LINEAR LFO SHAPE AUTHORED BY THE PHI FIELD — one coordinate in, a whole waveform out.
//
// Ported from the firmware's `automodulator.hpp` / `getLfoWaypointBank`, which is the automodulator's
// Mod knob: turning it does not scale a depth, it REDRAWS the LFO. Nine φ triangles produce five
// segment widths and four breakpoint levels, and the result is a shape that evolves continuously and
// deterministically as the coordinate moves.
//
// WHY THIS IS HERE AND NOT IN `src/mod/`, WHICH IS THE QUESTION THAT HAD TO BE ANSWERED FIRST.
// The SHAPE is not new — a piecewise-linear run of segments between breakpoints, looped, at a rate,
// is exactly `mod/mseg`, and the MSEG's geometry is a strict superset (eight segments to this one's
// five, and each of its segments may curve where these are all straight). `mod/mseg` also already
// owns the RATE (a musical division times a scale, or seconds with no tempo) and the WRAP (a loop
// flag with two markers, and a Free trigger), so those are not new either.
//
// WHAT IS GENUINELY NEW IS NOT A MODULATOR — IT IS AN AUTHOR. The MSEG's breakpoints are drawn by a
// hand and stored; these are COMPUTED, from a position in the φ field, through nine triangles. That
// is the character tier's job and it is the tier this file sits in, beside `phi_vox.hpp` and
// `phi_stair.hpp`, which do the same trick for other surfaces.
//
// SO THIS FILE DELIBERATELY DOES NOT OWN A RATE, A PHASE ACCUMULATOR, A LOOP FLAG, OR A TRIGGER MODE.
// Those are exactly the things `mod/modulator.h` and `mod/mseg` already answer, and a second answer
// to any of them would be the redundancy this port exists to avoid. What comes out of here is
// geometry: where the corners are and how steep the runs between them are. Something else decides
// when to read it.
//
// AND IT IS A PURE FUNCTION OF PHASE, WHICH IS THE ONE STRUCTURAL DIFFERENCE FROM THE MSEG.
// `mseg::Shape` is a state machine you advance a tick at a time; `evalLfoShape` is stateless, so one
// shape can be read at several phases in the same block for the price of the arithmetic. The
// automodulator needs exactly that — it taps one LFO at four offsets at once (left, right, comb
// left, comb right) — and a state machine cannot be tapped, only cloned.
//
// NO ALLOCATION AND NO STATE. `LfoShape` is a plain aggregate of 19 floats; `lfoShapeAt` returns one
// by value and `evalLfoShape` reads one. Safe to build at activate and read on the audio thread.
//
// ------------------------------------------------------------------------------------------------
// THIS IS A DELIBERATE DIVERGENCE FROM THE FIRMWARE, AND ONLY ONE MEASUREMENT CAN SEE IT.
// ------------------------------------------------------------------------------------------------
//
// The firmware computes this shape in float and then RENDERS it in q31, storing each run's slope as
// `segSlope * 2^31`. But a slope here is amplitude-per-unit-phase and reaches 18.9, so the store
// overflows int32 and saturates. MEASURED over 1024 coordinates x 5 segments: 4602 of 5120 segments,
// 89.9%, saturate. The q31 output therefore SNAPS to each corner's level and then ramps at a slope
// capped near 2.0 — a staircase, where the float shape is continuous:
//
//     mean |float - q31|                                   0.323   (on a signal spanning +-1)
//     max  |float - q31|                                   1.996
//     largest step between adjacent samples, dt = 1/8192 cycle:
//          float  0.002302                     q31  1.999992
//
// SO THE TWO HALVES OF THE FIRMWARE'S OWN FILE DISAGREE, AND ONLY ONE HAS A STATED INTENTION. The
// float half floors its segment widths under a comment that says why — "Higher minimum limits max
// slope to reduce aliasing" — and the integer half then drives almost every run at maximal slope,
// defeating exactly that. Porting the staircase would be porting a bug, so we take the float
// reading. (Firmware parity is a per-feature choice, and the goldens are gone.)
//
// AND HERE IS WHY IT NEEDED A MEASUREMENT RATHER THAN AN EAR. The two renderings have the SAME
// realised peak-to-peak — exactly 2.0 — because the corners are correct and only the runs between
// them are wrong. A difference check passes. A peak-to-peak check passes. "It still sounds like an
// LFO" passes. ONLY CONTINUITY SEPARATES THEM, which is CLAUDE.md's "a claim about a value cannot be
// inferred from a rendered buffer" landing on a waveform. `tests/test_phi_lfo_shape.cpp` carries the
// continuity case for that reason, and a known-bad that zeroes every slope — the firmware's own
// defect, reproduced on purpose — is what proves the case can see it.

namespace feathers::character {

/// The φ evaluator is vendored under the firmware's own namespace (`character/phi_triangle.hpp` is
/// byte-identical to it but for one include path), so the configs are named through it. `owltt.cpp`
/// spells it the same way.
namespace phi = owlet::dsp::phi;

/// HOW MANY MOVABLE BREAKPOINTS, and it is four because the firmware's is four. They sit between two
/// FIXED endpoints pinned at zero, so the shape always starts and ends at rest — which is what makes
/// it loop without a discontinuity no matter which coordinate authored it.
inline constexpr std::size_t kLfoWaypoints = 4;

/// Four movable corners plus two pinned endpoints is five runs between them.
inline constexpr std::size_t kLfoSegments = kLfoWaypoints + 1;

/// WHERE THE MOVABLE CORNERS MAY SIT, leaving room at both ends for the pinned zeros. Without this
/// the first breakpoint could land on 0 and coincide with the endpoint, which is a segment of no
/// width and a slope of infinity.
inline constexpr float kLfoPhaseMin = 0.05f;
inline constexpr float kLfoPhaseMax = 0.95f;

/// THE FLOOR UNDER A SEGMENT'S WIDTH, BEFORE NORMALISATION, and it is an ANTI-ALIASING control
/// rather than a safety clamp. The firmware's comment is explicit: "Higher minimum limits max slope
/// to reduce aliasing". A raw φ triangle gives ±1; mapping |raw| into [0.4, 1.0] means the widest
/// segment can be at most 2.5x the narrowest, so no run is ever steep enough to click.
///
/// LOAD-BEARING FOR THE SOUND, so it is a named constant and there is a case on the ratio it implies.
inline constexpr float kLfoSegmentWidthFloor = 0.4f;
inline constexpr float kLfoSegmentWidthSpan = 0.6f; // floor + span == 1.0, the full raw range

/// BELOW THIS MUCH PEAK-TO-PEAK THE SHAPE IS A FLAT LINE, on purpose. Four breakpoints that happen
/// to land on nearly the same level describe a waveform with no travel in it, and normalising that
/// to ±1 would amplify float noise into a full-scale LFO. The firmware chose 0.01 and so do we.
inline constexpr float kLfoFlatThreshold = 0.01f;

/// THE NINE TRIANGLES, verbatim from the firmware's `kLfoWaypointBank`. Five author the segment
/// widths and four author the breakpoint levels; all nine are bipolar, and the staggered phase
/// offsets are what keep them from moving in lockstep as the coordinate turns.
///
/// COPIED RATHER THAN SHARED, and that is not laziness: `libs/` does not carry the firmware and
/// `src/character/phi_triangle.hpp` is already our own copy of the evaluator (byte-identical to the
/// firmware's but for one include path, checked). These constants are the port.
inline constexpr std::array<phi::PhiTriConfig, 9> kLfoShapeBank = { {
	// The five segment widths, in order: 0→P1, P1→P2, P2→P3, P3→P4, P4→1.
	{ phi::kPhi075, 0.6f, 0.000f, true },
	{ phi::kPhi100, 0.5f, 0.111f, true },
	{ phi::kPhi125, 0.5f, 0.222f, true },
	{ phi::kPhi150, 0.6f, 0.333f, true },
	{ phi::kPhi175, 0.5f, 0.444f, true },
	// The four breakpoint levels. Lower φ exponents, so the levels evolve more slowly than the
	// widths do — the shape's rhythm changes faster than its outline.
	{ phi::kPhi050, 0.7f, 0.555f, true },
	{ phi::kPhi067, 0.6f, 0.666f, true },
	{ phi::kPhiN050, 0.6f, 0.777f, true },
	{ phi::kPhi025, 0.7f, 0.888f, true },
} };

/// ONE AUTHORED WAVEFORM: where its corners are, and the runs between them precomputed.
///
/// THE PRECOMPUTED HALF IS NOT A CACHE AND CANNOT GO STALE, because it is filled in the same pass
/// that fills `phase` and `amplitude` and there is no setter. That matters here specifically:
/// CLAUDE.md's ABA rule is about a key remembered across a rebuild, and the way to be immune is for
/// the contents and everything derived from them to move together by construction. A struct with no
/// mutating operation does that for free.
struct LfoShape {
	/// The movable corners' positions, strictly increasing, inside [kLfoPhaseMin, kLfoPhaseMax].
	float phase[kLfoWaypoints] {};
	/// Their levels, in -1..+1, normalised so the shape's peak-to-peak travel is exactly 2.
	float amplitude[kLfoWaypoints] {};

	/// Where each of the five runs begins: 0, P1, P2, P3, P4.
	float segStart[kLfoSegments] {};
	/// The level each run begins at: 0, A1, A2, A3, A4.
	float segAmp[kLfoSegments] {};
	/// Level change per unit phase across that run. The reason this is stored rather than derived at
	/// read time is that a read happens several times a block and a build happens when a knob moves.
	float segSlope[kLfoSegments] {};

	friend bool operator==(const LfoShape&, const LfoShape&) = default;
};

/// THE AUTHOR: a coordinate in the φ field, and the waveform it names.
///
/// @param coord      the zone control's position, 0..1 — `params.mod` in the firmware, which stores
///                   it as 0..1023 and divides. Float here, which is this project's port of every
///                   zone control (`device/zone_param.h` states the same conversion for the same
///                   reason).
/// @param phaseOffset the control's own phase offset, added to the coordinate before the field is
///                   read. This is the "keep turning past the last zone" half of a zone coordinate:
///                   the same eight zones shifted somewhere else in the field.
///
/// DETERMINISTIC AND PURE. Same arguments, same shape, always — which is what lets a case assert
/// values rather than compare renders.
namespace detail {

/// THE REST OF `lfoShapeAt`, SHARED BY BOTH ENTRY POINTS so the two cannot drift.
inline LfoShape lfoShapeFromRaw(const std::array<float, 9>& raw) {

	// THE WIDTHS. |raw| rather than raw, because a width cannot be negative and the triangles are
	// bipolar — folding is what makes the mapping continuous through zero instead of jumping the
	// full range when a triangle changes sign.
	float width[kLfoSegments];
	for (std::size_t i = 0; i < kLfoSegments; ++i) {
		width[i] = kLfoSegmentWidthFloor + std::abs(raw[i]) * kLfoSegmentWidthSpan;
	}

	// Running totals give the corners their order FOR FREE. Widths are all strictly positive, so the
	// cumulative sums are strictly increasing, so the corners are — there is no sort here and there
	// must not be one, because a sort would let a coordinate authored as "narrow then wide" come out
	// the other way round and the shape would jump.
	float cumulative[kLfoWaypoints];
	cumulative[0] = width[0];
	for (std::size_t i = 1; i < kLfoWaypoints; ++i) {
		cumulative[i] = cumulative[i - 1] + width[i];
	}
	const float total = cumulative[kLfoWaypoints - 1] + width[kLfoSegments - 1];

	LfoShape shape {};
	const float phaseRange = kLfoPhaseMax - kLfoPhaseMin;
	for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
		shape.phase[i] = kLfoPhaseMin + (cumulative[i] / total) * phaseRange;
		shape.amplitude[i] = raw[kLfoSegments + i];
	}

	// THE LEVELS, RENORMALISED TO A FIXED PEAK-TO-PEAK. Without this the shape's loudness would swing
	// with the coordinate — four triangles that happen to agree give a quiet LFO and four that
	// disagree give a loud one — and the depth control downstream would be fighting it.
	//
	// THE PINNED ENDPOINTS COUNT. They are at zero and they are part of the waveform, so a shape
	// whose four corners are all positive still has to reach down to 0; leaving them out would
	// normalise the corners' own spread and let the shape clip past ±1 on the way to its endpoints.
	float minAmp = 0.0f;
	float maxAmp = 0.0f;
	for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
		minAmp = std::min(minAmp, shape.amplitude[i]);
		maxAmp = std::max(maxAmp, shape.amplitude[i]);
	}
	const float ampRange = maxAmp - minAmp;
	if (ampRange > kLfoFlatThreshold) {
		const float scale = 2.0f / ampRange;
		const float centre = (maxAmp + minAmp) * 0.5f;
		for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
			shape.amplitude[i] = (shape.amplitude[i] - centre) * scale;
		}
	}
	else {
		for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
			shape.amplitude[i] = 0.0f;
		}
	}

	// THE RUNS. segStart[0] and the implicit end at 1.0 are the pinned endpoints; segAmp[0] is the
	// zero they are pinned to, and the last run's slope aims back at that same zero.
	shape.segStart[0] = 0.0f;
	shape.segAmp[0] = 0.0f;
	for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
		shape.segStart[i + 1] = shape.phase[i];
		shape.segAmp[i + 1] = shape.amplitude[i];
	}

	for (std::size_t i = 0; i < kLfoSegments; ++i) {
		const float from = shape.segStart[i];
		const float to = (i + 1 < kLfoSegments) ? shape.segStart[i + 1] : 1.0f;
		const float toLevel = (i + 1 < kLfoSegments) ? shape.segAmp[i + 1] : 0.0f;
		const float span = to - from;
		// THIS GUARD CANNOT FIRE FOR ANY FINITE COORDINATE, AND IT SAYS SO RATHER THAN BEING DELETED.
		// The narrowest a run can be is bounded below by the width floor: an interior run is
		// `phaseRange * width[i] / total` with width[i] >= 0.4 and total <= 5.0, so at least 0.072;
		// the two outer runs additionally carry kLfoPhaseMin, so at least 0.122. Both are two orders
		// of magnitude above this threshold. It stays because the bound is an argument about the
		// widths and the widths come from a bank somebody may retune — see the case that pins the
		// bound, which is what would fail if they did.
		shape.segSlope[i] = (span > 0.001f) ? (toLevel - shape.segAmp[i]) / span : 0.0f;
	}

	return shape;
}

} // namespace detail

/// **THE SHAPE FROM NINE INDEPENDENT PHASES, ONE PER TRIANGLE.**
///
/// The bank form below is this function with all nine phases equal, and it stays the entry point every
/// existing caller uses. This overload exists because automod's phase space gives each phi triangle
/// its own coordinate — nine of its thirty-nine live in here, which a count of `automod_derived.h`
/// alone never sees.
///
/// **BIT-IDENTICAL WHEN THE PHASES ARE EQUAL, BY CONSTRUCTION RATHER THAN BY TESTING.**
/// `evalTriangleBank`'s scalar path is literally `results[i] = evalTriangle(phase, freqMult,
/// configs[i])`, and `PHI_TRIANGLE_USE_NEON` defaults to 0 and is defined nowhere in this repo — so
/// per-triangle evaluation is a strict generalisation of the bank, not an alternative to it.
inline LfoShape lfoShapeAtPhases(const std::array<double, 9>& phases) {
	std::array<float, 9> raw {};
	for (std::size_t i = 0; i < 9; ++i) {
		raw[i] = phi::evalTriangle(phases[i], 1.0f, kLfoShapeBank[i]);
	}
	return detail::lfoShapeFromRaw(raw);
}

inline LfoShape lfoShapeAt(float coord, float phaseOffset) {
	const double fieldPhase = static_cast<double>(coord) + static_cast<double>(phaseOffset);
	const std::array<float, 9> raw = phi::evalTriangleBank<9>(fieldPhase, 1.0f, kLfoShapeBank);
	return detail::lfoShapeFromRaw(raw);
}

/// READ THE WAVEFORM AT A PHASE. `phase` is 0..1 across one cycle; anything outside is wrapped, so a
/// caller may hand over an offset tap without folding it first.
///
/// STATELESS ON PURPOSE — see the file header. This is the entry point that lets one shape be tapped
/// at several phases in one block, which is the whole reason the automodulator's LFO is a function
/// rather than a state machine.
inline float evalLfoShape(const LfoShape& shape, float phase) {
	// A non-finite phase has no segment, and returning the rest level is the one answer that cannot
	// make noise. `std::clamp` would NOT do this job: clamp(NaN, 0, 1) returns NaN — see the
	// playbook's 8a trap.
	//
	// MEASURED REDUNDANT, AND KEPT ANYWAY, WHICH IS A DECISION RATHER THAN AN OVERSIGHT. Deleting
	// this guard alone leaves every case green: the `!(wrapped < 1.0f)` fixup below catches every
	// non-finite input on its own, because NaN compares false against everything and both infinities
	// reach it as NaN (`inf - floor(inf)`). Deleting BOTH goes red. So this is a masked pair in the
	// playbook's 8b sense, and the measurement says this half could go.
	//
	// It stays because what would then be carrying correctness is the accident that `NaN < 1.0f` is
	// false, inside a guard written for a rounding edge on negative phases. That is a load-bearing
	// side effect nobody would find on purpose, and the next person to tighten the fixup would take
	// the NaN handling out with it and see nothing fail. One cheap explicit test beats that.
	if (!std::isfinite(phase)) {
		return 0.0f;
	}
	float wrapped = phase - std::floor(phase);
	// A TINY NEGATIVE PHASE WRAPS TO EXACTLY 1.0f, WHICH IS OUTSIDE THE CYCLE. `floor(-1e-30f)` is
	// -1, and `-1e-30f + 1` rounds to 1.0f because the addend is far below the ulp of 1 — so the
	// "wrapped" phase lands one past the last segment's end. MEASURED by enumeration: 855,638,016
	// negative floats do this, and NO positive float in [0.5, 32) does, so this is a negative-side
	// hazard specifically. The automodulator reaches it: it taps one shape at several offsets and a
	// negative offset puts a tap just below zero.
	//
	// THE FIRST DRAFT OF THIS COMMENT SAID "a value just below an integer", which is the same
	// arithmetic reasoned about from the positive side and is FALSE there — the subtraction is exact
	// for a positive phase (Sterbenz), so the guard would have looked like waste to the next reader.
	if (!(wrapped < 1.0f)) {
		wrapped = 0.0f;
	}

	std::size_t seg = kLfoSegments - 1;
	for (std::size_t i = 0; i < kLfoWaypoints; ++i) {
		if (wrapped <= shape.phase[i]) {
			seg = i;
			break;
		}
	}
	return shape.segAmp[seg] + (wrapped - shape.segStart[seg]) * shape.segSlope[seg];
}

} // namespace feathers::character
