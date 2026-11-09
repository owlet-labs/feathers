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

#include <span>
#include <string>
#include <vector>

// WHAT A RACK CAN BE GIVEN, GROUPED THE WAY THE DEVICES ASK TO BE.
//
// The device browser's tree is tags first, directory as fallback: a node is a pinned tag, and anything
// with no pinned tags falls back to its position in the directory structure. This is that, at one level — which is all nine devices need,
// and the shape a deeper tree would nest rather than replace.
//
// IT IS A PURE FUNCTION OVER THE REGISTRY AND HOLDS NO STATE, which is what lets a venue check the
// grouping without a window. The widget that draws it is a separate thing and should stay one: this
// is the answer, not the picture of it.
//
//------------------------------------------------------------------------------------------------//
// WHAT IT REFUSES TO SHOW, AND WHY THAT IS THE INTERESTING PART                                    //
//------------------------------------------------------------------------------------------------//
//
// A device the rack cannot RUN must not be offered. `feathers_clap.cpp` already refuses to enumerate
// one — *"`audio == nullptr` IS the 'cannot be an audio device' declaration... so enumerating them
// here would advertise tags the very next call rejects"* — and the same rule applies with `ModOps`
// beside it: what the rack can host is `audio != nullptr || mod != nullptr`, asked of the device.
//
// THE HONESTY RULE, APPLIED TO A LIST. An entry that cannot be added is a control offering something
// it cannot do, and it fails in the worst way — the click looks like it worked and nothing appears.

namespace feathers::rack {

/// One thing a player can pick.
struct BrowserEntry {
	/// The device's four-byte tag — what `addDevice` takes.
	const char* deviceTag = nullptr;
	/// What the row says.
	const char* name = nullptr;
};

/// One group, named by a tag a device declared.
struct BrowserGroup {
	std::string name;
	std::vector<BrowserEntry> entries;
};

/// EVERY DEVICE THIS BUILD CAN PUT IN A RACK, grouped by its declared `device::Category`.
///
/// **THE 2026-08-22 "TAGS FIRST" GROUPING ABOVE IS SUPERSEDED BY THE DECISION OF 2026-09-24**
/// (this browser switches to categories): every add surface files by
/// category now — headings in category order, devices alphabetical — through `device::groupByCategory`,
/// the one grouping the popup add menus also use. Tags stay for search and for the strip's modulator
/// filter. Each device appears once, because it declares one category.
///
/// A DEVICE WITH NO CATEGORY GROUPS UNDER "Other" rather than being dropped — a guest that declared
/// none is exactly who lands there.
std::vector<BrowserGroup> browseDevices(std::span<const device::Descriptor> devices);

/// THE SAME OFFER, UNGROUPED — what `browseDevices` files, in registry order. The rack's (+) and its "Add
/// before…" hand this to `ui::addCategorisedDevices`, the popup Feathers' fx lane uses (backlog KP), so the
/// two plugins' add menus are one menu; `browseDevices` keeps the grouped form for the (+) row's own list.
std::vector<const device::Descriptor*> offeredDevices(std::span<const device::Descriptor> devices);

} // namespace feathers::rack
