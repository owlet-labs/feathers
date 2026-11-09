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

#include "device/device.h"
#include "fx/shaper/shaper_phase_space.h"
#include "device/registry.h"
#include "fx/shaper/shaper_curve.h"
#include "dsp/slice_build.h" // fieldWithOverride — the six extras' scalers

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// THE TABLE SHAPER — a waveshaper whose curve is a PLACE in a φ-field rather than a preset.
//
// Two axes build the transfer curve and everything else plays it. KNEE says how hard, COLOUR says
// what kind, and the six basis functions behind them are blended by half-rectified triangles at
// φ-power rates, so no two positions produce the same curve and the field never repeats.
//
// TWO AXES THAT ARE NOT THE SAME KIND OF THING, and the difference is measured rather than asserted.
// It decides how each is drawn, how each is named, and why only one of them expands into a pad.
//
//   KNEE IS A SCALE. It maps straight through to `drive`, and every quantity downstream of it is
//   monotone in it. MEASURED across the eight zone centres, colour 0, gamma 0, THD of a sine at
//   −10 dBFS: 3.3, 5.6, 9.3, 13.5, 17.1, 19.8, 21.9, 23.3 percent. Strictly increasing, no
//   inversions, no surprises. A zone of Knee is a RANGE OF ONE QUANTITY, and its name is an ordinal
//   with a measurement behind it.
//
//   COLOUR IS A FIELD. MEASURED, 256 samples per zone, as the distance the six-weight vector travels
//   across the zone: 2.3, 2.9, 4.5, 5.7, 7.0, 11.3, **1.4**, 20.0. **A zone of Colour is not one
//   character** — the field keeps moving inside it, and it moves nine times faster at the top of the
//   axis than at the bottom, which is `freqMult = 1 + colour² × 3` visible in a number. The one
//   exception is zone 6 at 1.4, where a single basis leads 99% of the zone, and that is not the field
//   behaving: it is the hard-coded inflator gate in `deriveCurve`.
//
// SO THE ZONE NAMES BELOW SAY DIFFERENT KINDS OF THING FOR THE TWO CONTROLS, deliberately. Knee's
// name a position on a scale; Colour's name what was measured to lead the zone, and say when nothing
// leads it. A Colour zone whose name promised one character would be a name that is wrong for most of
// the zone it captions — which is the failure the zone-name rule exists to prevent, arriving through
// the front door rather than through invention.
//
// AND THAT IS WHY ONLY COLOUR GETS THE PAD. A scale has one dimension and gains nothing from a
// second; a field has as many as you give it. Gamma is the second — the firmware's "secret knob",
// reached there by push-and-twist on a hardware encoder, which is a gesture and not a coordinate.
// Given an axis it becomes what it always was: MEASURED, the weight vector travels 13.9 across
// gamma 0..1 at fixed colour, which is more than six of the eight colour zones. It is a real
// dimension of the field and it was hidden behind a modifier key.
//
// THE CARD IS THE GAMMA-ZERO LINE AND THE PAD IS THE PLANE. That falls out rather than being
// designed: `deriveCurve` gates the inflator on `gamma == 0`, so the card — which has no gamma
// control and leaves it at its default — is exactly the line where the one named character exists.
// Open the pad and step off it and the field is continuous again. Nothing is hidden on the card and
// nothing is lost by opening the panel.
//
// PLACEMENT: THE BUS BY DEFAULT, PER VOICE ON `Param::Poly` (backlog OM 2026-10-24: a poly mode
// on the shapers). **ONE TABLE FOR EVERY VOICE**: in Poly the table is shared, while the un-baked params can
// still move per voice. Under Poly each voice is its own shaper — its staging,
// filters, memory envelope, extras and note (the Lowpass extra tracks THAT voice's pitch), and a per-voice route
// reaches every PER-SAMPLE control by the host's per-voice resolve — but the TABLE is the bus instance's, one bank
// built from the device's own row (`AudioOps::beginSharedBlock`) and read by every voice (`pullHandBlocks`). The BAKED
// side is `Settings::sameCurveAs`'s list — Knee, Colour, Gamma, the pad's travel, Deadzone and its power, Colour Mode,
// Inflate — and a voice takes those from the bus each block, so nothing per voice can move the table; none of them is
// a modulation destination today, which the design leaves as it is (provisionally). MEASURED (test_fx_shaper "OM:
// MEASURED ..."): a voice was 9,312 B with its own table and is 688 B now; the bank (~8.6 KB) is the bus's alone. A
// voice seated as Poly comes on plays the device's table from its first block: a bank its voices would hear as the
// identity is built whole at once (`Shaper::buildNow`, one rebuild's work, ~0.3 ms measured) rather than amortised.

namespace feathers::fx::shaper {

/// EMPHASIS' RANGE, in dB of high-frequency lift into the curve (the lows get the same cut). ±12 is
/// a 24 dB span end to end — deliberately strong so the knob is plainly audible; REASONED, not tuned
/// by ear. See `Param::Emphasis`.
inline constexpr double kEmphasisMaxDb = 12.0;
/// WHERE THE TILT PIVOTS — the frequency the shelf leaves at 0 dB whatever the setting.
inline constexpr double kEmphasisPivotHz = 800.0;
/// THE SLEW STAGE'S "HOW BRIGHT IS THE SOURCE", FIXED. It was the Harmonics knob, whose default was
/// this; Lowpass and Integrator therefore sound as they did at that default.
inline constexpr float kSlewHarmonics = 0.5f;

/// **THE MEMORY STAGES' ONE ENVELOPE (backlog IE).** Bloom and Sag both read how hard and how long you
/// have been playing, which is one quantity, so they share one follower: a peak detector on the
/// trimmed, emphasised input, per channel. Attack fast enough to catch a pick, release slow enough to
/// be heard as the circuit recovering rather than as a gain wobble. Constants, not knobs, until an ear
/// says otherwise.
inline constexpr double kMemoryAttackSeconds = 0.010;
inline constexpr double kMemoryReleaseSeconds = 0.250;
/// BLOOM'S FULL SCALE: the bias, as a fraction of full scale, at Bloom 1 and an envelope of 1.
inline constexpr float kBloomDepth = 0.35f;
/// SAG'S FULL SCALE: how far the rails fall, as a fraction, at Sag 1 and an envelope of 1. Floored in
/// the engine so the division by the rail height stays bounded.
inline constexpr float kSagDepth = 0.6f;
/// THE LOWEST THE RAILS GO, whatever the knob and the envelope say — the stage divides by the rail.
inline constexpr float kSagFloor = 0.25f;
/// LOOP'S CEILING — the fed-back residue's gain at Loop 1. Below one, so the linear region, where the
/// residue is zero anyway, is contractive whatever the knob says.
inline constexpr float kLoopMaxGain = 0.9f;
/// LOOP'S DARKENING: the one-pole in the loop. What recirculates is the low part of what the curve took
/// away, so the loop thickens rather than fizzes.
inline constexpr double kLoopCornerHz = 2000.0;
/// WHERE LOOP READS THE CURVE'S ORIGIN SLOPE: a secant this close to zero, small enough to sit inside
/// every curve's first table entry pair and large enough to stay out of float noise.
inline constexpr float kLoopProbe = 1.0e-3f;
/// STAIR'S RANGE: steps per unit of the curve's input, from fine at the knob's first notch to two at its
/// end, geometric between so each notch is the same RATIO coarser.
inline constexpr float kStairFinest = 32.0f;
inline constexpr float kStairCoarsest = 2.0f;

/// **THE INPUT FILTERS' RANGE (backlog FA)** — 20 Hz to 20 kHz for both, stored as log2(Hz) (`Unit::LogHertz`),
/// Featherverb's full-range filters (backlog LB) for the same reason. Each END IS OFF, not a corner: the highpass at
/// its floor and the lowpass at its ceiling pass the input to the bit, so a fresh shaper sounds exactly as it did
/// before these existed.
inline constexpr double kInputFilterMinLog2 = 4.321928094887363;  // log2(20)
inline constexpr double kInputFilterMaxLog2 = 14.287712379549449; // log2(20000)


/// A ONE-POLE'S EXACT COEFFICIENT FOR A CORNER IN log2(Hz), `1 - exp(-2π·fc/fs)` — the form `Shaper`'s note
/// lowpass uses, and for its measured reason: the linear `2π·fc/fs` passes 1 below the top of this range.
[[nodiscard]] inline float inputFilterAlpha(float log2Hz, double sampleRate) {
	const double hz = std::exp2(static_cast<double>(log2Hz));
	return static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * hz / sampleRate));
}

/// **THE CROSSFADE MIX (backlog FA): the input against the shaped output, LINEAR.** The two are the same signal
/// where the curve is gentle, so an equal-power law would lift the middle of the knob by up to 3 dB. At 1 it returns
/// the wet untouched, which is what keeps the default bit for bit the device before this existed. **NOT CLAMPED —
/// *no clamp on the mix*.** The dry share passes as it came, so a hot input at Mix < 100% leaves
/// above 0 dBFS; the 0 dBFS rule covers the SHAPED path only. A clamp here hard-clipped the dry on 120 of 256
/// samples at +3 dB and 168 at +6 dB (Mix 50%, MEASURED), which is why it went. One
/// function, which the engine and the plot both call.
[[nodiscard]] inline float dryWetMix(float dry, float wet, float mix) {
	if (mix >= 1.0f) {
		return wet;
	}
	return dry + (wet - dry) * mix;
}

[[nodiscard]] inline float stairStepsFor(float stair) {
	if (stair <= 0.0f) {
		return 0.0f;
	}
	return kStairFinest * std::pow(kStairCoarsest / kStairFinest, std::fmin(stair, 1.0f));
}

/// The shaper's controls. A control is addressed by its INDEX here and never by name — device.h's
/// permanent rule.
enum class Param : uint8_t {
	Enabled = 0, ///< bypass switch; the head's square

	//-- the curve. Each rebuilds the table, so each is a MONO destination (OM, 2026-10-31): under Poly one value for every voice. --//
	Knee = 1,   ///< the scale axis — 0 is a true bypass, 1 is maximum steepness
	Colour = 2, ///< the field axis — which bases are blended, and how
	Gamma = 3,  ///< the field's second axis; 0 is the line the card sits on
	//-- what plays the curve. These two are cheap per-sample gains, so these two modulate. -------//
	Drive = 4, ///< how far past the threshold the table is swept; 0 is the unit-slope read
	/// **AN AMPLITUDE THRESHOLD, NOT A CROSSFADE — RENAMED FROM `Mix` (backlog FA, 2026-10-10).** It was shown as
	/// "Shaper Mix" and set where the shaped path takes over; the real crossfade is `DryWet`. Its KEY stays
	/// `shaper_mix`, because what its number means did not change — only its name (the ledger's rule).
	Threshold = 5,

	/// **EMPHASIS — WHICH FREQUENCIES THE CURVE BITES. Was `Harmonics`, and that did nothing.**
	///
	/// What does harmonics knob do? ive never heard it impact the sound — it set
	/// only the slew stage's duty cycle, audible only through Lowpass or Integrator, both off by
	/// default (the firmware read the number off the oscillator type; a bus shaper cannot see one).
	/// Asked what it should become, the choice was a **pre/de-emphasis tilt**: a first-order tilt shelf
	/// pivoting at `kEmphasisPivotHz` BEFORE the curve and its exact inverse AFTER it. Wherever the
	/// device is linear the pair cancels, so unshaped signal comes back as it went in; what the curve
	/// bends comes back shaped by the inverse. Positive: highs +dB and lows −dB into the curve, so the top
	/// end saturates first — and the inverse then cuts what that made, so the grit comes back DARK (tape-
	/// like). Negative: the lows drive the curve and the inverse LIFTS the harmonics they throw up, so the
	/// grit comes back BRIGHT and fizzy. MEASURED on a saw (share of the added distortion above 2 kHz):
	/// 13 % at -12 dB, 1.4 % at 0, 1.5 % at +12 — the opposite of this doc's first draft. 0 is flat, and
	/// is exactly the device as it was.
	///
	/// MODULATABLE: coefficients follow it at the block rate, and it is no table term, so it costs no
	/// rebuild. The slew duty it used to set is now `kSlewHarmonics`, the old default, so Lowpass and
	/// Integrator sound as they did at that default.
	Emphasis = 6,

	//-- THE EXTRAS. Five bits behind push-and-twist in the firmware; five switches here. ---------//
	//
	// THEIR AMOUNTS ARE NOT KNOBS AND MUST NOT BECOME ONES. Every one of these has a depth, a rate or
	// a ratio, and every one of those is READ OFF THE COLOUR FIELD at the current position — sub
	// ratio, stride, comb depth and rotation amount come out of `kExtrasBank`, and the slew intensity
	// out of its own triangle. A knob per amount would be nine more controls, and it would break the
	// one claim this device makes: that the position IS the sound. The switch says whether the field's
	// answer is heard; the field says what the answer is.
	Sub = 7,        ///< sub-harmonic gain modulation, counted off zero crossings
	Feedback = 8,   ///< comb feedback at the field's stride
	Mangle = 9,     ///< rotate the sample word: aliasing, and it is as rude as it sounds
	                ///<
	                ///< **NAMED `Mangle` AND NOT `Rotate`, WHICH IT WAS**, so that the rotate
	                ///< mechanic stays consistent. `Rotate`
	                ///< is the PLANE mechanic — it turns the basis a pad travels in, and it means the
	                ///< same thing on every device that has one. A bitwise rotation of the sample word
	                ///< is a different verb that happened to share the word, and leaving it here would
	                ///< have put two unrelated `Rotate`s on one device. The mechanism is kept in the
	                ///< description; the NAME now says what it does to the sound.
	Lowpass = 10,   ///< one-pole lowpass ahead of the table, cutoff tracking the note
	Integrator = 11, ///< zero-crossing-reset integrator — triangle-ish. Runs AFTER Lowpass, not instead

	/// HYSTERESIS' FIELD SCALER — the φ layer's own `fieldWithOverride`, in front of the one
	/// per-sample effect that had no control at all.
	///
	/// IT IS A BIAS AND NOT AN AMOUNT, which is why it rests at 0.5 rather than 0. `slice_build.h`
	/// states the curve: knob 0 gives none of the field, 0.5 gives the field exactly as the position
	/// authored it, 1 gives all of it. Six of the flagship's fields already sit behind this same
	/// function.
	///
	/// A SCALER RATHER THAN A SWITCH, DELIBERATELY, and the header's own rule is what decides it: the
	/// extras' AMOUNTS must not become knobs because the position is meant to be the sound. A bias in
	/// front of a field does not break that — at 0.5 the position IS still the sound, and the control
	/// says how much of what the position said you want to hear. A switch would only offer all or
	/// nothing of it.
	///
	/// WHY THIS ONE FIELD GETS IT. Hysteresis is applied unconditionally — the firmware does not carry
	/// it in `extrasMask` either — and it is the only per-sample effect with neither a switch nor a
	/// scaler. MEASURED at the default position (colour 0, where the field reads its extreme -1.000),
	/// 220 Hz at -12 dBFS: THD 26.5% with the field as authored against 6.25% with it removed. Twenty
	/// of those twenty-six points are this one ungated term.
	Hysteresis = 12,

	/// **THE PLANE'S TURN — the basis the pad travels in, not a bit rotation.**
	///
	/// BY DESIGN, Rotate mutates the BASIS VECTORS and leaves the puck where it is, so
	/// the sound at rest cannot move — what changes is the meaning of the NEXT move. `slice_basis.h`
	/// says the same thing from the other side: *a rotate that changes the sound is redundant with
	/// gamma*, and a session broke that rule and had to be corrected. It is structural rather than a
	/// convention: travel says where the phases ARE, rotate says how a move MAPS into them, and a
	/// rotate that altered the output would collapse two independent things into one.
	///
	/// NOT MODULATABLE, and not because it is cheap to leave out: a modulated basis would make the
	/// same gesture mean something different from one block to the next, which is the one property
	/// this control exists to hold still.
	Rotate = 13,

	/// **HOW ABRUPTLY THE SHAPED PATH TAKES OVER ONCE MIX'S THRESHOLD IS CROSSED.** The firmware's
	/// `blendAggression` as its own control: there it was `0.1 + knee^2 * 1.9` and so put Knee inside
	/// the Mix crossfade, which was rejected. On a knob of its own it keeps Knee bending the curve
	/// and Mix choosing where the bend starts, and adds the third thing neither of them said.
	///
	/// **MODULATABLE, AND IT IS THE ONLY PART OF THIS DEVICE'S CHARACTER THAT IS FREE TO MODULATE.**
	/// It reaches the sound through `stagingFor` — a per-block scalar read per sample — and touches
	/// no table entry, so it is deliberately absent from `sameCurveAs` and asks for no rebuild. Knee,
	/// by contrast, bakes `drive` and `preExpandAmount` INTO the table: MEASURED, moving Knee 0.05 at
	/// drive 0.5 changes the transfer by rms 0.61 of which the table owns essentially all, so a
	/// modulated Knee can only turn its table over at ~47 Hz. This one has no such ceiling.
	Aggression = 14,

	/// **INPUT TRIM, IN DECIBELS — how hard the signal drives the table, set before anything else.**
	///
	/// A plain input gain knob on the card and the window. It earns its place more than it did when it
	/// was first proposed, because the device's own low-level gain is now the design: MEASURED at colour 0,
	/// knee 0.5, the transfer's slope is 3.93x at the origin and 1.00x at full scale, so where the
	/// signal sits against the threshold decides most of what you hear. A trim is the control that
	/// moves it there.
	///
	/// **ON THE INPUT, SO THE DRY MOVES WITH IT.** That is what makes it a trim rather than a second
	/// Drive: Drive scales the read POSITION and leaves the dry alone, so it changes the shaped path
	/// only; this scales what the whole device sees, including the sample the threshold is compared
	/// against and the dry that a sub-threshold sample is returned as.
	///
	/// **MODULATABLE, AND FREE LIKE `Aggression`**: one multiply per sample, no table term, absent
	/// from `sameCurveAs`.
	Trim = 15,

	/// **THE DEADZONE, AS A SWITCH — OFF BY DEFAULT.** Compared against the reference, the port's tone
	/// was more aggressive and jumpy than the firmware's, and a deadzone switch is the remedy.
	///
	/// WHY IT WAS ALWAYS ON: the port derives every field at every position (backlog EN kept the
	/// port's way), while the firmware's own default path — gamma 0, harmonic weight under 0.8 — goes
	/// through a plain `deriveParameters` that leaves the deadzone at zero. So a stock hardware shaper has
	/// no deadzone and a stock port did, at whatever width the colour field gave it; zone 0 is named
	/// "Gate" for it. Now it joins the five extras as the sixth switch: the field still says HOW WIDE
	/// and WHERE, the switch says whether it is heard. The same shape as the others, for their reason.
	///
	/// A TABLE TERM, so it is in `sameCurveAs` and flipping it rebuilds the table. A MONO destination (OM, 2026-10-31).
	Deadzone = 16,
	Bloom = 17,  ///< memory: the input's own envelope biases the curve — thicker the harder you play
	Sag = 18,    ///< memory: the envelope lowers the rails — spongy, and it recovers
	Loop = 19,   ///< memory: what the curve took away, darkened and fed back into it
	Cheby = 20,  ///< table: crossfade toward T2 (left) or T3 (right) — a TUNED harmonic
	Stair = 21,  ///< table: quantise the curve's input into steps
	// **EACH EXTRA'S POWER — a second destination beside its amount (backlog IK, 2026-09-23).** Amount and
	// on/off were conflated under modulation, so each extra has two mod destinations. With 0 = off, a modulated amount on an extra that was off turned it ON, so
	// Off meant nothing under modulation. The engine hears `on ? amount : 0`: the amount is kept while
	// off, and a switch routed from a gate turns the extra on and off without touching its depth.
	SubOn = 22,
	FeedbackOn = 23,
	MangleOn = 24,
	LowpassOn = 25,
	IntegratorOn = 26,
	DeadzoneOn = 27,

	/// **COLOUR'S MODE — INFLATE OR FREE (backlog IW).** The dropdown is a mode selector, not a phase
	/// region selector, and Inflate comes first because Free is infinite while Inflate is fenced. Colour had one
	/// discrete sound: an Oxford-style inflator hard-wired into one band of the field on the gamma-zero line, found
	/// only by landing on it. Now it is a MODE: Free is the continuous field (`Colour`, unbounded), Inflate blends the
	/// field toward the inflator by `InflateAmount`. A stepped Choice, drawn as Colour's mode cell.
	ColourMode = 28,
	/// **HOW FAR INFLATE BLENDS THE FIELD INTO THE INFLATOR, 0..1** (a sweepable Inflate amount). 0 is the
	/// field at the current Colour, so switching into Inflate does not jump; 1 is the inflator the band used to be.
	/// Fenced: bounded, it cannot drag out of the mode. A MONO destination: a table term, like Knee and Colour (OM, 2026-10-31).
	InflateAmount = 29,

	/// **THE INPUT FILTERS AND THE CROSSFADE (backlog FA 2026-09-16: top-level input HPF and
	/// LPF controls and a normal crossfade mix knob).** A one-pole highpass and lowpass at fixed corners, after
	/// Trim and before everything else, so they are a tone control on the WHOLE device: the crossfade's dry is
	/// taken after them. Not the note-tracking `Lowpass` extra, which is the character mechanism's own. Each
	/// is off at its open end. MODULATABLE: a coefficient per block, no table term.
	InputHpf = 30,
	InputLpf = 31,
	/// THE DRY/WET CROSSFADE, 0 dry .. 1 wet, default 1 — see `dryWetMix`. Modulatable; it glides per sample on
	/// the threshold's half-life.
	DryWet = 32,

	/// **POLY — ONE SHAPER PER VOICE, OR ONE ON THE BUS (backlog OM).** Off by default, which is the device as it
	/// was, bit for bit: the host runs the bus instance and builds no voice instances (backlog NS). On: each voice
	/// runs its own instance before the voice's level, so a chord does not intermodulate and per-voice modulation
	/// (velocity, a voice's envelope) shapes each note on its own. Not a modulation destination: a modulatable Poly
	/// would keep 20 voice instances built for ever (`Converger::wantsVoices`).
	Poly = 33,

	kCount
};

/// **WHAT STILL CHANGES THE SOUND AT KNEE 0** — the one list the card and the window light by there. At Knee 0
/// `Shaper::processSample` consults no table, so everything the curve owns is out of the signal; what runs
/// ABOVE the bypass is not. Trim and the input filters run there (see `processSample`), and Knee is the way back
/// out. Trim was missing until backlog FA's follow-up: greyed at Knee 0 while it still moved what the clamp sees.
[[nodiscard]] constexpr bool actsAtKneeZero(Param p) noexcept {
	return p == Param::Knee || p == Param::Trim || p == Param::InputHpf || p == Param::InputLpf;
}

/// **COLOUR'S MODES, IN THE DROPDOWN'S ORDER (backlog IW).** inflate comes first because free is infinite —
/// every fenced mode is listed before Free, which is always last.
inline constexpr const char* kColourModeNames[] = {"Inflate", "Free"};
inline constexpr uint32_t kColourInflate = 0;
inline constexpr uint32_t kColourFree = 1;
/// THE GATE MASKS the two followers carry: Colour is live in Free, the Inflate amount in Inflate.
inline constexpr uint32_t kColourFreeMask = 1u << kColourFree;
inline constexpr uint32_t kColourInflateMask = 1u << kColourInflate;

/// The shaper's state — what a patch stores, in the device's own terms.
struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — see `kParamDescs`, and the static_assert after it that
	/// pins the two together.
	bool enabled = true;
	float knee = 0.5f;
	float colour = 0.0f;
	/// COLOUR'S MODE (backlog IW): Free reads the field at `colour`; Inflate blends it into the inflator.
	uint32_t colourMode = kColourFree;
	/// How far Inflate blends — 0 the field, 1 the inflator. See Param::InflateAmount.
	float inflateAmount = 1.0f;
	float gamma = 0.0f;
	/// COLOUR'S OWN phase offset, summed with `gamma` — see Param::ColourPhase.
	float drive = 0.0f; ///< 0 is the unit-slope read; the sweep is 4^drive. See shaper.cpp
	/// THE AMPLITUDE THRESHOLD, 0..1, 1 shapes everything — `Param::Threshold`, once called mix.
	float threshold = 1.0f;
	/// THE INPUT FILTERS' CORNERS IN log2(Hz), each at its open end, which is off — see `Param::InputHpf`.
	float inputHpf = static_cast<float>(kInputFilterMinLog2);
	float inputLpf = static_cast<float>(kInputFilterMaxLog2);
	/// THE DRY/WET CROSSFADE, 1 all wet — see `Param::DryWet`.
	float dryWet = 1.0f;
	/// THE TILT, in dB of high-frequency emphasis into the curve (lows get the opposite). See
	/// `Param::Emphasis`. 0 is flat.
	float emphasisDb = 0.0f;
	/// HYSTERESIS' FIELD BIAS — 0.5 is the field as the position authored it, 0 removes it. DEFAULT 0:
	/// see the descriptor row. See Param::Hysteresis.
	float hysteresis = 0.0f;
	/// **THE SIX EXTRAS ARE FIELD SCALERS, NOT SWITCHES (backlog II, 2026-09-23).** turn the
	/// extras buttons onto 0=off field scalers. Each scales its field by `fieldWithOverride` — the law
	/// Hysteresis already had: 0 is off, 0.5 is the field as the position authored it (bit for bit what
	/// the switch's On gave), 1 lifts it to the most the field can give. The field at zero switches
	/// every one of them off, MEASURED against the engine: sub depth, comb depth, bit count, deadzone
	/// width and the integrator's blend are all amounts, and Lowpass is gated on slew intensity > 0.
	/// The interval and the comb's stride are STRUCTURE, not amounts, and stay the field's.
	float sub = 0.5f;
	float feedback = 0.5f;
	/// THE BIT ROTATION — renamed from `rotate` when the PLANE's turn took that name. See
	/// `Param::Mangle`.
	float mangle = 0.5f;

	/// **THE PLANE'S TURN, 0..1.** Not the bit rotation above. It mutates the basis a pad travels in
	/// and reaches the audio path through nothing at all — see `Param::Rotate` and the case "Rotate
	/// alone leaves the shaper's phases standing".
	float rotate = 0.0f;

	/// THE INPUT TRIM IN DECIBELS, 0 = unity. See `Param::Trim`. Not in `sameCurveAs`: it is a gain
	/// on the input, not a term of the table.
	float trim = 0.0f;

	/// THE CROSSFADE'S WIDTH, 0..1, neutral at 0.5. See `Param::Aggression` and `aggressionFor`.
	///
	/// **NOT IN `sameCurveAs`, AND THAT IS THE POINT OF IT.** It reaches the sound through the
	/// staging, never through a table entry, so it costs no rebuild and may be modulated freely.
	float aggression = 0.5f;

	/// **THE PAD'S ACCUMULATED TRAVEL, one term per phase.** The only thing that moves the eight
	/// phases INDEPENDENTLY — colour, its phase and gamma are one direction between them. Lives past
	/// the declared params in the row; see `kTravelFirst`.
	std::array<float, phase_space::kPhaseCount> travel {};
	float lowpass = 0.5f;
	float integrator = 0.5f;
	/// THE DEADZONE'S SCALER — see `Param::Deadzone`. 0: `curveOf` zeroes the field's width.
	float deadzone = 0.5f;
	float bloom = 0.0f;
	float sag = 0.0f;
	float loop = 0.0f;
	float cheby = 0.0f; ///< -1..+1, see `Param::Cheby`
	float stair = 0.0f;
	/// EACH EXTRA'S POWER — see `Param::SubOn`. Off by default, with the amount at the field's own, so
	/// switching one on is exactly what the old switch's On was.
	bool subOn = false;
	bool feedbackOn = false;
	bool mangleOn = false;
	bool lowpassOn = false;
	bool integratorOn = false;
	bool deadzoneOn = false;
	/// PER VOICE OR ON THE BUS — see `Param::Poly`. The DSP never reads it: the HOST decides which instances run.
	bool poly = false;


	/// WHETHER THE TABLE IS EVEN CONSULTED. Knee at the bottom is a true bypass — not "a curve that
	/// happens to be straight", which would still cost a lookup and would still be one interpolation
	/// away from the identity.
	[[nodiscard]] bool linear() const { return knee < 0.001f; }

	/// WHAT THE TWO CURVE AXES ARE, AS ONE VALUE — the thing a rebuild is keyed on.
	///
	/// IT IS A METHOD AND NOT THREE COMPARISONS AT THE CALL SITE, because there is exactly one place
	/// that may decide a rebuild is needed and exactly one definition of what makes one needed. A
	/// fourth field joining the curve later moves this and nothing else; a call site comparing three
	/// members by hand would keep compiling and would stop rebuilding.
	/// NOT PART OF `sameCurveAs`, and that is a fact about where the term lives rather than an
	/// oversight: hysteresis is a per-sample table OFFSET, not a table entry, so this scaler reaches
	/// the sound through `beginBlock` on the next block and never asks for a rebuild.
	///
	/// **`travel` IS PART OF IT, AND THAT IS THE WHOLE POINT OF THE BLOB.** The pad's displacement is
	/// a TABLE TERM — it moves the eight phases the curve is built from — so a change to it must ask
	/// for a rebuild exactly as a colour change does. Left out, the blob reached `deriveCurve` and the
	/// table never noticed: the case "travel in the blob reaches the sound" caught precisely that, and
	/// it is the difference between a store that is read and a store that is heard.
	[[nodiscard]] bool sameCurveAs(const Settings& o) const {
		return knee == o.knee && colour == o.colour && gamma == o.gamma
		       && travel == o.travel && deadzone == o.deadzone && deadzoneOn == o.deadzoneOn
		       // COLOUR'S MODE AND INFLATE'S AMOUNT ARE TABLE TERMS (backlog IW): `applyInflate` moves the six weights.
		       && colourMode == o.colourMode && inflateAmount == o.inflateAmount;
	}

	/// FIELD-BY-FIELD, DEFAULTED. It exists so a card can ask "is the screen showing these settings"
	/// — `ShaperCard::update` compares what it drew against what it reads and asks for a frame only
	/// when they differ. `mod::dahdsr::Settings` and `mod::Playhead` carry the same defaulted
	/// comparison for the same reason.
	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings — what a patch stores and what the transport carries.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// **WHERE THE FOLDED TRAVEL SITS IN THE ROW** — immediately past the declared params, which is the
/// blob contract `device_row.h` states: `params.size() + extraState`, never more and never less. Here
/// rather than in `shaper_phase_space.h` because that header knows nothing of `Param` and must not:
/// this file includes IT, not the other way round.
inline constexpr size_t kTravelFirst = static_cast<size_t>(Param::kCount);

/// Decode a row, with ABSENT FIELDS AT THEIR DECLARED DEFAULTS rather than at zero. `fieldsPresent`
/// is how many of `row` are real; device_row.h's rule, and it matters here because zero is a
/// perfectly good value for every one of these controls and means something different for each.
[[nodiscard]] Settings stateOf(const Row& row, size_t fieldsPresent);
[[nodiscard]] Row rowOf(const Settings& settings);

inline constexpr const char* kOffOnNames[] = {"Off", "On"};


/// WHAT EACH ZONE OF A ZONED CONTROL IS — beside the control, not in the editor.
///
/// OWLTT's rule and OWLTT's reason: tooltips are meant to survive the device going standalone, and a
/// name list living in `shaper_expanded.cpp` is a list a standalone shaper does not have. It is also
/// a list that can describe the wrong control, which one sitting beside `{"Colour", ...}` cannot.
struct ZoneName {
	const char* name;
	const char* description;
};


/// **COLOUR HAS NO ZONE NAMES ANY MORE (backlog IW).** Its eight were MEASURED descriptions of a continuum (the record
/// is kept in the backlog's IW entry), and the one fixed character among them — Inflate — is a MODE now
/// (`Param::ColourMode`). `kColourZones` stays as Colour's drag unit.

/// Traits: what the shaper says about itself.
/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "distortion", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "SHPR",
	.name = "Shaper",
	.hasCard = true,
	.hasExpanded = true,
	// PER VOICE ON THE POLY SWITCH (backlog OM), off by default. See the header note for what it costs.
	.perVoiceControl = static_cast<int>(Param::Poly),
	// **EIGHT FLOATS OF FOLDED TRAVEL, one per phi phase** — the shaper had none, so unbounded travel
	// had nowhere to accumulate. Automod declares thirty-nine for the same reason. See
	// `shaper_phase_space.h`; the count is the device's N and nothing else may assume it.
	.extraState = phase_space::kPhaseCount,
	.extraStateKey = "travel",
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::DryWet),
	.mixInDevice = true,
};

inline constexpr device::ParamDesc kParamDescs[] = {
    // **ON BY DEFAULT.** Every effect defaults to on when it is added; the shaper was one that
    // defaulted to off. See `fx::filter::kParamDescs` for the full statement of the decision and
    // what it replaced.
    {"enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Bypass switch — off passes the signal through untouched"},

    // THE ZONE COUNT IS DECLARED, which is what makes a route's amount mean one zone rather than the
    // whole range (ParamDesc::zones) and what makes the ring on the face agree with the sound.
    // NEITHER OF THESE TWO IS MODULATABLE, so neither reader is exercised today — declared anyway,
    // because the count is also what tells the widget how many turns the knob has, and that IS
    // exercised. `kColourZoneNames` must stay this length; a case pins them together. KNEE IS NOT
    // HERE ANY MORE — see its declaration below for why a scale wants one rotation, not eight.
    // KNEE DECLARES NO ZONES, and the field is what made it a slow knob. Knee need not be a slow
    // knob; it is a normal range knob.
    //
    // `ParamDesc::zones` is not a label — `ZoneKnob` gives each zone a full rotation, so eight zones
    // is eight turns of travel for what is a plain drive control. The zones were never in the DSP
    // either: knee maps straight through to `drive`, and every quantity downstream is monotone in it.
    // MEASURED at the eight octile centres, THD of a sine at -10 dBFS: 3.3, 5.6, 9.3, 13.5, 17.1,
    // 19.8, 21.9, 23.3 percent — strictly increasing, no inversions. That is a SCALE, and a scale
    // wants one rotation.
    //
    // (The eight measured names that used to sit in `kKneeZoneNames` went with the field. They named
    // points on a continuum rather than places, and a names table nothing reads is a table that rots.
    // The ladder above is the part worth keeping and `test_warp`'s sibling case still pins it.)
    {"shaper_knee", "Shaper Knee", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "How hard — a monotone scale of saturation. 0 is a true bypass. Under Poly it is one value for every voice: it sets the curve they share"},
    // **UNBOUNDED, BY THE DECISION OF 2026-09-09 THAT NONE OF THESE IS BOUNDED.**
    //
    // It declared 0..1 and had a companion counter, `Shaper Colour Phase`, to carry laps into. That
    // pair is gone: the counter was summed into `gamma` unscaled, so it was measurably a SECOND GAMMA
    // KNOB rather than Colour's own phase — `colourPhase = 1` and `gamma = 1` produced byte-identical
    // curves. And the carry that fed it was incoherent: one lap of Colour is worth gamma 4 on the six
    // main bases and gamma 2.5 on asymmetry (measured, and it is asymmetry's own half-rate
    // acceleration), so no single carry value was right and +1 was neither.
    //
    // So the wall goes and the counter goes with it. Turning past the eighth zone now keeps turning
    // Colour, which was the intent in the first place — turning past the eighth zone keeps driving
    // the phase, with one control doing it, rather than two pretending to.
    //
    // **`Unit::Number` RATHER THAN `Percent`.** A percentage prints a hundred times its value, which
    // is a fine caption for a bounded 0..1 knob and a lie about an unbounded coordinate — and
    // `ValueBox::readoutTracksValue` declines digit-place dragging on any readout that is not the
    // value, so a Percent declaration here would withhold the gesture this control most needs.
    //
    // **`modulatable` STAYS FALSE.** The shaper's abstention is its own decision and is not what this
    // change is about; see the note that used to sit on the phase half.
    {"shaper_colour", "Shaper Colour", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, true, false,
     "What kind — the Free mode's field, blending six shaping curves, and it keeps sweeping past them."
     " Under Poly it is one value for every voice: it sets the curve they share",
     // FREE'S FOLLOWER (backlog IW): live when the mode is Free. `kColourZones` stays as its drag unit. A ROUTE'S
     // SPAN IS ONE LAP (1.0, the range it declared before it was unbounded), not the declared ±2^23 (OM, 2026-10-31).
     kColourZones, 1.0, {}, static_cast<int16_t>(Param::ColourMode), kColourFreeMask},
    // **UNBOUNDED, BY A STANDING DECISION** — shaper gamma conforms, because the field is infinite
    // too — and an installed build still showed gamma stuck at 0-100%. `gamma_box.h` was written in anticipation of exactly this edit and says so: its span
    // is `min(sixteen laps, the declared range)`, so widening here gives the box its sixteen with no
    // change there. The field has no end; the 0..1 was a drag window mistaken for a domain, which is
    // the same misreading that once cost a whole XY proposal.
    //
    // SYMMETRIC, for `Param::Rotate`'s reason: the default is 0 and a hand must be able to drag both
    // ways from where every patch starts.
    {"shaper_gamma", "Shaper Gamma", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, true, false,
     "The colour field's second axis. At 0 the named zones hold; off it the field is continuous. Under Poly it is one value for every voice: it sets the curve they share",
     0, 1.0}, // a route's span is one unit, as Colour's (OM, 2026-10-31)

    {"shaper_drive", "Shaper Drive", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "How far past the Threshold the curve is swept — more drive is more bend and more harmonics, "
     "never more level. What sits below the threshold passes untouched; what sits above never "
     "exceeds it, or 0 dBFS"},
    // **THRESHOLD, ONCE "MIX" (backlog FA).** The key is the old one on purpose: renamed, same meaning.
    {"shaper_mix", "Shaper Threshold", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "How much of the signal reaches the curve: it lowers an amplitude threshold, so a low setting "
     "shapes only the peaks. 100% shapes everything"},
    // INDEX 6, MATCHING `Param::Emphasis` — was `Harmonics`. CENTRED at 0 dB (flat); ±12 dB is highs
    // up and lows down by that much into the curve, and the inverse after it.
    {"shaper_emphasis", "Shaper Emphasis", -kEmphasisMaxDb, kEmphasisMaxDb, 0.0, false, device::Unit::Decibel, {}, true, false,
     "Which frequencies drive the curve. Up: the highs saturate first and their grit comes back darker "
     "(tape-like). Down: the lows drive it and the grit comes back bright and fizzy",
     0, 0.0, {}, -1, 0, true},

    // THE SIX EXTRAS ARE FIELD SCALERS (backlog II) — 0 off, 50% the field's own (what the old
    // switch's On gave, to the bit), 100% the most the field can give. See `Settings::sub`.
    // THE FIVE PER-SAMPLE ONES ARE MOD DESTINATIONS: the engine re-reads their amounts every block, so they cost
    // no rebuild — the property the declarations case asserts of every destination. Deadzone, Cheby and
    // Stair are TABLE terms and stay off, for the reason Knee and Colour are: a rebuild per change.
    {"shaper_sub", "Shaper Sub", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Sub-harmonic: flips the gain every few zero crossings. How much of the field's depth you hear — "
     "0 off, 50% the field's own. The interval is the field's"},
    {"shaper_feedback", "Shaper Feedback", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Comb feedback at the colour field's stride. How much of the field's depth you hear — 0 off, 50% "
     "the field's own"},
    {"shaper_mangle", "Shaper Mangle", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Rotates the bits of the sample word — aliasing artefacts, as rude as it sounds. Scales the "
     "field's bit count: 0 off, 50% the field's own"},
    {"shaper_lowpass", "Shaper Lowpass", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "One-pole lowpass ahead of the curve, tracking the note, so only the low harmonics are shaped. "
     "Scales the field's slew: 0 off, 50% the field's own"},
    {"shaper_integrator", "Shaper Integrator", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Accumulates between zero crossings for a triangle-ish shape. Runs after Lowpass. Scales the "
     "field's slew: 0 off, 50% the field's own"},

    // INDEX 13, MATCHING `Param::Hysteresis`. This array is index-aligned with the enum and a
    // static_assert pins only its LENGTH, so an entry in the wrong slot compiles and silently
    // describes a different control — which is exactly what the first draft of this did, putting it
    // at 12 and giving Hysteresis ColourPhase's default of 0 instead of the field-neutral 0.5.
    // **DEFAULT 0, NOT THE FIELD-NEUTRAL 0.5**, to match the reference. The firmware's own default path leaves hysteresis off, and at
    // the default position it was twenty of 26.5 points of THD (measured, see the enum). 0.5 is still
    // "the position's own" for a hand that wants it.
    {"shaper_hysteresis", "Shaper Hysteresis", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,
     "How much of the field's hysteresis you hear — 0.5 is the position's own, 0 removes it"},

    // INDEX 14, MATCHING `Param::Rotate` — the PLANE's turn. Not modulatable: a modulated basis makes
    // one gesture mean different things from block to block, which is what this holds still.
    // **UNBOUNDED, AND `Unit::Number` RATHER THAN `Percent`.** It was stuck at a finite 0-100 range;
    // it is made unbounded the way Feathers' Explore makes its rotate unbounded.
    //
    // MEASURED, and it is why the wall was real rather than cosmetic. `planeTurnRate` spaces the
    // coordinates on {0.618, 0.382, 0.236, 0.146, 0.090}, so across the WHOLE old 0..1 range the
    // fastest coordinate advanced 62% of one triangle period and the slowest 9% — not one full turn
    // on any of them. The angular EXTENT does saturate early (24.2 degrees by rotate 1, 28.6 by
    // rotate 100), but the count of distinct planes does not: **11 distinct x-directions at 5-degree
    // resolution over 0..1, 121 over 0..11, 960 over 0..100.** Turning further does not aim further
    // away; it keeps finding DIFFERENT orientations at a similar distance, which is exactly Explore's
    // reading of its own rotate — *"a re-aiming rather than an amount, so it has no ends"*.
    //
    // **THE RANGE IS A DRAG WINDOW, NOT A DOMAIN**, the same idiom automod's fines and gamma use, and
    // it is SYMMETRIC because a turn has two directions and the default sits at 0. Declared 0..N the
    // way gamma is, the very first backwards drag would hit a wall at the place every patch starts.
    //
    // Its seating owns the sensitivity: `GammaBox` gives a drag sixteen units whatever the range, so
    // widening this does not make the control coarser. A plain `Knob` would have — `Knob::dragTo`
    // maps one 140px drag across the whole declared range, so a wide range on a knob is only a
    // coarser knob.
    {"shaper_rotate", "Shaper Rotate", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, false, false,
     "Turns the plane the pad travels in. Changes what a move MEANS, never the sound at rest"},

    // INDEX 15, MATCHING `Param::Aggression`. **NEUTRAL AT 0.5, WHICH IS TODAY'S SOUND EXACTLY** —
    // `aggressionFor(0.5) == 1.0`, so a device that never touches this knob blends as it always has.
    // That is the right default for a control being ADDED rather than a compatibility concession:
    // the knob's job is to depart from the current crossfade, so the current crossfade is where it
    // rests. **MODULATABLE**, and see the enum for why it is the only free one here.
    {"shaper_aggression", "Shaper Aggression", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "How abruptly the shaped path takes over past the Threshold — left is a wider, softer "
     "crossfade, right is a snap"},

    // INDEX 16, MATCHING `Param::Trim`. **DECIBELS, AS DECIBELS** (backlog DL, 2026-09-16): this carried
    // `Unit::Number` under an apology — "there is no dB unit and inventing one for a single control would
    // be the larger change" — and printed "+6.00" with the unit left to the tooltip. Bus Utility brought
    // `Unit::Decibel` (DK) and the box prints "+6.0dB" for it; the stored float is unchanged, only the
    // reading. **DEFAULT 0 dB IS UNITY AND IS THE ONLY HONEST DEFAULT** — a trim that is not at unity
    // when untouched is a gain hiding in a device that already has two.
    //
    // +-24 dB: enough to bring a quiet source up to where the curve bends and to back a hot one off
    // the ceiling, which is the whole job. MODULATABLE — see the enum.
    {"shaper_trim", "Shaper Trim", -24.0, 24.0, 0.0, false, device::Unit::Decibel, {}, true, false,
     "Input trim in dB, ahead of everything — sets how hard the signal drives the curve"},
    // INDEX 16, MATCHING `Param::Deadzone`. OFF BY DEFAULT — see the enum for why.
    {"shaper_deadzone", "Shaper Deadzone", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Collapses part of the curve to near-silence — gritty, gate-like crossover. Scales the field's "
     "width: 0 off and the curve is whole, 50% the field's own. The centre is the field's. Under Poly it is one value for every voice: it sets the curve they share"},
    {"shaper_bloom", "Shaper Bloom", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Memory: the harder and longer you play, the further the curve is pushed off centre, so loud "
     "passages thicken with even harmonics and relax as you back off — a tube's bias shift"},
    {"shaper_sag", "Shaper Sag", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Memory: loud playing lowers the curve's ceiling, so it clips earlier and quieter, then recovers — "
     "a power supply dipping under load. Quiet playing passes as before"},
    {"shaper_loop", "Shaper Loop", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Memory: while you play, what the curve fails to pass is darkened and fed back into it, so the "
     "distortion feeds on itself — thicker and stickier — and lets go when you stop. Wild on the folders"},
    {"shaper_cheby", "Shaper Cheby", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A tuned harmonic. Left fades the curve toward Chebyshev T2 — a full-scale sine becomes its "
     "octave, and it is the curve's one even-harmonic road. Right fades toward T3, the twelfth",
     0, 0.0, {}, -1, 0, true},
    {"shaper_stair", "Shaper Stair", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Quantises the curve's input into steps: lo-fi, bit-crushed grit. Turning up makes the steps "
     "coarser, from 32 to 2; quiet signal rounds to silence the way a crusher's does"},
    // THE SIX POWERS (backlog IK) — each extra's on/off, a destination of its own beside its amount so a
    // modulated amount cannot switch an extra on. A two-state switch is a well-defined destination: a
    // full-depth route moves it across its whole range (SKLITTER's Engage is the precedent). Deadzone's
    // is a TABLE term, like its amount, so it is a MONO destination (OM, 2026-10-31).
    {"shaper_sub_on", "Shaper Sub On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Sub-harmonic on or off. Its depth is the Sub amount, kept while it is off"},
    {"shaper_feedback_on", "Shaper Feedback On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Comb feedback on or off. Its depth is the Feedback amount, kept while it is off"},
    {"shaper_mangle_on", "Shaper Mangle On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Bit rotation on or off. Its bit count is the Mangle amount, kept while it is off"},
    {"shaper_lowpass_on", "Shaper Lowpass On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "The lowpass ahead of the curve, on or off. Its amount is kept while it is off"},
    {"shaper_integrator_on", "Shaper Integrator On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "The integrator, on or off. Its amount is kept while it is off"},
    {"shaper_deadzone_on", "Shaper Deadzone On", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "The deadzone, on or off. Its width is the Deadzone amount, kept while it is off. Under Poly it is one value for every voice: it sets the curve they share"},
    // **COLOUR'S MODE, AND INFLATE'S AMOUNT (backlog IW).** The mode is a stepped Choice drawn as Colour's mode cell;
    // Colour follows it in Free, the amount in Inflate. Defaults: Free, so a fresh shaper is the field it always was;
    // and a FULL amount, so the first pick of Inflate is the inflator the band used to be.
    {.key = "shaper_colour_mode",
     .name = "Shaper Colour Mode",
     .min = 0.0,
     .max = 1.0,
     .def = static_cast<double>(kColourFree),
     .stepped = true,
     .unit = device::Unit::Choice,
     .names = kColourModeNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Inflate blends the curve toward a loudness-inflation curve; Free is the continuous Colour field."
                " Under Poly it is one value for every voice: it sets the curve they share",
     .shortName = "Colour",
     .modeCell = true},
    {.key = "shaper_inflate",
     .name = "Shaper Inflate",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = device::Unit::Percent,
     .names = {},
     // A DESTINATION SINCE 2026-10-31 (only the parameters that dont cost a bake shold be poly modulable. the
     // others can be in mono): a table term, so it is mono under Poly, and a rebuild is rate-limited by the build.
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the curve is blended into the inflation curve: 0 is the Colour field, 100% full inflation."
                " Under Poly it is one value for every voice: it sets the curve they share",
     .gateControl = static_cast<int16_t>(Param::ColourMode),
     .gateMask = kColourInflateMask},
    // **THE INPUT FILTERS AND THE CROSSFADE (backlog FA).** Defaults are the open ends — off — and full wet, so a
    // fresh shaper is the device it was before these existed, bit for bit.
    {.key = "shaper_input_hpf",
     .name = "Shaper HPF",
     .min = kInputFilterMinLog2,
     .max = kInputFilterMaxLog2,
     .def = kInputFilterMinLog2,
     .stepped = false,
     .unit = device::Unit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Highpass on the device's input, before the curve and before the Mix's dry — keeps the low end "
                "from driving the curve. Off at 20 Hz",
     .shortName = "HPF",
     .offAtEnd = -1},
    {.key = "shaper_input_lpf",
     .name = "Shaper LPF",
     .min = kInputFilterMinLog2,
     .max = kInputFilterMaxLog2,
     .def = kInputFilterMaxLog2,
     .stepped = false,
     .unit = device::Unit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Lowpass on the device's input, before the curve and before the Mix's dry — a fixed corner, "
                "unlike the Lowpass extra, which tracks the note. Off at 20 kHz",
     .shortName = "LPF",
     .offAtEnd = +1},
    {.key = "shaper_dry_wet",
     .name = "Shaper Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = device::Unit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry/wet: a crossfade from the filtered input (0) to the shaped output (100%). The dry is not "
                "limited, so below 100% a hot input passes above 0 dBFS",
     .shortName = "Mix"},
    {.key = "shaper_poly",
     .name = "Shaper Poly",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = device::Unit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: one shaper per voice, before the voice's level — each note keeps its own state and a chord "
                "does not intermodulate, and per-voice modulation of Drive, Threshold, the extras and Mix moves each note "
                "on its own. The curve itself (Knee, Colour, Gamma, the pad, Deadzone, Inflate) is ONE table for every "
                "voice, set by the device. Off: one shaper on the summed bus",
     .shortName = "Poly"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// WHETHER AN INPUT FILTER IS IN (backlog FA): off at its open end, by `device::atOffEnd` on its own descriptor —
/// the one test the readout's "off" also makes, so the engine and the box cannot disagree about where Off begins.
[[nodiscard]] inline bool inputHighpassOn(float log2Hz) {
	return !device::atOffEnd(kParamDescs[static_cast<size_t>(Param::InputHpf)], static_cast<double>(log2Hz));
}
[[nodiscard]] inline bool inputLowpassOn(float log2Hz) {
	return !device::atOffEnd(kParamDescs[static_cast<size_t>(Param::InputLpf)], static_cast<double>(log2Hz));
}

/// THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER — see `fx::owltt` for the full statement of why a
/// descriptor default and a struct initialiser that disagree give a device two different fresh
/// states.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

/// THE FIVE EXTRAS, IN DECLARATION ORDER — one table, read by every surface that draws them.
///
/// IT LIVES BESIDE THE CONTROLS AND NOT IN AN EDITOR, for `kColourZoneNames`' reason: a list of this
/// device's controls that sits in one surface's file is a list the OTHER surface has to copy, and a
/// copy is a thing that can come to name a control it is not labelled with. The panel drew these
/// from `ShaperExpanded::kExtraParams`; when the card gained them too, that array would have become
/// the second copy. It is here instead.
inline constexpr Param kExtraSwitches[] = {
    Param::Sub, Param::Feedback, Param::Mangle, Param::Lowpass, Param::Integrator, Param::Deadzone,
};
inline constexpr size_t kExtraSwitchCount = std::size(kExtraSwitches);
/// **EACH EXTRA'S POWER, INDEX-ALIGNED WITH `kExtraSwitches`** (backlog IK) — one pill drives both: its
/// click is the power, its drag the amount.
inline constexpr Param kExtraPowers[] = {
    Param::SubOn, Param::FeedbackOn, Param::MangleOn, Param::LowpassOn, Param::IntegratorOn, Param::DeadzoneOn,
};
static_assert(std::size(kExtraPowers) == kExtraSwitchCount);

/// **THE CURVE THESE SETTINGS BUILD — THE ONE CALL THE ENGINE AND THE PLOT BOTH MAKE.** `deriveCurve`
/// plus the switches that gate a table term, so the drawn curve and the heard one cannot disagree
/// about whether the deadzone is in: a gate written at each call site is a gate one of them forgets.
[[nodiscard]] inline CurveParams curveOf(const Settings& s) {
	CurveParams p = deriveCurve(s.knee, s.colour, s.gamma, kSlewHarmonics, &s.travel);
	// COLOUR'S INFLATE MODE (backlog IW): the field, blended toward the inflator by the amount — here, in the one call
	// the engine and the plot both make, so the heard curve and the drawn one cannot disagree about the mode.
	if (s.colourMode == kColourInflate) {
		applyInflate(p, s.inflateAmount);
	}
	// THE SIX FIELD SCALERS (backlog II), each by `fieldWithOverride` — the one law Hysteresis already
	// uses: 0 off, 0.5 the field bit for bit, 1 the most it gives. Comb depth and rotation scale the
	// field's TRIANGLE and are re-mapped by the firmware's own mappings, so their top is the mapping's.
	// GATED BY EACH EXTRA'S POWER (backlog IK): off is off whatever the amount, or its modulation, says.
	const auto gated = [](bool on, float amount) { return on ? amount : 0.0f; };
	p.deadzoneWidth = fieldWithOverride(p.deadzoneWidth, gated(s.deadzoneOn, s.deadzone));
	p.subIntensity = fieldWithOverride(p.subIntensity, gated(s.subOn, s.sub));
	p.feedback = owlet::dsp::phi::feedbackFromTriangle(fieldWithOverride(p.extrasField[2], gated(s.feedbackOn, s.feedback)));
	p.rotation = owlet::dsp::phi::rotationFromTriangle(fieldWithOverride(p.extrasField[3], gated(s.mangleOn, s.mangle)));
	p.lowpassAmount = fieldWithOverride(p.slewIntensity, gated(s.lowpassOn, s.lowpass));
	p.integratorAmount = fieldWithOverride(p.slewIntensity, gated(s.integratorOn, s.integrator));
	p.chebyMix = s.cheby;
	p.stairSteps = stairStepsFor(s.stair);
	return p;
}

/// A CONTROL'S NAME WITHOUT THE DEVICE'S — "Shaper Knee" becomes "Knee".
///
/// **THE DEVICE QUALIFIES ITS NAMES BECAUSE A HOST'S PARAMETER LIST IS FLAT; A SURFACE THAT ALREADY
/// SAYS WHICH DEVICE IT IS SHOULD PRINT THE TAIL.** Both of this device's surfaces wanted that and
/// both had written it: `shaper_expanded.cpp` as a `shortLabelOf`, `shaper_card.cpp` open-coded with
/// a bare `label + 7` beside its own `rfind`. Two copies of one rule, one of them carrying a magic
/// number that only agrees with the prefix by inspection.
///
/// SO IT IS HERE, WHERE THE PREFIX IS. `kTraits.name` is what the descriptions are qualified WITH, so
/// the length that gets skipped is derived from it rather than typed — a device renamed from "Shaper"
/// to anything else moves this with it, which `label + 7` could not do.
[[nodiscard]] inline const char* shortLabelOf(Param p) {
	const char* name = kParamDescs[static_cast<size_t>(p)].name;
	const std::string_view prefix { kTraits.name };
	const std::string_view full { name };
	return (full.size() > prefix.size() + 1 && full.rfind(prefix, 0) == 0
	        && full[prefix.size()] == ' ')
	           ? name + prefix.size() + 1
	           : name;
}

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in shaper.cpp, where the DSP class is.
extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::DriveColour,
};

} // namespace feathers::fx::shaper
