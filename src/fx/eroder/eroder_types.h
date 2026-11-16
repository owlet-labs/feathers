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

#include "dsp/analyser_bins.h"
#include "device/registry.h"
#include "fx/fx_tenant.h"

#include <array>
#include <cstddef>
#include <cstdint>

//--------------------------------------------------------------------------------------------------//
// THE ERODER — the reference's, ported.                                                                 //
//--------------------------------------------------------------------------------------------------//
//
// The firmware's eroder, as faithful as possible, with bus pitch tracking.
//
// THE REFERENCE is our firmware fork, branch `owlet-firmware-1.3` at `2567b04c`:
// `dsp/eroder.{h,cpp}`, its menu in `gui/menu_item/fx/eroder.h`, and its two callers
// (`sound.cpp`, `global_effectable_for_clip.cpp`). A read-only clone sits in
// `worktrees/.eroder-reference/`, gitignored, with its provenance beside it.
//
// **WHAT IT IS: A VERY SHORT COMB WHOSE READ HEAD IS SHAKEN BY FILTERED NOISE.** A feedback delay of
// 1.5 to 5.8 ms; white noise, blended with a sample-and-hold that re-draws on zero crossings, goes
// through a resonant state-variable filter; its BANDPASS — plus an optional pitched triangle — moves
// the delay time by up to half of itself, each channel separately. The wet is soft-clipped, folded,
// highpassed and ADDED to a dry that always passes whole.
//
// **FOUR CONTROLS DRIVE ELEVEN QUANTITIES.** `Tone` and `Character` are each a phase into a bank of
// phi triangles — six and five of them, incommensurate, so turning one knob sweeps its quantities in
// and out against each other rather than in step. That is the instrument; a port that exposed the
// eleven would be a different device. `derive()` below is that mapping as a pure function, which is
// what lets every one of the eleven be ASSERTED rather than listened for.
//
// **THE CALLERS WERE READ AS WELL AS THE PARAMS STRUCT, because automod's port was not and missed a
// control** (`automod_types.h`, "FREQ — THE ONE CONTROL THE PORT MISSED": it lived in the firmware's
// param system, invisible from the struct). The eroder has one of those too — `GLOBAL_ERODER_CUTOFF`
// reaches the DSP only as `processEroder`'s `cutoffValue` argument — and that is all of them: Tone,
// Character, Cutoff, Mix, the two phase offsets and the shared Gamma.
//
// ---------------------------------------------------------------------------------------------------
// WHAT IS NOT THE FIRMWARE'S, EACH DELIBERATE
// ---------------------------------------------------------------------------------------------------
//
//   - **SECONDS AND HERTZ, NOT SAMPLES AND COEFFICIENTS.** The firmware is 44.1 kHz by construction:
//     a 256-sample line, `/ 44100.0f` in the triangle and the highpass, and the SVF's corner given
//     as a raw coefficient (0.15). Every one is restated in physical units HERE, from the 44.1 kHz
//     figure, so that at 44.1 kHz the numbers are the firmware's and at 96 kHz the SOUND is.
//   - **FLOAT, WITH THE SATURATION KEPT WHERE IT IS CHARACTER.** The inner loop is Q31 with
//     `add_saturate` at six sums. Five are inside the wet path and are kept as clamps to ±1 — a comb
//     near unity feedback LIVES on that ceiling. The sixth is the output sum, `dry + wet`, and is
//     NOT kept: on the hardware everything is below full scale already, and here it would make this
//     device hard-clip a hot dry signal it otherwise passes untouched.
//   - **`2^x` IS EXACT.** `fastPow2` is a speed approximation of it, not a voicing.
//   - **THE NOISE IS OWNED AND RE-SEEDED BY `reset`.** `getNoise()` is one global generator; the
//     registry's reset and partition sweeps need the same samples in to be the same samples out.
//   - **NO PHI-CACHE THROTTLE.** The firmware re-evaluates the banks every second buffer to save
//     cycles. Here they are evaluated when the phase moves.
//   - **THE SMOOTHING IS A TIME, AND THE FIRST BLOCK SNAPS.** The firmware's is 5% PER BUFFER, which
//     is not a time constant at all — it depends on the host's block length — and it glides up from
//     zero at power-on. See `kSmoothingSeconds`.
//   - **ONE UNBOUNDED COORDINATE PER ZONED KNOB**, not a 0..1 knob plus a phase offset. The firmware
//     sums them before anything reads either (`freqNorm + effectiveFreq()`), so they were always one
//     quantity; this is automod's collapse (backlog BJ) and the standing design that none of
//     these is bounded. The zones are LABELS and a modulation scale and nothing else — the
//     DSP reads the phase as a continuum — so a Tone of 1.3 is the firmware's knob at 0.3 with a
//     phase offset of 1.
//   - **`Mix` DEFAULTS TO HALF.** The firmware's is 0, because there every effect is always in the
//     chain and zero is how one is off. Here adding the device is the gesture, and the design is
//     that an effect does something the moment it is added.

namespace feathers::fx::eroder {

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S NUMBERS, IN PHYSICAL UNITS.                                                         //
//--------------------------------------------------------------------------------------------------//

/// THE RATE EVERY FIRMWARE CONSTANT BELOW WAS WRITTEN AT.
inline constexpr double kReferenceRate = 44100.0;

/// **THE REFERENCE'S 0 dBFS IS 1/128 OF THE CEILING, AND THE FIRST RELEASE OF THIS PORT MISSED IT.**
/// `fixedpoint.h`: `EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`. A hardware signal at full scale is 2^24, 42 dB
/// under the q31 ceiling — so the soft clip's knee (2/3 of the ceiling), the fold's period and every
/// `add_saturate` sit far above anything a Sound normally hands the eroder. There the two shapers are
/// LINEAR: a gain of `drive`, which the wet compensation's `/ drive` cancels.
///
/// The port first ran its wet path at full scale, where 1.0 IS the ceiling: the shapers crushed the
/// signal and the compensation divided it down again. the effect is
/// nonexistent most settings, broadly much less intense than on [the hardware]. MEASURED against a q31
/// transcription of the firmware's loop at the firmware's own level (`test_fx_eroder.cpp`): the wet
/// was 15 dB low at the default coordinate. automod met the same factor and made it a knob
/// (`EnvTrim`); here it is the ENTIRE difference between a working port and an inaudible one, and it
/// is not a matter of taste, so it is a constant.
inline constexpr double kHeadroom = 128.0;

/// `kEroderMaxDelay = 256` samples — 5.8 ms. The comb's lowest fundamental, 172 Hz.
inline constexpr double kLineSeconds = 256.0 / kReferenceRate;
/// `kMinDelay = kEroderMaxDelay * 0.25` — 1.45 ms, 689 Hz. Where the BASE delay may sit.
inline constexpr double kMinBaseDelaySeconds = 64.0 / kReferenceRate;
inline constexpr double kMaxBaseDelaySeconds = 255.0 / kReferenceRate;
/// Where the MODULATED delay may reach: `minDelayQ16 = 1 << 16`, one sample.
inline constexpr double kMinDelaySeconds = 1.0 / kReferenceRate;

/// `std::max(0.05f, freqResults[0])` and `[1]` — the floors that keep a dead zone of the bank from
/// being a dead device: the delay never quite reaches its minimum and the comb is never quite open.
inline constexpr double kBankFloor = 0.05;
/// `* 0.95f` — "capped for stability".
inline constexpr double kFeedbackCeiling = 0.95;
/// `std::max(0.15f, charResults[0])` — depth's own, larger, floor.
inline constexpr double kDepthFloor = 0.15;
/// `charResults[1] * 0.99f`; the SVF's damping is one minus this.
inline constexpr double kResonanceCeiling = 0.99;

/// `modScaleQ = baseDelay * 32768` against a Q16.16 delay: full-scale modulation moves the read head
/// by HALF the base delay.
inline constexpr double kModExcursion = 0.5;

/// `clipDrive = 1 + clip * 4`, `foldDrive = 1 + fold * 3`.
inline constexpr double kClipDriveSpan = 4.0;
inline constexpr double kFoldDriveSpan = 3.0;
/// `kKnee = 2/3`, and above it `>> 2` — a quarter slope.
inline constexpr float kClipKnee = 2.0f / 3.0f;
inline constexpr float kClipSlopeAboveKnee = 0.25f;

/// `100.0f * fastPow2(3.0f * freqResults[4])` — 100 Hz to 800 Hz before pitch and Cutoff.
inline constexpr double kHpfBaseHz = 100.0;
inline constexpr double kHpfOctaves = 3.0;
/// `std::min(6.28 * hpf / 44100, 0.5f)`.
inline constexpr double kHpfAlphaMax = 0.5;

/// `261.626f` — middle C, the pitched triangle's frequency at note 60 with no offset.
inline constexpr double kTriangleBaseHz = 261.626;
/// `charResults[4] * 4.0f` — the bipolar bank entry, as octaves.
inline constexpr double kTriangleOffsetOctaves = 4.0;
/// `std::min(step, 0.9f)`, per 44.1 kHz modulation tick (the firmware's sample), as a fraction of full scale.
inline constexpr double kTriangleStepMax = 0.9;

/// **THE SVF'S CORNER. `kCutoffReference = 0.15f` IS A COEFFICIENT, NOT A FREQUENCY** — the
/// Chamberlin `f = 2 sin(pi * fc / rate)`. Inverted at 44.1 kHz it is 1053.9 Hz, and so are its two
/// clamps: 0.01 is 70.2 Hz and 0.85 is 6162 Hz. `derive()` works in hertz and converts back AT 44.1 kHz
/// at every host rate, because the SVF runs on the 44.1 kHz modulation clock (`Eroder::tickPhase_`, NX-D1).
inline constexpr double kSvfReferenceCoefficient = 0.15;
inline constexpr double kSvfCoefficientMin = 0.01;
inline constexpr double kSvfCoefficientMax = 0.85;

/// `(noteCode - 60) / 12` — middle C is where pitch tracking does nothing. `NoteContext`'s own
/// default is 60, so a device that has never been told a note behaves as the firmware's does with
/// `noteCode = -1`.
inline constexpr double kReferenceNote = 60.0;

/// `knobOctaves = cutoffValue / 0x40000000` over a q31 — two octaves each way — and then
/// `std::clamp(fastPow2(knobOctaves), 0.125f, 8.0f)`, a clamp at THREE that the knob alone can never
/// reach. Kept, because a modulated Cutoff is the one thing that could.
inline constexpr double kCutoffOctaves = 2.0;
inline constexpr double kCutoffRatioMin = 0.125;
inline constexpr double kCutoffRatioMax = 8.0;

/// `1024.0 * gammaPhase` — Gamma is Tone's and Character's phase, a thousand times coarser.
inline constexpr double kGammaScale = 1024.0;

/// **THE FIRMWARE SMOOTHS 5% PER BUFFER, WHICH IS NOT A TIME.** `kEroderSmoothingAlpha = 0.05` is
/// applied once per `processEroder` call, so the glide's length is the host's block length times
/// twenty — a different device in every DAW and at every buffer setting. The reference renders up to
/// 128 frames at a time, which puts its own glide near 58 ms; 50 ms is that, stated as what it is.
/// REASONED, NOT MEASURED ON HARDWARE.
inline constexpr double kSmoothingSeconds = 0.05;

/// HOW OFTEN THE CONTROL PATH RUNS, in frames, on a grid the device carries across calls. 32 is the
/// registry's common grid (the filter's and drawn's), and `test_fx_devices.cpp` warms every device
/// for a whole number of them.
inline constexpr int kControlFrames = 32;

inline constexpr uint32_t kZones = 8;
/// One zone of an eight-zone coordinate, in the coordinate's own units — what a full-depth route
/// moves it by. The firmware's `combinePresetAndCables`: "full modulation = 1 zone".
inline constexpr double kOneZone = 1.0 / static_cast<double>(kZones);
/// THE DRAG WINDOW. Not a bound — see `shaper_types.h` on Colour and Gamma.
inline constexpr double kPhaseMax = 8388608.0;

//--------------------------------------------------------------------------------------------------//
// DUST'S NUMBERS — the second engine (`dust.h`). Ours, chosen; none is a port of anything.           //
//--------------------------------------------------------------------------------------------------//

/// AMOUNT IS HOW FAR THE READ HEAD TRAVELS EITHER SIDE OF ITS CENTRE, AS A TIME, log2 seconds. The bottom of the
/// range is "no travel at all" (the LogSeconds convention); the top is 2^-9 s, just under 2 ms.
inline constexpr double kDustAmountLog2Min = -18.0;
inline constexpr double kDustAmountLog2Max = -9.0;
inline constexpr double kDustAmountLog2Default = -13.0; // 122 microseconds
inline constexpr double kDustAmountMaxSeconds = 1.0 / 512.0;

/// FREQ IS THE CENTRE OF THE BAND THE SHAKING SITS IN (and the sine's frequency), log2 hertz: 20 Hz to 18 kHz.
inline constexpr double kDustFreqLog2Min = 4.321928094887362;
inline constexpr double kDustFreqLog2Max = 14.135709286104401;
inline constexpr double kDustFreqLog2Default = 11.965784284662087; // 4 kHz
/// Where a tracked or modulated Freq is held: above anything that is not a tremor, below the rate's own ceiling.
inline constexpr double kDustFreqFloorHz = 5.0;
inline constexpr double kDustFreqCeilingHz = 20000.0;
inline constexpr double kDustFreqNyquistFraction = 0.45;

/// WIDTH RUNS THE BAND'S Q from narrow (a wavering tone) to wide (hiss).
inline constexpr double kDustQNarrow = 16.0;
inline constexpr double kDustQWide = 0.5;

/// THE BAND NOISE'S RMS AT THE HEAD, whatever the Freq, the Width and the rate. Under one so its peaks mostly fit
/// the clamp at full travel; a sine, at the other end of Source, is 0.707.
inline constexpr double kDustNoiseRms = 0.45;
/// The variance of the generator's uniform draw on [-1, 1).
inline constexpr double kDustWhiteVariance = 1.0 / 3.0;

/// THE HEAD NEVER COMES CLOSER TO THE WRITE THAN THIS: the four-point interpolation reads two frames ahead of it.
inline constexpr double kDustMarginFrames = 2.0;

/// Resonance at either end is this much feedback round the line.
inline constexpr double kDustFeedbackCeiling = 0.9;
/// What the line may hold, in full scales: far above any signal, and a ceiling should the feedback ever run away.
inline constexpr float kDustCeiling = 8.0f;

/// **DRIVE IS A GAIN INTO A SHAPER, FROM UNDER ITS KNEE TO FAR OVER IT**: 2^-3 at the bottom of the knob, where a
/// full-scale signal sits in the shaper's straight part and comes out as it went in, to 2^4 at the top. So the
/// knob arrives at the sound rather than switching it on.
inline constexpr double kDriveLog2Min = -3.0;
inline constexpr double kDriveLog2Max = 4.0;
/// THE LEVEL THE SHAPER'S MAKE-UP IS MATCHED AT: a sine of this peak comes out with the RMS it went in with, at any
/// Drive, Type and Shape. Quieter detail comes up from there; louder peaks come down.
inline constexpr double kDriveReferencePeak = 0.5;
inline constexpr int kDriveReferencePoints = 128;
/// Soft's dead zone at the top of Shape, as a fraction of `kDriveReferencePeak` (an input under it is silent).
inline constexpr float kSoftStarveSpan = 0.6f;
/// Tube's two halves: the negative half is a tanh of `r u`, over `r`, with `r` from the bottom of Shape to the top.
inline constexpr float kTubeRatioMin = 1.5f;
inline constexpr float kTubeRatioMax = 8.0f;
/// Fold's symmetry: the sine folder is read from a bias of this many quarter-periods at the top of Shape.
inline constexpr float kFoldBiasSpan = 0.75f;
/// Wreck's folder at the top of Shape reaches this many quarter-periods past its first peak.
inline constexpr float kWreckFoldSpan = 4.0f;
/// Lean's bias at the top of Shape, in the tanh's own units; none at the bottom.
inline constexpr float kLeanBiasSpan = 1.2f;
/// CRUSH HOLDS EACH SAMPLE FOR A TIME: nothing at the bottom of Drive, 1 / 500 s at the top, whatever the rate.
inline constexpr double kCrushTopHz = 40000.0;
inline constexpr double kCrushOctaves = 6.3;
/// And rounds it to a step: 10 bits at the bottom of Shape, a bit and a half at the top.
inline constexpr double kCrushBitsFine = 10.0;
inline constexpr double kCrushBitsCoarse = 1.5;
/// SLEW's rate frequency runs, log, from the top at the bottom of the Drive knob to the bottom at the top of it.
inline constexpr double kSlewTopHz = 20000.0;
inline constexpr double kSlewBottomHz = 50.0;
/// The rate is held under this fraction of the sample rate.
inline constexpr double kSlewMaxRateFraction = 0.45;
/// Tube, Fold and Lean leave a standing offset; this takes it back out.
inline constexpr double kDriveDcHz = 10.0;

/// **HP WET**, log2 hertz: the bottom of the range is "no filter at all", the top 8 kHz.
inline constexpr double kHpWetLog2Min = 4.321928094887362;  // 20 Hz
inline constexpr double kHpWetLog2Max = 12.965784284662087; // 8 kHz
inline constexpr double kHpWetQ = 0.7071067811865476;

/// **STEP** holds the shake for a time, log2 seconds: none at the bottom of the range, 1/32 s at the top.
inline constexpr double kStepLog2Min = -15.0;
inline constexpr double kStepLog2Max = -5.0;
/// Detune at the top puts the right side's sine this many octaves above the left's.
inline constexpr double kDetuneOctaves = 1.0;
/// Mod Fold at the top drives the sine this many times past its peak before it is folded.
inline constexpr float kModFoldSpan = 4.0f;
/// **HEAT** at the top runs the Comb's wet this many octaves hotter into its clip and fold (`kHeadroom` is seven).
inline constexpr double kHeatOctaves = 7.0;

/// **FOLLOW**: the input's peak, fast up and slow down, read against a level at which the shake is whole.
inline constexpr double kFollowAttackSeconds = 0.002;
inline constexpr double kFollowReleaseSeconds = 0.12;
inline constexpr float kFollowFullAt = 0.5f; // -6 dB
/// Below this the follower is nothing, so silence ends it.
inline constexpr float kFollowFloor = 1.0e-9f;

/// How long the Amount, the Resonance and the Mix take to arrive: a time, applied a sample at a time.
inline constexpr double kDustSmoothingSeconds = 0.02;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// **TWO ENGINES IN ONE DEVICE.** `Dust` shakes a very short delay the signal itself passes through, one quantity
/// to a control. `Comb` is the resonant line whose eleven quantities ride two coordinates. A Choice is stored by its
/// label, so the order here is free to change.
/// **AND A THIRD, `Rust`**: Dust's shaker (the band of noise, the sine, the hold) MULTIPLYING the signal instead of
/// moving a read head — a ring modulator whose carrier is that band. It shares Dust's modulator controls.
enum class Mode : int { Dust, Comb, Rust, kCount };
inline constexpr const char* kModeNames[] = { "Dust", "Comb", "Rust" };
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}
inline constexpr uint32_t kDustOnly = kModeBit(Mode::Dust);
inline constexpr uint32_t kCombOnly = kModeBit(Mode::Comb);
inline constexpr uint32_t kRustOnly = kModeBit(Mode::Rust);
/// The modulator's controls: the two engines a band of noise and a sine shake or multiply.
inline constexpr uint32_t kShakers = kDustOnly | kRustOnly;

/// **WHAT DRIVE PUSHES THE SHAKEN SIGNAL INTO.** Each has one coordinate of its own (`Param::DriveShape`), which
/// stops at its ends: a Shape never rolls into the next Type.
///   Soft     a tanh behind a dead zone: Shape starves it, so everything under a share of the reference level is
///            silent and what is over it comes up from there; at the bottom it is a plain tanh
///   Tube     a tanh whose negative half is steeper than its positive, both through zero at a slope of one: Shape
///            is how unequal, so the even harmonics
///   Fold     a sine wavefolder read from a bias: Shape is the bias, so the even harmonics; none at the bottom
///   Wreck    a tanh into a folder, in series; Shape is how far past its first peak the folder is pushed, and in
///            Dust the line's feedback is taken AFTER the pair, so Resonance rings through them
///   Jag      a tanh into a TRIANGLE folder, Wreck's sharp counterpart; Shape is how far past its first peak it is pushed,
///            and in Dust the feedback is taken after the pair, as in Wreck
///   Lean     a tanh read from a bias, but the two channels lean OPPOSITE ways: the even harmonics come
///            out in opposite polarity left and right, so they are width, and summed to mono they cancel and leave
///            the tanh's odd ones. Shape is the lean, so the width; at its bottom there is none and both are a tanh
///   Paunch   a plain tanh on the left; on the right the tanh's residue is turned over by Shape, so the ODD harmonics
///            come out in opposite polarity left and right: at full Shape the mono sum is the clean signal and the
///            distortion is all side; at half the right is clean; at the bottom both are Soft's tanh
///   Crush    no shaper: Drive holds each sample longer, Shape rounds it coarser (10 bits down to a bit and a half)
///   Decimate no shaper: Drive holds each sample longer, as Crush does, and nothing is rounded. Shape goes from the
///            held steps (a plain sample-and-hold) to a straight line from the previous held value to the current one
///            across the hold: a first-order hold, which LAGS by one hold period. Between, the two outputs are blended
///   Slew     no shaper: a slew limiter (`Slew`, `shell.h`) whose rate frequency Drive sets, from 20 kHz at the bottom
///            of the knob to 50 Hz at the top. Shape is its curve, from a hard limit to a one-pole
///   Lopside  the same slew at its hard limit with Shape the balance: at the bottom rising steps are a quarter of
///            falling ones, in the middle they are alike, at the top the other way
/// Crush, Decimate, Slew and Lopside are not oversampled and not level-matched (their make-up is 1).
/// BOTH ENGINES HAVE THE STAGE (`drive_stage.h`), last on their wet: Dust's before its HP Wet, the Comb's on the wet
/// it is about to add to the dry.
enum class DriveMode : int { Soft, Tube, Fold, Wreck, Jag, Lean, Paunch, Crush, Decimate, Slew, Lopside, kCount };
inline constexpr const char* kDriveModeNames[] = { "Soft", "Tube", "Fold", "Wreck", "Jag", "Lean",
	                                               "Paunch", "Crush", "Decimate", "Slew", "Lopside" };
static_assert(sizeof(kDriveModeNames) / sizeof(kDriveModeNames[0]) == static_cast<size_t>(DriveMode::kCount));
/// A Type that is not a curve: the stage does something else with it (and never oversamples).
inline constexpr bool driveIsNotCurve(DriveMode mode) {
	return mode == DriveMode::Crush || mode == DriveMode::Decimate || mode == DriveMode::Slew || mode == DriveMode::Lopside;
}

enum class Param : int {
	Enabled,
	Tone,      ///< phase into the tone bank: delay, feedback, clip, fold, highpass, triangle depth
	Character, ///< phase into the character bank: depth, resonance, S&H blend, width, triangle pitch
	Gamma,     ///< both phases at once, 1024 times coarser
	Cutoff,    ///< octaves on the SVF, the highpass and the triangle together
	Mix,       ///< Comb: how much wet is ADDED to a whole dry. Dust: the crossfade from dry to shaken

	Mode,      ///< which engine
	Amount,    ///< Dust: the head's travel, log2 seconds
	Freq,      ///< Dust: the band's centre and the sine's frequency, log2 hertz
	Width,     ///< Dust: the band's width
	Source,    ///< Dust: sine at 0, band noise at 1
	Grit,      ///< Dust: how much of the shake is a value held between the input's zero crossings
	Stereo,    ///< Dust: one shake for both sides at 0, one each at 1
	Resonance, ///< Dust: signed feedback round the line
	Drive,     ///< both engines: how hard the wet is pushed into `DriveMode`'s shaper
	Track,     ///< Dust: Freq follows the last note played
	DriveMode, ///< both engines: which shaper
	DriveShape, ///< both engines: the shaper's own coordinate, 0..1, stopping at both ends
	HpWet,     ///< every engine: a highpass on the wet, after the shaper; log2 hertz, the bottom is none
	SubThru,   ///< every engine: what HP Wet took from the wet is given back from the dry
	Follow,    ///< every engine: how much of the shake rides the input's level
	RustBias,  ///< Rust: how much of the signal passes the multiplier untouched — ring at 0, tremolo toward 1
	RustRectify, ///< Rust: how much of the carrier is folded to one side
	Step,      ///< Dust, Rust: the shake is held for this long at a time, log2 seconds; the bottom is not at all
	Detune,    ///< Dust, Rust: how far the right side's sine runs above the left's, up to an octave
	ModFold,   ///< Dust, Rust: the sine pushed past its peaks and folded back
	Heat,      ///< Comb: how hot the wet runs into its own clip and fold
	Blend,     ///< Comb: Mix crossfades from dry to wet instead of adding the wet to a whole dry

	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// **NO ZONE NAMES (backlog IW).** Tone and Character are continuous phi coordinates — the DSP reads
/// the phase as a continuum and branches on no zone — so they are FREE-ONLY: no dropdown, a sigil for the lap. The
/// firmware's names (Sub … Full, Deep … Open) were never measured against the bank, and MEASURED on 2026-09-23 (backlog
/// IQ) a point just past a boundary sounded no more like its new zone than its old one. `kZones` stays as the drag
/// and modulation unit.

inline constexpr const char* kTags[] = { "distortion", "comb", "fx" };

/// THE FACE: the firmware's four knobs in the firmware's menu order, then Gamma — which on the
/// hardware is not a menu item at all but a gesture (push and twist), so it goes last.
///
///   Tone     Character   Cutoff
///   Mix      Gamma
///
/// AND DUST'S, which shares the room: a control gated to the other engine is not drawn, so only one set shows.
///
///   Mode/Track  Amount     Freq
///   Width       Source     Grit
///   Stereo      Resonance  Drive
///   Mix
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::Mode),      static_cast<uint8_t>(Param::Track),
	static_cast<uint8_t>(Param::Amount),    static_cast<uint8_t>(Param::Freq),
	static_cast<uint8_t>(Param::Width),     static_cast<uint8_t>(Param::Source),
	static_cast<uint8_t>(Param::Grit),      static_cast<uint8_t>(Param::Stereo),
	static_cast<uint8_t>(Param::Resonance), static_cast<uint8_t>(Param::Drive),
	static_cast<uint8_t>(Param::DriveMode), static_cast<uint8_t>(Param::DriveShape),
	static_cast<uint8_t>(Param::HpWet),     static_cast<uint8_t>(Param::SubThru),
	static_cast<uint8_t>(Param::Follow),    static_cast<uint8_t>(Param::RustBias),
	static_cast<uint8_t>(Param::RustRectify), static_cast<uint8_t>(Param::Step),
	static_cast<uint8_t>(Param::Detune),    static_cast<uint8_t>(Param::ModFold),
	static_cast<uint8_t>(Param::Tone),      static_cast<uint8_t>(Param::Character),
	static_cast<uint8_t>(Param::Cutoff),    static_cast<uint8_t>(Param::Mix),
	static_cast<uint8_t>(Param::Gamma),     static_cast<uint8_t>(Param::Heat),
	static_cast<uint8_t>(Param::Blend),
};

inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "eroder_drive", "2026-11-15",
	  "a gain of 1 to 16 into a tanh that switched in whole became a gain from under the knee to over it into a chosen shaper (eroder_drive_amount, eroder_drive_mode, eroder_drive_shape)" },
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "ERODER",
	.name = "Eroder",
	.hasCard = true,
	.hasExpanded = true,
	// ON THE BUS, FOLLOWING THE LAST NOTE — by design, and the firmware's own arrangement
	// (`getLastNoteCode()`).
	.perVoiceControl = -1,
	.extraState = 0,
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = -1,
	.faceControls = 0,
	.faceOrder = kFaceOrder,
	// **THREE ACROSS, NOT THE DELAY'S FOUR — PHOTOGRAPHED AT FOUR AND IT DID NOT FIT.** A zoned
	// coordinate's cell carries a readout, a caption and a zone stepper, and Gamma is a value box with
	// its label inside it: at a quarter of the card "CHARACTER" clipped to "HARACTE" and "Gamma" ran
	// into its own number. Two rows either way, so the width costs nothing.
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
    {.key = "eroder_enabled", .name = "Eroder Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the eroder from the chain"},
    {.key = "eroder_tone", .name = "Eroder Tone",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "One knob for six things: the comb's length and feedback, soft clip, fold, the "
                "highpass and the pitched wobble's depth, each swept at its own rate so they come "
                "and go against each other. It keeps turning, and the mark beside it shows which lap",
     .zones = kZones,
     .modSpan = kOneZone,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly},
    {.key = "eroder_character", .name = "Eroder Character",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "One knob for the noise that shakes the comb: how deep, how resonant, how much of it "
                "is sample-and-hold, how wide in stereo, and where the pitched wobble sits. One continuous "
                "turn — it keeps turning, and the mark beside it shows which lap",
     .zones = kZones,
     .modSpan = kOneZone,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly},
    {.key = "eroder_gamma", .name = "Eroder Gamma",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Moves Tone and Character together, a thousand times coarser — a different eroder at "
                "every step",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly},
    {.key = "eroder_cutoff", .name = "Eroder Cutoff",
     .min = -kCutoffOctaves,
     .max = kCutoffOctaves,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Octaves, on three things at once: the noise filter, the wet highpass and the "
                "pitched wobble. All three also follow the last note played",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly,
     .centred = true,
     .suffix = "oct"},
    {.key = "eroder_mix", .name = "Eroder Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "In Dust and Rust, the crossfade from the dry signal to the effect. In Comb, how much comb "
                "is added to a dry that passes whole, or the same crossfade with Blend on"},
    {.key = "eroder_mode", .name = "Eroder Mode",
     .min = 0.0,
     .max = static_cast<double>(Mode::kCount) - 1.0,
     .def = static_cast<double>(Mode::Dust),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dust passes the signal through a very short delay whose length is shaken by a band of "
                "noise or a sine: fine grain to coarse hash, with a control for each part of it. Comb is a "
                "resonant comb shaken by noise, steered by two coordinates that each move several things at "
                "once. Rust multiplies the signal by that same band of noise or sine instead of delaying it"},
    {.key = "eroder_amount", .name = "Eroder Amount",
     .min = kDustAmountLog2Min,
     .max = kDustAmountLog2Max,
     .def = kDustAmountLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the delay is shaken, as a time. A few microseconds is a haze on the top end; "
                "a millisecond tears the signal apart. At the bottom of its travel nothing moves",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kDustOnly},
    {.key = "eroder_freq", .name = "Eroder Freq",
     .min = kDustFreqLog2Min,
     .max = kDustFreqLog2Max,
     .def = kDustFreqLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     // A ROUTE MAY CARRY IT PAST THE KNOB: with Track on, the note's ratio is applied after this value, so a low
     // note needs more than the knob's top to reach the top of the band. `deriveDust` holds the result in hertz.
     .offsetsMayLeaveRange = true,
     .tooltip = "Where the shaking sits: the centre of the noise band and the frequency of the sine. Low "
                "is wobble and flutter, high is hiss and dust",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers},
    {.key = "eroder_width", .name = "Eroder Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How wide the noise band is: narrow is a wavering tone around Freq, wide is plain hiss. "
                "It does nothing to the sine",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers},
    {.key = "eroder_source", .name = "Eroder Source",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "What shakes the delay: a sine at Freq at the bottom, the noise band at the top, a blend "
                "between",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers},
    {.key = "eroder_grit", .name = "Eroder Grit",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Replaces the shake with a random value held until the input next crosses zero, so the "
                "delay jumps in step with the sound: crackle that follows the pitch",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers},
    {.key = "eroder_stereo", .name = "Eroder Stereo",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "At the bottom both sides are shaken alike; at the top each has its own noise, and the "
                "sine runs a quarter turn apart",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers},
    {.key = "eroder_resonance", .name = "Eroder Resonance",
     .min = -1.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Feeds the shaken signal back round the delay, so it rings at a pitch Amount sets. Left "
                "of centre the feedback is inverted, which rings an octave lower and hollow",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kDustOnly,
     .centred = true,
     // "RESONANCE" is as long as "CHARACTER", which touched its cell's border at three across.
     .shortName = "Reso"},
    {.key = "eroder_drive_amount", .name = "Eroder Drive",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How hard the effect's own signal is pushed into the shaper Type names. It comes in from "
                "nothing, and the level is held about where it was. In Crush and Decimate it is how long each sample "
	                "is held, in Slew and Lopside how slow the slew is",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0},
    {.key = "eroder_track", .name = "Eroder Track",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Freq follows the last note played, with middle C leaving it where it is set",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers,
     .centred = false,
     .shortName = nullptr,
     .stacked = true},
    {.key = "eroder_drive_mode", .name = "Eroder Drive Type",
     .min = 0.0,
     .max = static_cast<double>(static_cast<int>(DriveMode::kCount) - 1),
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kDriveModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "What Drive pushes into: Soft starves the quiet parts and rounds the rest, Tube's two halves "
	                "differ, Fold is a sine folder that can lean to one side, Wreck is a distortion into a folder "
	                "(in Dust the feedback runs through both), Jag is a distortion into a sharp folder, Lean "
	                "leans left and right opposite ways, so its even harmonics are stereo width, Paunch puts "
	                "its odd harmonics in opposite polarity left and right, Crush holds and rounds each sample, "
	                "Decimate holds each sample, as steps or as lines, Slew limits how fast the signal may move, "
	                "Lopside lets it rise and fall at different speeds",
	     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Type"},
    {.key = "eroder_drive_shape", .name = "Eroder Drive Shape",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The Type's own colour: Soft starves quiet parts, Tube's two halves differ, Fold's symmetry, "
	                "how far Wreck and Jag fold, how wide Lean leans, how much of Paunch is turned over, "
	                "Crush's bit depth, Decimate from steps to lines, Slew from hard to soft, which way "
	                "Lopside drags",
	     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Shape"},
    {.key = "eroder_hp_wet", .name = "Eroder HP Wet",
     .min = kHpWetLog2Min,
     .max = kHpWetLog2Max,
     .def = kHpWetLog2Min,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Takes the low end out of the shaken signal, after the shaper. At the bottom there is no "
                "filter",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "HP Wet"},
    {.key = "eroder_sub_thru", .name = "Eroder Sub Thru",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Gives back from the dry signal what HP Wet took away, so the low end stays whole at any Mix",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Sub Thru",
     .stacked = true},
    {.key = "eroder_follow", .name = "Eroder Follow",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the shaking rides the input's level: at the top it is still while nothing "
                "plays and whole once the input reaches half scale",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0},
    {.key = "eroder_rust_bias", .name = "Eroder Bias",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the signal passes the multiplier untouched. At the bottom the band replaces the "
                "signal's own pitch with sums and differences; toward the top it is a rough tremolo",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kRustOnly,
     .centred = false,
     .shortName = "Bias"},
    {.key = "eroder_rust_rectify", .name = "Eroder Rectify",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Folds the multiplying band to one side, which doubles its pitch and lets more of the signal "
                "through",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kRustOnly,
     .centred = false,
     .shortName = "Rectify"},
    {.key = "eroder_step", .name = "Eroder Step",
     .min = kStepLog2Min,
     .max = kStepLog2Max,
     .def = kStepLog2Min,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Holds the shake still for this long at a time, so it moves in steps. At the bottom it moves "
                "freely",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers,
     .centred = false,
     .shortName = "Step"},
    {.key = "eroder_detune", .name = "Eroder Detune",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Runs the right side's sine above the left's, up to an octave, so the two sides beat against "
                "each other. It is the sine's: with Source all noise there is nothing to detune",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers,
     .centred = false,
     .shortName = "Detune"},
    {.key = "eroder_mod_fold", .name = "Eroder Mod Fold",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Pushes the sine past its peaks and folds it back, which adds overtones to the shake. It is "
                "the sine's: with Source all noise there is nothing to fold",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakers,
     .centred = false,
     .shortName = "Mod Fold"},
    {.key = "eroder_heat", .name = "Eroder Heat",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How hot the comb runs into its own clip and fold. At the bottom they barely touch the signal; "
                "up, they bite",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly,
     .centred = false,
     .shortName = "Heat"},
    {.key = "eroder_blend", .name = "Eroder Blend",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Mix crossfades from the dry signal to the comb, so the top is the comb alone. Off, the comb is "
                "added to the whole dry signal",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kCombOnly,
     .centred = false,
     .shortName = "Blend",
     .stacked = true},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

struct Settings {
	bool enabled = true;
	float tone = 0.0f;
	float character = 0.0f;
	float gamma = 0.0f;
	float cutoffOctaves = 0.0f;
	float mix = 1.0f;

	Mode mode = Mode::Dust;
	float amount = static_cast<float>(kDustAmountLog2Default); ///< log2 seconds
	float freq = static_cast<float>(kDustFreqLog2Default);     ///< log2 hertz
	float width = 0.5f;
	float source = 1.0f;
	float grit = 0.0f;
	float stereo = 1.0f;
	float resonance = 0.0f;
	float drive = 0.0f;
	bool track = false;
	DriveMode driveMode = DriveMode::Soft;
	float driveShape = 0.0f;
	float hpWet = static_cast<float>(kHpWetLog2Min); ///< log2 hertz; the bottom is no filter
	bool subThru = false;
	float follow = 0.0f;
	float rustBias = 0.0f;
	float rustRectify = 0.0f;
	float step = static_cast<float>(kStepLog2Min); ///< log2 seconds; the bottom is no hold
	float detune = 0.0f;
	float modFold = 0.0f;
	float heat = 0.0f;
	bool blend = false;

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
// THE MAPPING: FOUR CONTROLS TO ELEVEN QUANTITIES.                                                   //
//--------------------------------------------------------------------------------------------------//

/// THE TWO PHASES THE BANKS ARE READ AT. `freqNorm + freqPhaseOffset + 1024 * gammaPhase`, with the
/// first two already one coordinate here.
struct Phases {
	double tone = 0.0;
	double character = 0.0;
};
[[nodiscard]] Phases phasesOf(double tone, double character, double gamma) noexcept;

/// THE BANKS' RAW OUTPUT — six and five triangles, before any mapping. Exposed so a test can assert
/// the bank CONFIGURATIONS against the firmware's table independently of what is made of them.
using ToneBank = std::array<float, 6>;
using CharacterBank = std::array<float, 5>;
[[nodiscard]] ToneBank evalToneBank(double phase) noexcept;
[[nodiscard]] CharacterBank evalCharacterBank(double phase) noexcept;

/// **EVERYTHING THE SAMPLE LOOP RUNS ON, IN PHYSICAL UNITS.** A pure function of the two phases, the
/// Cutoff, the Mix, the note and the rate — so "Tone at 0.4 gives a 3.1 ms comb at 62% feedback" is
/// an equality on a struct rather than something inferred from a rendered buffer.
struct Derived {
	double baseDelaySeconds = kMinBaseDelaySeconds;
	double feedback = 0.0;
	double clipIntensity = 0.0;
	double foldIntensity = 0.0;
	double clipDrive = 1.0;
	double foldDrive = 1.0;
	double hpfHz = kHpfBaseHz;
	double hpfAlpha = 0.0;
	double triangleDepth = 0.0;
	double triangleHz = kTriangleBaseHz;
	double depth = kDepthFloor;
	double resonance = 0.0;
	double shBlend = 0.0;
	double width = 0.0;
	double svfHz = 0.0;
	double svfCoefficient = kSvfReferenceCoefficient;
	/// **WHAT `wetGain`'S `(1 - feedback)` TAKES TOO MUCH OF.** A comb's broadband power gain is 1 / (1 - fb^2), so
	/// the gain that holds its level is sqrt(1 - fb^2); `(1 - fb)` is that times sqrt((1 - fb) / (1 + fb)), which made
	/// a higher feedback QUIETER. This is the factor back: sqrt((1 + fb) / (1 - fb)). Kept apart from `wetGain`,
	/// which stays the reference's arithmetic.
	double levelMakeUp = 1.0;
	/// `wetLevel * (1 - feedback) / clipDrive / foldDrive`, each drive only when its stage is on.
	double wetGain = 0.0;
};

[[nodiscard]] Derived derive(const Phases& phases, double cutoffOctaves, double mix, double pitchSemitones,
                             double sampleRate) noexcept;

/// THE TWO WAVESHAPERS, as the firmware has them and in the firmware's order. Out here rather than
/// in the `.cpp` for the usual reason: their knee, slope and fold period are values.
[[nodiscard]] float softClip(float x, float drive) noexcept;
[[nodiscard]] float triFold(float x, float drive) noexcept;

/// **THE WET'S WAVESHAPING STAGE, WHOLE: SOFT CLIP, THEN FOLD, each only when its intensity is
/// above zero.** One function rather than four lines in the loop because the ORDER is a value too —
/// a fold of a clipped signal and a clip of a folded one are different sounds — and a line order
/// inside a sample loop is something only a reader can check.
[[nodiscard]] float shapeWet(float wet, const Derived& derived) noexcept;

//--------------------------------------------------------------------------------------------------//
// DUST'S MAPPING: ONE QUANTITY PER CONTROL.                                                          //
//--------------------------------------------------------------------------------------------------//

/// **EVERYTHING DUST'S SAMPLE LOOP RUNS ON.** A pure function of the settings, the note and the rate.
struct DustDerived {
	double amountSeconds = 0.0; ///< the head's travel either side of its centre; 0 at the bottom of the range
	double freqHz = 0.0;        ///< after tracking and the clamps
	double q = 1.0;
	// THE BAND'S FILTER, trapezoidal: g = tan(pi f / rate), k = 1 / Q, and the three solved coefficients.
	double g = 0.0;
	double k = 1.0;
	double a1 = 1.0;
	double a2 = 0.0;
	double a3 = 0.0;
	/// What the band's output (two such filters in series) is multiplied by so its RMS is `kDustNoiseRms`.
	double noiseGain = 0.0;
	double sineStep = 0.0;        ///< radians a frame
	double sinePhaseOffset = 0.0; ///< the right side's lead, radians
	double feedback = 0.0;
	double wetGain = 0.0; ///< Mix, with the comb's broadband gain taken back out
	double dryGain = 1.0;
	/// THE SHAPER (`shapeDrive`): off at the bottom of Drive, else the gain in and the make-up out.
	bool driven = false;
	DriveMode driveMode = DriveMode::Soft;
	float driveShape = 0.0f;
	double driveGain = 1.0;
	double driveNorm = 1.0;
	/// Crush: how many frames a sample is held, and the step it is rounded to.
	double crushHoldFrames = 1.0;
	double crushStep = 0.0;
	/// Slew and Lopside: the rate frequency, the curve and the balance the stage's `Slew` is set to.
	float slewHz = 0.0f;
	float slewCurve = 0.0f;
	float slewBalance = 0.0f;
	float driveMix = 1.0f;
	/// HP WET, trapezoidal like the band: off at the bottom of the range.
	bool hpOn = false;
	double hpHz = 0.0;
	double hpG = 0.0;
	double hpK = 0.0;
	double hpA1 = 1.0;
	double hpA2 = 0.0;
	double hpA3 = 0.0;
	bool subThru = false;
	float follow = 0.0f;
	/// THE SHAKE'S EXTRAS: frames it is held for (one is every frame), the right sine's step, the sine's fold.
	double stepHoldFrames = 1.0;
	double sineStepRight = 0.0;
	bool detuned = false;
	float modFold = 0.0f;
	/// RUST: the engine multiplies instead of reading a moved head.
	bool ring = false;
	float rustBias = 0.0f;
	float rustRectify = 0.0f;
	float source = 1.0f;
	float grit = 0.0f;
	float stereo = 1.0f;
	float stereoNorm = 1.0f;
};

/// **WHAT A `DriveStage` RUNS ON** (`drive_stage.h`): both engines derive one from the same three controls.
struct DriveSettings {
	bool driven = false; ///< false at the bottom of Drive: the stage is not run
	DriveMode mode = DriveMode::Soft;
	float shape = 0.0f;
	float gain = 1.0f; ///< into the curve
	float norm = 1.0f; ///< the make-up out of it
	double crushHoldFrames = 1.0;
	float crushStep = 0.0f;
	/// Slew and Lopside: the rate frequency, the curve and the balance the stage's `Slew` is set to.
	float slewHz = 0.0f;
	float slewCurve = 0.0f;
	float slewBalance = 0.0f;
	/// THE STAGE'S OWN MIX, 0..1: the output is `dry + mix * (processed - dry)`. At 0 the stage is not run and the
	/// signal is the bits it was; at 1 nothing is blended (the stage is exactly what it is without a mix).
	float mix = 1.0f;
};
/// @param reuse  settings this may take the make-up from when the Drive, Type and Shape are the same — the make-up
///               is a quadrature, and the control path asks for it every block.
/// @param mix    the stage's own mix, for a caller that sets one; the Drive control itself is always at 1
[[nodiscard]] DriveSettings deriveDrive(float drive, DriveMode mode, float shape, double sampleRate,
                                        const DriveSettings* reuse = nullptr, float mix = 1.0f) noexcept;

/// **WHAT A `WetHighpass` RUNS ON**: off at the bottom of HP Wet, else the trapezoidal coefficients of a two-pole
/// highpass at `hz`.
struct HighpassSettings {
	bool on = false;
	double hz = 0.0;
	float a1 = 1.0f;
	float a2 = 0.0f;
	float a3 = 0.0f;
	float k = 0.0f;
};
[[nodiscard]] HighpassSettings deriveHighpass(float hpWetLog2Hz, double sampleRate) noexcept;

/// @param reuse  a result this one may take the shaper's make-up from when the Drive, Type and Shape are the same —
///               the make-up is a quadrature, and the control path asks for it every block.
[[nodiscard]] DustDerived deriveDust(const Settings& settings, double pitchSemitones, double sampleRate,
                                     const DustDerived* reuse = nullptr) noexcept;

/// **THE SHAPER'S CURVE**, of a signal already multiplied by the Drive's gain `gain`: Soft, Tube, Fold, Wreck, Jag, Lean and Paunch (its left).
/// Every one is 0 at 0; all but Soft at Shape above 0 (whose dead zone is a share of the reference level, so it needs
/// the gain) go straight through it. The Types that are not curves are the identity here.
[[nodiscard]] float shapeDrive(DriveMode mode, float driven, float shape, float gain) noexcept;
/// THE RIGHT CHANNEL'S CURVE: the left's, except in Lean, where it is the left's turned over (`-f(-u)`) — the same
/// odd harmonics, the even ones in the other polarity.
[[nodiscard]] float shapeDriveRight(DriveMode mode, float driven, float shape, float gain) noexcept;
/// The make-up that follows it: `kDriveReferencePeak`'s sine out at the RMS it went in with.
[[nodiscard]] double driveMakeUp(DriveMode mode, double gain, float shape) noexcept;
/// THE SHAPER'S GAIN FOR A VANISHING SIGNAL, make-up included: what a feedback through it is divided by, so a tail
/// always dies.
[[nodiscard]] double driveSmallSignalGain(DriveMode mode, double gain, float shape) noexcept;
[[nodiscard]] HighpassSettings highpassOf(const DustDerived& derived) noexcept;
/// The stage's settings as `deriveDust` found them.
[[nodiscard]] DriveSettings driveOf(const DustDerived& derived) noexcept;
/// The curve's slope at zero (0 in Soft above the bottom of Shape: its dead zone).
[[nodiscard]] double driveSlope(DriveMode mode, float shape) noexcept;
/// WHAT FOLLOW MAKES OF THE FOLLOWER: 1 at no Follow, the follower's reach of `kFollowFullAt` at all of it.
[[nodiscard]] float followGain(float follow, float envelope) noexcept;

//--------------------------------------------------------------------------------------------------//
// WHAT THE DEVICE PUBLISHES FOR ITS CARD AND WINDOW.                                                 //
//--------------------------------------------------------------------------------------------------//

/// **FACTS ONLY THE RUNNING DEVICE HAS**, once per process call, in this order:
///   - THE SPECTRUM FRAME, the EQ's layout (`dsp/analyser.h`): a counter, 96 bins of what came in, 96 of what went
///     out (dB, power density), and the counter again. The readings store has no seqlock, so a reader that finds the
///     two counters different has read a frame half-written and drops it;
///   - THE NOTE the bus last played, in semitones: what Track and the Comb's tuning follow;
///   - WHERE THE READ HEAD HAS BEEN during the call, each side: the nearest and the farthest it sat behind the write,
///     in seconds, sampled once a control block. Both engines publish it.
/// Everything else either surface draws (the band, the Comb's eleven quantities) is a pure function of the controls
/// and the note: `deriveDust` and `derive`, run where they are drawn.
enum class Reading : size_t {
	FrameA = 0,
	PreFirst = 1,
	PostFirst = 1 + dsp::kAnalyserBins,
	FrameB = 1 + 2 * dsp::kAnalyserBins,
	PitchSemitones,
	HeadNearL,
	HeadFarL,
	HeadNearR,
	HeadFarR,
	kCount,
};
inline constexpr size_t kReadingCount = static_cast<size_t>(Reading::kCount);

extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::DriveColour,
};

} // namespace feathers::fx::eroder
