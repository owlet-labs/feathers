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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

//--------------------------------------------------------------------------------------------------//
// FEATHERVERB — the reference's reverb, PORTED FIRST AND IMPROVED LATER, as the shaper was. The four //
// design decisions are listed below. The DSP is `featherverb_core.{h,cpp}`: the firmware's           //
// own text, bit-exact against it at 44.1 kHz (`tests/test_fx_featherverb.cpp` compiles the firmware  //
// file itself as the reference).                                                                     //
//                                                                                                    //
// THIS FILE IS THE DEVICE AROUND IT, and every choice in it is a decision or is marked as one:       //
// - Zone 2 is EIGHT MODES AS THE FIRMWARE HAS THEM (decision 1): a `modeCell` Choice (Mouse … Vast)  //
//   and the position inside that eighth, which is the reference's own `zone2 & 127`.                    //
// - Zones 1 and 3 are PLAIN BOUNDED KNOBS (decision 2): 0..1 onto the reference's 0..1023.              //
// - Other sample rates SCALE (decision 3): the core's `PORT:` edits.                                 //
// - An INSERT WITH A MIX KNOB (decision 4): the firmware's reverb is a send; here the send is the mono //
//   sum of the input, and `Mix` crossfades dry against the return.                                   //
// The remaining controls are the reference's own reverb menu, at its own ranges (0..1, which its menu   //
// shows as 0..50) and its own song defaults (`song.cpp`: room 30/50, damping 36/50, width 1, LPF 1). //
//--------------------------------------------------------------------------------------------------//

namespace feathers::fx::featherverb {

/// **THE LEVEL CONTRACT, the Eroder's lesson** (its wet was 15 dB low for missing it): inside the
/// hardware's mix, full scale is `EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128 = 2^24`. So a Feathers sample of
/// 1.0 is 2^24 on the way into the core, and 2^24 on the way out is 1.0 again. The send is the MONO
/// MID, `(L + R) / 2`. The return comes back at the reference's unpanned reverb level, `2^29`
/// (`audio_engine.cpp`, no sidechain: `(0x20000000 >> 15) * (0x20000000 >> 14)`).
inline constexpr double kFirmwareFullScale = 16777216.0;
inline constexpr int32_t kUnpannedLevel = 1 << 29;

/// **THE GAIN STAGING — THE PORT'S MAIN CHALLENGE (2026-09-24).** The hardware's input signal sits well
/// below clipping, because it runs with roughly 28 dB of headroom; in a DAW, signals more commonly run near
/// 0 dBFS. The 28 dB is approximately right, an estimate rather than a measurement. A DAW signal at 0 dBFS is placed where a hardware
/// signal sits, 28 dB under the reverb's own ceiling (the core's output saturation, 16x the reference's
/// full scale, +24 dB), and the return is made up so the wet comes back at the input's level:
/// - `kSendGain`, -14 dB: MEASURED — a 0 dBFS sine pluck's wet peaks 28.7..32.4 dB under that ceiling in
///   the five rooms, and 22..27 dB under it in Sky, Owl and Vast, which run hotter by design (Sky's wet
///   peaks at +5.9 dBFS). Every threshold inside the core (the ceiling, Sky/Vast's C3 knee, Owl's floors)
///   then meets the signal where the firmware's tuning expects it.
/// - `kReturnGain`, +4 dB: the rest of the -10 dB that MEASURED the rooms level with their input (the
///   core is linear below its ceiling, so where the trim is split changes no level, only headroom).
/// The history: at unity the return ran +9.6..+15.5 dB over the input and a 0 dBFS clamp on the wet
/// squared the plucks off; the clamp, which was plain clipping, is gone.
inline constexpr double kSendGain = 0.19952623149688797;  // 10^(-14/20)
inline constexpr double kReturnGain = 1.5848931924611136; // 10^(+4/20)

/// **CHARACTER — every mode's own hidden internals, on one rolling zone knob** (a zone knob
/// with phi triangles; every mode its own bank; Predelay only a delay; rolling).
/// One unbounded coordinate, the Eroder's shape: six bipolar phi triangles of it, which the core maps per
/// mode (`Core::refreshCharacter`). EVERY TRIANGLE IS ZERO AT 0, SO CHARACTER 0 IS THE FIRMWARE. It replaced
/// an Owl-only "drive" that MEASURED as moving nothing useful, even in Owl mode.
inline constexpr double kCharacterPhaseMax = 8388608.0; // 2^23, the Eroder's travel
inline constexpr uint32_t kCharacterZones = 8;
inline constexpr double kCharacterOneZone = 1.0 / static_cast<double>(kCharacterZones);

/// The reference's zone resolution: every zone control is an integer 0..1023, eight zones of 128.
inline constexpr int32_t kZoneMax = 1023;
inline constexpr int32_t kZoneWidth = 128;
inline constexpr int kModes = 9;

/// **Mix's time to settle.** The firmware has no Mix knob, since the send is its level. A step in it
/// here would click, so it glides.
inline constexpr double kMixSmoothingSeconds = 0.01;

/// **THE REFERENCE'S REVERB GATE** (`audio_engine.cpp`, `renderReverb`): the reverb runs while something has
/// sent to it in the last 12 s, OR while the level is above `log(mean |x|) > 9` in its units (e^9 = 8103
/// against a full scale of 2^24, about -66 dB). Past both, it stops, and its state FREEZES rather than
/// clearing. The port keeps it for a measured reason: Owl's servo and Vast's chain never reach exact
/// silence on their own (they sustain at a floor forever, 213 s measured), and on the hardware this gate
/// is what ends them. Two departures, both forced by an insert: "sent" means the input was not zero;
/// and the level is the reverb's own return, since the reference's is its whole mix, which is not here.
inline constexpr double kGateSeconds = 12.0;
inline constexpr double kGateLogLevel = 9.0;

enum class Param : int {
	Enabled,
	Mix,       ///< dry against the reverb's return — decision 4
	Room,      ///< `setRoomSize`: the FDN's feedback, 0.32 + 0.12 × this
	Damping,   ///< `setDamping`: the FDN's and the tail's one-pole darkening
	Width,     ///< `setWidth`: early side, tail side, and the global side boost
	Hpf,       ///< the input highpass, log2(Hz) (backlog LB; was `setHPF`'s 0..1)
	Lpf,       ///< the output lowpass, log2(Hz) (backlog LB; was `setLPF`'s 0..1)
	Matrix,    ///< ZONE 1 — the FDN matrix and the D0/D1 lengths, through phi triangles
	SizeMode,  ///< ZONE 2's eighth: the topology (Mouse … Trees are one topology at four sizes)
	Size,      ///< ZONE 2 inside that eighth: the continuous size
	Decay,     ///< ZONE 3 — per-line feedback, cascade coefficients, wobble, width breath
	Predelay,  ///< `setPredelay`: 0..50 ms, and in Owl the servo's attack
	Character, ///< a rolling phi coordinate onto each mode's own hidden internals (`Core::refreshCharacter`)
	Oversample, ///< backlog JE: the firmware core at twice the host rate (`featherverb_oversample.h`); not Aurora
	ConvMode,  ///< backlog JD: Off, Parallel, Series, Conv Only — a `modeCell`
	ConvBlend, ///< backlog JD: the convolution's share of the wet (Parallel and Series)
	ConvPredelay, ///< backlog JD: the convolver's own delay, 0..`kConvPredelayMaxSeconds` (squared taper)
	ConvFeedback, ///< backlog JD: the convolution fed back into itself, through its predelay
	InputGain, ///< backlog LA: dB on what enters the reverb and the convolution; the dry is untouched

	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE REFERENCE'S NAMES, from `gui/menu_item/reverb/featherverb.h`.
/// **OWL AFTER VAST** (with the feedback servo, Owl is the vaster of the two, so it comes after Vast
/// in the menu). The MENU order is the device's; the firmware's zone numbers are not moved
/// (`kModeZone`), so the core, the parity cases and the reference all still say zone 6 = Owl, 7 = Vast.
/// **AURORA BEFORE VAST** (placed by tail size: its tails run 1.2 to 12 s, Sky's country; Vast and
/// Owl are vaster). Aurora is NOT a firmware zone: it is
/// Featherverb's own engine (`featherverb_aurora.h`), run in the core's place.
inline constexpr const char* kSizeModeNames[] = { "Mouse", "Rabbit", "Lake", "Trees",
	                                               "Feather", "Sky", "Aurora", "Vast", "Owl" };
/// The firmware zone (`zone2 >> 7`) each menu entry selects.
/// Aurora has no zone of its own; while it runs, the idle core is kept on Sky's.
inline constexpr int kModeZone[] = { 0, 1, 2, 3, 4, 5, 5, 7, 6 };
/// Menu positions, for the device's own code and its tests.
inline constexpr int kAuroraMenu = 6;
inline constexpr int kVastMenu = 7;
inline constexpr int kOwlMenu = 8;
/// Every menu entry the firmware core runs (all but Aurora) — what the parity and Character cases walk.
inline constexpr int kFirmwareMenus[] = { 0, 1, 2, 3, 4, 5, 7, 8 };
static_assert(std::size(kSizeModeNames) == kModes);

/// **THE CONVOLUTION'S MODES** (backlog JD, first pass).
/// A mode is a ROUTING (decision IW: a dropdown is a mode selector). Exciter, which injects inside the
/// reverb, is the second pass.
inline constexpr const char* kConvModeNames[] = { "Off", "Parallel", "Series", "Conv Only", "Exciter" };
inline constexpr int kConvOff = 0;
inline constexpr int kConvParallel = 1;
inline constexpr int kConvSeries = 2;
inline constexpr int kConvOnly = 3;
/// **EXCITER** (excite mode injects somewhere inside the existing architectures): the convolution is not heard; it is INJECTED into the reverb's loop — past its highpass and
/// predelay, where every mode's feedback network and cascade take their input — so the algorithmic tail grows out
/// of the room. Blend is what excites the reverb: the dry send against the convolved one.
inline constexpr int kConvExciter = 4;
inline constexpr int kConvModes = 5;
/// The injection arrives `kExciteLagFrames` late: the convolution runs after the reverb in each chunk, so its
/// output reaches the reverb through a fixed ring a chunk long — a constant, small extra predelay on the room.
inline constexpr int kExciteLagFrames = 256;
/// The generated IR's length (`conv::generateTestIr`) until IRs can be loaded, and the time the three
/// routing gains take to glide, so a mode change does not click.
inline constexpr double kConvIrSeconds = 2.5;
inline constexpr double kConvGainSeconds = 0.02;
/// **THE LONGEST IR A FILE MAY BRING** (the convolver's capacity, set at prepare): longer files are cut
/// here with a fade (`conv::loadIrFile`). CPU scales with the IR actually loaded, not with this; memory
/// with this. Six seconds holds a large hall's tail; the scope's decision 2 can move it.
inline constexpr double kConvMaxIrSeconds = 6.0;
/// The dip around a new IR's swap into a running convolver: down, swap, up, this long each way.
inline constexpr double kConvSwapSeconds = 0.01;
/// **A TURN OF OVERSAMPLE** (backlog JE): the reverb fades out this long, the core is rebuilt at the new rate (a
/// different rate is a different layout of its one buffer, so the tail cannot carry over), and the new one starts.
inline constexpr double kOversampleFadeSeconds = 0.01;
/// The convolver's own predelay, at the top of its knob (a squared taper, so the short end gets the travel),
/// and how fast a turn of it glides (a moving read head: a turn is a brief pitch bend, never a click).
inline constexpr double kConvPredelayMaxSeconds = 0.5;
inline constexpr double kConvPredelayGlideSeconds = 0.05;
/// **FEEDBACK AT THE TOP OF ITS KNOB, AS A SHARE OF WHAT WOULD RING FOREVER.** The loop's gain is divided by
/// the IR's own peak gain (`conv::ConvKernel::peakGain`, its loudest frequency), so 100% would sustain that
/// frequency indefinitely whatever the IR: this is the margin below it. A soft saturation in the loop keeps a
/// hot input from running away before the loop has decayed.
inline constexpr double kConvFeedbackMax = 0.95;
/// **THE LOOP'S CEILING**: what the feedback puts back is `L · tanh(g · v / L)` — linear for small returns, and never
/// more than `L` (~-22 dBFS) however far the knob drives it, so a bloom settles near the input's own level.
/// MEASURED on the generated room at 100%: a plain tanh (ceiling 1) bloomed at rms 4.9, a ceiling of 0.25 at 1.2.
inline constexpr float kConvLoopCeiling = 0.08f;

/// **OWL'S TRIM ALONG DECAY** (build 1 and 2 and the safety limiter).
/// Owl's Decay turns its loop's
/// gain up and the level with it — MEASURED on -6 dBFS 800 Hz plucks, a wet peak of -3.7 dBFS at Decay 0 and +18 at 0.5,
/// where every other kind stays at or under 0 dBFS; the reference has ~28 dB of headroom to hide it in, an insert does not.
/// So Owl's return is TRIMMED by this much (dB) at Decay 0, 0.1 … 1, interpolated: a pure level, so its tone is untouched.
/// **A FIT, AND GUARDED AS ONE** (a fit must be redone if the plugin changes, so this says when):
/// `test_fx_featherverb.cpp`'s "Owl's trim holds" case re-measures Owl across Decay, Room, Character and pitch on every
/// run and FAILS when any point drifts more than 3 dB from level — printing the table to paste in its place.
inline constexpr float kOwlDecayTrimDb[11] = { 0.0f, -4.6f, -9.3f, -10.7f, -14.1f, -18.2f, -18.2f, -18.2f, -19.4f, -19.9f, -19.9f };
[[nodiscard]] inline float owlDecayTrimDb(float decay) noexcept {
	const float d = std::clamp(decay == decay ? decay : 0.0f, 0.0f, 1.0f) * 10.0f;
	const int i = std::min(9, static_cast<int>(d));
	const float f = d - static_cast<float>(i);
	return kOwlDecayTrimDb[i] + f * (kOwlDecayTrimDb[i + 1] - kOwlDecayTrimDb[i]);
}

/// THE WET'S SAFETY CEILING (fixed at 0 dBFS, after first being declined): a stereo-linked peak limiter
/// on the reverb's whole wet — every kind, the convolution too — so no wet sample leaves above this, with a release of
/// `kWetLimiterReleaseSeconds`. Below the ceiling it is a pure delay, so the wet stays linear there.
/// **ITS LOOKAHEAD** (backlog LD, option (a)): the first version attacked INSTANTLY, which squared off every
/// new peak at exactly the ceiling and stepped the gain by up to 2.8 dB in one sample — the tiny glitches heard on louder
/// signals. Now the wet is delayed by `kWetLookaheadSeconds` (72 samples at 48 kHz) and the gain ramps across it
/// (`dsp/lookahead_limiter.h`, linked, at these settings). The delay is the WET'S ONLY, against an undelayed dry: more predelay, not latency, so it is
/// not reported to the host, and it is the same at every setting.
inline constexpr float kWetCeiling = 1.0f; // 0 dBFS
inline constexpr double kWetLimiterReleaseSeconds = 0.1;
inline constexpr double kWetLookaheadSeconds = 0.0015;

/// **HPF AND LPF OVER THE WHOLE BAND** (backlog LB 2026-10-04: the LPF and HPF cover the full range).
/// Each is a corner in Hz, stored as log2(Hz) (`Unit::LogHertz`: equal travel per octave),
/// 20 Hz to 20 kHz, and the core computes its one-pole from Hz at the running rate (`Core::setHpfHz`). The firmware's
/// knobs ran 0..1 linearly over the coefficient: the HPF about 35..700 Hz (Owl 21..380 Hz), the LPF about 740 Hz..21 kHz.
inline constexpr double kFilterMinLog2 = 4.321928094887363;  // log2(20)
inline constexpr double kFilterMaxLog2 = 14.287712379549449; // log2(20000)
/// THE HPF DEFAULTS TO 150 Hz (backlog MR), above the
/// firmware's 0 (coefficient 0.005 at 44.1 kHz, a pole at `-ln(0.995) · 44100 / 2π` = 35.18 Hz), which LB carried over.
/// The LPF stays where the firmware's knob sat: the top of its travel, 20 kHz, against the firmware's 1 (coefficient
/// 0.95, a pole at 21.03 kHz).
inline constexpr double kHpfDefaultLog2 = 7.228818690495881; // log2(150)
inline constexpr double kLpfDefaultLog2 = kFilterMaxLog2;

/// **INPUT** (backlog LA: input level meter and input gain): a gain on the send, so on what the
/// reverb and the convolution hear, before both; the dry is not touched, so it sets how hard the tank is driven and not
/// how loud the device is. Its range is the dynamics devices' (KZ). It glides over `dyn::kRampSeconds`.
inline constexpr double kInputGainMinDb = -24.0;
inline constexpr double kInputGainMaxDb = 24.0;

/// **WHAT THE METER PUBLISHES, IN THIS ORDER** — the contract between `processStereo` and the card and window
/// (`ui::featherverb_meter`). Each held `dyn::kHoldSeconds`, as COMP's.
enum class Reading : size_t {
	/// LINEAR peak of what enters the reverb — the send's mono mid, after Input — in the device's own units (1 = 0 dBFS).
	InPeak,
	/// LINEAR peak of the wet after its 0 dBFS limiter, either channel, before Mix.
	WetPeak,
	/// THE WET LIMITER'S GAIN REDUCTION, as a POSITIVE number of dB. 0 is none.
	LimiterGr,
	kCount,
};

inline constexpr const char* kTags[] = { "reverb", "space", "fx" };

inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::SizeMode), static_cast<uint8_t>(Param::Size),
	static_cast<uint8_t>(Param::Room),     static_cast<uint8_t>(Param::Matrix),
	static_cast<uint8_t>(Param::Decay),    static_cast<uint8_t>(Param::Mix),
	// INPUT ALONE ON THE LAST ROW, and the meter beside it in the room the row leaves (`ui::FeatherverbCard`).
	static_cast<uint8_t>(Param::InputGain),
};

/// **KEYS THIS DEVICE NO LONGER HAS** (backlog JF): a patch holding one drops it with the reason said, and
/// the key can never name a control again (`device::RetiredKey`).
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "featherverb_owl_comp", "2026-09-25", "Owl's compressor is always on; the audition switch is gone" },
	{ "featherverb_hpf", "2026-10-10", "LB: a 0..1 knob on the firmware's coefficient; now featherverb_hpf_hz, log2(Hz) over 20 Hz..20 kHz" },
	{ "featherverb_lpf", "2026-10-10", "LB: a 0..1 knob on the firmware's coefficient; now featherverb_lpf_hz, log2(Hz) over 20 Hz..20 kHz" },
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "FVERB",
	.name = "Featherverb",
	.hasCard = true,
	// THE PANEL IS `GenericExpanded`, because only it seats a `modeCell` Choice as a zone knob with its
	// value box. The card shows the six that matter most.
	.hasExpanded = true,
	// A reverb is a bus effect; a per-voice copy would be 79 kB and a tail per voice.
	.perVoiceControl = -1,
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

/// ALL EIGHT MODES: `Size` is relevant in every one of them. Its gate exists only so the panel seats it
/// inside `Size Mode`'s cell.
inline constexpr uint32_t kEveryMode = (1u << kModes) - 1u;

inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "featherverb_enabled", .name = "Featherverb Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the reverb from the chain"},
    {.key = "featherverb_mix", .name = "Featherverb Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 0.3,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry against reverb. This reverb is an insert here, not a send",
     .shortName = "Mix"},
    {.key = "featherverb_room", .name = "Featherverb Room",
     .min = 0.0,
     .max = 1.0,
     .def = 0.6,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Feedback — how long the reverb rings",
     .shortName = "Room"},
    {.key = "featherverb_damping", .name = "Featherverb Damping",
     .min = 0.0,
     .max = 1.0,
     .def = 0.72,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How fast the highs die away inside the reverb",
     .shortName = "Damping"},
    {.key = "featherverb_width", .name = "Featherverb Width",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Stereo spread of the early reflections and the tail",
     .shortName = "Width"},
    {.key = "featherverb_hpf_hz", .name = "Featherverb HPF",
     .min = kFilterMinLog2,
     .max = kFilterMaxLog2,
     .def = kHpfDefaultLog2,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Highpass on what goes into the reverb",
     .shortName = "HPF"},
    {.key = "featherverb_lpf_hz", .name = "Featherverb LPF",
     .min = kFilterMinLog2,
     .max = kFilterMaxLog2,
     .def = kLpfDefaultLog2,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Lowpass on what comes out of the reverb",
     .shortName = "LPF"},
    {.key = "featherverb_matrix", .name = "Featherverb Matrix",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Zone 1: how the three early lines feed each other and how long two of "
                "them are — L/R, Front, Depth, Space, Diffuse, Stereo, Swirl, Complex",
     .shortName = "Matrix"},
    {.key = "featherverb_size_mode", .name = "Featherverb Size Mode",
     .min = 0.0,
     .max = static_cast<double>(kModes - 1),
     .def = 4.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kSizeModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Zone 2: Mouse to Trees are rooms from small to cathedral; Feather, Sky, "
                "Vast and Owl are each a different reverb. Aurora is Featherverb's own: a tail that slowly climbs "
                "as it rings (Matrix sets how fast)",
     .shortName = "Size",
     .modeCell = true},
    {.key = "featherverb_size", .name = "Featherverb Size",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Where inside the chosen size: longer lines, more tail, less early",
     .gateControl = static_cast<int16_t>(Param::SizeMode),
     .gateMask = kEveryMode,
     .shortName = "Size"},
    {.key = "featherverb_decay", .name = "Featherverb Decay",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Zone 3: the decay's shape — Balanced, Attack, Sustain, Bounce, Scoop, "
                "Hump, Sparse, Dense",
     .shortName = "Decay"},
    {.key = "featherverb_predelay", .name = "Featherverb Predelay",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Up to 50 ms before the reverb starts. In Owl it also sets how fast the tail swells",
     .shortName = "Predelay"},
    {.key = "featherverb_character", .name = "Featherverb Character",
     .min = -kCharacterPhaseMax,
     .max = kCharacterPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "One knob for each mode's hidden character: in the rooms, diffusion and density; in Feather, "
                "its stereo field; in Sky and Vast, where the tail builds and how it moves; in Owl, how far "
                "and how fast it blooms. It keeps turning, and the mark beside it shows which lap",
     .zones = kCharacterZones,
     .modSpan = kCharacterOneZone,
     .shortName = "Character"},
    {.key = "featherverb_oversample", .name = "Featherverb Oversample",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Runs the reverb at twice the rate: less aliasing grit up top at 44.1 and 48 kHz, for about two "
                "and a half times the reverb's CPU. Off is the original reverb exactly. A turn clears the tail. Aurora "
                "does not need it",
     .gateControl = static_cast<int16_t>(Param::SizeMode),
     .gateMask = kEveryMode & ~(1u << kAuroraMenu),
     .shortName = "Oversample",
     .ownCell = true},
    {.key = "featherverb_conv_mode", .name = "Featherverb Conv Mode",
     .min = 0.0,
     .max = static_cast<double>(kConvModes - 1),
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kConvModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "What the convolution does: Off (and costs nothing), Parallel beside the reverb, Series "
                "after it, Conv Only in place of it (and the reverb costs nothing), or Exciter inside it: the "
                "room feeds the reverb's own loop and the tail grows out of it",
     .shortName = "Conv",
     .modeCell = true},
    {.key = "featherverb_conv_blend", .name = "Featherverb Conv Blend",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the wet is the convolution: the reverb against the convolved input (Parallel), "
                "or against the convolved reverb (Series); in Exciter, how much of what drives the reverb is the "
                "room rather than the dry input",
     .gateControl = static_cast<int16_t>(Param::ConvMode),
     .gateMask = (1u << kConvParallel) | (1u << kConvSeries) | (1u << kConvExciter),
     .shortName = "Blend"},
    {.key = "featherverb_conv_predelay", .name = "Featherverb Conv Predelay",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Up to half a second before the convolution starts — its own, apart from the reverb's",
     .shortName = "Conv Pre"},
    {.key = "featherverb_conv_feedback", .name = "Featherverb Conv Feedback",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The convolution fed back into itself through its predelay: the room heard again and again, "
                "ringing longer toward the top",
     .shortName = "Conv Fdbk"},
    {.key = "featherverb_input_gain", .name = "Featherverb Input",
     .min = kInputGainMinDb,
     .max = kInputGainMaxDb,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Decibel,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Gain on what goes into the reverb, before it; the dry is untouched. The meter beside it shows what "
                "goes in, the wet, and how hard the wet's limiter is working",
     .shortName = "Input"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

struct Settings {
	bool enabled = true;
	float mix = 0.3f;
	float room = 0.6f;
	float damping = 0.72f;
	float width = 1.0f;
	float hpfLog2Hz = static_cast<float>(kHpfDefaultLog2); ///< log2(Hz), as the control stores it
	float lpfLog2Hz = static_cast<float>(kLpfDefaultLog2);
	float matrix = 0.0f;
	int sizeMode = 4;
	float size = 0.0f;
	float decay = 0.0f;
	float predelay = 0.0f;
	float character = 0.0f;
	bool oversample = false;
	int convMode = 0;
	float convBlend = 0.5f;
	float convPredelay = 0.0f;
	float convFeedback = 0.0f;
	float inputGainDb = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);

/// THE REFERENCE'S THREE ZONE INTEGERS, from the controls. Zones 1 and 3 are `round(x · 1023)`; Zone 2 is
/// `mode · 128 + round(size · 127)`, which is exactly the menu's integer for that mode and position.
[[nodiscard]] int32_t zone1Of(const Settings& s) noexcept;
[[nodiscard]] int32_t zone2Of(const Settings& s) noexcept;
[[nodiscard]] int32_t zone3Of(const Settings& s) noexcept;
/// CHARACTER'S SIX TRIANGLES at this coordinate — bipolar phi triangles, all zero at 0 (the firmware).
[[nodiscard]] std::array<float, 11> characterBankOf(float character) noexcept;

extern const device::AudioOps kAudioOps;
/// The body: which impulse response, as a filename (`conv::IrRef`).
extern const device::BodyOps kBodyOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.body = &kBodyOps,
	.category = device::Category::TimeSpace,
};

/// The enable's two declarations, pinned together (OWLTT's and HARM's reason).
static_assert(Settings {}.enabled == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

} // namespace feathers::fx::featherverb
