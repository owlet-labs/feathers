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

#include "plugin/ui/text_field.h"

#include <optional>
#include <visage/ui.h>

// **THE UNDO CHORD, AND THE TEST FOR "SOMEONE IS TYPING"** — one spelling for every editor that walks an `EditHistory` from the
// keyboard (Feathers' window and the rack plugin's), so the two cannot disagree about which keys are undo or when they stand down.

namespace feathers::ui {

/// WHICH WAY AN UNDO CHORD WALKS: false for undo (main modifier + Z), true for redo (main modifier + Shift + Z, or main
/// modifier + Y); nullopt for any other key. Alt held is someone else's chord.
[[nodiscard]] inline std::optional<bool> undoChordOf(const visage::KeyEvent& e) {
	if (!e.isMainModifier() || e.isAltDown()) {
		return std::nullopt;
	}
	if (e.keyCode() == visage::KeyCode::Z) {
		return e.isShiftDown();
	}
	if (e.keyCode() == visage::KeyCode::Y && !e.isShiftDown()) {
		return true;
	}
	return std::nullopt;
}

/// A TEXT ENTRY THAT IS EDITING AND HOLDS THE KEYBOARD, anywhere under `frame`. Its Ctrl+Z is the field's (visage's
/// `TextEditor` keeps its own undo and never lets the chord up) or, for our `TextField`, which has none, the host's — never the
/// editor's history.
[[nodiscard]] inline bool textEntryFocusedUnder(visage::Frame* frame) {
	if (frame == nullptr || !frame->isVisible()) {
		return false;
	}
	// OUR FIELD BY ITS OWN WORD: it edits only while it holds the keyboard (a focus loss ends the edit), so `editing()` is the
	// answer even where no window event handler tracks the focus.
	if (const auto* field = dynamic_cast<const TextField*>(frame); field != nullptr && field->editing()) {
		return true;
	}
	// visage's `TextEditor` answers `receivesTextInput` whenever it exists, so only a FOCUSED one counts.
	if (frame->hasKeyboardFocus() && frame->receivesTextInput()) {
		return true;
	}
	for (visage::Frame* child : frame->children()) {
		if (textEntryFocusedUnder(child)) {
			return true;
		}
	}
	return false;
}

} // namespace feathers::ui
