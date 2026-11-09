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
// THE DISPERSER — the reference's, ported.                                                              //
//--------------------------------------------------------------------------------------------------//
//
// lets port the dispersion plugin.
//
// THE REFERENCE is our firmware fork, branch `owlet-firmware-1.3` at `2567b04c`:
// `dsp/disperser.{h,cpp}` (the DSP, most of it inline in the header), its menu in
// `gui/menu_item/fx/disperser.h`, the biquad allpass in `dsp/filter/ladder_components.h`, and its
// callers (`mod_controllable_audio.cpp`, `sound.cpp`, `global_effectable_for_clip.cpp`).
//
// **WHAT IT IS: A CASCADE OF UP TO 32 SECOND-ORDER ALLPASSES, ROUTED EIGHT WAYS, WITH A DELAY LINE
// THAT FEEDS ITS OWN OUTPUT BACK IN.** An allpass passes every frequency at unity and delays each by a
// different amount, so a cascade smears a transient into a chirp — the kHz Disperser's sound. Topo
// picks the ROUTING (series, a ladder, two clusters, cross-coupled, parallel, nested, tapped) and,
// inside each zone, the Q and a set of phi-triangle quantities; Twist adds the stereo width, the dry
// echo (Punch), how the stages are spread (Curve), the regenerating echo (Chirp) and a Q tilt. It is
// 100% WET: the firmware has no mix and neither does this.
//
// **THE CALLERS WERE READ AS WELL AS THE PARAMS STRUCT** (automod's port missed a control by not doing
// so). `processDisperser` takes Topo and Twist as preset + cables, and the NOTE as `getLastNoteCode()`
// — which a Sound starts at INT_MIN and sets on every note-on and note-off, and a clip never sets. So
// "no note" is a state the firmware distinguishes from middle C, and this port keeps it (`heardNote_`).
// Everything else is fields: Freq, Stages and the three secret phases.

namespace feathers::fx::disperser {

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S NUMBERS, IN PHYSICAL UNITS.                                                         //
//--------------------------------------------------------------------------------------------------//

/// THE RATE EVERY FIRMWARE CONSTANT BELOW WAS WRITTEN AT.
inline constexpr double kReferenceRate = 44100.0;

/// **THE REFERENCE'S 0 dBFS IS 1/128 OF THE CEILING** (`EFFECTIVE_0DBFS_Q31`), and the Eroder's first
/// release was inaudible for missing it. Here every saturation — the allpass's `vqadd`s, the delay
/// line's `signed_saturate<30>`, the routing blends' `qadd` — sits 42 dB (the line 30 dB) above a
/// full-scale signal, so they are the ceiling of a runaway and not a colour. The signal path runs in
/// the firmware's units (in by 1/128, out by 128) so that they sit exactly where the firmware's do.
inline constexpr double kHeadroom = 128.0;

/// **THE FIRMWARE'S BUFFER, AS A TIME.** Everything the firmware does "per buffer" — the smoothing, the
/// transient detector, the activity counter, the delay's fold, and (every fourth) the coefficients and
/// the two LFOs — assumes 128 frames at 44.1 kHz, and the LFOs say so outright (`numSamples *
/// kCoeffUpdateStride / 44100`). Here it is a fixed grid of this length, carried ACROSS process calls,
/// so the output cannot depend on where the host cuts its blocks.
inline constexpr double kBlockSeconds = 128.0 / kReferenceRate;
/// `kCoeffUpdateStride = 4` — the coefficients and LFOs move every fourth block (11.6 ms).
inline constexpr int kCoeffUpdateStride = 4;
/// `kDisperserSmoothingAlpha = 0.03f`, per block — a time constant of 95 ms.
inline constexpr double kSmoothingAlpha = 0.03;

inline constexpr int kMaxStages = 32;
inline constexpr int kMinStages = 1;
/// **8, THE FIRMWARE'S OWN CEILING WITHOUT ITS `DisperserHiCPU` FEATURE.** The firmware's default is
/// 0 — off — because there every effect is always in the chain and zero is how it is off. Here adding
/// the device is the gesture; 8 is the most the reference offers a Sound by default.
inline constexpr int kDefaultStages = 8;

/// `DisperserDelayState::kMaxDelaySamples = 8820` — 200 ms.
inline constexpr double kLineSeconds = 8820.0 / kReferenceRate;
/// `calculateFoldedDelay`'s range: 441 and 8819 samples, and `kMaxOctaves = 4.32f` (not quite
/// log2(8819/441) = 4.3217 — the firmware's literal, kept).
inline constexpr double kFoldMinSeconds = 441.0 / kReferenceRate;
inline constexpr double kFoldMaxSeconds = 8819.0 / kReferenceRate;
inline constexpr double kFoldMaxOctaves = 4.32;
/// `kMinDelayQ16 = 441 << 16` — the 2f tap never shorter than 10 ms.
inline constexpr double kHarmonicMinSeconds = 441.0 / kReferenceRate;
/// `kDelayCrossfadeSamples = 256` — the write crossfade when the delay jumps by more than a tenth.
inline constexpr double kWriteCrossfadeSeconds = 256.0 / kReferenceRate;

/// **THE CHIRP LOOP'S CEILING — NOT THE FIRMWARE'S.** The firmware's doubled delay write puts its chirp
/// loop above unity near the top of the Chirp zone and it runs away (port doc §4.2). The round trip's
/// bound, `2 · (writeF + write2F) · feedback`, is held at this; see `Disperser::tick`.
inline constexpr float kChirpLoopCeiling = 0.97f;
/// `markActive()` — "~2 seconds of buffers at 128 samples/buffer": 700 blocks.
inline constexpr int kActivityBlocks = 700;
/// `hasEnergy()`'s threshold, `ONE_Q31 / 65536`, as a fraction of the ceiling: 54 dB under 0 dBFS. THE
/// FIRMWARE'S, recorded and NOT used — the port's gate is exact (`Disperser::hasEnergy`).
inline constexpr double kEnergyThreshold = 1.0 / 65536.0;

/// `centerHz = exp2f(freq * k127Recip * 13.0f)` — the Freq knob is thirteen octaves over 0..127, and
/// its value IS log2 of the frequency. 0 is 1 Hz, 127 is 8192 Hz.
inline constexpr double kFreqOctaves = 13.0;
inline constexpr double kFreqIndexMax = 127.0;
/// The firmware's knob default, 64, as log2(Hz): 93.8 Hz.
inline constexpr double kDefaultFreqLog2 = 64.0 * kFreqOctaves / kFreqIndexMax;
/// `(freq - 64) / 64 * 6.5` — with a note, the same knob is an offset from it, 64 being none.
inline constexpr double kOffsetCentreIndex = 64.0;
inline constexpr double kOffsetOctavesPerHalf = 6.5;
/// Where that offset runs: knob 0 is -6.5 octaves, knob 127 is +6.398.
inline constexpr double kOffsetMin = -kOffsetOctavesPerHalf;
inline constexpr double kOffsetMax = (kFreqIndexMax - kOffsetCentreIndex) / kOffsetCentreIndex * kOffsetOctavesPerHalf;
/// `spread * k127Recip * 4.0f` — the stages spread up to four octaves either side of the centre.
inline constexpr double kSpreadOctaves = 4.0;

/// `std::clamp(stageHz..., 1.0f, 16000.0f)`.
inline constexpr double kStageHzMin = 1.0;
inline constexpr double kStageHzMax = 16000.0;
/// `std::clamp(q, 0.5f, 20.0f)`, per stage after the tilt too.
inline constexpr double kQMin = 0.5;
inline constexpr double kQMax = 20.0;
/// **THE Q OFFSET** ("y axis drag on the window ... be q modification (added to the native q
/// setting)"): octaves of Q, either way, multiplied onto the Q the zones and the Q pill produce (`qWithOffset`).
/// Three octaves each way spans the whole engine range from any native Q in the middle of it.
inline constexpr double kQOffsetOctaves = 3.0;
/// **`BiquadAllpassCoeffs::compute` CLAMPS THE PREWARP ANGLE `w = pi fc / fs` TO [0.001, 1.4]** "for
/// the Padé approximation's validity" — which at 44.1 kHz is 14.04 Hz and 19.65 kHz. So the stages the
/// firmware's comments call "1 Hz subharmonics" run at 14 Hz. As hertz here; 1.4 rad also stays a
/// ceiling at any rate, since the tangent's pole is at pi/2.
inline constexpr double kAllpassLowHz = 0.001 * kReferenceRate / 3.14159265358979;
inline constexpr double kAllpassHighHz = 1.4 * kReferenceRate / 3.14159265358979;
inline constexpr double kAllpassMaxAngle = 1.4;

/// The two stereo LFOs (`updateCoefficients`): 0.06 Hz at ±0.06 octave, and φ^0.33 of that rate at
/// ±0.04, plus a 0.01-octave baseline, the whole half-spread clamped to ±0.3.
inline constexpr double kWidthLfoHz = 0.06;
inline constexpr double kWidthLfoDepth = 0.06;
inline constexpr double kDetuneLfoRatio = 1.1746627;
inline constexpr double kDetuneLfoDepth = 0.04;
inline constexpr double kBaselineStereoOffset = 0.01;
inline constexpr double kMaxLrOffset = 0.3;

/// `1024.0 * gammaPhase` — Gamma is both phases at once, a thousand times coarser.
inline constexpr double kGammaScale = 1024.0;

inline constexpr uint32_t kZones = 8;
/// One zone of an eight-zone coordinate, in the coordinate's own units — what a full-depth route
/// moves it by (the Eroder's reading of the firmware's `combinePresetAndCables`).
inline constexpr double kOneZone = 1.0 / static_cast<double>(kZones);
/// THE DRAG WINDOW. Not a bound — see `shaper_types.h` on Colour and Gamma.
inline constexpr double kPhaseMax = 8388608.0;

/// **WHICH OF THE EIGHT ROUTINGS**, by Topo's zone. `processWithTopology`'s switch: 1, 3, 4, 5 and 7 have
/// their own routing, and 0, 2 and 6 are all the plain cascade — Owlpass differs by how its stages are
/// DISTRIBUTED (two clusters) and Diffuse only by its phi patterns.
enum class Topology : int {
	Cascade = 0,
	Ladder = 1,
	Owlpass = 2,
	Cross = 3,
	Parallel = 4,
	Nested = 5,
	Diffuse = 6,
	Spring = 7,
};

/// THE ROUTINGS' q31 BLEND CONSTANTS, EXACTLY — `0x66666666` is not quite 0.8. Here, not in the engine's
/// file, because the card's plot runs the same routings and must blend by the same numbers.
constexpr float q31f(uint32_t v) { return static_cast<float>(static_cast<double>(v) / 2147483648.0); }
inline constexpr float kNested80 = q31f(0x66666666u);
inline constexpr float kNested20 = q31f(0x19999999u);
inline constexpr float kSpring70 = q31f(0x59999999u);
inline constexpr float kSpring15 = q31f(0x13333333u);
inline constexpr float kSpring875 = q31f(0x70000000u);
inline constexpr float kSpring125 = q31f(0x10000000u);

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// **TOPO AND TWIST ARE THE HOUSE'S MODE CELLS** — the zone controls had been nonconforming. Twist is infinite
/// and the others are fenced; increasing an offset never changes the zone dropdown's entry; and there is only
/// one Twist because it is infinite. Each is a mode (the zone, a
/// dropdown) and the value that mode reads, as `ModeValueCell` draws them (the Shaper's Colour, the voice
/// stack's Shape). Topo's eight modes are all FENCED: a position 0..1 inside the chosen routing, which no drag
/// and no route can carry into another. Twist's five single zones are fenced the same way; its sixth mode,
/// Twist, is the firmware's meta zone and the only INFINITE one — the phase the encoder drives by turning past
/// the fifth zone, unbounded upward. The engine still reads one coordinate for each (`Settings::topo`,
/// `Settings::twist`); the codec builds it from the mode and the value (`stateOf`).
///
/// **TOPO BECAME INFINITE (round 3).** Each routing keeps its dropdown entry, and its value is
/// an UNBOUNDED PHASE driving that routing's phi triangles — the firmware's own mechanism, the in-zone position
/// plus `topoPhaseOffset` that the reference exposed as the hidden push-encoder "dispMetaTopo". From 0 to 1 the
/// phase IS round 2's fenced position (the knob across the zone, bit for bit); past 1 the knob rests at the top
/// of the zone and the rest is the phase offset, below 0 it rests at the bottom (`topoPlaceOf`). So the phase
/// never changes the routing, and it evolves only what the offset reaches: param0/1/2, Q, detuning, 2f and
/// emphasis. Twist's five single zones stay fenced, by design.
enum class Param : int {
	Enabled,
	TopoMode,   ///< the routing: eight fenced modes
	TopoPhase,  ///< the routing's unbounded phase — Q and the per-zone phi quantities (`topoPlaceOf`)
	TwistMode,  ///< Width, Punch, Curve, Chirp, QTilt (fenced), or Twist (infinite)
	TwistPos,   ///< 0..1 inside a fenced Twist mode
	TwistPhase, ///< the Twist mode's unbounded phase
	Gamma,      ///< both phases, 1024 times coarser
	Stages,     ///< 1..32 allpasses
	KeyTrack,   ///< whether the centre follows the last note
	Freq,       ///< the centre, log2(Hz), when not tracking
	Offset,     ///< the centre's offset from the note, octaves, when tracking
	QOffset,    ///< octaves of Q added to the native Q, the plot handle's vertical drag (card and window) (`qWithOffset`)
	// **THE PILLS : "make them the enable/value pills like in shaper"** — each quantity the two
	// modes derive, as the Shaper's extras: an amount that is a FIELD SCALER (`overrideField`: 0 takes the
	// quantity to its neutral, 0.5 is the value the zones derived, 1 the most it can be) and a power beside it.
	// **ROUND 3 : "pills default to 'on' they can disable the extras by being switched off. they
	// default to the neutral values that leave twist as factory."** On, the amount scales the zones' value, and
	// the default (on, 0.5) IS the zones' own, which is the firmware; off DISABLES the extra (`kPillOff`).
	WidthAmount,
	PunchAmount,
	ChirpAmount,
	CurveAmount,
	QTiltAmount,
	QAmount,
	DetuneAmount,
	EmphasisAmount,
	HarmonicAmount,
	WidthOn,
	PunchOn,
	ChirpOn,
	CurveOn,
	QTiltOn,
	QOn,
	DetuneOn,
	EmphasisOn,
	HarmonicOn,

	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either. Mix is a crossfade (100% the wet), the wet Gain a fader position (unity at 0.5) on the wet
	/// only. Both defaults leave the device as it was, to the bit.
	Mix,
	WetGain,

	kCount,
};

/// THE PILLS, IN THE CARD'S ORDER — what the two modes derive, Twist's first. See `Param::WidthAmount`.
enum class Pill : int { Width, Punch, Chirp, Curve, QTilt, Q, Detune, Emphasis, Harmonic, kCount };
inline constexpr size_t kPillCount = static_cast<size_t>(Pill::kCount);
[[nodiscard]] constexpr Param pillAmountParam(Pill p) {
	return static_cast<Param>(static_cast<int>(Param::WidthAmount) + static_cast<int>(p));
}
[[nodiscard]] constexpr Param pillOnParam(Pill p) {
	return static_cast<Param>(static_cast<int>(Param::WidthOn) + static_cast<int>(p));
}
inline constexpr const char* kPillNames[kPillCount] = { "Width", "Punch", "Chirp", "Curve", "QTilt",
	                                                    "Q",     "Detune", "Emph",  "2f" };
/// **WHAT A PILL SWITCHED OFF PUTS IN ITS QUANTITY'S PLACE — the extra disabled (round 3, 2026-09-27).** The
/// Shaper's convention: off is the amount at zero (`ShaperSettings`' `gated`), whatever the amount says. So the
/// five amounts go to none (Width, Punch, Chirp, Detune, 2f: 0); the three that lean either way from a neutral go
/// to the neutral (Curve 0.5, the even spread; QTilt 0, no tilt; Emphasis 0, flat). Q is the one that is not an
/// amount at all — the Shaper has no such extra, and its zero (Q 0.5, the broadest the zones reach) is an
/// extreme, not a disabling — so off it is the firmware's own default Q, `TopoParams::q`'s 2.0.
inline constexpr double kPillOff[kPillCount] = { 0.0, 0.0, 0.0, 0.5, 0.0, 2.0, 0.0, 0.0, 0.0 };

/// TOPO'S MODES — the routings, all fenced. TWIST'S — five fenced single zones, then the infinite Twist.
inline constexpr const char* kTopoModeNames[] = { "Cascade", "Ladder", "Owlpass", "Cross",
	                                              "Parallel", "Nested", "Diffuse", "Spring" };
inline constexpr const char* kTwistModeNames[] = { "Width", "Punch", "Curve", "Chirp", "QTilt", "Twist" };
inline constexpr uint32_t kTwistInfinite = 5;
/// THE FOLLOWERS' GATES: a Topo position in every routing; Twist's position in the five fenced modes, its
/// phase in Twist.
inline constexpr uint32_t kAllTopoModes = 0xFFu;
inline constexpr uint32_t kTwistFencedModes = 0x1Fu;
inline constexpr uint32_t kTwistInfiniteMode = 1u << kTwistInfinite;
/// **HOW FAR ACROSS ITS ZONE A FENCED POSITION OF 1 SITS** — a hair short of the next zone, so the top of the
/// value is still the zone the dropdown names on the firmware's q31 grid (whose zones end a few parts in 10^9
/// before the eighths do) and after a float carries it.
inline constexpr double kFencedSpan = 1.0 - 1e-5;
/// Where the infinite Twist phase starts on the firmware's coordinate: the sixth zone, where turning on
/// past the single zones begins the combined evolution.
inline constexpr double kTwistInfiniteStart = 5.0 / 8.0;

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// **THE FIRMWARE'S NAMES, AND THEY ARE MEASURED FACTS THIS TIME, NOT LABELS.** Unlike the Eroder's,
/// Topo's zones BRANCH: five of the eight select a different signal routing and all eight select a
/// different set of phi patterns (`computeTopoParams`'s switch). The descriptions say what the code does
/// and nothing it does not — Diffuse's firmware comment promises "randomized per-stage coefficient
/// variation", and there is no such code; its routing is the cascade's.
inline constexpr device::ZoneName kTopoZoneNames[kZones] = {
	{ "Cascade", "Every stage in series — the classic disperser" },
	{ "Ladder", "In series, each stage blending in 5% to 25% of the other channel first" },
	{ "Owlpass", "Stages split into two clusters that move up to five octaves apart" },
	{ "Cross", "In series, the two channels cross-blended after every fourth stage" },
	{ "Parallel", "Two half-cascades side by side, summed" },
	{ "Nested", "The second half of the stages fed half dry, half from the first; channels blended 20%" },
	{ "Diffuse", "In series like Cascade, with its own spread and stereo pattern" },
	{ "Spring", "In series, with taps at one and two thirds blended back in" },
};

/// **TRUE ONLY WITH GAMMA AT ZERO** — `computeDisperserTwistParams` gives Width…QTilt one quantity each only
/// while its phase offset is zero; with Gamma every mode is the combined phi evolution Twist is.
inline constexpr device::ZoneName kTwistZoneNames[] = {
	{ "Width", "Moves the two channels' stages apart in frequency" },
	{ "Punch", "An echo of the dry signal, fed back through the stages once" },
	{ "Curve", "How the stages are spread: bunched low, even, or bunched high" },
	{ "Chirp", "The output fed back through the stages as repeating echoes" },
	{ "QTilt", "Sharper high stages, then sharper low ones" },
	{ "Twist", "All of the above at once, each moving at its own rate — and it keeps turning" },
};

inline constexpr const char* kTags[] = { "allpass", "phase", "fx" };

/// THE FACE (for a generic surface): the two mode cells first because they are the instrument, then the
/// centre, the count, Key Track beside the knob it reinterprets, and Gamma last.
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::TopoMode), static_cast<uint8_t>(Param::TwistMode),
	static_cast<uint8_t>(Param::Freq),     static_cast<uint8_t>(Param::Offset),
	static_cast<uint8_t>(Param::Stages),   static_cast<uint8_t>(Param::KeyTrack),
	static_cast<uint8_t>(Param::Gamma),
};

/// THE TWO UNBOUNDED COORDINATES, RETIRED — see `Param`.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "disperser_topo", "2026-09-26",
	  "one unbounded coordinate became the house mode cell: a fenced routing (disperser_topo_mode) and a position inside it (disperser_topo_pos)" },
	{ "disperser_twist", "2026-09-26",
	  "one unbounded coordinate became the house mode cell: five fenced zones and the infinite Twist (disperser_twist_mode, _pos, _phase)" },
	{ "disperser_topo_pos", "2026-09-27",
	  "Topo became infinite: a fenced 0..1 position became an unbounded phase (disperser_topo_phase); 0..1 is the same place" },
	// THE PILLS' POWERS, RETIRED (round 3) — off meant "the zones' own value" and was the
	// default; off now disables the extra (`kPillOff`) and on at 0.5 is the zones' own. See `Param::WidthAmount`.
	{ "disperser_width_on", "2026-09-27", "off meant the zones' own Width; off now disables it (disperser_width_power)" },
	{ "disperser_punch_on", "2026-09-27", "off meant the zones' own Punch; off now disables it (disperser_punch_power)" },
	{ "disperser_chirp_on", "2026-09-27", "off meant the zones' own Chirp; off now disables it (disperser_chirp_power)" },
	{ "disperser_curve_on", "2026-09-27", "off meant the zones' own Curve; off now the even spread (disperser_curve_power)" },
	{ "disperser_qtilt_on", "2026-09-27", "off meant the zones' own QTilt; off now disables it (disperser_qtilt_power)" },
	{ "disperser_q_on", "2026-09-27", "off meant the zones' own Q; off now the firmware's default Q (disperser_q_power)" },
	{ "disperser_detune_on", "2026-09-27", "off meant the zones' own Detune; off now disables it (disperser_detune_power)" },
	{ "disperser_emphasis_on", "2026-09-27", "off meant the zones' own Emphasis; off now flat (disperser_emphasis_power)" },
	{ "disperser_harmonic_on", "2026-09-27", "off meant the zones' own 2f; off now disables it (disperser_harmonic_power)" },
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "DISPERSE",
	.name = "Disperser",
	.hasCard = true,
	.hasExpanded = true,
	// ON THE BUS ONLY, BY DESIGN: never offered per voice. It follows the last note
	// through `setNoteContext`, the firmware's own arrangement.
	.perVoiceControl = -1,
	.extraState = 0,
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = -1,
	.faceControls = 0,
	.faceOrder = kFaceOrder,
	.faceColumns = 3,
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

/// Key Track's two values, as a mask for the controls it gates.
inline constexpr uint32_t kWhenNotTracking = 1u << 0;
inline constexpr uint32_t kWhenTracking = 1u << 1;

/// EVERY ROW BY FIELD NAME — device.h records the positional-init hazard twice over.
inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "disperser_enabled", .name = "Disperser Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the disperser from the chain"},
    // TOPO: THE MODE CELL — the routing, fenced (see `Param`). Not a destination: the routing moves only with a
    // hand, as the firmware's `ZoneBasedParam<8, true>` clip had it; its position is.
    {.key = "disperser_topo_mode", .name = "Disperser Topo",
     .min = 0.0,
     .max = static_cast<double>(std::size(kTopoModeNames) - 1),
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kTopoModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "How the stages are wired: in series, laddered, in two clusters, crossed, in parallel, nested, "
                "diffused or sprung",
     .shortName = "Topo",
     .modeCell = true},
    {.key = "disperser_topo_phase", .name = "Disperser Topo Phase",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The routing's phase: from 0 to 1 the stages sharpen from broad to resonant across it, and past "
                "either end its patterns keep turning — the routing never changes",
     // AN UNBOUNDED COUNTER, as Gamma and automod's fines (`device::isDragWindow`): no zones inside a routing,
     // and a full-depth route moves it ONE LAP — 0 to 1 is the whole of the routing's old fenced travel.
     .zones = 0,
     .modSpan = 1.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::TopoMode),
     .gateMask = kAllTopoModes},
    // TWIST: five fenced single zones and the infinite Twist.
    {.key = "disperser_twist_mode", .name = "Disperser Twist",
     .min = 0.0,
     .max = static_cast<double>(std::size(kTwistModeNames) - 1),
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kTwistModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Width, an echo, the stage spread, repeating echoes, a Q tilt — or Twist, all of them at once, "
                "turning without end",
     .shortName = "Twist",
     .modeCell = true},
    {.key = "disperser_twist_pos", .name = "Disperser Twist Position",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the chosen quantity, across its zone — never past the zone's edge",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::TwistMode),
     .gateMask = kTwistFencedModes},
    {.key = "disperser_twist_phase", .name = "Disperser Twist Phase",
     .min = 0.0,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Twist's phase: every quantity moving at its own rate, and it keeps turning — each lap a "
                "different blend",
     .zones = kZones,
     .modSpan = kOneZone,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::TwistMode),
     .gateMask = kTwistInfiniteMode},
    {.key = "disperser_gamma", .name = "Disperser Gamma",
     .min = -kPhaseMax,
     .max = kPhaseMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Shifts Topo's and Twist's patterns together, a thousand times coarser — a different "
                "disperser at every step"},
    {.key = "disperser_stages", .name = "Disperser Stages",
     .min = static_cast<double>(kMinStages),
     .max = static_cast<double>(kMaxStages),
     .def = static_cast<double>(kDefaultStages),
     .stepped = true,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "How many allpass stages: more is a longer, deeper chirp"},
    {.key = "disperser_key_track", .name = "Disperser Key Track",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the stages centre on the last note played, moved by Offset. Off: Freq sets the centre"},
    {.key = "disperser_freq", .name = "Disperser Freq",
     .min = 0.0,
     .max = kFreqOctaves,
     .def = kDefaultFreqLog2,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true, // the firmware's Freq is a field no cable reaches; here the centre is smoothed, so a route is safe
     .offsetsMayLeaveRange = false,
     .tooltip = "Where the stages centre, and so where the group-delay curve peaks; the echoes' delay follows "
                "it. Drag it along the plot",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::KeyTrack),
     .gateMask = kWhenNotTracking},
    {.key = "disperser_offset", .name = "Disperser Offset",
     .min = kOffsetMin,
     .max = kOffsetMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Octaves from the last note. Before any note has played, the same knob sets the centre "
                "directly, 1 Hz at the bottom to 8 kHz at the top",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::KeyTrack),
     .gateMask = kWhenTracking,
     .centred = true},
    {.key = "disperser_q_offset", .name = "Disperser Q Offset",
     .min = -kQOffsetOctaves,
     .max = kQOffsetOctaves,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Octaves of Q on top of what Topo and the Q pill give: up sharpens every stage toward a ring, "
                "down broadens it. Drag the plot's handle up and down",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .centred = true},
    // THE PILLS — see `Param::WidthAmount`. Amounts first, then the powers, the Shaper's order.
    {.key = "disperser_width", .name = "Disperser Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The two channels' stages moved apart: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_punch", .name = "Disperser Punch",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The dry echo fed back through the stages: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_chirp", .name = "Disperser Chirp",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The repeating echoes: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_curve", .name = "Disperser Curve",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the stage spread leans from even: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_qtilt", .name = "Disperser QTilt",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the stages' sharpness tilts across the range: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_q", .name = "Disperser Q",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The stages' sharpness, broad to resonant: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_detune", .name = "Disperser Detune",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The stages' detuning from their grid: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_emphasis", .name = "Disperser Emphasis",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the stages' levels tilt across the range: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_harmonic", .name = "Disperser 2f",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The echo at twice the delay's rate: at 50% the value the zones give; lower takes it toward none, higher toward the most. Heard while its power is on"},
    {.key = "disperser_width_power", .name = "Disperser Width On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Width, and 50% is exactly theirs. Off: no Width"},
    {.key = "disperser_punch_power", .name = "Disperser Punch On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Punch, and 50% is exactly theirs. Off: no Punch"},
    {.key = "disperser_chirp_power", .name = "Disperser Chirp On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Chirp, and 50% is exactly theirs. Off: no Chirp"},
    {.key = "disperser_curve_power", .name = "Disperser Curve On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Curve, and 50% is exactly theirs. Off: the even spread"},
    {.key = "disperser_qtilt_power", .name = "Disperser QTilt On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own QTilt, and 50% is exactly theirs. Off: no QTilt"},
    {.key = "disperser_q_power", .name = "Disperser Q On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Q, and 50% is exactly theirs. Off: the firmware's default Q (2)"},
    {.key = "disperser_detune_power", .name = "Disperser Detune On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Detune, and 50% is exactly theirs. Off: no Detune"},
    {.key = "disperser_emphasis_power", .name = "Disperser Emphasis On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own Emphasis, and 50% is exactly theirs. Off: flat"},
    {.key = "disperser_harmonic_power", .name = "Disperser 2f On",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "On: the pill's amount scales the zones' own 2f, and 50% is exactly theirs. Off: no 2f"},
    {.key = "disperser_mix", .name = "Disperser Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry against dispersed, around the whole device"},
    {.key = "disperser_wet_gain", .name = "Disperser Wet Gain",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Fader,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Level of the dispersed signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

struct Settings {
	bool enabled = true;
	/// **THE COORDINATES THE ENGINE READS** — lap plus the firmware's knob. The codec builds them from the modes
	/// and their values (`topoCoordinateOf`, `twistCoordinateOf`); a case may set them directly to reach a
	/// coordinate no mode cell names (a Topo lap, the transcription's grid).
	float topo = 0.0f;
	float twist = 0.0f;
	/// TOPO'S PHASE PAST ITS ZONE — added to the lap `topo` carries, never to its knob, so it cannot reach the
	/// routing (`topoPlaceOf`). Zero for every Topo phase from 0 to 1.
	float topoLap = 0.0f;
	/// THE MODE CELLS AS STORED — see `Param::TopoMode`.
	uint32_t topoMode = 0;
	float topoPhase = 0.0f;
	uint32_t twistMode = 0;
	float twistPos = 0.0f;
	float twistPhase = 0.0f;
	/// THE PILLS — see `Param::WidthAmount`. On at 0.5, the default: the zones' own values, the firmware.
	std::array<float, kPillCount> pillAmount { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
	std::array<bool, kPillCount> pillOn { true, true, true, true, true, true, true, true, true };
	float gamma = 0.0f;
	int stages = kDefaultStages;
	bool keyTrack = true;
	float freqLog2 = static_cast<float>(kDefaultFreqLog2);
	float offsetOctaves = 0.0f;
	/// THE Q OFFSET, octaves — see `kQOffsetOctaves`. Zero leaves the native Q untouched, bit for bit.
	float qOffsetOctaves = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE MODE CELLS ONTO THE ENGINE'S COORDINATES. A fenced position runs to `kFencedSpan` of its zone and no
/// further; the infinite Twist phase starts at the sixth zone and turns on through the laps.
[[nodiscard]] double topoCoordinateOf(uint32_t mode, double pos) noexcept;
[[nodiscard]] double twistCoordinateOf(uint32_t mode, double pos, double phase) noexcept;
/// **TOPO'S INFINITE PHASE ONTO THE ENGINE** (round 3, see `Param`): the coordinate (the routing, and the knob
/// inside it) and the lap added to the firmware's `topoPhaseOffset`. The knob is round 2's fenced position of the
/// phase clamped to 0..1; the lap is what the clamp took off. Continuous everywhere, and the routing is `mode`
/// for every phase.
struct TopoPlace {
	double coordinate = 0.0;
	double lap = 0.0;
};
[[nodiscard]] TopoPlace topoPlaceOf(uint32_t mode, double phase) noexcept;

/// **A PILL'S LAW** — the Shaper's `fieldWithOverride` (`dsp/slice_build.h`) on a quantity normalised to 0..1:
/// 0 takes it to none, 0.5 leaves it, 1 lifts it to the most. See `applyPills`.
[[nodiscard]] double overrideField(double field, double amount) noexcept;

Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);
float valueOf(const State& state, Param param);
void setValueOf(State* state, Param param, float value);

//--------------------------------------------------------------------------------------------------//
// THE MAPPING: A COORDINATE TO A ZONE, A POSITION AND A LAP.                                         //
//--------------------------------------------------------------------------------------------------//

/// **ONE UNBOUNDED COORDINATE IS THE FIRMWARE'S KNOB AND ITS PHASE OFFSET.** The reference's menu
/// auto-wraps (`supportsAutoWrap`): turning past the end puts the knob back at 0 and adds ONE to the
/// phase offset, and past the start the reverse. So `c = lap + knob`, the knob is `c - floor(c)` and
/// the lap is the offset. Push-and-twist on the hardware steps the offset by 128 — c += 128 here.
struct Coordinate {
	double knob = 0.0; ///< 0..1, the firmware's q31 knob over ONE_Q31
	double lap = 0.0;  ///< the firmware's `topoPhaseOffset` / `twistPhaseOffset`
};
[[nodiscard]] Coordinate splitCoordinate(double c) noexcept;

/// `computeZoneQ31`: which of eight zones the knob is in and where inside it, 0..1.
struct Zone {
	int index = 0;
	double position = 0.0;
};
[[nodiscard]] Zone zoneOf(double knob) noexcept;

/// `DisperserTopoParams`, field for field.
struct TopoParams {
	int zone = 0;
	double q = 2.0;
	double param0 = 0.0;
	double param1 = 0.0;
	double param2 = 0.0;
	double lrOffset = 0.0;
	double detuning = 0.0;
	double harmonicBlend = 0.0;
	double emphasis = 0.0;
};

/// `DisperserTwistParams`, field for field.
struct TwistParams {
	double width = 0.0;
	double punch = 0.0;
	double chirpAmount = 0.0;
	double spreadCurve = 0.5;
	double qTilt = 0.0;
	double phaseOffset = 0.0;
	double lfoRateScale = 1.0;
};

/// `computeDisperserTopoParams`. `knob` is 0..1; `phRaw` the lap plus 1024 Gamma; `twistPhaseOffset`
/// what Twist hands over to rotate the detuning/harmonic/emphasis patterns.
[[nodiscard]] TopoParams computeTopoParams(double knob, double phRaw, double twistPhaseOffset) noexcept;
/// `computeDisperserTwistParams`. Past the first lap, or with any Gamma, `phRaw` is not zero and the
/// firmware leaves its zones for the combined evolution over the whole knob — kept.
[[nodiscard]] TwistParams computeTwistParams(double knob, double phRaw) noexcept;
/// THE PILLS, APPLIED to what the two modes derived — the one call the engine and the plot both make. ON:
/// Width, Punch, Chirp, Detune and 2f are amounts on 0..1; Curve, QTilt and Emphasis lean either way from a
/// neutral, so the pill scales the lean and keeps its side; Q is read on its own log scale, 0.5 to 20; and an
/// amount of exactly 0.5 leaves the zones' value untouched, bit for bit. OFF: the quantity is `kPillOff`'s.
void applyPills(const Settings& s, TopoParams& topo, TwistParams& twist) noexcept;
/// **THE Q OFFSET ON THE NATIVE Q** — the one law the engine (with its glided offset) and the plot (with the
/// stored one) both apply, after the pills: `q · 2^octaves`, held to the engine's Q range. Zero returns `q`
/// itself, so a patch that never touches it is the firmware bit for bit.
[[nodiscard]] double qWithOffset(double q, double octaves) noexcept;

/// **WHAT `processDisperser` MAKES OF THE TWO**, before the coefficients: the spread (0..1 of four
/// octaves), the L/R offset in octaves, Owlpass's mode separation, Cross's mix.
struct Routing {
	double spread = 0.0;
	double lrOffset = 0.0;
	double bimodalSeparation = 0.0;
	double crossMix = 0.0;
};
[[nodiscard]] Routing routingOf(const TopoParams& topo, const TwistParams& twist) noexcept;

/// **THE CENTRE, AS THE FIRMWARE'S 0..127 INDEX** (`log2(Hz) = index * 13 / 127`). Key Track off: the
/// Freq knob. On and a note heard: the note times 2^Offset. On and no note yet: the Offset knob read as
/// the absolute knob it is on the hardware. Continuous — the firmware's is a `uint8_t`.
[[nodiscard]] double freqIndexOf(const Settings& s, bool haveNote, double noteSemitones) noexcept;

/// `calculateFoldedDelay`, in samples at `sampleRate`: fold in octave space into [10, 200] ms, then
/// move the ORIGINAL by the nearest whole number of fifths to land there.
[[nodiscard]] double foldedDelaySamples(double samples, double sampleRate) noexcept;

/// `BiquadAllpassCoeffs::compute`, in the firmware's storage: `a1Half` is a1 / 2, clamped to
/// [-1, 0.9999]; `a2` clamped the same. The tangent is EXACT here (the firmware's is a Padé fit).
///
/// **DOUBLE, AND SO ARE THE STATES THEY RUN ON — MEASURED, NOT PREFERRED.** A stage near its 14 Hz floor
/// has its poles 0.001 from the unit circle, and its two DF2T states then run hundreds of times the
/// signal. q31 holds those to a fixed 2^-31; a float holds them to 2^-24 OF THEIR OWN SIZE, which is
/// coarser, and the pole amplifies the difference: the first draft (float) sat 1.3e-4 of full scale off
/// the firmware's loop inside ONE stage and one block, and 1.5e-3 after half a second through eight.
struct AllpassCoeffs {
	double a1Half = 0.0;
	double a2 = 0.0;
	/// **THE SAME ALLPASS AS A NORMALIZED LATTICE** (Gray–Markel): reflection coefficients `k2 = a2`,
	/// `k1 = a1 / (1 + a2)`, and `c = sqrt(1 - k^2)` for each. The engine runs this form — see `runStage`.
	double k1 = 0.0;
	double c1 = 1.0;
	double k2 = 0.0;
	double c2 = 1.0;
};
[[nodiscard]] AllpassCoeffs allpassCoeffs(double hz, double q, double sampleRate) noexcept;

/// **WHAT THE DEVICE PUBLISHES, once per process call, for the card and the panel** — facts only the running
/// device has: whether a note has been heard and which (the plot's marker), and the delay road's per-block
/// gains after the cap (the panel's feedback strip). Unchanged at rest, so a surface that compares them
/// repaints zero times while nothing moves.
enum class Reading : size_t {
	NoteHeard,     ///< 1 once a gated note has arrived, else 0
	NoteSemitones, ///< the pitch the centre tracks, A4 = 69
	PunchGain,     ///< the dry write's gain this block (doubled on the write, as the firmware's)
	ChirpGain,     ///< the output write's gain at f this block, after the cap
	Chirp2FGain,   ///< the output write's gain at 2f this block, after the cap
	LoopGain,      ///< the chirp loop's bound BEFORE the cap; the cap holds it at `kChirpLoopCeiling`
	EchoMs,        ///< where the writes land, ms
	/// **THE STAGES' EMPHASIS GAINS AS THE ENGINE HOLDS THEM, 32 of them.** Not a function of the settings:
	/// they glide a sixteenth toward their targets only when the coefficients recompute, and a static setting
	/// without detuning stops recomputing after one update — so they rest 1/16 of the way from unity
	/// (the firmware's behaviour, kept; port doc §4). In a routing that SUMS paths they shape the phase, so
	/// the plot draws the engine's own rather than the targets.
	StageGain0,
	kCount = StageGain0 + static_cast<size_t>(kMaxStages),
};

/// **WHERE THE STAGES SIT — `updateCoefficients`' loop as a pure function.** One function, called by the
/// engine at its coefficient stride and by the card's plot on the message thread, so the picture cannot
/// drift from the sound. In the firmware's float, in its order: the engine holds the transcription to
/// 1e-5 of full scale through it. `halfSpread` is the L/R half-offset in octaves AFTER the two LFOs; the
/// emphasis gain is the TARGET (the engine glides toward it a sixteenth per update).
struct StageLayout {
	int count = 0;
	std::array<float, kMaxStages> hzBase {}; ///< undetuned, before the L/R offset — what the echo's delay reads
	std::array<float, kMaxStages> hzL {};
	std::array<float, kMaxStages> hzR {};
	std::array<float, kMaxStages> q {};
	std::array<float, kMaxStages> gainTarget {};
};
[[nodiscard]] StageLayout layoutStages(float centerHz, float spreadOctaves, float halfSpread, float q, int stages,
                                       float spreadCurve, float qTilt, float bimodalSeparation, float detuning,
                                       float emphasis) noexcept;

/// `centerHz = exp2f(freq * 13 / 127)`, from the (continuous) index.
[[nodiscard]] float centerHzOf(double freqIndex) noexcept;
/// The L/R half-offset with both LFOs at their centre — what the plot draws. The engine adds the LFOs.
[[nodiscard]] float restingHalfSpread(double lrOffset) noexcept;

extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	// AN ALLPASS CASCADE IS A PHASER WITHOUT ITS LFO, and a phaser is filed under Modulation FX.
	.category = device::Category::ModulationFx,
};

} // namespace feathers::fx::disperser
