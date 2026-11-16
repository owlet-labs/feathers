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

#include "rack/parallel_rack.h"

#include <array>
#include <cstddef>

// ADD-MENU SHORTCUTS: a device the menu offers once, set up on the way in.
//
// "Tabs" and "Pads" under Containers are a Rack that arrives already in that view: Tabs is the Tabs view with Parallel routing, Pads the Pads
// view with Keyed routing. They are not devices of their own (nothing is stored under another tag): picking one adds a Rack and writes its
// two controls, the way a hand would have. Every add menu that offers the Rack offers them, after the devices in its list, so a pick's id is
// `offered.size() + k` and a menu that does not offer the Rack offers neither.

namespace feathers::ui {

struct AddShortcut {
	const char* name;
	const char* tag; ///< the device it adds
	size_t view;     ///< control index and value of its two settings
	float viewValue;
	size_t mode;
	float modeValue;
};

inline constexpr std::array<AddShortcut, 2> kAddShortcuts { {
	{ "Tabs", "RACK", static_cast<size_t>(rack::parallel::Param::View), 1.0f, static_cast<size_t>(rack::parallel::Param::Mode),
	  static_cast<float>(rack::parallel::Mode::Parallel) },
	{ "Pads", "RACK", static_cast<size_t>(rack::parallel::Param::View), 2.0f, static_cast<size_t>(rack::parallel::Param::Mode),
	  static_cast<float>(rack::parallel::Mode::Keyed) },
} };

/// The shortcut a menu id names in a menu that offered `offered` devices, or null.
[[nodiscard]] inline const AddShortcut* shortcutOfId(size_t offered, int id) {
	if (id < 0 || static_cast<size_t>(id) < offered || static_cast<size_t>(id) - offered >= kAddShortcuts.size()) {
		return nullptr;
	}
	return &kAddShortcuts[static_cast<size_t>(id) - offered];
}

} // namespace feathers::ui
