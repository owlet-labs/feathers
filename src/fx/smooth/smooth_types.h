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

#include "device/registry.h"
#include "fx/fx_tenant.h"

#include <array>
#include <cstddef>
#include <cstdint>

//--------------------------------------------------------------------------------------------------//
// THE SMOOTH SHAPER — the reference's sine shaper, ported.                                              //
//--------------------------------------------------------------------------------------------------//
//
// Named "Smooth Shaper" in the plugin. The port minds the gain staging differences between the
// hardware and a DAW, which matter a lot, especially for the Chebyshev modes.
//
// THE REFERENCE is our firmware fork, branch `owlet-firmware-1.3` at `2567b04c`:
// `dsp/sine_shaper.{hpp,cpp}`, its menu in `gui/menu_item/fx/sine_shaper.h`, and its callers
// in `voice.cpp`. Those files were the instrument this port was tested against when it was ported.
// "Sine shaper" is said only where code cites the firmware.
//
// **WHAT IT IS: EIGHT WAVESHAPERS BEHIND ONE KNOB, AND FOUR MODIFIERS BEHIND ANOTHER.** Harmonic picks
// the algorithm — two blends of normalised Chebyshev polynomials (clean, and through a sine first),
// five four-way blends of FM-style operators (FM, Fold, Ring, Add, Mod), and a cascaded polynomial that
// is MEANT to wrap in 32-bit arithmetic (Poly) — and where inside it you stand moves a blend of four
// weights along incommensurate phi triangles. Four PILLS add stereo motion, even harmonics, rectification
// and feedback, any of them together; Twist sweeps all four at once along phi triangles.
//
// ---------------------------------------------------------------------------------------------------
// THE GAIN STAGING, WHICH IS THE PART THAT MATTERS
// ---------------------------------------------------------------------------------------------------
//
// **1.0 HERE IS THE REFERENCE'S 0 dBFS = 2^24 = ONE_Q31 / 128** (the Eroder's convention, `kHeadroom`). A
// full-level hardware voice enters the shaper at 2^24; a full-level Feathers voice enters the lane at
// about 1.0 (MEASURED, design doc: 0.6-1.3 peak at default knobs), so the map puts one where the other
// sits and no extra constant is needed. The shaper then has its OWN ceilings, and they are kept:
//   - Zones 0/1 (the Chebyshev blends) saturate their input at 0.5 (-6.02 dB), the exact-harmonics
//     point: `shiftSaturate(driven, 8)`. At Drive centre a full voice enters at x = 2,
//     flat-topped BY DESIGN — the firmware's own comment says the harsh edge past it is the signal that
//     you have overdriven the polynomial.
//   - Zone 7 (Poly) saturates at 1.0 (0 dB): `shiftSaturate(driven, 7)`.
//   - The drive clamps at the q31 ceiling, 128.0 here, which nothing reaches.
// Chebyshev wet is `P(x) · 0.027 · 0.707 · ONE_Q31`, which is 2.443·P(x) in these units: +13.8 dB over
// the dry below the clip. That is the firmware's level, not a mistake, and it is kept.
//
// ---------------------------------------------------------------------------------------------------
// WHAT IS NOT THE FIRMWARE'S, EACH DELIBERATE (the design's departures table)
// ---------------------------------------------------------------------------------------------------
//
//   - **TIME CONSTANTS FOLLOW THE HOST RATE.** The firmware is 44.1 kHz by construction: the drive and
//     coefficient smoothing alphas, the 100 Hz wet highpass's alpha, the feedback tap's one-pole and the
//     stereo LFO's `kSampleRate`. Each is restated from its 44.1 kHz value HERE so that at 44.1 kHz the
//     number is the firmware's to the bit and at 96 kHz the SOUND is.
//   - **ONE SMOOTHER PER INSTANCE.** The firmware's coefficient smoother lives on the Sound and is
//     shared by every voice's call, so it runs faster the more voices sound — the sound would depend on
//     voice count. Here each voice has its own, and a new note's smoother starts AT its target (the
//     state the shared one is in whenever the patch is held still) rather than gliding up from the
//     firmware's power-on {1, 0, 0, 0, 0}, which would be 5 ms of clean fundamental on every note.
//   - **THE PER-BUFFER SETUP RUNS ON A FIXED 128-FRAME GRID CARRIED ACROSS CALLS** (`kBlockFrames`) —
//     the zone, the targets, the convergence test, the stereo LFO and the feedback amount. The firmware
//     does it once per buffer, whose length is the host's in a plugin; the reference's is 128. The 4-frame
//     coefficient stride runs inside it, as the firmware's does. So the output cannot depend on how the
//     host cuts its blocks, and at 44.1 kHz with 128-frame calls it is the firmware's frame for frame.
//   - **UNDEFINED BEHAVIOUR IS MADE DEFINED.** Float → q31 conversions saturate (ARM `vcvt`), and the
//     wraps the firmware relies on — Zone 7's polynomial above all — are done in `uint32_t`.
//   - **HARMONIC IS A MODE AND AN UNBOUNDED COORDINATE** (within a zone the coordinate drives phi
//     triangles, so there is no reason to clip it at 100%). The Mode
//     menu is the firmware's zone; the coordinate is where inside it, and it keeps turning. ONE LAP IS
//     +1.0 ON `harmonicPhaseOffset` — the unit the firmware's own Twist encoder wraps in
//     (`zone_based.h`'s auto-wrap) — so the firmware is handed `posInZone = frac(h)` and
//     `harmonicPhaseOffset = floor(h) + 0.125 · H Phase`, every such pair is a state the firmware can be
//     in, and with H Phase at zero the blend bank's phase IS h, so the phi triangles run on seamlessly
//     past a lap. NOT seamless everywhere: the FM zones' input gain `1 + 3·pos` and Poly's cascade blend
//     `pos` saw back at each lap, as the firmware's do whenever H Phase pushes the position across its
//     wrap; the coefficient smoother glides the step. The firmware's clip-to-zone on a modulated
//     Harmonic existed so a route could not cross into another algorithm; the Mode menu now holds that
//     line, so a route here crosses laps and never modes. BELOW ZERO the firmware never goes (its
//     phases are clamped at zero), and its `fmod` would hand back a negative position there, so the
//     port wraps by floor (`wrapUnit`) — identical at and above zero, and a declared departure below.
//   - **TWIST SPLITS INTO FOUR PILLS AND ONE INFINITE SWEEP**. The firmware's Twist is one
//     knob over eight zones — Width, Evens, Rect, Fdbk, then four of a phi sweep of all four — so it applies
//     ONE effect at a time. Here each of the four is its own FaderPill (an amount and a power, `Pill`), and
//     they sound TOGETHER, which the firmware cannot do. The pills control the amount and the extras still
//     come from the harmonic phase: a pill sets its effect's depth (width, evens, rect, a quarter of feedback),
//     and what the firmware varied with the in-zone position — Width's LFO speed-up and phase, Evens'
//     flavour (positive half compressed, or negative half blended to dry) — is read from the EXTRAS PHASE,
//     `wrap(harmonic position + harmonicPhaseOffset + wrap(γ · φ))` (`twistExtrasPhaseOf`). Each pill is a
//     firmware zone at a position (`pillQ31Of`) with its depth swapped in, so ONE pill on, the sweep off,
//     at the depth the firmware's own zone has at that position, is the firmware's zone to the bit; any
//     other depth, or several pills, is a declared departure. TWIST is the sweep alone, unbounded, behind
//     its own power (the sweep at its origin is not silent): `t = lap + pos`, and with `lap + 1024·γ` zero
//     the firmware's meta zones at `pos` (zones 4-7), otherwise its phi path at `pos` with
//     `twistPhaseOffset = lap` — never a named zone. Pills ADD to the sweep, clamped to the firmware's
//     ranges (`twistOf`).
//   - **MIX DEFAULTS TO FULL**, the firmware's 127. Its default is 0 because there every effect is always
//     in the chain and zero is how it is off; here adding the device is the gesture. Full is 127/128
//     wet, as the firmware's `uint8 << 24` is — never quite all wet.
//   - **POLY: PER VOICE BY DEFAULT**, the reference's position — after the oscillators, before the amp
//     envelope, so the timbre holds through a note and a chord does not intermodulate. Off is a bus
//     insert, which is the firmware's clip path.
//   - **SILENCE REACHES EXACT ZERO** — the plugin sleeps only on an exactly-silent block. The wet highpass's
//     floored q31 update has a dead band a negative state never leaves, which would hold a ~-100 dB constant on
//     the output for ever; after 50 ms of exactly-zero input, with both states inside that band, they are
//     cleared (the REST, `SmoothShaper::renderFrames`). Unreachable with a signal present.
//   - **A STACKED VOICE IS TRIMMED TO A SINGLE VOICE'S LOUDNESS** (law (b); `stackTrimOf`,
//     `kStackCoherenceSeconds`). Voice stacking sums a voice's parts before the per-voice devices; coherent parts
//     would drive the shaper up to +9 dB harder. Per voice only, exactly 1 at a stack of one.
//   - **THE MONO ROAD IS THE STEREO ROAD'S LEFT** — the house contract, so a
//     voice's timbre cannot change when the character bank's stereo window opens or shuts. The firmware's
//     mono overload (no stereo LFO) is kept in the reference only.
//   - **NOT PORTED: `filterGain` / `hasFilters`.** Dead in the firmware — every caller passes 0.
//   - **THE FIRMWARE'S STEREO TARGET CACHE IS FIXED** (fix it). Its key omitted the LFO's
//     position offset, so after position stereo's positive half the two sides stayed a little apart until a
//     control moved. The targets are computed afresh every block; against the reference the port is exact
//     everywhere but the stereo passages with Width live, where the difference is bounded at -34 dB.

namespace feathers::fx::smooth {

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S NUMBERS.                                                                            //
//--------------------------------------------------------------------------------------------------//

/// THE RATE EVERY FIRMWARE TIME CONSTANT BELOW WAS WRITTEN AT.
inline constexpr double kReferenceRate = 44100.0;

/// THE REFERENCE'S 0 dBFS, `EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`: 1.0 here is 2^24 there.
inline constexpr float kQ31PerUnit = 16777216.0f;

inline constexpr int32_t kOneQ31 = 2147483647;
inline constexpr int32_t kZones = 8;
/// `computeZoneWidth(8)`: one zone of a 0..ONE_Q31 knob.
inline constexpr int32_t kZoneWidthQ31 = kOneQ31 / kZones;

/// `kSmoothingStride`: the coefficients move every fourth frame.
inline constexpr int kSmoothingStride = 4;
/// `kPerSampleAlpha` at 44.1 kHz — the drive's per-frame IIR, and a quarter of the coefficients' strided one.
inline constexpr double kPerSampleAlphaAtReference = 0.005;
/// `kSmoothingConvergenceEpsilon`.
inline constexpr float kConvergenceEpsilon = 1e-6f;
/// `kOutputHpfAlpha = 0.01425 · ONE_Q31` at 44.1 kHz — 2π·100/44100, a 100 Hz highpass on the wet.
inline constexpr double kOutputHpfAlphaAtReference = 0.01425;
/// `fbState = (fbState + hpfWet) >> 1` — a one-pole whose pole is 0.5 at 44.1 kHz (~4.9 kHz).
inline constexpr double kFeedbackPoleAtReference = 0.5;

/// THE GRID THE PER-BUFFER SETUP RUNS ON — the reference's buffer, as a fixed length. See the header.
inline constexpr int kBlockFrames = 128;

/// THE REST (`SmoothShaper::renderFrames`): how long the input must be EXACTLY zero before the integer states,
/// if they are inside the wet highpass's dead band, are cleared — CLIP's `kRestSilentSeconds`.
inline constexpr double kRestSilentSeconds = 0.05;

/// **THE STACK TRIM'S COHERENCE TIME — FITTED, NOT DERIVED** (law (b), chosen 2026-09-26). A stacked voice's
/// parts are summed before the per-voice devices at 1/√N each, which holds the loudness of N DECORRELATED parts; at
/// small detune they are still coherent and sum to √N louder. The trim estimates that coherence from the beat rate of
/// the outermost pair, ρ = exp(−Δf · τ). τ is FITTED to V5 (the default engine) over 63 measured cells — N 2/4/8 ×
/// detune 0–50 c × keys 36/60/84 — at a ONE-SECOND horizon after the note starts, with an RMS residual of 0.56 dB
/// (`test_fx_smooth.cpp` pins the table). A shorter horizon would want a longer τ; an engine
/// that decorrelates its parts differently would want a refit, and `test_fx_smooth.cpp` pins it to the table.
inline constexpr double kStackCoherenceSeconds = 8.1;

/// THE GAIN THAT MAKES A STACKED VOICE ENTER THE SHAPER AT A SINGLE VOICE'S LOUDNESS: 1 / √(1 + (N − 1) ρ), with
/// ρ = exp(−Δf · τ) and Δf = f · (2^(detune / 1200) − 1) the outermost pair's beat. Exactly 1 for a stack of one;
/// 1 / √N at 0 cents; toward 1 as the parts decorrelate. It does not flatten a detuned stack's onset peak (every part
/// starts in phase) — that is the stack's own crest, and the reference's too.
///
/// **AND ρ STARTS FROM THE PARTS' OWN 0-CENT COHERENCE** : ρ = ρ0 · exp(−Δf · τ), ρ0 =
/// `NoteContext::stackCoherence`. Parts on stands of their own (a Pos route, Width) play different tables, which at
/// 0 cents already sum as little as decorrelated ones, so law (b) with ρ0 = 1 cut them by up to 7.9 dB below their
/// stands' level. ρ0 is 1 for a plain stack and changes nothing there, to the bit. The trim brings a stack to the
/// parts' POWER AVERAGE — the stands' own loudness is the sound of the stand and is left alone.
[[nodiscard]] float stackTrimOf(int stackCount, float detuneCents, float pitchSemitones, float coherence = 1.0f) noexcept;

/// The stereo LFO: 0.05..3 Hz before Twist's frequency multiplier; `kStereoDriveScale`.
inline constexpr float kMinLfoHz = 0.05f;
inline constexpr float kMaxLfoHz = 3.0f;
inline constexpr float kStereoDriveScale = 0.25f;

/// THE DRIVE FADER — the firmware's FULL law, not the voice path's ±2^30 window: its
/// `computeDriveGain` squares the q31 into 0..4x, gain = 4 · ((d + 1) / 2)², and the clip path takes all of it.
/// With the fader's value v = (d + 1) / 2 that is 4v² — silent at 0, unity at 0.5, +12 dB at 1, the `Fader`
/// unit's own square taper. So q31 = (2v − 1) · 2^31, saturated one short of the top as the firmware's is.
inline constexpr double kDriveQ31Full = 2147483648.0;

/// The menu's steps for the two phase offsets that remain controls, per click: the unit each counts in.
/// (The third, T Phase at 128 per click, is 128 laps of Twist and was folded into it — see the header.)
inline constexpr float kHarmonicPhasePerClick = 0.125f; ///< `harmonicPhaseOffset += offset * 0.125`
inline constexpr float kGammaPerClick = 1.0f;           ///< `gammaPhase += offset`
/// WHAT ONE LAP OF A COORDINATE ADDS TO ITS FIRMWARE PHASE OFFSET — the auto-wrap's `phaseOffset ± 1.0`.
inline constexpr float kPhasePerLap = 1.0f;
/// What a full-depth route moves the sweep by: a quarter lap, one meta zone of the firmware's four.
inline constexpr double kSweepModSpan = 0.25;
/// THE DRAG WINDOW. Not a bound — a phase keeps turning; the eroder's and shaper's idiom.
inline constexpr double kPhaseMax = 8388608.0;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

enum class Param : int {
	Enabled,
	Poly,          ///< per voice (the reference's place) or on the bus
	Drive,         ///< a fader, 0..1: 4v² into the shaper — silent, unity at 0.5, 4x at the top
	HarmonicMode,  ///< which of the eight waveshapers — the firmware's Harmonic ZONE
	Harmonic,      ///< where inside it, unbounded: a lap is 1.0 (the whole part adds to `harmonicPhaseOffset`)
	Sweep,         ///< Twist: the phi sweep of all four, unbounded, a lap is 1.0 — heard while SweepOn
	Mix,           ///< wet/dry, 0..127/128 wet
	HarmonicPhase, ///< `harmonicPhaseOffset`, in the menu's clicks of 0.125 — eight of them are a lap of Harmonic
	Gamma,         ///< `gammaPhase`, in clicks of 1: 1024 laps of Twist
	SweepOn,
	// THE PILLS (`Pill`): amounts first, then the powers — the Disperser's and the Shaper's order.
	Width,
	Evens,
	Rect,
	Fdbk,
	WidthOn,
	EvensOn,
	RectOn,
	FdbkOn,

	kCount,
};

/// THE FOUR TWISTS, EACH ITS OWN PILL — the firmware's first four Twist zones, in its order.
enum class Pill : int { Width, Evens, Rect, Fdbk, kCount };
inline constexpr size_t kPillCount = static_cast<size_t>(Pill::kCount);
inline constexpr const char* kPillNames[] = { "Width", "Evens", "Rect", "Fdbk" };
[[nodiscard]] constexpr Param pillAmountParam(Pill p) noexcept {
	return static_cast<Param>(static_cast<int>(Param::Width) + static_cast<int>(p));
}
[[nodiscard]] constexpr Param pillOnParam(Pill p) noexcept {
	return static_cast<Param>(static_cast<int>(Param::WidthOn) + static_cast<int>(p));
}

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE FIRMWARE'S ZONE NAMES (`SineShaperHarmonic::getZoneName`), and each is an algorithm in the code —
/// a branch, not a description of a region — so the name cannot be wrong for part of its zone.
inline constexpr const char* kHarmonicModeNames[] = { "3579", "3579wm", "FM", "Fold", "Ring", "Add", "Mod", "Poly" };

/// THE ZONES THE CODE BRANCHES ON, 0..7 in `sineShapeCore`.
enum class HarmonicMode : int {
	Cheby = 0,     ///< "3579": Chebyshev T3/T5/T7/T9, raw input
	ChebyWarm = 1, ///< "3579wm": the same through sin(x · 3π/4) first
	Fm = 2,
	Fold = 3,
	Ring = 4,
	Add = 5,
	Mod = 6,
	Poly = 7,      ///< the cascaded polynomial, which wraps
};

inline constexpr const char* kTags[] = { "distortion", "waveshaper", "fx" };

/// THE FACE, THREE ACROSS: the firmware's four knobs in its menu order with the mode beside Harmonic, then
/// the two phase offsets left standing — which on the hardware are not menu items at all but push-and-twist
/// gestures on Harmonic and Mix, so they come last. (T Phase is Twist's laps now.) `Poly` STACKS under `Mode` (the Delay's Dub under its Mode):
/// two short controls in one knob-height cell rather than a switch stretched to a knob's height.
///
/// **THREE, NOT FOUR — PHOTOGRAPHED AT FOUR AND IT DID NOT FIT**, the Eroder's finding again: at a quarter
/// of the card "HARMONIC" clipped to "HARM...", Twist's zone read "‹Widtl›", and the three phase boxes ran
/// their captions into their numbers ("H Phas0.00").
///
///   Drive     Mode/Poly   Harmonic
///   Twist     Mix         H Phase
///   Gamma
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::Drive),         static_cast<uint8_t>(Param::HarmonicMode),
	static_cast<uint8_t>(Param::Poly),          static_cast<uint8_t>(Param::Harmonic),
	static_cast<uint8_t>(Param::Sweep),         static_cast<uint8_t>(Param::Mix),
	static_cast<uint8_t>(Param::HarmonicPhase), static_cast<uint8_t>(Param::Gamma),
	static_cast<uint8_t>(Param::SweepOn),       static_cast<uint8_t>(Param::Width),
	static_cast<uint8_t>(Param::WidthOn),       static_cast<uint8_t>(Param::Evens),
	static_cast<uint8_t>(Param::EvensOn),       static_cast<uint8_t>(Param::Rect),
	static_cast<uint8_t>(Param::RectOn),        static_cast<uint8_t>(Param::Fdbk),
	static_cast<uint8_t>(Param::FdbkOn),
};

/// A STORED NUMBER CHANGED MEANING, SO ITS KEY WENT WITH IT (the JF rule).
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "smooth_harmonic", "2026-09-26",
	  "a 0..1 position clamped to the mode became an unbounded coordinate (smooth_harmonic_coord)" },
	{ "smooth_twist", "2026-09-26",
	  "a 0..1 knob became an unbounded coordinate whose laps are the firmware's twistPhaseOffset (smooth_twist_coord)" },
	{ "smooth_twist_phase", "2026-09-26", "folded into Twist: one click of 128 is 128 laps of smooth_twist_coord" },
	{ "smooth_twist_coord", "2026-10-03",
	  "Twist split into four pills (smooth_width/evens/rect/fdbk) and the phi sweep alone (smooth_sweep)" },
	{ "smooth_drive", "2026-09-26",
	  "a -1..1 knob over the voice path's 0.25..2.25x became a 0..1 fader over the full 0..4x law (smooth_drive_fader)" },
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "smth",
	.name = "Smooth Shaper",
	.hasCard = true,
	// A HAND-WRITTEN CARD AND A PANEL (`smooth_card.h`, `smooth_expanded.h`) — bonus points for some
	// kind of visualizer. The panel holds every control; the card, all but the three phases.
	.hasExpanded = true,
	// **PER VOICE BY DEFAULT — the Filter's and HARM's road.** The lane holds one instance per voice
	// while Poly is on and runs each on its own voice before the voice's level.
	.perVoiceControl = static_cast<int>(Param::Poly),
	.extraState = 0,
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = -1,
	.faceControls = 0,
	.faceOrder = kFaceOrder,
	.faceColumns = 3,
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

/// EVERY ROW BY FIELD NAME — device.h records the positional-init hazard twice over.
inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "smooth_enabled", .name = "Smooth Shaper Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the smooth shaper from the chain"},
    {.key = "smooth_poly", .name = "Smooth Shaper Poly",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: one shaper per voice, before the voice's level — each note keeps its "
                "own timbre and a chord does not intermodulate. Off: one shaper on the summed bus",
     .stacked = true},
    {.key = "smooth_drive_fader", .name = "Smooth Shaper Drive",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Fader,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Gain into the shaper: silent at the bottom, unity at the centre, +12 dB at the top — or drag the "
                "ceiling on the curve. A full voice already overdrives 3579 at unity; turn down for clean harmonics",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false},
    {.key = "smooth_harmonic_mode", .name = "Smooth Shaper Harmonic Mode",
     .min = 0.0,
     .max = static_cast<double>(kZones - 1),
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kHarmonicModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Which waveshaper. 3579: odd harmonics from Chebyshev polynomials; 3579wm: the same, warmer. "
                "FM, Fold, Ring, Add, Mod: four blends of an operator each. Poly: a folding polynomial",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Mode"},
    // ONE LAP PER 1.0, IN EIGHTHS — H Phase's step, so eight regions a lap — and none of them named (the
    // waveshaper's name is the Mode menu's), so the cell shows the lap's sigil. A full-depth route moves one
    // whole lap (`modSpan`), the firmware's "full modulation = 1 zone".
    {.key = "smooth_harmonic_coord", .name = "Smooth Shaper Harmonic",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Where inside the waveshaper: moves a blend of its four harmonics or operators, each on its "
                "own slow triangle. It keeps turning — each whole number is another lap of the same mode. "
                "Grab a digit to drag by that place",
     .zones = static_cast<uint32_t>(kZones),
     .modSpan = 1.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Harmonic"},
    // TWIST, THE SWEEP ALONE — the firmware's meta zones and phi laps, never a named zone (see the header).
    {.key = "smooth_sweep", .name = "Smooth Shaper Twist",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Sweeps width, evens, rect and feedback together, each on its own phi triangle. It keeps "
                "turning: every lap is a sweep of its own. Heard while its power is on; the pills add to it",
     .zones = 4, // the firmware's four meta zones, unnamed: the cell marks the lap, never a dropdown
     .modSpan = kSweepModSpan,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Twist"},
    {.key = "smooth_mix", .name = "Smooth Shaper Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry to shaped. The shaped signal is not level-matched: the harmonic modes run well above the "
                "dry"},
    {.key = "smooth_harmonic_phase", .name = "Smooth Shaper Harmonic Phase",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Shifts where inside the mode you stand against its blend, an eighth per step, and wraps. "
                "Eight steps are one lap of Harmonic. In Poly it sets the self-multiplication blend",
     .shortName = "H Phase"},
    {.key = "smooth_gamma", .name = "Smooth Shaper Gamma",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "The same as turning Twist 1024 laps: a different sweep of all four twists, and of the "
                "harmonic blend, at every step"},
    {.key = "smooth_sweep_on", .name = "Smooth Shaper Twist On",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Twist's power: on, the sweep of all four is heard under the pills"},
    {.key = "smooth_width", .name = "Smooth Shaper Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Stereo width moved by a slow LFO. Its speed and phase come from Harmonic's position. Heard while its power is on"},
    {.key = "smooth_evens", .name = "Smooth Shaper Evens",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Even harmonics. Harmonic's position picks the flavour: the positive half compressed, or the negative half blended toward the dry. Heard while its power is on"},
    {.key = "smooth_rect", .name = "Smooth Shaper Rect",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Toward the rectified wave. Heard while its power is on"},
    {.key = "smooth_fdbk", .name = "Smooth Shaper Fdbk",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The wet fed back into the input, up to a quarter. In Fold and Mod it can sustain itself after the input stops. Heard while its power is on"},
    {.key = "smooth_width_on", .name = "Smooth Shaper Width On",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Width's power: on, the pill's amount is heard"},
    {.key = "smooth_evens_on", .name = "Smooth Shaper Evens On",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Evens's power: on, the pill's amount is heard"},
    {.key = "smooth_rect_on", .name = "Smooth Shaper Rect On",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Rect's power: on, the pill's amount is heard"},
    {.key = "smooth_fdbk_on", .name = "Smooth Shaper Fdbk On",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Fdbk's power: on, the pill's amount is heard"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

struct Settings {
	bool enabled = true;
	bool poly = true;
	float drive = 0.5f; ///< the fader: unity
	int harmonicMode = 0;
	float harmonic = 0.0f; ///< unbounded; a lap is 1.0
	float sweep = 0.0f;    ///< Twist, unbounded; a lap is 1.0
	bool sweepOn = false;
	std::array<float, kPillCount> pillAmount { 0.5f, 0.5f, 0.5f, 0.5f };
	std::array<bool, kPillCount> pillOn {};
	float mix = 1.0f;
	float harmonicPhase = 0.0f;
	float gamma = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);
float valueOf(const State& state, Param param);
void setValueOf(State* state, Param param, float value);

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS IN THE FIRMWARE'S UNITS — each a pure function, so a test can compare it with the     //
// reference's own. Out here rather than in the .cpp for the usual reason: they are values.           //
//--------------------------------------------------------------------------------------------------//

/// THE SETTINGS AS `processSineShaper` RECEIVES THEM: q31 knobs and the three raw phase floats.
struct FirmwareControls {
	int32_t drive = 0;
	int32_t harmonic = 0;  ///< the mode's zone plus `frac(Harmonic)` inside it
	int32_t twist = 0;     ///< the SWEEP as the firmware's knob: in zones 4-7 on its named lap, the whole knob off it
	int32_t mix = 0;       ///< `mix << 24`, continuous here
	float twistPhaseOffset = 0.0f;    ///< `floor(sweep)`: the laps, as the encoder's auto-wrap counts them
	bool sweepOn = false;
	std::array<float, kPillCount> pills {}; ///< each pill's amount while on, else 0
	float harmonicPhaseOffset = 0.0f; ///< `floor(Harmonic) + 0.125 · H Phase`
	float gammaPhase = 0.0f;
};
[[nodiscard]] FirmwareControls firmwareControlsOf(const Settings& s) noexcept;

/// THE LAP A COORDINATE IS ON, AND WHERE INSIDE IT: `floor(x)` and `x - floor(x)` in [0, 1), with a rounding
/// guard so the position can never read 1. Shared by the device, the picture and the cards.
struct Lap {
	float whole = 0.0f;
	float position = 0.0f;
};
[[nodiscard]] Lap lapOf(float coordinate) noexcept;

/// `fmod(x, 1)` FOR x ≥ 0, BIT FOR BIT, and a floor-wrap into [0, 1) below zero, where the firmware's `fmod`
/// would hand back a negative position (it never gets there: its phases are clamped at zero). See the header.
[[nodiscard]] float wrapUnit(float x) noexcept;

/// `computeZoneQ31` over eight zones: the index and the position inside the zone.
struct Zone {
	int32_t index = 0;
	float position = 0.0f;
};
[[nodiscard]] Zone zoneOf(int32_t harmonicQ31) noexcept;

/// `computeDriveGain`: the q31 drive squared into 0..4x.
[[nodiscard]] float driveGainOf(int32_t driveQ31) noexcept;

/// `SineShaperTwistParams`, field for field.
struct TwistParams {
	float stereoWidth = 0.0f;
	float stereoFreqMult = 1.0f;
	float stereoPhaseOffset = 0.0f;
	float stereoLfoRate = 0.0f;
	float evenAmount = 0.0f;
	float evenDryBlend = 0.0f;
	float rectAmount = 0.0f;
	float feedbackAmount = 0.0f;
	float phaseHarmonic = 0.0f;
	float phaseHarmonicFreqMod = 0.0f;
};
/// `computeSineShaperTwistParams` — Twist's eight zones, or the phi sweep when a phase is off zero.
[[nodiscard]] TwistParams twistParamsOf(int32_t twistQ31, float twistPhaseOffset, float harmonicPhaseOffset,
                                        float gammaPhase) noexcept;

/// THE EXTRAS PHASE: where the pills read their character — `wrap(harmonic position + harmonicPhaseOffset +
/// wrap(γ · φ))`, the position the blend reads, with Gamma folded in on a phi multiple (see the header).
[[nodiscard]] float twistExtrasPhaseOf(const FirmwareControls& fc) noexcept;
/// THE FIRMWARE TWIST KNOB A PILL STANDS FOR: Width and Evens at the extras phase in their zones; Rect at half
/// its amount (the triangle's rising side) and Fdbk at its amount, in theirs.
[[nodiscard]] int32_t pillQ31Of(Pill pill, float amount, float extrasPhase) noexcept;
/// THE TWIST THE SHAPER HEARS: the sweep (or nothing), plus every pill that is on, clamped to the firmware's
/// ranges. One pill alone, sweep off, at its zone's own depth, is `twistParamsOf(pillQ31Of(...))` exactly.
[[nodiscard]] TwistParams twistOf(const FirmwareControls& fc) noexcept;

/// `computeBlendWeights4`: four weights, log-scaled phi triangles, normalised to sum 1.
[[nodiscard]] std::array<float, 4> blendWeightsOf(float posInZone, double gammaPhase) noexcept;

/// THE STEREO LFO'S SHAPE: `triangleFloat(phase, 0.5)` — bipolar, and silent for half of each cycle, so it
/// spends more of its time near zero. Positive: the two sides at different POSITIONS; negative: the right
/// side at LESS DRIVE.
[[nodiscard]] float stereoLfoValueOf(float phase) noexcept;
/// WHERE A NEW VOICE'S LFO STARTS: `wrapPhase(phaseHarmonic · φ)`, so each Twist position gives each note
/// its own starting phase.
[[nodiscard]] float stereoLfoSeedOf(float phaseHarmonic) noexcept;

/// THE FIVE COEFFICIENTS EVERY ZONE CARRIES IN THE SAME STRUCT, each zone reading them its own way —
/// `ShaperWeights`, with its q31 twins for the FM zones.
///   Zones 0/1: c1..c9 of P(x) = c1·x + c3·x³ + c5·x⁵ + c7·x⁷ + c9·x⁹.
///   Zones 2-6: c1 = the input gain (1..4), c3/c5/c7/c9 = the four operators' weights.
///   Zone 7:    c1 = the cascade blend, c3 = the self-multiplication blend.
struct Coefficients {
	float c1 = 0.0f;
	float c3 = 0.0f;
	float c5 = 0.0f;
	float c7 = 0.0f;
	float c9 = 0.0f;
	int32_t c1q = 0;
	int32_t c3q = 0;
	int32_t c5q = 0;
	int32_t c7q = 0;
	int32_t c9q = 0;
	/// `updateQ31`, with the conversions saturating (see the header).
	void updateQ31() noexcept;
};
/// `computeShaperWeightsFromPos` → `weightsToCoeffs`: the Chebyshev blend at a position.
[[nodiscard]] Coefficients chebyshevCoefficientsOf(float posInZone, double gammaPhase) noexcept;

/// THE NINE TARGETS A BLOCK SMOOTHS TOWARD — c1, c3..c9 left, c3..c9 right — as `sineShapeBuffer` picks them:
/// per zone, from the position in the zone, Twist's harmonic phase and, in stereo, the LFO's position offset
/// (`stereoMod`, 0 when position stereo is off). ONE FUNCTION FOR THE AUDIO AND THE PICTURE: `beginBlock`
/// calls it, and so does `pictureOf`.
using CoefficientTargets = std::array<float, 9>;
[[nodiscard]] CoefficientTargets targetsOf(int zone, float posInZone, const TwistParams& twist, float stereoMod) noexcept;

//--------------------------------------------------------------------------------------------------//
// THE PICTURE — what the card and the panel draw, computed through the SAME core the audio runs.     //
//--------------------------------------------------------------------------------------------------//

/// THE SHAPER, STANDING STILL: these settings resolved exactly as a block's setup resolves them, with the
/// coefficients at their targets (a smoother that has arrived) and the stereo LFO at rest. What it leaves
/// out is what cannot be drawn as one curve: the feedback loop, the 100 Hz highpass on the wet, and Mix.
struct Picture {
	int zone = 0;
	float driveGain = 1.0f;
	Coefficients weights {};
	TwistParams twist {};
	/// WHERE THE ZONE'S INPUT SATURATES, as an INPUT level (1.0 = the reference's 0 dBFS): 0.5 / drive in the
	/// Chebyshev zones, 1 / drive in Poly. In the FM zones nothing saturates — the driven input is a phase —
	/// so this is where the driven signal reaches 0 dBFS, drawn as a reference and not as a clip.
	float ceilingInput = 0.5f;
	bool clips = true;
};
[[nodiscard]] Picture pictureOf(const Settings& settings) noexcept;

/// THE WET SHAPER'S OUTPUT FOR ONE INPUT LEVEL, both in units of the reference's 0 dBFS — `shapeSample` itself.
[[nodiscard]] float transferAt(const Picture& picture, float input) noexcept;

/// THE DRIVE THAT PUTS THE CEILING AT `ceilingInput` — the inverse of `Picture::ceilingInput`, clamped to the
/// fader's 0..4x, and returned as the Drive FADER's value (0..1). What dragging the ceiling writes. A ceiling at
/// infinity (or not a number) is the fader at 0.
[[nodiscard]] float driveKnobForCeiling(int zone, float ceilingInput) noexcept;
/// AND FORWARD: the ceiling (an input level) that the Drive FADER value `driveKnob` (0..1) puts it at, by the
/// shaper's own gain law — what the modulated ceiling and the handle's drag anchor are drawn from. At a gain of
/// zero NOTHING reaches the shaper and the ceiling is +infinity: off-scale, never a division by zero.
[[nodiscard]] float ceilingInputOf(int zone, float driveKnob) noexcept;
/// `ceilingReferenceOf(zone) / gain`, +infinity at a gain of zero. The one place the division is made.
[[nodiscard]] float ceilingOfGain(int zone, float gain) noexcept;

/// THE HARMONICS: one cycle of a sine at `level` (1.0 = the reference's 0 dBFS) through `transferAt`,
/// `kHarmonicSamples` points, and the magnitudes of H1..H9 by DFT, in the same units.
inline constexpr int kHarmonicSamples = 256;
inline constexpr int kHarmonicCount = 9;
[[nodiscard]] std::array<float, kHarmonicCount> harmonicsOf(const Picture& picture, float level) noexcept;

//--------------------------------------------------------------------------------------------------//
// WHAT THE DEVICE PUBLISHES.                                                                         //
//--------------------------------------------------------------------------------------------------//

/// THE READINGS, IN THIS ORDER. Only the BUS instance has a readings road (`RackEngine` installs the sink on
/// the slot's instance, not on its voices), so with Poly on there is nothing to publish and the card says so.
enum class Reading : size_t {
	/// LINEAR peak of the incoming signal, both channels, held 50 ms — where the signal sits against the ceiling.
	InPeak,
	kCount,
};

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S PRIMITIVES, AS THE FIRMWARE HAS THEM.                                               //
//--------------------------------------------------------------------------------------------------//

/// `getSine`: the 256-point `sineWaveSmall` table with 16-bit linear interpolation, peak ≈ ONE_Q31.
/// NOT `std::sin` — the table's interpolation error is part of the firmware's colour (design).
[[nodiscard]] int32_t tableSine(uint32_t phase) noexcept;
/// Poly's folding polynomial, applied twice, in wrapping int32 fixed point.
[[nodiscard]] int32_t cascadePolynomial(int32_t x) noexcept;
/// Saturate x to the range of a (32 - shift)-bit signed integer, then shift it up by `shift`.
[[nodiscard]] int32_t shiftSaturate(int32_t x, int shift) noexcept;
/// A float to q31, toward zero, SATURATING, 0 for NaN — the reference's `vcvt.s32.f32`.
[[nodiscard]] int32_t saturatingQ31(float x) noexcept;

/// WHAT ONE SAMPLE'S SHAPER SAW — written by `shapeSample` when asked, because the gain staging is a claim
/// about values (x = 2 into the Chebyshevs at Drive centre) and a rendered buffer cannot answer it.
struct ShapeProbe {
	int32_t driven = 0;    ///< the input times the drive, clamped to the q31 range
	int32_t zoneInput = 0; ///< what the zone's shaper was handed: after the `<< 8` / `<< 7` saturation in
	                       ///< zones 0, 1 and 7, and `driven` itself in the FM zones
};

/// `sineShapeCore`: one sample, one channel, wet only. `driven` is made inside from `input · driveGain`.
[[nodiscard]] int32_t shapeSample(int32_t input, float driveGain, int zone, const Coefficients& w, float evenAmount,
                                  float evenDryBlend, float rectAmount, ShapeProbe* probe = nullptr) noexcept;

extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::DriveColour,
};

} // namespace feathers::fx::smooth
