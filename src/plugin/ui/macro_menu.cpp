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

#include "plugin/ui/macro_menu.h"
#include "plugin/ui/menu_anchor.h"

#include "visage_ui/frame.h"
#include "visage_ui/popup_menu.h"

namespace feathers::ui {

namespace {

/// ONE ROW OF THE MENU, OR A SEPARATOR (`id < 0`, empty label).
struct Row {
	int id;
	std::string label;
};

/// THE X ON THE ONE YOU ARE IN, written into the label for the reason `ValueBox::choiceLabelAt` gives:
/// the popup's own `select` is read by the macOS menu bar and by nothing this editor draws. The
/// unmarked rows carry the same cell so the names line up in a column.
std::string marked(bool current, const char* name) {
	return (current ? std::string("X  ") : std::string("     ")) + name;
}

/// THE ONE LIST, read by both the popup and the case — so what a case asserts is what a hand sees,
/// rather than a second spelling of it that could drift.
std::vector<Row> rows(const MacroMenuHooks& hooks) {
	const device::MacroType type = hooks.readType();
	const bool bipolar = hooks.readBipolar();
	std::vector<Row> out {
		{ kMacroMenuKnob, marked(type == device::MacroType::Knob, "Knob") },
		{ kMacroMenuLatch, marked(type == device::MacroType::Latch, "Latch") },
		{ kMacroMenuMomentary, marked(type == device::MacroType::Momentary, "Momentary") },
	};
	if (hooks.offerRelative) {
		out.push_back({ kMacroMenuRelative, marked(type == device::MacroType::Relative, "Relative (encoder)") });
	}
	out.push_back({ -1, {} });
	// **NO POLARITY ON A RELATIVE MACRO** — it has no value to read either way, so the two rows would be
	// switches that change nothing (the honesty rule). They come back with the type.
	if (type != device::MacroType::Relative) {
		out.push_back({ kMacroMenuUnipolar, marked(!bipolar, "Unipolar  0..100%") });
		out.push_back({ kMacroMenuBipolar, marked(bipolar, "Bipolar  -100..100%") });
		out.push_back({ -1, {} });
	}
	// NO X: renaming is an act, not a state the macro is in.
	out.push_back({ kMacroMenuRename, "Rename…" });
	return out;
}

} // namespace

visage::PopupMenu macroMenu(const MacroMenuHooks& hooks) {
	visage::PopupMenu menu;
	if (!hooks.complete()) {
		return menu;
	}
	for (const Row& row : rows(hooks)) {
		if (row.id < 0) {
			menu.addBreak();
		}
		else {
			menu.addOption(row.id, row.label);
		}
	}
	// **THE HOOKS BY VALUE, THE ANCHOR NOT AT ALL** — the shape `showRouteMenu` has. The selection
	// arrives after this call has returned, and a lambda holding the anchor would hold a widget the
	// editor is free to have rebuilt by then.
	menu.onSelection() = [hooks](int id) { chooseMacroMenuEntry(hooks, id); };
	return menu;
}

bool showMacroMenu(visage::Frame& anchor, const MacroMenuHooks& hooks) {
	if (!hooks.complete()) {
		return false; // not a macro: the press goes on to its ordinary road
	}
	visage::PopupMenu menu = macroMenu(hooks);
	// THE ONE ROAD EVERY DROPDOWN OPENS BY (`menu_anchor.h`): placed against the anchor's whole box,
	// and a press on the same box while its menu is open closes it. The press is still consumed then,
	// which is why this returns true either way — a re-click is the menu's, not the knob's drag.
	(void)showMenuAt(std::move(menu), anchor, { 0.0f, 0.0f, anchor.width(), anchor.height() });
	return true;
}

std::vector<std::string> macroMenuLabelsForTest(const MacroMenuHooks& hooks) {
	std::vector<std::string> labels;
	if (!hooks.complete()) {
		return labels;
	}
	for (Row& row : rows(hooks)) {
		labels.push_back(std::move(row.label));
	}
	return labels;
}

void chooseMacroMenuEntry(const MacroMenuHooks& hooks, int id) {
	if (!hooks.complete()) {
		return;
	}
	switch (id) {
		case kMacroMenuKnob:
		case kMacroMenuLatch:
		case kMacroMenuMomentary:
			hooks.writeType(static_cast<device::MacroType>(id));
			return;
		case kMacroMenuRelative:
			if (hooks.offerRelative) {
				hooks.writeType(device::MacroType::Relative);
			}
			return;
		case kMacroMenuUnipolar:
		case kMacroMenuBipolar:
			hooks.writeBipolar(id == kMacroMenuBipolar);
			return;
		case kMacroMenuRename:
			hooks.beginRename();
			return;
		default:
			return; // a dismissed menu (-1), or no entry at all
	}
}

} // namespace feathers::ui
