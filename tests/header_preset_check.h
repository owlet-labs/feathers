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
#include "menu_placement_check.h"
#include "plugin/preset_store.h"
#include "plugin/ui/device_preset_menu.h"
#include "plugin/ui/lane_selection.h"
#include "plugin/ui/preset_browser.h"

#include <string>
#include <vector>

// **THE HEADER'S "Presets..." ROW, DRIVEN THE WAY A HAND DRIVES IT.** The row opens the full preset box's own popup (the browser's list, with
// its views by folder and by tag, its Manage submenu and "Save as..."), and "Save as..." opens the box's own dialog with a name AND a tags field.
// These helpers pick the rows through the popups and type into the dialog's fields, so a case asserts what the user would see.

namespace header_presets {

inline const std::string kSaveAs = "Save as\xe2\x80\xa6";

/// Pick "Presets..." from the card menu `menu`: the box's list opens as the newest popup (null when it did not).
inline visage::PopupMenuFrame* openBox(visage::PopupMenuFrame& menu, visage::Frame& anyFrame) {
	if (!menu_check::pickRow(menu, "Presets...")) {
		return nullptr;
	}
	menu.timerCallback(); // the card menu finishes closing, as it does a moment after a pick
	return menu_check::newestPopup(anyFrame);
}

/// From the box's list `list`: pick "Save as...", fill the dialog's name and tags, and submit it. The browser, or null when a step failed.
inline feathers::ui::PresetBrowser* saveThroughDialog(visage::PopupMenuFrame& list, feathers::ui::LaneSelection& selection, const std::string& name,
                                                      const std::string& tags) {
	feathers::ui::PresetBrowser* box = selection.verbs().presets().browser();
	if (box == nullptr || !menu_check::pickRow(list, kSaveAs)) {
		return nullptr;
	}
	list.timerCallback(); // the box's list finishes closing
	box->typeForTest(name);
	box->typeTagsForTest(tags);
	box->submitPrompt();
	return box;
}

/// Whether the store holds `name` in `bank` with exactly the tag `tag` among its tags.
inline bool storedWithTag(const std::string& bank, const std::string& name, const std::string& tag) {
	for (const feathers::StoredPreset& stored : feathers::PresetStore::loadAll(bank)) {
		if (stored.name != name) {
			continue;
		}
		for (const std::string& t : stored.meta.tags) {
			if (t == tag) {
				return true;
			}
		}
	}
	return false;
}

} // namespace header_presets
