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
#include "plugin/ui/emitters_front.h"

#include <memory>
#include <string>

// ARPS SENT TO FRONT OF CHAIN, KEPT IN AN EDITOR'S PREFS — the window-controls side's two halves
// (`window_controls_side_prefs.h`) for the setting `emitters_front.h` declares. The prefs file and never the patch.

namespace feathers::ui {

/// The key: `on` or `off`. No line reads ON, the default.
inline constexpr const char* kEmittersFrontPrefKey = "chain.arps_to_front";

[[nodiscard]] inline bool emittersToFrontIn(const EditorPrefs& prefs) {
	return prefs.text(kEmittersFrontPrefKey) != "off";
}

/// At an editor's startup: the stored choice becomes the process's.
inline void restoreEmittersToFront(const EditorPrefs& prefs) {
	(void)setEmittersToFront(emittersToFrontIn(prefs));
}

/// For an editor's lifetime: every `setEmittersToFront` in the process is written to `prefs`.
[[nodiscard]] inline std::unique_ptr<EmittersFrontWatch> keepEmittersToFrontIn(EditorPrefs& prefs) {
	return std::make_unique<EmittersFrontWatch>([&prefs](bool on) { prefs.setText(kEmittersFrontPrefKey, on ? "on" : "off"); });
}

} // namespace feathers::ui
