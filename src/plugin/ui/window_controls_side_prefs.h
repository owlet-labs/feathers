/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/editor_prefs.h"
#include "plugin/ui/window_controls_side.h"

#include <memory>
#include <string>

// THE WINDOW-CONTROLS SIDE, KEPT IN AN EDITOR'S PREFS — the tooltip detail's two halves
// (`tooltip_prefs.h`) for the setting `window_controls_side.h` declares (backlog OD). The prefs file and
// never the patch.

namespace feathers::ui {

/// The key the editors keep the side under: `left` or `right`. NO LINE reads as the platform's norm
/// (left on macOS, right elsewhere), so a desk that never chose follows its OS.
inline constexpr const char* kWindowControlsSidePrefKey = "ui.window_controls";

[[nodiscard]] inline WindowControlsSide windowControlsSideIn(const EditorPrefs& prefs) {
	const std::string text = prefs.text(kWindowControlsSidePrefKey);
	if (text == "left") {
		return WindowControlsSide::Left;
	}
	if (text == "right") {
		return WindowControlsSide::Right;
	}
	return platformDefaultWindowControlsSide();
}

/// At an editor's startup: the stored side (or the platform's norm) becomes the process's.
inline void restoreWindowControlsSide(const EditorPrefs& prefs) {
	(void)setWindowControlsSide(windowControlsSideIn(prefs));
}

/// For an editor's lifetime: every `setWindowControlsSide` in the process is written to `prefs`.
[[nodiscard]] inline std::unique_ptr<WindowControlsSideWatch> keepWindowControlsSideIn(EditorPrefs& prefs) {
	return std::make_unique<WindowControlsSideWatch>([&prefs](WindowControlsSide side) {
		prefs.setText(kWindowControlsSidePrefKey, side == WindowControlsSide::Left ? "left" : "right");
	});
}

} // namespace feathers::ui
