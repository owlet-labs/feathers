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

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

// WHICH END OF A WINDOW'S HEADER THE WINDOW CONTROLS SIT AT (backlog OD: a setting that
// moves a window's open/close widgets to its left-hand side, to follow OS norms). A DESK preference, never
// patch state: two people sharing a patch do not share a platform. The default follows the host OS's
// norm (left on macOS, right elsewhere) and Settings overrides it; the choice is kept in the editor's
// prefs (`window_controls_side_prefs.h`) and everything that draws a header just asks
// `windowControlsSide()`, at the moment it lays out or draws, so a change shows on the next frame.
//
// Header-only, like the chrome hue's registry is one file: the slot is a function-local static of an
// inline function, so every TU in a binary sees ONE value. MESSAGE THREAD.

namespace feathers::ui {

enum class WindowControlsSide { Left, Right };

/// THE PLATFORM'S NORM, as a function of the platform so a case can ask both answers.
[[nodiscard]] constexpr WindowControlsSide platformWindowControlsSide(bool apple) noexcept {
	return apple ? WindowControlsSide::Left : WindowControlsSide::Right;
}

/// The norm of the platform this binary was built for.
[[nodiscard]] constexpr WindowControlsSide platformDefaultWindowControlsSide() noexcept {
#if defined(__APPLE__)
	return platformWindowControlsSide(true);
#else
	return platformWindowControlsSide(false);
#endif
}

/// WHO KEEPS THE SIDE — an editor that owns prefs holds one for its lifetime; every
/// `setWindowControlsSide` tells every watch alive (the tooltip detail's rule). MESSAGE THREAD.
class WindowControlsSideWatch;

namespace window_controls_detail {
inline WindowControlsSide& sideSlot() noexcept {
	static WindowControlsSide side = platformDefaultWindowControlsSide();
	return side;
}
inline std::vector<WindowControlsSideWatch*>& watches() {
	static std::vector<WindowControlsSideWatch*> list;
	return list;
}
} // namespace window_controls_detail

/// The side in force. The platform's norm until something says otherwise.
[[nodiscard]] inline WindowControlsSide windowControlsSide() noexcept {
	return window_controls_detail::sideSlot();
}

class WindowControlsSideWatch {
public:
	explicit WindowControlsSideWatch(std::function<void(WindowControlsSide)> changed)
	    : changed_(std::move(changed)) {
		window_controls_detail::watches().push_back(this);
	}
	~WindowControlsSideWatch() { std::erase(window_controls_detail::watches(), this); }
	WindowControlsSideWatch(const WindowControlsSideWatch&) = delete;
	WindowControlsSideWatch& operator=(const WindowControlsSideWatch&) = delete;

private:
	friend size_t setWindowControlsSide(WindowControlsSide side);
	std::function<void(WindowControlsSide)> changed_;
};

/// **THE HOOK THE SETTINGS SCREEN BINDS.** Changes the side for the whole process and tells every watch
/// alive. Returns how many it told; none when the side was already `side`.
inline size_t setWindowControlsSide(WindowControlsSide side) {
	if (side == window_controls_detail::sideSlot()) {
		return 0;
	}
	window_controls_detail::sideSlot() = side;
	// A COPY: nothing a watch does may add or remove one mid-walk.
	const std::vector<WindowControlsSideWatch*> told = window_controls_detail::watches();
	for (WindowControlsSideWatch* w : told) {
		if (w->changed_) {
			w->changed_(side);
		}
	}
	return told.size();
}

} // namespace feathers::ui
