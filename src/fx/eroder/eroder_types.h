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
#include "fx/wet_highpass.h"

#include <array>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

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
/// A HARD CEILING ON THE WET, just before Mix (about +12 dBFS), in every engine. It is a safety for a self-oscillating loop, not
/// a tone stage: anything smaller passes to the bit.
inline constexpr float kWetCeiling = 4.0f;

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
inline constexpr double kHpWetLog2Min = fx::kWetHighpassLog2Min;  // 20 Hz
inline constexpr double kHpWetLog2Max = fx::kWetHighpassLog2Max; // 8 kHz
inline constexpr double kHpWetQ = fx::kWetHighpassQ;

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
// THE SHARED SECTION'S NUMBERS (`loops.h`): emphasis, smear and the two feedback loops, the same for every engine.   //
//--------------------------------------------------------------------------------------------------//

/// THE EMPHASIS RANGE, dB either way.
inline constexpr double kEmphasisMaxDb = 18.0;
/// **WIDTH IS THE BELL'S WIDTH, in octaves between the half-gain points, log-mapped:** 0 is a quarter of an octave, 1 is three.
/// (`Width` is the noise band's control in the shaking engines; the emphasis reads the same coordinate, so the Comb, which
/// does not draw it, keeps whatever it was left at.)
inline constexpr double kEmphasisWidthOctavesMin = 0.25;
inline constexpr double kEmphasisWidthOctavesMax = 3.0;
/// **THE OUTER LOOP'S GAIN AT THE TOP OF ITS KNOB**, before the divisions that keep it under one: just short of the edge.
inline constexpr double kLoopMaxGain = 0.98;
/// THE INTERVAL, semitones either way: the outer delay is the period of Freq times `2^(-interval / 12)`.
inline constexpr double kLoopIntervalSemitones = 24.0;
/// What the loops' delay lines can hold, seconds: five hertz (the lowest Freq a route can reach) two octaves down.
inline constexpr double kLoopLineSeconds = 0.85;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// **TWO ENGINES IN ONE DEVICE.** `Dust` shakes a very short delay the signal itself passes through, one quantity
/// to a control. `Comb` is the resonant line whose eleven quantities ride two coordinates. A Choice is stored by its
/// label, so the order here is free to change.
/// **AND A THIRD, `Rust`**: Dust's shaker (the band of noise, the sine, the hold) MULTIPLYING the signal instead of
/// moving a read head — a ring modulator whose carrier is that band. It shares Dust's modulator controls.
/// **AND A FOURTH, `Etch`**: no shake and no delay at all, two `DriveStage` slots of their own (Type, Amount, Shape and
/// Mix each) in series or in parallel, between the shared section's Smear and the Drive. At rest it is a true bypass.
enum class Mode : int { Dust, Comb, Rust, Etch, kCount };
inline constexpr const char* kModeNames[] = { "Dust", "Comb", "Rust", "Etch" };
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}
inline constexpr uint32_t kDustOnly = kModeBit(Mode::Dust);
inline constexpr uint32_t kCombOnly = kModeBit(Mode::Comb);
inline constexpr uint32_t kRustOnly = kModeBit(Mode::Rust);
inline constexpr uint32_t kEtchOnly = kModeBit(Mode::Etch);
/// What the shared section needs from the shaker's controls: Freq, Width, Resonance and Track are live in Etch too (the
/// emphasis's centre and width, the loops' pitch).
inline constexpr uint32_t kShakersAndEtch = kDustOnly | kRustOnly | kEtchOnly;
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
///   Chonk    a soft saturator (a tanh) on the left; on the right the saturator's difference from the clean signal is turned
///            over by Shape (`r_out = tanh(g r) + 2 shape (r - tanh(g r))`), so the ODD harmonics come out in opposite
///            polarity left and right and the mono sum at full Shape is the clean signal. As Drive rises the right thins out
///            and then flips (its small-signal gain is `2 - gain * norm` at full Shape). At the bottom of Shape both are
///            Soft's tanh. The right is not held to the left's ceiling here (the wet ceiling follows)
///   Paunch-S the MID (`(l + r) / 2`) rectified, `a |m|`, added to the left and taken from the right: even harmonics that are
///            stereo width and cancel in mono. The dry passes at unity (no make-up, no saturation). Amount is `a`, linear, 0..4.
///            Shape is the direction: opposite polarity at the bottom (all in the side), none in the middle, the same on both
///            sides at the top (all in the mid)
///   Paunch-H the same with the square, `kappa (a m)^2`: a pure octave above (see `paunchSide`)
///   Paunch-OS the ODD counterpart of Paunch-S: a pure third harmonic of the mid, sized to the signal's own tracked level
///            (`paunchOdd`), added to the left and taken from the right, so odd harmonics that are stereo width and vanish in
///            mono. Level-independent: the 3rd sits at a/4 of the fundamental at any level. It makes no offset
///   Paunch-OH the same with the plain cube: LEVEL-DEPENDENT, its 3rd grows with the cube of the level and of `a`
///            (`kappa3 a^3 (m^3 - 1.5 P m)`, the fundamental leak cancelled from the same tracked level), so quiet passages
///            stay clean and loud ones spread
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
enum class DriveMode : int { Soft, Tube, Fold, Wreck, Jag, Lean, Chonk, PaunchS, PaunchH, PaunchOS, PaunchOH, Crush, Decimate, Slew, Lopside, kCount };
inline constexpr const char* kDriveModeNames[] = { "Soft", "Tube", "Fold", "Wreck", "Jag", "Lean (w)", "Chonk (w)",
	                                               "Paunch-S (w)", "Paunch-H (w)", "Paunch-OS (w)", "Paunch-OH (w)", "Crush", "Decimate", "Slew", "Lopside" };
/// **(w) MARKS THE TYPES THAT ADD STEREO WIDTH** (the two sides differ): Lean, Chonk and the four Paunch Types. A patch stores a Choice
/// by its label, so the labels they were saved under read as the new ones: Lean shipped as "Lean", and Chonk as "Paunch". The match is
/// of the whole label; the other new labels never shipped bare.
inline constexpr device::ChoiceAlias kDriveTypeAliases[] = { { "Paunch", "Chonk (w)" }, { "Lean", "Lean (w)" } };
static_assert(sizeof(kDriveModeNames) / sizeof(kDriveModeNames[0]) == static_cast<size_t>(DriveMode::kCount));
/// THE TWO PAUNCH LAWS (Paunch-S and Paunch-H): a function of the mid added to one side and taken from the other, not a curve
/// of one channel.
inline constexpr bool driveIsPaunchLaw(DriveMode mode) {
	return mode == DriveMode::PaunchS || mode == DriveMode::PaunchH;
}
/// THE LEVEL-TRACKED ODD LAWS (Paunch-OS): they make no offset and keep a one-pole follower of the mid's power.
inline constexpr bool driveIsPaunchOdd(DriveMode mode) {
	return mode == DriveMode::PaunchOS || mode == DriveMode::PaunchOH;
}
/// EITHER FAMILY OF MID LAW: the Amount is linear 0..4, no make-up, no saturation, not a curve of one channel.
inline constexpr bool driveIsMidLaw(DriveMode mode) {
	return driveIsPaunchLaw(mode) || driveIsPaunchOdd(mode);
}
/// A Type that is not a curve: the stage does something else with it. Crush, Decimate, Slew and Lopside never oversample; the
/// two Paunch laws do (they are in the oversampled region), but have no make-up and no saturation, so the curve's helpers
/// (`shapeDrive`, `driveMakeUp`) treat them as the identity.
inline constexpr bool driveIsNotCurve(DriveMode mode) {
	return mode == DriveMode::Crush || mode == DriveMode::Decimate || mode == DriveMode::Slew || mode == DriveMode::Lopside ||
	       driveIsMidLaw(mode);
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
	// THE SHARED SECTION, every engine (`loops.h`).
	EmphAmount,   ///< dB: a filter before the engine and (with Restore) its inverse after the Drive; 0 is neither
	EmphShape,    ///< Tilt (pivots at Freq) or Bell (peaks at Freq, Width octaves wide)
	EmphRestore,  ///< the inverse after the Drive
	EmphInLoop,   ///< the filter sits after the sum, so each pass round a loop goes through it
	Smear,        ///< eight allpass sections centred on Freq, before the engine
	FbTap,        ///< where the inner feedback is taken: Clean (before the Drive) or Driven (after it)
	Loop,         ///< the outer loop's amount: the Driven tap through a delay of one period of Freq, shifted by Interval
	LoopInterval, ///< semitones; the outer delay is the period of Freq times 2^(-interval / 12)
	// ETCH'S TWO SLOTS (`drive_stage.h`), each a Drive stage of its own.
	SlotAAmount, ///< Etch: slot A's Type's amount, as Drive's
	SlotAType,
	SlotAShape,
	SlotAMix,
	SlotBAmount,
	SlotBType,
	SlotBShape,
	SlotBMix,
	SlotRoute, ///< Etch: B after A (Series) or both on the same input, their changes added (Parallel)
	EmphOffset, ///< every engine: semitones from Freq to where the emphasis is tuned (Smear and the loops stay at Freq)
	Input,      ///< every engine: decibels of gain on the signal entering the wet chain, ahead of everything in it
	HpWetSemis, ///< Dust, Rust, Etch with Track on: HP Wet in semitones from the played note, in HP Wet's cell; the bottom is no filter
	DcBlock,    ///< every engine: Off or On: the one post high-pass that takes the offset the shapers make out after the Drive (it replaced the DC amount)
	DriveBal,   ///< every engine: Chonk's balance, the Drive cell's second parameter for that Type; 100% keeps the two sides at the balance they came in with
	DriveRate,  ///< every engine: the Paunch Types' rotation LFO, the Drive cell's second parameter for those Types; the bottom is off (a static angle)
	SlotARate,  ///< Etch: slot A's rotation LFO rate (Paunch Types), the cell's second parameter for them
	SlotBRate,
	SlotABal,   ///< Etch: slot A's Chonk balance, the cell's second parameter for that Type
	SlotBBal,
	DriveRateMult, ///< every engine with Track: the Paunch Types' rotation rate as a multiple of the played note's frequency (0 is off), in the Rate row's place while Track is on
	SlotARateMult,
	SlotBRateMult,
	/// THE PAUNCH TYPES' ANGLE (Drive, slot A, slot B): its OWN key, because Shape keeps its old meaning for every other Type and a key names one meaning.
	DriveAngle,
	SlotAAngle,
	SlotBAngle,

	kCount,
};
/// How many controls the shared section added after the first release of the face (they are not on it yet).
inline constexpr size_t kSharedParamCount = 8;
/// And the nine Etch added after them (its slots and their route), likewise not on the face yet.
inline constexpr size_t kEtchParamCount = 9;
/// And the emphasis offset after those.
inline constexpr size_t kEmphOffsetParamCount = 1;
/// And the input drive after that.
inline constexpr size_t kInputParamCount = 1;
/// And HP Wet's semitone face after that.
inline constexpr size_t kHpWetSemisParamCount = 1;
/// And the DC amount after that.
inline constexpr size_t kDcParamCount = 1;
/// And Chonk's balance, and the Paunch Types' rotation rate, after that.
inline constexpr size_t kDriveBalParamCount = 1;
inline constexpr size_t kDriveRateParamCount = 1;
/// And the four slot second parameters (Rate and Bal, A and B) after those.
inline constexpr size_t kSlotSecondParamCount = 4;
/// And the three subharmonic Choices (Drive, slot A, slot B) after those.
inline constexpr size_t kRateMultParamCount = 3;
/// And the three Angle controls after those.
inline constexpr size_t kAngleParamCount = 3;
/// THE TRACKED RATE: a multiple of the played note's frequency, 0..1, 0 is off (a static angle). Drag lands on the subharmonics below (the
/// control stays continuous: a typed value is kept as typed, and a modulated one is not snapped).
inline constexpr double kRateMultDefault = 0.5;
inline constexpr double kRateMultDetents[] = { 0.0, 1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 5.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0 };
/// THE ROTATION LFO'S RANGE: log2 hertz; the bottom is off, then 0.05 Hz to 20 Hz.
inline constexpr double kDriveRateLog2Min = -4.321928094887362; // 0.05 Hz, which is OFF
inline constexpr double kDriveRateLog2Max = 4.321928094887362;  // 20 Hz
inline constexpr double kDriveRateLog2Default = -1.0;           // 0.5 Hz
/// **HP WET'S SEMITONE FACE'S RANGE**: from the played note, -24 to +128 semitones, and the minimum is no filter (as HP Wet's own).
inline constexpr double kHpWetSemisMin = -24.0;
inline constexpr double kHpWetSemisMax = 128.0;
/// The input drive's range, decibels either way.
inline constexpr double kInputMaxDb = 24.0;
/// The linear gain of an input drive in decibels: exactly one at zero, so the chain is untouched there.
[[nodiscard]] inline float inputGainOf(float db) noexcept {
	return db == 0.0f ? 1.0f : static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}
/// The emphasis offset's range, in semitones either way.
inline constexpr double kEmphOffsetSemitones = 48.0;

inline constexpr const char* kEmphShapeNames[] = { "Tilt", "Bell" };
inline constexpr const char* kFbTapNames[] = { "Clean", "Driven" };
inline constexpr const char* kSlotRouteNames[] = { "Series", "Parallel" };

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
	{ "eroder_dc", "2026-11-18", "replaced by an on/off DC block (eroder_dc_block)" },
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
     .gateMask = kShakersAndEtch},
    {.key = "eroder_width", .name = "Eroder Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How wide the noise band is, and how wide the Bell is: narrow is a wavering tone around Freq, wide is plain "
                "hiss; inactive while Source is all sine and the Emphasis is not a Bell above or below 0 dB. It does nothing to "
                "the sine",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch},
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
     .tooltip = "Feeds the shaken signal back round the delay, so it rings at a pitch Amount sets (in Rust, one "
                "period of Freq). Left of centre the feedback is inverted, which rings an octave lower and hollow",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch,
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
     .gateMask = kShakersAndEtch,
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
     .tooltip = "What Drive pushes into. (w) marks the Types that add stereo width. Soft starves the quiet parts and rounds the rest, Tube's two halves "
	                "differ, Fold is a sine folder that can lean to one side, Wreck is a distortion into a folder "
	                "(in Dust the feedback runs through both), Jag is a distortion into a sharp folder, Lean (w) "
	                "leans left and right opposite ways, so its even harmonics are stereo width, Chonk (w) puts "
	                "its odd harmonics in opposite polarity left and right and thins, then flips, its right side as Drive rises, Paunch-S (w) adds the rectified mid to the "
	                "left and takes it from the right: even harmonics that are stereo width and vanish in mono; "
	                "Paunch-H (w) does it with the square, a pure octave above, Paunch-OS (w) adds a third harmonic sized to the signal's own level to one side and takes it from the other, Paunch-OH (w) does the same with the plain cube, so it grows quickly with level: quiet passages stay clean and loud ones spread, Crush holds and rounds each sample, "
	                "Decimate holds each sample, as steps or as lines, Slew limits how fast the signal may move, "
	                "Lopside lets it rise and fall at different speeds",
	     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Type", .choiceAliases = kDriveTypeAliases},
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
	                "how far Wreck and Jag fold, how wide Lean leans, how much of Chonk (w)'s right side is turned over, "
	                "(the Paunch Types read their own Angle instead), "
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
                "filter. With Track on, a distance in semitones from the played note takes this control's place",
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
     .tooltip = "Gives back from the dry signal what HP Wet took away, so the low end stays whole at any Mix; inactive while HP "
                "Wet is at its bottom, where there is no filter, and in the Comb while Blend is off",
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
     .tooltip = "Runs the right side's sine above the left's, up to an octave, so the two sides beat against each other; "
                "inactive while Source is all noise, which has no sine to detune",
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
     .tooltip = "Pushes the sine past its peaks and folds it back, which adds overtones to the shake; inactive while Source is "
                "all noise, which has no sine to fold",
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
    {.key = "eroder_emph_amount", .name = "Eroder Emphasis",
     .min = -kEmphasisMaxDb,
     .max = kEmphasisMaxDb,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Decibel,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Tilts or bends the signal on its way into the effect, and (with Restore) back on its way out, so what "
                "the Drive and the loops bite on is not what you hear. At zero neither filter runs",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Emphasis"},
    {.key = "eroder_emph_shape", .name = "Eroder Emphasis Shape",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kEmphShapeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Tilt lifts one side of Freq and lowers the other; Bell lifts or lowers a band centred on Freq, as wide as "
                "Width says (a quarter of an octave at the bottom, three at the top); inactive while Emphasis is at 0 dB",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Shape"},
    {.key = "eroder_emph_restore", .name = "Eroder Restore",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Puts the opposite filter after the Drive, so the effect's tone comes back; inactive while Emphasis is at 0 dB. "
                "Off, the emphasis stays in the sound, and only what the Drive made of the emphasised signal is left",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Restore",
     .stacked = true},
    {.key = "eroder_emph_in_loop", .name = "Eroder In Loop",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Moves the emphasis to after the feedback joins the signal, so every trip round a loop goes through it again; "
                "inactive while Emphasis is at 0 dB, or while Reso and Loop are both at 0. The loops' limiters hold what "
                "circulates, so a loop set high howls at a level and does not run away",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "In Loop",
     .stacked = true},
    {.key = "eroder_smear", .name = "Eroder Smear",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Eight allpass filters centred on Freq: the level stays where it is and the timing is thrown out of step "
                "about Freq, so a click becomes a chirp and a square's corners come apart. Inside the loops",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Smear"},
    {.key = "eroder_fb_tap", .name = "Eroder Tap",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kFbTapNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Where the feedback is taken, inactive while Reso is at 0 (the Comb's feedback always runs): Clean, before the "
                "Drive, so the Drive colours only what you hear; Driven, after it, so the loop rings through the Drive, and "
                "a hot Drive makes it ring harder",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Tap"},
    {.key = "eroder_loop", .name = "Eroder Loop",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "A second feedback, from after the Drive through a delay of one period of Freq, moved by Interval, and "
                "a limiter. Its top is the edge of ringing for ever; it never goes past it",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Loop"},
    {.key = "eroder_loop_interval", .name = "Eroder Interval",
     .min = -kLoopIntervalSemitones,
     .max = kLoopIntervalSemitones,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far above (or, below zero, under) the pitch of Freq the outer loop rings: its delay is one period of Freq "
                "divided by two for every twelve semitones; inactive while Loop is at 0",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Interval",
     .suffix = "st"},
    {.key = "eroder_slot_a_amount", .name = "Eroder Slot A Amount",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How hard slot A's Type works on the signal, as Drive does: the gain into a curve, how long a sample is held, how slow a slew is. At the bottom the slot is not run",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "A"},
    {.key = "eroder_slot_a_type", .name = "Eroder Slot A Type",
     .min = 0.0,
     .max = static_cast<double>(static_cast<int>(DriveMode::kCount) - 1),
     .def = static_cast<double>(DriveMode::Slew),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kDriveModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "What slot A pushes the signal into: any of the Drive's Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Type", .choiceAliases = kDriveTypeAliases},
    {.key = "eroder_slot_a_shape", .name = "Eroder Slot A Shape",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The Type's own colour in slot A, as Drive's Shape is",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Shape"},
    {.key = "eroder_slot_a_mix", .name = "Eroder Slot A Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of slot A's result is heard against what went into it. At the bottom the slot is not run",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Mix"},
    {.key = "eroder_slot_b_amount", .name = "Eroder Slot B Amount",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How hard slot B's Type works on the signal, as Drive does: the gain into a curve, how long a sample is held, how slow a slew is. At the bottom the slot is not run",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "B"},
    {.key = "eroder_slot_b_type", .name = "Eroder Slot B Type",
     .min = 0.0,
     .max = static_cast<double>(static_cast<int>(DriveMode::kCount) - 1),
     .def = static_cast<double>(DriveMode::Decimate),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kDriveModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "What slot B pushes the signal into: any of the Drive's Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Type", .choiceAliases = kDriveTypeAliases},
    {.key = "eroder_slot_b_shape", .name = "Eroder Slot B Shape",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The Type's own colour in slot B, as Drive's Shape is",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Shape"},
    {.key = "eroder_slot_b_mix", .name = "Eroder Slot B Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of slot B's result is heard against what went into it. At the bottom the slot is not run",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Mix"},
    {.key = "eroder_slot_route", .name = "Eroder Slot Route",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kSlotRouteNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Series puts slot B after slot A. Parallel gives both the same input and adds what each changed",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kEtchOnly,
     .centred = false,
     .shortName = "Route"},
    {.key = "eroder_emph_offset", .name = "Eroder Emphasis Offset",
     .min = -kEmphOffsetSemitones,
     .max = kEmphOffsetSemitones,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Moves the emphasis away from Freq: it is tuned this many semitones above (or, below zero, under) it. "
                "Smear and the loops stay on Freq",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Offset",
     .suffix = "st"},
    {.key = "eroder_input", .name = "Eroder Input",
     .min = -kInputMaxDb,
     .max = kInputMaxDb,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Decibel,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "A gain into the effect: it raises the level of what the wet chain hears, and how hard everything after it "
                "is driven. The dry, Sub Thru and Follow take the signal before it",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Input"},
    {.key = "eroder_hp_wet_semis", .name = "Eroder HP Wet Semitones",
     .min = kHpWetSemisMin,
     .max = kHpWetSemisMax,
     .def = kHpWetSemisMin,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "While Track is on, sets the wet highpass this many semitones above the played note (0 is the note itself, "
                "12 an octave above). At the bottom there is no filter. With Track off, HP Wet's frequency applies instead",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch,
     .centred = false,
     .shortName = "HP Wet",
     .offAtEnd = -1,
     .suffix = "st"},
    {.key = "eroder_dc_block", .name = "Eroder DC Block",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Removes the offset the shaper leaves (below 10 Hz); inactive unless a running Drive or slot Type makes one (Tube, Fold, Lean, Paunch-S or Paunch-H). "
                "Off lets it through to whatever follows. The loops never carry it",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "DC"},
    {.key = "eroder_drive_bal", .name = "Eroder Drive Balance",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Chonk (w) only: how much of the level it adds to one side more than the other is taken out. At 100% the two sides "
                "keep the same level whatever the material, with the width left in; at 0% Chonk is exactly as it was, and its right side "
                "can sound quieter or louder than the left. Shown only while the Drive is on Chonk (w); an Etch slot's Chonk (w) is always at 100%",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Bal"},
    {.key = "eroder_drive_rate", .name = "Eroder Drive Rate",
     .min = kDriveRateLog2Min,
     .max = kDriveRateLog2Max,
     .def = kDriveRateLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: how fast the angle of the addition turns, so no side leans on average. At the bottom it is off and "
                "Shape's angle holds still. Shown only while the Drive is on one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_slot_a_rate", .name = "Eroder Slot A Rate",
     .min = kDriveRateLog2Min,
     .max = kDriveRateLog2Max,
     .def = kDriveRateLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: how fast the angle of the addition turns, so no side leans on average. At the bottom it is off and "
                "Shape's angle holds still. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_slot_b_rate", .name = "Eroder Slot B Rate",
     .min = kDriveRateLog2Min,
     .max = kDriveRateLog2Max,
     .def = kDriveRateLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: how fast the angle of the addition turns, so no side leans on average. At the bottom it is off and "
                "Shape's angle holds still. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_slot_a_bal", .name = "Eroder Slot A Balance",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Chonk (w) only: how much of the level it adds to one side more than the other is taken out. At 100% the two sides "
                "keep the same level whatever the material, with the width left in; at 0% Chonk is exactly as it was, and its right side "
                "can sound quieter or louder than the left. Shown in a slot's cell only while its Type is Chonk (w)",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Bal"},
    {.key = "eroder_slot_b_bal", .name = "Eroder Slot B Balance",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Chonk (w) only: how much of the level it adds to one side more than the other is taken out. At 100% the two sides "
                "keep the same level whatever the material, with the width left in; at 0% Chonk is exactly as it was, and its right side "
                "can sound quieter or louder than the left. Shown in a slot's cell only while its Type is Chonk (w)",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Bal"},
    {.key = "eroder_drive_rate_mult", .name = "Eroder Drive Rate Multiple",
     .min = 0.0,
     .max = 1.0,
     .def = kRateMultDefault,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "With Track on, how fast the angle turns as a multiple of the played note's frequency: 0.5 is an octave below it, 0.25 two. "
                "The addition's partials then sit on the harmonics of that frequency. Drag lands on 1/16, 1/12, 1/8, 1/6, 1/5, 1/4, 1/3, 1/2 and 1; "
                "type any value in. Off (0) holds the angle still. With Track off the rate in hertz applies instead. Shown only while the Drive is on one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_slot_a_rate_mult", .name = "Eroder Slot A Rate Multiple",
     .min = 0.0,
     .max = 1.0,
     .def = kRateMultDefault,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "With Track on, how fast the angle turns as a multiple of the played note's frequency: 0.5 is an octave below it, 0.25 two. "
                "The addition's partials then sit on the harmonics of that frequency. Drag lands on 1/16, 1/12, 1/8, 1/6, 1/5, 1/4, 1/3, 1/2 and 1; "
                "type any value in. Off (0) holds the angle still. With Track off the rate in hertz applies instead. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_slot_b_rate_mult", .name = "Eroder Slot B Rate Multiple",
     .min = 0.0,
     .max = 1.0,
     .def = kRateMultDefault,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "With Track on, how fast the angle turns as a multiple of the played note's frequency: 0.5 is an octave below it, 0.25 two. "
                "The addition's partials then sit on the harmonics of that frequency. Drag lands on 1/16, 1/12, 1/8, 1/6, 1/5, 1/4, 1/3, 1/2 and 1; "
                "type any value in. Off (0) holds the angle still. With Track off the rate in hertz applies instead. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kShakersAndEtch,
     .centred = false,
     .shortName = "Rate",
     .offAtEnd = -1},
    {.key = "eroder_drive_angle", .name = "Eroder Drive Angle",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: the angle of the addition. 0% puts it in opposite sides, 25% in both the same, 50% in opposite sides the other way, "
                "75% in both inverted. A rotation rate turns it from there. It has a key of its own because Shape keeps its old meaning for every other Type. Shown only while the Drive is on one of the four Paunch (w) Types, in the place of Shape",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "S"},
    {.key = "eroder_slot_a_angle", .name = "Eroder Slot A Angle",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: the angle of the addition. 0% puts it in opposite sides, 25% in both the same, 50% in opposite sides the other way, "
                "75% in both inverted. A rotation rate turns it from there. It has a key of its own because Shape keeps its old meaning for every other Type. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types, in the place of Shape",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "S"},
    {.key = "eroder_slot_b_angle", .name = "Eroder Slot B Angle",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Paunch Types only: the angle of the addition. 0% puts it in opposite sides, 25% in both the same, 50% in opposite sides the other way, "
                "75% in both inverted. A rotation rate turns it from there. It has a key of its own because Shape keeps its old meaning for every other Type. Shown in a slot's cell only while its Type is one of the four Paunch (w) Types, in the place of Shape",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "S"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

/// ONE OF ETCH'S TWO SLOTS: a Drive stage with its own four controls.
struct SlotSettings {
	float amount = 0.0f;
	DriveMode type = DriveMode::Slew;
	float shape = 0.0f;
	float mix = 1.0f;
	/// THE PAUNCH TYPES' ANGLE (turns, 0..1): read in place of `shape` by those four Types (`driveCoordinate`), whose old Shape key keeps its meaning for every other Type.
	float angle = 0.0f;
	/// THE TYPE'S SECOND PARAMETER, as the Drive's: the Paunch Types' rotation rate (log2 hertz, the bottom is off) and Chonk's balance (0..1).
	/// THE STRUCT'S DEFAULT IS OFF (a hand-built `Settings` holds the angle still); the CONTROL's default, which a patch lacking the key and a
	/// fresh instance read, is `kDriveRateLog2Default`.
	float rate = static_cast<float>(kDriveRateLog2Min);
	float bal = 1.0f;
	float rateMult = 0.0f; ///< with Track on, the multiple of the played note's frequency in place of `rate` (the struct's default is off, the control's `kRateMultDefault`)
	friend bool operator==(const SlotSettings&, const SlotSettings&) = default;
};

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
	/// THE PAUNCH TYPES' ANGLE, read in place of `driveShape` by those four Types (`driveCoordinate`).
	float driveAngle = 0.0f;
	float hpWet = static_cast<float>(kHpWetLog2Min); ///< log2 hertz; the bottom is no filter
	float hpWetSemis = static_cast<float>(kHpWetSemisMin); ///< with Track on, instead of hpWet: semitones from the note; the bottom is no filter
	bool subThru = false;
	float follow = 0.0f;
	float rustBias = 0.0f;
	float rustRectify = 0.0f;
	float step = static_cast<float>(kStepLog2Min); ///< log2 seconds; the bottom is no hold
	float detune = 0.0f;
	float modFold = 0.0f;
	float heat = 0.0f;
	bool blend = false;
	// THE SHARED SECTION.
	float emphAmount = 0.0f; ///< dB
	bool emphBell = false;   ///< false is Tilt
	bool emphRestore = true;
	bool emphInLoop = false;
	float smear = 0.0f;
	bool tapDriven = false;
	float loop = 0.0f;
	float loopInterval = 0.0f; ///< semitones
	float emphOffset = 0.0f;   ///< semitones from Freq to the emphasis's tuning
	float inputDb = 0.0f;      ///< gain on the signal entering the wet chain
	// ETCH'S SLOTS.
	SlotSettings slotA { 0.0f, DriveMode::Slew, 0.0f, 1.0f };
	SlotSettings slotB { 0.0f, DriveMode::Decimate, 0.0f, 1.0f };
	bool slotParallel = false;
	/// THE DC BLOCK, 1 (On) or 0 (Off) (`eroder_dc_block`): whether the one post high-pass takes the offset the shapers make out after the Drive.
	float dc = 1.0f;
	/// CHONK'S BALANCE, 0..1 (`eroder_drive_bal`): read only when the Drive's Type is Chonk (`DriveSettings::balance`).
	float driveBal = 1.0f;
	/// THE PAUNCH TYPES' ROTATION LFO RATE, log2 hertz (`eroder_drive_rate`); the bottom is off.
	float driveRate = static_cast<float>(kDriveRateLog2Min); ///< off in a hand-built `Settings`; the control's default is `kDriveRateLog2Default`
	float driveRateMult = 0.0f; ///< with Track on, the multiple of the played note's frequency in place of `driveRate`

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

/// **WHAT THE SHARED SECTION'S LOOPS RUN ON** (`loops.h`): a pure function of the settings, the engine's Freq and the Drive's
/// gain, so each of the stability rules is an equality on a struct.
struct LoopDerived {
	bool tapDriven = false;
	bool inLoop = false;  ///< In Loop is on AND there is an emphasis
	bool emphOn = false;
	/// The inner feedback runs through the limiter: Driven, or the emphasis in the loop.
	bool limitInner = false;
	/// The inner loop's gain, signed, as set: nothing is divided out of it.
	double innerFeedback = 0.0;
	/// `loop * kLoopMaxGain`; exactly 0 at Loop 0.
	double loopGain = 0.0;
	/// One period of Freq, and the outer delay: that times `2^(-interval / 12)`. Seconds, as set: the Smear's own delay adds inside the loop.
	double periodSeconds = 0.0;
	double outerSeconds = 0.0;
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
	/// THE OFFSET IS LEFT IN: the stage does not run its own DC removal (`DriveStage`). The Etch slots always pass their offset
	/// forward; the one DC stage sits after the Drive (`dcAmount`). False by default, so a stage used alone removes its own.
	bool leaveDc = false;
	/// HOW MUCH OF THE STAGE'S OWN OFFSET IT REMOVES, 0..1 (the Drive's `eroder_dc`; it glides). The estimate of the offset runs at
	/// any amount, so the loops' taps can take the fully blocked signal (`DriveStage::fullyBlocked`).
	float dcAmount = 1.0f;
	/// CHONK'S BALANCE, 0..1 (`eroder_drive_bal`): how much of the mid-correlated part of what Chonk adds to the side is taken out (`widthBalance`).
	/// The other width Types always take all of it; an Etch slot's Chonk is fixed at 1.
	float balance = 1.0f;
	/// THE PAUNCH TYPES' ROTATION LFO, in hertz; 0 holds the angle `shape` names still (a slot's is always 0).
	float rateHz = 0.0f;
};
/// **THE TYPES THAT MAKE AN OFFSET**: Tube, Fold, Lean (its halves are unequal) and Paunch-S and Paunch-H (a rectified or squared mid is
/// never negative). The one DC stage after the Drive takes it out, whichever stage made it.
inline constexpr bool driveMakesOffset(DriveMode mode) {
	return mode == DriveMode::Tube || mode == DriveMode::Fold || mode == DriveMode::Lean || driveIsPaunchLaw(mode);
}
/// WHETHER A STAGE THAT IS RUN MAKES AN OFFSET.
[[nodiscard]] inline bool makesOffset(const DriveSettings& s) noexcept {
	return s.driven && s.mix > 0.0f && driveMakesOffset(s.mode);
}
/// WHETHER ANY STAGE THE CONTROLS RUN MAKES AN OFFSET: the Drive, or in Etch a slot with an Amount and a Mix.
[[nodiscard]] inline bool chainMakesOffset(const Settings& s) noexcept {
	const auto slot = [](const SlotSettings& t) { return t.amount > 0.0f && t.mix > 0.0f && driveMakesOffset(t.type); };
	return (s.drive > 0.0f && driveMakesOffset(s.driveMode)) || (s.mode == Mode::Etch && (slot(s.slotA) || slot(s.slotB)));
}
/// WHETHER THE DC STAGE IS RUNNING for these controls: some stage makes an offset and the amount is above 0.
[[nodiscard]] inline bool dcStageRuns(const Settings& s) noexcept {
	return s.dc > 0.0f && chainMakesOffset(s);
}
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
	/// ETCH: no shake and no line; two slots, each its own stage's settings with the mix as the control gave it (Follow
	/// scales it per sample), and whether they run side by side.
	bool etch = false;
	DriveSettings slotA {};
	DriveSettings slotB {};
	bool slotParallel = false;
	/// THE SHARED SECTION'S LOOPS, for this engine's Freq.
	LoopDerived loops {};
};

/// @param reuse  settings this may take the make-up from when the Drive, Type and Shape are the same — the make-up
///               is a quadrature, and the control path asks for it every block.
/// @param mix    the stage's own mix, for a caller that sets one; the Drive control itself is always at 1
[[nodiscard]] DriveSettings deriveDrive(float drive, DriveMode mode, float shape, double sampleRate,
                                        const DriveSettings* reuse = nullptr, float mix = 1.0f) noexcept;
/// @param centreHz   the engine's Freq (the Comb's own pitch)
/// @param innerFeedbackRaw  the engine's inner loop gain, signed
[[nodiscard]] LoopDerived deriveLoops(const Settings& settings, double centreHz, double innerFeedbackRaw) noexcept;

/// The shared wet highpass's settings (`fx/wet_highpass.h`).
using HighpassSettings = fx::HighpassSettings;
[[nodiscard]] HighpassSettings deriveHighpass(float hpWetLog2Hz, double sampleRate) noexcept;
/// **THE WET HIGHPASS THE CONTROLS ASK FOR**: with `tracks` (Track on in an engine that has it) the corner is the played note's
/// frequency times 2^(semitones / 12), clamped to HP Wet's own range (20 Hz to 8 kHz), and HP Wet's hertz is ignored; off at
/// the semitones' minimum. Without `tracks` HP Wet's hertz applies and the semitones are ignored. THE NOTE'S FREQUENCY is equal
/// temperament with MIDI note 69 at 440 Hz (`pitchSemitones` is a MIDI note number; 60 is middle C).
[[nodiscard]] HighpassSettings deriveWetHighpass(const Settings& settings, bool tracks, double pitchSemitones,
                                                 double sampleRate) noexcept;
/// Whether the wet highpass is in the signal for these controls: what Sub Thru and the picture read.
[[nodiscard]] bool wetHighpassOn(const Settings& settings, bool tracks) noexcept;

/// @param reuse  a result this one may take the shaper's make-up from when the Drive, Type and Shape are the same —
///               the make-up is a quadrature, and the control path asks for it every block.
[[nodiscard]] DustDerived deriveDust(const Settings& settings, double pitchSemitones, double sampleRate,
                                     const DustDerived* reuse = nullptr) noexcept;

/// **THE SHAPER'S CURVE**, of a signal already multiplied by the Drive's gain `gain`: Soft, Tube, Fold, Wreck, Jag, Lean and Chonk (its left).
/// The two Paunch laws are not curves of one channel (`paunchLaw`) and are the identity here.
/// Every one is 0 at 0; all but Soft at Shape above 0 (whose dead zone is a share of the reference level, so it needs
/// the gain) go straight through it. The Types that are not curves are the identity here.
[[nodiscard]] float shapeDrive(DriveMode mode, float driven, float shape, float gain) noexcept;
/// THE RIGHT CHANNEL'S CURVE: the left's, except in Lean, where it is the left's turned over (`-f(-u)`) — the same
/// odd harmonics, the even ones in the other polarity.
[[nodiscard]] float shapeDriveRight(DriveMode mode, float driven, float shape, float gain) noexcept;
/// **THE PAUNCH LAWS' SQUARE'S FIXED COEFFICIENT.** With the mid `m = P sin`, Paunch-S's `a |m|` has a 2nd harmonic of amplitude
/// `a P 4 / (3 pi)` (the rectified sine's 2nd term) and Paunch-H's `kappa (a m)^2` one of `kappa (a P)^2 / 2` (the square is
/// `(1 - cos 2wt) / 2`). Equal at full Amount (`a = 4`) at `P = kDriveReferencePeak`: `kappa (4 P)^2 / 2 = 4 P 4 / (3 pi)`, so
/// `kappa = 8 / (3 pi 4 P) = 4 / (3 pi)` for `P = 0.5` (0.4244).
inline constexpr double kPaunchAmountMax = 4.0;
inline constexpr double kPaunchSquareK = 8.0 / (3.0 * std::numbers::pi * kPaunchAmountMax * kDriveReferencePeak);
/// THE LAW'S SIGNAL `s` for a mid `m` and amount `a` (Paunch-S: `a |m|`; Paunch-H: `kappa (a m)^2`; else 0). Even in `m`, never negative.
[[nodiscard]] inline float paunchSide(DriveMode mode, float m, float a) noexcept {
	if (mode == DriveMode::PaunchS) {
		return a * std::abs(m);
	}
	if (mode == DriveMode::PaunchH) {
		const float am = a * m;
		return static_cast<float>(kPaunchSquareK) * am * am;
	}
	return 0.0f;
}
/// **THE ADDITION'S ANGLE** (the four Paunch Types): Shape is an angle `phi0 = 2 pi shape`, and the rotation LFO adds its own turns. With `s`
/// the Type's addition,  `l += s (cos phi + sin phi)`,  `r += s (-cos phi + sin phi)`:
///   shape 0     opposite sides (left +, right -): all in the side, none in the mid; the mono sum is the dry's,
///   shape 0.25  both the same: all in the mid, none in the side,
///   shape 0.5   opposite the other way (left -, right +),
///   shape 0.75  both inverted.
/// The two gains are a rotation of the pair (mid, side) of the addition, so the energy added, `(dl)^2 + (dr)^2 = 2 s^2`, does not depend on
/// the angle. The dry is untouched in every case. Amount 0 is none. `a` is the Amount's linear 0..4. The caller removes the offset `s` carries.
struct Rotation {
	float left = 1.0f;  ///< cos + sin
	float right = -1.0f; ///< -cos + sin
};
/// **THE COORDINATE A TYPE READS**: the four Paunch Types read their own Angle, every other Type reads Shape with the meaning it always had.
/// (v0.2.1's Paunch Shape was a weight, `1 - 2 shape`: side below the middle, none at 0.5, mid above; a patch's Shape on a Paunch Type is not read now.)
[[nodiscard]] inline float driveCoordinate(DriveMode mode, float shape, float angle) noexcept {
	return driveIsPaunchLaw(mode) || driveIsPaunchOdd(mode) ? angle : shape;
}
/// **THE TYPE'S SECOND PARAMETER** — the third row of the Drive's cell and of a slot's: Bal for Chonk, Rate for the four Paunch Types, none for
/// every other Type (the row is then not drawn). The one small table the cells read; more Types are added here.
enum class SecondParam { None, Bal, Rate };
[[nodiscard]] inline constexpr SecondParam driveSecondParam(DriveMode mode) noexcept {
	if (mode == DriveMode::Chonk) {
		return SecondParam::Bal;
	}
	return driveIsPaunchLaw(mode) || driveIsPaunchOdd(mode) ? SecondParam::Rate : SecondParam::None;
}
/// THE ROTATION LFO'S RATE in hertz from its control (log2 hertz, the bottom is off): 0 is off.
[[nodiscard]] inline float driveRateHz(float log2Hz) noexcept {
	return static_cast<double>(log2Hz) <= kDriveRateLog2Min + device::kOffEndSlack ? 0.0f : static_cast<float>(std::exp2(static_cast<double>(log2Hz)));
}
/// THE TRACKED NOTE'S FREQUENCY in hertz: the note as MIDI (semitones) at A440. HP Wet's semitone control and the rotation's subharmonic read it.
[[nodiscard]] inline double trackedNoteHz(double pitchSemitones) noexcept {
	return 440.0 * std::exp2((pitchSemitones - 69.0) / 12.0);
}
/// THE ROTATION LFO'S RATE for a stage, in hertz: with Track on the played note's frequency times the multiple (0 is off), with it off the
/// Hz control's (`driveRateHz`). Held under a quarter of the rate it runs at.
[[nodiscard]] inline float rotationRateHz(bool tracks, double pitchSemitones, float log2Hz, float mult, double sampleRate) noexcept {
	float hz = 0.0f;
	if (tracks) {
		hz = mult > 0.0f ? static_cast<float>(trackedNoteHz(pitchSemitones) * static_cast<double>(std::min(mult, 1.0f))) : 0.0f;
	}
	else {
		hz = driveRateHz(log2Hz);
	}
	const double ceiling = 0.25 * (sampleRate > 0.0 ? sampleRate : 48000.0);
	return std::isfinite(hz) ? static_cast<float>(std::min(static_cast<double>(hz), ceiling)) : 0.0f;
}
/// THE ANGLE'S GAINS for `shape` (turns) and the LFO's `phase` (turns); exact at shape 0 (1, -1), so Shape 0 is what it always was.
[[nodiscard]] inline Rotation rotationOf(float shape, double phase = 0.0) noexcept {
	const double turns = static_cast<double>(shape) + phase;
	const double phi = 2.0 * std::numbers::pi * (turns - std::floor(turns));
	const double c = std::cos(phi);
	const double sn = std::sin(phi);
	return { static_cast<float>(c + sn), static_cast<float>(-c + sn) };
}
inline void paunchSpread(float& l, float& r, float s, const Rotation& rot) noexcept {
	l += rot.left * s;
	r += rot.right * s;
}
inline void paunchLaw(DriveMode mode, float& l, float& r, float a, const Rotation& rot) noexcept {
	paunchSpread(l, r, paunchSide(mode, 0.5f * (l + r), a), rot);
}
inline void paunchLaw(DriveMode mode, float& l, float& r, float a, float shape) noexcept {
	paunchLaw(mode, l, r, a, rotationOf(shape));
}
/// **THE LEVEL-TRACKED ODD LAW'S FOLLOWER**: a one-pole on the power of the mid, `kPaunchFollowSeconds` long at the rate the stage
/// runs at (`paunchFollowCoefficient`), per-sample state. The divisor is floored at `kPaunchFollowFloor` so silence divides by
/// nothing, the state is flushed to exact zero under `kPaunchFollowFlush` (a decay cannot park in the denormals), and the ratio
/// `q = m^2 / (2 P)` is capped at `kPaunchFollowCap`: for a steady sine (`P = A^2 / 2`) `q` is at most 1, so the cap never touches
/// it; it bounds peaks and onsets (a signal that starts from silence meets `P` near 0, so `q` is the cap and `|s| <= 1.25 a |m|`).
/// The time constant is 60 ms, not 30: the follower of a sine's power ripples at twice its frequency (`1 / (2 w tau)` of the mean
/// at 220 Hz: 1.2 % at 30 ms), which leaks a fundamental into the side at about `-38 dB` of the mid's at `a = 4`; at 60 ms that
/// is `-44 dB` and the two sides' fundamentals agree to 0.1 dB.
inline constexpr double kPaunchFollowSeconds = 0.060;
inline constexpr float kPaunchFollowCap = 2.0f;
inline constexpr float kPaunchFollowFloor = 1.0e-12f;
inline constexpr float kPaunchFollowFlush = 1.0e-20f;
[[nodiscard]] inline float paunchFollowCoefficient(double rate) noexcept {
	return static_cast<float>(1.0 - std::exp(-1.0 / (kPaunchFollowSeconds * (rate > 0.0 ? rate : 48000.0))));
}
/// **PAUNCH-OH'S COEFFICIENT** `kappa3`: its 3rd harmonic is `kappa3 a^3 A^3 / 4` (`m^3 - 1.5 P m = -(A^3 / 4) sin 3t` for `P = A^2 / 2`)
/// against Paunch-OS's `a A / 4`; equal at full Amount (`a = 4`) and the reference peak `A = P_ref` when
/// `kappa3 (4 A)^2 = 1`, so `kappa3 = 1 / (a_max^2 A_ref^2) = 1 / (16 * 0.25) = 0.25`.
inline constexpr float kPaunchCubeCeiling = 4.0f;
inline constexpr double kPaunchCubeK = 1.0 / (kPaunchAmountMax * kPaunchAmountMax * kDriveReferencePeak * kDriveReferencePeak);
/// **THE ODD LAWS, ONE FRAME, in place**, with the follower's state `power` (the mean square of the mid) advanced by one sample.
/// Paunch-OS: `s = a m (q - 0.75)`, `q = min(m^2 / (2 P), 2)`. For `m = A sin t`, `P = A^2 / 2`: `m^3 / A^2 - 0.75 m` is
/// `-(A / 4) sin 3t`, so `s` is a pure 3rd at `a / 4` of the fundamental at any level, with no fundamental in it and no offset
/// (`s` is odd). Spread to the two sides as the even laws are (`paunchSpread`). Everything is a function of the mid, so the
/// two channels share one follower.
inline void paunchOdd(DriveMode mode, float& l, float& r, float a, const Rotation& rot, float& power, float coefficient) noexcept {
	const float m = 0.5f * (l + r);
	const float m2 = m * m;
	power += (m2 - power) * coefficient;
	if (power < kPaunchFollowFlush) {
		power = 0.0f;
	}
	const float p = power > kPaunchFollowFloor ? power : kPaunchFollowFloor;
	const float q = std::min(m2 / (2.0f * p), kPaunchFollowCap);
	float s = 0.0f;
	if (mode == DriveMode::PaunchOS) {
		s = a * m * (q - 0.75f);
	}
	else if (mode == DriveMode::PaunchOH) {
		// THE CUBE'S ARGUMENT IS HELD AT +-4 (+12 dBFS): a cube is unbounded, and with the angle turned toward the mid its product is fed to the next
		// stage and round the loops, where three of them in a row overflow a float in a few thousand samples. Nothing below +12 dBFS meets it.
		const float mc = std::clamp(m, -kPaunchCubeCeiling, kPaunchCubeCeiling);
		s = static_cast<float>(kPaunchCubeK) * (a * a * a) * (mc * mc * mc - 1.5f * power * mc);
	}
	paunchSpread(l, r, s, rot);
}
inline void paunchOdd(DriveMode mode, float& l, float& r, float a, float shape, float& power, float coefficient) noexcept {
	paunchOdd(mode, l, r, a, rotationOf(shape), power, coefficient);
}
/// **THE WIDTH TYPES' LEVEL TRIM.** Lean and Chonk add something to the two sides that is not the same on both, so
/// one side can come out louder than the other (the four Paunch Types have the rotation LFO for that). The stage must not change the balance of what came in: this keeps it, slowly.
///   `Pin`, `Pout` are the running mean squares of the stage's input and of the law's output, each side (one-pole, `kTrimSeconds`);
///   `k = sqrt((Pin_L / Pin_R) / (Pout_L / Pout_R))` is how much louder the left should be, as an amplitude ratio;
///   `g_L = k^(amount / 2)`, `g_R = 1 / g_L` (split evenly, so the overall level is unchanged); `l' = g_L l`, `r' = g_R r`.
/// It is a gain, so it never touches the spectrum. The mean squares are taken on DC-removed copies (the stage's own `kDriveDcHz` one-pole),
/// so the offsets the Paunch laws make do not read as level; the gains are applied to the full signals. The one thing it gives up: with
/// unequal gains the mono sum is `M (g_L + g_R) / 2 + S (g_L - g_R) / 2`, a little of the side in the mid.
/// Where either input side is under `kTrimPanRatio` of the other (a hard-panned input) or any power is under the floor (silence), `k` is 1,
/// and it is clamped to +-36 dB. The means start as true means (the weight of the n-th frame is `1 / n` until that falls to the one-pole's
/// own), so an onset from silence is not left to a slow average. Per frame, so a host that cuts its blocks anywhere gets the same bits;
/// `amount` is 1 for every Type but Chonk (its Bal); at 0 the law's output is returned untouched, to the bit.
inline constexpr double kTrimSeconds = 0.100;
inline constexpr float kTrimFloor = 1.0e-14f;
inline constexpr float kTrimFlush = 1.0e-20f;
inline constexpr float kTrimPanRatio = 1.0e-4f;
inline constexpr float kTrimMax = 64.0f; // +-36 dB: Chonk's right side at full Drive can sit 25 dB and more under the left, which +-12 dB could not reach
struct WidthTrim {
	float pin[2] = {};
	float pout[2] = {};
	float dcIn[2] = {};
	float dcOut[2] = {};
	float count = 0.0f;
	float k = 1.0f; ///< the last frame's k (a test seam)
	float coefficient = 0.0f;
	float dcCoefficient = 0.0f;
	void prepare(double rate, double seconds = kTrimSeconds) noexcept {
		const double r = rate > 0.0 ? rate : 48000.0;
		coefficient = static_cast<float>(1.0 - std::exp(-1.0 / (seconds * r)));
		dcCoefficient = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / r));
	}
	void reset() noexcept {
		for (int c = 0; c < 2; ++c) {
			pin[c] = pout[c] = dcIn[c] = dcOut[c] = 0.0f;
		}
		count = 0.0f;
		k = 1.0f;
	}
};
/// WHICH TYPES RUN THE TRIM: Lean (always on) and Chonk (by its Bal). The four Paunch Types have the rotation LFO instead.
[[nodiscard]] inline bool driveBalances(DriveMode mode, float) noexcept {
	return mode == DriveMode::Lean || mode == DriveMode::Chonk;
}
/// `forceUnity`: a test seam that makes the target ratio 1 (it centres the output instead of keeping the input's balance).
inline void widthTrim(WidthTrim& s, float lIn, float rIn, float& l, float& r, float amount, bool forceUnity = false) noexcept {
	if (s.count == 0.0f && lIn == 0.0f && rIn == 0.0f && l == 0.0f && r == 0.0f) {
		return; // nothing has been heard yet: nothing to learn from, and a zero stays zero
	}
	const float in[2] = { lIn, rIn };
	const float out[2] = { l, r };
	const float n = s.count + 1.0f;
	const float w = std::max(s.coefficient, 1.0f / n);
	if (w > s.coefficient) {
		s.count = n;
	}
	for (int c = 0; c < 2; ++c) {
		s.dcIn[c] += (in[c] - s.dcIn[c]) * s.dcCoefficient;
		s.dcOut[c] += (out[c] - s.dcOut[c]) * s.dcCoefficient;
		const float xi = in[c] - s.dcIn[c];
		const float xo = out[c] - s.dcOut[c];
		s.pin[c] += (xi * xi - s.pin[c]) * w;
		s.pout[c] += (xo * xo - s.pout[c]) * w;
	}
	if (s.pin[0] < kTrimFlush && s.pin[1] < kTrimFlush && s.pout[0] < kTrimFlush && s.pout[1] < kTrimFlush) {
		s.reset(); // a decay cannot park in the denormals, and the next onset starts from true means
		return;
	}
	float k = 1.0f;
	const bool live = s.pin[0] >= kTrimFloor && s.pin[1] >= kTrimFloor && s.pout[0] >= kTrimFloor && s.pout[1] >= kTrimFloor &&
	                  s.pin[0] >= kTrimPanRatio * s.pin[1] && s.pin[1] >= kTrimPanRatio * s.pin[0];
	if (live) {
		const float ratioIn = forceUnity ? 1.0f : s.pin[0] / s.pin[1];
		k = std::clamp(std::sqrt(ratioIn * s.pout[1] / s.pout[0]), 1.0f / kTrimMax, kTrimMax);
	}
	s.k = k;
	if (amount <= 0.0f) {
		return;
	}
	const float gl = amount == 1.0f ? std::sqrt(k) : std::pow(k, 0.5f * amount);
	l *= gl;
	r *= 1.0f / gl;
}
/// The make-up that follows it: `kDriveReferencePeak`'s sine out at the RMS it went in with.
[[nodiscard]] double driveMakeUp(DriveMode mode, double gain, float shape) noexcept;
/// THE SHAPER'S GAIN FOR A VANISHING SIGNAL, make-up included: what a feedback through it is divided by, so a tail
/// always dies. For the Paunch laws, which have no make-up: Paunch-H is 1 (the square has no slope at zero); Paunch-S is the
/// worst case of a channel's gain, `1 + a |w|` (a mono input, where the rectifier's slope has either sign: `1 +- a w`), with
/// `gain` the Amount's `a` and `w = 1 - 2 shape`.
[[nodiscard]] double driveSmallSignalGain(DriveMode mode, double gain, float shape, bool rotating = false) noexcept;
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
///     in seconds, sampled once a control block. Both engines publish it;
///   - EACH LOOP'S LIMITER, as it stood over the call: the lowest gain it held (1 when it let everything by) and the highest
///     level of its return against its threshold (`LoopLimiter`), sampled once a control block. Readings only.
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
	LimiterInnerGain, ///< the inner loop's limiter: the lowest gain it held over the call, 1 at rest
	LimiterOuterGain, ///< the outer loop's
	LimiterInnerLevel, ///< the inner loop's return, as the limiter's envelope against its threshold, the highest over the call
	LimiterOuterLevel,
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
