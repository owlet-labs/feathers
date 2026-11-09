/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "doctest.h"
#include "plugin/ui/menu_anchor.h"

#include <cmath>
#include <string>

// **A MENU HANGS OFF ITS BOX** — the negation of the defect where a dropdown menu did not appear next to
// the box it belongs to, stated as geometry in ONE coordinate space: the frame the popup
// was parented on, which is where `lastMenuPlacementForTest` reports both rectangles.
//
// `box` IS COMPUTED BY THE CASE, from the widget's own accessor and its position — not read back from
// the placement — so a site that passed the wrong anchor fails here rather than agreeing with itself.

namespace menu_check {

inline bool near(float a, float b) { return std::abs(a - b) < 0.5f; }

inline void checkHangsOff(const feathers::ui::MenuPlacement& p, const visage::Bounds& box) {
	REQUIRE(p.shown);
	const visage::Bounds& a = p.anchor;
	const visage::Bounds& m = p.list;
	CAPTURE(box.x());
	CAPTURE(box.y());
	CAPTURE(box.width());
	CAPTURE(box.height());
	CAPTURE(m.x());
	CAPTURE(m.y());
	CAPTURE(m.width());
	CAPTURE(m.height());
	CAPTURE(p.windowWidth);
	CAPTURE(p.windowHeight);
	// THE SITE ANCHORED ON THE BOX IT OPENED FROM.
	CHECK(near(a.x(), box.x()));
	CHECK(near(a.y(), box.y()));
	CHECK(near(a.width(), box.width()));
	CHECK(near(a.height(), box.height()));
	REQUIRE(m.width() > 0.0f);
	REQUIRE(m.height() > 0.0f);
	// INSIDE THE WINDOW IT WAS OPENED IN.
	CHECK(m.x() >= -0.5f);
	CHECK(m.y() >= -0.5f);
	CHECK(m.right() <= p.windowWidth + 0.5f);
	CHECK(m.bottom() <= p.windowHeight + 0.5f);
	// TOUCHING THE BOX ON ONE EDGE, AND SQUARE TO IT — under or over it with an edge in line with one of
	// the box's, or beside it on the box's own line. **A CORNER IS NOT "NEXT TO".** visage's horizontal
	// flip put OWLTT's popped-out menu a whole menu-width LEFT of the box, meeting it only at one corner,
	// and a check that asked merely for touching passed it.
	const bool alongX = near(m.x(), box.x()) || near(m.right(), box.right());
	const bool alongY = near(m.y(), box.y()) || (m.y() <= box.y() + 0.5f && m.bottom() >= box.bottom() - 0.5f);
	const bool below = near(m.y(), box.bottom()) && alongX;
	const bool above = near(m.bottom(), box.y()) && alongX;
	const bool right = near(m.x(), box.right()) && alongY;
	const bool left = near(m.right(), box.x()) && alongY;
	CHECK((below || above || right || left));
	// ...AND NOT OVER IT.
	const bool covers = m.x() < box.right() - 0.5f && m.right() > box.x() + 0.5f && m.y() < box.bottom() - 0.5f
	                    && m.bottom() > box.y() + 0.5f;
	CHECK_FALSE(covers);
}

/// `local` in `frame`'s own coordinates, converted into the coordinates of the frame a popup opened
/// from it is parented on — visage's `topParentFrame`.
inline visage::Bounds inTop(visage::Frame& frame, const visage::Bounds& local) {
	const visage::Bounds at = frame.topParentFrame()->relativeBounds(&frame);
	return { at.x() + local.x(), at.y() + local.y(), local.width(), local.height() };
}

/// THE NEWEST POPUP OVER `frame` — visage parents each on the source's top frame and appends it, so the
/// last one among the top frame's children is the one opened last. `nullptr` if none.
inline visage::PopupMenuFrame* newestPopup(visage::Frame& frame) {
	const auto& children = frame.topParentFrame()->children();
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		if (auto* popup = dynamic_cast<visage::PopupMenuFrame*>(*it); popup != nullptr) {
			return popup;
		}
	}
	return nullptr;
}

/// THE POPUP'S ROWS — its first list, the one `showMenuAt` places.
inline visage::PopupList* rowsOf(visage::PopupMenuFrame& popup) {
	return popup.children().empty() ? nullptr : dynamic_cast<visage::PopupList*>(popup.children().front());
}

/// THE ENABLED ROW LABELLED `label`, at any depth — a device in a category submenu is a leaf of it.
inline const visage::PopupMenu* findRow(visage::PopupMenuFrame& popup, const std::string& label) {
	visage::PopupList* list = rowsOf(popup);
	if (list == nullptr) {
		return nullptr;
	}
	const visage::PopupMenu* found = nullptr;
	const auto walk = [&](const auto& self, const visage::PopupMenu& option) -> void {
		for (const visage::PopupMenu& sub : option.options()) {
			self(self, sub);
		}
		if (found == nullptr && !option.hasOptions() && option.enabled() && option.name().toUtf8() == label) {
			found = &option;
		}
	};
	for (int i = 0; i < list->numOptions(); ++i) {
		walk(walk, list->option(i));
	}
	return found;
}

/// **PICK THE ROW LABELLED `label` THE WAY visage DOES** — `PopupList::selectHoveredIndex` hands the row
/// to `PopupMenuFrame::optionSelected`, which runs the menu's selection callbacks and only then exits the
/// popup. Not the site's own callback called by hand, because what a cascade breaks is what happens
/// while that call is still on the stack. Returns false if no enabled row has the label.
inline bool pickRow(visage::PopupMenuFrame& popup, const std::string& label) {
	const visage::PopupMenu* row = findRow(popup, label);
	if (row == nullptr) {
		return false;
	}
	popup.optionSelected(*row, rowsOf(popup));
	return true;
}

} // namespace menu_check
