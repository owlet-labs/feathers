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
#include "mod/macros/macros_core.h"

#include <array>
#include <cstddef>

// THE ONE- AND EIGHT-MACRO DEVICES' DESCRIPTORS, BUILT FROM THE COUNT.
//
// The four-macro MACROS device keeps its hand-written descriptor (`macros_types.h`) so its tag, keys and
// ledger are untouched. These two follow the same conventions: keys macro_n, bipolar_n, type_n and
// enabled; the values are the only modulatable controls and each is a stereo destination; each output
// is a source naming its own polarity switch. The row is laid out by `macroField` and its siblings.

namespace feathers::mod::macros {

inline constexpr const char* const kVariantOffOn[] = { "Off", "On" };

namespace variant_tables {
inline constexpr const char* kMacroKey[8] = { "macro_1", "macro_2", "macro_3", "macro_4", "macro_5", "macro_6", "macro_7", "macro_8" };
inline constexpr const char* kBipolarKey[8] = { "bipolar_1", "bipolar_2", "bipolar_3", "bipolar_4", "bipolar_5", "bipolar_6", "bipolar_7", "bipolar_8" };
inline constexpr const char* kTypeKey[8] = { "type_1", "type_2", "type_3", "type_4", "type_5", "type_6", "type_7", "type_8" };
inline constexpr const char* kMacroName[8] = { "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8" };
inline constexpr const char* kBipolarName[8] = { "Bipolar 1", "Bipolar 2", "Bipolar 3", "Bipolar 4", "Bipolar 5", "Bipolar 6", "Bipolar 7", "Bipolar 8" };
inline constexpr const char* kTypeName[8] = { "Type 1", "Type 2", "Type 3", "Type 4", "Type 5", "Type 6", "Type 7", "Type 8" };
inline constexpr const char* kShort[8] = { "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8" };
inline constexpr const char* kValueTip[8] = {
	"Macro 1's value — what its output publishes", "Macro 2's value — what its output publishes",
	"Macro 3's value — what its output publishes", "Macro 4's value — what its output publishes",
	"Macro 5's value — what its output publishes", "Macro 6's value — what its output publishes",
	"Macro 7's value — what its output publishes", "Macro 8's value — what its output publishes" };
inline constexpr const char* kBipolarTip[8] = {
	"Macro 1's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 2's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 3's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 4's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 5's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 6's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 7's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1",
	"Macro 8's output swings -1 to +1, centred on the knob's middle, instead of 0 to 1" };
inline constexpr const char* kTypeTip[8] = {
	"How Macro 1's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 2's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 3's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 4's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 5's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 6's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 7's control plays — a knob turns, a latch toggles, a momentary holds while pressed",
	"How Macro 8's control plays — a knob turns, a latch toggles, a momentary holds while pressed" };
} // namespace variant_tables

template <size_t N>
[[nodiscard]] constexpr std::array<device::ParamDesc, 3 * N + 1> makeVariantParams() {
	static_assert(N >= 1 && N <= 8);
	std::array<device::ParamDesc, 3 * N + 1> out {};
	for (size_t n = 0; n < N; ++n) {
		out[macroField(n)] = device::ParamDesc { .key = variant_tables::kMacroKey[n], .name = variant_tables::kMacroName[n],
			.min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent, .names = {},
			.modulatable = true, .tooltip = variant_tables::kValueTip[n], .stereo = true };
		out[bipolarField(N, n)] = device::ParamDesc { .key = variant_tables::kBipolarKey[n], .name = variant_tables::kBipolarName[n],
			.min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kVariantOffOn,
			.modulatable = false, .tooltip = variant_tables::kBipolarTip[n] };
		out[typeField(N, n)] = device::ParamDesc { .key = variant_tables::kTypeKey[n], .name = variant_tables::kTypeName[n],
			.min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
			.names = device::kAbsoluteMacroTypeNames, .modulatable = false, .tooltip = variant_tables::kTypeTip[n] };
	}
	// ON BY DEFAULT — a modulator added to a chain should modulate.
	out[enableField(N)] = device::ParamDesc { .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0,
		.stepped = true, .unit = device::Unit::Choice, .names = kVariantOffOn, .modulatable = true,
		.tooltip = "Bypass switch — off: every output rests at 0" };
	return out;
}

template <size_t N>
[[nodiscard]] constexpr std::array<device::SourceDesc, N> makeVariantOutputs() {
	std::array<device::SourceDesc, N> out {};
	for (size_t n = 0; n < N; ++n) {
		out[n] = device::SourceDesc { .key = variant_tables::kMacroKey[n], .name = variant_tables::kMacroName[n], .bipolar = true,
			.bipolarControl = static_cast<int>(bipolarField(N, n)), .pairFrom = 1u << static_cast<unsigned>(macroField(n)),
			.shortName = variant_tables::kShort[n] };
	}
	return out;
}

inline constexpr const char* kVariantTags[] = { "macro", "modulator" };

} // namespace feathers::mod::macros
