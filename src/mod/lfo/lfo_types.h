/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/registry.h"
#include "mod/modulator.h"
#include "mod/trig.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace feathers::mod::lfo {

/// THE WAVEFORM. Three base shapes plus two random walks; the skew knobs deform the periodic ones
/// into anything between and beyond. Both randoms are deterministic per song-phase.
///
/// **WHY THERE ARE TWO RANDOMS AND NOT ONE RANDOM WITH A SMOOTHING KNOB** (decided 2026-09-19,
/// on the numbers below). The Smoothing control is a one-pole with a time constant in seconds, and a
/// one-pole on a STAIRCASE cannot give a smooth random: it settles early in each step and then the
/// output sits, so what you hear is a corner followed by a plateau — smooth and steppy at the same
/// time. MEASURED on the engine, free-running, seed 7, one second of ticks: at 2 Hz with
/// tau = 125 ms, **80% of ticks are flat** and the remaining 20% is the glide. And winding tau up far
/// enough to remove the plateau costs DEPTH, because the pole stops reaching its targets: at 8 Hz the
/// output span runs 0.82 (tau 31 ms) → 0.60 (125 ms) → **0.32 (500 ms)**. There is no setting of that
/// knob which is both smooth and full, so no amount of tuning it was going to be the fix.
///
/// `RandomGlide` INTERPOLATES BETWEEN THE STEP VALUES INSTEAD, so it is always moving and always
/// arrives — full depth at every rate, and no plateau to be steppy about. The stepped `Random` is
/// untouched and still has the corner-rounder on top. This is the conventional S&H / smooth-random
/// pair, and the reason it is a WAVEFORM rather than a mode of the Smoothing knob is the complaint
/// that started the day: a control that means a time on four waves and a proportion on a fifth is the
/// same confusion the `Smoothing On` switch was deleted for.
///
/// **APPENDED AFTER `Random`, so no stored index changes its meaning.** A Choice table that grows at
/// the end leaves every existing value reading what it read.
enum class Waveform : uint8_t {
	Sine,
	Triangle,
	Square,
	Random,
	RandomGlide,
	kCount,
};

inline constexpr const char* kWaveformNames[] = { "Sine", "Triangle", "Square", "Random",
	                                              "Random Glide" };

/// WHICH WAVEFORMS A GATED CONTROL BELONGS TO, as a mask over `Waveform` — `ParamDesc::gateMask`'s
/// shape. Named rather than written as bits at each use, so "the shaped waves" is one fact and a
/// fifth waveform has one place to be added to.
inline constexpr uint32_t kWaveBit(Waveform w) {
	return 1u << static_cast<uint32_t>(w);
}
/// The three that are a function of phase, and so the three the skew weights can deform.
inline constexpr uint32_t kShapedWaves =
    kWaveBit(Waveform::Sine) | kWaveBit(Waveform::Triangle) | kWaveBit(Waveform::Square);
/// The two that are a sequence rather than a shape, and so the only ones a seed means anything to.
/// BOTH, or `RandomGlide` would be a walk whose seed the card refuses to show — the honesty rule in
/// the direction that hides a live control rather than offering a dead one.
inline constexpr uint32_t kRandomWave =
    kWaveBit(Waveform::Random) | kWaveBit(Waveform::RandomGlide);
static_assert(std::size(kWaveformNames) == static_cast<size_t>(Waveform::kCount));

/// ORDER IS THE STREAM ORDER AND THE DESCRIPTOR ORDER AND THE ROW ORDER, all three.
enum class Param : int {
	//-- The globals.
	Waveform,       ///< sine, triangle, square, random, random glide
	SkewPrePeak,    ///< time weight of 0°–90° (before the sine peak) — periodic modes only
	SkewPreCross,   ///< how much of the cycle is the first half — periodic modes only
	SkewPreTrough,  ///< where the trough sits inside the SECOND HALF — periodic modes only
	Seed,           ///< which random walk plays — Random only, and gated on the waveform
	TimeBase,       ///< which musical division the rate is — or Note for audio rate
	TimeScale,      ///< how many of that division the window is; seconds when there is no tempo
	Smoothing,      ///< how much the output is smoothed; at the bottom of its travel it is OFF
	Mode,           ///< how the shape runs — see Mode in modulator.h
	Phase,          ///< where in the shape it starts, 0..1
	PingPong,       ///< reverses at loop boundaries (for segment-based modes)
	Bipolar,        ///< swings -1..1 instead of 0..1
	Enabled,        ///< bypass switch
	Amount,         ///< host-applied depth
	/// **POLY — one wave per note, or one for the device**. See `dahdsr::Param::Poly`. OFF BY
	/// DEFAULT: an LFO is classically one wave the whole patch breathes with.
	Poly,
	/// **SMOOTH THROUGH RETRIGGER** (backlog MM) — see `mod::PolyShape::processNotes`.
	SmoothCarry,
	/// **TRIG — RESETS THE PHASE** (backlog MV): fired at 0.5, re-armed below 0.25 (`mod/trig.h`). Appended; keyed.
	Trig,
	/// **NOTES FIRE TRIG** (backlog NU): the note-on normal into Trig, and the switch that breaks it — `mod::notesTrigParamDesc`.
	NotesTrig,
	/// **STEREO PHASE — THE RIGHT EAR READS THE WAVE THIS FAR AHEAD** (backlog PA, 2026-10-29: a stereo phase OFFSET,
	/// 0–180°). Radians, read in degrees. At 0 the output is mono and is today's LFO to the bit; otherwise it is an ear
	/// pair (`SourceDesc::pairWhile`), the right ear the same deterministic shape at phase + offset — the random walks
	/// included, read that far along the SAME sequence, so there is no second draw and nothing new to persist.
	StereoPhase,

	kCount,
};

using Mode = mod::Mode;
inline constexpr auto& kOffOnNames = mod::kOffOnNames;
inline constexpr auto& kModeNames = mod::kModeNames;

/// TOTAL_SKEW is the sum that normalizes against. Three knobs are user-facing;
/// the fourth quadrant (270°–360°) is implicit and computed from these three.
inline constexpr float kSkewCount = 3.0f;

struct Settings {
	/// WHICH CONTROL IS THE TRIG, for `mod::PolyShape`'s per-note resolve (backlog MV, B).
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);

	Waveform wave = Waveform::Triangle;
	/// THE THREE SKEW KNOBS. Each 0..100%; normalized at eval time so the period holds.
	/// THE THREE SKEWS, each a FRACTION with 0.5 neutral — see `SkewMap` for how they nest, and the
	/// descriptors for why 0..1 rather than the 0..100 that printed 5000%.
	float skewPrePeak = 0.5f;
	float skewPreCross = 0.5f;
	float skewPreTrough = 0.5f;
	/// RANDOM SEED. Integer 0..999; which walk plays. Only meaningful in Random mode, and the
	/// descriptor says so with a gate rather than only in prose.
	int seed = 0;

	size_t timeBase = 0;
	float timeScale = 0.0f;
	/// AT THE BOTTOM OF ITS TRAVEL IT IS OFF — the `smoothingOn` flag that used to gate it is deleted
	/// And the bottom is `kSmoothingLog2SecondsMin`, not the `-10.0f` literal
	/// `tick()` used to test against: that was one octave up, so the lowest octave of this knob did
	/// nothing while the box printed a time. See `mod::smoothingCoeff`.
	float smoothing = kSmoothingLog2SecondsMin;

	Mode mode = Mode::Gate;
	float phase = 0.0f;
	bool pingPong = false;
	bool bipolar = false;
	bool enabled = true;
	float amount = 1.0f;
	/// POLY — see `Param::Poly`. Off by default, as declared.
	bool poly = false;
	/// SMOOTH THROUGH RETRIGGER — see `Param::SmoothCarry`. Off by default: a new note's shape starts its smoother afresh.
	bool smoothCarry = false;
	/// TRIG — the resolved reading `mod::PolyShape`'s edge detector watches; see `Param::Trig`.
	float trig = 0.0f;
	/// NOTES FIRE TRIG — see `Param::NotesTrig`. On by default: today's behaviour.
	bool notesTrig = true;
	/// STEREO PHASE, radians 0..π — see `Param::StereoPhase`. 0 by default: mono, today's LFO.
	float stereoPhase = 0.0f;

	[[nodiscard]] bool synced() const noexcept {
		return timeBase > 0;
	}
	/// THE RIGHT EAR'S LEAD, in cycles of the wave (in steps, for a walk): `stereoPhase` / 2π.
	[[nodiscard]] float stereoLead() const noexcept {
		return stereoPhase / (2.0f * static_cast<float>(M_PI));
	}
};

/// DEFAULTS — centralized so the row defaults, the card defaults and the sanitize
/// all read one place.
inline constexpr Settings defaults() { return Settings {}; }

/// THE TIME BASE NAMES. All the standard divisions plus "Note" for audio-rate tracking.
/// "Note" is the last entry; its index is kNoteSyncBase.
///
/// **THE FIRST IS "Hz", AND IT WAS "Off"** (backlog NJ). It never meant "no rate": it is the base where Rate is a
/// frequency in its own right, unsynced to the tempo. In Free mode its phase still follows the song's SECONDS while
/// the transport plays (see `Shape::tick`). A patch that stored "Off" reads it through `kTimeBaseAliases`.
inline constexpr const char* kTimeBaseNames[] = {
	"Hz",    "1/64", "1/64.", "1/64T", "1/32", "1/32.", "1/32T", "1/16", "1/16.", "1/16T",
	"1/8",   "1/8.", "1/8T",  "1/4",  "1/4.",  "1/4T",  "1/2",  "1/2.",  "1/2T",
	"1 bar", "1 bar.", "1 barT", "2 bar", "2 bar.", "2 barT", "4 bar", "4 bar.", "4 barT",
	"Note",
};
inline constexpr size_t kTimeBaseCount = std::size(kTimeBaseNames);
static_assert(kTimeBaseCount > device::kDivisionCount);
/// "Off" WAS THE FIRST BASE'S NAME UNTIL 2026-10-17 — see above. A Choice is stored by its label.
inline constexpr device::ChoiceAlias kTimeBaseAliases[] = { { "Off", "Hz" } };

/// NOTE_SYNC is a sentinel time base that means "derive the rate from the note".
/// It's the last entry in kTimeBaseNames.
inline constexpr size_t kNoteSyncBase = kTimeBaseCount - 1;

/// @todo Note sync requires note-frequency plumbing in ModTickContext; deferred.
[[maybe_unused]] constexpr bool kNoteSyncPlaceholder = true;

/// THE DESCRIPTOR TABLE. Index-aligned with Param. The waveform and choice controls
/// are stepped; the skew knobs and phase are continuous.
inline constexpr device::ParamDesc kParamDescs[] = {
	{ "waveform", "Waveform", 0.0, static_cast<double>(Waveform::kCount) - 1.0, 0.0, true, device::Unit::Choice,
	  kWaveformNames, false, false,
	  "The base shape — sine, triangle or square — the skew knobs deform" },
	// **THE THREE SKEWS ARE THE SHAPED WAVES', AND THE SEED IS RANDOM'S** — gated, so a card shows
	// each only where it does something (`device::relevantFor`). The skews remap a PHASE and
	// the random walk has no phase to remap; the seed picks a sequence and a sine has no sequence to
	// pick. Both were stated in prose in these very tooltips — "only meaningful in Random mode",
	// "periodic modes only" — and declared to nobody, so the card drew all four at all times and the
	// honesty rule was carried by a sentence a player has to hover to read.
	// **0..1 WITH 0.5 NEUTRAL, NOT 0..100 — because `Unit::Percent` in this project STORES A FRACTION**
	// and the box multiplies by 100 to print it. Declared 0..100, these read **5000%** (2026-09-19). Two spellings of percent in one declaration; the
	// range is what moves, or the project acquires a second convention.
	//
	// AND THEY NEST rather than each owning a quadrant — see `SkewMap`. Pre-Cross places the
	// mid-crossing and so sets the two halves; the other two divide the half each belongs to.
	{ "skew_pre_peak", "Skew Pre-Peak", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
	  "Where the peak sits INSIDE the first half — 0% puts it at the very start, 100% at the crossing",
	  0, 0.0, {}, static_cast<int16_t>(Param::Waveform), kShapedWaves },
	{ "skew_pre_cross", "Skew Pre-Cross", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
	  "How much of the cycle is the FIRST HALF — at 100% a triangle becomes a rising sawtooth, at 0% a falling one",
	  0, 0.0, {}, static_cast<int16_t>(Param::Waveform), kShapedWaves },
	{ "skew_pre_trough", "Skew Pre-Trough", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
	  "Where the trough sits INSIDE the second half — 0% puts it at the crossing, 100% at the end",
	  0, 0.0, {}, static_cast<int16_t>(Param::Waveform), kShapedWaves },
	{ "seed", "Seed", 0.0, 999.0, 0.0, true, device::Unit::Number, {}, false, false,
	  "Random seed — which random walk plays. The walk is deterministic against the song position, so "
	  "a seed is a sequence you choose rather than a surprise",
	  0, 0.0, {}, static_cast<int16_t>(Param::Waveform), kRandomWave },
	{ .key = "time_base", .name = "Time Base", .min = 0.0, .max = static_cast<double>(kTimeBaseCount) - 1.0,
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kTimeBaseNames, .modulatable = true,
	  .offsetsMayLeaveRange = false,
	  .tooltip = "Musical divisions the rate locks to — Hz makes Rate a frequency, unsynced to the tempo (in Free "
	             "mode it still follows the song's position); Note for audio-rate tracking",
	  .choiceAliases = kTimeBaseAliases },
	{ "rate", "Rate", kTimeLog2SecondsMin, kTimeLog2SecondsMax, 0.0, false, device::Unit::LogMultiple, {},
	  true, false,
	  "How fast the LFO runs — a multiplier on the division, or its frequency in Hz when Time Base is Hz" },
	{ "smoothing", "Smoothing", kSmoothingLog2SecondsMin, kSmoothingLog2SecondsMax,
	  static_cast<double>(defaults().smoothing), false, device::Unit::LogSeconds, {}, true, false,
	  "How much the corners are softened — square becomes trapezoid; at the bottom of its travel it is off" },
	{ "mode", "Mode", 0.0, static_cast<double>(Mode::kCount) - 1.0, 0.0, true, device::Unit::Choice,
	  kModeNames, false, false,
	  "How the LFO runs — Gate retriggers while held, Loop cycles, Once fires and holds, Free runs continuously, LFO Retrig runs and restarts on every note" },
	{ "phase", "Phase", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,
	  "Where in the waveform it starts — 0 begins at the start, 1 is the end" },
	{ "ping_pong", "Ping-Pong", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Reverses at the loop boundaries instead of wrapping forward" },
	{ "bipolar", "Bipolar", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Swings the output between -1 and +1 instead of 0 and 1" },
	{ "enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
	  "Bypass switch — off removes the LFO from the rack" },
	{ "amount", "Amount", -1.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
	  "How deeply the LFO modulates its destination — the output level of the modulation signal" },
	{ "poly", "Poly", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "On: every note gets its own wave, restarted at its note. Off: one wave every note shares" },
	{ "smooth_carry", "Smooth Carry", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Smooth through retrigger: a new note's wave glides from where the last note's smoothed output stood, "
	  "instead of starting its smoothing from zero. Needs Poly and some Smoothing",
	  0, 0.0, {}, static_cast<int16_t>(Param::Poly), 1u << 1 },
	mod::trigParamDesc("trig", "Trig",
	                   mod::trigTooltip<"Resets the wave to its start phase: it fires when this rises past half and "
	                                    "fires again only after it falls below a quarter. Route a trigger here, or "
	                                    "click to fire by hand.">()),
	mod::notesTrigParamDesc("notes_trig", "Notes Trig"),
	// NOT A DESTINATION, as Phase is not: whether the output is a pair is decided from this control's value at the
	// drain (`SourceDesc::pairWhile`), so a route moving it across zero would have to re-run the drain every tick.
	{ .key = "stereo_phase", .name = "Stereo Phase", .min = 0.0, .max = static_cast<double>(M_PI), .def = 0.0,
	  .stepped = false, .unit = device::Unit::Degrees, .names = {}, .modulatable = false,
	  .tooltip = "How far ahead the right ear reads the wave — 0° is mono, 90° a quarter cycle, 180° the opposite "
	             "side. Random reads the same walk that far along, not a second one" },
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE SINGLE OUTPUT. The host routes this to modulation destinations.

/// THE ROW. One float per control, index-aligned with Param.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE SINGLE OUTPUT. The host routes this to modulation destinations.
inline constexpr device::SourceDesc kOutputs[] = {
	/// PER VOICE IN VALUE (backlog GA, 2026-10-03) — see the MSEG's output and `mod/per_note.h`.
	{ .key = "lfo", .name = "LFO", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar), .perVoice = true,
	  .pairWhile = static_cast<int>(Param::StereoPhase) },
};

/// TAGS — what the browser groups this by.
inline constexpr const char* kTags[] = { "lfo", "modulator" };

/// WHAT THIS DEVICE IS, in the vocabulary device.h states.
inline constexpr device::Traits kTraits {
	.tag = "LFO",
	.name = "LFO",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = static_cast<int>(Param::Amount),
	.tags = kTags,
};

extern const device::ModOps kModOps;
/// AND HOW IT LISTENS TO THE NOTE LANE — the per-note waves. Defined in lfo.cpp.
extern const device::NoteOps kNoteOps;

/// ROW <-> SETTINGS conversion. Declared here so the codec test can call them.
Row rowOf(const Settings& s);
Settings settingsOf(const Row& row, size_t fieldsPresent);
void sanitize(Settings* settings);
float valueOf(const Settings& s, Param param);
void setValueOf(Settings* settings, Param param, float value);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::lfo
