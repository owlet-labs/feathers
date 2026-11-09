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

#include "plugin/editor_prefs.h"
#include "plugin/ui/chrome_hue.h"

#include <functional>
#include <memory>
#include <utility>

// THE CHROME HUE, KEPT IN AN EDITOR'S PREFS — the tooltip detail's two halves (`tooltip_prefs.h`) for
// the hue offset (backlog LG). The prefs file and never the patch: two people sharing a patch do not
// share a taste in colour, and nothing here is read by the engine.

namespace feathers::ui {

/// The key the editors keep the offset under, in degrees. No line reads as 0, the mock's own colours.
inline constexpr const char* kChromeHuePrefKey = "ui.hue";

/// At an editor's startup: the stored offset becomes the process's.
inline void restoreChromeHue(const EditorPrefs& prefs) {
	(void)setChromeHueOffset(prefs.number(kChromeHuePrefKey, 0.0));
}

/// For an editor's lifetime: every `setChromeHueOffset` in the process is written to `prefs`, and
/// `repaint` is called so the editor's whole tree draws in the new colours on its next frame.
[[nodiscard]] inline std::unique_ptr<ChromeHueWatch> keepChromeHueIn(EditorPrefs& prefs,
                                                                     std::function<void()> repaint) {
	return std::make_unique<ChromeHueWatch>([&prefs, repaint = std::move(repaint)](double degrees) {
		prefs.setNumber(kChromeHuePrefKey, degrees);
		if (repaint) {
			repaint();
		}
	});
}

/// THE ACCENT HUE's key, in degrees. Prefs written before it existed have no line and load as 0, which is
/// the chrome-only palette bit for bit.
inline constexpr const char* kAccentHuePrefKey = "ui.accent_hue";

/// At an editor's startup: the stored accent offset becomes the process's.
inline void restoreAccentHue(const EditorPrefs& prefs) {
	(void)setAccentHueOffset(prefs.number(kAccentHuePrefKey, 0.0));
}

/// For an editor's lifetime: every `setAccentHueOffset` is written to `prefs`, and `repaint` is called.
[[nodiscard]] inline std::unique_ptr<AccentHueWatch> keepAccentHueIn(EditorPrefs& prefs,
                                                                     std::function<void()> repaint) {
	return std::make_unique<AccentHueWatch>([&prefs, repaint = std::move(repaint)](double degrees) {
		prefs.setNumber(kAccentHuePrefKey, degrees);
		if (repaint) {
			repaint();
		}
	});
}

} // namespace feathers::ui
