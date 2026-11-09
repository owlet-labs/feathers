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
#include "mod/lfo/lfo_types.h"
#include "mod/modulator.h"
#include "mod/trig.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// SAMPLE & HOLD (backlog MW), with a built-in noise source and a trigger threshold.
//
// **WHAT IT DOES, IN ONE ORDER.** On every CLOCK it draws a uniform for Probability and the next value of its noise
// (the noise advances on every clock whether or not the sample is taken, so Probability never moves the noise in
// time); if the uniform is under Probability it TAKES the candidate — the noise, or the Input control as the rack
// resolved it — quantizes it, holds it, and emits one tick of 1 on its Trig Out. The held value then glides there
// through Slew, a one-pole, and leaves scaled by Amount. Trig Out is emitted unsmoothed and unscaled whatever Slew
// and Amount say (`mod::kTrigSmoothingRule`): it is a pulse for a Trig, and a Trig fires at 0.5.
//
// **THE CLOCK** is the device's own Rate (Hz, or a division of the tempo in the LFO's own Time Base vocabulary), each
// note's start, its Trig only, or Random — Poisson events at Rate's mean from `dsp::RandomEvents`, the core NOISE's
// Impulses and NA's Random Trigger share. **THE TRIG FIRES A CLOCK UNDER EVERY CLOCK SETTING**, so the pad samples
// by hand on any of them; `Trig` as the Clock means the Trig and nothing else.
//
// **POLY: ONE HOLD PER NOTE** — each note has its own clock phase, stream, edge and held value; Input and Trig are
// resolved per note when a per-voice source reaches them (`ModTickContext::perNote`).

namespace feathers::mod::samplehold {

/// WHAT IS SAMPLED. The three colours are of the HELD SEQUENCE — how one sample relates to the next — not of a
/// spectrum running in time, so a brown S&H wanders and a white one jumps at whatever Rate it is clocked.
enum class Source : uint8_t { White, Pink, Brown, Input, kCount };
inline constexpr const char* kSourceNames[] = { "White", "Pink", "Brown", "Input" };
static_assert(std::size(kSourceNames) == static_cast<size_t>(Source::kCount));

/// WHAT TAKES A SAMPLE.
enum class Clock : uint8_t { Rate, Note, Trig, Random, kCount };
inline constexpr const char* kClockNames[] = { "Rate", "Note", "Trig", "Random" };
static_assert(std::size(kClockNames) == static_cast<size_t>(Clock::kCount));

/// STABLE (the staircase restarts from Seed at reset, at the transport's start and, under Poly, per note by its
/// ordinal since then) OR FREE (it never restarts). NOISE's two words for the same choice.
enum class SeedMode : uint8_t { Stable, Free, kCount };
inline constexpr const char* kSeedModeNames[] = { "Stable", "Free" };

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// **THE LFO'S TIME BASE VOCABULARY, LESS ITS LAST ENTRY.** "Hz" and every division are the LFO's own labels, so the
/// two devices store and print the same words; "Note" is left off because the LFO's is a placeholder (its rate is
/// not plumbed, `lfo::kNoteSyncPlaceholder`) and a clock that does nothing would break the honesty rule.
inline constexpr size_t kTimeBaseCount = lfo::kTimeBaseCount - 1;
inline constexpr std::span<const char* const> kTimeBaseNames { lfo::kTimeBaseNames, kTimeBaseCount };
static_assert(kTimeBaseCount == device::kDivisionCount + 1, "Hz, then every division");

/// THE TOP OF QUANTIZE: steps per unit of output, so 12 at full route depth over an octave is semitones.
inline constexpr int kQuantizeMax = 48;

/// ORDER IS THE ROW ORDER AND THE DESCRIPTOR ORDER.
enum class Param : size_t {
	Source,
	/// THE VALUE SAMPLED UNDER Source = Input — rests at 0 and takes a route, as MATH's A does.
	Input,
	Clock,
	TimeBase,
	Rate,
	Slew,
	Quantize,
	Probability,
	SeedMode,
	Seed,
	Bipolar,
	/// ONE HOLD PER NOTE, or one for the device (`mod::PerNoteTable`). Off by default, as the LFO's.
	Poly,
	/// **INSIDE THE DEVICE, NOT THE HOST'S** — it scales the held output and never Trig Out, so it is not named as
	/// `amountControl` (the host would scale the pulse with it, and a pulse at 0.4 fires nothing).
	Amount,
	Enabled,
	/// **TRIG — TAKES A SAMPLE** (backlog MV): fired at 0.5, re-armed below 0.25 (`mod/trig.h`).
	Trig,

	kCount,
};

[[nodiscard]] constexpr uint32_t bit(auto v) noexcept {
	return 1u << static_cast<uint32_t>(v);
}
/// THE CLOCKS THAT RUN AT RATE — Rate's period, Random's mean.
inline constexpr uint32_t kRatedClocks = bit(Clock::Rate) | bit(Clock::Random);

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "source", .name = "Source", .min = 0.0, .max = static_cast<double>(Source::kCount) - 1.0, .def = 0.0,
	  .stepped = true, .unit = device::Unit::Choice, .names = kSourceNames, .modulatable = false,
	  .tooltip = "What is sampled: built-in noise — White jumps anywhere, Pink mostly stays near, Brown wanders — or "
	             "Input, whatever is routed into the Input knob" },
	{ .key = "input", .name = "Input", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "The value sampled when Source is Input — rests at 0; route a source into it",
	  .centred = true },
	{ .key = "clock", .name = "Clock", .min = 0.0, .max = static_cast<double>(Clock::kCount) - 1.0, .def = 0.0,
	  .stepped = true, .unit = device::Unit::Choice, .names = kClockNames, .modulatable = false,
	  .tooltip = "What takes a sample: Rate, each note's start, the Trig alone, or Random hits at Rate's average. "
	             "The Trig takes one under every clock" },
	{ .key = "time_base", .name = "Time Base", .min = 0.0, .max = static_cast<double>(kTimeBaseCount) - 1.0,
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kTimeBaseNames, .modulatable = true,
	  .tooltip = "Hz makes Rate a frequency; a division locks it to the tempo (and to the song while it plays)",
	  .gateControl = static_cast<int16_t>(Param::Clock), .gateMask = kRatedClocks,
	  .choiceAliases = lfo::kTimeBaseAliases },
	{ .key = "rate", .name = "Rate", .min = kTimeLog2SecondsMin, .max = kTimeLog2SecondsMax, .def = 0.0,
	  .stepped = false, .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,
	  .tooltip = "How often it samples — samples per second under Hz, or a multiple of the division; under Random "
	             "the average",
	  .gateControl = static_cast<int16_t>(Param::Clock), .gateMask = kRatedClocks },
	{ .key = "slew", .name = "Slew", .min = kSmoothingLog2SecondsMin, .max = kSmoothingLog2SecondsMax,
	  .def = kSmoothingLog2SecondsMin, .stepped = false, .unit = device::Unit::LogSeconds, .names = {},
	  .modulatable = true,
	  .tooltip = "Glides to each new value with this time constant instead of jumping; at the bottom it is off" },
	{ .key = "quantize", .name = "Quantize", .min = 0.0, .max = static_cast<double>(kQuantizeMax), .def = 0.0,
	  .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = false,
	  .tooltip = "Rounds each sample to steps of 1/N of full scale — 12 over an octave route is semitones; off is "
	             "continuous",
	  .offAtEnd = -1 },
	{ .key = "probability", .name = "Probability", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "The chance each clock takes a new sample; otherwise it goes on holding" },
	{ .key = "seed_mode", .name = "Seed Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kSeedModeNames, .modulatable = false,
	  .tooltip = "Stable: the same staircase from the Seed each time the transport starts (and per note under Poly). "
	             "Free: it never restarts. Both play the Seed's staircase; on a playing transport a synced Free step "
	             "takes the Seed's sample for that step of the song" },
	{ .key = "seed", .name = "Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {}, .modulatable = false,
	  .tooltip = "Which staircase plays — under Stable and Free alike" },
	{ .key = "bipolar", .name = "Bipolar", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
	  .tooltip = "On: the held value swings -1 to +1. Off: 0 to 1" },
	{ .key = "poly", .name = "Poly", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
	  .tooltip = "On: every note holds its own value on its own clock. Off: one hold every note shares" },
	{ .key = "amount", .name = "Amount", .min = -1.0, .max = 1.0, .def = 1.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Scales the held output. Trig Out is not scaled: it is a pulse to 1 for a Trig" },
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Bypass switch — off: both outputs rest at 0" },
	mod::trigParamDesc("trig", "Trig",
	                   mod::trigTooltip<"Takes a sample: it fires when this rises past half and again only after it "
	                                    "falls below a quarter. Route a trigger here, or click to sample by hand.">()),
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE TWO OUTPUTS — the held value, and one tick of 1 for every sample taken. BOTH PER VOICE: under Poly a voice
/// reads its own note's.
enum class Output : size_t { Held, TrigOut, kCount };
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "held", .name = "S&H", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar),
	  .perVoice = true },
	{ .key = "trig_out", .name = "Trig Out", .bipolar = false, .perVoice = true },
};
static_assert(std::size(kOutputs) == static_cast<size_t>(Output::kCount));

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

struct Settings {
	/// WHICH CONTROL IS THE TRIG, AND WHICH IS THE INPUT — the two the per-note resolve reads.
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);
	static constexpr size_t kInputControl = static_cast<size_t>(Param::Input);

	Source source = Source::White;
	float input = 0.0f;
	Clock clock = Clock::Rate;
	size_t timeBase = 0;
	float timeScale = 0.0f; ///< 1 Hz under Hz, one division synced — the LFO's default
	float slew = kSmoothingLog2SecondsMin;
	int quantize = 0;
	float probability = 1.0f;
	SeedMode seedMode = SeedMode::Stable;
	int seed = 0;
	bool bipolar = true;
	bool poly = false;
	float amount = 1.0f;
	bool enabled = true;
	float trig = 0.0f;

	[[nodiscard]] bool synced() const noexcept { return timeBase > 0; }
};

inline constexpr Settings defaults() { return Settings {}; }

inline constexpr const char* kTags[] = { "random", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "SNH",
	.name = "Sample & Hold",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
};

extern const device::ModOps kModOps;
extern const device::NoteOps kNoteOps;

/// **THE ONE HOLD, SAVED WITH THE PATCH** (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS WITH THE PATCH"):
/// the held sample and its slew, the streams' position, the colours' rows and walk, and the note ordinals — so a reload
/// goes on holding what was heard and takes its next sample where the saved one would have. In 16-bit words
/// (`device::BlockWriter`). **WHAT IS NOT SAVED, AND WHY**: a Poly note's own hold ends with its note, and a reload
/// starts no notes; the transport lock's place is the song's, read again on the first playing tick; and whether the
/// transport was playing, so a Stable hold reloaded under a playing transport restarts from its Seed as at any start.
inline constexpr uint32_t kHoldWords = 97;
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "snh_hold", .floats = kHoldWords },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock };

Row rowOf(const Settings& s);
Settings settingsOf(const Row& row, size_t fieldsPresent);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.state = &kStateOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::samplehold
