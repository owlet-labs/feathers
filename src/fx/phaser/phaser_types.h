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
#include "dsp/fast_math_approx.h"
#include "dsp/sinc_delay_line.h"
#include "fx/fx_tenant.h"
#include "fx/phaser/phaser_phase_space.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>

//--------------------------------------------------------------------------------------------------//
// THE PHASER/FLANGER.                                                                                //
//--------------------------------------------------------------------------------------------------//
//
// ONE DEVICE, TWO ENGINES: Mode picks an allpass cascade (Phaser) or a swept delay line (Flanger), and
// a feedback loop with Damping and Drive wraps either. The sweep point is Position plus an internal
// LFO and an envelope follower, each switchable off so an external modulator on Position can drive
// it alone. The cool side — through-zero, barberpole, key tracking, the explore pad — sits behind the
// same face.
//
// **THE PARAM TABLE IS THE WHOLE DEVICE, AND THE ENGINE READS EVERY ROW OF IT** (`phaser.h`). Where a control
// changes nothing — its Mode's other engine, a source that is off, Center/Delay under a tracked note — the gate
// columns below and the card say so; the honesty rule is answered there, not by leaving rows out of this table.

namespace feathers::fx::phaser {

/// THE DEVICE'S NAME — a WORKING TITLE (the design doc: "the device name is still the project's to choose"),
/// so it is stated once and everything that prints it reads it from here.
inline constexpr const char* kDeviceName = "Phaser/Flanger";

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// Declaration order is the ROW order; the card's order is `kFaceOrder`. A patch stores by key, so
/// either may move freely.
enum class Param : int {
	Enabled,
	Mode,        ///< Phaser | Flanger — the mode cell
	Stages,      ///< 2..12 allpasses (Phaser)
	CenterHz,    ///< the cascade's centre, log2(Hz) (Phaser)
	DelayMs,     ///< the line's centre delay, log2(s) (Flanger)
	Feedback,    ///< -1..1; the sign is the polarity
	Damping,     ///< the loop's one-pole lowpass, log2(Hz)
	Drive,       ///< the loop's saturator
	Mix,         ///< dry/wet; 0.5 is the classic phaser
	Position,    ///< -1..1, the manual sweep point — THE external-modulation target
	LfoOn,       ///< the LFO's power
	LfoDepth,    ///< the LFO's amount
	Sync,        ///< Free, or a tempo division
	RateHz,      ///< the LFO's free rate, log2(Hz) (Sync = Free)
	Stereo,      ///< L/R LFO phase offset, degrees
	EnvOn,       ///< the envelope follower's power
	EnvAmount,   ///< -1..1, the follower's amount
	EnvAttack,   ///< log2(s)
	EnvRelease,  ///< log2(s)
	EnvSens,     ///< dB of gain ahead of the follower
	Sweep,       ///< Normal | Up | Down (Up and Down are barberpole)
	ThroughZero, ///< Flanger only; latency-bearing
	KeyTrack,    ///< the last note sets the centre; Center/Delay are then the offset from it
	Rotate,      ///< turns the explore plane
	// **THE EXPLORE GROUPS' FIELD-SCALER PILLS** (round 2, Task 11): a knob (`fieldWithOverride`) and its power per
	// group, in `phase_space::Group`'s order, so `pillKnobParam(g)` and `pillPowerParam(g)` are arithmetic. Window only.
	XSpread,
	XSpreadOn,
	XStagger,
	XStaggerOn,
	XShape,
	XShapeOn,
	XColour,
	XColourOn,
	XBias,
	XBiasOn,
	XSkew,
	XSkewOn,
	XRateSkew,
	XRateSkewOn,
	XFbTilt,
	XFbTiltOn,
	XPairing,
	XPairingOn,
	XCurve,
	XCurveOn,

	kCount,
};
inline constexpr size_t kParamCount = static_cast<size_t>(Param::kCount);

/// GROUP `g`'s PILL: its knob, and its power.
[[nodiscard]] constexpr Param pillKnobParam(phase_space::Group g) noexcept {
	return static_cast<Param>(static_cast<int>(Param::XSpread) + 2 * static_cast<int>(g));
}
[[nodiscard]] constexpr Param pillPowerParam(phase_space::Group g) noexcept {
	return static_cast<Param>(static_cast<int>(pillKnobParam(g)) + 1);
}
static_assert(pillPowerParam(phase_space::Group::Curve) == Param::XCurveOn
                  && static_cast<size_t>(Param::XCurveOn) + 1 == kParamCount,
              "one knob and one power per group, in the groups' order, closing the table");

enum class Mode : uint32_t { Phaser = 0, Flanger = 1 };
enum class SweepShape : uint32_t { Normal = 0, Up = 1, Down = 2 };

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kModeNames[] = { "Phaser", "Flanger" };
inline constexpr const char* kSweepNames[] = { "Normal", "Up", "Down" };

/// THE STAGE COUNTS, by name — a patch stores a Choice by its LABEL (backlog JF), so the label is the
/// count and the index is only where it sits in this list. Even counts only: a first-order allpass
/// pair makes one notch, so an odd count would add a stage of phase and no notch.
inline constexpr const char* kStageNames[] = { "2", "4", "6", "8", "10", "12" };
inline constexpr int kStageCounts[] = { 2, 4, 6, 8, 10, 12 };
static_assert(std::size(kStageNames) == std::size(kStageCounts));
inline constexpr int kMaxStages = 12;
inline constexpr size_t kDefaultStagesIndex = 2;
static_assert(kStageCounts[kDefaultStagesIndex] == 6);

/// SYNC: "Free" (the Rate knob), then every tempo division. Built from `device::kDivisionNames` as the
/// delay's and automod's lists are, so it cannot drift from the table it shadows.
inline constexpr size_t kSyncChoiceCount = 1 + device::kDivisionCount;
constexpr std::array<const char*, kSyncChoiceCount> makeSyncNames() {
	std::array<const char*, kSyncChoiceCount> out {};
	out[0] = "Free";
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		out[1 + i] = device::kDivisionNames[i];
	}
	return out;
}
inline constexpr auto kSyncNames = makeSyncNames();
inline constexpr size_t kSyncFree = 0;
/// RATE IS GATED ON SYNC, and a gate mask is 32 bits wide (`ParamDesc::gateMask`).
static_assert(kSyncChoiceCount <= 32);

[[nodiscard]] constexpr bool isSynced(uint32_t sync) noexcept {
	return sync != kSyncFree && sync < kSyncChoiceCount;
}
/// The division a synced choice names, in beats; 0 for Free.
[[nodiscard]] constexpr double syncDivisionBeats(uint32_t sync) noexcept {
	return isSynced(sync) ? device::kDivisionBeats[sync - 1] : 0.0;
}

//--------------------------------------------------------------------------------------------------//
// THE RANGES. Hz and seconds are stored as their log2 (`Unit::LogHertz`, `Unit::LogSeconds`), as     //
// every frequency and time control in this project is: even in octaves, so the knob's travel is     //
// spent where an ear can hear it.                                                                    //
//--------------------------------------------------------------------------------------------------//

inline constexpr double kCenterLog2Min = 5.321928094887363;      // 40 Hz
inline constexpr double kCenterLog2Max = 13.550746785383243;     // 12 kHz
inline constexpr double kCenterLog2Default = 9.643856189774725;  // 800 Hz

/// **THE BOTTOM IS A REAL DELAY, NOT "NO TIME AT ALL"** — so this device prints it through its own
/// `kTextOps` rather than the LogSeconds convention, which would read "off" here and at the bottoms of
/// the follower's Attack and Release. None of the three skips its stage at the minimum.
inline constexpr double kDelayLog2Min = -12.287712379549451;     // 0.2 ms
inline constexpr double kDelayLog2Max = -6.058893689053569;      // 15 ms
inline constexpr double kDelayLog2Default = -8.643856189774725;  // 2.5 ms

/// THE TOP IS "NO DAMPING": the engine bypasses the lowpass exactly there, so the default loop is pure —
/// and `kTextOps` prints it "open" rather than "20k", which would promise a filter that is not running.
inline constexpr double kDampingLog2Min = 8.965784284662087;     // 500 Hz
inline constexpr double kDampingLog2Max = 14.287712379549449;    // 20 kHz

inline constexpr double kRateLog2Min = -6.643856189774724;       // 0.01 Hz
inline constexpr double kRateLog2Max = 4.321928094887363;        // 20 Hz
inline constexpr double kRateLog2Default = -1.736965594166206;   // 0.3 Hz

inline constexpr double kEnvAttackLog2Min = -10.965784284662087;   // 0.5 ms
/// THE FOLLOWER'S LONG ENDS (lets get larger env time possibilities): Attack to 5 s, Release
/// to 20 s — slow swells the sweep can ride. The meaning is unchanged (log2 seconds, log taper), so the keys stay.
/// The follower is a double one-pole stepped per 16-sample tick, α = 1 − e^(−16/(τ·sr)): ~1.7e-5 at 20 s and 48 kHz,
/// far above double's resolution, so a long release is exact rather than stalled.
inline constexpr double kEnvAttackLog2Max = 2.321928094887362;     // 5 s
inline constexpr double kEnvAttackLog2Default = -6.643856189774724; // 10 ms
inline constexpr double kEnvReleaseLog2Min = -6.643856189774724;   // 10 ms
inline constexpr double kEnvReleaseLog2Max = 4.321928094887363;    // 20 s
inline constexpr double kEnvReleaseLog2Default = -2.321928094887362; // 200 ms

inline constexpr double kEnvSensDbMax = 24.0;
/// **WHERE "+0 FROM THE NOTE" SITS ON CENTER AND DELAY** (separate Center and Key Offset knobs were
/// confusing — Key Offset is retired and the one knob is the offset while a note is tracked). Each is its
/// knob's DEFAULT, so Key Track switched on at the default lands exactly on the note: the phaser's offset is
/// 12·log2(Center / 800 Hz) semitones, the flanger's −12·log2(Delay / 2.5 ms) (a longer delay is a lower note).
/// Pinned to the descriptors' defaults below (`static_assert`), so moving a default cannot silently move "+0".
inline constexpr double kCenterKeyRefLog2 = kCenterLog2Default; // 800 Hz
inline constexpr double kDelayKeyRefLog2 = kDelayLog2Default;   // 2.5 ms
inline constexpr double kStereoDegreesMax = 180.0;

/// THE PLANE'S TURN — the shaper's drag window (`shaper_types.h`'s Rotate), not a bound.
inline constexpr double kRotateWindow = 8388608.0;

//--------------------------------------------------------------------------------------------------//
// THE GATES — which values of a gate control make a follower mean something (`ParamDesc::gateMask`). //
//--------------------------------------------------------------------------------------------------//

inline constexpr uint32_t kPhaserOnly = 1u << static_cast<uint32_t>(Mode::Phaser);
inline constexpr uint32_t kFlangerOnly = 1u << static_cast<uint32_t>(Mode::Flanger);
inline constexpr uint32_t kWhenOn = 1u << 1;
inline constexpr uint32_t kWhenFree = 1u << kSyncFree;

inline constexpr const char* kTags[] = { "phaser", "flanger", "modulation", "fx" };

/// THE FACE FOR A GENERIC SURFACE (the device's own card, `PhaserCard`, arrived in Task 9 and does not read
/// this): EVERY control but the enable, which the head draws, so a surface that seats the generic card still
/// reaches them all. The sweep first, then the loop, then the two sources, then the cool side.
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::Mode),       static_cast<uint8_t>(Param::Stages),
	static_cast<uint8_t>(Param::Position),   static_cast<uint8_t>(Param::Mix),
	static_cast<uint8_t>(Param::Feedback),   static_cast<uint8_t>(Param::Damping),
	static_cast<uint8_t>(Param::Drive),      static_cast<uint8_t>(Param::Stereo),
	static_cast<uint8_t>(Param::LfoOn),      static_cast<uint8_t>(Param::LfoDepth),
	static_cast<uint8_t>(Param::Sync),       static_cast<uint8_t>(Param::RateHz),
	static_cast<uint8_t>(Param::EnvOn),      static_cast<uint8_t>(Param::EnvAmount),
	static_cast<uint8_t>(Param::EnvAttack),  static_cast<uint8_t>(Param::EnvRelease),
	static_cast<uint8_t>(Param::EnvSens),    static_cast<uint8_t>(Param::Sweep),
	static_cast<uint8_t>(Param::ThroughZero), static_cast<uint8_t>(Param::KeyTrack),
	static_cast<uint8_t>(Param::Rotate),
	// CENTER AND DELAY ARE THE MODE CELL'S VALUE (they follow `Mode`), so a surface that seats mode cells
	// draws them inside it; listed for the one that does not.
	static_cast<uint8_t>(Param::CenterHz),   static_cast<uint8_t>(Param::DelayMs),
	// THE EXPLORE PILLS, amount and power by group (`PhaserExpanded` seats them as FaderPills).
	static_cast<uint8_t>(Param::XSpread),    static_cast<uint8_t>(Param::XSpreadOn),
	static_cast<uint8_t>(Param::XStagger),   static_cast<uint8_t>(Param::XStaggerOn),
	static_cast<uint8_t>(Param::XShape),     static_cast<uint8_t>(Param::XShapeOn),
	static_cast<uint8_t>(Param::XColour),    static_cast<uint8_t>(Param::XColourOn),
	static_cast<uint8_t>(Param::XBias),      static_cast<uint8_t>(Param::XBiasOn),
	static_cast<uint8_t>(Param::XSkew),      static_cast<uint8_t>(Param::XSkewOn),
	static_cast<uint8_t>(Param::XRateSkew),  static_cast<uint8_t>(Param::XRateSkewOn),
	static_cast<uint8_t>(Param::XFbTilt),    static_cast<uint8_t>(Param::XFbTiltOn),
	static_cast<uint8_t>(Param::XPairing),   static_cast<uint8_t>(Param::XPairingOn),
	static_cast<uint8_t>(Param::XCurve),     static_cast<uint8_t>(Param::XCurveOn),
};

/// **A PILL'S TWO ROWS** — the shaper's extras (`shaper_types.h`, backlog II/IK), law and look: a KNOB in front of its
/// group's fields, `fieldWithOverride` (0..1, a Percent, default `kFieldNeutral` = 50%, the field untouched — 0 forces
/// it to 0, 100% to 1; a modulation destination: the engine re-derives from the fields it holds whenever it moves, no
/// triangle read, no rebuild) and a power (On by default, so a fresh device is the layer as Task 7 built it; OFF IS NO
/// EFFECT, the group's offsets exactly 0). THE POWER IS NOT A DESTINATION, the shaper's are: ten more would take the
/// device to 34 modulatable controls against the rack's stride of 32 (the retired control stride, which renumbers
/// every routing if moved). The law is `phase_space::offsetOf`. `gate`/`mask` is the ONE gate the descriptor can say
/// (`PhaserExpanded` covers the rest — its header lists each decision).
constexpr fx::ParamDesc pillKnobDesc(const char* key, const char* name, const char* shortName, const char* tooltip,
                                     int16_t gate = -1, uint32_t mask = 0) {
	return { .key = key, .name = name,
	         .min = 0.0,
	         .max = 1.0,
	         .def = feathers::kFieldNeutral,
	         .stepped = false,
	         .unit = fx::ParamUnit::Percent,
	         .names = {},
	         .modulatable = true,
	         .offsetsMayLeaveRange = false,
	         .tooltip = tooltip,
	         .zones = 0,
	         .modSpan = 0.0,
	         .zoneNames = {},
	         .gateControl = gate,
	         .gateMask = mask,
	         .centred = false,
	         .shortName = shortName,
	         .stacked = false,
	         .digitBox = false,
	         .modeCell = false,
	         // A PILL GATED ON THE MODE IS NOT THE MODE CELL'S VALUE (Center and Delay are): its own cell, as Stages'.
	         .ownCell = gate == static_cast<int16_t>(Param::Mode) };
}
constexpr fx::ParamDesc pillPowerDesc(const char* key, const char* name, const char* shortName, const char* tooltip,
                                      int16_t gate = -1, uint32_t mask = 0) {
	return { .key = key, .name = name,
	         .min = 0.0,
	         .max = 1.0,
	         .def = 1.0,
	         .stepped = true,
	         .unit = fx::ParamUnit::Choice,
	         .names = kOffOnNames,
	         .modulatable = false,
	         .offsetsMayLeaveRange = false,
	         .tooltip = tooltip,
	         .zones = 0,
	         .modSpan = 0.0,
	         .zoneNames = {},
	         .gateControl = gate,
	         .gateMask = mask,
	         .centred = false,
	         .shortName = shortName,
	         .stacked = false,
	         .digitBox = false,
	         .modeCell = false,
	         // A PILL GATED ON THE MODE IS NOT THE MODE CELL'S VALUE (Center and Delay are): its own cell, as Stages'.
	         .ownCell = gate == static_cast<int16_t>(Param::Mode) };
}
/// THE PILLS' GATES — what the descriptor can express: Pairing is the phaser's alone, and Stagger and Shape act only
/// through the LFO's term (each is multiplied by the LFO's amount, which is 0 with it off — `stageSweepsOf`,
/// `lfoShapeWith`), so they are dead while it is off. NOT RATE SKEW: see its row.
inline constexpr int16_t kGateMode = static_cast<int16_t>(Param::Mode);
inline constexpr int16_t kGateLfo = static_cast<int16_t>(Param::LfoOn);

/// KEY OFFSET, RETIRED — see `kCenterKeyRefLog2`. Never reuse the key.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "phaser_key_offset", "2026-10-03", "merged into Center: while a note is tracked, Center is the offset from it" },
	// THE TEN PILLS' ROUND-2 KNOBS (0..2, a multiplier on the offsets), retired when the knob became the house override
	// (0..1, 0.5 neutral) — `phaser_x_<group>_field` now. Same number, different meaning, so new keys.
	{ "phaser_x_spread", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_stagger", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_shape", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_colour", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_bias", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_skew", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_rate_skew", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_fb_tilt", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_pairing", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
	{ "phaser_x_curve", "2026-10-04", "pill law changed from multiplier to fieldWithOverride" },
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "PHFL",
	.name = kDeviceName,
	// BOTH (Task 9): `PhaserCard` on the face, `PhaserExpanded` behind the head's expand mark.
	.hasCard = true,
	.hasExpanded = true,
	// BUS ONLY: the flanger's through-zero reports latency, and a latency-bearing device is never run per
	// voice (registry.h, `latencySamples`, assumption L2).
	.perVoiceControl = -1,
	// THE PAD'S FOLDED TRAVEL, one float per phase — the shaper's arrangement (`shaper_types.h`).
	.extraState = phase_space::kPhaseCount,
	.extraStateKey = "travel",
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = -1,
	.faceControls = 0,
	.faceOrder = kFaceOrder,
	.faceColumns = 4,
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	// SCHEMA 2 (round 2, 2026-10-03): the travel grew 30 → 34 floats, which the ledger counts as a meaning change of
	// `travel`. THE MIGRATION IS THE IDENTITY, by construction of the index table: the four new phases are APPENDED,
	// so a schema-1 blob's thirty floats keep their indices and meanings, and the four it lacks load as 0 — home, no
	// offset (the row rule's "what fits", `rowOfValues`, which also notes the short blob).
	.schema = 2,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

/// EVERY ROW BY FIELD NAME — device.h records the positional-init hazard twice over.
inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "phaser_enabled", .name = "Phaser Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off passes the signal through untouched"},
    // THE MODE CELL. Not a destination: switching engines mid-note is a rebuild, not a modulation.
    {.key = "phaser_mode", .name = "Phaser Mode",
     .min = 0.0,
     .max = static_cast<double>(std::size(kModeNames) - 1),
     .def = static_cast<double>(Mode::Phaser),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Phaser sweeps notches made by allpass stages; Flanger sweeps a short delay into a comb",
     .shortName = "Mode",
     .modeCell = true},
    // OWN CELL: Stages is not the mode's value (Center is), only a control the Flanger has no use for.
    {.key = "phaser_stages", .name = "Phaser Stages",
     .min = 0.0,
     .max = static_cast<double>(std::size(kStageNames) - 1),
     .def = static_cast<double>(kDefaultStagesIndex),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kStageNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "How many allpass stages: every two make one notch",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kPhaserOnly,
     .centred = false,
     .shortName = "Stages",
     .stacked = false,
     .digitBox = false,
     .modeCell = false,
     .ownCell = true},
    // **CENTER AND DELAY ARE THE OFFSET FROM THE NOTE WHILE KEY TRACK HAS ONE** (retiring Key
    // Offset): the note sets the centre (`keyCornerLog2Hz`, `keyDelayLog2s`) and the knob moves it, by its distance from
    // its default (`kCenterKeyRefLog2`, `kDelayKeyRefLog2`) — so the default is "on the note". The stored number keeps
    // its unit; the card and window PRINT it in semitones while a note is tracked (`phaserOffsetText`).
    {.key = "phaser_center_hz", .name = "Phaser Center",
     .min = kCenterLog2Min,
     .max = kCenterLog2Max,
     .def = kCenterLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Where the notches sit with the sweep at rest. While Key Track has a note, the offset from it in "
                "semitones: the default puts the first notch on the note",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kPhaserOnly,
     .centred = false,
     .shortName = "Center"},
    {.key = "phaser_delay", .name = "Phaser Delay",
     .min = kDelayLog2Min,
     .max = kDelayLog2Max,
     .def = kDelayLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The flanger's delay with the sweep at rest: shorter puts the comb's teeth further apart. While Key "
                "Track has a note, the offset from it in semitones: the default tunes the comb to the note",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kFlangerOnly,
     .centred = false,
     .shortName = "Delay"},
    {.key = "phaser_feedback", .name = "Phaser Feedback",
     .min = -1.0,
     .max = 1.0,
     .def = 0.3,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Output fed back to the input: sharper, ringing notches. The sign flips which frequencies ring",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Feedback"},
    {.key = "phaser_damping", .name = "Phaser Damping",
     .min = kDampingLog2Min,
     .max = kDampingLog2Max,
     .def = kDampingLog2Max,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "A lowpass inside the feedback loop: lower darkens each pass. At the top it is out of the loop",
     .shortName = "Damp"},
    {.key = "phaser_drive", .name = "Phaser Drive",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Saturation inside the feedback loop: grit, and it holds high feedback from running away",
     .shortName = "Drive"},
    {.key = "phaser_mix", .name = "Phaser Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry against wet. 50% is the classic phaser, the deepest notches; 100% is wet only, a vibrato",
     .shortName = "Mix"},
    {.key = "phaser_position", .name = "Phaser Position",
     .min = -1.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The sweep by hand: where the notches sit before the LFO and envelope move them. Route a "
                "modulator here to drive the sweep",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Position"},
    {.key = "phaser_lfo_on", .name = "Phaser LFO",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "The internal LFO. Off leaves the sweep to Position, the envelope and whatever is routed in",
     .shortName = "LFO"},
    {.key = "phaser_lfo_depth", .name = "Phaser LFO Depth",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far the LFO sweeps either side of Position",
     .shortName = "Depth"},
    {.key = "phaser_sync", .name = "Phaser Sync",
     .min = 0.0,
     .max = static_cast<double>(kSyncChoiceCount - 1),
     .def = static_cast<double>(kSyncFree),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kSyncNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Free runs the LFO at Rate; a division locks one cycle to that length of the song",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     // THE LFO'S OWN CONTROLS MEAN NOTHING WHILE IT IS OFF, as the follower's mean nothing while Env is off.
     .gateControl = static_cast<int16_t>(Param::LfoOn),
     .gateMask = kWhenOn,
     .centred = false,
     .shortName = "Sync"},
    // GATED ON SYNC, NOT ON THE MODE: Sync is an ordinary Choice, so Rate keeps its own cell and is hidden
    // whenever a division is chosen. **A CONTROL HAS ONE GATE, and Rate's is Sync** — so Rate is ALSO dead while
    // the LFO is off, and `PhaserExpanded` (the one surface that shows Rate) disables it then too, as the card
    // disables Center/Delay under a tracked note.
    {.key = "phaser_rate_hz", .name = "Phaser Rate",
     .min = kRateLog2Min,
     .max = kRateLog2Max,
     .def = kRateLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How fast the LFO sweeps, in cycles a second",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Sync),
     .gateMask = kWhenFree,
     .centred = false,
     .shortName = "Rate"},
    {.key = "phaser_stereo", .name = "Phaser Stereo",
     .min = 0.0,
     .max = kStereoDegreesMax,
     .def = 90.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far apart the left and right sweeps run, in degrees of the LFO's cycle. 0 is mono, 180 "
                "is opposite",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     // A PHASE OFFSET OF AN LFO THAT IS OFF IS NOTHING.
     .gateControl = static_cast<int16_t>(Param::LfoOn),
     .gateMask = kWhenOn,
     .centred = false,
     .shortName = "Stereo",
     // DEGREES READ WHOLE: "90", not "90.00".
     .wholeReadout = true},
    {.key = "phaser_env_on", .name = "Phaser Env",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "The envelope follower: the input's level moves the sweep",
     .shortName = "Env"},
    {.key = "phaser_env_amount", .name = "Phaser Env Amount",
     .min = -1.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far, and which way, a louder input moves the sweep",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Amount"},
    // THE FOLLOWER'S SHAPE MEANS NOTHING WHILE THE FOLLOWER IS OFF (the design: "ENV controls greyed while
    // its pill is off"). The amount is the pill's own and is not gated, as the disperser's pills are not.
    {.key = "phaser_env_attack", .name = "Phaser Env Attack",
     .min = kEnvAttackLog2Min,
     .max = kEnvAttackLog2Max,
     .def = kEnvAttackLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How fast the follower rises when the input gets louder",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::EnvOn),
     .gateMask = kWhenOn,
     .centred = false,
     .shortName = "Attack"},
    {.key = "phaser_env_release", .name = "Phaser Env Release",
     .min = kEnvReleaseLog2Min,
     .max = kEnvReleaseLog2Max,
     .def = kEnvReleaseLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How fast the follower falls back when the input gets quieter",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::EnvOn),
     .gateMask = kWhenOn,
     .centred = false,
     .shortName = "Release"},
    {.key = "phaser_env_sens_db", .name = "Phaser Env Sens",
     .min = -kEnvSensDbMax,
     .max = kEnvSensDbMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Decibel,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Gain ahead of the follower: raise it for a quiet source, lower it for a loud one",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::EnvOn),
     .gateMask = kWhenOn,
     .centred = false,
     .shortName = "Sens"},
    {.key = "phaser_sweep", .name = "Phaser Sweep",
     .min = 0.0,
     .max = static_cast<double>(std::size(kSweepNames) - 1),
     .def = static_cast<double>(SweepShape::Normal),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kSweepNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Normal sweeps back and forth. Up and Down rise or fall for ever, the barberpole",
     // NO GATE (decided 2026-10-04): with the LFO off every voice sits on Position, but the barberpole is still
     // not Normal once the voices differ — the explore layer's Fb tilt gives each phaser voice its own loop, its spread
     // gives each flanger tap its own delay — and the windows (turning with ψ) decide which is heard. So Sweep is always live.
     .shortName = "Sweep"},
    // NOT MODULATABLE: it changes the device's latency, and a control that changes a latency must not be
    // (registry.h, `latencySamples`, assumption L1). OWN CELL, for Stages' reason.
    {.key = "phaser_through_zero", .name = "Phaser Through Zero",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Delays the dry signal too, so the sweep passes through zero delay and cancels completely, "
                "as two tape decks do. Adds latency",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kFlangerOnly,
     .centred = false,
     .shortName = "Thru 0",
     .stacked = false,
     .digitBox = false,
     .modeCell = false,
     .ownCell = true},
    {.key = "phaser_key_track", .name = "Phaser Key Track",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "The last note played sets the centre: the phaser's first notch, or the flanger's comb, "
                "lands on the note, and Center or Delay becomes the offset from it",
     .shortName = "Key"},
    // THE SHAPER'S ROTATE, row for row: a drag window, and not modulatable because a moving basis would
    // make one gesture mean something different from block to block.
    {.key = "phaser_rotate", .name = "Phaser Rotate",
     .min = -kRotateWindow,
     .max = kRotateWindow,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Turns the plane the pad travels in. Changes what a move MEANS, never the sound at rest",
     .shortName = "Rotate"},
    // THE EXPLORE PILLS (round 2, Task 11; the house law since 2026-10-04), in `phase_space::Group`'s order — keys
    // `phaser_x_<group>_field` and `phaser_x_<group>_on` (the round-2 `phaser_x_<group>` multipliers are retired). Each
    // stands in front of one group's fields: at 50% (the default) none moves the sound at home; away from it, every one
    // does (0 and 100% force the field whatever the travel).
    pillKnobDesc("phaser_x_spread_field", "Phaser X Spread", "Spread",
                 "The pad's spread of each stage's corner (the flanger's tap delays). 0 = field forced to its minimum, "
                 "middle = the terrain's value, full = forced to its maximum; switch off = no effect"),
    pillPowerDesc("phaser_x_spread_on", "Phaser X Spread On", "Spread", "The pad's per-stage spread on or off"),
    pillKnobDesc("phaser_x_stagger_field", "Phaser X Stagger", "Stagger",
                 "The pad's stagger of each stage's LFO phase (the flanger's taps). 0 = field forced to its minimum, "
                 "middle = the terrain's value, full = forced to its maximum; switch off = no effect",
                 kGateLfo, kWhenOn),
    pillPowerDesc("phaser_x_stagger_on", "Phaser X Stagger On", "Stagger", "The pad's per-stage LFO stagger on or off",
                  kGateLfo, kWhenOn),
    pillKnobDesc("phaser_x_shape_field", "Phaser X Shape", "Shape",
                 "The pad's bend of the LFO toward skewed, square or triangle; both ends bend it, the sine is the "
                 "middle at home. 0 = field forced to its minimum, middle = the terrain's value, full = forced to its "
                 "maximum; switch off = no effect",
                 kGateLfo, kWhenOn),
    pillPowerDesc("phaser_x_shape_on", "Phaser X Shape On", "Shape", "The pad's LFO shape on or off", kGateLfo, kWhenOn),
    pillKnobDesc("phaser_x_colour_field", "Phaser X Colour", "Colour",
                 "The pad's move of the feedback's damping corner, heard only while Damping is below open. 0 = field "
                 "forced to its minimum, middle = the terrain's value, full = forced to its maximum; switch off = no "
                 "effect"),
    pillPowerDesc("phaser_x_colour_on", "Phaser X Colour On", "Colour", "The pad's damping colour on or off"),
    pillKnobDesc("phaser_x_bias_field", "Phaser X Bias", "Bias",
                 "The pad's bias of the feedback's saturator off centre, heard only with some Drive. 0 = field forced "
                 "to its minimum, middle = the terrain's value, full = forced to its maximum; switch off = no effect"),
    pillPowerDesc("phaser_x_bias_on", "Phaser X Bias On", "Bias", "The pad's drive asymmetry on or off"),
    pillKnobDesc("phaser_x_skew_field", "Phaser X Skew", "Skew",
                 "The pad's move of the right channel's centre against the left's. 0 = field forced to its minimum, "
                 "middle = the terrain's value, full = forced to its maximum; switch off = no effect"),
    pillPowerDesc("phaser_x_skew_on", "Phaser X Skew On", "Skew", "The pad's stereo skew on or off"),
    pillKnobDesc("phaser_x_rate_skew_field", "Phaser X Rate Skew", "Rate skew",
                 "The pad's speeding or slowing of the right LFO against the left, so the stereo image rolls. 0 = "
                 "field forced to its minimum, middle = the terrain's value, full = forced to its maximum; switch off "
                 "= no effect"),
    // UNGATED IN THE DESCRIPTOR: under the barberpole ψ_R's rate is heard with the LFO off too (voices made unlike by the
    // Fb tilt or the spread), so LFO On is not its gate — `PhaserExpanded` shows it live with the LFO on or a barberpole.
    pillPowerDesc("phaser_x_rate_skew_on", "Phaser X Rate Skew On", "Rate skew", "The pad's rate skew on or off"),
    pillKnobDesc("phaser_x_fb_tilt_field", "Phaser X Fb Tilt", "Fb tilt",
                 "The pad's tilt of Feedback between the channels (the barberpole phaser's three voices). 0 = field "
                 "forced to its minimum, middle = the terrain's value, full = forced to its maximum; switch off = no "
                 "effect"),
    pillPowerDesc("phaser_x_fb_tilt_on", "Phaser X Fb Tilt On", "Fb tilt", "The pad's feedback tilt on or off"),
    pillKnobDesc("phaser_x_pairing_field", "Phaser X Pairing", "Pairing",
                 "The pad's pull of each pair of stages apart around their corner. Phaser only. 0 = field forced to its "
                 "minimum, middle = the terrain's value, full = forced to its maximum; switch off = no effect",
                 kGateMode, kPhaserOnly),
    pillPowerDesc("phaser_x_pairing_on", "Phaser X Pairing On", "Pairing", "The pad's notch pairing on or off",
                  kGateMode, kPhaserOnly),
    pillKnobDesc("phaser_x_curve_field", "Phaser X Curve", "Curve",
                 "The pad's bend of the sweep, so it lingers near the middle or near the ends. 0 = field forced to its "
                 "minimum, middle = the terrain's value, full = forced to its maximum; switch off = no effect"),
    pillPowerDesc("phaser_x_curve_on", "Phaser X Curve On", "Curve", "The pad's sweep curve on or off"),
};
static_assert(std::size(kParamDescs) == kParamCount);
// "+0 FROM THE NOTE" IS EACH KNOB'S DEFAULT — `kCenterKeyRefLog2`'s note.
static_assert(kParamDescs[static_cast<size_t>(Param::CenterHz)].def == kCenterKeyRefLog2);
static_assert(kParamDescs[static_cast<size_t>(Param::DelayMs)].def == kDelayKeyRefLog2);

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

/// THE CONTROLS AS STORED — Hz and seconds as their log2, exactly the row's numbers, so the codec is
/// lossless; the engine converts.
struct Settings {
	bool enabled = true;
	Mode mode = Mode::Phaser;
	int stages = kStageCounts[kDefaultStagesIndex];
	float centerLog2Hz = static_cast<float>(kCenterLog2Default);
	float delayLog2s = static_cast<float>(kDelayLog2Default);
	float feedback = 0.3f;
	float dampingLog2Hz = static_cast<float>(kDampingLog2Max);
	float drive = 0.0f;
	float mix = 0.5f;
	float position = 0.0f;
	bool lfoOn = true;
	float lfoDepth = 0.5f;
	uint32_t sync = static_cast<uint32_t>(kSyncFree);
	float rateLog2Hz = static_cast<float>(kRateLog2Default);
	float stereoDegrees = 90.0f;
	bool envOn = false;
	float envAmount = 0.5f;
	float envAttackLog2s = static_cast<float>(kEnvAttackLog2Default);
	float envReleaseLog2s = static_cast<float>(kEnvReleaseLog2Default);
	float envSensDb = 0.0f;
	SweepShape sweep = SweepShape::Normal;
	bool throughZero = false;
	bool keyTrack = false;
	float rotate = 0.0f;
	/// **THE PAD'S FOLDED TRAVEL, one term per phase.** Lives past the declared params in the row (see
	/// `kTravelFirst`), so `rowOf`/`stateOf` do not carry it; the ops table's `setFields` does, as the
	/// shaper's does.
	std::array<float, phase_space::kPhaseCount> travel {};
	/// **THE PILLS** (`Param::XSpread …`), by `phase_space::Group`: each group's knob 0..1 and its power. The derive puts
	/// group g's knob in front of its fields (`phase_space::offsetOf`, the house `fieldWithOverride`) — the defaults,
	/// 0.5 (`kFieldNeutral`) and On, are the layer as Task 7 built it.
	std::array<float, phase_space::kGroupCount> pillKnob = phase_space::Pills {}.knob;
	std::array<bool, phase_space::kGroupCount> pillOn = phase_space::Pills {}.on;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// THE STORED PILLS AS THE DERIVE TAKES THEM — what the engine, the response model and the terrain derive with.
[[nodiscard]] inline phase_space::Pills pillsOf(const Settings& s) noexcept {
	return phase_space::Pills { .knob = s.pillKnob, .on = s.pillOn };
}

struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, kParamCount>;

/// **WHERE THE FOLDED TRAVEL SITS IN THE WIDE ROW** — immediately past the declared params, the blob
/// contract `device_row.h` states: `params.size() + extraState`.
inline constexpr size_t kTravelFirst = kParamCount;

/// Decode a row, with ABSENT FIELDS AT THEIR DECLARED DEFAULTS rather than at zero (`fieldsPresent` is
/// how many of `row` are real — device_row.h's rule).
Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);
float valueOf(const State& state, Param param);
void setValueOf(State* state, Param param, float value);

/// **WHAT THE DEVICE PUBLISHES, once per process call, for its surfaces** — the facts only the running device
/// has: where each channel's sweep stands after the LFO and the follower, and what that makes of each engine.
/// Each is the value the LAST control tick of the block computed (the one the engine is ramping toward), so a
/// surface draws what the device is doing, not a second computation that might disagree with it.
enum class Reading : size_t {
	SweepL,      ///< the left channel's sweep point, −1..1 (the barberpole's: the voices' window-weighted centre)
	SweepR,      ///< the right channel's
	LfoPhase,    ///< the LFO's phase on the left channel, 0..1 (the right runs Stereo/360 ahead)
	Env,         ///< the envelope follower, 0..1 over [−48, 0] dBFS (`envelopeOf`)
	/// The phaser's corner on the left, Hz, at the channel's sweep — BEFORE the explore layer's per-stage spread and
	/// LFO offsets (each stage's own is `stageCornerHz` of its own sweep; at home every stage sits here). UNDER THE
	/// BARBERPOLE, the voices' corners averaged IN LOG by their windows: 2^(Σ w_k·log2 fc_k).
	CornerHzL,
	CornerHzR,   ///< the right's (moved by the explore layer's stereo skew)
	/// THE FLANGER'S DELAY ON THE LEFT, ms, as the comb hears it: the wet's read, or with through-zero the wet's
	/// read LESS the dry's — the relative delay, which crosses zero and goes negative below the crossing. UNDER
	/// THE BARBERPOLE, the taps' delays averaged by their windows (relative under through-zero, too).
	DelayMsL,
	DelayMsR,    ///< the right's
	/// THE TRACKED NOTE'S OWN FREQUENCY, Hz — without the Center/Delay offset, which the readers apply through
	/// `targetsOf` as the engine does (so the plot's centre is the sound's). 0 while Key Track is off, and while it is
	/// on with no note since the device was reset.
	NoteHz,
	/// The barberpole's ramp phase ψ on the left, 0..1 (voice k is at ψ + k/3); 0 with Sweep = Normal.
	BarberPhase,
	kCount,
};
inline constexpr size_t kReadingCount = static_cast<size_t>(Reading::kCount);

//--------------------------------------------------------------------------------------------------//
// THE ARITHMETIC EVERY ENGINE SHARES — pure functions, so the engine, the tests and (Task 8) the plot //
// read one statement of each.                                                                        //
//--------------------------------------------------------------------------------------------------//

/// THE CONTROL TICK: corners and delays are computed once per this many samples and ramped linearly
/// across it, so a coefficient never steps and a transcendental is not paid per sample.
inline constexpr int kControlTick = 16;

/// How far a full sweep (s = ±1) moves the phaser's corner, in octaves either side of Center.
inline constexpr double kPhaserOctaves = 3.0;
/// The corner's hard limits. The top is under Nyquist with room for the bilinear prewarp (tan of π·0.45 is
/// 6.3; at 0.5 it is a pole).
inline constexpr double kCornerMinHz = 20.0;
inline constexpr double kCornerMaxFraction = 0.45;

/// **THE LINEAR LOOP'S CEILING.** With Drive at 0 the loop is linear, and at |fb| = 1 an allpass loop is
/// LOSSLESS — whatever frequency the cascade turns through a whole cycle rings for ever and noise in
/// accumulates without bound. 0.995 keeps a ring of a few seconds and a finite gain (1/(1 − 0.995) =
/// 46 dB at the peak). As Drive rises the saturator takes over the bounding, so the ceiling opens linearly
/// to the full ±1 by `kCeilingOpenDrive` — continuous in Drive, never a switch at 0.
inline constexpr double kMaxLinearFeedback = 0.995;
inline constexpr double kCeilingOpenDrive = 0.1;
/// The saturator's input gain at full Drive: `sat(v) = tanh(v·k)/k`, k = 7·Drive.
inline constexpr double kDriveGainSpan = 7.0;
/// `fasttanh` is a Padé approximant that stops saturating past ±5 (fast_math_approx.h), so its argument
/// is clamped here first.
inline constexpr float kTanhArgLimit = 4.5f;

/// States below this are snapped to exact zero — 300 dB down, far under any converter and far above the
/// denormals, where a geometric decay parks instead of arriving (test_fx_devices' silence sweep).
inline constexpr float kSnapLevel = 1.0e-15f;

/// **THE LOOP'S SATURATOR**: `tanh(v·k)/k` with k = 7·Drive, and the identity at Drive 0. Continuous in
/// Drive: tanh(vk)/k = v − (vk)²·v/3 + …, which goes to v as k → 0, so the smallest Drive is a whisper of
/// curve rather than a step. Bounded by 1/k — what holds a ±1 loop.
[[nodiscard]] inline float saturate(float v, float drive) noexcept {
	const float k = static_cast<float>(kDriveGainSpan) * drive;
	if (k == 0.0f) {
		return v;
	}
	return dsp::fasttanh(std::clamp(v * k, -kTanhArgLimit, kTanhArgLimit)) / k;
}

/// **THE SATURATOR, ASYMMETRIC — the explore layer's drive asymmetry (index 28).** A bias `b` inside the tanh and the
/// DC it makes removed:
///
///     sat_b(v) = (tanh(v·k + b) − tanh(b)) / k
///              = (1 − tanh²(b)) · tanh(v·k) / (k · (1 + tanh(b)·tanh(v·k)))     (tanh's addition formula)
///
/// - **COMPUTED IN THE SECOND FORM.** The subtraction loses a small v·k against b's float precision (v·k + b rounds to
///   a neighbour of b, and the difference comes back as whole ULPs of tanh(b)); the product form has no b-sized term
///   to round against — tiny in, tiny out, linearly.
/// - **ZERO IN, ZERO OUT, EXACTLY** (tanh(0) is 0), so a silent loop stays exactly silent and a tail reaches exact zero
///   as the symmetric one does — the DC is removed in the algebra, not by a filter with its own tail to settle.
/// - **NON-EXPANSIVE: ITS SLOPE NEVER EXCEEDS ONE**, so |sat_b(v)| ≤ |v| and the loop is never hotter than the linear
///   one. Its slope (1 − t²)(1 − tanh²b)/(1 + tanh(b)·t)² peaks at exactly 1 where t = −tanh(b), and is 1 − tanh²(b)
///   at 0. **THIS IS WHY IT IS NOT NORMALISED TO A UNIT SLOPE AT ZERO** — MEASURED: that first draft (the same curve
///   times 1/(1 − tanh²b)) has a slope over one on one side of zero, and at Feedback 0.9, Drive 0.6, bias 0.4 the loop
///   SELF-OSCILLATED (an output of 0.2 still ringing 400 blocks into silence) — a tail that never ends.
/// - **THE BIAS RAMPS IN WITH DRIVE**, b·min(1, Drive/`kCeilingOpenDrive`), so as Drive → 0 the curve goes to the
///   symmetric one and on to the identity: continuous in Drive, never a step at 0 (with no saturator there is nothing
///   to bias). It is `saturate(v, drive)` exactly at b = 0.
/// - Bounded by (1 + |tanh b|)/k: 1.47/k at |b| = 0.5, against the symmetric 1/k — what holds a ±1 loop.
///
/// tanh(b) IS A FUNCTION OF THE BIAS AND THE DRIVE ALONE, so the engine computes it once a tick (`biasTanhOf`, from the
/// drive ramp's end value) and runs `saturateWith` per sample; this function is the two together, for a caller with
/// one sample to shape.
[[nodiscard]] inline float biasTanhOf(float drive, float bias) noexcept {
	if (bias == 0.0f) {
		return 0.0f;
	}
	const float b = bias * std::clamp(drive / static_cast<float>(kCeilingOpenDrive), 0.0f, 1.0f);
	return dsp::fasttanh(std::clamp(b, -kTanhArgLimit, kTanhArgLimit));
}
/// The asymmetric saturator with tanh(b) in hand — `saturate(v, drive)` exactly when it is 0.
[[nodiscard]] inline float saturateWith(float v, float drive, float biasTanh) noexcept {
	const float k = static_cast<float>(kDriveGainSpan) * drive;
	if (k == 0.0f || biasTanh == 0.0f) {
		return saturate(v, drive);
	}
	const float tb = biasTanh;
	const float t = dsp::fasttanh(std::clamp(v * k, -kTanhArgLimit, kTanhArgLimit));
	return (1.0f - tb * tb) * t / (k * (1.0f + tb * t));
}
[[nodiscard]] inline float saturate(float v, float drive, float bias) noexcept {
	return saturateWith(v, drive, biasTanhOf(drive, bias));
}

/// **THE LOOP'S DC BLOCKER'S CORNER, Hz** — a TPT one-pole highpass after the asymmetric saturator, run ONLY while the
/// bias is nonzero (at home the loop is the plain one, bit for bit). An asymmetric curve RECTIFIES: a loud loop makes
/// DC, and the loop's DC gain (every allpass is +1 at DC, the damping lowpass too) would recirculate it toward
/// fb/(1 − fb) of itself — a DC offset on the output and a sweep-independent push into the saturator's bent side. 10 Hz
/// sits under the musical range: the highpass turns 100 Hz by atan(10/100) = 5.7° and loses 0.04 dB there, so the
/// loop's resonances barely move. Its time constant, 1/(2π·10) = 15.9 ms, is what a change of bias settles in.
inline constexpr double kLoopDcCornerHz = 10.0;

/// **THE SWEEP POINT, −1..1**: Position, plus the LFO's term (LfoDepth · lfo(φ), 0 with the LFO off), plus the
/// follower's (EnvAmount · env, 0 with it off), clamped. Per channel: the right's LFO term reads its own phase.
/// With both sources off it is Position exactly — the road an external modulator drives the sweep by.
/// **A NON-FINITE SUM FALLS BACK TO POSITION** (a transport handing over a nonsense tempo can put a NaN in the
/// LFO's term), so no corner or delay is ever computed from a NaN.
///
/// **THE EXPLORE LAYER'S SWEEP CURVE** (index 33): `curveExp` = 2^c bends the SUM before the clamp, s → sign(s)·|s|^e
/// (`curvedSweepOf`) — e < 1 pushes the sweep toward its ends, e > 1 lets it linger near the middle. At e = 1 (home)
/// the sum is untouched, bit for bit (the curve is not even evaluated).
///
/// **A FINITE SLOPE THROUGH ZERO** (the Task 11 review): |s|^e with e < 1 is vertical at 0, so every zero crossing of
/// the sweep stepped the flanger's per-sample read (~22 samples at e = 0.25). Under |s| < `kCurveLinearSpan` the curve
/// is the straight line to (ε, ε^e) — C0 where it meets the power curve, slope ε^(e−1) (9.5 at e = 0.25), and ±1 still
/// maps to ±1. Not C1: the power curve's slope there is e·ε^(e−1), a corner of ratio e, which bends the read's RATE and
/// never steps the read. e ≥ 1 is flat at 0 already and takes the power curve unchanged.
inline constexpr double kCurveLinearSpan = 0.05;
[[nodiscard]] inline double curvedSweepOf(double sum, double curveExp) noexcept {
	if (curveExp == 1.0) {
		return sum;
	}
	const double a = std::fabs(sum);
	if (curveExp < 1.0 && a < kCurveLinearSpan) {
		return sum * (std::pow(kCurveLinearSpan, curveExp) / kCurveLinearSpan);
	}
	return std::copysign(std::pow(a, curveExp), sum);
}
/// THE CURVE'S EXPONENT from the derived c: 2^c, so c = 0 is exactly 1.
[[nodiscard]] inline double sweepCurveExponentOf(double c) noexcept { return c == 0.0 ? 1.0 : std::exp2(c); }
[[nodiscard]] inline double sweepOf(double position, double lfoTerm = 0.0, double envTerm = 0.0,
                                    double curveExp = 1.0) noexcept {
	const double sum = curvedSweepOf(position + lfoTerm + envTerm, curveExp);
	const double s = std::isfinite(sum) ? sum : position;
	return std::isfinite(s) ? std::clamp(s, -1.0, 1.0) : 0.0;
}

/// **THE LFO'S SHAPE: A SINE, −1..1, at phase 0..1.** One function, so the explore layer has one place to bend it;
/// at home it is this sine.
[[nodiscard]] inline double lfoShape(double phase) noexcept {
	return std::sin(2.0 * std::numbers::pi * phase);
}

/// How far full skew moves the shape's peak: the rising half's share of the cycle goes from 0.5 to 0.1.
inline constexpr double kSkewReach = 0.4;
/// The squareness's soft-clip gain at full: tanh(g·y)/tanh(g), g = 6 — within 1% of a square over 80% of a cycle.
inline constexpr double kSquareGain = 6.0;

/// **THE LFO'S SHAPE, BENT BY THE EXPLORE LAYER** (`phase_space::LfoBend`, indices 24..26). At home — all three 0 —
/// it is `lfoShape(phase)`, the same expression, so zero travel is the sine bit for bit. Otherwise, in order:
///   - SKEW bends the phase map: the rising half takes `0.5 − 0.4·skew` of the cycle and the falling half the rest,
///     piecewise linear, so 0 ↦ 0 and 1 ↦ 1 and the shape stays continuous across the wrap;
///   - SQUARENESS soft-clips: tanh(g·y)/tanh(g), g = 6·square — continuous into 0 (the limit is y), ±1 at ±1;
///   - TRIANGLE BLEND mixes toward the triangle of the (skewed) phase, 0 → +1 at a quarter → −1 at three quarters,
///     the sine's own phase.
/// Every step maps [−1, 1] into itself, so the shape is continuous in every amount and stays in [−1, 1].
///
/// `lfoShapeWith` takes the squareness's normaliser 1/tanh(g) already in hand (`squareNormOf`) — a function of the
/// bend alone, so a caller evaluating many phases of one bend (the flanger, per sample) pays its tanh once a tick.
[[nodiscard]] inline double squareNormOf(const phase_space::LfoBend& bend) noexcept {
	return bend.square > 0.0 ? 1.0 / std::tanh(kSquareGain * std::min(bend.square, 1.0)) : 1.0;
}
[[nodiscard]] inline double lfoShapeWith(double phase, const phase_space::LfoBend& bend, double squareNorm) noexcept {
	if (bend.atHome()) {
		return lfoShape(phase);
	}
	double p = phase - std::floor(phase);
	if (!(p >= 0.0 && p < 1.0)) {
		p = 0.0;
	}
	if (bend.skew > 0.0) {
		const double b = 0.5 - kSkewReach * std::min(bend.skew, 1.0);
		p = p < b ? 0.5 * p / b : 0.5 + 0.5 * (p - b) / (1.0 - b);
	}
	double y = std::sin(2.0 * std::numbers::pi * p);
	if (bend.square > 0.0) {
		y = std::tanh(kSquareGain * std::min(bend.square, 1.0) * y) * squareNorm;
	}
	if (bend.triangle > 0.0) {
		const double tri = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
		y += std::min(bend.triangle, 1.0) * (tri - y);
	}
	return y;
}
[[nodiscard]] inline double lfoShape(double phase, const phase_space::LfoBend& bend) noexcept {
	return lfoShapeWith(phase, bend, squareNormOf(bend));
}
/// **THE BARBERPOLE** (Sweep = Up | Down): three VOICES of the engine at ramp phases ψ_k = frac(ψ + k/3), each
/// sweeping `s_k = Position + LfoDepth·(2ψ_k − 1) + EnvAmount·env` — a sawtooth across the LFO's span, so voice k
/// climbs the whole sweep and drops back to the bottom once a cycle — and each heard through the window
/// `w_k = (2/3)·sin²(π ψ_k)`. The window is ZERO where its voice drops, and the three always sum to exactly one
/// (sin²x + sin²(x + π/3) + sin²(x + 2π/3) = 3/2), so the motion never ends and never steps. The LFO's SHAPE is
/// not used: the ramp is the motion.
///
/// **RATE SETS THE CLIMB, NOT THE CROSSINGS** (the barberpole climbs at a speed Depth does not
/// change). A voice climbs 2·Depth of sweep per turn of ψ — 6·Depth octaves of corner in the Phaser (3 octaves a
/// unit of sweep), 4·Depth octaves of delay in the Flanger (`kFlangerOctaves` = 2) — so if ψ turned at Rate, Depth
/// would widen the span AND speed the motion. So ψ has its OWN phase, turning at Rate·(kBarberDepthRef / Depth)
/// (`barberCyclesPerSecond`): the climb is 3·Rate octaves a second in the Phaser and 2·Rate in the Flanger, at every
/// Depth, and at the default Depth (0.5) ψ
/// turns at Rate exactly, so the default sounds as it did. It is song-locked as the LFO is, by the same
/// `TransportLock` at the SCALED period (a moving Depth frequency-modulates ψ rather than re-placing it); a synced
/// division is one crossing per division at Depth 0.5 ONLY — at any other Depth the crossings are N·(Depth/0.5)
/// divisions apart and sit on the division grid only where that ratio is whole. ψ_L is that phase (Up) or one minus it (Down,
/// `barberPhaseOf`); Normal's φ is untouched by any of this.
inline constexpr int kBarberVoices = 3;

/// THE DEPTH AT WHICH ψ TURNS AT RATE EXACTLY — the LFO Depth's default, so the default barberpole is unchanged.
inline constexpr double kBarberDepthRef = 0.5;
/// **ψ NEVER TURNS FASTER THAN THIS**, Hz. Each voice is heard through a window that opens and closes once a turn of
/// ψ, so ψ's rate is an amplitude modulation of each voice; at 8 Hz the sidebands it puts on a voice sit within 8 Hz
/// of each partial, inside one critical band and below the ~15 Hz where AM begins to read as roughness — the windows
/// stay a motion, not a buzz. At small Depth the cap holds (below Depth = Rate·0.5/8) and the climb slows there.
inline constexpr double kBarberMaxRateHz = 8.0;
/// THE DEPTH THE SCALE IS TAKEN AT, AT LEAST — only a guard on the divide: at Depth 0 (or the LFO off) every voice
/// sits on Position and ψ's speed is inaudible. For any Rate above kBarberMaxRateHz·kBarberDepthMin/kBarberDepthRef
/// (0.25 Hz) the cap binds before this floor does.
inline constexpr double kBarberDepthMin = 1.0 / 64.0;

/// ψ's RATE, cycles a second, for the LFO running at `lfoCyclesPerSecond` with the glided Depth `depth` (0 with the
/// LFO off): Rate·(kBarberDepthRef / max(|Depth|, kBarberDepthMin)), capped at kBarberMaxRateHz. Non-finite or
/// negative rates answer 0.
[[nodiscard]] inline double barberCyclesPerSecond(double lfoCyclesPerSecond, double depth) noexcept {
	const double d = std::max(std::fabs(depth), kBarberDepthMin);
	const double r = lfoCyclesPerSecond * (kBarberDepthRef / d);
	return std::isfinite(r) && r > 0.0 ? std::min(r, kBarberMaxRateHz) : 0.0;
}

[[nodiscard]] constexpr bool isBarberpole(SweepShape sweep) noexcept {
	return sweep == SweepShape::Up || sweep == SweepShape::Down;
}

/// **ψ FROM THE BARBERPOLE'S OWN PHASE ρ** (`Phaser::barberPhaseFor`, turning at `barberCyclesPerSecond`): Up rides
/// ρ, Down rides 1 − ρ — so both are as song-locked as ρ is, and Down is Up played backwards. In [0, 1) (`1 − 0` and a
/// rounding just under an integer both land on 0).
[[nodiscard]] inline double barberPhaseOf(double lfoPhase, SweepShape sweep) noexcept {
	const double p = sweep == SweepShape::Down ? 1.0 - lfoPhase : lfoPhase;
	const double f = p - std::floor(p);
	return f < 1.0 && f >= 0.0 ? f : 0.0;
}

/// Voice `voice`'s ramp phase: ψ + voice/3, wrapped into [0, 1).
[[nodiscard]] inline double barberVoicePhase(double psi, int voice) noexcept {
	const double p = psi + static_cast<double>(voice) / static_cast<double>(kBarberVoices);
	const double f = p - std::floor(p);
	return f < 1.0 && f >= 0.0 ? f : 0.0;
}

/// **THE WINDOW FROM cos(2πψ_k)**: (2/3)·sin²(πψ_k) = (1 − cos 2πψ_k)/3. The flanger reads its windows PER SAMPLE
/// off a rotating phasor (one complex multiply a channel, its real part against the three cube roots of unity),
/// so this is the one statement both engines' windows go through.
[[nodiscard]] inline double barberGainOfCos(double cosTwoPiPsi) noexcept { return (1.0 - cosTwoPiPsi) / 3.0; }

/// **VOICE k's WINDOW**, w_k = (2/3)·sin²(π ψ_k): 0 at its wrap, 2/3 mid-travel.
[[nodiscard]] inline double barberGain(double voicePhase) noexcept {
	return barberGainOfCos(std::cos(2.0 * std::numbers::pi * voicePhase));
}

/// **VOICE k's SWEEP**: Position, plus the ramp across the LFO's span, plus the follower's term — the plain
/// sweep with `2ψ_k − 1` where the LFO's shape was (the Env term ADDS, by design, 2026-10-03).
[[nodiscard]] inline double barberSweepOf(double position, double lfoDepth, double voicePhase, double envTerm,
                                          double curveExp = 1.0) noexcept {
	return sweepOf(position, lfoDepth * (2.0 * voicePhase - 1.0), envTerm, curveExp);
}

/// **THE ENVELOPE FOLLOWER'S FLOOR**: its 0..1 reading spans [−48, 0] dBFS of the (Sens-scaled) input peak.
inline constexpr double kEnvFloorDb = -48.0;
/// THE FOLLOWER'S LEVEL (a linear peak) AS ITS 0..1 READING: 0 at or below −48 dBFS (and for silence), 1 at
/// or above 0 dBFS, linear in dB between.
[[nodiscard]] inline double envelopeOf(double level) noexcept {
	if (!(level > 0.0)) {
		return 0.0;
	}
	return std::clamp((20.0 * std::log10(level) - kEnvFloorDb) / -kEnvFloorDb, 0.0, 1.0);
}

/// THE PHASER'S CORNER at sweep `s`: `Center · 2^(3s)`, clamped to [20 Hz, 0.45·sr].
[[nodiscard]] inline double phaserCornerHz(double centerLog2Hz, double s, double sampleRate) noexcept {
	return std::clamp(std::exp2(centerLog2Hz + s * kPhaserOctaves), kCornerMinHz, kCornerMaxFraction * sampleRate);
}

/// **STAGE i's CORNER**: `Center · 2^(3·s_i + spread_i)` — its own sweep, moved by the explore layer's per-stage
/// spread in octaves (indices 0..11; 0.0 at home, where this is `phaserCornerHz` bit for bit: adding +0.0 is
/// exact). Clamped as the plain corner is, AFTER the spread, so a spread stage is never outside [20 Hz, 0.45·sr].
[[nodiscard]] inline double stageCornerHz(double centerLog2Hz, double s, double spreadOctaves,
                                          double sampleRate) noexcept {
	return std::clamp(std::exp2(centerLog2Hz + s * kPhaserOctaves + spreadOctaves), kCornerMinHz,
	                  kCornerMaxFraction * sampleRate);
}

/// **STAGE i's WHOLE EXPLORE OFFSET, octaves** — its spread (indices 0..11) plus the NOTCH PAIRING (index 32): stage 2j
/// up by p, stage 2j+1 down by p, so each pair parts around the corner their spread puts them at. What `stageCornerHz`
/// is handed by the engine (`aimStages`) and the response model alike. At home it is +0.0 exactly (0.0 + ±0.0).
[[nodiscard]] inline double stageSpreadOf(size_t stage, const phase_space::Explore& e) noexcept {
	const double pair = (stage % 2 == 0) ? e.pairingOctaves : -e.pairingOctaves;
	return e.spreadOctaves[stage % static_cast<size_t>(kMaxStages)] + pair;
}

/// **THE FEEDBACK TILT'S SHARE FOR ONE LOOP** (index 31), added to the stored Feedback before its clamp and ceiling
/// (`loopFeedbackOf`). `perVoice` is the PHASER'S barberpole, where every voice has its own loop: voice k gets
/// tilt·(k − 1), both channels. Everywhere else — the phaser's Normal, and the flanger in either sweep, whose taps share
/// one loop a channel — the channels split it: L + tilt/2, R − tilt/2. +0.0 at home, exactly.
[[nodiscard]] inline double feedbackTiltOf(bool perVoice, int voice, int channel, double tilt) noexcept {
	if (perVoice) {
		return tilt * static_cast<double>(voice - 1);
	}
	return channel == 1 ? -0.5 * tilt : 0.5 * tilt;
}

/// **AN LFO PHASE OFFSET AS A SWEEP VALUE**: a sweep `s` moved by `LfoDepth·(lfoShape(φ + off) − lfoShape(φ))`,
/// clamped — the two shape readings already in hand. The barberpole's per-stage road (`stageSweepsOf`) and the
/// flanger's per-tap road (its tick target and its per-sample taps) both go through it.
[[nodiscard]] inline double offsetSweepOf(double s, double lfoAmount, double shapeAtOffset, double shapeHome) noexcept {
	return sweepOf(s, lfoAmount * (shapeAtOffset - shapeHome));
}

/// **THE LFO SHAPES ONE CHANNEL'S STAGES READ THIS TICK**: lfoShape(φ + off_i) only where off_i (explore indices
/// 12..23) is nonzero, and lfoShape(φ) only if any is — so at home no stage pays a shape, and with travel each shape is
/// read once a channel rather than once a voice.
struct StageShapes {
	std::array<double, kMaxStages> at {}; ///< lfoShape(φ + off_i); 0 where off_i is 0 (never read there)
	double home = 0.0;                    ///< lfoShape(φ); 0 when `any` is false (never read then)
	bool any = false;                     ///< some off_i is nonzero
};
[[nodiscard]] inline StageShapes stageShapesOf(double lfoPhase, const phase_space::Explore& e, double squareNorm) noexcept {
	StageShapes out;
	for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
		const double off = e.lfoPhase[i];
		if (off != 0.0) {
			out.at[i] = lfoShapeWith(lfoPhase + off, e.bend, squareNorm);
			out.any = true;
		}
	}
	if (out.any) {
		out.home = lfoShapeWith(lfoPhase, e.bend, squareNorm);
	}
	return out;
}

/// **EVERY STAGE'S SWEEP FOR ONE VOICE OF ONE CHANNEL** — the one statement of the explore layer's per-stage LFO
/// offsets, read by the engine's tick and (Task 8) by the response model. Stage i with off_i = 0 sweeps the voice's
/// own `voiceSweep`, exactly. Otherwise:
///   - **SWEEP = NORMAL: THE OFFSET ACTS ON φ** — the plain sweep read at lfoShape(φ + off_i):
///     `sweepOf(Position, LfoDepth·lfoShape(φ + off_i), envTerm)`, which has no wrap to hide.
///   - **BARBERPOLE: ON THE SWEEP VALUE, NEVER ON ψ** (`offsetSweepOf`): s_k + LfoDepth·(lfoShape(φ + off_i) −
///     lfoShape(φ)), φ the channel's LFO phase (the one ψ is taken from). **DECISION (2026-10-03):** offsetting
///     ψ per stage would make a stage wrap (jump the whole sweep) while its voice's window is nonzero, and the wrap's
///     click is exactly what the window exists to hide. This mapping has no wrap: it is the difference of two readings
///     of one continuous, periodic shape, so it is smooth in φ, bounded by 2·LfoDepth, and — like the Normal mapping —
///     spreads the stages by the LFO's own motion: the stage leads or lags the sweep by off_i of the LFO's cycle.
/// `shapes` is `stageShapesOf` at the channel's φ.
/// `curveExp` is the sweep curve's (index 33): a Normal stage's sweep is a whole sweep and is curved as the channel's
/// is; a barberpole stage's is the voice's already-curved s_k plus an offset, not curved again.
inline void stageSweepsOf(std::array<double, kMaxStages>& out, bool barberpole, double voiceSweep, double position,
                          double lfoAmount, double envTerm, const phase_space::Explore& e,
                          const StageShapes& shapes, double curveExp = 1.0) noexcept {
	for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
		if (e.lfoPhase[i] == 0.0) {
			out[i] = voiceSweep;
		}
		else if (!barberpole) {
			out[i] = sweepOf(position, lfoAmount * shapes.at[i], envTerm, curveExp);
		}
		else {
			out[i] = offsetSweepOf(voiceSweep, lfoAmount, shapes.at[i], shapes.home);
		}
	}
}

/// **THE CENTRE A CHANNEL SWEEPS AROUND, IN TWO STEPS** — the engine glides between them, so they are two functions
/// and the response model (Task 8) composes them without the glide: `channelCentreLog2(centreTargetLog2(...), ...)`.
/// STEP ONE, `centreTargetLog2` (key tracking chosen over the knob), sits with key tracking below.
///
/// STEP TWO, `channelCentreLog2`: THE PHASER'S CHANNEL CENTRE — the (glided) centre, the right channel moved by the
/// explore layer's stereo skew (index 29; +0.0 at home, which is exact). The flanger's channel skew rides its tap
/// offset instead (`tapDelayOffsetLog2`), because its per-sample read shares one delay ramp across channels.
[[nodiscard]] inline double channelCentreLog2(double centreLog2, int channel, const phase_space::Explore& e) noexcept {
	return centreLog2 + (channel == 1 ? e.stereoOctaves : 0.0);
}

/// **THE FLANGER'S PER-TAP EXPLORE OFFSETS** (the index table in `phaser_phase_space.h`): Normal reads ONE tap a
/// channel — spread 0 and LFO offset 12 on L, 1 and 13 on R (so 2 and 14 are inert there) — and the barberpole's tap
/// k reads index k on both channels; the stereo skew adds to R's delay. 0 for a tap Normal does not run.
[[nodiscard]] inline bool tapReadsExplore(bool barberpole, int tap) noexcept { return barberpole || tap == 0; }
[[nodiscard]] inline size_t tapExploreIndex(bool barberpole, int channel, int tap) noexcept {
	return static_cast<size_t>(barberpole ? tap : channel);
}
/// The tap's delay offset, octaves (log2 of the delay): its spread, plus the stereo skew on R.
[[nodiscard]] inline double tapDelayOffsetLog2(bool barberpole, int channel, int tap,
                                               const phase_space::Explore& e) noexcept {
	if (!tapReadsExplore(barberpole, tap)) {
		return 0.0;
	}
	const double skew = channel == 1 ? e.stereoOctaves : 0.0;
	return e.spreadOctaves[tapExploreIndex(barberpole, channel, tap)] + skew;
}
/// The tap's LFO phase offset, cycles.
[[nodiscard]] inline double tapPhaseOffset(bool barberpole, int channel, int tap, const phase_space::Explore& e) noexcept {
	return tapReadsExplore(barberpole, tap) ? e.lfoPhase[tapExploreIndex(barberpole, channel, tap)] : 0.0;
}
/// **EVERY TAP'S TWO OFFSETS ON ONE CHANNEL, AS THE ENGINE RAMPS THEM** — in float, the ramps' own type, so a reader
/// that has no ramps (the response model) holds the values the engine's ramps land on.
struct TapOffsets {
	std::array<float, kBarberVoices> log2 {};  ///< `tapDelayOffsetLog2`, tap by tap
	std::array<float, kBarberVoices> phase {}; ///< `tapPhaseOffset`
};
[[nodiscard]] inline TapOffsets tapOffsetsOf(bool barberpole, int channel, const phase_space::Explore& e) noexcept {
	TapOffsets out;
	for (size_t k = 0; k < out.log2.size(); ++k) {
		const int tap = static_cast<int>(k);
		out.log2[k] = static_cast<float>(tapDelayOffsetLog2(barberpole, channel, tap, e));
		out.phase[k] = static_cast<float>(tapPhaseOffset(barberpole, channel, tap, e));
	}
	return out;
}

/// How far a full sweep moves the flanger's delay, in octaves either side of Delay.
inline constexpr double kFlangerOctaves = 2.0;
/// The longest delay the flanger's sweep may reach.
inline constexpr double kMaxDelayMs = 20.0;
/// **THROUGH-ZERO'S DRY DELAY** — fixed, so the latency it costs is a constant the host can be told.
inline constexpr double kTzDryMs = 8.0;
/// The shortest read a sinc line allows: its window reaches six samples to the newer side (`sinc_delay_line.h`).
inline constexpr int kMinDelaySamples = dsp::SincTable::kHalfTaps;
/// **THE FLANGER'S KERNEL BANDWIDTH: 1.0, A DELTA AT EVERY WHOLE-SAMPLE READ** (`SincTable::tapAt`) — so the
/// through-zero crossing, where the wet reads at the dry's whole-sample delay, is the plain sample, and the
/// dry can be a plain ring buffer with no kernel to darken it. The comb's 0.455 is a voiced lowpass.
inline constexpr double kFlangerBandwidth = 1.0;
/// THE WET LINE, sized at 192 kHz: 20 ms of sweep plus the 8 ms the through-zero dry sits behind is 5376
/// samples, and a read needs kMinDelay to spare at the far end. Above 192 kHz the delays clamp to the line
/// (`flangerDelaySamples`), as the automod comb's do past its `kCombMaxSampleRate`.
inline constexpr int kLineSize = 8192;
static_assert(static_cast<int>((kMaxDelayMs + kTzDryMs) * 192.0) + kMinDelaySamples <= kLineSize - kMinDelaySamples);
/// THE THROUGH-ZERO DRY RING: 8 ms at 384 kHz is 3072 samples, so the dry delay is a true 8 ms at every rate a
/// host offers. Above 512 kHz it clamps to the ring, and the reported latency with it (`tzDrySamples`). The wet's
/// through-zero read is at most twice the dry (`tzWetSamples`), 6144 at 384 kHz, inside the wet line.
inline constexpr int kDryLineSize = 4096;
static_assert(static_cast<int>(kTzDryMs * 384.0) <= kDryLineSize);
static_assert(2 * static_cast<int>(kTzDryMs * 384.0) <= kLineSize - kMinDelaySamples);
/// The longest read either line allows.
inline constexpr double kMaxWetReadSamples = static_cast<double>(kLineSize - kMinDelaySamples);

/// THE FLANGER'S DELAY at sweep `s`, in samples: `Delay · 2^(2s)`, clamped to [kMinDelay, 20 ms] and to the line.
[[nodiscard]] inline double flangerDelaySamples(double delayLog2s, double s, double sampleRate) noexcept {
	const double samples = std::exp2(delayLog2s + s * kFlangerOctaves) * sampleRate;
	const double top = std::min(kMaxDelayMs * 0.001 * sampleRate, kMaxWetReadSamples);
	return std::clamp(samples, static_cast<double>(kMinDelaySamples), std::max(top, static_cast<double>(kMinDelaySamples)));
}
/// **THROUGH-ZERO'S DRY DELAY, in whole samples** — the latency the device reports, exactly: 8 ms, held to the
/// ring's length (it bites above 512 kHz, and the report bites with it), and 0 for a rate that is no rate.
[[nodiscard]] inline int tzDrySamples(double sampleRate) noexcept {
	if (!(sampleRate > 0.0)) {
		return 0;
	}
	return static_cast<int>(std::min<long>(std::lround(kTzDryMs * 0.001 * sampleRate), kDryLineSize));
}
/// Through-zero's excursion either side of the dry, samples: Delay, held to the dry less kMinDelay, so s = −1 still
/// reads a legal delay. Split out so a caller reading several taps at one Delay pays its exp2 once (`tzWetOfSpan`).
[[nodiscard]] inline double tzSpanSamples(double delayLog2s, double drySamples, double sampleRate) noexcept {
	return std::max(0.0, std::min(std::exp2(delayLog2s) * sampleRate, drySamples - static_cast<double>(kMinDelaySamples)));
}
/// Through-zero's wet read from the dry and the span: `dry + s·span`, never under kMinDelay.
[[nodiscard]] inline double tzWetOfSpan(double drySamples, double span, double s) noexcept {
	return std::max(drySamples + std::clamp(s, -1.0, 1.0) * span, static_cast<double>(kMinDelaySamples));
}
/// **THROUGH-ZERO'S WET DELAY**: the dry's, plus `s · Delay` — so the RELATIVE delay is s·Delay and crosses
/// zero at s = 0. LINEAR in s, not octaves: the octave sweep never reaches zero, which is the whole point here.
[[nodiscard]] inline double tzWetSamples(double delayLog2s, double s, double sampleRate) noexcept {
	const double dry = static_cast<double>(tzDrySamples(sampleRate));
	return tzWetOfSpan(dry, tzSpanSamples(delayLog2s, dry, sampleRate), s);
}
/// **THE LATENCY THE DEVICE REPORTS**: through-zero's dry delay whenever the Flanger runs, through-zero ON OR OFF —
/// the plain flanger's whole output is held back by the same amount (`FlangerChannel::late`), so flipping Through
/// Zero never moves the latency. A latency that moves costs a host restart, and a restart cost every held note
/// (switching the TZ switch also kills sound temporarily). The Phaser reports none; a Mode
/// flip still moves it. Mode is a stored switch and not modulatable (registry.h, `latencySamples`, assumption L1).
[[nodiscard]] inline uint32_t latencySamplesFor(Mode mode, double sampleRate) noexcept {
	return mode == Mode::Flanger ? static_cast<uint32_t>(tzDrySamples(sampleRate)) : 0u;
}

/// THE FEEDBACK THE LOOP ACTUALLY USES — the stored value under the ceiling, which opens linearly from 0.995
/// at Drive 0 to 1 at Drive `kCeilingOpenDrive` (see `kMaxLinearFeedback`).
[[nodiscard]] inline double effectiveFeedback(double feedback, double drive) noexcept {
	const double open = std::clamp(drive / kCeilingOpenDrive, 0.0, 1.0);
	const double limit = kMaxLinearFeedback + (1.0 - kMaxLinearFeedback) * open;
	return std::clamp(feedback, -limit, limit);
}

/// **DAMPING IS OUT OF THE LOOP EXACTLY AT ITS STORED TOP.** Compared IN FLOAT, because that is what a row
/// stores: float(kDampingLog2Max) rounds BELOW the double, so a double comparison would never see the top
/// of the knob as the top. The readout ("open") asks this same question.
[[nodiscard]] inline bool dampingOpen(double dampingLog2Hz) noexcept {
	return static_cast<float>(dampingLog2Hz) >= static_cast<float>(kDampingLog2Max);
}
/// The damping lowpass's corner below the top, held under 0.45·sr (20 kHz is past that at 44.1 kHz).
[[nodiscard]] inline double dampingCornerHz(double dampingLog2Hz, double sampleRate) noexcept {
	return std::min(std::exp2(dampingLog2Hz), kCornerMaxFraction * sampleRate);
}
/// **THE DAMPING CORNER WITH THE EXPLORE LAYER'S COLOUR** (index 27, ±2 octaves): the knob's corner moved by
/// `colourOctaves`, floored at `kCornerMinHz` and held under 0.45·sr as the plain one is. The caller asks it only
/// while `dampingOpen` is false — **AN OPEN LOOP STAYS OPEN**: the colour moves a lowpass that is running, and never
/// puts one into a loop the knob took it out of (the knob's top means "no filter", and a pad gesture making it mean
/// "a filter at 5 kHz" would be the pad overriding a literal control). Nor does a colour above the top open it.
///
/// **THE COLOUR FADES OUT OVER THE KNOB'S TOP OCTAVE** (`kDampingColourFadeOctaves`), scaled by
/// clamp((top − knob)/1 oct, 0, 1), so at the top the coloured corner IS the plain one and the lowpass leaves the loop
/// from where the plain knob would have it, near 20 kHz — without the fade, a −2 oct colour would have the lowpass at
/// 5 kHz one tick and out of the loop the next: a step in the loop's tone at the exit. `top` is the STORED top (the
/// float a row holds, `dampingOpen`'s comparison), so the fade is exactly 0 where the loop opens.
///
/// At colour 0 this is `dampingCornerHz` bit for bit: 0·fade is +0.0 and adding it is exact (and the floor cannot
/// bite: the knob's bottom is 500 Hz).
inline constexpr double kDampingColourFadeOctaves = 1.0;
[[nodiscard]] inline double colouredDampingCornerHz(double dampingLog2Hz, double colourOctaves,
                                                    double sampleRate) noexcept {
	const double top = static_cast<double>(static_cast<float>(kDampingLog2Max));
	const double fade = std::clamp((top - dampingLog2Hz) / kDampingColourFadeOctaves, 0.0, 1.0);
	return dampingCornerHz(std::max(dampingLog2Hz + colourOctaves * fade, std::log2(kCornerMinHz)), sampleRate);
}

/// **ONE PHASER STAGE: A FIRST-ORDER TPT ALLPASS, `ap = 2·lp − x`.** The lowpass is `dsp::TptOnePole`'s
/// (Zavalishin §3.10), H_lp(z) = g(z + 1) / ((1 + g)z + (g − 1)) with g = tan(π·fc/sr). Then
///
///     H_ap = 2·H_lp − 1 = (2g(z + 1) − (1 + g)z − (g − 1)) / ((1 + g)z + (g − 1))
///                       = ((g − 1)z + (g + 1)) / ((g + 1)z + (g − 1)),
///
/// and on the unit circle the numerator is the denominator's conjugate times z, so |H_ap| = 1 at every
/// frequency: +1 at DC, −1 at Nyquist, −90° at fc. Its phase is the analog prototype's
/// −2·atan(ω/ωc) through the prewarp: φ(f) = −2·atan(tan(πf/sr) / g).
///
/// Of g directly, and of the frequency as a fraction of the rate — the form the engine's own
/// coefficients are read back in.
[[nodiscard]] inline std::complex<double> stageResponseOfG(double g, double fOverSr) noexcept {
	const std::complex<double> z = std::polar(1.0, 2.0 * std::numbers::pi * fOverSr);
	return ((g - 1.0) * z + (g + 1.0)) / ((g + 1.0) * z + (g - 1.0));
}
/// The same stage by its corner (exact tan, not the engine's `fasttan`: 3e-7 apart, under any plot's pixel).
[[nodiscard]] inline std::complex<double> stageResponse(double fc, double sampleRate, double f) noexcept {
	return stageResponseOfG(std::tan(std::numbers::pi * fc / sampleRate), f / sampleRate);
}

/// **A TPT ONE-POLE'S GAIN G = g/(1 + g) AT CORNER `fc`, AS THE ENGINE HOLDS IT** — g = tan(π·fc/sr) through `fasttan`,
/// in float. Every stage's corner and the damping lowpass's are turned into G by this one statement, so the response
/// model (`phaser_response.h`) reads the very float the engine runs. g comes back as G/(1 − G).
[[nodiscard]] inline float tptGainOf(double fc, double sampleRate) noexcept {
	const float g = dsp::fasttan(std::numbers::pi_v<float> * static_cast<float>(fc / sampleRate));
	return g / (1.0f + g);
}
/// **THE LOOP DC BLOCKER'S LOWPASS GAIN** at `kLoopDcCornerHz` — exact tan (it is paid once, at prepare).
[[nodiscard]] inline float loopDcGainOf(double sampleRate) noexcept {
	const double g = std::tan(std::numbers::pi * kLoopDcCornerHz / sampleRate);
	return static_cast<float>(g / (1.0 + g));
}

/// **CHANNEL `channel`'s LFO PHASE** from the left's: the right reads Stereo/360 of a cycle ahead, wrapped.
[[nodiscard]] inline double channelLfoPhase(double leftPhase, double stereoDegrees, int channel) noexcept {
	if (channel != 1) {
		return leftPhase;
	}
	const double ahead = leftPhase + stereoDegrees / 360.0;
	return ahead - std::floor(ahead);
}
/// **CHANNEL `channel`'s BARBERPOLE RAMP PHASE ψ** from the left's: the right's is Stereo/360 on, wrapped.
[[nodiscard]] inline double channelBarberPhase(double leftPsi, double stereoDegrees, int channel) noexcept {
	return channel != 1 ? leftPsi : barberVoicePhase(leftPsi + stereoDegrees / 360.0, 0);
}

//--------------------------------------------------------------------------------------------------//
// KEY TRACKING — the note sets the centre the sweep moves around.                                   //
//--------------------------------------------------------------------------------------------------//

/// **THE NOTE'S OWN FREQUENCY** (the `NoteHz` reading): A4 = 69, glide and bend already folded in by the host —
/// 440·2^((pitch − 69)/12). The knob's offset is applied after (`trackedHzOf`), so the reading is the NOTE.
///
/// **THE PITCH IS CLAMPED TO [−60, 200] SEMITONES** (`kKeyPitchMin`, `kKeyPitchMax`): 0.25 Hz to ~850 kHz, so the
/// frequency is always finite and positive — a host handing over a pitch of 1e4 would otherwise make it inf, and −1e4
/// would make it 0, and neither is a frequency a centre can be computed from. Both ends are far outside anything the
/// corner and delay clamps let through, so in-range notes are untouched; and the knob's offset (at most about ±52
/// semitones either way) cannot take a clamped note to an infinity or a zero.
inline constexpr double kKeyPitchMin = -60.0;
inline constexpr double kKeyPitchMax = 200.0;
[[nodiscard]] inline double keyHzOf(double pitchSemitones) noexcept {
	const double p = std::isfinite(pitchSemitones) ? std::clamp(pitchSemitones, kKeyPitchMin, kKeyPitchMax) : 69.0;
	return 440.0 * std::exp2((p - 69.0) / 12.0);
}

/// **THE CENTRE KNOB AS AN OFFSET FROM THE NOTE**, semitones — its distance from its default in the mode's sense:
/// the phaser's 12·log2(Center / 800 Hz), the flanger's −12·log2(Delay / 2.5 ms) (a longer delay is a lower note).
/// `knobLog2` is the stored value, log2(Hz) or log2(s).
///
/// **THE REFERENCE AS A ROW STORES IT** — the knob is a float in the row, so "+0" is the float its default rounds to,
/// and the default lands on the note EXACTLY (the double default would leave ~1e-6 st, ~6e-8 of the frequency).
[[nodiscard]] inline double keyOffsetSemitonesOf(Mode mode, double knobLog2) noexcept {
	constexpr double kCenterRef = static_cast<double>(static_cast<float>(kCenterKeyRefLog2));
	constexpr double kDelayRef = static_cast<double>(static_cast<float>(kDelayKeyRefLog2));
	return mode == Mode::Phaser ? 12.0 * (knobLog2 - kCenterRef) : -12.0 * (knobLog2 - kDelayRef);
}

/// THE FREQUENCY THE CENTRE IS PLACED ON while a note is tracked: the note moved by the knob's offset,
/// noteHz·2^(offset/12).
[[nodiscard]] inline double trackedHzOf(Mode mode, double knobLog2, double noteHz) noexcept {
	return noteHz * std::exp2(keyOffsetSemitonesOf(mode, knobLog2) / 12.0);
}

/// **THE PHASER'S CENTRE THAT PUTS THE FIRST NOTCH EXACTLY ON `hz`**, log2(Hz), for N equal stages.
///
/// The plan's statement is the analog prototype's, `Center = f / tan(π/(2N))`: each stage turns ω through
/// 2·atan(ω/ωc), and the dry sum nulls where N of them make π. Through the bilinear transform that is only
/// approximately where the DIGITAL notch lands — the prewarp maps ω to tan(πf/sr), not to f. It is exact at N = 2
/// (tan(π/4) = 1) and drifts with N and pitch: COMPUTED at 48 kHz, its response at the note is −42 dB for N = 6 at
/// note 72, −29 dB for N = 12 at note 72, −17 dB for N = 12 at note 84 (`test_fx_phaser`'s notch case asserts the
/// N = 12, note 72 miss).
/// So this solves the digital condition instead: a stage at prewarped corner g turns f through
/// −2·atan(tan(πf/sr)/g) (`stageResponseOfG`), and the first null is where that is −π/N, so
///
///     g = tan(π·f/sr) / tan(π/(2N)),   fc = (sr/π)·atan(g),
///
/// which the engine's `aimStages` maps straight back to g = tan(π·fc/sr) — the notch EXACTLY on the note, to the
/// engine's `fasttan` and float G. Clamped to the corner's own limits [20 Hz, 0.45·sr]; a note too high for
/// its notch to fit under them gets the top, and its notch lands above the note.
[[nodiscard]] inline double keyCornerLog2Hz(double hz, int stages, double sampleRate) noexcept {
	const double top = kCornerMaxFraction * sampleRate;
	const int n = std::max(stages, 1);
	// Under Nyquist with room: tan is a pole at a half, and a note up there has no notch to place anyway.
	const double f = std::clamp(hz, 0.0, 0.49 * sampleRate);
	const double g = std::tan(std::numbers::pi * f / sampleRate) / std::tan(std::numbers::pi / (2.0 * n));
	const double fc = sampleRate / std::numbers::pi * std::atan(g);
	return std::log2(std::clamp(std::isfinite(fc) ? fc : top, kCornerMinHz, top));
}

/// **THE FLANGER'S DELAY THAT TUNES ITS COMB TO `hz`**, log2(s): one period, 1/f — the comb's teeth (and, with
/// feedback, its resonance) at f and its harmonics. Clamped to the ENGINE's delay range, [kMinDelay samples,
/// 20 ms], not the knob's 0.2..15 ms: C2 (65.4 Hz) is 15.3 ms, and a tracked low note should be in tune rather
/// than clamped by a knob it is not using. Under THROUGH-ZERO this is the SPAN — the relative delay at the sweep's
/// extremes — which `tzSpanSamples` holds under the dry delay less kMinDelay (7.875 ms at 48 kHz, so notes
/// under about 127 Hz there clamp).
[[nodiscard]] inline double keyDelayLog2s(double hz, double sampleRate) noexcept {
	const double lo = std::log2(static_cast<double>(kMinDelaySamples) / sampleRate);
	const double hi = std::log2(kMaxDelayMs * 0.001);
	const double d = -std::log2(hz);
	return std::isfinite(d) ? std::clamp(d, lo, std::max(lo, hi)) : hi;
}

/// **THE CENTRE'S TARGET — THE KNOB, OR THE NOTE MOVED BY IT** (step one of `channelCentreLog2`'s two): with a
/// tracked note (`tracking`: Key Track on and a note heard), the centre of the note moved by the knob's offset
/// (`trackedHzOf`) — `keyCornerLog2Hz`, its first notch there, or `keyDelayLog2s`, its comb tuned there — else the
/// knob's own log2. log2(Hz) for the phaser, log2(s) for the flanger. `noteHz` is the note's own frequency.
[[nodiscard]] inline double centreTargetLog2(Mode mode, double knobLog2, bool tracking, double noteHz, int stages,
                                             double sampleRate) noexcept {
	if (!tracking) {
		return knobLog2;
	}
	const double hz = trackedHzOf(mode, knobLog2, noteHz);
	return mode == Mode::Phaser ? keyCornerLog2Hz(hz, stages, sampleRate) : keyDelayLog2s(hz, sampleRate);
}

//--------------------------------------------------------------------------------------------------//
// WHAT THE GLIDES AIM AT — one statement for the engine's tick and the response model's landed values. //
//--------------------------------------------------------------------------------------------------//

/// **EVERY GLIDE'S TARGET, AND THE STEPPED VALUES BESIDE THEM**, from the stored settings and the tracked note
/// (`noteHz`: the note's own frequency — Center or Delay is its offset — or 0 for none). The engine glides toward these; the response model takes
/// them as landed (the glides land exactly, `Phaser::kGlideSnap`). Drive, Feedback and Mix are the RAW stored values:
/// their clamps act on the glided value (`loopDriveOf`, `loopFeedbackOf`, `loopMixOf`).
struct Targets {
	int stages = 6;            ///< the live stage count, 1..kMaxStages
	bool throughZero = false;  ///< the stored switch in the Flanger, false in the Phaser (it acts only in the flanger)
	bool tracking = false;     ///< Key Track on AND a note to track
	double centerLog2 = 0.0;   ///< the phaser's centre, log2(Hz): the note's or the knob's (`centreTargetLog2`)
	double delayLog2 = 0.0;    ///< the flanger's, log2(s)
	double position = 0.0;
	double feedback = 0.0;
	double drive = 0.0;
	double mix = 0.0;
	double dampingLog2 = 0.0;
	double rateLog2 = 0.0;
	double stereoDegrees = 0.0;
	double lfoAmount = 0.0;    ///< LFO Depth, or 0 with the LFO off
	double envAmount = 0.0;    ///< Env Amount, or 0 with the follower off
	double envSensDb = 0.0;
	double envAttackLog2 = 0.0;
	double envReleaseLog2 = 0.0;
};
[[nodiscard]] inline Targets targetsOf(const Settings& t, double noteHz, double sampleRate) noexcept {
	Targets out;
	out.stages = std::clamp(t.stages, 1, kMaxStages);
	// THROUGH-ZERO IS THE FLANGER'S: the stored switch in the flanger, false in the phaser — so the engine and the model
	// read one fact, and the phaser's DelayMs readings are not offset by a dry delay its audio never takes.
	out.throughZero = t.mode == Mode::Flanger && t.throughZero;
	out.tracking = t.keyTrack && noteHz > 0.0;
	out.centerLog2 = centreTargetLog2(Mode::Phaser, static_cast<double>(t.centerLog2Hz), out.tracking, noteHz,
	                                  out.stages, sampleRate);
	out.delayLog2 = centreTargetLog2(Mode::Flanger, static_cast<double>(t.delayLog2s), out.tracking, noteHz,
	                                 out.stages, sampleRate);
	out.position = static_cast<double>(t.position);
	out.feedback = static_cast<double>(t.feedback);
	out.drive = static_cast<double>(t.drive);
	out.mix = static_cast<double>(t.mix);
	out.dampingLog2 = static_cast<double>(t.dampingLog2Hz);
	out.rateLog2 = static_cast<double>(t.rateLog2Hz);
	out.stereoDegrees = static_cast<double>(t.stereoDegrees);
	out.lfoAmount = t.lfoOn ? static_cast<double>(t.lfoDepth) : 0.0;
	out.envAmount = t.envOn ? static_cast<double>(t.envAmount) : 0.0;
	out.envSensDb = static_cast<double>(t.envSensDb);
	out.envAttackLog2 = static_cast<double>(t.envAttackLog2s);
	out.envReleaseLog2 = static_cast<double>(t.envReleaseLog2s);
	return out;
}
/// THE LOOP'S DRIVE as its ramp holds it, from the glided raw value.
[[nodiscard]] inline float loopDriveOf(double drive) noexcept {
	return static_cast<float>(std::clamp(drive, 0.0, 1.0));
}
/// THE LOOP'S FEEDBACK as its ramp holds it: the glided raw value under the ceiling the ramp's Drive opens.
[[nodiscard]] inline float loopFeedbackOf(double feedback, float drive) noexcept {
	return static_cast<float>(effectiveFeedback(feedback, static_cast<double>(drive)));
}
/// THE MIX as its ramp holds it.
[[nodiscard]] inline float loopMixOf(double mix) noexcept { return static_cast<float>(std::clamp(mix, 0.0, 1.0)); }

//--------------------------------------------------------------------------------------------------//
// ONE CHANNEL'S SWEEP AT ONE TICK — the tick's whole sweep arithmetic, as a pure function, so the     //
// engine (`Phaser::tick`) and the response model (`phaser_response.h`) run the same statement.        //
//--------------------------------------------------------------------------------------------------//

/// **ONE VOICE OF ONE CHANNEL, AS A TICK COMPUTES IT.** Sweep = Normal is voice 0 alone, with ψ 0 and window 1.
struct VoiceSweep {
	double psi = 0.0;      ///< its ramp phase ψ_k (Normal: 0)
	double s = 0.0;        ///< its sweep point
	double w = 0.0;        ///< its window (Normal: voice 0 is 1, the others 0)
	double cornerHz = 0.0; ///< the phaser's corner at s (each stage's before the explore spread)
	double delay = 0.0;    ///< the flanger's wet read at s, samples (with the explore layer's offsets)
	/// EACH STAGE'S OWN SWEEP — the explore layer's per-stage LFO offsets (on φ in Normal, on the sweep VALUE
	/// under the barberpole); every entry is `s` at home.
	std::array<double, kMaxStages> stageS {};
};

/// WHAT A CHANNEL'S SWEEP IS COMPUTED FROM — every value the GLIDED one (the engine's) or the landed one (the model's).
struct SweepInputs {
	bool flanger = false;
	bool barberpole = false;
	bool throughZero = false;
	double position = 0.0;
	double lfoAmount = 0.0;  ///< LfoDepth, or 0 with the LFO off
	double envTerm = 0.0;    ///< EnvAmount · env, or 0 with the follower off
	double lfoPhase = 0.0;   ///< this channel's φ (`channelLfoPhase`)
	double psi = 0.0;        ///< this channel's ψ (`channelBarberPhase`); unread in Normal
	double centreLog2 = 0.0; ///< the phaser's channel centre (`channelCentreLog2`)
	double delayLog2 = 0.0;  ///< the flanger's centre delay
	double squareNorm = 1.0; ///< `squareNormOf(explore->bend)`
	double sampleRate = 48000.0;
	TapOffsets taps {};      ///< `tapOffsetsOf` — the engine passes its ramps' targets, which are those values
};

/// **THE CHANNEL'S VOICES, AND WHAT THE READINGS REPORT OF THEM.**
struct ChannelSweep {
	std::array<VoiceSweep, kBarberVoices> voices {};
	int count = 1;         ///< 1 in Normal, `kBarberVoices` under the barberpole
	double sweep = 0.0;    ///< Normal: s. Barberpole: Σ w_k·s_k
	double cornerHz = 0.0; ///< Normal: fc. Barberpole: 2^(Σ w_k·log2 fc_k)
	double delay = 0.0;    ///< Normal: d. Barberpole: Σ w_k·d_k
};

/// **ONE CHANNEL'S SWEEP AT ONE TICK** — every voice's ramp phase, sweep, window, corner, flanger read and per-stage
/// sweeps. The flanger's tap offsets: Normal's channel reads its LFO at φ + its tap-0 offset and its delay moved by
/// tap 0's spread (and R's skew); the barberpole's tap k moves its sweep by its LFO offset AS A SWEEP VALUE
/// (`offsetSweepOf`, never on ψ) and its delay by its spread. The phaser's stages read `stageSweepsOf`. `e` is the
/// explore layer's derived set (the engine's glided one; the model's landed `exploreOf`).
inline void channelSweepOf(const SweepInputs& in, const phase_space::Explore& e, ChannelSweep& out) noexcept {
	const phase_space::LfoBend& bend = e.bend;
	const double phase = in.lfoPhase;
	const double sr = in.sampleRate;
	// THE SWEEP CURVE (index 33) on every primary sweep: 1 at home, where `sweepOf` does not evaluate it.
	const double curveExp = sweepCurveExponentOf(e.sweepCurve);
	// THE PHASER'S PER-STAGE LFO SHAPES, read once a channel (`stageShapesOf`: none at home). The flanger has no
	// stages, so it reads none.
	const StageShapes shapes = in.flanger ? StageShapes {} : stageShapesOf(phase, e, in.squareNorm);
	const auto delayAt = [&](double dl, double s) {
		return in.throughZero ? tzWetSamples(dl, s, sr) : flangerDelaySamples(dl, s, sr);
	};
	const auto stagesOf = [&](VoiceSweep& v, bool barber) {
		if (in.flanger) {
			v.stageS.fill(v.s);
		}
		else {
			stageSweepsOf(v.stageS, barber, v.s, in.position, in.lfoAmount, in.envTerm, e, shapes, curveExp);
		}
	};
	if (!in.barberpole) {
		// THE FLANGER'S CHANNEL READS ITS LFO AT φ + its tap-0 offset (the value the per-sample ramp lands on).
		const double chOff = in.flanger ? static_cast<double>(in.taps.phase[0]) : 0.0;
		VoiceSweep& v = out.voices[0];
		v.psi = 0.0;
		v.w = 1.0;
		v.s = sweepOf(in.position, in.lfoAmount * lfoShape(phase + chOff, bend), in.envTerm, curveExp);
		v.cornerHz = phaserCornerHz(in.centreLog2, v.s, sr);
		stagesOf(v, false);
		v.delay = delayAt(in.delayLog2 + static_cast<double>(in.taps.log2[0]), v.s);
		out.count = 1;
		out.sweep = v.s;
		out.cornerHz = v.cornerHz;
		out.delay = v.delay;
		return;
	}
	// THE BARBERPOLE: the readings are the voices' window-weighted centre (the corner averaged in log).
	double weighted = 0.0;
	double logCorner = 0.0;
	double delay = 0.0;
	for (size_t k = 0; k < static_cast<size_t>(kBarberVoices); ++k) {
		VoiceSweep& v = out.voices[k];
		v.psi = barberVoicePhase(in.psi, static_cast<int>(k));
		v.w = barberGain(v.psi);
		v.s = barberSweepOf(in.position, in.lfoAmount, v.psi, in.envTerm, curveExp);
		v.cornerHz = phaserCornerHz(in.centreLog2, v.s, sr);
		const double tapOff = static_cast<double>(in.taps.phase[k]);
		// THE TAP'S OFFSET IS MEASURED FROM lfoShape(φ) — `stageShapesOf`'s home, the same expression.
		const double sTap = tapOff == 0.0 ? v.s
		                                  : offsetSweepOf(v.s, in.lfoAmount, lfoShape(phase + tapOff, bend),
		                                                  lfoShapeWith(phase, bend, in.squareNorm));
		v.delay = delayAt(in.delayLog2 + static_cast<double>(in.taps.log2[k]), sTap);
		stagesOf(v, true);
		weighted += v.w * v.s;
		logCorner += v.w * std::log2(v.cornerHz);
		delay += v.w * v.delay;
	}
	out.count = kBarberVoices;
	out.sweep = weighted;
	out.cornerHz = std::exp2(logCorner);
	out.delay = delay;
}

extern const device::AudioOps kAudioOps;

/// THE DEVICE'S OWN READOUTS: Delay, Env Attack and Env Release in milliseconds down to their bottoms
/// (never "off"), and Damping's top as "open". Every other control prints by its declared unit. (Center and Delay
/// print in semitones while a note is tracked, but a readout cannot know that; the card and window do it,
/// `phaserOffsetText`.)
extern const device::TextOps kTextOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.text = &kTextOps,
	.category = device::Category::ModulationFx,
};

} // namespace feathers::fx::phaser
