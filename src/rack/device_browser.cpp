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

#include "rack/device_browser.h"

#include "device/device_groups.h"

#include <algorithm>

namespace feathers::rack {

std::vector<const device::Descriptor*> offeredDevices(std::span<const device::Descriptor> devices) {
	std::vector<const device::Descriptor*> offered;
	for (const device::Descriptor& d : devices) {
		// WHAT THE RACK CAN RUN, ASKED OF THE DEVICE. Null IS the capability in both cases, so a
		// device that is neither an audio device nor a modulator is not offered — see the header on
		// why advertising one would be the honesty rule's failure case.
		if (d.audio == nullptr && d.mod == nullptr) {
			continue;
		}
		if (d.traits.tag == nullptr || d.traits.name == nullptr) {
			continue;
		}
		offered.push_back(&d);
	}
	return offered;
}

std::vector<BrowserGroup> browseDevices(std::span<const device::Descriptor> devices) {
	const std::vector<const device::Descriptor*> offered = offeredDevices(devices);

	// FILED BY CATEGORY, THE SAME WAY EVERY POPUP ADD MENU IS — the 2026-08-22 tags-first grouping is
	// superseded by the decision of 2026-09-24. Headings in
	// `device::Category` order, devices alphabetical within one, a device that declares none under
	// "Other". This replaced grouping by `tags.front()` in registry order: one taxonomy on every add
	// surface, rather than the rack's browser keeping a second one of free strings.
	std::vector<BrowserGroup> groups;
	for (const device::CategoryGroup& g : device::groupByCategory(offered)) {
		BrowserGroup group { device::categoryName(g.category), {} };
		for (const size_t i : g.members) {
			group.entries.push_back(BrowserEntry { offered[i]->traits.tag, offered[i]->traits.name });
		}
		groups.push_back(std::move(group));
	}
	return groups;
}

} // namespace feathers::rack
