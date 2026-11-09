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

#include "device/registry.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <span>
#include <vector>

// AN OFFER, FILED BY CATEGORY — the one grouping every add surface draws.
//
// PURE AND SEPARATE FROM ANY WIDGET, so the grouping is checked without a window: the popup menus
// (`plugin/ui/device_menu.h`) and the rack's takeover browser (`rack::browseDevices`) both draw what
// this returns, which is what keeps them from disagreeing about where a device lives.
//
// IT FILES AN OFFER; IT NEVER DECIDES ONE. Which devices a surface may add stays that surface's own
// question (a lane asks `audio`, the strip asks its tag, a chain asks its catalogue); this takes the
// answer and says which heading each goes under. So a category with nothing eligible does not exist in
// the result — there is no empty heading to omit.

namespace feathers::device {

struct CategoryGroup {
	Category category = Category::None;
	/// INDICES INTO THE OFFER, not descriptors, so a menu row's id stays the position in the list its
	/// surface already picks from by index — the `…ChoiceForTest(n)` roads are unchanged by filing.
	std::vector<size_t> members;
};

namespace detail {
inline bool nameBefore(const char* a, const char* b) {
	const char* x = a != nullptr ? a : "";
	const char* y = b != nullptr ? b : "";
	for (; *x != '\0' && *y != '\0'; ++x, ++y) {
		const int cx = std::tolower(static_cast<unsigned char>(*x));
		const int cy = std::tolower(static_cast<unsigned char>(*y));
		if (cx != cy) {
			return cx < cy;
		}
	}
	return *x == '\0' && *y != '\0';
}
} // namespace detail

/// Categories in `Category`'s declaration order (`None` last), devices ALPHABETICAL within one —
/// case-insensitively, so "SKLITTER" sorts as a word and not ahead of every lower-case letter. Equal
/// names keep their offered order. A null entry in the offer is skipped.
inline std::vector<CategoryGroup> groupByCategory(std::span<const Descriptor* const> offered) {
	std::vector<CategoryGroup> groups;
	for (size_t i = 0; i < offered.size(); ++i) {
		if (offered[i] == nullptr) {
			continue;
		}
		const Category c = offered[i]->category;
		auto at = std::find_if(groups.begin(), groups.end(), [c](const CategoryGroup& g) { return g.category == c; });
		if (at == groups.end()) {
			groups.push_back({ c, {} });
			at = groups.end() - 1;
		}
		at->members.push_back(i);
	}
	std::stable_sort(groups.begin(), groups.end(), [](const CategoryGroup& a, const CategoryGroup& b) {
		return categoryRank(a.category) < categoryRank(b.category);
	});
	for (CategoryGroup& g : groups) {
		std::stable_sort(g.members.begin(), g.members.end(), [&offered](size_t a, size_t b) {
			return detail::nameBefore(offered[a]->traits.name, offered[b]->traits.name);
		});
	}
	return groups;
}

} // namespace feathers::device
