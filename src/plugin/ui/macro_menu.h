/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/macro_type.h"

#include <functional>
#include <string>
#include <vector>

namespace visage {
class Frame;
class PopupMenu;
}

// **ONE MENU FOR A MACRO — WHAT IT IS, WHICH WAY IT READS, WHAT IT IS CALLED.**
//
// A right-click on a macro's control lists its three types, its two polarities and Rename, with an X
// on the type and the polarity it is in now. Every choice is on the face before one is applied, which
// is what the multivalue-toggle rule asks of a control with more than two states: a chip
// that cycled Knob → Latch → Momentary would teach the options by applying them.
//
// **A FREE FUNCTION, NOT A KNOB METHOD,** for the reason `mod_route_menu.h` gives: nothing about it is
// a knob. The knob opens it today and the chip a Latch or Momentary macro becomes opens it next (A7),
// and two copies would be two spellings of one gesture.

namespace feathers::ui {

/// THE HOST'S HALF OF THE MENU. Each pair is the host's own read and verb — the bridge's
/// `macroBipolar`/`setMacroBipolar`, `macroType`/`setMacroType` — so the menu decides only which one a
/// pick means and never holds a copy of the state it shows. COPIED into the menu when it opens, so the
/// selection runs on the hooks as they were and reaches no widget: whatever the hooks themselves
/// capture is the host's lifetime to keep, as it is for every other verb it hands out.
struct MacroMenuHooks {
	std::function<bool()> readBipolar;
	std::function<void(bool)> writeBipolar;
	std::function<device::MacroType()> readType;
	std::function<void(device::MacroType)> writeType;
	std::function<void()> beginRename;
	/// **WHETHER THIS HOST HAS THE RELATIVE ROAD (backlog PQ).** Feathers' header macros do; the rack's and the
	/// MACROS device's do not yet, so their menus list the three absolute types alone and a Relative pick cannot
	/// reach a host that would refuse it.
	bool offerRelative = false;

	/// ALL FIVE OR NONE: a menu that listed Rename with nothing behind it would be a row that lies.
	[[nodiscard]] bool complete() const noexcept {
		return readBipolar && writeBipolar && readType && writeType && beginRename;
	}
};

/// The entries' ids, which are also what `chooseMacroMenuEntry` takes. The types come first and in
/// `device::MacroType` order, so a type's id IS its enum value.
enum MacroMenuEntry : int {
	kMacroMenuKnob = static_cast<int>(device::MacroType::Knob),
	kMacroMenuLatch = static_cast<int>(device::MacroType::Latch),
	kMacroMenuMomentary = static_cast<int>(device::MacroType::Momentary),
	kMacroMenuRelative = static_cast<int>(device::MacroType::Relative),
	kMacroMenuUnipolar = static_cast<int>(device::MacroType::kCount),
	kMacroMenuBipolar,
	kMacroMenuRename,
};

/// Opens the menu under `anchor`. False when the hooks are incomplete — not a macro, so the press goes
/// on to its ordinary road.
bool showMacroMenu(visage::Frame& anchor, const MacroMenuHooks& hooks);

/// **THE SAME MENU, BUILT AND NOT SHOWN** — for a caller that adds to it before it opens: a MACROS card's
/// knob is a device control, so its menu carries the host rows too (`appendHostRows`, host slots T7).
/// Empty when the hooks are incomplete.
[[nodiscard]] visage::PopupMenu macroMenu(const MacroMenuHooks& hooks);

/// WHAT THE MENU WOULD LIST, in order, exactly as it would label each row; a separator is "". What a
/// case reads instead of opening a popup, which this repo cannot click (X11 pointer synthesis is banned
/// on the development display). Empty when the hooks are incomplete.
[[nodiscard]] std::vector<std::string> macroMenuLabelsForTest(const MacroMenuHooks& hooks);

/// **THE VERB BEHIND A PICK** — everything choosing entry `id` does, callable without a menu. An id
/// that is not an entry does nothing, as does any id on incomplete hooks.
void chooseMacroMenuEntry(const MacroMenuHooks& hooks, int id);

} // namespace feathers::ui
