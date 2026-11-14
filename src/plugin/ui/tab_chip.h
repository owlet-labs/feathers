/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/theme.h"

#include <string>
#include <visage/ui.h>

// ONE LOOK FOR A WINDOW'S TABS. Drawn's frame tabs ("Frame 1" ... "Frame 4") and HARM's partial tabs ("1" ... "4") are the same
// control in the same place — a row of small selectors at the top of a window's picture — so they are drawn by the same
// function and cannot drift into two looks. What a tab SELECTS is the window's own view state (it is not a parameter, is not
// automated and is not modulated), which is also why this draws and nothing more: the hit test and the selection stay with
// the window that owns them.

namespace feathers::ui {

/// A tab: filled in the accent while it is the selected one, an outline otherwise, its text in tracked capitals at the left.
inline void drawTabChip(visage::Canvas& canvas, const visage::Bounds& tab, const std::string& text, bool selected) {
	canvas.setColor(selected ? kAccent : withAlpha(kLine, 0.35f));
	if (selected) {
		canvas.roundedRectangle(tab.x(), tab.y(), tab.width(), tab.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(tab.x(), tab.y(), tab.width(), tab.height(), 3.0f, 1.0f);
	}
	canvas.setColor(selected ? kSurface : kMuted);
	drawTracked(canvas, text, font(kControlSize), kControlTracking, tab.x() + 8.0f, tab.y(), tab.height());
}

} // namespace feathers::ui
