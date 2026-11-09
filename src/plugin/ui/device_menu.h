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

#include "device/device_groups.h"

#include <visage/ui.h>

#include <vector>

// THE ADD MENU'S SHAPE, WRITTEN ONCE FOR EVERY LANE THAT HAS ONE — the fx lane, the modulator strip and
// a chain's column (which is also SKLITTER's inner lane) all file their offer through this, so the three
// cannot drift into three taxonomies.
//
// THE RULES, AND THEY ARE THE SAME EVERYWHERE:
//   - one submenu per category, in `device::Category` order, devices alphabetical inside;
//   - a category with ONE device is still a submenu — the heading is where a player learns what kind of
//     thing it is, and a menu whose rows are sometimes headings and sometimes devices has two shapes;
//   - a menu whose whole offer is ONE category is FLAT (the modulator strip, which offers modulators
//     only): a single heading would be one extra hover that tells the player nothing;
//   - a category nothing is eligible for is absent, because `groupByCategory` never makes it.
//
// A ROW'S ID IS ITS INDEX IN THE OFFER, so the root's `onSelection` — which visage calls for a pick at
// any depth — reads exactly what it read when the menu was flat. `enabled(i)` greys a device, never a
// heading: a disabled heading cannot be hovered open, and "Chain full" should still show a player what
// they could have had.

namespace feathers::ui {

template <typename Enabled>
void addCategorisedDevices(visage::PopupMenu& menu, const std::vector<const device::Descriptor*>& offered,
                           Enabled&& enabled) {
	const std::vector<device::CategoryGroup> groups = device::groupByCategory(offered);
	const auto row = [&](visage::PopupMenu& into, size_t i) {
		into.addOption(static_cast<int>(i), offered[i]->traits.name).enable(enabled(i));
	};
	if (groups.size() == 1) {
		for (const size_t i : groups.front().members) {
			row(menu, i);
		}
		return;
	}
	for (const device::CategoryGroup& g : groups) {
		visage::PopupMenu sub(device::categoryName(g.category));
		for (const size_t i : g.members) {
			row(sub, i);
		}
		menu.addSubMenu(std::move(sub));
	}
}

} // namespace feathers::ui
