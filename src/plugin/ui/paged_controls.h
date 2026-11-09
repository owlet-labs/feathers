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

// **A SURFACE THAT SHOWS ONE PAGE OF ITS CONTROLS AT A TIME** (the Pattern card shows one tab on its face at a
// time). The page is UI state, not a device control, so no gate sweep reaches it: a guard
// that compares a card with its window per state (tests/gui_widgets/test_gui_widgets_tooltips.cpp, "a device's window
// offers every control its card offers") asks this interface for the pages and shows the SAME page on both before it
// compares. A card and a window of one device implement it together or not at all.

#include <cstddef>

namespace feathers::ui {

class PagedControls {
public:
	virtual ~PagedControls() = default;
	[[nodiscard]] virtual size_t pageCount() const = 0;
	/// Shows page `page` (and lays the surface out for it).
	virtual void showPage(size_t page) = 0;
};

} // namespace feathers::ui
