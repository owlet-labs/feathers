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

#include "character/phi_lfo_shape.hpp"
#include "fx/automod/automod_phase_space.h"
#include "fx/automod/automod_comb.h"
#include "fx/automod/automod_filter.h"
#include "fx/automod/automod_spring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

// WHAT THREE COORDINATES MEAN — the whole of the automodulator's character layer, as one function.
//
// The device has three zone controls (Type, Flavour, Mod), each with its own phase offset, plus a
// device-wide gamma. Everything else it does — where the filter sits, how the comb is tuned, how
// bouncy the springs are, how the envelope responds — is READ OUT OF THE PHI FIELD at those
// positions. This file is that reading.
//
// ---------------------------------------------------------------------------------------------
// A PURE FUNCTION RETURNING A VALUE, WHERE THE FIRMWARE MUTATES A CACHE IN PLACE.
// ---------------------------------------------------------------------------------------------
//
// `updateAutomodPhiCache` writes into an `AutomodPhiCache` and keeps a `prevType`,
// `prevFlavorPhaseOffset`, `prevGammaPhase` beside it to know when to run again. Those prev-fields
// are a staleness mechanism, and staleness is the thing this project has paid for twice this month —
// the voice that kept playing a vowel baked two rebuilds earlier, and a card fold keyed on a
// reusable slot. A VALUE RETURNED WHOLE CANNOT BE STALE. There is nothing to invalidate because
// there is nothing held.
//
// AND THE REASON THAT IS WORTH A STRUCT COPY: every one of these numbers becomes assertable WITHOUT
// INSTANTIATING AN EFFECT. "Assert the value, not the rendered buffer" stops being a discipline and
// becomes the path of least resistance, which is the only way a rule like that survives contact.
//
// IT RUNS ON THE AUDIO THREAD, gated on the coordinates having moved, so the struct is a fixed-size
// trivially-copyable aggregate with no heap anywhere in it. The static_assert below is the guard,
// not the comment.
//
// ---------------------------------------------------------------------------------------------
// WHAT IS NOT HERE
// ---------------------------------------------------------------------------------------------
//
// NO TREMOLO. `kTremoloDepthTriangle` has duty 0.0 and is therefore identically zero everywhere —
// measured over 100,000 positions across four flavour sweeps against 0.999992 for a live triangle —
// with `// Disabled: band mixing provides AM` written beside it in the firmware. Porting it would
// be declaring a control that cannot change the sound. See the reasoning recorded in the envelope
// follower's history.
//
// NO q31, NO PRE-BAKED INTEGER FIELDS. The firmware's cache carries `phaseU32`, `segAmpQ`,
// `invSegWidthQ` and a dozen more, all of them ARM CPU work. What survives is what the sound needs.

namespace feathers::fx::automod {

namespace phi = owlet::dsp::phi;

//------------------------------------------------------------------------------------------------//
// The banks — ported verbatim from `automodulator.hpp`, which is where the tuning lives.           //
//------------------------------------------------------------------------------------------------//

/// FROM TYPE: [0] comb feedback (bipolar), [1] comb mix, [2] SVF self-feedback (bipolar).
///
/// **COMB FEEDBACK IS BIPOLAR, AND ITS SIGN IS THE FIELD'S (backlog IS.5 2026-09-23: comb feedback is
/// phi-derived rather than a negative switch, so the feedback triangle is simply bipolar).** Across
/// the triangle's duty it runs 0 → +max → 0 → −max → 0: positive feedback rings at the delay's harmonics,
/// negative at the ODD harmonics of half its frequency — the hollow, clarinet-like comb, an octave down.
/// The trade taken knowingly: positive feedback now covers half the Type span it covered when the
/// triangle was unipolar, and the other half is negative.
inline constexpr std::array<phi::PhiTriConfig, 3> kTypeScalarBank = { {
	{ phi::kPhi250, 0.8f, 0.0f, true },
	{ phi::kPhi350, 0.85f, 0.0f, false },
	{ phi::kPhi175, 0.25f, 0.1078f, true },
} };

/// FROM TYPE: the three filter-mix weights, before normalisation.
inline constexpr std::array<phi::PhiTriConfig, 3> kFilterMixBank = { {
	{ phi::kPhi050, 0.6f, 0.50f, false },
	{ phi::kPhi067, 0.5f, 0.00f, false },
	{ phi::kPhi100, 0.4f, 0.00f, false },
} };

/// FROM FLAVOUR: [0] cutoff base, [1] resonance, [2] filter mod depth, [3] env attack,
/// [4] env release, [5] comb static offset, [6] comb LFO depth, [7] comb phase offset,
/// [8] comb mono collapse. The firmware's [9] and [10] are the dead tremolo pair and are not here.
inline constexpr std::array<phi::PhiTriConfig, 9> kFlavorScalarBank = { {
	{ phi::kPhi150, 0.75f, 0.0f, false },
	{ phi::kPhi175, 0.8f, 0.4f, false },
	{ phi::kPhi200, 0.5f, 0.50f, false },
	{ phi::kPhi125, 0.7f, 0.15f, false },
	{ phi::kPhi175, 0.7f, 0.65f, false },
	{ phi::kPhi125, 0.85f, 0.25f, false },
	{ phi::kPhi175, 0.75f, 0.00f, false },
	{ phi::kPhi175, 0.65f, 0.35f, false },
	{ phi::kPhi225, 0.5f, 0.15f, false },
} };

/// FROM FLAVOUR: how strongly each band's weight responds to the LFO, and at what phase.
inline constexpr std::array<phi::PhiTriConfig, 3> kFilterLfoResponseBank = { {
	{ phi::kPhi125, 0.5f, 0.00f, false },
	{ phi::kPhi175, 0.5f, 0.40f, false },
	{ phi::kPhi225, 0.5f, 0.70f, false },
} };
inline constexpr std::array<phi::PhiTriConfig, 3> kFilterPhaseOffsetBank = { {
	{ phi::kPhi100, 0.5f, 0.00f, false },
	{ phi::kPhi150, 0.6f, 0.33f, false },
	{ phi::kPhi200, 0.7f, 0.66f, false },
} };

/// FROM MOD: [0] stereo offset, [1..4] envelope influences, [5] envelope-to-value, [6] spring
/// frequency, [7] spring damping.
inline constexpr std::array<phi::PhiTriConfig, 8> kModScalarBank = { {
	{ phi::kPhi150, 0.7f, 0.0f, false },
	{ phi::kPhi150, 0.5f, 0.121f, true },
	{ phi::kPhi175, 0.5f, 0.109f, true },
	{ phi::kPhi125, 0.5f, 0.135f, true },
	{ phi::kPhi150, 0.5f, 0.121f, true },
	{ phi::kPhi125, 0.5f, 0.135f, true },
	{ phi::kPhi175, 0.5f, 0.25f, false },
	{ phi::kPhi150, 0.4f, 0.4f, false },
} };

/// FROM MOD: where a retriggered LFO starts in its own waveform.
inline constexpr phi::PhiTriConfig kLfoInitialPhaseTriangle { phi::kPhi250, 1.0f, 0.0f, false };

//------------------------------------------------------------------------------------------------//

/// WHERE THE THREE ZONE CONTROLS ARE, and the phases they are read at.
///
/// EACH COORDINATE CARRIES ITS OWN OFFSET **AND** THE DEVICE GAMMA, which is the firmware's
/// arrangement and the decision of 2026-08-29 to keep the device gamma. The two are different
/// gestures: an offset moves ONE control's character, the gamma moves all three together.
struct AutomodCoords {
	float type = 0.0f;   ///< 0..1
	float flavor = 0.0f; ///< 0..1
	float mod = 0.0f;    ///< 0..1

	float gammaPhase = 0.0f;

	/// THE FOLDED TRAVEL, carried through so the derive can add it. See `Settings::foldedTravel`.
	std::array<float, kFoldedTravelFloats> foldedTravel {};

	/// THE LFO'S RATE, IN Hz, AND IT IS AN INPUT RATHER THAN A DERIVED VALUE. The spring frequencies
	/// scale with it — the firmware asks for a fixed number of BOUNCES PER LFO CYCLE, so a faster LFO
	/// needs a faster spring to keep the same character — so the derive cannot compute them without
	/// knowing the rate. The rate itself comes from a knob, not from the field.
	float lfoHz = 1.0f;

	friend bool operator==(const AutomodCoords&, const AutomodCoords&) = default;
};

/// EVERYTHING THE THREE COORDINATES DECIDE. Plain floats, no pointers, no containers.
struct AutomodDerived {
	//-- the filter ------------------------------------------------------------------------------//
	/// Where the corner sits before modulation, 0..1 across the filter's range.
	float filterCutoffBase = 0.0f;
	float filterResonance = 0.0f;
	/// How far the spring-smoothed LFO moves the corner, 0..1.
	float filterModDepth = 0.0f;
	/// The three band weights, already constant-power normalised.
	FilterMix filterMix {};
	/// BIPOLAR. Positive feeds the lowpass output back into the corner (a screaming filter);
	/// negative pushes the LFO's phase instead (chaotic rather than resonant).
	float svfFeedback = 0.0f;

	/// How strongly each band's weight follows the LFO, and at what phase offset.
	float bandResponse[3] {};
	float bandPhaseOffset[3] {};
	/// TRUE WHEN NONE OF THE THREE RESPONDS, so the mix can be set once instead of per buffer. A
	/// performance shortcut whose CONDITION is part of the derive, not a runtime guess.
	bool staticFilterMix = true;

	//-- the comb --------------------------------------------------------------------------------//
	CombSettings comb {};
	/// Where the comb's taps sit relative to the filter's, in turns.
	float combPhaseOffset = 0.0f;

	//-- the envelope follower -------------------------------------------------------------------//
	float envAttackSeconds = 0.0f;
	float envReleaseSeconds = 0.0f;

	//-- what the envelope does to the LFO -------------------------------------------------------//
	/// All bipolar: the field decides the SIGN as well as the amount, so a coordinate can make a
	/// loud note open the filter further or close it.
	///
	/// THREE ARE READ, TWO ARE NOT, AND THAT MATCHES THE REFERENCE (parity port, 2026-09-16 —
	/// `envelopeSteerFor` in automod.h). Depth, derivative→depth and value steer the filter's spring
	/// target. The two PHASE influences are evaluated and applied to nothing, exactly as in the
	/// firmware, where the phase push sits behind `AUTOMOD_ENABLE_PHASE_PUSH`, default 0. They stay
	/// in the bank so the mod coordinate's eight outputs keep their indices; wiring them is a design
	/// call rather than a port's.
	float envDepthInfluence = 0.0f;
	float envPhaseInfluence = 0.0f; ///< unread — the firmware's phase push is compiled out
	float envDerivDepthInfluence = 0.0f;
	float envDerivPhaseInfluence = 0.0f; ///< unread — as above
	float envValueInfluence = 0.0f;

	//-- the springs -----------------------------------------------------------------------------//
	float springHz = 5.0f;
	float springZeta = kCriticalDamping;
	/// Critically damped by construction — overshoot on a delay time is a pitch artefact, not a
	/// bounce, which is why this one has no zeta of its own.
	float combSpringHz = 5.0f;

	//-- the LFO ---------------------------------------------------------------------------------//
	/// How far the right channel trails the left, in turns.
	float stereoPhaseOffset = 0.0f;
	/// Where a retriggered LFO starts in its waveform, 0..1.
	float lfoStartPhase = 0.0f;
	/// The waveform itself.
	character::LfoShape shape {};

	friend bool operator==(const AutomodDerived&, const AutomodDerived&) = default;
};

/// THE GUARD, NOT THE COMMENT. This is copied on the audio thread whenever a coordinate moves; a
/// member that allocated, or that needed a copy constructor, would make that a heap operation in the
/// one place this project forbids one.
static_assert(std::is_trivially_copyable_v<AutomodDerived>,
              "AutomodDerived is copied on the audio thread; it must not own anything");

//------------------------------------------------------------------------------------------------//
// The mapping constants — each one a range somebody has to be able to find and change.            //
//------------------------------------------------------------------------------------------------//

/// THE ENVELOPE'S TIME RANGES, AND THESE TWO NUMBERS ARE THE ONLY THING IN THIS FILE CHOSEN BY EAR
/// RATHER THAN PORTED — flagged here because that makes them the first thing to retune.
///
/// The firmware stores a per-BUFFER one-pole coefficient (`flavorScalar * flavorScalar`), which is
/// not a duration at all: its meaning moves with the host's block size, which is the defect the
/// envelope follower's header records at length. Our follower takes SECONDS, so the field's 0..1 has
/// to name a time, and no faithful conversion exists to copy — the quantity being converted was
/// never a time.
///
/// So: a musically sensible span on a log taper, which is this project's convention for every time
/// control it has. Fast attacks and slower releases, and the field's value runs FAST-to-SLOW inverted
/// so a higher scalar still means a quicker response, as it did in the firmware.
inline constexpr float kEnvAttackFastSeconds = 0.0005f;
inline constexpr float kEnvAttackSlowSeconds = 0.200f;
inline constexpr float kEnvReleaseFastSeconds = 0.005f;
inline constexpr float kEnvReleaseSlowSeconds = 2.000f;

/// THE COMB'S MODULATION RANGE, `kCombMaxModRangeSamples`, lives in `automod_comb.h` with the rest of its reference-rate
/// numbers, since the longest delay is now derived from it (backlog IS.5).

/// The spring's bounce range, in bounces per LFO cycle, and where its frequency is clamped.
inline constexpr float kSpringBouncesMin = 0.5f;
inline constexpr float kSpringBouncesMax = 12.0f;
inline constexpr float kSpringHzFloor = 0.5f;
inline constexpr float kSpringHzCeiling = 80.0f;
/// The comb's spring tracks four times the LFO rate — responsive enough not to lag a sweep, slow
/// enough to smooth the LFO's corners.
inline constexpr float kCombSpringMultiplier = 4.0f;

/// A LOWPASS FLOOR ADDED BEFORE NORMALISATION, so the three weights can never all sit in their dead
/// zones at once and mute the device. The firmware's, and it is the difference between a coordinate
/// that sounds thin and one that sounds broken.
inline constexpr float kFilterMixLowFloor = 0.1f;

//------------------------------------------------------------------------------------------------//

/// A time from a 0..1 field value, on a log taper. `value` 1 gives `fast`, 0 gives `slow`.
inline float envTimeFor(float value, float fastSeconds, float slowSeconds) {
	const float v = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
	// Geometric interpolation: v = 0 gives `slow`, v = 1 gives `fast`, and every equal step of v is
	// an equal RATIO — the same taper `mod/modulator.h` argues for on every time control here.
	return slowSeconds * std::exp2(v * std::log2(fastSeconds / slowSeconds));
}

/// **THE PERIOD OF EACH TRIANGLE, IN PHASE UNITS — what a stored residue must be wrapped to.**
///
/// `evalTriangle` computes `wrapPhase((phase + offset) * phiFreq * freqMult)`, so triangle `i` repeats
/// every `1 / phiFreq_i` of phase. **The phi frequencies are irrational relative to each other and to
/// 1**, which is the whole point of the bank — no two coordinates come back into alignment. Two
/// consequences that a first draft of this got wrong:
///
///   - **A whole number of "laps" is not a period.** Adding 1.0, or 1e7, returns NO triangle to where
///     it was. A test asserting "ten million laps sound like zero laps" is asserting something false
///     about the field, and mine did until it failed.
///   - **So a residue cannot be wrapped at 1.0.** Wrapping the stored value at anything but this
///     triangle's own period changes the sound.
[[nodiscard]] inline const std::array<double, phase_space::kTriangles>& trianglePeriods() {
	static const std::array<double, phase_space::kTriangles> kPeriods = [] {
		std::array<double, phase_space::kTriangles> out {};
		const auto fill = [&out](size_t first, auto& bank) {
			for (size_t i = 0; i < bank.size(); ++i) {
				const double freq = static_cast<double>(bank[i].phiFreq);
				out[first + i] = freq != 0.0 ? 1.0 / std::fabs(freq) : 0.0;
			}
		};
		fill(phase_space::kTypeScalars, kTypeScalarBank);
		fill(phase_space::kFilterMix, kFilterMixBank);
		fill(phase_space::kFlavourScalars, kFlavorScalarBank);
		fill(phase_space::kFilterLfoResponse, kFilterLfoResponseBank);
		fill(phase_space::kFilterPhaseOffset, kFilterPhaseOffsetBank);
		fill(phase_space::kModScalars, kModScalarBank);
		out[phase_space::kLfoInitialPhase] =
		    kLfoInitialPhaseTriangle.phiFreq != 0.0f
		        ? 1.0 / std::fabs(static_cast<double>(kLfoInitialPhaseTriangle.phiFreq))
		        : 0.0;
		fill(phase_space::kLfoShape, character::kLfoShapeBank);
		return out;
	}();
	return kPeriods;
}

/// **WRAP A RESIDUE FOR STORAGE, PER TRIANGLE, SO THE FLOAT NEVER CARRIES A LARGE INTEGER PART.**
///
/// BY DESIGN, unbounded accumulation is fine as long as the surfaces are engineered not to
/// saturate double. The accumulator is double and the sum in `deriveAutomod` is double, so the
/// arithmetic is safe at any size — but the STORED form is a float in the device's row, and a float
/// at 1e7 has a spacing of 1.0. **Half a lap of travel is simply gone at that magnitude**, which is
/// the 1e-9 the LFO shape bank lost to a float-summed phase, scaled by however far a hand has come.
///
/// So the writer wraps before storing. Each coordinate to ITS OWN period, which keeps every triangle
/// at exactly the value it had while keeping the number small.
///
/// **IT TAKES THE DOUBLE ACCUMULATOR, NOT THE STORED FLOATS, AND THE FIRST VERSION TOOK THE FLOATS.**
/// That version could not work and the case proved it in one run: by the time a value is a float at
/// 1e6 its fraction is already gone, so wrapping it afterwards wraps a number that has nothing left to
/// keep. "Before any float conversion" is not a stylistic preference — it is the only order in which
/// the operation means anything.
///
/// **WHAT THIS COSTS, SAID OUT LOUD: two different journeys become indistinguishable.** A residue of
/// `p` and one of `2p` on the same triangle are the same sound and store as the same number, so "how
/// far have I come" is not recoverable. That is inherent to a periodic field rather than a choice
/// here — the field genuinely does not distinguish them — and it is why the residue is not a position
/// readout and there is no "return home" verb.
inline void wrapForStorage(const std::array<double, kFoldedTravelFloats>& accumulator,
                           std::array<float, kFoldedTravelFloats>& stored) {
	const auto& periods = trianglePeriods();
	for (size_t i = 0; i < kFoldedTravelFloats; ++i) {
		const double v = std::isfinite(accumulator[i]) ? accumulator[i] : 0.0;
		const double period = periods[i];
		stored[i] = static_cast<float>(period > 0.0 ? v - period * std::floor(v / period) : v);
	}
}

/// **THE READING, AT ONE PHASE PER PHI TRIANGLE.** Pure: same phases, same answer, every time.
///
/// BY DESIGN, Automod has the N required to drive all of its phi triangles, and the
/// whole topological complex — home, an integrated trajectory, a basis slicing the space, and rotate
/// turning the plane through it. This is the derive's half of that: it no longer knows about three
/// coordinates, only about thirty-nine phases.
///
/// **THE THREE-COORDINATE ENTRY POINT BELOW IS THIS FUNCTION AT A RANK-1 HOME**, and every existing
/// caller still uses it. we can just have 1d gamma and phase offset and phase
/// push in the n-dim unit vector — which is exactly what `phase_space::homeFor` builds: gamma along
/// all thirty-nine, each coordinate along its own group's unit vector.
inline AutomodDerived deriveAutomodAt(const AutomodCoords& in, const phase_space::Phases& ph) {
	namespace ps = phase_space;
	const auto finite = [](float v) { return std::isfinite(v) ? v : 0.0f; };

	AutomodDerived out {};

	//-- from TYPE -------------------------------------------------------------------------------//
	const auto typeScalars = ps::evalAt<3>(ph, ps::kTypeScalars, kTypeScalarBank);
	out.comb.feedback = typeScalars[0] * 0.85f;
	out.comb.mix = typeScalars[1];
	out.svfFeedback = typeScalars[2];

	// THE FILTER MIX IS CONSTANT-POWER NORMALISED, not sum-to-one, and that is the firmware's and it
	// is right: the three bands are summed, so equal PERCEIVED loudness across the blend needs the
	// squares to sum to one rather than the weights.
	{
		const auto raw = ps::evalAt<3>(ph, ps::kFilterMix, kFilterMixBank);
		const float low = raw[0] + kFilterMixLowFloor;
		const float sumSquares = low * low + raw[1] * raw[1] + raw[2] * raw[2];
		const float invRms = 1.0f / std::sqrt(std::max(sumSquares, 1e-12f));
		out.filterMix = { low * invRms, raw[1] * invRms, raw[2] * invRms };
	}

	//-- from FLAVOUR ----------------------------------------------------------------------------//
	const auto flavorScalars = ps::evalAt<9>(ph, ps::kFlavourScalars, kFlavorScalarBank);
	out.filterCutoffBase = flavorScalars[0];
	out.filterResonance = flavorScalars[1] * 0.85f;
	out.filterModDepth = flavorScalars[2];
	out.envAttackSeconds = envTimeFor(flavorScalars[3], kEnvAttackFastSeconds, kEnvAttackSlowSeconds);
	out.envReleaseSeconds = envTimeFor(flavorScalars[4], kEnvReleaseFastSeconds, kEnvReleaseSlowSeconds);

	// THE COMB'S BASE DELAY SPANS WHAT IS LEFT AFTER THE MODULATION RANGE IS RESERVED AT THE TOP —
	// AND ONLY AT THE TOP, WHICH IS ASYMMETRIC AND IS THE FIRMWARE'S.
	//
	// AN EARLIER DRAFT OF THIS COMMENT CLAIMED THE CLAMP IN `Comb::delayFor` WAS THEREFORE ONLY A
	// GUARD. It is not, and the case that sweeps the cube caught it: with the base as low as
	// `kCombMinDelaySamples` and the modulation range as wide as 400 samples, `base - range` reaches
	// **-194** at the worst corner. The clamp is LOAD-BEARING at the bottom of the sweep.
	//
	// SO THE BOTTOM OF A WIDE SWEEP FLATTENS rather than continuing, which is an audible behaviour
	// and not merely a safety net: the comb's pitch stops falling and sits at the floor for part of
	// the LFO cycle. The firmware does exactly this — `kMinDelay` is 4 there against a 400-sample
	// range — so it is ported rather than introduced. Reserving the range at the bottom too would
	// remove the flattening and move every short comb delay upward, which is a different instrument.
	{
		const float span = kCombMaxDelaySamples - kCombMinDelaySamples - kCombMaxModRangeSamples;
		out.comb.baseDelaySamples = kCombMinDelaySamples + flavorScalars[5] * span;
		out.comb.modRangeSamples = flavorScalars[6] * kCombMaxModRangeSamples;
	}
	out.combPhaseOffset = flavorScalars[7];
	out.comb.monoCollapse = flavorScalars[8];

	{
		const auto response = ps::evalAt<3>(ph, ps::kFilterLfoResponse, kFilterLfoResponseBank);
		const auto offsets = ps::evalAt<3>(ph, ps::kFilterPhaseOffset, kFilterPhaseOffsetBank);
		constexpr float kResponseThreshold = 0.01f;
		out.staticFilterMix = true;
		for (int i = 0; i < 3; ++i) {
			out.bandResponse[i] = response[i];
			out.bandPhaseOffset[i] = offsets[i];
			if (response[i] >= kResponseThreshold) {
				out.staticFilterMix = false;
			}
		}
	}

	//-- from MOD --------------------------------------------------------------------------------//
	const auto modScalars = ps::evalAt<8>(ph, ps::kModScalars, kModScalarBank);
	out.stereoPhaseOffset = modScalars[0];
	out.envDepthInfluence = modScalars[1];
	out.envPhaseInfluence = modScalars[2];
	out.envDerivDepthInfluence = modScalars[3];
	out.envDerivPhaseInfluence = modScalars[4];
	out.envValueInfluence = modScalars[5];

	// THE SPRINGS SCALE WITH THE LFO RATE, so the character is a number of BOUNCES PER CYCLE rather
	// than a fixed frequency — a fast LFO with a slow spring would simply be smoothed away.
	{
		const float lfoHz = std::max(finite(in.lfoHz), 0.01f);
		const float bounces = kSpringBouncesMin + modScalars[6] * (kSpringBouncesMax - kSpringBouncesMin);
		out.springHz = std::clamp(bounces * lfoHz, kSpringHzFloor, kSpringHzCeiling);

		// TWO COMPENSATIONS, BOTH THE FIRMWARE'S, AND BOTH ARE ABOUT KEEPING THE BOUNCE VISIBLE
		// WITHOUT LETTING IT RING. A slow LFO needs more bounciness before an overshoot reads as one;
		// a low-frequency spring needs less before it stops settling. Neither is a stability guard —
		// the zeta floor below is that.
		const float bounciness = modScalars[7] * 0.9f;
		constexpr float kCalibrationRate = 20.0f;
		const float rateRatio = lfoHz / kCalibrationRate;
		const float rateCompensation = std::clamp(std::sqrt(rateRatio * std::sqrt(rateRatio)), 0.02f, 2.0f);
		const float freqNorm = std::clamp((out.springHz - kSpringHzFloor) / (kSpringHzCeiling - kSpringHzFloor),
		                                  0.0f, 1.0f);
		const float freqCompensation = 0.5f + (1.0f - freqNorm) * 3.5f;
		const float compensated = bounciness / (rateCompensation * freqCompensation);
		out.springZeta = std::max(1.0f - compensated, 0.15f);

		out.combSpringHz = std::clamp(lfoHz * kCombSpringMultiplier, kSpringHzFloor, kSpringHzCeiling);
	}

	//-- the waveform ----------------------------------------------------------------------------//
	// The shape reads the mod coordinate WITHOUT its own offset added twice: `lfoShapeAt` takes the
	// coordinate and the offset separately and sums them itself, so the gamma rides in the offset.
	{
		// THE NINE INSIDE `lfoShapeAt` — the ones a count of this file alone never sees.
		std::array<double, 9> shapePhases {};
		for (size_t i = 0; i < 9; ++i) {
			shapePhases[i] = ph[ps::kLfoShape + i];
		}
		out.shape = character::lfoShapeAtPhases(shapePhases);
	}
	out.lfoStartPhase = phi::evalTriangle(ph[ps::kLfoInitialPhase], 1.0f, kLfoInitialPhaseTriangle);

	return out;
}

/// THE THREE-COORDINATE READING — every existing caller's entry point, unchanged in behaviour.
///
/// **AT A RANK-1 HOME THIS IS BIT-IDENTICAL TO WHAT IT ALWAYS WAS**, because `evalAt` handing every
/// triangle in a bank the same phase reproduces `evalTriangleBank` exactly: that function's scalar
/// path IS this loop, and NEON is never enabled in this repo. So the 39-space can exist and the sound
/// cannot move until something builds a home that is not rank-1.
inline AutomodDerived deriveAutomod(const AutomodCoords& in) {
	const auto finite = [](float v) { return std::isfinite(v) ? v : 0.0f; };
	// EACH COORDINATE PLUS ITS OWN OFFSET PLUS THE DEVICE GAMMA — the arrangement kept, and now
	// also the rank-1 home: gamma pushes along all thirty-nine, each coordinate along its group's
	// unit vector. we can just have 1d gamma and phase offset and phase push in the n-dim
	// unit vector.
	namespace ps = phase_space;
	// **GAMMA PUSHED ALONG ITS OWN AXIS, WHICH IS THE WHOLE SPACE.** It used to be folded into each of
	// the three sums, which is the same arithmetic and hid the rank: gamma is "getting in an
	// airplane", rank 39, where a coordinate's phase is rank 6, 15 or 18.
	//
	// **BIT-IDENTICAL, AND BY ASSOCIATION RATHER THAN BY LUCK.** The old expression parsed as
	// `((coord + phase) + gamma)`; pushing gamma afterwards computes `((coord + phase) + gamma)` too.
	// Same order, same rounding. Had the push come first this would have been a different sum and a
	// different sound — which is why it is spelled out rather than assumed.
	//
	// **AND THE COORDINATE IS NOW ONE NUMBER RATHER THAN A SUM OF TWO — backlog BJ, piece 3.** This
	// read `double(in.type) + in.typePhase`, two row floats added in DOUBLE; the collapsed param is
	// a single row float carrying what that sum carried.
	//
	// **THE PRECISION CLAIM, STATED EXACTLY, BECAUSE IT IS TEMPTING TO OVERCLAIM IT.** At the default
	// the two are bit-identical and trivially so: the phase halves defaulted to 0, and `0.30f + 0.0`
	// is `0.30f`. Away from it they are NOT identical in general — summing two floats in double is
	// exact, while the collapsed control rounds the sum to float once when it is stored. What that
	// costs is one rounding at the row, on a value the row was always going to hold as a float
	// anyway; what it cannot do is change any state that exists, because collapsing the params broke
	// the patch format and there are no saved patches for the difference to appear in.
	ps::Phases home = ps::homeFor(static_cast<double>(finite(in.type)),
	                              static_cast<double>(finite(in.flavor)),
	                              static_cast<double>(finite(in.mod)));
	ps::pushAlong(home, ps::kGammaAxis, static_cast<double>(finite(in.gammaPhase)));

	// **THE FOLDED TRAVEL, ADDED PER COORDINATE, IN DOUBLE.** Unbounded by design, so the engineering
	// obligation is here: `home` is `std::array<double>` and this addition is double, so a residue of
	// any size costs no precision in the sum. **What must never happen is a large value reaching a
	// float before it is wrapped** — that is the 1e-9 the LFO shape bank lost to a float-summed phase,
	// scaled up by however far a hand has travelled.
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		home[i] += static_cast<double>(finite(in.foldedTravel[i]));
	}
	return deriveAutomodAt(in, home);
}


} // namespace feathers::fx::automod
