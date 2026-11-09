/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/registry.h"
#include "mod/modulator.h"
#include "mod/trig.h"

#include <array>
#include <cmath>
#include <cstddef>

// THE MSEG: A SHAPE MADE OF SEGMENTS, WHERE THE DAHDSR IS A SHAPE MADE OF STAGES.
//
// Both were asked for on 2026-08-22 — a standard DAHDSR envelope generator and an MSEG, with the goal
// of parity with Bitwig — and the second one is not the same request twice. A DAHDSR has SIX stages
// with fixed meanings and the user turns their times; an MSEG has however many segments the user
// draws, each with a duration, a destination level and a curve, and the meanings are whatever the
// shape says. Bitwig ships them as separate modules for that reason and so do we.
//
// WHAT IT IS FOR, BEYOND ITSELF. The device-browser plan calls this the TEST rather than a feature:
// "a second modulator type with a different control count is the only thing that demonstrates pin #1
// is actually gone rather than merely widened". A rack that can only ever hold DAHDSRs has not been
// unpinned, it has been widened by one; the claim is only checkable once something with a different
// shape sits in a slot beside one.
//
// WHAT IT REUSES, WHICH IS ALMOST EVERYTHING. The gate, the control tick, the trigger modes, the
// smoothing, the bipolar rescale, the log2-seconds time taper — all of it is mod/modulator.h now,
// promoted out of the DAHDSR by this device asking for it. What is genuinely new is two things: a
// variable number of segments, and LOOP MARKERS.
//
// THE MARKERS ARE THE POINT OF THE SECOND DEVICE. The rack plan records what a plain looping flag
// cannot say: "Bitwig's Segments loops between two points INSIDE the shape, so an attack plays once
// and a middle repeats under it — the sampler's sustain loop. On the MSEG that is two draggable
// markers rather than a flag, and that is where it should land." So it lands here, as `loopStart`
// and `loopEnd` segment indices, and the DAHDSR keeps its flag.

namespace feathers::mod::mseg {

/// THE MUSICAL DIVISIONS THE WINDOW CAN BE, in beats.
///
/// So the dropdown names a length in BEATS and the scale knob multiplies it.
///
/// IN BEATS RATHER THAN IN SECONDS, because a beat is the only thing the host tells us that does not
/// change meaning with the tempo — `TransportClock::secondsPerBeat` is the whole of what arrives, and
/// a table in seconds would have to be rebuilt every time somebody drew a tempo ramp.
///
/// A 4/4 BAR IS FOUR BEATS AND THAT IS AN ASSUMPTION, stated because it is one. CLAP reports a time
/// signature and this table ignores it, so "1 bar" is four beats in 7/8 as well. Correct for the
/// overwhelming majority of sessions and wrong in a way a user in 7/8 would notice; fixing it means
/// carrying the signature on the clock, which is a widening this arc can do later without moving
/// anything else.
/// THE MUSICAL DIVISIONS — **MOVED TO `device/device.h`, AND THESE ALIASES ARE WHY NOTHING ELSE
/// CHANGED.** Same reason `TransportClock` moved and the same day: A list of musical divisions
/// is a fact about MUSIC, not about a multi-segment envelope — a delay in the fx lane wants exactly
/// this table, and an fx device should not have to include a modulator's header to find out what a
/// dotted eighth is.
///
/// The names stay valid here for the fifteen references in `mseg.cpp` and `test_mod_mseg.cpp`.
using device::kDivisionBeats;
using device::kDivisionNames;
using device::kDivisionCount;
using device::kDefaultDivision;

/// HOW LONG THE SHAPE'S WINDOW IS, from the two controls and whatever the host has said.
///
/// TWO SOURCES FOR THE WINDOW AND ONE MEANING FOR A SHARE — which is the arrangement that dissolves
/// the awkward question. A share never becomes a duration; the WINDOW is what changes:
///
///     tempo known    W = divisionBeats * secondsPerBeat * scale
///     no tempo       W = scale, read as seconds
///
/// The share divides it either way. The device degrades from "one bar" to "a second and a half" and
/// stays the same device — no second code path, no field with two readings, and nothing in the card
/// asking which kind of number it is holding.
///
/// THE COST, TAKEN DELIBERATELY: a no-tempo shape does NOT keep the absolute durations it had before
/// this existed. The format is breaking anyway (approved), and "the field means one thing"
/// is worth more than "the numbers are unchanged in a case most users never reach" — standalone, and
/// hosts offering neither timeline.
inline double windowSeconds(double divisionBeats, double scaleSeconds, TransportClock clock) {
	if (!(scaleSeconds > 0.0) || !std::isfinite(scaleSeconds)) {
		return 0.0; // a window with no length: the shape freezes rather than dividing by zero
	}
	if (!clock.haveTempo()) {
		return scaleSeconds;
	}
	return divisionBeats * clock.secondsPerBeat * scaleSeconds;
}

/// HOW MANY SEGMENTS A SHAPE MAY HAVE.
///
/// Fixed, and it has to be: the no-allocation-after-activate rule means the audio thread cannot grow
/// a shape, so the storage is an array and the count rides beside it. Eight is chosen against the
/// CARD rather than against the DSP — a segment needs enough width to grab a breakpoint in, and a
/// card is 216px wide. The engine would not care about sixteen.
inline constexpr size_t kMaxSegments = 8;

/// Fields per segment: how long it takes, where it ends up, and how it bends getting there.
inline constexpr size_t kSegmentFields = 3;

/// WHAT "NO SUSTAIN POINT" IS. A shape may run start to finish and stop, with the gate deciding only
/// when it BEGINS — that is a one-shot envelope and it is a legitimate MSEG. A sustain point is the
/// other kind: the shape runs to that breakpoint and waits there for the gate to fall.
///
/// Stated as a value outside the segment range rather than as a second boolean, because "sustains at
/// segment 3" and "does not sustain" are answers to one question and a bool beside an index is two
/// places for them to disagree.
inline constexpr int kNoSustain = -1;

/// THE MODE — one dropdown replacing the old Loop / SingleTrigger / Trigger toggles (see
/// mod::Mode). A modulator's mode is a fact about how it answers the gate, shared by both modulators.
using Mode = mod::Mode;

//------------------------------------------------------------------//
// The controls — index-aligned with kParamDescs and with Row below. //
//------------------------------------------------------------------//

/// THE ORDER IS THE STREAM ORDER AND THE DESCRIPTOR ORDER AND THE ROW ORDER, all three, exactly as
/// the DAHDSR's is. The globals come first so that a shape with fewer segments than the maximum still
/// reads its globals from a short payload — see settingsOf's `fieldsPresent`.
enum class Param : int {
	//-- The globals.
	StartLevel,     ///< where the shape begins, 0..1
	SegmentCount,   ///< how many of the segments below are live
	SustainSegment, ///< which breakpoint holds the gate, or kNoSustain
	LoopStart,      ///< first segment of the loop
	LoopEnd,        ///< last segment of the loop
	TimeBase,       ///< which musical division the window is — an index into kDivisionBeats
	TimeScale,      ///< how many of that division the window is; seconds, when there is no tempo
	/// **AND NO SWITCH BESIDE IT — the bottom of this control's travel IS off**.
	/// See `mod::smoothingCoeff`, which is the one law all three modulators now share.
	Smoothing,
	/// **THE MODE — one dropdown replacing Loop, SingleTrigger and Trigger** (see
	/// `dahdsr::Param::Mode`). Collapses three overlapping toggles into one enum. NOT MODULATABLE — a
	/// mode under a modulator is a switch chattering at 750 Hz.
	Mode,
	/// **PHASE — where in the shape it starts, 0..1.** 0 begins at the first segment; 1 is the end of
	/// the last. The engine walks the segment shares to find the segment and the phase within it, the
	/// same arithmetic `lockTo` uses for song-position locking, so a phase setting and a locked
	/// position cannot disagree about where a boundary is. NOT MODULATABLE — a ramping phase is a
	/// time-warp that would click.
	Phase,
	/// **PING-PONG: the loop plays forward then backward.** Off (the default) wraps from loopEnd
	/// straight to loopStart; on reverses at the boundaries so the middle segment travels both ways.
	/// NOT modulatable — a ramping flag is a switch chattering at 750 Hz, the same reason Type and
	/// Mode decline.
	PingPong,
	Bipolar,

	/// **THE BYPASS SWITCH, DRAWN BY THE SHARED HEAD.** See `dahdsr::Param::Enabled` for the reasoning;
	/// the only difference here is WHERE it goes. **It must stay inside the GLOBAL block**, because
	/// `kFirstSegmentParam` and the `static_assert` below tie `kCount` to
	/// `kFirstSegmentParam + kMaxSegments * kSegmentFields` — appending after the segments would break
	/// that arithmetic, and the assert exists so nobody has to count enumerators by hand.
	Enabled,
	/// **THE AMOUNT** — one knob scaling every output before every route (backlog DH; see
	/// `dahdsr::Param::Amount`), applied by the host, not by the shape — and the ONLY depth: the shape's own Depth (which
	/// scaled its travel about the start level) was removed, backlog FH, as the DAHDSR has none. INSIDE THE GLOBAL BLOCK, as `Enabled`'s note
	/// requires — which moves every segment column down one, so `kStateVersion` moved with it (8).
	Amount,
	/// **POLY — one shape per note, or one for the device**. See `dahdsr::Param::Poly`. In the
	/// global block for `Enabled`'s reason; a KEYED column, so the segments moving down one moves no saved patch.
	Poly,
	/// **SMOOTH THROUGH RETRIGGER** (backlog MM) — see `mod::PolyShape::processNotes`. Global, keyed.
	SmoothCarry,
	/// **TRIG — RESTARTS THE SHAPE** (backlog MV): fired at 0.5, re-armed below 0.25 (`mod/trig.h`). Global, keyed.
	Trig,
	/// **NOTES FIRE TRIG** (backlog NU): the note-on normal into Trig, and the switch that breaks it — `mod::notesTrigParamDesc`.
	/// Global, keyed.
	NotesTrig,
	/// **STEREO PHASE — THE RIGHT EAR READS THE SHAPE THIS FAR AHEAD** (backlog PA, the LFO's control on the MSEG).
	/// Radians 0..π, read in degrees, and 360° is the WINDOW — the unit `Phase` is in, so the right ear is the shape at
	/// phase + offset in every mode: ahead in time by that share of the window, through the same loop, hold and end as
	/// the left ear (`Shape::rawRightAt`). At 0 the output is mono and is today's MSEG to the bit; otherwise it is an ear
	/// pair (`SourceDesc::pairWhile`). Derived from the left ear's position, so there is nothing new to persist.
	/// Global, keyed.
	StereoPhase,

	//-- Then the per-segment block, three fields apiece, segment-major.
	Seg0Time, Seg0Level, Seg0Shape,
	Seg1Time, Seg1Level, Seg1Shape,
	Seg2Time, Seg2Level, Seg2Shape,
	Seg3Time, Seg3Level, Seg3Shape,
	Seg4Time, Seg4Level, Seg4Shape,
	Seg5Time, Seg5Level, Seg5Shape,
	Seg6Time, Seg6Level, Seg6Shape,
	Seg7Time, Seg7Level, Seg7Shape,

	kCount,
};

/// Where the per-segment block starts, so the two halves can be reasoned about separately without
/// anybody counting the enumerators above by hand.
inline constexpr size_t kFirstSegmentParam = static_cast<size_t>(Param::Seg0Time);
static_assert(static_cast<size_t>(Param::kCount)
              == kFirstSegmentParam + kMaxSegments * kSegmentFields);

/// Which field of a segment: the three in the order they appear.
enum class SegmentField : size_t { Time, Level, Shape };

/// The Param for one field of one segment. THE ONLY WAY ANYTHING SHOULD ADDRESS A SEGMENT CONTROL —
/// the enumerators above are written out because an enum cannot be generated, not because anyone
/// should be reaching for `Param::Seg5Level` by name.
constexpr Param segmentParam(size_t segment, SegmentField field) {
	return static_cast<Param>(kFirstSegmentParam + segment * kSegmentFields
	                          + static_cast<size_t>(field));
}

//---------------//
// The declaring //
//---------------//

/// The per-segment control names, written out because a constexpr descriptor table needs string
/// literals and there is no constexpr way to build "Seg 3 Time" from a number.
inline constexpr const char* kSegmentNames[kMaxSegments][kSegmentFields] = {
	{ "Seg 1 Time", "Seg 1 Level", "Seg 1 Curve" },
	{ "Seg 2 Time", "Seg 2 Level", "Seg 2 Curve" },
	{ "Seg 3 Time", "Seg 3 Level", "Seg 3 Curve" },
	{ "Seg 4 Time", "Seg 4 Level", "Seg 4 Curve" },
	{ "Seg 5 Time", "Seg 5 Level", "Seg 5 Curve" },
	{ "Seg 6 Time", "Seg 6 Level", "Seg 6 Curve" },
	{ "Seg 7 Time", "Seg 7 Level", "Seg 7 Curve" },
	{ "Seg 8 Time", "Seg 8 Level", "Seg 8 Curve" },
};

/// The per-segment state KEYS, parallel to `kSegmentNames` — each is its name lowercased with every run
/// of other characters turned to `_` (see `device::ParamDesc::key`). A key names a meaning and is frozen,
/// so a renamed segment label keeps its key here.
inline constexpr const char* kSegmentKeys[kMaxSegments][kSegmentFields] = {
	{ "seg_1_time", "seg_1_level", "seg_1_curve" },
	{ "seg_2_time", "seg_2_level", "seg_2_curve" },
	{ "seg_3_time", "seg_3_level", "seg_3_curve" },
	{ "seg_4_time", "seg_4_level", "seg_4_curve" },
	{ "seg_5_time", "seg_5_level", "seg_5_curve" },
	{ "seg_6_time", "seg_6_level", "seg_6_curve" },
	{ "seg_7_time", "seg_7_level", "seg_7_curve" },
	{ "seg_8_time", "seg_8_level", "seg_8_curve" },
};

/// How many segments a DEFAULT shape has, and what it is: a three-segment rise-hold-fall, which is
/// the smallest shape that looks like an envelope rather than like a ramp. A device that opens on an
/// empty shape asks the user to build one before it does anything, which is a worse first minute than
/// a shape they can hear and then change.
inline constexpr size_t kDefaultSegments = 3;

/// THE DESCRIPTOR TABLE, built by a function so the twenty-four segment entries are a loop rather
/// than twenty-four hand-written lines that could disagree with each other. The DAHDSR's table is
/// written out because it has fifteen entries with fifteen different meanings; this one has thirteen
/// like that and twenty-four that are the same three, eight times.
constexpr std::array<device::ParamDesc, static_cast<size_t>(Param::kCount)> makeParamDescs() {
	std::array<device::ParamDesc, static_cast<size_t>(Param::kCount)> d {};
	const auto at = [&d](Param p) -> device::ParamDesc& { return d[static_cast<size_t>(p)]; };

	at(Param::StartLevel) = { "start_level", "Start Level", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
		                      "The level the shape begins at before the first segment" };
	// COUNTS AND INDICES ARE STEPPED AND NOT MODULATABLE. A route that could move the segment count
	// would be modulating the SHAPE OF THE STATE MACHINE rather than a number in it — the same reason
	// the filter's keytrack is patch state rather than a knob the DSP reads.
	at(Param::SegmentCount) = { "segments", "Segments",          1.0,  static_cast<double>(kMaxSegments),
		                        static_cast<double>(kDefaultSegments), true, device::Unit::Number,
		                        {},                   false, false, "How many segments the shape has — more segments, more detail" };
	at(Param::SustainSegment) = { "sustain_at", "Sustain At", static_cast<double>(kNoSustain),
		                          static_cast<double>(kMaxSegments) - 1.0, 1.0, true,
		                          device::Unit::Number, {}, false, false,
		                          "Which segment the shape holds at while the note is pressed — Off means no sustain" };
	at(Param::LoopStart) = { "loop_from", "Loop From", 0.0, static_cast<double>(kMaxSegments) - 1.0, 0.0, true,
		                     device::Unit::Number, {}, false, false,
		                          "First segment of the loop — the shape cycles from here to Loop To" };
	at(Param::LoopEnd) = { "loop_to", "Loop To", 0.0, static_cast<double>(kMaxSegments) - 1.0,
		                   static_cast<double>(kDefaultSegments) - 1.0, true, device::Unit::Number,
		                   {}, false, false, "Last segment of the loop — the shape returns to Loop From after this" };
	// **PING-PONG: the loop plays forward then backward.** Off (the default) wraps from loopEnd
	// straight to loopStart; on reverses at the boundaries so the middle segment travels both ways.
	// NOT modulatable — a ramping flag is a switch chattering at 750 Hz.
	at(Param::PingPong) = { "ping_pong", "Ping-Pong", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
		                "Off: the loop wraps from Loop To straight to Loop From. On: it reverses at both ends and plays the middle segments both ways" };

	// THE TWO SCALES ARE THE MSEG'S MODULATABLE HANDLES ON ITS OWN SHAPE, and they are the reason the
	// breakpoints do not need to be. Modulating "how fast the whole shape runs" and "how far it
	// travels" reaches every musical thing a hand wants from an MSEG under modulation; modulating one
	// breakpoint out of eight reaches almost nothing and costs eight destinations per device.
	// STEPPED, AND MODULATABLE SINCE 2026-10-30 (read as
	// general — lead's call). It said "not modulatable: a route would move which SHAPE OF TIME the
	// device is in rather than a number in it". A routed division lands on whole indices (stepped) and the window follows
	// through `mod::TransportLock`, which re-anchors only at a jump, so the shape keeps its place on the song.
	at(Param::TimeBase) = { "time_base", "Time Base", 0.0, static_cast<double>(kDivisionCount) - 1.0,
		                    static_cast<double>(kDefaultDivision), true, device::Unit::Choice,
		                    kDivisionNames, true, false,
		                    "Musical divisions the rate snaps to — or a free-running speed" };

	// THE SCALE KNOB, AND IT IS THE SAME KNOB IT ALWAYS WAS — re-meaned rather than replaced. It
	// reached the engine as a multiplier on every duration; it now multiplies the WINDOW, which is the
	// same arithmetic once segments are shares of that window.
	//
	// LogSeconds, AND THE TAPER IS RIGHT FOR BOTH OF ITS ROLES RATHER THAN BY LUCK. Standalone it IS a
	// duration, so log2 is the taper a duration wants. Synced it is a multiplier against a division —
	// and MUSICAL RATIOS ARE POWERS OF TWO, so a log2 control lands exactly on half, double and
	// quarter at whole-number steps. One range serves both honestly.
	//
	// WHAT DOES NOT SERVE BOTH IS THE UNIT, and that is a wart worth naming rather than hiding. The
	// same stored number means "1.5 seconds" with no tempo and "1.5x the division" with one, and
	// `ParamDesc` declares ONE unit — so a card that wants to print it correctly has to ask the clock.
	// Every alternative I could find is worse: a second knob doubles the controls, and a fixed nominal
	// tempo makes the standalone case quietly lie about what it is doing.
	at(Param::TimeScale) = { "rate", "Rate", kTimeLog2SecondsMin, kTimeLog2SecondsMax, 0.0, false,
		                     device::Unit::LogMultiple, {}, true, false,
		                      "How fast the whole shape runs — standalone it is a duration, synced it scales the division" };

	// DEFAULTS TO THE BOTTOM OF ITS TRAVEL, WHICH IS OFF — see the DAHDSR's descriptor for the
	// argument. It read 20 ms beside a `Smoothing On` that defaulted to 0, so the number described
	// nothing the device was doing.
	at(Param::Smoothing) = { "smoothing", "Smoothing", kSmoothingLog2SecondsMin, kSmoothingLog2SecondsMax,
		                     static_cast<double>(kSmoothingLog2SecondsMin), false,
		                     device::Unit::LogSeconds, {}, true, false,
		                      "How much the shape's corners are softened — prevents clicks on fast segments; at the bottom of its travel it is off" };
	// **THE MODE — one dropdown replacing Loop / SingleTrigger / Trigger** (see `dahdsr::Param::Mode`).
	// Five choices: Gate, Loop, Once, Free, LFO Retrig. NOT MODULATABLE — a mode under a modulator is a
	// switch chattering at 750 Hz, the same reason the filter's Type declines.
	at(Param::Mode) = { "mode", "Mode", 0.0, static_cast<double>(mod::Mode::kCount) - 1.0, 0.0, true,
		                   device::Unit::Choice, kModeNames, false, false,
		                    "How the shape runs — Gate retriggers while held, Loop cycles, Once fires and holds, Free runs continuously, LFO Retrig runs and restarts on every note" };
	at(Param::Bipolar) = { "bipolar", "Bipolar", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
		                   "Swings the shape between -1 and +1 instead of 0 and 1" };
	// **PHASE — where in the shape it starts, 0..1.** The engine walks the segment shares to find the
	// segment and phase, the same arithmetic `lockTo` uses for song-position locking. NOT MODULATABLE —
	// a ramping phase is a time-warp that would click.
	at(Param::Phase) = { "phase", "Phase", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,
		              "Where in the shape it starts — 0 begins at the first segment, 1 is the end of the last" };
	/// ON BY DEFAULT — a modulator added to a rack should modulate. See `dahdsr`'s note.
	at(Param::Enabled) = { "enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
		                   "Switches the whole shape on or off" };
	/// See `Param::Amount`. Modulatable — a depth a source cannot reach would be the odd one out.
	at(Param::Amount) = { "amount", "Amount", -1.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
		                  "How deeply the shape modulates its destination" };
	/// See `Param::Poly`. ON BY DEFAULT — a drawn envelope is a note's, as the DAHDSR's is.
	at(Param::Poly) = { "poly", "Poly", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
		                "On: every note gets its own shape. Off: one shape every note shares, restarted by notes as the mode says" };
	/// See `Param::SmoothCarry`. Gated on Poly: under mono the one shape's smoother already runs through a retrigger.
	at(Param::SmoothCarry) = { "smooth_carry", "Smooth Carry", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames,
		                       false, false,
		                       "Smooth through retrigger: a new note's shape glides from where the last note's smoothed "
		                       "output stood, instead of starting its smoothing from zero. Needs Poly and some Smoothing",
		                       0, 0.0, {}, static_cast<int16_t>(Param::Poly), 1u << 1 };
	at(Param::Trig) = mod::trigParamDesc("trig", "Trig",
	                                     mod::trigTooltip<"Restarts the shape from its start: it fires when this rises "
	                                                      "past half and fires again only after it falls below a "
	                                                      "quarter. Route a trigger here, or click to fire by hand.">());
	at(Param::NotesTrig) = mod::notesTrigParamDesc("notes_trig", "Notes Trig");
	// NOT A DESTINATION, as the LFO's is not: whether the output is a pair is decided from this control's value at the
	// drain (`SourceDesc::pairWhile`), so a route moving it across zero would have to re-run the drain every tick.
	at(Param::StereoPhase) = { "stereo_phase", "Stereo Phase", 0.0, static_cast<double>(M_PI), 0.0, false,
		                       device::Unit::Degrees, {}, false, false,
		                       "How far ahead the right ear reads the shape — 0° is mono, 360° would be the whole "
		                       "window, so 180° is half of it. It runs through the same loop, hold and end as the left" };

	for (size_t s = 0; s < kMaxSegments; ++s) {
		// A SEGMENT'S TIME DEFAULTS TO 200 ms — log2(0.2) — rather than to the bottom of the range,
		// because the bottom means "no time at all" and a shape whose segments all take no time is a
		// vertical edge. The DAHDSR can default a stage to off because its stages have names and a
		// hold nobody asked for should not happen; an MSEG's segments exist only because someone drew
		// them.
		d[static_cast<size_t>(segmentParam(s, SegmentField::Time))] = {
			kSegmentKeys[s][0], kSegmentNames[s][0], kTimeLog2SecondsMin, kTimeLog2SecondsMax, -2.321928094887362, false,
			device::Unit::LogSeconds, {}, false, false,
			"How long this segment takes to reach its target level"
		};
		d[static_cast<size_t>(segmentParam(s, SegmentField::Level))] = {
			kSegmentKeys[s][1], kSegmentNames[s][1], 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, false, false,
			"This segment's target level"
		};
		d[static_cast<size_t>(segmentParam(s, SegmentField::Shape))] = {
			kSegmentKeys[s][2], kSegmentNames[s][2], -kShapeMax, kShapeMax, 0.0, false, device::Unit::Percent, {}, false, false,
			"Bends this segment — negative is a slow start that snaps, positive is a fast start that eases"
		};
	}
	return d;
}

inline constexpr auto kParamDescs = makeParamDescs();
static_assert(kParamDescs.size() == static_cast<size_t>(Param::kCount));

/// WHAT THIS DEVICE PUBLISHES TO THE MODULATION BUS — one shaped signal.
///
/// ONE, THOUGH IT HAS EIGHT SEGMENTS, and the distinction is worth stating because "multi-segment"
/// sounds like "multi-output" and is not. `Mseg::value()` returns a single float: the segments are
/// the SHAPE of one signal over time, not eight signals. A device with genuinely several outputs —
/// an XY pad — declares several here, and the host reads this list rather than guessing from
/// anything about the device's internals.
///
/// BIPOLAR IS DECLARED TRUE THOUGH THE SWITCH DEFAULTS OFF: a device declares the WIDEST signal it
/// can publish, not what it happens to be doing this block, for the reason the DAHDSR's does — and
/// it NAMES that switch, so a surface drawing a reach can ask what is being published right now
/// rather than what could be. See `device::SourceDesc::bipolarControl`.
inline constexpr device::SourceDesc kOutputs[] = {
	/// PER VOICE IN VALUE (backlog GA, 2026-10-03): the collapsed shape for every un-migrated destination, and one
	/// shape per NOTE behind `ModOps::outputForNote` for the per-voice runs — see `mod/per_note.h`.
	{ .key = "mseg", .name = "MSEG", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar), .perVoice = true,
	  .pairWhile = static_cast<int>(Param::StereoPhase) },
};

/// WHAT THIS DEVICE IS, in the vocabulary device.h states. The card is its own — an MSEG is the
/// canonical case for that, since a list of thirty-seven knobs shows a shape to nobody.
/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "lfo", "env", "modulator" };

inline constexpr device::Traits kTraits {
	// FOUR BYTES, AND IT WAS MISSING. The first draft omitted this, so `kTraits.tag` was a null
	// pointer — and audition.cpp reads a device's tag as four bytes without asking, so the first
	// commit to put an MSEG in a slot would have read through it. A landmine timed to go off in
	// somebody else's commit, looking like a bug in the rack wiring rather than in a declaration
	// merged hours earlier. It arrived as a -Wmissing-field-initializers warning about the OTHER
	// omitted field, which is the only reason anyone looked.
	.tag = "MSEG",
	.name = "MSEG",
	.hasCard = true,
	// DECLINED FOR NOW, AND FOR THE OPPOSITE REASON THE DAHDSR DECLINES.
	//
	// The DAHDSR says false because it is COMPLETE on one card — six times, three curves and four
	// switches inside the 320px cap, with nothing left over to put in a second view. device.h names
	// this device as the case that goes the other way: "a filter has a response curve, an envelope
	// has a shape, an MSEG has an editor". Eight breakpoints with draggable handles and two loop
	// markers do not fit a 216px card the way an ADSR's four numbers do, and the parity audit already
	// priced a pop-out editor as "a second instrument surface".
	//
	// So this will become true. It is false TODAY because nothing renders an expanded view, and a
	// device that declared one would be promising a surface the host cannot draw — the honesty rule,
	// which does not care that the promise is about future work. device.h's own wording is the test:
	// declining costs nothing, so decline until the surface exists.
	.hasExpanded = false,
	/// THE HEAD'S SQUARE. See `Param::Enabled`.
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = static_cast<int>(Param::Amount),
	.tags = kTags,
};

/// EVERYTHING THIS DEVICE SAYS ABOUT ITSELF, assembled from the declarations above rather than
/// repeating any of them. This is what the host's registry holds and what a generic card would walk.
/// HOW A HOST ADVANCES THIS DEVICE — defined in mseg.cpp. See device::ModOps.
extern const device::ModOps kModOps;
/// AND HOW IT LISTENS TO THE NOTE LANE — the per-note shapes. Defined in mseg.cpp.
extern const device::NoteOps kNoteOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.category = device::Category::Modulators,
};

/// The flattened state, one float per control, index-aligned with Param. The switches and the counts
/// ride as floats for the DAHDSR's stated reason: the row is one kind of thing and its length is one
/// multiplication, where a byte-packed struct would be a place for two builds to disagree about
/// padding.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

//---------------//
// The settings  //
//---------------//

/// One segment of the shape.
struct Segment {
	/// ITS SHARE OF THE SHAPE'S WINDOW, in log2. The bottom of the range means no time at all, so a
	/// segment there is a vertical step to its level rather than a ramp.
	///
	/// A SHARE, NOT A DURATION, AND ALWAYS A SHARE. The time base scales the entire
	/// x axis, so the whole curve fits inside of (time base choice) * scale knob. The window is
	/// what the two controls decide; a segment says only what FRACTION of it this segment takes, and
	/// the engine spends `share_i / sum(shares) * window` on it. That normalisation is what makes
	/// "the whole curve fits inside the window" true BY CONSTRUCTION rather than by the shares
	/// happening to add up.
	///
	/// ONE MEANING, UNCONDITIONALLY — the field does not become a duration when the host has no
	/// tempo. What has two sources is the WINDOW, not this: with a tempo it is `division * scale`,
	/// without one it is `scale` read as seconds. The share divides it either way, so no reader
	/// anywhere has to ask which kind of number it is holding.
	///
	/// STILL log2, and still the same helpers the DAHDSR's durations use, because the taper is right
	/// for a share for the same reason it is right for a duration: a linear control spends most of its
	/// travel on the large end where nobody needs resolution.
	float share = -2.321928094887362f;
	/// Where it ENDS. A segment does not carry where it starts — that is the previous segment's end,
	/// or startLevel for the first — because carrying both is two numbers for one breakpoint and they
	/// can disagree.
	float level = 1.0f;
	/// How it bends getting there. See mod::kShapeMax for which way positive goes.
	float shape = 0.0f;

	friend bool operator==(const Segment&, const Segment&) = default;
};

struct Settings {
	/// WHICH CONTROL IS THE TRIG, for `mod::PolyShape`'s per-note resolve (backlog MV, B).
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);

	float startLevel = 0.0f;
	std::array<Segment, kMaxSegments> segment {};
	size_t count = kDefaultSegments;

	/// Which breakpoint the gate holds at, or kNoSustain. Segment index: the shape runs THROUGH this
	/// segment and then waits at its end level.
	int sustainSegment = 1;

	size_t loopStart = 0;
	size_t loopEnd = kDefaultSegments - 1;

	/// **PING-PONG: the loop plays forward then backward.** Off (the default) wraps from loopEnd
	/// straight to loopStart; on reverses at the boundaries so the middle segment travels both ways.
	/// NOT modulatable — a ramping flag is a switch chattering at 750 Hz, the same reason Type and
	/// Mode decline.
	bool pingPong = false;

	/// Which of kDivisionBeats the window is built on.
	size_t timeBase = kDefaultDivision;
	/// log2 of the scale: how many divisions the window is, or how many seconds when there is no
	/// tempo. Zero means one — 2^0 — which is "exactly the division" and "one second".
	float timeScale = 0.0f;

	/// AT THE BOTTOM OF ITS TRAVEL IT IS OFF, which is the whole control — the `smoothingOn` flag
	/// that used to gate it is deleted. See `mod::smoothingCoeff`.
	float smoothing = kSmoothingLog2SecondsMin;

	/// **THE MODE — one dropdown replacing Loop / SingleTrigger / Trigger** (see
	/// `dahdsr::Param::Mode`). Five choices: Gate, Loop, Once, Free, LFO Retrig.
	Mode mode = Mode::Gate;
	/// Where in the shape it starts, 0..1. 0 begins at the first segment; 1 is the end of the last.
	/// The engine walks the segment shares to find the segment and phase. NOT modulatable.
	float phase = 0.0f;
	bool bipolar = false;

	/// **THE BYPASS, AND IT LIVES HERE BECAUSE THE DESCRIPTOR SAYS IT EXISTS.** `Param::Enabled` and
	/// `Traits::enableControl` were added so the shared head draws a square; this member is what makes
	/// the square mean something. Without it `rowOf` left that index at the zero a fresh `Row` starts
	/// with, so every round trip through these settings turned the MSEG OFF (2026-09-02: the
	/// enable got wedged, stuck until reload).
	///
	/// ON BY DEFAULT, which is the declared default too: a modulator added to a rack should modulate.
	/// **AND "WHICH IS THE DECLARED DEFAULT TOO" IS NOW ASSERTED RATHER THAN SAID** — see the
	/// static_assert below. Two devices were found saying that sentence while the two numbers
	/// disagreed (2026-09-04, `fx::owltt` and `fx::automod`); a claim of agreement in a comment is not
	/// one the compiler makes.
	bool enabled = true;
	/// THE AMOUNT (backlog DH) — carried so the row round-trips whole; the host reads it, not the shape.
	float amount = 1.0f;
	/// POLY — see `Param::Poly`. On by default, as declared.
	bool poly = true;
	/// SMOOTH THROUGH RETRIGGER — see `Param::SmoothCarry`. Off by default.
	bool smoothCarry = false;
	/// TRIG — the resolved reading `mod::PolyShape`'s edge detector watches; see `Param::Trig`.
	float trig = 0.0f;
	/// NOTES FIRE TRIG — see `Param::NotesTrig`. On by default: today's behaviour.
	bool notesTrig = true;
	/// STEREO PHASE, radians 0..π — see `Param::StereoPhase`. 0 by default: mono, today's MSEG.
	float stereoPhase = 0.0f;

	/// THE RIGHT EAR'S LEAD, as a share of the window: `stereoPhase` / 2π.
	[[nodiscard]] float stereoLead() const noexcept {
		return stereoPhase / (2.0f * static_cast<float>(M_PI));
	}

	friend bool operator==(const Settings&, const Settings&) = default;
};

static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.poly == (kParamDescs[static_cast<size_t>(Param::Poly)].def >= 0.5));

//-----------//
// The verbs //
//-----------//

/// The flattened view of a shape, and back. Same contract as the DAHDSR's pair, including
/// `fieldsPresent`: a payload written by a build with fewer controls fills the rest from the declared
/// defaults rather than from zero.
Row rowOf(const Settings& settings);
Settings settingsOf(const Row& row, size_t fieldsPresent);

/// WHICH BREAKPOINT ACTUALLY HOLDS THE GATE — which is not always the one that is stored.
///
/// A LOOPING SHAPE HAS NO SUSTAIN POINT, and that is a fact about the DEVICE rather than a tidying of
/// the picture. The two are answers to the same question — what does the shape do while the gate is
/// up — and there is no arrangement of the markers in which both of them do something:
///
///   - sustain INSIDE the loop, or before it: the shape reaches the breakpoint first and holds, so
///     the loop never runs while the gate is up, and the gate falling releases past it.
///   - sustain AFTER loopEnd: the loop runs while the gate is up and the shape only arrives at the
///     breakpoint once the gate has fallen — and the hold below requires the gate, so it never holds.
///
/// So one of the two is always dead, whatever the indices say, and today it is the LOOP that dies:
/// the branch order in advance() gives the breakpoint first refusal, which makes a sustain marker
/// silently disable a loop the user turned on.
///
/// THE LOOP WINS, WHICH IS ALSO WHAT THE DAHDSR ALREADY DOES. Its `looped` flag re-fires the envelope
/// the instant the stage machine reaches Sustain, so a looping envelope never holds either. Two
/// modulators in one rack that disagreed about which marker survives a loop would be a worse answer
/// than either choice on its own.
///
/// ONE FUNCTION, ASKED BY BOTH THE ENGINE AND THE GEOMETRY, which is what makes the marker and the
/// sound agree BY CONSTRUCTION. Fixing this in the two places separately would leave two conditions
/// to keep in step, and the honesty rule is exactly about them drifting apart.
///
/// A PREDICATE RATHER THAN A SANITISE. `sanitize` could clear the field and every reader would then
/// see kNoSustain for free — but it would DESTROY the stored index, so turning Loop on and off again
/// would lose a breakpoint the user placed. The value is kept and overruled.
constexpr int sustainInForce(const Settings& settings) {
	// ONLY GATE MODE HOLDS AT A BREAKPOINT. Every other mode — Loop, Once, Free, LFO Retrig — runs
	// past it, so a sustain marker on a non-gated shape is not a hold point.
	return settings.mode == Mode::Gate ? settings.sustainSegment : kNoSustain;
}

/// Clamp and round every field by its declaration. Walked by index, which is what makes it exhaustive
/// by construction — a control appended to Param is sanitised without anyone remembering to come
/// here.
void sanitize(Settings* settings);

/// One control's value, and setting it. The two halves of what a widget needs, and the only road a
/// card or a route should take to a field.
float valueOf(const Settings& settings, Param param);
void setValueOf(Settings* settings, Param param, float value);

/// How long the whole shape takes at the current time scale, in seconds — the sum of its live
/// segments. What a card needs to lay out an x axis that is proportional to TIME, which is the
/// lesson the DAHDSR's card paid for.
/// `clock` decides which of the window's two sources applies — see windowSeconds. A caller with no
/// host, such as the card laying out an x axis, passes `unknown()` and gets the standalone window,
/// which is the honest answer for a picture drawn with no tempo in sight.
double totalSeconds(const Settings& settings, TransportClock clock = TransportClock::unknown());

} // namespace feathers::mod::mseg
