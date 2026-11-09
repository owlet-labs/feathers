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

#include "device/macro_type.h"
#include "device/registry.h"

#include <cstddef>

// MACROS — FOUR MACRO KNOBS AS A MODULATOR DEVICE (macro-modulation phase B).
//
// **WHAT IT HAS THAT A HEADER MACRO DOES NOT.** Feathers' four header macros and the rack's eight are
// core SOURCES: they publish, and nothing modulates them. These four are ordinary device params, so
// each is a modulation DESTINATION as well as a source — an LFO can ride Macro 2, and every route out
// of Macro 2 rides with it. And a device can be dropped where it is wanted: in Feathers' modulator rail,
// in a rack chain, more than one of them.
//
// **EACH OUTPUT IS ITS MACRO'S RESOLVED VALUE**, the one the engine hands `setFields` after its routes
// have displaced it — `RackEngine::advanceModulators` calls `resolveInto` and then `setFields` with
// `slot.resolved`, the same road every other modulator's controls are modulated by. So a route INTO
// Macro n moves output n in the same pass, and the device does no resolving of its own.
//
// **THE SAME THREE PER-MACRO STATES THE HEADER MACROS HAVE — polarity, type and (in B2) a name** —
// but as params rather than patch state beside them, because here the device's row IS its patch state.

namespace feathers::mod::macros {

inline constexpr size_t kMacroCount = 4;

/// ORDER IS THE ROW ORDER AND THE DESCRIPTOR ORDER: the four values, the four polarities, the four
/// types, the switch.
enum class Param : size_t {
	Macro1 = 0,
	Macro2,
	Macro3,
	Macro4,
	/// **Off: the output is the value, 0..1. On: `2v − 1`, −1..1** — `device::macroToBipolar`, the rule
	/// the header's and the rack's bipolar macros follow. The stored value never changes shape.
	Bipolar1,
	Bipolar2,
	Bipolar3,
	Bipolar4,
	/// **HOW THE CARD'S CONTROL DRIVES THE VALUE** — Knob, Latch or Momentary, `device::MacroType`'s
	/// order. THE DEVICE NEVER READS IT: a Latch or a Momentary is a way for a hand to write 0 or 1, and
	/// what reaches `setFields` is the value either way. It is here so a patch keeps it.
	/// NOT DEAD: the MACROS card (macro-modulation B3) is its reader, and nothing of B1 installs before B3 lands.
	Type1,
	Type2,
	Type3,
	Type4,
	/// **THE BYPASS SWITCH** — see `dahdsr::Param::Enabled`. Read by the host, never by this device: a
	/// disabled modulator publishes its rest value (`RackEngine::advanceModulators`).
	Enabled,

	kCount,
};

[[nodiscard]] constexpr size_t macroParam(size_t n) noexcept { return static_cast<size_t>(Param::Macro1) + n; }
[[nodiscard]] constexpr size_t bipolarParam(size_t n) noexcept { return static_cast<size_t>(Param::Bipolar1) + n; }
[[nodiscard]] constexpr size_t typeParam(size_t n) noexcept { return static_cast<size_t>(Param::Type1) + n; }

inline constexpr const char* const kOffOnNames[] = { "Off", "On" };

/// THE ROW'S DECLARATIONS, four at a time.
///
/// **THE VALUES ARE THE ONLY MODULATABLE CONTROLS.** A polarity or a type that moved under a route would
/// change what every downstream route MEANS mid-note — the same reason the tracker's mapping is fixed.
inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "macro_1", .name = "Macro 1", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "Macro 1's value — what its output publishes", .stereo = true },
	{ .key = "macro_2", .name = "Macro 2", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "Macro 2's value — what its output publishes", .stereo = true },
	{ .key = "macro_3", .name = "Macro 3", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "Macro 3's value — what its output publishes", .stereo = true },
	{ .key = "macro_4", .name = "Macro 4", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "Macro 4's value — what its output publishes", .stereo = true },
	{ .key = "bipolar_1", .name = "Bipolar 1", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Macro 1's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1" },
	{ .key = "bipolar_2", .name = "Bipolar 2", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Macro 2's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1" },
	{ .key = "bipolar_3", .name = "Bipolar 3", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Macro 3's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1" },
	{ .key = "bipolar_4", .name = "Bipolar 4", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "Macro 4's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1" },
	{ .key = "type_1", .name = "Type 1", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = device::kAbsoluteMacroTypeNames, .modulatable = false,
	  .tooltip = "How Macro 1's control plays — a knob turns, a latch toggles, a momentary holds while pressed" },
	{ .key = "type_2", .name = "Type 2", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = device::kAbsoluteMacroTypeNames, .modulatable = false,
	  .tooltip = "How Macro 2's control plays — a knob turns, a latch toggles, a momentary holds while pressed" },
	{ .key = "type_3", .name = "Type 3", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = device::kAbsoluteMacroTypeNames, .modulatable = false,
	  .tooltip = "How Macro 3's control plays — a knob turns, a latch toggles, a momentary holds while pressed" },
	{ .key = "type_4", .name = "Type 4", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = device::kAbsoluteMacroTypeNames, .modulatable = false,
	  .tooltip = "How Macro 4's control plays — a knob turns, a latch toggles, a momentary holds while pressed" },
	/// ON BY DEFAULT — a modulator added to a chain should modulate.
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = true, .tooltip = "Bypass switch — off: every output rests at 0" },
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));
static_assert(std::size(device::kAbsoluteMacroTypeNames) == 3, "Type n's declared range is 0..2 — the MACROS device has no relative road (backlog PQ)");

/// **FOUR OUTPUTS, EACH NAMING ITS OWN POLARITY SWITCH** — so the mod ring draws each route's reach from
/// the switch that actually moves it, the way the LFO's and the MSEG's do.
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "macro_1", .name = "Macro 1", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar1),
	  .pairFrom = 1u << static_cast<unsigned>(Param::Macro1) },
	{ .key = "macro_2", .name = "Macro 2", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar2),
	  .pairFrom = 1u << static_cast<unsigned>(Param::Macro2) },
	{ .key = "macro_3", .name = "Macro 3", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar3),
	  .pairFrom = 1u << static_cast<unsigned>(Param::Macro3) },
	{ .key = "macro_4", .name = "Macro 4", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar4),
	  .pairFrom = 1u << static_cast<unsigned>(Param::Macro4) },
};
static_assert(std::size(kOutputs) == kMacroCount);

inline constexpr const char* kTags[] = { "macro", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "MACROS",
	.name = "Macros",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	/// **NO AMOUNT.** Each route already carries its own depth and a macro's value is itself the depth a
	/// hand sets; a fifth knob scaling all four would be a second way to say the same thing.
	.tags = kTags,
};

extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::macros
