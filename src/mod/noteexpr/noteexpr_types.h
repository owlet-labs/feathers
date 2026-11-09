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

// THE NOTE EXPRESSIONS MODULATOR — seven per-note controllers, made routable.
//
// **Decided 2026-09-10: build a note-expressions modulator that exposes mod lanes publishing to each voice,
// with no defaults — a mod in the patch handles the mapping.** So this device maps nothing. It publishes what the host sent and a patch decides
// where each one goes — which is backlog DS's own unfinished sentence answered by moving the decision
// rather than making it. DS declined to wire the other six because each *"needs a decision about what
// it maps to on THIS instrument ... wiring them blind would be inventing behaviour"*. A routable
// source is not a mapping; it is the absence of one, offered to the player.
//
// **SEVEN, BECAUSE CLAP DEFINES SEVEN.** `clap/events.h`: VOLUME, PAN, TUNING, VIBRATO, EXPRESSION,
// BRIGHTNESS, PRESSURE — ids 0..6, and the output index IS the CLAP id so nothing maps between two
// numberings. `device::kMaxDeviceOutputs` was raised from 4 to exactly this; the edges doc §6 named
// that constant as the thing that had stopped this device existing. (It is a bound of 64 since 2026-11-07, with each
// device's output storage sized from its own count.)
//
// **IT WATCHES THE LANE, IT DOES NOT INTERCEPT IT.** `processNotes` passes every event through
// untouched — this device's whole output is an observation. A device that WANTS to change an
// expression can (that is reading (2), live since 2026-09-02 for notes and 2026-09-10 for
// expressions); this one has no reason to, and a passthrough that is honest about being one is
// cheaper to reason about than a rule nobody asked for.
//
// **WHY IT IS ONE DEVICE AND NOT SEVEN OUTPUTS ON THE TRACKER**, which is the placement question
// backlog DU left open and answered with a recommendation: the Tracker's rule and root and spread are
// *about pitch*, so hanging five more outputs on it would make it two devices in a trench coat. This
// keeps the Tracker about pitch.

namespace feathers::mod::noteexpr {

/// **HOW A CHORD BECOMES ONE NUMBER — the collapse rule, and it belongs to the SOURCE.**
///
/// Backlog DU: *"THE COLLAPSE RULE BELONGS TO THE SOURCE, NOT THE DESTINATION. The SOURCE knows what
/// its values mean — 'pressure' collapses to last-note sensibly and 'sum' would be nonsense."* This
/// is that rule, made a control so the player can move it rather than a constant somebody chose.
///
/// **AND IT EXISTS BECAUSE NOTHING READS PER VOICE YET.** Every modulation destination in the plugin
/// — rack controls, fx controls, the synth's own params — resolves once per block; `modulatedRowAt`
/// and `modulatedRackRowAt` are both walked per slot per block. So a per-note source has to answer
/// with ONE number today, and this says which. When a destination migrates to per-voice it reads the
/// voice's own value and this rule stops applying to it — which is DU's *"default trivial handling
/// until they migrate"*, from the source's side.
enum class Rule : int {
	/// The most recent note still held. The Tracker's default and `busNoteContext`'s own choice, so
	/// two sources following "the note" follow the same one.
	Last = 0,
	/// The lowest note still held.
	Lowest,
	/// The highest note still held.
	Highest,
	/// The largest value any held note is carrying. Useful for pressure on a chord, where the player's
	/// intent is usually "how hard am I leaning" rather than "how hard is this one finger".
	Max,
};

inline constexpr const char* kRuleNames[] = { "Last", "Lowest", "Highest", "Max" };

inline constexpr const char* kEnabledNames[] = { "Off", "On" };

enum class Param : size_t {
	/// WHICH NOTE'S EXPRESSIONS ARE PUBLISHED — see `Rule`.
	Rule = 0,
	/// **THE BYPASS SWITCH**, declared for the reason `dahdsr::Param::Enabled` is: the head's square
	/// is a control like any other, and a device whose bypass lived outside its parameter list would
	/// be a device the row could not draw generically.
	Enabled,
	/// **THE AMOUNT** — one knob scaling every expression's output before every route (backlog DH;
	/// see `dahdsr::Param::Amount`), the per-voice runs included. Applied by the host; this device never
	/// reads it. Appended.
	Amount,
	Count,
};

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "rule",
	  .name = "Rule",
	  .min = 0.0,
	  .max = static_cast<double>(std::size(kRuleNames) - 1),
	  .def = 0.0,
	  .stepped = true,
	  .unit = device::Unit::Choice,
	  .names = kRuleNames,
	  /// **NOT MODULATABLE, AND THAT IS THE HONESTY RULE RATHER THAN AN OVERSIGHT.** A source whose
	  /// own selection rule could be modulated by another source is a knot nobody asked for, and the
	  /// rule is a statement about what the player means rather than a value that moves.
	  .modulatable = false,
	  .tooltip = "How note gestures map to the seven expression outputs" },
	{ .key = "enabled",
	  .name = "Enabled",
	  .min = 0.0,
	  .max = 1.0,
	  .def = 1.0,
	  .stepped = true,
	  .unit = device::Unit::Choice,
	  /// **AN ON/OFF SWITCH IS A CHOICE WITH TWO ENTRIES**, which the head's square reads
	  /// and what `test_gui_widgets` requires of any control named by `Traits::enableControl`.
	  .names = kEnabledNames,
	  .modulatable = true,
	  .tooltip = "Switches note expression routing on or off" },
	/// See `Param::Amount`. Modulatable.
	{ .key = "amount",
	  .name = "Amount",
	  .min = -1.0,
	  .max = 1.0,
	  .def = 1.0,
	  .stepped = false,
	  .unit = device::Unit::Percent,
	  .names = {},
	  .modulatable = true,
	  .tooltip = "How deeply the note expressions modulate their destinations" },
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::Count),
              "every Param needs a descriptor and vice versa");

/// **ONE OUTPUT PER CLAP EXPRESSION, IN CLAP'S OWN ORDER — the index IS the id.**
///
/// Named after what the host calls them rather than after what they might do here, which is the whole
/// posture of this device: no defaults; modulation in the patch handles it. A player routing Pressure
/// somewhere is told what the source IS, not what we think it is for.
///
/// **THE POLARITIES ARE CLAP'S, READ FROM ITS OWN DOCUMENTATION rather than guessed.** VOLUME is a
/// gain in 0..4 with 1 as unity, PAN is 0..1 with 0.5 centred, TUNING is semitones either side of the
/// note, and the remaining four are 0..1. Only TUNING is genuinely bipolar in the sense a mod ring
/// means it — a value that straddles zero — so it is the only one that says so.
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "vol", .name = "Vol", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "pan", .name = "Pan", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "tune", .name = "Tune", .bipolar = true, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "vib", .name = "Vib", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "expr", .name = "Expr", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "brite", .name = "Brite", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
	{ .key = "press", .name = "Press", .bipolar = false, .bipolarControl = device::kPolarityFixed, .perVoice = true },
};

/// **HOW MANY, AS A NUMBER THIS LAYER OWNS.**
///
/// **THIS FILE DOES NOT INCLUDE `clap/events.h` AND MUST NOT.** `src/device/` and `src/mod/` are the
/// portable layer — the one the standard is meant to be able to ship — and a device that included a
/// host's API would make every device that followed it do the same. The first draft of this file did
/// include it, and the build refused: `feathers-mod` has no clap include path, which is that posture
/// enforced by the link rather than by a comment.
///
/// **SO THE CROSS-CHECK LIVES WHERE THE TWO MEET.** `plugin/device_registry.h` sees both this and
/// CLAP, and asserts there that these seven are CLAP's seven. The number is stated once here and
/// checked once there; neither file has to include the other's world.
inline constexpr size_t kOutputCount = std::size(kOutputs);

static_assert(kOutputCount <= device::kMaxDeviceOutputs,
              "the source axis must admit every expression; see device::kMaxDeviceOutputs");

inline constexpr const char* kTags[] = { "note", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "NEXP",
	.name = "Note Expressions",
	.hasCard = true,
	.hasExpanded = false,
	/// THE HEAD'S SQUARE. See `Param::Enabled`.
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = static_cast<int>(Param::Amount),
	.tags = kTags,
};

extern const device::NoteOps kNoteOps;
extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::noteexpr
