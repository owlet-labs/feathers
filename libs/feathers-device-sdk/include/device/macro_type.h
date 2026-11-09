/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace feathers::device {

/// **WHAT A MACRO'S KNOB IS.** The CLAP parameter under a macro never changes — it stays a 0..1 value
/// the DAW automates by that name. The type is how the control on the face drives it: a Knob turns, a
/// Latch toggles between its ends on a click, a Momentary holds the top end only while pressed. Patch
/// state beside the parameters, like the polarity (backlog EG).
///
/// **RELATIVE IS THE ODD ONE OUT (backlog PQ).** The first three
/// all drive a VALUE. A Relative macro has no value: its face is an endless encoder, a turn is an
/// INCREMENT, and each route from it edits its destination's stored base (`ui::RelativeMacroGesture`)
/// rather than offsetting it per block — so the engine reads a Relative macro as resting at 0. Feathers'
/// header macros offer it; the rack's and the MACROS device's do not yet (`kAbsoluteMacroTypeCount`).
enum class MacroType : uint8_t { Knob = 0, Latch = 1, Momentary = 2, Relative = 3, kCount };

/// THE TYPES' NAMES, IN `MacroType`'S ORDER — so a Choice control that stores a type (the MACROS
/// device's `Type n`) stores the same index this enum is, and one table says which word is which.
inline constexpr const char* const kMacroTypeNames[] = { "Knob", "Latch", "Momentary", "Relative" };
static_assert(std::size(kMacroTypeNames) == static_cast<size_t>(MacroType::kCount));

/// **THE TYPES THAT DRIVE A VALUE** — the first three. What a host without the relative road (the rack,
/// the MACROS device) offers and accepts; Relative is the one past them.
inline constexpr size_t kAbsoluteMacroTypeCount = 3;
inline constexpr const char* const kAbsoluteMacroTypeNames[] = { "Knob", "Latch", "Momentary" };
static_assert(std::size(kAbsoluteMacroTypeNames) == kAbsoluteMacroTypeCount);
static_assert(static_cast<size_t>(MacroType::Relative) == kAbsoluteMacroTypeCount, "Relative follows the absolute three");

/// A type a host without the relative road may hold.
[[nodiscard]] constexpr bool isAbsoluteMacroType(MacroType type) noexcept {
	return static_cast<size_t>(type) < kAbsoluteMacroTypeCount;
}

/// **A MACRO'S STORED 0..1 SEEN BIPOLAR, AND BACK (backlog EG).** The one rule every bipolar macro
/// follows: the header's and the rack's knobs through `ui::BipolarMacro`, and the MACROS device's
/// outputs. Here, in the device vocabulary, so `src/mod` can use it without reaching into the plugin.
[[nodiscard]] constexpr double macroToBipolar(double stored) noexcept { return 2.0 * stored - 1.0; }
[[nodiscard]] constexpr double macroToStored(double bipolar) noexcept { return 0.5 * (bipolar + 1.0); }

/// A macro's user name, in BYTES of UTF-8. A longer name is cut on a code-point boundary, never
/// inside one, so a clamped name is always valid UTF-8 if the input was.
inline constexpr size_t kMacroNameMax = 24;

/// Truncates `name` to at most kMacroNameMax bytes without splitting a UTF-8 code point: when the cut
/// falls on a continuation byte (10xxxxxx) it backs up to the lead byte of that code point and cuts
/// before it.
inline std::string clampMacroName(std::string_view name) {
	if (name.size() <= kMacroNameMax) {
		return std::string(name);
	}
	size_t cut = kMacroNameMax;
	while (cut > 0 && (static_cast<uint8_t>(name[cut]) & 0xC0U) == 0x80U) {
		--cut;
	}
	return std::string(name.substr(0, cut));
}

/// "Macro N" for macro `index` — the parameter table's own spelling, which is what the DAW shows. One
/// spelling for both hosts: Feathers' macros and the rack's are named alike until a hand renames one.
inline std::string defaultMacroName(size_t index) {
	return "Macro " + std::to_string(index + 1);
}

/// A name as a bridge holds it: clamped, and EMPTY when it says nothing the default does not, so
/// "renamed" is one test (non-empty) rather than a comparison every reader has to remember to make.
inline std::string normalisedMacroName(size_t index, std::string_view name) {
	std::string clamped = clampMacroName(name);
	return clamped == defaultMacroName(index) ? std::string {} : clamped;
}

} // namespace feathers::device
