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
#include <cstdint>

// THE STEP SEQUENCER (backlog MX). Up to 32 steps, each with a value, a gate (On, Off or Tie), a probability, a ratchet count
// and a slew; five directions; a clock from its own Rate (free or synced, the LFO's Time Base vocabulary) or from
// its Trig; a second Trig that resets it. Outputs the step value, a gate and a one-tick trigger (backlog MV).
//
// **THE STEPS ARE ROW STATE, NOT PARAMETERS** (`Traits::extraState`, under the key `steps`). The requirement:
// the generated steps are STORED in the patch, so a reload plays exactly what was heard. The seed alone is
// not enough: a reseed must not silently change a saved pattern. So Randomise is an ACT on the message thread
// (`randomise`, below) that writes new steps into the row, one undo step; the Seed decides what the NEXT act and the
// device's own dice draw (probability, the random directions, the Turing machine), and never moves a stored step.
//
// **TWO FLOATS A STEP, BECAUSE THE ROW HAS 128.** Six fields at 32 steps would be 192 floats, over
// `device::kMaxDeviceFields`. The value rides as itself; the other five are packed into one integer-valued float
// (`packStep`), exact below 2^24. ZERO IS THE DEFAULT STEP — gate On, 100%, one hit, no slew, unlocked — because a
// fresh row's extra state is all zeros, and that must be a pattern that plays.

namespace feathers::mod::stepseq {

inline constexpr size_t kMaxSteps = 32;
inline constexpr size_t kDefaultSteps = 16;
/// THE MOST HITS A RATCHET SUBDIVIDES A STEP INTO.
inline constexpr int kMaxRatchet = 8;

/// A STEP'S GATE. ON plays a note; OFF is a rest; TIE continues the step before it — no trigger, the gate held across
/// the boundary — and takes its own value (with its slew, a slide). Choices stored as a code in the packed word.
enum class Gate : uint8_t { On, Off, Tie, kCount };
inline constexpr const char* kGateNames[] = { "On", "Off", "Tie" };

enum class Direction : uint8_t { Forward, Backward, PingPong, Random, RandomWalk, kCount };
inline constexpr const char* kDirectionNames[] = { "Forward", "Backward", "Ping-Pong", "Random", "Random Walk" };
static_assert(std::size(kDirectionNames) == static_cast<size_t>(Direction::kCount));

/// WHAT MOVES IT ON: its own Rate, or only its Trig. Under Rate a Trig still advances it, at once.
enum class Clock : uint8_t { Rate, Trig, kCount };
inline constexpr const char* kClockNames[] = { "Rate", "Trig" };

/// THE LFO'S TIME BASE VOCABULARY, "Hz" first (backlog NJ), without the LFO's "Note" (audio rate is no step rate).
inline constexpr const char* kTimeBaseNames[] = {
	"Hz",    "1/64", "1/64.", "1/64T", "1/32", "1/32.", "1/32T", "1/16", "1/16.", "1/16T",
	"1/8",   "1/8.", "1/8T",  "1/4",  "1/4.",  "1/4T",  "1/2",  "1/2.",  "1/2T",
	"1 bar", "1 bar.", "1 barT", "2 bar", "2 bar.", "2 barT", "4 bar", "4 bar.", "4 barT",
};
inline constexpr size_t kTimeBaseCount = std::size(kTimeBaseNames);
static_assert(kTimeBaseCount == device::kDivisionCount + 1, "Hz, then every division in device.h's order");
/// 1/16 — the step a hand assumes.
inline constexpr size_t kDefaultTimeBase = 7;

using Mode = mod::Mode;
inline constexpr auto& kOffOnNames = mod::kOffOnNames;

/// A STEP'S FIELDS, AS LANES: what the grid's tabs show and a drag paints. EACH HAS ITS OWN RANDOMISE AND ITS OWN TRIG
/// (backlog NT;), so a fire moves its lane whichever tab is shown, or with no editor at all.
enum class Lane : uint8_t { Value, Gate, Probability, Ratchet, Slew, kCount };
inline constexpr const char* kLaneNames[] = { "Value", "Gate", "Prob", "Ratchet", "Slew" };
static_assert(std::size(kLaneNames) == static_cast<size_t>(Lane::kCount));

/// ORDER IS THE DESCRIPTOR ORDER AND THE ROW ORDER; a patch stores each by its KEY, so it is free to move.
enum class Param : int {
	Steps,
	Direction,
	Clock,
	TimeBase,
	Rate,
	GateLength,
	Retrig,
	Poly,
	Bipolar,
	Seed,
	RandomAmount,
	Turing,
	TuringLock,
	Enabled,
	/// ADVANCE — one step on, at once (backlog MV: fires at 0.5, re-arms below 0.25).
	Trig,
	/// RESET — back to the first step, at once. Beats an Advance on the same tick.
	Reset,
	/// **THE OUTPUT AMOUNT** (backlog NT): scales the VALUE output and nothing else — INSIDE the device, as S&H's does,
	/// because the host's `amountControl` would scale the Gate and the Trigger too (see `kTraits`).
	Amount,
	/// **EACH LANE'S RANDOMISE TRIG** (backlog NT), in `Lane` order: a fire randomises exactly that lane as its button does
	/// (the locks, Rand Amt, Seed), on the audio thread, reaching the record through `publishExtraState` with no undo step.
	RandValue,
	RandGate,
	RandProb,
	RandRatchet,
	RandSlew,
	kCount,
};

[[nodiscard]] constexpr size_t at(Param p) { return static_cast<size_t>(p); }

/// THE ROW'S EXTRA STATE: two floats a step, then the Randomise count, then the basis (`ModOps::extraStateBasis`).
inline constexpr size_t kStepFloats = 2;
inline constexpr size_t kGenerationFloat = kMaxSteps * kStepFloats;
inline constexpr size_t kBasisFloat = kGenerationFloat + 1;
inline constexpr size_t kExtraFloats = kBasisFloat + 1;
inline constexpr size_t kParamCount = static_cast<size_t>(Param::kCount);
static_assert(kParamCount + kExtraFloats <= device::kMaxDeviceFields);

/// The row index of step `s`'s value, and of its packed word.
[[nodiscard]] constexpr size_t valueIndex(size_t s) { return kParamCount + s * kStepFloats; }
[[nodiscard]] constexpr size_t wordIndex(size_t s) { return kParamCount + s * kStepFloats + 1; }
[[nodiscard]] constexpr size_t generationIndex() { return kParamCount + kGenerationFloat; }
[[nodiscard]] constexpr size_t basisIndex() { return kParamCount + kBasisFloat; }

/// ONE STEP.
struct Step {
	float value = 0.0f; ///< 0..1
	Gate gate = Gate::On;
	float probability = 1.0f; ///< 0..1, in whole percent
	int ratchet = 1;          ///< 1..kMaxRatchet
	float slew = 0.0f;        ///< 0..1 of the step, in whole percent
	bool locked = false;      ///< kept by Randomise and by the Turing machine

	friend bool operator==(const Step&, const Step&) = default;
};

/// THE PACKED WORD, a whole number below 2^20, so a float carries it exactly: bits 0-6 are 100 minus the probability
/// in percent, 7-13 the slew in percent, 14-16 the ratchet minus one, 17-18 the gate, 19 the lock. Every field reads 0
/// at its default, so 0 is the default step.
[[nodiscard]] float packStep(const Step& s) noexcept;
/// The inverse, from the row's two floats; anything out of range (a hand-edited file, a NaN) is clamped home.
[[nodiscard]] Step unpackStep(float value, float word) noexcept;

/// ALL 32 STEPS AND THE RANDOMISE COUNT — the extra state, decoded.
struct Pattern {
	std::array<Step, kMaxSteps> step {};
	/// HOW MANY TIMES RANDOMISE HAS RUN ON THIS PATTERN — folded into the act's dice, so a second press with the same
	/// seed draws a new pattern rather than the same one again. Stored, so the act is reproducible from the patch.
	uint32_t generation = 0;
	/// **WHOSE WRITING THE STEPS ARE** (`ModOps::extraStateBasis`): a hand's edit and a load write a fresh one, the
	/// device never does — so the device knows to drop its Turing rewrites for a hand's steps, and a snapshot of the
	/// device's taken before that edit is never written over it. Compared to the bit.
	float basis = 0.0f;

	friend bool operator==(const Pattern&, const Pattern&) = default;
};

/// THE DESCRIPTORS.
inline constexpr uint32_t kTuringOn = 1u << 1;
inline constexpr uint32_t kRetrigOn = 1u << 1;
/// RATE IS READ ONLY UNDER CLOCK = RATE — the honesty rule's gate; Time Base with it.
inline constexpr uint32_t kRateClock = 1u << 0;

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "steps", .name = "Steps", .min = 1.0, .max = static_cast<double>(kMaxSteps),
	  .def = static_cast<double>(kDefaultSteps), .stepped = true, .unit = device::Unit::Number, .names = {},
	  .tooltip = "How many steps the pattern has" },
	{ .key = "direction", .name = "Direction", .min = 0.0, .max = static_cast<double>(Direction::kCount) - 1.0,
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kDirectionNames,
	  .tooltip = "The order the steps play in. Random picks any step; Random Walk moves one step either way" },
	{ .key = "clock", .name = "Clock", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kClockNames,
	  .tooltip = "What moves it on. Rate: its own clock, and a Trig also advances it at once. Trig: only the Trig" },
	{ .key = "time_base", .name = "Time Base", .min = 0.0, .max = static_cast<double>(kTimeBaseCount) - 1.0,
	  .def = static_cast<double>(kDefaultTimeBase), .stepped = true, .unit = device::Unit::Choice,
	  .names = kTimeBaseNames, .modulatable = true,
	  .tooltip = "The musical division one step lasts, or Hz to make Rate a step frequency unsynced to the tempo",
	  .gateControl = static_cast<int16_t>(Param::Clock), .gateMask = kRateClock },
	{ .key = "rate", .name = "Rate", .min = kTimeLog2SecondsMin, .max = kTimeLog2SecondsMax, .def = 0.0,
	  .stepped = false, .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,
	  .tooltip = "How fast the steps go: a multiple of the division, or steps per second when Time Base is Hz",
	  .gateControl = static_cast<int16_t>(Param::Clock), .gateMask = kRateClock },
	{ .key = "gate_length", .name = "Gate Length", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "How much of a step (or of each ratchet hit) the gate output stays high" },
	{ .key = "retrig", .name = "Retrig", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames,
	  .tooltip = "On: every note starts the pattern from its first step. Off: it runs free, and on a playing synced "
	             "transport its steps land on the song's grid" },
	{ .key = "poly", .name = "Poly", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames,
	  .tooltip = "On: every note runs its own pattern from its own start. Off: one pattern every note shares. Needs Retrig",
	  .gateControl = static_cast<int16_t>(Param::Retrig), .gateMask = kRetrigOn },
	{ .key = "bipolar", .name = "Bipolar", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames,
	  .tooltip = "Swings the value output between -1 and +1 instead of 0 and 1" },
	{ .key = "seed", .name = "Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {},
	  .tooltip = "Which dice it rolls: what the next Randomise draws, and its probability, random directions and "
	             "Turing changes. Changing it never changes the steps you have" },
	{ .key = "random_amount", .name = "Rand Amount", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {},
	  .tooltip = "How far Randomise moves the unlocked steps of the lane shown: a little is a variation, all the way "
	             "is a fresh pattern",
	  .shortName = "Rand Amt" },
	{ .key = "turing", .name = "Turing", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames,
	  .tooltip = "A Turing machine: as each unlocked step plays, its value may be replaced by a new random one, so the "
	             "loop slowly rewrites itself. Lock says how much is kept" },
	{ .key = "turing_lock", .name = "Lock", .min = 0.0, .max = 1.0, .def = 0.75, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "How much of the Turing loop is kept as it plays: at 100% it repeats exactly, at 0% every step is "
	             "new every time",
	  .gateControl = static_cast<int16_t>(Param::Turing), .gateMask = kTuringOn },
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Switches the sequencer on or off" },
	mod::trigParamDesc("trig", "Advance",
	                   mod::trigTooltip<"Moves to the next step at once: it fires when this rises past half and again "
	                                    "only after it falls below a quarter. Route a clock or a trigger here, or click "
	                                    "to fire by hand.">()),
	mod::trigParamDesc("reset", "Reset",
	                   mod::trigTooltip<"Back to the first step at once, firing as Advance does. Route a bar's trigger "
	                                    "here, or click.">()),
	// **THE VALUE OUTPUT'S AMOUNT** (backlog NT). The
	// LFO's range and default; unity is the output as it was, to the bit.
	{ .key = "amount", .name = "Amount", .min = -1.0, .max = 1.0, .def = 1.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Scales the Value output, and below zero inverts it. Gate and Trigger are not scaled: they are pulses "
	             "for a Trig, which fires at half" },
	mod::trigParamDesc("rand_value", "Randomise Value",
	                   mod::trigTooltip<"Randomises the steps' values as the Value page's button does: it fires when this "
	                                    "rises past half and again only after it falls below a quarter. Route a trigger "
	                                    "here, or click to fire by hand.">()),
	mod::trigParamDesc("rand_gate", "Randomise Gate",
	                   mod::trigTooltip<"Randomises the steps' gates as the Gate page's button does, firing as the other "
	                                    "Trigs do.">()),
	mod::trigParamDesc("rand_prob", "Randomise Prob",
	                   mod::trigTooltip<"Randomises the steps' probabilities as the Prob page's button does, firing as "
	                                    "the other Trigs do.">()),
	mod::trigParamDesc("rand_ratchet", "Randomise Ratchet",
	                   mod::trigTooltip<"Randomises the steps' ratchets as the Ratchet page's button does, firing as the "
	                                    "other Trigs do.">()),
	mod::trigParamDesc("rand_slew", "Randomise Slew",
	                   mod::trigTooltip<"Randomises the steps' slews as the Slew page's button does, firing as the other "
	                                    "Trigs do.">()),
};
static_assert(std::size(kParamDescs) == kParamCount);

/// A LANE'S RANDOMISE TRIG.
[[nodiscard]] constexpr Param randomiseParam(Lane lane) {
	return static_cast<Param>(static_cast<int>(Param::RandValue) + static_cast<int>(lane));
}
inline constexpr size_t kLaneCount = static_cast<size_t>(Lane::kCount);
static_assert(static_cast<int>(Param::RandSlew) - static_cast<int>(Param::RandValue) + 1 == static_cast<int>(kLaneCount));

/// **THE SEVEN PADS' FIRE COUNTS IN ONE WORD** (`ModOps::trigFires`, `Device::fireWord`): Advance in bits 0-7, Reset in
/// 8-11, each lane's Randomise in four bits from 12. A pad looks only for a change, and a Trig can fire at most every
/// other tick (375 a second), so even four bits wrap only past 16 fires between two of a card's looks.
inline constexpr uint32_t kFireAdvanceShift = 0;
inline constexpr uint32_t kFireResetShift = 8;
inline constexpr uint32_t kFireAdvanceMask = 0xFFu;
inline constexpr uint32_t kFireResetMask = 0xFu;
inline constexpr uint32_t kFireRandomMask = 0xFu;
[[nodiscard]] constexpr uint32_t fireRandomShift(Lane lane) {
	return 12u + 4u * static_cast<uint32_t>(lane);
}
static_assert(fireRandomShift(Lane::Slew) + 4u == 32u);

/// THE OUTPUTS: the step's value, its gate, and a one-tick pulse at each hit. ALL PER VOICE under Poly.
inline constexpr size_t kValueOutput = 0;
inline constexpr size_t kGateOutput = 1;
inline constexpr size_t kTriggerOutput = 2;
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "value", .name = "Value", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar),
	  .perVoice = true },
	{ .key = "gate", .name = "Gate", .bipolar = false, .perVoice = true },
	{ .key = "trigger", .name = "Trigger", .bipolar = false, .perVoice = true },
};

inline constexpr const char* kTags[] = { "sequencer", "random", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "STEPSEQ",
	.name = "Step Seq",
	.hasCard = true,
	// NO WINDOW: the card is the whole of it, the grid drawn and edited on its face.
	.hasExpanded = false,
	.extraState = kExtraFloats,
	.extraStateKey = "steps_pattern",
	.enableControl = static_cast<int>(Param::Enabled),
	// NO amountControl: the host would scale the TRIGGER by it too, and a pulse scaled below 0.5 fires nothing.
	.tags = kTags,
};

extern const device::ModOps kModOps;
extern const device::NoteOps kNoteOps;

/// **WHERE THE ONE PATTERN IS PLAYING, SAVED WITH THE PATCH** (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE
/// PERSISTS WITH THE PATCH"): the dice, the step and the step after, the ping-pong heading, how far into the step, the
/// slew's endpoints and the counts — so a reload off the transport goes on from the step it was on with the same dice,
/// rather than from the top. A NEW KEY beside `steps_pattern` (backlog NM prerequisite 3): the pattern is a hand's,
/// this is the device's own. In 16-bit words (`device::BlockWriter`). **WHAT IS NOT SAVED, AND WHY**: on a playing
/// transport the step is the song's (`TransportLock`), restarted on the first playing tick, so a saved place would only
/// be overruled; a Poly note's pattern ends with its note (approved 2026-10-17); the Turing rewrites are already the
/// pattern's (backlog MX).
inline constexpr uint32_t kPlayWords = 47;
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "steps_play", .floats = kPlayWords },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock };

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.state = &kStateOps,
	.category = device::Category::Modulators,
};

/// EVERYTHING THE ENGINE READS, decoded from the row.
struct Settings {
	/// THE TWO TRIGS, for `mod::PolyShape`'s per-note resolve (backlog MV, B) and for the Reset beside it.
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);
	static constexpr size_t kResetControl = static_cast<size_t>(Param::Reset);

	size_t steps = kDefaultSteps;
	Direction direction = Direction::Forward;
	Clock clock = Clock::Rate;
	size_t timeBase = kDefaultTimeBase;
	float rate = 0.0f;
	float gateLength = 0.5f;
	bool retrig = true;
	bool poly = false;
	bool bipolar = false;
	int seed = 0;
	float randomAmount = 1.0f;
	bool turing = false;
	float turingLock = 0.75f;
	bool enabled = true;
	float trig = 0.0f;
	float reset = 0.0f;
	/// THE VALUE OUTPUT'S AMOUNT — see `Param::Amount`.
	float amount = 1.0f;
	/// EACH LANE'S RANDOMISE TRIG, in `Lane` order — see `Param::RandValue`.
	std::array<float, static_cast<size_t>(Lane::kCount)> randTrig {};
	Pattern pattern {};

	/// WHAT `mod::PolyShape` READS: Retrig is LFO Retrig (every note restarts it), its absence is Free — under which
	/// PolyShape is mono whatever Poly says, as for every modulator. No smoothing: each step carries its own slew.
	Mode mode = Mode::LfoRetrig;
	float smoothing = kSmoothingLog2SecondsMin;
	bool smoothCarry = false;

	[[nodiscard]] bool synced() const noexcept { return timeBase > 0; }

	friend bool operator==(const Settings&, const Settings&) = default;
};

static_assert(Settings {}.enabled == (kParamDescs[at(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.retrig == (kParamDescs[at(Param::Retrig)].def >= 0.5));
static_assert(Settings {}.poly == (kParamDescs[at(Param::Poly)].def >= 0.5));

/// THE ROW: the controls, then the pattern.
using Row = std::array<float, kParamCount + kExtraFloats>;

[[nodiscard]] Row rowOf(const Settings& s);
/// A short row reads its missing controls at their declared defaults and its missing steps as zero — the default step.
[[nodiscard]] Settings settingsOf(const float* fields, size_t fieldsPresent);

/// The pattern alone, read from and written into a row (or a host's fields, through `fieldAt`).
[[nodiscard]] Pattern patternOf(const float* fields, size_t fieldsPresent);
void writePattern(const Pattern& pattern, float* fields);

/// THE STEP RATE IN Hz from Time Base and Rate, and whatever the host said of the tempo — the LFO's `rateHz`: synced
/// with no tempo, the scale read as Hz.
[[nodiscard]] double stepHz(const Settings& s, double secondsPerBeat) noexcept;

/// **RANDOMISE — THE ACT, NOT THE DICE AT PLAY.** Moves the first `steps` UNLOCKED steps' `lane` by `amount` toward a
/// draw: a continuous field to `v + amount × (draw − v)`, a discrete one (gate, ratchet) re-drawn with probability
/// `amount`. The dice are seeded by (`seed`, the pattern's generation, the lane), and the generation then counts the
/// act — so the same patch and the same press draw the same pattern, and a second press draws a new one. A pure
/// function of its arguments, so a test holds it without a GUI and the card records its before and after.
[[nodiscard]] Pattern randomise(const Pattern& pattern, size_t steps, Lane lane, float amount, int seed) noexcept;

/// A step's field on a lane, as 0..1 (what the grid draws), and setting it from 0..1 (what a drag writes).
[[nodiscard]] float laneValue(const Step& step, Lane lane) noexcept;
void setLaneValue(Step* step, Lane lane, float unit) noexcept;

} // namespace feathers::mod::stepseq
