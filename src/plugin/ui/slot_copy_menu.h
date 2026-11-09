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

// **THE COPY HALF OF A SLOT TAB'S CONTEXT MENU** — the Drawn filter's waypoint (frame) tabs first, then the Pattern's tabs
// (the same copy-to-1/2/3/4 and every context option as on the Drawn filter's waypoint tabs):
// "Copy to <name>" for every OTHER slot, in order, then "Copy to all others" — the plural of the lines
// above it, placed after them so a verb that overwrites every slot does not sit where a hand aiming for one lands
// (Drawn's design). One list, so the two menus cannot drift apart.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {

/// Appends to `options`: `{i, "Copy to " + nameOf(i)}` for each slot `i` of `count` but `self`, then `{allId, "Copy to all
/// others"}`.
inline void appendCopyToOptions(std::vector<std::pair<int, std::string>>& options, size_t count, size_t self,
                                const std::function<std::string(size_t)>& nameOf, int allId) {
	for (size_t i = 0; i < count; ++i) {
		if (i == self) {
			continue;
		}
		options.emplace_back(static_cast<int>(i), std::string("Copy to ") + nameOf(i));
	}
	options.emplace_back(allId, "Copy to all others");
}

} // namespace feathers::ui
