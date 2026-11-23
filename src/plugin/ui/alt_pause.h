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

#include "plugin/ui/mod_state.h"

#include <visage/app.h>

#include <cstddef>

namespace feathers::ui {

/// **THE ALT PAUSE, POLLED ONCE PER FRAME — the one road every editor's tick shares** (synth, Chain, Rack).
///
/// A held Alt pauses assign mode: `ModState::assigning()` goes false and every control's glow (`ModDisplay::armed`,
/// `routable`) goes with it. The modifier is read from the platform's live state (`Window::modifierState()`), NOT from a
/// key event, so it does not depend on which frame holds the keyboard: a device window's panel, a popup or the root all
/// see the same answer. `probe` is the test seam (a headless editor has no window); negative, or no window that can
/// answer, leaves the flag alone so the mouse-event feed keeps it. Returns how many frames were told.
inline size_t pollAltPause(ModState& mod, visage::ApplicationWindow* app, int (*probe)() = nullptr) {
	int modifiers = -1; // negative: nothing read
	if (probe != nullptr) {
		modifiers = probe();
	}
	else if (app != nullptr && app->window() != nullptr && app->window()->hasModifierState()) {
		modifiers = app->window()->modifierState();
	}
	return modifiers >= 0 ? mod.setBypass((modifiers & visage::kModifierAlt) != 0) : 0;
}

} // namespace feathers::ui
