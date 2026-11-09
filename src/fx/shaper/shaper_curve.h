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
#include "character/util.hpp"
#include "fx/shaper/shaper_phase_space.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// THE SHAPER'S CURVE — everything about the transfer function, and nothing about running it.
//
// THE DEVICE AND THE PICTURE READ THE SAME MATHS, which is the only reason this is a file of its own.
// A waveshaper's card is a plot of its transfer curve; a plot computed from a second restatement of
// the curve is a plot that agrees on the day it is written and drifts thereafter. `ShaperCard` and
// `ShaperExpanded` call `buildTable` on a 160-entry span, the engine calls it on a 1025-entry one,
// and both walk `transferAt`.
//
// IT IS A RE-DERIVATION IN FLOAT, NOT A TRANSLITERATION. The firmware's `table_shaper.h` is q31
// throughout — int16 tables, Q16 thresholds, saturating shifts, a per-instance `inputScaleShift_`
// that exists to move a subtractive voice's ~1.4M peak into a range the table can address. NONE of
// that tier survives here and none of it should: a plugin's signal is float in [-1, +1], so the
// scaling tier IS the identity, and the plugin renders in float, never through a q31 render
// layer. What is ported is the SHAPE — the six bases, their φ-power
// interference, the amplitude-dependent wet blend, the extras — with the numbers re-expressed in the
// domain the shape was always about.
//
// THREE DELIBERATE DIVERGENCES, EACH RECORDED WHERE IT IS MADE RATHER THAN IN A CHANGELOG:
//
//   1. `std::tanh` REPLACES THE FIRMWARE'S `fastTanh`. That approximation exists because a Cortex-M7
//      has no transcendental unit; this table is built on a desktop, off the per-sample path, and the
//      true function is both cheaper to reason about and the one every basis was designed against.
//   2. THE TABLE IS FLOAT, NOT int16. The firmware quantises to 15 bits and the doc records the
//      resulting choice of 1024 entries as a click-versus-quality tradeoff. Float removes the
//      quantisation half of that tradeoff entirely; 1024 is kept because it is the resolution the
//      SHAPES were tuned at, not because of the storage.
//   3. THERE IS NO `deriveParameters` BESIDE `deriveParametersWithPhase`. The firmware has both, and
//      this note used to say the second reduces exactly to the first at gammaPhase == 0. **IT DOES
//      NOT** (Shaper parity sweep, 2026-09-16, read in `table_shaper.h`): the vanilla `deriveParameters`
//      assigns nine fields — drive, the six weights, threshold, asymmetry, pre-expansion and the zone-6
//      case — and leaves ELEVEN at the struct's defaults: deadzone width and phase, hysteresis and its
//      mix influence, the three drifts, sub, feedback, rotation and slew. `regenerateTable` routes
//      there whenever gammaPhase == 0 and the harmonic weight is under 0.8, so the firmware's DEFAULT
//      position has no deadzone, no hysteresis and four inert switches. This port derives all of them
//      at every gamma — the zone names in `shaper_types.h` were measured with them live (zone 0 is
//      "Gate" because of the deadzone). DECIDED, keep ours (backlog EN) — one
//      function, no special position, because a gamma of exactly zero is not a position a
//      lap-carrying coordinate should treat as a mode.
//
// AND ONE THING DELIBERATELY NOT PORTED. `TableShaperParams` carries `driftMultIntensity`,
// `driftAddIntensity` and `driftStereoOffset`; the mapper derives all three, `TableShaperCore` stores
// them in Q16 and publishes three getters for them — and **NOTHING IN THE FIRMWARE READS ANY OF THE
// THREE.** Measured, not assumed: grepping every consumer name across the whole firmware tree returns
// hits in `table_shaper.h` alone, and `shaper_buffer.h` — the file that applies every other extra —
// does not contain the word. So they are derived, stored, exposed, and dead. Porting them would mean
// INVENTING the sag/pull behaviour their names promise, which is writing a new effect while calling
// it a port. They are named here so the next reader does not have to redo the grep.

namespace feathers::fx::shaper {

/// HOW MANY SEGMENTS THE ENGINE'S TABLE HAS. The firmware's choice, kept for the reason above: the
/// six basis shapes were tuned at this resolution, and a wavefolder's fine structure is the first
/// thing a coarser table smooths away.
inline constexpr size_t kTableSize = 1024;
inline constexpr size_t kTableEntries = kTableSize + 1;

//--------------------------------------------------------------------------------------------//
// THE EXTRAS, AS BITS — the firmware's `extrasMask`, which reached a player through push+twist  //
// on a hardware encoder. The bits are kept because the DSP is gated on them; the GESTURE is not, //
// because this device has a panel and a panel can afford five labelled switches.                //
//--------------------------------------------------------------------------------------------//

inline constexpr uint8_t kExtraSub = 1u << 0;        ///< sub-harmonic gain modulation, counted off zero crossings
inline constexpr uint8_t kExtraFeedback = 1u << 1;   ///< comb feedback at the stride rate
inline constexpr uint8_t kExtraRotate = 1u << 2;     ///< bit rotation of the sample word: aliasing
inline constexpr uint8_t kExtraLpf = 1u << 3;        ///< one-pole lowpass ahead of the table
inline constexpr uint8_t kExtraIntegrator = 1u << 4; ///< zero-crossing-reset integrator

/// EVERY QUANTITY THE COLOUR FIELD PRODUCES AT ONE POSITION.
///
/// A PLACE, NOT A PRESET. Each field below is a φ-power triangle read at (colour, gamma), so no two
/// positions in the plane produce the same set and none of them repeats. The device does not store
/// these — it stores the POSITION, and derives these from it. That is what makes the XY pad the
/// device's real control surface rather than a shortcut to two knobs.
struct CurveParams {
	//-- the table's shape --------------------------------------------------------------------//
	float drive = 0.0f;           ///< 0 = linear bypass; the Knee axis, straight through
	float inflatorWeight = 1.0f;  ///< expand quiet, compress loud — punchy
	float polyWeight = 0.0f;      ///< Taylor series of tanh — softer knee than tanh itself
	float hardKneeWeight = 0.0f;  ///< hard clip — crisp, aggressive
	float chebyWeight = 0.0f;     ///< Chebyshev T5 wavefolder — synthy
	float sineFoldWeight = 0.0f;  ///< sine folder — harmonic-rich
	float rectifierWeight = 0.0f; ///< diode rectifier — asymmetric
	float asymmetry = 0.5f;       ///< 0.5 = symmetric; anything else buys even harmonics
	float deadzoneWidth = 0.0f;   ///< how much of the curve collapses to silence
	float deadzonePhase = 0.5f;   ///< where the surviving window is centred; 0.5 = about zero
	float preExpandAmount = 0.0f; ///< Oxford-style pre-stage every basis sees

	//-- the two READ shapes the colour field does NOT produce (backlog IE; read-side since IM) ------//
	//
	// SET BY THEIR OWN KNOBS AND OUTSIDE THE φ WALK, so every position on the Colour axis keeps its
	// firmware weights exactly. They are NOT table terms: `readTableAt` applies them to the table
	// coordinate every sample (see `ReadShapes`), which is what lets them be modulation destinations.
	// Carried here because `curveOf` is where settings become a curve, and the engine and the plot
	// both read them off it.
	float chebyMix = 0.0f; ///< -1..+1: left crossfades toward T2 (the octave), right toward T3 (the twelfth)
	float stairSteps = 0.0f; ///< 0 = off; otherwise the input is quantised to this many steps per unit

	//-- the per-sample extras, all gated by the mask ------------------------------------------//
	float hysteresis = 0.0f;   ///< bipolar: a direction-dependent offset into the table
	float subIntensity = 0.0f; ///< how hard the sub-harmonic modulates gain
	int32_t subRatio = 2;      ///< zero crossings per sign flip: 2 = octave, 3 = twelfth, ...
	int32_t stride = 64;       ///< samples between zero-crossing checks; also the comb's period
	float feedback = 0.0f;     ///< comb depth, capped below unity
	int32_t rotation = 0;      ///< bits to rotate the sample word by
	float slewIntensity = 0.0f; ///< the FIELD's slew — see the two amounts below for what is heard
	/// **THE FIELD'S FOUR BANK TRIANGLES, before the firmware's mappings** — interval, stride, comb depth,
	/// rotation. Carried so `curveOf` can scale the TRIANGLE (backlog II): the knob's top is then the
	/// mapping's own top (comb 0.8, 31 bits), where scaling the mapped value would overshoot it.
	std::array<float, 4> extrasField {};
	/// WHAT LOWPASS AND INTEGRATOR EACH HEAR of the one slew field — its own scaler each (backlog II).
	/// `deriveCurve` sets both to the field, which is the old switch's On; `curveOf` scales them.
	float lowpassAmount = 0.0f;
	float integratorAmount = 0.0f;

	/// THE FIELD THE COLOUR AXIS DOES NOT PRODUCE. `threshold` is derived by the firmware's mapper
	/// and then explicitly retired inside the table build — `table_shaper.h:637` reads "Threshold (T)
	/// removed - linear zone now handled by runtime mix blend", with the code that used it left
	/// commented out beneath. It is carried here because the mapper still computes it and a reader
	/// comparing the two files would otherwise think a term had been dropped in the port. NOTHING
	/// READS IT. If a future change gives it a job, it has one already-correct derivation waiting.
	float threshold = 1.0f;
};

/// WHAT THE COLOUR FIELD SAYS AT ONE POSITION — the whole mapper, in one function.
///
/// @param knee     the Knee axis, 0..1. Straight through to `drive`; 0 is a true bypass.
/// @param colour   the Colour axis, 0..1. Sweeps every triangle below at its own φ-power rate.
/// @param gamma    the second axis of the pad. The firmware's "secret knob", where it was an
///                 unbounded integer reached by push+twist; here it is a bounded coordinate, so the
///                 plane is a plane rather than a line with a hidden dimension.
/// @param harmonics how bright the incoming signal is, 0 = sine .. 1 = square. It sets ONLY the duty
///                 cycle of the slew triangle: a sine needs the transient stage rarely and a square
///                 needs it always. The firmware read this off the oscillator type, which a bus
///                 effect has no access to, so it is a declared control here.
///
/// THE ACCELERATION IS THE POINT. `freqMult` grows with colour², so the low end of the axis is slow
/// — sweet spots are findable — and the top is fragmented. The φ-power ratios guarantee the six
/// weights never come back into alignment, so no two positions along the axis are the same set.
/// **`travel` IS THE PAD'S ACCUMULATED DISPLACEMENT, ONE TERM PER PHASE, OR NULL FOR NONE.**
///
/// The eight phases are what colour and gamma put them at (see `shaper_phase_space.h`: those two are
/// the SAME DIRECTION, 5.5 degrees apart at worst, so between them they move along one line). Travel
/// is the only thing that moves the phases INDEPENDENTLY, which is why the pad exists and why a plane
/// is the only way to get a second dimension out of this device.
///
/// **ADDED AS A TERM RATHER THAN BY RESTRUCTURING THE READ, and that is deliberate.** Each phase here
/// is built by a specific sequence of float and double operations, and a rewrite that computed the
/// same quantity a tidier way would land on different last bits — a silent sound change to every
/// existing patch. Adding `+ 0.0f` is exact, so **with no travel this function is bit-identical to
/// what it was**, which is what makes the pad a UI change rather than a DSP one. The case "no travel
/// leaves the curve bit-identical" holds it to that.
///
/// **A POINTER TO THE WHOLE ARRAY, NOT TO ITS FIRST FLOAT.** This took `const float*`, so the length
/// was a promise the type could not keep — and when the extras' ten phases were appended after the
/// curve's eight, a test still passing an eight-float array read ten floats past it (found by ASan,
/// 2026-10-03: stack-buffer-overflow at `t(ps::kDeadzoneWidthPhase)`). Spelled this way a short array
/// does not compile, so no caller can be out of date with `kPhaseCount` again.
[[nodiscard]] inline CurveParams deriveCurve(float knee, float colour, float gamma,
                                             float harmonics = 0.5f,
                                             const std::array<float, phase_space::kPhaseCount>* travel = nullptr) {
	namespace phi = owlet::dsp::phi;
	using owlet::dsp::triangleSimpleUnipolar;

	CurveParams p;
	p.drive = std::clamp(knee, 0.0f, 1.0f);

	// **COLOUR IS UNBOUNDED, AND ONLY ITS ACCELERATION SATURATES.** By design none of these axes is
	// bounded — the same decision as turning past the 8th zone to keep driving the phase,
	// applied to the axis rather than to a companion counter.
	//
	// **THIS USED TO BE ONE CLAMPED `yNorm` DOING TWO JOBS**, and separating them is the whole change:
	//
	//   - **THE POSITION** is where you are on the line, and it has no ends. It drives the decorrelated
	//     triangles — color is supposed to drive decorrelated phi triangles — and clamping it
	//     was a wall at the eighth zone, the same defect this repo has now fixed on Gamma, Rotate,
	//     both OWLTT phases and both shaper phases.
	//   - **THE ACCELERATION** is a shape, not a position, and it SATURATES. `freqMult` is
	//     `1 + colour² · 3`, so an unbounded position would make the phase grow cubically — 2^72 at the
	//     top of the declared range, well past the 10^15 where a double stops being able to reduce it
	//     mod 1. Saturating it keeps the reduction sound and makes the map linear past the first lap,
	//     which is also the only monotone continuous choice: taking the fractional part instead would
	//     drop the rate from 4x back to 1x at every lap boundary, which is a jump.
	//
	// **AND IT IS BIT-IDENTICAL EVERYWHERE COLOUR COULD ALREADY BE.** The clamp is the identity on
	// 0..1, so every value that fits in the old declared range renders exactly as it did; the change
	// adds territory rather than moving any. Verified by sweeping the curve before and after.
	constexpr float kAccelFactor = 3.0f;
	const float accel = std::clamp(colour, 0.0f, 1.0f);
	const float freqMult = 1.0f + accel * accel * kAccelFactor;
	const float yNorm = colour;

	// DOUBLE PRECISION FOR THE PHASE, and it is not caution. Gamma multiplies a φ-power before it is
	// wrapped, so the quantity being reduced mod 1 grows with gamma while the answer stays in [0,1) —
	// exactly the shape that loses every significant bit in float first. The firmware's own comment
	// puts the safe ceiling at 10^15, which is a double's.
	const double ph = static_cast<double>(gamma);
	const float ph225 = phi::wrapPhase(ph * phi::kPhi225);
	const float ph200 = phi::wrapPhase(ph * phi::kPhi200);
	const float ph175 = phi::wrapPhase(ph * phi::kPhi175);
	const float ph250 = phi::wrapPhase(ph * phi::kPhi250);
	const float ph150 = phi::wrapPhase(ph * phi::kPhi150);
	const float ph125 = phi::wrapPhase(ph * phi::kPhi125);
	const float ph275 = phi::wrapPhase(ph * phi::kPhi275);
	const float ph100 = phi::wrapPhase(ph * phi::kPhi100);

	// **THE SUM STAYS IN DOUBLE UNTIL IT IS WRAPPED, AND UNBOUNDING COLOUR IS WHAT FORCED IT.**
	//
	// This returned a `float`. Colour used to stop at 1, where the largest term is about 27 and a
	// float holds it comfortably — so the float sum below was harmless. Past the bound it is not:
	// at the top of the declared range the term is ~5e7, where a float's ULP is 4, and adding
	// gamma's wrapped phase (always < 1) to it **changes nothing at all**.
	//
	// **THE SWEEP CAUGHT EXACTLY THAT: `Shaper :: Shaper Gamma` appeared in `test_fx_devices`'s DEAD
	// list.** Not wired wrong — arithmetically swallowed. It is the hazard `automod_derived.h` states
	// in as many words, *"what must never happen is a large value reaching a float before it is
	// wrapped"*, reached here for the first time because the axis stopped having an end.
	//
	// `wrapPhase` takes a double and reduces mod 1 in double, so keeping the addend a double costs
	// one cast and makes the reduction sound at any position.
	const auto base = [yNorm, freqMult](float freq) {
		return static_cast<double>(yNorm) * static_cast<double>(freq) * static_cast<double>(freqMult);
	};

	// HALF-RECTIFIED, WHICH IS WHERE THE CHARACTER COMES FROM. At 50% duty each basis is OFF for half
	// its cycle, so most positions have two or three of the six alive rather than all six averaged
	// into mud. The fixed offsets spread their gaps across the axis.
	constexpr float kPhaseWidth = 0.5f;
	// ZERO WHERE THERE IS NO PAD TRAVEL, and exactly zero: `+ 0.0f` cannot move a float.
	const auto t = [travel](size_t i) { return travel != nullptr ? (*travel)[i] : 0.0f; };
	namespace ps = phase_space; // where each extra's travel lives — see `kCurvePhaseCount`
	p.inflatorWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi225) + ph225 + t(0)), kPhaseWidth);
	p.polyWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi200) + ph200 + 0.167f + t(1)), kPhaseWidth);
	p.hardKneeWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi175) + ph175 + 0.333f + t(2)), kPhaseWidth);
	p.chebyWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi250) + ph250 + 0.5f + t(3)), kPhaseWidth);
	p.sineFoldWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi150) + ph150 + 0.667f + t(4)), kPhaseWidth);
	p.rectifierWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi125) + ph125 + 0.833f + t(5)), kPhaseWidth);

	p.threshold = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi275) + ph275 + 0.25f + t(6)), kPhaseWidth);

	// ASYMMETRY GETS ITS OWN, SLOWER ACCELERATION. Even harmonics are the ear's coarsest reading of
	// this device, so the term that produces them moves at half the rate of the terms that produce
	// timbre — a sweep changes colour faster than it changes parity.
	// **`accel`, NOT THE POSITION** — asymmetry saturates for the same reason `freqMult` does, and it
	// is the one place the split above is easy to miss: this is a second acceleration with its own
	// factor, so unbounding colour without touching it would have left one term growing cubically
	// while every other one went linear.
	const float asymFreqMult = 1.0f + accel * accel * (kAccelFactor * 0.5f);
	const double asymBase = static_cast<double>(yNorm) * phi::kPhi100 * asymFreqMult;
	p.asymmetry = 0.3f + triangleSimpleUnipolar(phi::wrapPhase(asymBase + ph100 + t(7)), kPhaseWidth) * 0.4f;

	// THE DEADZONE, at 20% duty and a slow φ^-0.5 — a sparse effect that mostly is not there. Its
	// centre is squared so it spends most of its life near zero and only occasionally excurses.
	constexpr float kDeadzoneDuty = 0.2f;
	const float phDzWidth = phi::wrapPhase(ph * phi::kPhiN050);
	const float phDzPhase = phi::wrapPhase(ph * phi::kPhi033);
	const double dzWidthBase = static_cast<double>(yNorm) * phi::kPhiN050 * freqMult;
	p.deadzoneWidth = triangleSimpleUnipolar(phi::wrapPhase(dzWidthBase + phDzWidth + t(ps::kDeadzoneWidthPhase)), kDeadzoneDuty);

	constexpr float kDeadzonePhaseDuty = 0.30f;
	const double dzPhaseBase = static_cast<double>(yNorm) * phi::kPhi033 * freqMult;
	const float dzPhaseRaw
	    = triangleSimpleUnipolar(phi::wrapPhase(dzPhaseBase + phDzPhase + t(ps::kDeadzoneCentrePhase)), kDeadzonePhaseDuty);
	p.deadzonePhase = dzPhaseRaw * dzPhaseRaw;

	// HYSTERESIS — full triangle mapped to [-1, +1]. Positive is a punchy attack with a soft decay
	// (tube-like), negative is the swell.
	const float phHyst = phi::wrapPhase(ph * phi::kPhi067);
	const double hystBase = static_cast<double>(yNorm) * phi::kPhi067 * freqMult;
	const float hystRaw = triangleSimpleUnipolar(phi::wrapPhase(hystBase + phHyst + t(ps::kHysteresisPhase)), 1.0f);
	p.hysteresis = hystRaw * 2.0f - 1.0f;

	// THE SUB, at 30% duty — sparser still, because a sub-harmonic that is always present is not an
	// effect, it is a second oscillator.
	constexpr float kSubDuty = 0.3f;
	const float phSub = phi::wrapPhase(ph * phi::kPhi350);
	const double subBase = static_cast<double>(yNorm) * phi::kPhi350 * freqMult;
	p.subIntensity = triangleSimpleUnipolar(phi::wrapPhase(subBase + phSub + t(ps::kSubDepthPhase)), kSubDuty);

	// THE FOUR BANK ENTRIES — sub ratio, stride, feedback depth, rotation amount. Read through
	// `kExtrasBank`, which is the φ layer's own declaration of them, so the frequencies and duties
	// live beside the other triangle banks rather than being restated here.
	const phi::PhiTriContext extrasCtx {yNorm, freqMult, 1.0f, gamma};
	// THE BANK, ONE TRIANGLE AT A TIME so each takes its own travel — `evalBank` is this loop without the
	// term, and `phaseOffset + 0.0f` is `phaseOffset` to the bit, so at rest this IS the firmware's bank.
	std::array<float, phi::kExtrasBank.size()> extras {};
	for (size_t i = 0; i < extras.size(); ++i) {
		const phi::PhiTriConfig& cfg = phi::kExtrasBank[i];
		extras[i] = extrasCtx.eval(cfg.phiFreq, cfg.duty, cfg.phaseOffset + t(ps::kSubIntervalPhase + i), cfg.bipolar);
	}
	p.subRatio = phi::subRatioFromTriangle(extras[0]);
	p.stride = phi::strideFromTriangle(extras[1]);
	p.feedback = phi::feedbackFromTriangle(extras[2]);
	p.rotation = phi::rotationFromTriangle(extras[3]);
	p.extrasField = extras;

	// THE SLEW TRIANGLE'S DUTY IS THE ONE THING `harmonics` TOUCHES. A sine (0.0) gets 20% duty
	// because it has no transients to soften; a square (1.0) gets 100% because it is nothing but.
	const float slewDuty = std::min(1.0f, std::clamp(harmonics, 0.0f, 1.0f) + 0.2f);
	const float phSlew = phi::wrapPhase(ph * phi::kPhi175);
	const double slewBase = static_cast<double>(yNorm) * phi::kPhi175 * freqMult;
	p.slewIntensity = triangleSimpleUnipolar(phi::wrapPhase(slewBase + phSlew + t(ps::kSlewPhase)), slewDuty);
	p.lowpassAmount = p.slewIntensity;
	p.integratorAmount = p.slewIntensity;

	// PRE-EXPANSION IS THE ONE TERM KNEE SCALES DIRECTLY. At knee 0 there is no expansion and the
	// device is a limiter; at knee 1 the full range is available and colour chooses where in it to
	// sit. Two cycles across the axis, so it is a broad wash rather than a fine texture.
	const float phPreExp = phi::wrapPhase(ph * phi::kPhi050);
	// IN DOUBLE TO THE WRAP, like the thirteen terms above it (backlog EH, shape 2): this one summed in
	// float, so past |colour| = 2^22 the base was an integer, the wrapped phase could not move it, and
	// pre-expansion sat at exactly zero across the outer half of the declared travel — the failure the
	// `base` lambda's comment names, on the fourteenth term.
	const double preExpBase = static_cast<double>(yNorm) * 2.0;
	p.preExpandAmount = p.drive
	                    * triangleSimpleUnipolar(
	                        phi::wrapPhase(preExpBase + static_cast<double>(phPreExp) + t(ps::kPreStagePhase)), 1.0f);

	// **NO SPECIAL BAND ANY MORE (backlog IW).** An Oxford-style inflator used to be hard-wired into one eighth of the
	// gamma-zero line here — the firmware's one named place, findable only by landing on it. the dropdown is a
	// mode selector, not a phase region selector; it is Colour's Inflate MODE now, blended in by `applyInflate`, and
	// the field is continuous everywhere.
	return p;
}

/// **INFLATE, AS A MODE (backlog IW).** Blends a field curve toward the Oxford-style inflator by
/// `amount`: 0 leaves the field as it is (so switching Colour into Inflate does not jump), 1 is exactly the inflator
/// the gamma-zero band used to impose — the same weights, pre-expansion and symmetry, now reached on purpose rather
/// than by landing on one line. A straight-line blend of each term, so the table rebuilds smoothly as it moves.
inline void applyInflate(CurveParams& p, float amount) {
	const float t = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
	const auto toward = [t](float& v, float target) { v += (target - v) * t; };
	toward(p.inflatorWeight, 1.0f);
	toward(p.polyWeight, 0.5f);
	toward(p.hardKneeWeight, 0.0f);
	toward(p.chebyWeight, 0.0f);
	toward(p.sineFoldWeight, 0.0f);
	toward(p.rectifierWeight, 0.0f);
	toward(p.preExpandAmount, p.drive);
	toward(p.asymmetry, 0.5f);
}

/// **COLOUR'S DRAG UNIT: an eighth of a lap** — the firmware's Y axis in eighths. It used to be the grid its one
/// special band was aligned to; since backlog IW that band is Colour's Inflate MODE, and this is only the unit.
inline constexpr int32_t kColourZones = 8;

/// THE TRANSFER FUNCTION AT ONE INPUT — the six bases, blended, with every modifier applied.
///
/// @param x an input sample in [-1, +1]
/// @return the shaped value, BEFORE the centring and normalisation `buildTable` applies. It is not
///         bounded to [-1, +1]: the folders overshoot by construction, and that overshoot is what
///         the normalisation pass measures. A caller wanting a drawable curve wants `buildTable`.
[[nodiscard]] inline float transferAt(const CurveParams& p, float x) {
	const float mag = std::fabs(x);
	const float sign = (x >= 0.0f) ? 1.0f : -1.0f;

	// STEEPNESS, 1 TO 10. The knob the whole device is named for: this is the "knee" the axis means.
	const float k = 1.0f + p.drive * 9.0f;
	const float asymRatio = 0.5f + p.asymmetry;

	// NORMALISED PER SIDE, so asymmetry changes the SHAPE of each half rather than its level. Without
	// this the asymmetric k would simply make one side quieter, which reads as a DC offset and not as
	// even harmonics.
	const float invTanhNormPos = 1.0f / std::fmax(0.01f, std::tanh(k * asymRatio));
	const float invTanhNormNeg = 1.0f / std::fmax(0.01f, std::tanh(k * (2.0f - asymRatio)));

	const float weightSum = p.inflatorWeight + p.polyWeight + p.hardKneeWeight + p.chebyWeight
	                        + p.sineFoldWeight + p.rectifierWeight;
	// THE TWO GUARDS ARE DIFFERENT COMPARISONS, as the firmware has them (`table_shaper.h`: `>` for the
	// inverse, `>=` for "has weights") — Shaper parity sweep, 2026-09-16. Both were `>=` here, so at a
	// weight sum of exactly 0.001 the port normalised by 1000 where the firmware normalises by 1. A
	// measure-zero point, kept exact because the port's whole claim is that this function IS the
	// reference's, term for term.
	const bool hasWeights = weightSum >= 0.001f;
	const float invWeightSum = (weightSum > 0.001f) ? (1.0f / weightSum) : 1.0f;

	const float norm = mag;
	const float intensity = 1.0f + p.drive;
	const float overdriven = norm * intensity;

	// THE PRE-STAGE EVERY BASIS SEES. Gain of 1 + amount/2 at zero crossing falling to 1 at full
	// scale, so quiet passages are pushed up into the curve's interesting region and loud ones are
	// left where they were. Baked into the table, so it costs nothing per sample.
	float preExpanded = overdriven;
	if (p.preExpandAmount > 0.001f) {
		const float absOd = std::fabs(overdriven);
		preExpanded = overdriven * (1.0f + p.preExpandAmount * 0.5f * (1.0f - absOd * absOd));
	}

	const float kEff = k * ((x >= 0.0f) ? asymRatio : (2.0f - asymRatio));
	const float invTanhNorm = (x >= 0.0f) ? invTanhNormPos : invTanhNormNeg;

	// BASIS 1 — INFLATOR. Its own expansion on top of the pre-stage, then tanh: 1.5x gain at the zero
	// crossing falling to unity at full scale, so quiet grows and loud compresses.
	const float absPreExp = std::fabs(preExpanded);
	const float inflatorExpand = 1.0f + 0.5f * (1.0f - absPreExp * absPreExp);
	const float inflatorOut = std::tanh(preExpanded * inflatorExpand * kEff) * invTanhNorm;

	// BASIS 2 — POLYNOMIAL. x - x³/3 + x⁵/5 is tanh's Taylor series, and truncating it is what gives
	// the softer knee: the series turns over before tanh does.
	const float pe2 = preExpanded * preExpanded;
	const float pe3 = pe2 * preExpanded;
	const float pe5 = pe3 * pe2;
	const float polyRaw = preExpanded - pe3 / 3.0f + pe5 / 5.0f;
	const float polyOut = std::tanh(polyRaw * kEff) * invTanhNorm;

	// BASIS 3 — HARD CLIP.
	const float hardClipOut = std::clamp(preExpanded, -1.0f, 1.0f);

	// BASIS 4 — CHEBYSHEV T5, WRAPPED. T5 alone leaves the range; folding its output through a
	// period-4 triangle is what turns overshoot into extra harmonics rather than into clipping.
	const float chebyIn = preExpanded * 1.2f;
	const float chebyIn2 = chebyIn * chebyIn;
	const float chebyIn3 = chebyIn2 * chebyIn;
	const float chebyIn5 = chebyIn3 * chebyIn2;
	const float chebyRaw = 16.0f * chebyIn5 - 20.0f * chebyIn3 + 5.0f * chebyIn;
	const float chebyPhase = (chebyRaw + 1.0f) - 4.0f * std::floor((chebyRaw + 1.0f) * 0.25f);
	const float chebyFold = (chebyPhase <= 2.0f) ? (chebyPhase - 1.0f) : (3.0f - chebyPhase);
	const float chebyOut = std::fabs(chebyFold);

	// BASIS 5 — SINE FOLDER. The folding rate rises with drive, so the same position gets denser as
	// the knee opens.
	constexpr float kSineFoldA = 0.4f;
	const float sineFoldB = 3.14159265f * (1.0f + p.drive);
	const float sineFoldRaw = std::tanh(preExpanded / kSineFoldA) * std::sin(sineFoldB * preExpanded)
	                          + std::tanh(preExpanded) * 0.3f;
	const float sineFoldOut = std::fmin(std::fabs(sineFoldRaw), 1.0f);

	// BASIS 6 — RECTIFIER. The bias is what makes it a diode rather than an absolute value: below the
	// bias the curve is flat, which is the forward-voltage knee.
	const float bias = 0.2f * p.drive;
	const float rectOut = std::tanh((std::fabs(preExpanded + bias) - bias) * 2.0f);

	// EVERY BASIS ABOVE RETURNS A MAGNITUDE and the sign is reapplied once, at the end. That is why
	// four of the six take a `fabs`: the curve is built for |x| and mirrored, so asymmetry has exactly
	// one place it can enter — `kEff` — instead of six.
	float basisOut = norm;
	if (hasWeights) {
		basisOut = (inflatorOut * p.inflatorWeight + polyOut * p.polyWeight
		            + hardClipOut * p.hardKneeWeight + chebyOut * p.chebyWeight
		            + sineFoldOut * p.sineFoldWeight + rectOut * p.rectifierWeight)
		           * invWeightSum;
	}

	float value = sign * basisOut;

	// THE DEADZONE — a window of the curve survives and everything outside it collapses.
	//
	// TO AN EPSILON RATHER THAN TO ZERO, and the epsilon is load-bearing. A hard zero outside an
	// off-centre window leaves the table with a DC step at the boundary; a signed epsilon keeps the
	// two halves balanced about zero while being 84 dB down, which is silence by any ear and not by
	// the normalisation pass that follows.
	if (p.deadzoneWidth > 0.001f) {
		const float passthrough = 1.0f - 0.8f * p.deadzoneWidth;
		const float centreX = p.deadzonePhase * 2.0f - 1.0f;
		if (x < centreX - passthrough || x > centreX + passthrough) {
			constexpr float kDeadzoneEpsilon = 4.0f / 32767.0f;
			value = (value >= 0.0f) ? kDeadzoneEpsilon : -kDeadzoneEpsilon;
		}
	}

	return value;
}

/// THE EXTREMES A PARTIAL BUILD HAS SEEN SO FAR — what makes the build resumable.
///
/// IT EXISTS BECAUSE THE ENGINE BUILDS IN CHUNKS. `Shaper` fills a slice of the back table per block
/// so a colour drag never costs one audio callback the whole build; the normalisation that follows
/// needs the extremes over the WHOLE curve, so they are accumulated across the chunks rather than
/// recovered by a second walk. The full-table helper below uses the same pair, which is what stops
/// the amortised path and the drawn path from being two different builds.
struct TableExtent {
	float lo = 1e30f;
	float hi = -1e30f;
};

/// FILL ENTRIES [first, last) OF `table`, accumulating extremes into `extent`.
///
/// The x for entry i is `2i/(n-1) - 1`, so a 1025-entry table lands exactly on i/512 - 1 and a
/// 160-entry one drawn on a card samples the same curve more coarsely. Both are the same function of
/// the same params, which is the whole point of this file.
inline void fillTable(const CurveParams& p, std::span<float> table, size_t first, size_t last,
                      TableExtent& extent) {
	if (table.size() < 2) {
		return;
	}
	const float scale = 2.0f / static_cast<float>(table.size() - 1);
	const size_t end = std::min(last, table.size());
	for (size_t i = first; i < end; ++i) {
		const float value = transferAt(p, static_cast<float>(i) * scale - 1.0f);
		table[i] = value;
		extent.lo = std::fmin(extent.lo, value);
		extent.hi = std::fmax(extent.hi, value);
	}
}

/// ANCHOR A COMPLETED TABLE AT THE ORIGIN AND NORMALISE IT TO A PEAK OF ONE.
///
/// **A WAVESHAPER MUST MAP ZERO TO ZERO.** It is memoryless, so whatever it returns for a silent
/// input it returns forever — a curve with f(0) != 0 does not colour silence, it REPLACES it with a
/// constant. Anchoring makes "silence in, silence out" true OF THE CURVE, which is where it belongs.
///
/// IT IS NOT WHAT DELIVERS EXACT SILENCE AT THE OUTPUT, AND SAYING SO WOULD BE OVERCLAIMING. Measured
/// by known-bad: reverting this to midpoint centring fails the table's own case and leaves both
/// silence cases GREEN, because the DC blocker's denormal flush removes the constant either way. The
/// two are separate properties with separate guards — this one is about the curve being right, the
/// flush is about the output reaching zero — and each is red under its own mutation and no other.
///
/// THIS USED TO CENTRE ON THE MIDPOINT OF THE EXTREMES, which is a reasonable-looking thing to do and
/// is wrong here. The midpoint is not f(0) for any curve that is not odd, and four of the six bases
/// are not odd — so nearly every position in the field produced a table whose centre sat off zero.
/// MEASURED at the default settings: -7.4e-5 at the table's centre — a constant this device emitted
/// into silence forever. `test_fx_devices.cpp`'s silence sweep is what surfaced the whole area (the
/// device was still sounding 4.3 seconds after the audio stopped, its tail parked on a denormal at
/// 5.3e-43); the tail was the blocker's and the offset was this function's, and both needed fixing.
///
/// PEAK-NORMALISED RATHER THAN PEAK-TO-PEAK, because once the origin is fixed there is only one
/// degree of freedom left. The louder half reaches +-1 and the quieter half does not, which is what
/// asymmetry MEANS — a curve forced to fill the range symmetrically would have had its asymmetry
/// normalised back out.
///
/// THE DC BLOCKER STAYS, and is not made redundant by this. Anchoring removes the offset at silence;
/// asymmetric shaping still produces an amplitude-DEPENDENT offset on loud signal, which is inherent
/// to generating even harmonics and is the whole point of the asymmetry term.
inline void normaliseTable(std::span<float> table, const TableExtent& extent) {
	if (table.size() < 3) {
		return;
	}
	// EXACTLY x == 0 FOR AN ODD-SIZED TABLE, which kTableEntries is: entry n/2 of 2n+1 entries maps
	// to 2*(n/2)/(2n) - 1 = 0. A table with an even count has no entry at the origin and could only
	// be anchored by interpolation, which is why the size is odd.
	const float zero = table[table.size() / 2];
	const float lo = extent.lo - zero;
	const float hi = extent.hi - zero;
	const float peak = std::fmax(std::fabs(lo), std::fabs(hi));
	const float gain = (peak > 0.001f) ? (1.0f / peak) : 1.0f;
	for (float& value : table) {
		value = (value - zero) * gain;
	}
}

/// THE STEEPEST SECANT FROM THE ORIGIN, ONE PER HALF — the number that lets the engine read a table
/// under a unit-slope bound.
///
/// THE ENGINE NO LONGER READS THE TABLE AT THE SIGNAL. It reads it at the signal's EXCESS over the mix
/// threshold, and adds what it finds to the threshold: `out = t + a·T(r·e)`. Two of the
/// constraints then have to hold for EVERY table in the field — the output never above 0 dBFS, and
/// the shaped top never above the dry it replaced (`|out| <= |in|`) — and the second one is a claim
/// about the table's SHAPE: `a·T(r·e) <= e` for all `e`. With `m` the largest `|T(u)|/|u|` on the
/// half being read, `a·T(r·e) <= a·m·r·e`, so `a·m·r <= 1` is sufficient, whatever the table does
/// after its first rise. That is what this computes.
///
/// PER HALF, AND A SECANT RATHER THAN THE TANGENT, both because the field was MEASURED and refused
/// the simpler forms. The tangent at the origin runs 6..40 across the brief's own knee × colour grid,
/// and every table there sits below it — but at colour 0.6 (gamma 0) the curve rises 0.3 ABOVE its
/// tangent, and at colour 0.2 and 1.0 the positive half is identically zero (a rectifier) while the
/// negative half rises 0.9 above a tangent of 0.13. The two halves differ by up to 1.5x elsewhere
/// (colour 0.38 at gamma 0.5: 33 against 50). A tangent bound would have let those tables through
/// above the input; a shared bound would have given the gentler half a slope below 1 at the crossing
/// for no reason.
///
/// A HALF THAT IS FLAT reports 0, and the engine's read of it is 0 whatever the rate — the guard
/// against dividing by it lives there, not here.
struct TableBound {
	float secantPos = 1.0f; ///< max over u > 0 of |T(u)| / u   (the identity's is 1)
	float secantNeg = 1.0f; ///< max over u < 0 of |T(u)| / |u|

	/// **|T| AT THE DOMAIN EDGE, ONE PER HALF — what the read reaches when the excess is at the
	/// headroom.** Not derivable from the secant: the secant is the steepest chord from the origin and
	/// a half-rectified table attains it on a ramp partway out, while this is the end of the curve.
	/// The read normalises by THIS so the transfer arrives at the ceiling; see `readTableAt`.
	float topPos = 1.0f;
	float topNeg = 1.0f;

};

[[nodiscard]] inline TableBound boundTable(std::span<const float> table) {
	TableBound bound;
	if (table.size() < 3) {
		return bound;
	}
	const size_t mid = table.size() / 2;
	const float step = 2.0f / static_cast<float>(table.size() - 1);
	bound.secantPos = 0.0f;
	bound.secantNeg = 0.0f;
	for (size_t i = mid + 1; i < table.size(); ++i) {
		const float u = static_cast<float>(i - mid) * step;
		bound.secantPos = std::fmax(bound.secantPos, std::fabs(table[i]) / u);
	}
	for (size_t i = 0; i < mid; ++i) {
		const float u = static_cast<float>(mid - i) * step;
		bound.secantNeg = std::fmax(bound.secantNeg, std::fabs(table[i]) / u);
	}
	// THE ENDS, READ RATHER THAN ASSUMED. `normaliseTable` puts the PEAK at 1, and for a half-rectified
	// table that peak is on an interior ramp — colour 0.625 measures T(1) = -0.900 with the peak at
	// 0.79. Assuming the edge is 1 would be a silent gain on exactly the curves that fold.
	bound.topPos = std::fabs(table[table.size() - 1]);
	bound.topNeg = std::fabs(table[0]);
	return bound;
}

/// THE WHOLE BUILD, for a caller with no reason to spread it out — the tests and both UI surfaces.
inline void buildTable(const CurveParams& p, std::span<float> table) {
	TableExtent extent;
	fillTable(p, table, 0, table.size(), extent);
	normaliseTable(table, extent);
}

/// READ THE TABLE AT AN ARBITRARY INPUT, linearly interpolated.
///
/// CLAMPED RATHER THAN WRAPPED. Past full scale a waveshaper's honest answer is its endpoint: the
/// table is the function's whole domain, and wrapping would fold a hot signal back through the curve
/// a second time — an effect nobody asked for, arriving exactly when a player pushes the drive.
[[nodiscard]] inline float lookup(std::span<const float> table, float x) {
	if (table.size() < 2) {
		return x;
	}
	// NOT A NUMBER IS NOT AN INDEX (backlog EH, shape 3). `std::clamp` passes a NaN through — both
	// comparisons are false — and `static_cast<size_t>(NaN)` is undefined, then indexes the table; the
	// input is the previous device's audio, which nothing upstream promises finite. The one site in
	// the sweep where a bad cast was a memory read rather than a wrong number.
	if (!std::isfinite(x)) {
		return 0.0f;
	}
	const float last = static_cast<float>(table.size() - 1);
	const float pos = std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f) * last;
	const auto i = static_cast<size_t>(pos);
	const size_t j = std::min(i + 1, table.size() - 1);
	const float frac = pos - static_cast<float>(i);
	return table[i] + (table[j] - table[i]) * frac;
}

// **THE DEVICE'S OWN TRANSFER, AS ONE DEFINITION BOTH THE ENGINE AND THE PLOT READ.**
//
// **THIS EXISTS BECAUSE THE DRAWN CURVE AND THE SOUND HAVE NOW DIVERGED TWICE IN ONE DAY.** On a
// scope, the displayed transfer disagreed with the output even with all modifiers off.
// The first divergence was a post-gain the plot did not know
// about; the second is structural and older — the plot drew the raw TABLE over the whole domain,
// while the device passes input through UNCHANGED below its threshold and reads the table only on the
// EXCESS above it. Those were never the same function.
//
// A restatement in the plot is what drifted, so a second restatement would drift again the next time
// the DSP moves — which has happened twice today. These functions are the definition; `Shaper` and
// `drawShaperCurve` both call them, and a case pins the engine against them so the two cannot part.

/// HOW FAR SHORT OF THE CEILING FULL WET SITS. Mix 1.0 must cross the threshold slightly below 1.0 or
/// a full-scale input is never fully shaped.
inline constexpr float kFullWetUndershoot = 0.0154f;

/// THE SMALLEST HEADROOM THE ARITHMETIC WILL DIVIDE BY. At zero headroom the read is nothing, which is
/// correct, so this only has to keep the numbers finite.
inline constexpr float kMinHeadroom = 1.0e-6f;

/// WHERE THE SHAPING STARTS AND HOW ABRUPTLY IT TAKES OVER — derived from mix alone.
struct Staging {
	float threshold = 1.0f;
	/// **WHERE THE READ STARTS — the threshold, floored at zero** (backlog IF). At Mix 1 `threshold` sits
	/// just UNDER zero so the blend is already whole at the origin (no dry notch); reading the table
	/// from there too made a zero input read its steep start, and the curve STEPPED to ±f(0) at every
	/// zero crossing — MEASURED 0.046 at the defaults, 0.77 at Knee 1 / Drive 1, where the firmware's
	/// anchored table has no step. The blend keeps `threshold`; the read starts here.
	float readFrom = 0.0f;
	float headroom = 0.0f; ///< `1 - readFrom`: what a shaped sample may add to where the read starts
	float blendSlope = 0.0f;
};
/// **HOW FAST THE DRIVE METER FALLS, IN dB PER SECOND — a DISPLAY parameter, not a DSP one.**
///
/// This was `kDriveDecay = 0.99976f`, a per-sample multiplier, and it was wrong twice over.
///
/// **IT WAS TWENTY TIMES TOO FAST, not too slow.** `meterInput` runs per SAMPLE, so at 48 kHz the
/// coefficient works out at `ln(0.99976) x 48000 = -11.52 nepers/s`, which is **-100.1 dB/s** — a
/// fall of 20 dB in 200 ms. My own comment claimed "~200 ms to a tenth", which is the same number
/// read as though it were gentle. A meter that would not move at all was really frozen, not
/// decaying — though the decay was also far too slow. The freeze was a repaint
/// hole (see `ShaperCard::update`); the decay was the opposite of the impression, and no one
/// would have known without the arithmetic.
///
/// **AND IT WAS SAMPLE-RATE DEPENDENT, which is a defect on its own.** A per-sample constant means
/// the meter falls at 100 dB/s at 48 kHz and 200 dB/s at 96 kHz — the display changing meaning with
/// the audio settings, silently.
///
/// SO IT IS STATED AS A RATE AND CONVERTED AT THE KNOWN RATE. 20 dB/s is the ordinary bar for a peak
/// meter a person reads by eye, and it is a PROPOSAL rather than a taste: the ear rules once it
/// can see it move, which could not be done before. The conversion, for the reader who wants it:
///
///     per-sample coefficient = 10^(-kDriveFallDbPerSecond / (20 * sampleRate))
///     at 48 kHz and 20 dB/s   = 10^(-1/48000) = 0.99995203
inline constexpr float kDriveFallDbPerSecond = 20.0f;


/// HOW WIDE A SWEEP THE DRIVE ASKS FOR: `R = 2^(kSweepOctaves * drive)`, 1 at drive 0 and 4 at 1.
///
/// MOVED HERE FROM `shaper.cpp` FOR THE SAME REASON AS THE STAGING: the plot needs the number the
/// device uses, and a second spelling of it is a second thing to drift.
inline constexpr float kSweepOctaves = 2.0f;

[[nodiscard]] inline float sweepFor(float drive) { return std::exp2(kSweepOctaves * drive); }
/// **ONE CALL, WHERE THIS WRAPPED A SUM.** `curveFor(knee, colour, gamma, colourPhase, …)` existed to
/// fold `gamma + colourPhase` so that the device and the plot could not spell the summation two ways —
/// and they HAD spelled it two ways, which is what it was written to stop: *"the plot did not add
/// `colourPhase` and the device did, so moving Colour Phase changed the sound and not the picture."*
///
/// **THE PARAM IS GONE (none of these should be bounded), so the sum is gone and
/// the wrapper with it.** Colour is unbounded now and carries its own laps; the counter that used to
/// receive them was measurably a second gamma knob. There is one phase input again, so a wrapper that
/// exists to reconcile two of them has nothing to reconcile — and leaving it as a pass-through would
/// be a seam whose only job was to look like the old one.
///
/// Callers take `deriveCurve` directly. `travel` is the pad's accumulated displacement, one term per
/// phase, or null.

/// **HOW MUCH STEEPER TODAY'S CROSSFADE IS THAN THE WIDEST THE CEILING ALLOWS.**
///
/// The blend completes at `diff = 1/blendSlope` and must be complete by `diff = headroom`, so the
/// widest legal slope is exactly `1/headroom` — a crossfade running the whole way from the threshold
/// to full scale. Today's slope is `(1 + 16*mixNorm^2)/headroom`, so this ratio IS the room the
/// ceiling leaves, and it is a function of MIX alone: 1.16 at mix 0.05, 5 at 0.25, 17 at 0.5, 65 at
/// 1.0. There is more room to widen at high mix and almost none at low, which is a fact about the
/// device and not a choice.
[[nodiscard]] inline float blendRatioFor(float mix) {
	const float mixNorm = mix * 2.0f;
	return 1.0f + 16.0f * mixNorm * mixNorm;
}

/// **THE CROSSFADE'S WIDTH, AS ITS OWN CONTROL — the firmware's `blendAggression`, off Knee.**
///
/// The firmware scaled the mix crossfade's slope by `0.1 + knee^2 * 1.9`, which put Knee inside the
/// Mix crossfade. That was rejected on 2026-09-02 — Knee bends the curve, and Mix is where the
/// bend starts and how abruptly — and the MECHANIC came back on 2026-09-04, on a knob of its
/// own, which keeps that orthogonality and gains a control the firmware never had.
///
/// **THE KNOB IS MEASURED IN THE CEILING'S OWN ROOM, NOT IN A FIXED SPAN, and that is the second
/// correction to it**: the aggression/blend fade zone was tiny; at minimum aggression it goes
/// all the way from threshold to the end of the table. It was a fixed 20:1 multiplier, so at mix 1
/// the widest it could reach was 14.5x the floor and the fade got a fourteenth of the room the
/// ceiling actually allowed. Returning `ratio^(2*knob)` against a floor of `1/headroom` puts
/// **knob 0 exactly ON the floor — the blend completing at `|x| = 1.0`, the end of the table** —
/// neutral at today's slope, and the top the same factor beyond it.
///
/// So the SPAN is mix-dependent, and honestly so: the knob can only widen into room the ceiling has
/// left, and at low mix there is almost none. A fixed span that ignored that would either break the
/// ceiling at low mix or waste the room at high.
[[nodiscard]] inline float aggressionFor(float knob, float ratio) {
	return std::pow(ratio, 2.0f * std::clamp(knob, 0.0f, 1.0f));
}

/// @param aggression the crossfade-width knob, 0..1, neutral at 0.5. **NOT DEFAULTED, DELIBERATELY.**
///        Every other term this function needs is passed explicitly and a missing one would not
///        compile; a defaulted one would mean "neutral" and let a caller quietly draw or play a
///        different crossfade from the rest of the device. That exact trap — `curveFor`'s
///        `travel = nullptr` — cost two silent bugs in one day (the plot drew the no-travel curve,
///        and `advanceBuild` built the no-travel TABLE), so this argument is required and the
///        compiler is what finds the call sites.
[[nodiscard]] inline Staging stagingFor(float mix, float aggression) {
	Staging out;
	out.threshold = 1.0f - (1.0f + kFullWetUndershoot) * mix;
	out.readFrom = std::fmax(out.threshold, 0.0f);
	out.headroom = 1.0f - out.readFrom;
	const float headroom = std::fmax(out.headroom, kMinHeadroom);
	// **THE FLOOR IS THE UNIT OF THE KNOB, not a clamp it happens to hit.** `1/headroom` is the
	// slope whose blend finishes exactly as the excess reaches the headroom — the crossfade running
	// the whole way from the threshold to full scale — and the knob is `ratio^(2*knob)` of it.
	const float floorSlope = 1.0f / headroom;
	const float wanted = floorSlope * aggressionFor(aggression, blendRatioFor(mix));

	// **THE CEILING IS NOT NEGOTIABLE, AND THIS IS WHERE IT IS ENFORCED RATHER THAN IN THE KNOB'S
	// RANGE.** `processSample` blends over `min(diff * blendSlope, 1)`, so the crossfade completes at
	// `diff = 1/blendSlope` and must be complete by the time `|x|` reaches 1.0 — the excess reaches
	// the headroom — or a hot sample leaves part-DRY, and the dry side is `x` itself, which is above
	// full scale. That is the guarantee the `ceiling` work exists for.
	//
	// Today's slope carries a margin of `1 + 16*mixNorm^2` over that requirement, and a widening
	// aggression spends it. **MEASURED, the smallest aggression that still clears the ceiling depends
	// on MIX**: 0.862 at mix 0.05, 0.610 at 0.10, 0.200 at 0.25, 0.059 at 0.50, 0.015 at 1.00. So no
	// fixed knob range can be both safe at low mix and useful at high — which is why the knob is free
	// and the SLOPE is floored here. Below the floor the control stops widening and says so by not
	// changing the sound; that is the ceiling binding, not a dead range.
	out.blendSlope = std::fmax(wanted, floorSlope);
	return out;
}

/// **THE KNEE PRE-SQUASH IS RETIRED (2026-09-22), and Knee reaches the sound through the table.**
///
/// It was added on 2026-09-03 as a first GUESS, to be auditioned later —
/// when the read was unit-slope and normalised Knee out of the table's first rise, so Knee needed a
/// job. The read was then rebuilt to reach the whole table (`rate = sweep / headroom`, below), which
/// hands Knee back the job the firmware gives it: steepness `1 + 9·knee` and overdrive `1 + knee`
/// INSIDE the table. From then on the squash only pulled reads back from the table's outer folds.
///
/// MEASURED against a transcription of the firmware's `deriveParameters` + `evaluateTransfer`, a sine
/// at 0.9 across 96 colours, harmonics 2..16: at Knee 1 the port's spread across Colour was 9.8 dB
/// against the firmware's 13.3, and its wildest colour stopped at 97 % THD against the firmware's
/// 204 %. Without the squash: 11.7 dB and 216 %. By ear there was less timbre variation than the
/// firmware has. The case "KNEE: Colour's timbre spread
/// is the firmware's" holds it.
/// **HOW MUCH OF THE TABLE IS IN THE SIGNAL AT ALL, AS A FUNCTION OF KNEE — the fix for
/// a very abrupt jump between off and minimum knee.**
///
/// MEASURED (backlog BI, the probe in `test_fx_shaper.cpp`), rising saw at 0 dBFS, drive 0, colour
/// 0.38: knee 0 is 0.00 dB and peak 1.000 because `Settings::linear` is a true bypass consulting no
/// table; knee 0.001, the first non-linear value, is **-0.91 dB with peak 0.923**. The first 0.1% of
/// the knob costs 0.91 dB and the next 25% costs 0.17 — the step is five times the whole next
/// quarter-turn, which is what an abrupt jump sounds like.
///
/// **AND KNEE CANNOT CLOSE IT, WHICH IS WHY THIS EXISTS RATHER THAN A REFINEMENT OF THE SQUASH.**
/// Knee 0.001 and knee 0.05 differ by 0.008 dB, so the table's cost near zero is not a function of
/// knee at all: knee then governed only a pre-squash inside the read (retired since — see above), while the
/// discontinuity is between NO TABLE and TABLE-WITHOUT-SQUASH. Making the squash gentler cannot
/// interpolate a gap the squash does not own. Something has to fade the table IN, and the device
/// already owns a proven dry/wet crossfade to fade it with.
///
/// SO THE FADE MULTIPLIES `blend`, THE AMPLITUDE CROSSFADE, rather than adding a second one. That
/// crossfade is already the thing that must agree with the sub-threshold branch at zero — "THE
/// THRESHOLD IS CONTINUOUS" pins it — so a factor on it inherits that continuity instead of opening
/// a new seam beside it.
///
/// **THE TOP IS A CANDIDATE FOR AUDITION, NOT A MEASURED OPTIMUM.** 0.05 spreads the 0.91 dB over the
/// first 5% of the knob, about 0.18 dB per step at a hundred steps of travel. That it is smooth is
/// arithmetic; that it FEELS like the same control is the ear, and the number is here in one place
/// so it can be moved by one edit.
inline constexpr float kKneeFadeTop = 0.05f;

/// ONE DEFINITION, BECAUSE THE ENGINE AND THE PLOT BOTH APPLY IT. The drawn transfer and the sound
/// diverged twice in one day over exactly this kind of term living in two places; see the note above
/// `staticTransferAt`.
[[nodiscard]] inline float kneeFadeFor(float knee) {
	if (kKneeFadeTop <= 0.0f) {
		return 1.0f;
	}
	return std::clamp(knee / kKneeFadeTop, 0.0f, 1.0f);
}

/// **STAIR AND CHEBY, APPLIED AT THE READ — backlog IM (2026-09-23).** It was asked whether they gained
/// anything from being table terms; they did not, and it cost them modulation: a table term is a rebuild
/// and a crossfade (~21 ms) per change. Both act on the TABLE COORDINATE `u = rate * excess`, exactly
/// where the table was built, so doing them here is the same shape read at the exact point:
///   - Stair rounds `u` to its grid before the lookup (mid-tread: quiet input rounds to zero).
///   - Cheby crossfades the looked-up, already-normalised value toward Tn of the same coordinate, taken
///     through the knee's intensity and the pre-stage as the table's bases are. T3 is bounded and
///     anchored as it stands; T2 anchored and normalised is t^2, which is what the table held.
/// Deadzone stays IN the table, by design: it is the firmware's, and the firmware renormalises
/// after it — per sample it would get quieter as it widened.
/// The default is the identity, so a caller that passes nothing reads the table unchanged.
struct ReadShapes {
	float stairSteps = 0.0f; ///< 0 = off; else steps per unit of the table coordinate
	float chebyMix = 0.0f;   ///< -1..+1: toward T2 (left) or T3 (right)
	float intensity = 1.0f;  ///< the knee's overdrive, `1 + drive` — what the table's bases saw
	float preExpand = 0.0f;  ///< the pre-stage every basis sees
};

/// THE SHAPES A CURVE'S SETTINGS ASK FOR — one function, so the engine and the plot cannot disagree.
[[nodiscard]] inline ReadShapes readShapesOf(const CurveParams& p) {
	return { p.stairSteps, p.chebyMix, 1.0f + p.drive, p.preExpandAmount };
}

/// ONE TABLE COORDINATE THROUGH THE SHAPES: Stair's grid, the lookup, then Cheby's crossfade.
[[nodiscard]] inline float shapedLookup(std::span<const float> table, float u, const ReadShapes& shapes) {
	if (shapes.stairSteps > 0.0f) {
		u = std::round(u * shapes.stairSteps) / shapes.stairSteps;
	}
	float value = lookup(table, u);
	if (shapes.chebyMix != 0.0f) {
		// THE POLYNOMIAL'S ARGUMENT, AS THE TABLE'S BASES TAKE IT: the magnitude overdriven by the knee,
		// through the pre-stage, clamped to the polynomials' own domain, signed back.
		const float m = std::fmin(std::fabs(u), 1.0f);
		const float od = m * shapes.intensity;
		const float pre = shapes.preExpand > 0.001f ? od * (1.0f + shapes.preExpand * 0.5f * (1.0f - od * od)) : od;
		const float t = std::fmin(pre, 1.0f);
		const float tn = (shapes.chebyMix < 0.0f) ? t * t : (u < 0.0f ? -1.0f : 1.0f) * (4.0f * t * t - 3.0f) * t;
		value += (tn - value) * std::fmin(std::fabs(shapes.chebyMix), 1.0f);
	}
	return value;
}

/// THE TABLE READ: `a*T(r*e)`, with `r` from the half the excess points into. The excess is read as
/// it is — the Knee pre-squash that used to sit here is retired (see the note above `kneeFadeFor`).
///
/// A FLAT HALF reads 0 whatever the rate, and at zero headroom `a` is zero, so the guard only has to
/// keep the arithmetic finite.
[[nodiscard]] inline float readTableAt(std::span<const float> table, const TableBound& bound,
                                       float headroom, float sweep, float excess, const ReadShapes& shapes = {}) {
	const float secant = (excess >= 0.0f) ? bound.secantPos : bound.secantNeg;
	// **`secant <= 0` NO LONGER RETURNS SILENCE HERE** — it is handled below by passing the excess
	// through unshaped. It sat in this guard beside two genuinely degenerate conditions and inherited
	// their answer; a flat half of a table is not degenerate, it is a curve that does nothing, and
	// the honest output for "does nothing" is the input. See the note at the scale.
	if (headroom <= kMinHeadroom || sweep <= 0.0f) {
		return 0.0f;
	}
	// **A FLAT HALF PASSES THROUGH UNSHAPED; IT DOES NOT MUTE — this is the whole of the fix here.**
	//
	// MEASURED (backlog BI): at colours 1/8, 5/8 and 8/8 — zone boundaries, with eight colour zones —
	// the raw table has content on ONE side only. Colour 0.625 gives `lo=-0.943, hi=+0.000122`, so
	// `boundTable` returns **`secantPos = 0`**, and that condition used to sit in the degenerate-input
	// guard above and return 0. Every positive sample of every waveform came out at the threshold:
	// input +0.56..+0.82 measured out -0.0148, which is silence with a DC offset.
	//
	// **RETURNING 0 WAS NEVER THE HONEST ANSWER.** A half of a table with nothing in it means NO
	// SHAPING, and the output for "no shaping" is the excess itself — the same thing knee 0 does when
	// there is no table at all. Muting half a waveform is the one result a shaper should never
	// produce from a curve it cannot read, and it is not a degenerate INPUT, which is what that guard
	// is for.
	//
	// IT KEEPS BOTH RULES. `|out| = |excess|` satisfies `|out| <= |excess|` with equality, and the
	// total is `threshold + excess = |x|`, which is under 0 dBFS because the input is.
	//
	// **WHAT THIS DOES NOT FIX, AND IT IS THE LARGER HALF: the other half of those tables still reads
	// almost nothing**, because the read window misses where the curve acts — see the note on `rate`
	// below and backlog BI. That needs a decision, because every way of widening the window costs
	// `|out| <= |excess|`.
	const float top = (excess >= 0.0f) ? bound.topPos : bound.topNeg;
	if (secant <= 0.0f || top <= 0.001f) {
		return excess;
	}
	//---------------------------------------------------------------------------------------------//
	// THE EXCESS, SCALED SO FULL SCALE LANDS ON THE TABLE'S EDGE — AND THE FOLDS BECOME REACHABLE.  //
	//---------------------------------------------------------------------------------------------//
	//
	// **THE STATEMENT OF IT, 2026-09-03:** only the excess above the threshold feeds the
	// table, renormalised so the signal fed to the table is scaled as signal/(0dBFS-threshold),
	// so the table still saturates when the signal reaches 0 dBFS. `0 dBFS - threshold` IS
	// `headroom`, so `rate = 1/headroom` sends an input at full scale to the domain edge. Drive
	// multiplies it, which is what drive is for: `lookup` pins the argument, so more drive is more of
	// the curve's top.
	//
	// **AND IT REPLACES A GLOBAL SECANT DIVIDE THAT WAS HIDING THE DEVICE'S OWN CHARACTER.** DECIDED,
	// same day: the dry/wet peak relationship is relaxed, because it was forcing odd conditions.
	// `|out| <= |excess|` was enforced by `a·m·r = 1` with `m` the steepest chord
	// ANYWHERE in the table, so one steep segment taxed every sample — and MEASURED, `secantPos` is
	// 6.38 at knee 0.05 and 14.88 at knee 1. The read reached only the first `sweep/secant` of the
	// domain: 0.21 of the table at colour 0.25, 0.23 at 0.5.
	//
	// **WHAT SAT IN THE UNREACHED PART IS THE FOLDING, AND THE FOLDING IS THE DEVICE.** yeah
	// its a shaper/folder really. MEASURED direction reversals across the positive half: **0 at
	// colour 0 — a plain monotone saturation — 6 at colour 0.25, and 16 at colour 0.38.** The old
	// window ended around the first fifth, which is the monotone rise, so almost none of the folding
	// was reachable at any setting. The level loss, the three silent colours and the transfer never
	// arriving at 0 dBFS were that one fact in different clothes.
	//
	// `scale = headroom` needs no derivation: `normaliseTable` already put `max|T|` at 1, so the
	// output is bounded by the headroom by construction. **The build and the read now measure the
	// table with the same yardstick.**
	//
	// WHAT REMAINS GUARANTEED: `|T| <= 1` gives `|out| <= headroom`, and `headroom = 1 - threshold`,
	// so the sum cannot pass 0 dBFS. The format is safe; only the dry-relative bound is gone, and a
	// folder is a device that deliberately puts the shaped top ABOVE the dry it replaced.
	const float rate = sweep / headroom;
	const float scale = headroom;
	return scale * shapedLookup(table, rate * excess, shapes);
}

/// **WHAT THE DEVICE DOES TO ONE SAMPLE, WITH MODIFIERS AND HYSTERESIS OFF.**
///
/// Identity below the threshold; above it, the input's EXCESS over the threshold is
/// read through the table and crossfaded in, then the whole thing is held at the ceiling. That is
/// `processSample`'s shape, and a case asserts the engine agrees with this to the bit for a settled
/// device.
///
/// **HYSTERESIS AND THE MODIFIERS ARE ABSENT ON PURPOSE, NOT FORGOTTEN.** They are state — they depend
/// on where the signal came from, not only on where it is — so no static transfer can carry them, and
/// a plot that pretended otherwise would draw a curve no single sample ever follows.
[[nodiscard]] inline float staticTransferAt(std::span<const float> table, const TableBound& bound,
                                            const Staging& staging, float sweep, float kneeFade,
                                            float x, const ReadShapes& shapes = {}) {
	const float absX = std::fabs(x);
	const float sign = (x < 0.0f) ? -1.0f : 1.0f;
	if (absX - staging.threshold <= 0.0f) {
		return std::clamp(x, -1.0f, 1.0f);
	}
	// **THE KNEE FADE MULTIPLIES THE AMPLITUDE BLEND** — see `kneeFadeFor`. The engine does the same
	// thing on the same term, which is the only reason the drawn curve and the sound stay one function.
	const float blend = kneeFade * std::min((absX - staging.threshold) * staging.blendSlope, 1.0f);
	const float excess = absX - staging.readFrom; // the read starts at zero or above — see `readFrom`
	const float shaped = sign * staging.readFrom
	                     + readTableAt(table, bound, staging.headroom, sweep, sign * excess, shapes);
	return std::clamp(x * (1.0f - blend) + shaped * blend, -1.0f, 1.0f);
}


//--------------------------------------------------------------------------------------------//
// EMPHASIS — the tilt into the curve, and its exact inverse out of it                         //
//--------------------------------------------------------------------------------------------//

/// ONE FIRST-ORDER SECTION, transposed direct form II: `y = b0·x + z;  z = b1·x − a1·y`.
struct TiltSection {
	float b0 = 1.0f;
	float b1 = 0.0f;
	float a1 = 0.0f;

	[[nodiscard]] float step(float x, float& z) const {
		const float y = b0 * x + z;
		z = b1 * x - a1 * y;
		return y;
	}
};

/// **THE TILT SHELF, AND ITS INVERSE.** The analogue prototype is `H(s) = (√g·s + 1) / (s + √g)`
/// normalised to the pivot: DC gain `1/√g`, high-frequency gain `√g`, unity at the pivot on a
/// log-symmetric slope — so `db` lifts the highs by `db` and cuts the lows by the same. Bilinear with
/// the pivot pre-warped, `K = tan(π·pivot/rate)`:
///
///     b0 = (√g + K) / (1 + √g·K)     b1 = (K − √g) / (1 + √g·K)     a1 = (√g·K − 1) / (1 + √g·K)
///
/// **THE INVERSE SWAPS NUMERATOR AND DENOMINATOR**, and is stable because the section is minimum
/// phase (its zero, like its pole, sits inside the unit circle for every `g > 0`). So `pre` then
/// `inverse` is the identity to float rounding wherever nothing between them is non-linear — which is
/// what makes Emphasis tilt only what the curve bends. At 0 dB both sections are exactly the identity:
/// `b0 = 1` and `b1 = a1`, so the state stays at zero and the output is the input to the bit.
[[nodiscard]] inline TiltSection tiltSection(double db, double pivotHz, double rate, bool inverse) {
	const double sg = std::pow(10.0, db / 20.0);
	const double nyquistSafe = std::min(pivotHz, rate * 0.45);
	const double k = std::tan(std::numbers::pi * nyquistSafe / rate);
	const double n0 = sg + k;
	const double n1 = k - sg;
	const double d0 = 1.0 + sg * k;
	const double d1 = sg * k - 1.0;
	TiltSection s;
	if (!inverse) {
		s.b0 = static_cast<float>(n0 / d0);
		s.b1 = static_cast<float>(n1 / d0);
		s.a1 = static_cast<float>(d1 / d0);
	}
	else {
		s.b0 = static_cast<float>(d0 / n0);
		s.b1 = static_cast<float>(d1 / n0);
		s.a1 = static_cast<float>(n1 / n0);
	}
	return s;
}
} // namespace feathers::fx::shaper
