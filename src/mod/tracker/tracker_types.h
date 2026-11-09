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

#include <array>
#include <cstddef>

// THE PITCH TRACKER — the note lane's first caller, and the device that answers an early question
// properly.
//
// Which pitch should a host report for a held chord? The answer is that the host gives every device
// every note and lets each device decide what to do with them. So the
// host reports none, this device decides, and WHICH RULE IT USES IS ONE OF ITS OWN CONTROLS — a
// musical choice living in a patch rather than in a host, where a bass-follower and a lead-follower
// can disagree without either being wrong.
//
// **IT PUBLISHES A MODULATION SOURCE, WHICH IS THE INTERESTING PART.** The note you are playing
// becomes routable to any control in the rack through machinery that already exists: the backplane
// learns nothing new, because a source is a source whether an envelope or a keyboard made it.
//
// IT READS NOTES AND PASSES THEM ON UNCHANGED. That is not a formality — see `NoteOps::processNotes`.
// A tracker that swallowed the stream would silently break every device after it in the chain, and a
// device whose whole job is to observe has no business consuming.

namespace feathers::mod::tracker {

/// WHICH HELD NOTE THE TRACKER FOLLOWS.
///
/// THE THREE ANSWERS A HOST WOULD HAVE HAD TO CHOOSE BETWEEN, offered as a control instead. With one
/// key held they all agree; they differ only in chords, where there is no correct answer and therefore
/// no honest default for a host to impose.
enum class Rule : int {
	Last = 0,  ///< The most recent note down. What a mono synth does and what a played line expects.
	Lowest,    ///< The bottom of the chord. What a bass follower wants.
	Highest,   ///< The top. What a melody follower wants when the left hand is holding pads.
};

enum class Param : size_t {
	Rule = 0,
	/// **THE NOTE THAT READS ZERO.** A tracker publishing raw MIDI numbers would be unroutable in
	/// practice — every destination would need the same 0..127 idea of range and none of them has one
	/// — so the output is a distance from HERE rather than an absolute pitch.
	///
	RootNote,
	/// **HOW MANY OCTAVES EITHER SIDE OF THE ROOT REACH FULL SCALE**, so the root plus `Spread`
	/// octaves reads +1 and the root minus `Spread` octaves reads −1.
	///
	/// IT REPLACED A LOW/HIGH PAIR, and the pair is what made the old mapping awkward to think about:
	/// two absolute pitches whose midpoint was wherever they happened to sit, so "no modulation" was
	/// not a note you could name. A root and a distance put zero on a note the player chose.
	Spread,
	/// How fast the output moves toward a new pitch, 0..1. Zero is a jump.
	Glide,

	/// **THE BYPASS SWITCH** — see `dahdsr::Param::Enabled`. Declared here for the same reason even
	/// though this device is not yet registered in `kDevices`: the head draws a switch iff the
	/// descriptor declares one, and a device that gains a card later should not need this remembered
	/// then.
	Enabled,
	/// **THE AMOUNT** — one knob scaling the output before every route (backlog DH; see
	/// `dahdsr::Param::Amount`). On a tracker it reads as a keytrack amount: half tracking at 0.5,
	/// inverted below zero. Applied by the host; this device never reads it. Appended.
	Amount,

	kCount,
};

inline constexpr const char* kRuleNames[] = { "Last", "Lowest", "Highest" };

/// Off/On, for the enable switch — the same two words every other device's switch uses.
inline constexpr const char* const kOffOnNames[] = { "Off", "On" };

inline constexpr device::ParamDesc kParamDescs[] = {
	/// STEPPED AND NAMED, because it is a choice between three musical behaviours rather than a
	/// position on a scale — and a host that quantises it is doing the player a favour.
	{ .key = "rule",
	  .name = "Rule",
	  .min = 0.0,
	  .max = 2.0,
	  .def = 0.0,
	  .stepped = true,
	  .unit = device::Unit::Choice,
	  .names = kRuleNames,
	  .modulatable = false,
	  .tooltip = "Which held note it follows — the last played, the lowest or the highest. Devices after this follow its note" },
	/// **60 IS CHOSEN, NOT INHERITED, AND A HOST KEY WOULD REPLACE IT.**
	///
	/// The root was asked to follow the song's key by default. **CLAP has no key-signature
	/// concept** — `clap_event_transport` in the vendored 1.2.10 carries tempo and time signature and
	/// nothing about key, and the only draft near it is about note NAMING. So there is no host key to
	/// read and this default is a decision rather than a reading.
	///
	/// NOR IS IT INHERITED FROM THE FIRMWARE: the reference has no tracker-like source with a root
	/// convention. **It does carry a song key** — `currentSong->key.rootNote`, read by the keyboard
	/// layouts — so the concept asked for is real in the host's own instrument and it is the
	/// TRANSPORT that is silent, not the idea. If CLAP ever carries a key signature, this default is
	/// what it replaces.
	{ .key = "root_note", .name = "Root Note", .min = 0.0, .max = 127.0, .def = 60.0, .stepped = true,
	  .unit = device::Unit::Note, .names = {}, .modulatable = false,
	  .tooltip = "The note the tracker maps to the centre of its output range" },
	/// TWO OCTAVES EITHER SIDE BY DEFAULT, so an ordinary played range fills most of the output
	/// without a hand-played line pinning it at the ends. Never zero: the minimum is one octave,
	/// because a spread of nothing is a divide and a mapping with no width is not a setting anyone
	/// wants — it is the shape of a bug.
	{ .key = "spread", .name = "Spread", .min = 1.0, .max = 5.0, .def = 2.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {}, .modulatable = false,
	  .tooltip = "How many octaves of output a played octave covers — wider spreads the playing range" },
	/// THE ONE MODULATABLE CONTROL. Glide is a performance gesture — the others describe what the
	/// tracker IS, and a mapping range that moved under a held note would make the output meaningless.
	{ .key = "glide", .name = "Glide", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "How long the pitch takes to slide between notes — a portamento" },
	/// ON BY DEFAULT — a modulator added to a chain should modulate.
	{ .key = "enabled",
	  .name = "Enabled",
	  .min = 0.0,
	  .max = 1.0,
	  .def = 1.0,
	  .stepped = true,
	  .unit = device::Unit::Choice,
	  .names = kOffOnNames,
	  .modulatable = true, .tooltip = "Switches the tracker on or off" },
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
	  .tooltip = "How deeply the tracker modulates its destination — the output level of the pitch signal" },
};

/// **ONE OUTPUT, BIPOLAR AND CENTRED ON THE ROOT.** This said the opposite — *"a pitch has a bottom
/// and a top and nothing meaningful at zero"* — and that was true of a Low/High mapping, where zero
/// was whichever note happened to sit at the bottom. With a root it is false: zero IS a note, the one
/// the player named, and a note below it modulates the other way.
///
/// **IT NO LONGER CHANGES NOTHING, AND THE PARAGRAPH THAT SAID SO HAS BEEN CORRECTED RATHER THAN
/// LEFT.** It read: *"`SourceDesc::bipolar` has exactly one reader in the repo —
/// `backplane::SourceView::isBipolarAt` — and that function has no callers... a true statement
/// waiting for a surface to read it."* A surface reads it now: the mod ring draws its reach from the
/// source's polarity, so this declaration is the mechanism and not a note about one. The polarity
/// that PLAYS is still the value below — `ParamSet::rackOffsetAt` is `amount * sourceValue * span`
/// and consults no flag — which is why the two must agree.
///
/// **AND THIS ONE DECLARES NO `bipolarControl`, WHICH IS A STATEMENT RATHER THAN AN OMISSION.** The
/// DAHDSR and the MSEG name their Bipolar switch there because theirs moves; a note below the root
/// modulates the other way ALWAYS, so this output has no switch and its ring straddles the value
/// forever. `kPolarityFixed` means "no control moves this", not "unipolar".
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "pitch", .name = "Pitch", .bipolar = true },
};

inline constexpr const char* kTags[] = { "note", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "TRAK",
	.name = "Tracker",
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

} // namespace feathers::mod::tracker
