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

#include <visage/graphics.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

// REPAINT: PARTIAL OR EVERY FRAME, a setting in the settings screen.
//
// The value lives in visage, `visage::Canvas::fullRedraw()` (our fork): while ON, `Canvas::submit` invalidates every
// layer every frame, so each swap-chain buffer is composited whole and no buffer can present a region whose change it
// missed; OFF, only damage is repainted. It used to be the compile-time `FEATHERS_FULL_REDRAW` alone — ON in Linux
// installs, OFF in the GitHub macOS build — so the Mac ran the less-exercised partial path with no way to compare.
//
// **ITS DEFAULT IS STILL THE BUILD'S**: `Canvas::kFullRedrawCompiledDefault` (ON where FEATHERS_FULL_REDRAW is
// defined). A prefs file with no line therefore behaves exactly as before this setting existed.
//
// AN EDITOR PREFERENCE, NOT PATCH STATE, and PROCESS-GLOBAL (every canvas reads one atomic): the emitters-front road
// (`emitters_front.h`) — one value, a watch per editor writing it to its prefs, both editors restoring it.

namespace feathers::ui {

class FullRedrawWatch;

namespace full_redraw_detail {
inline std::vector<FullRedrawWatch*>& watches() {
	static std::vector<FullRedrawWatch*> list;
	return list;
}
} // namespace full_redraw_detail

/// Whether every frame repaints the whole window.
[[nodiscard]] inline bool fullRedraw() noexcept {
	return visage::Canvas::fullRedraw();
}

/// What a desk with no stored choice gets: the build's compile-time default.
[[nodiscard]] constexpr bool fullRedrawDefault() noexcept {
	return visage::Canvas::kFullRedrawCompiledDefault;
}

/// One editor's ear on the setting, for its prefs and a repaint. Main thread.
class FullRedrawWatch {
public:
	explicit FullRedrawWatch(std::function<void(bool)> changed) : changed_(std::move(changed)) {
		full_redraw_detail::watches().push_back(this);
	}
	~FullRedrawWatch() { std::erase(full_redraw_detail::watches(), this); }
	FullRedrawWatch(const FullRedrawWatch&) = delete;
	FullRedrawWatch& operator=(const FullRedrawWatch&) = delete;

private:
	friend size_t setFullRedraw(bool on);
	std::function<void(bool)> changed_;
};

/// Set it for the process (the canvas flag); returns how many watches were told (0 when it did not move).
inline size_t setFullRedraw(bool on) {
	if (on == visage::Canvas::fullRedraw()) {
		return 0;
	}
	visage::Canvas::setFullRedraw(on);
	const std::vector<FullRedrawWatch*> told = full_redraw_detail::watches();
	for (FullRedrawWatch* w : told) {
		if (w->changed_) {
			w->changed_(on);
		}
	}
	return told.size();
}

/// The key: `on` or `off`. No line reads as the build's default.
inline constexpr const char* kFullRedrawPrefKey = "ui.full_redraw";

[[nodiscard]] inline bool fullRedrawIn(const EditorPrefs& prefs) {
	const std::string stored = prefs.text(kFullRedrawPrefKey);
	if (stored == "on") {
		return true;
	}
	if (stored == "off") {
		return false;
	}
	return fullRedrawDefault();
}

/// At an editor's startup: the stored choice (else the build's default) becomes the process's.
/// **A BUILD WITHOUT THE DEBUG OPTIONS SECTION (`FEATHERS_DEBUG_OPTIONS` off) IGNORES THE STORED CHOICE** and takes the
/// build's default, since nothing on its screen could show or change it.
inline void restoreFullRedraw(const EditorPrefs& prefs) {
#ifdef FEATHERS_DEBUG_OPTIONS
	(void)setFullRedraw(fullRedrawIn(prefs));
#else
	(void)prefs;
	(void)setFullRedraw(fullRedrawDefault());
#endif
}

/// For an editor's lifetime: every `setFullRedraw` is written to `prefs`, and `repaint` is called so the switch
/// to partial starts from a whole, current frame.
[[nodiscard]] inline std::unique_ptr<FullRedrawWatch> keepFullRedrawIn(EditorPrefs& prefs, std::function<void()> repaint) {
	return std::make_unique<FullRedrawWatch>([&prefs, repaint = std::move(repaint)](bool on) {
		prefs.setText(kFullRedrawPrefKey, on ? "on" : "off");
		if (repaint) {
			repaint();
		}
	});
}

} // namespace feathers::ui
