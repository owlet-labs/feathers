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
#include "device/registry.h"
#include "mod/arp/humanize.h"
#include "mod/modulator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

// THE CLASSIC ARP — the note lane's first device that PRODUCES notes. It sits in the
// modulator lane, which is the synth's note lane: the devices to its left see the held chord, and the devices to its
// right, then the voices, see its steps.
//
// **TRANSPORT-LOCKED, SO NOTHING ABOUT WHERE IT IS NEEDS STORING**. Grid slot g starts at the song
// position g × the step length (plus Nudge and the Feel) and plays pattern[(g − Offset) mod length]. A scrub, a loop or a
// bounce gives the same notes, and a chord change jumps to the bar position in the new pattern instead of restarting.
// What cannot be derived is stored: Latch's held chord and the free clock's phase (`kBlocks`).

namespace feathers::mod::arp {

/// **WHAT A TRIGGER NOTE DOES TO THE CLOCK** (the Pattern device's Trig, design §7.5): nothing (the transport runs it),
/// gate it (it plays while one is held — the arps' way before 2026-11-05), or restart its cycle on the note's sample.
enum class TrigMode : uint8_t { Transport, Gate, Restart };

/// **RUN — WHEN AN ARP PLAYS**. It replaced the Only w/ Transport switch (`stop_with_transport`,
/// retired and carried: Off is Always, On is Transport).
///   - **Always** — the song's grid while the transport plays; stopped, its own clock from the first key you press.
///   - **Transport** — only while the transport plays (the switch's On, and the default).
///   - **Note** — its own clock at the song's tempo, from step 1 on the first key pressed after every key was let go, for
///     as long as a key is held: a legato key carries on, the last release stops it. The transport is not consulted.
enum class RunMode : uint8_t { Always, Transport, Note };
inline constexpr const char* kRunNames[] = { "Always", "Transport", "Note" };
/// THE SWITCH'S LABELS, CARRIED (`RetiredKey::carriedInto`): a patch that stored Only w/ Transport reads as Run.
inline constexpr device::ChoiceAlias kRunAliases[] = { { "Off", "Always" }, { "On", "Transport" } };

/// THE ORDER THE CHORD IS PLAYED IN. A Choice, so it opens as a list (CLAUDE.md: no multivalue toggles).
enum class Order : uint8_t { Up, Down, UpDown, UpDownRepeat, Played, Random, kCount };
inline constexpr const char* kOrderNames[] = { "Up", "Down", "Up-Down", "Up-Down Repeat", "Played", "Random" };
static_assert(std::size(kOrderNames) == static_cast<size_t>(Order::kCount));

/// HOW OCTAVES ARE WALKED: each note through every octave before the next note, or the whole chord once per octave.
enum class OctaveMode : uint8_t { EachNote, WholeChord, kCount };
inline constexpr const char* kOctaveModeNames[] = { "Each Note", "Whole Chord" };


/// THE LFO/STEP SEQ TIME BASE VOCABULARY, "Hz" first.
inline constexpr const char* kTimeBaseNames[] = {
	"Hz",    "1/64", "1/64.", "1/64T", "1/32", "1/32.", "1/32T", "1/16", "1/16.", "1/16T",
	"1/8",   "1/8.", "1/8T",  "1/4",  "1/4.",  "1/4T",  "1/2",  "1/2.",  "1/2T",
	"1 bar", "1 bar.", "1 barT", "2 bar", "2 bar.", "2 barT", "4 bar", "4 bar.", "4 barT",
};
inline constexpr size_t kTimeBaseCount = std::size(kTimeBaseNames);
static_assert(kTimeBaseCount == device::kDivisionCount + 1, "Hz, then every division in device.h's order");
/// 1/16 — the arp a hand assumes.
inline constexpr size_t kDefaultTimeBase = 7;
static_assert(device::kDivisionBeats[kDefaultTimeBase - 1] == 0.25);

inline constexpr auto& kOffOnNames = mod::kOffOnNames;

/// THE MOST KEYS IT HOLDS, and so the longest pattern: 16 keys × 4 octaves, twice for Up-Down Repeat.
inline constexpr size_t kMaxHeld = 16;
inline constexpr int kMaxOctaves = 4;
inline constexpr size_t kMaxPattern = kMaxHeld * static_cast<size_t>(kMaxOctaves) * 2;

/// ORDER IS THE DESCRIPTOR ORDER AND THE ROW ORDER; a patch stores each by its KEY, so it is free to move.
enum class Param : int {
	Enabled,
	TimeBase,
	Rate,
	Order,
	Octaves,
	OctaveMode,
	Gate,
	Feel,
	FeelAmount,
	FreePhase, // (backlog QA: Free's own position, unbounded — the Pattern device's)
	Wobble, // (zone 2, design §12-ter: the Pattern device's, every feel)
	FixedVelocity,
	Velocity,
	Latch,
	Offset,
	Nudge,
	Run,
	Seed,
	kCount,
};
[[nodiscard]] constexpr size_t at(Param p) { return static_cast<size_t>(p); }
inline constexpr size_t kParamCount = static_cast<size_t>(Param::kCount);

/// THE GATES (CLAUDE.md's honesty rule): Seed draws only under Random; the fixed Velocity is read only under Fixed;
/// the Octave mode means nothing with one octave.
inline constexpr uint32_t kRandomOrder = 1u << static_cast<unsigned>(Order::Random);
inline constexpr uint32_t kFixedVelocity = 1u << 1;
inline constexpr uint32_t kSeveralOctaves = (1u << 2) | (1u << 3) | (1u << 4);

/// **THE FEEL, WHERE SWING WAS**: SKLITTER's
/// own groove modes, in its order — Swing, Groove and Free on the quarters, eighths or sixteenths — through its warp
/// (`fx/sklitter/groove.h`, by way of `humanize::shiftSteps`, which the Pattern device's Humanize uses too). A Choice and an amount, as SKLITTER's Groove: an amount never rolls into another feel.
inline constexpr const char* kFeelNames[] = { "Straight", "Swing 4",  "Swing 8", "Swing 16", "Groove 4",
	                                          "Groove 8", "Groove 16", "Free 4", "Free 8",  "Free 16" };
inline constexpr size_t kFeelCount = std::size(kFeelNames);
inline constexpr uint32_t kFeelOn = ((1u << kFeelCount) - 1u) & ~1u;
/// THE FEELS THAT READ FEEL AMOUNT AS A DEPTH (every one but Straight and the Free three, which read Free Phase).
inline constexpr uint32_t kFeelDepth = kFeelOn & ~humanize::kFreeModes;

/// THE LONGEST GATE: two steps, so a note can overlap the next one by a whole step. Above 100% the notes tie.
inline constexpr double kGateMax = 2.0;
/// THE OFFSET'S REACH, in whole steps either way. A pattern of more steps than this is still reachable, because the
/// offset rotates the pattern modulo its length.
inline constexpr double kOffsetReach = 16.0;

/// EVERY CONTROL IS MODULATABLE. A stepped one snaps, so a modulated Offset still moves in whole
/// steps.
inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Switches the arp on or off. Off, the notes you play pass straight through" },
	{ .key = "time_base", .name = "Time Base", .min = 0.0, .max = static_cast<double>(kTimeBaseCount) - 1.0,
	  .def = static_cast<double>(kDefaultTimeBase), .stepped = true, .unit = device::Unit::Choice,
	  .names = kTimeBaseNames, .modulatable = true,
	  .tooltip = "The musical division one step lasts, or Hz to make Rate a step frequency unsynced to the tempo" },
	{ .key = "rate", .name = "Rate", .min = kTimeLog2SecondsMin, .max = kTimeLog2SecondsMax, .def = 0.0,
	  .stepped = false, .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,
	  .tooltip = "How fast the steps go: a multiple of the division, or steps per second when Time Base is Hz" },
	{ .key = "order", .name = "Order", .min = 0.0, .max = static_cast<double>(Order::kCount) - 1.0, .def = 0.0,
	  .stepped = true, .unit = device::Unit::Choice, .names = kOrderNames, .modulatable = true,
	  .tooltip = "The order the held notes play in. Up-Down Repeat plays the top and bottom notes twice; Played keeps "
	             "the order you pressed them; Random picks one each step from the Seed" },
	{ .key = "octaves", .name = "Octaves", .min = 1.0, .max = static_cast<double>(kMaxOctaves), .def = 1.0,
	  .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "How many octaves up the pattern climbs" },
	{ .key = "octave_mode", .name = "Octave Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOctaveModeNames, .modulatable = true,
	  .tooltip = "Each Note: every note climbs its octaves before the next. Whole Chord: the chord plays once per octave",
	  .gateControl = static_cast<int16_t>(Param::Octaves), .gateMask = kSeveralOctaves, .shortName = "Oct Mode" },
	{ .key = "gate", .name = "Gate", .min = 0.01, .max = kGateMax, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "How much of a step each note lasts. Over 100% it overlaps the next note — a tie" },
	{ .key = "feel", .name = "Feel", .min = 0.0, .max = static_cast<double>(kFeelCount) - 1.0, .def = 0.0,
	  .stepped = true, .unit = device::Unit::Choice, .names = kFeelNames, .modulatable = true,
	  .tooltip = "The bar's feel: Swing delays the off-beats (the MPC's), Groove leans them by a phi template, Free walks "
	             "a phase through it",
	  // DRAWN AS SKLITTER'S GROOVE IS: one zone-knob cell, the
	  // feel on its ZoneSelect and Feel Amount its box — still two params, so an amount never rolls into another feel.
	  .modeCell = true,
	  .choiceAliases = humanize::kFeelAliases },
	{ .key = "feel_amount", .name = "Feel Amount", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "How much of the feel: at 100% Swing's off-beats land 74.5% of the way through their pair",
	  .gateControl = static_cast<int16_t>(Param::Feel), .gateMask = kFeelDepth, .shortName = "Amount" },
	// FREE'S PHASE (backlog QA): the Pattern device's `free_phase`, so the two arps' Free is one control.
	{ .key = "free_phase", .name = "Free Phase", .min = -humanize::kFreePhaseReach, .max = humanize::kFreePhaseReach,
	  .def = 0.0, .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .offsetsMayLeaveRange = true,
	  .tooltip = "Where along Free's walk of phi grooves: 0 is straight, every other position a different groove, and "
	             "no end — it never comes back round. Drag a digit to move that place",
	  .modSpan = 1.0, .gateControl = static_cast<int16_t>(Param::Feel), .gateMask = humanize::kFreeModes,
	  .shortName = "Phase", .digitBox = true },
	{ .key = "wobble", .name = "Wobble", .min = -humanize::kFreePhaseReach, .max = humanize::kFreePhaseReach, .def = 0.0,
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true, .offsetsMayLeaveRange = true,
	  .tooltip = "A second groove, a bit whacky: each step pushed or dragged, rushed or dragged in runs, ghosted or rung, "
	             "clipped short. 0 is none, every other position another, and no end. Under every feel",
	  .modSpan = 1.0, .shortName = "Wobble", .digitBox = true },
	/// **A SWITCH NAMED FOR WHAT IT TURNS ON**, not a Played/Fixed choice: a two-way choice whose words are not Off/On
	/// prints its VALUE on a chip ("Played"), which does not say what it toggles (2026-10-30, from the window's capture).
	{ .key = "fixed_velocity", .name = "Fixed Velocity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Off: each step has the velocity its key was played with. On: every step has the Velocity below",
	  .shortName = "Fixed Vel" },
	{ .key = "velocity", .name = "Velocity", .min = 0.0, .max = 1.0, .def = 0.8, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "The velocity every step has while Fixed Velocity is on",
	  .gateControl = static_cast<int16_t>(Param::FixedVelocity), .gateMask = kFixedVelocity },
	{ .key = "latch", .name = "Latch", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Keeps playing the chord after you let go. A new chord, played after letting go of every key, "
	             "replaces it" },
	{ .key = "offset", .name = "Offset", .min = -kOffsetReach, .max = kOffsetReach, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Shifts the pattern against the song's grid by whole steps" },
	{ .key = "nudge", .name = "Nudge", .min = -0.5, .max = 0.5, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Moves every step earlier or later by up to half a step", .centred = true },
	{ .key = "run", .name = "Run", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kRunNames, .modulatable = false, // a run mode is chosen, not swept
	  .tooltip = "When it plays. Always: on the song's grid while the transport plays, and with it stopped on its own clock "
	             "from the first key you press. Transport: only while the transport plays. Note: from step 1 on the first "
	             "key after every key was let go, at the song's tempo, for as long as a key is held (a legato key carries on)",
	  .choiceAliases = kRunAliases },
	{ .key = "seed", .name = "Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Which dice Random rolls. The same seed at the same song position always picks the same note",
	  .gateControl = static_cast<int16_t>(Param::Order), .gateMask = kRandomOrder },
};
static_assert(std::size(kParamDescs) == kParamCount);

/// THE MODULATION SOURCES: where the step is in the pattern, and the note it is playing.
inline constexpr size_t kStepOutput = 0;
inline constexpr size_t kPitchOutput = 1;
inline constexpr size_t kVelocityOutput = 2;
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "step", .name = "Step", .bipolar = false },
	{ .key = "pitch", .name = "Pitch", .bipolar = false },
	{ .key = "velocity", .name = "Velocity", .bipolar = false },
};

/// RETIRED — never reuse.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "velocity_mode", "2026-10-30",
	  "a Played/Fixed choice became the Off/On switch fixed_velocity, so its chip names what it toggles" },
	{ "swing", "2026-10-30",
	  "the swing knob is replaced by the bar feel (feel + feel_amount), shared with the Pattern device's Humanize" },
	{ "free_fine", "2026-11-07",
	  "a second zone was meant as orthogonal phi fields for more nuanced control, not finer steps" },
	{ "stop_with_transport", "2026-11-07",
	  "the Only w/ Transport switch became Run (Always, Transport, Note) — its Off and On are carried",
	  "run" },
};

/// "note-effect" puts it in Feathers' modulation-rack picker beside the modulators (`belongsInModulationRack`, 2026-11-04:
/// it was "modulator" until the Note FX category gave the arps their own heading); the browser
/// groups by the FRONT tag, which stays "note".
inline constexpr const char* kTags[] = { "note", "sequencer", "arpeggiator", "note-effect" };

inline constexpr device::Traits kTraits {
	.tag = "ARP",
	.name = "Arp",
	.hasCard = true,
	.hasExpanded = true,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
};

/// **WHAT CANNOT BE DERIVED, SAVED WITH THE PATCH** (CLAUDE.md, "random and captured state persists"): Latch's held
/// chord, and the free clock's phase while it runs. Everything else is the song position's. In 16-bit words
/// (`device::BlockWriter`): a flag for "latched", the count, then per held note its key, channel, velocity and press
/// order; then a flag for "free clock running" and its phase in steps.
inline constexpr uint32_t kHeldWords = 1 + 1 + 4; // key, channel, velocity (f32), order (u32)
inline constexpr uint32_t kPlayWords = 1 + 1 + static_cast<uint32_t>(kMaxHeld) * kHeldWords + 1 + 4;
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "arp_play", .floats = kPlayWords },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock };

extern const device::ModOps kModOps;
extern const device::NoteOps kNoteOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.state = &kStateOps,
	.category = device::Category::Notes,
};

/// EVERYTHING THE ARP READS, decoded from the row.
struct Settings {
	bool enabled = true;
	size_t timeBase = kDefaultTimeBase;
	float rate = 0.0f;
	Order order = Order::Up;
	int octaves = 1;
	OctaveMode octaveMode = OctaveMode::EachNote;
	float gate = 0.5f;
	size_t feel = 0;
	float feelAmount = 0.5f;
	/// FREE'S PHASE AS STORED (backlog QA): unbounded; `Arp::feelOf` reads it.
	double freePhase = 0.0;
	/// WOBBLE (zone 2, design §12-ter): unbounded, every feel.
	double wobble = 0.0;
	bool fixedVelocity = false;
	float velocity = 0.8f;
	bool latch = false;
	int offset = 0;
	float nudge = 0.0f;
	/// TRANSPORT BY DEFAULT, now one of three (`RunMode`).
	RunMode run = RunMode::Transport;
	int seed = 0;

	[[nodiscard]] bool synced() const noexcept { return timeBase > 0; }
	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, kParamCount>;
[[nodiscard]] Row rowOf(const Settings& s);
/// A short row reads its missing controls at their declared defaults; anything out of range is clamped home.
[[nodiscard]] Settings settingsOf(const float* fields, size_t fieldsPresent);

} // namespace feathers::mod::arp
