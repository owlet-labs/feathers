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
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

enum class Param : int {
	Enabled,
	Tone,      ///< phase into the tone bank: delay, feedback, clip, fold, highpass, triangle depth
	Character, ///< phase into the character bank: depth, resonance, S&H blend, width, triangle pitch
	Gamma,     ///< both phases at once, 1024 times coarser
	Cutoff,    ///< octaves on the SVF, the highpass and the triangle together
	Mix,       ///< how much wet is ADDED; the dry always passes whole

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
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::Tone), static_cast<uint8_t>(Param::Character),
	static_cast<uint8_t>(Param::Cutoff), static_cast<uint8_t>(Param::Mix),
	static_cast<uint8_t>(Param::Gamma),
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "ERODER",
	.name = "Eroder",
	.hasCard = true,
	.hasExpanded = false,
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
     .modSpan = kOneZone},
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
     .modSpan = kOneZone},
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
                "every step"},
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
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .suffix = "oct"},
    {.key = "eroder_mix", .name = "Eroder Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much eroded signal is ADDED. The dry always passes whole, so this is a send and "
                "not a crossfade; the level is compensated for feedback and drive"},
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
	float mix = 0.5f;

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

extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::DriveColour,
};

} // namespace feathers::fx::eroder
