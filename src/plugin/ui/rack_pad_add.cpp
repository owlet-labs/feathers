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

#include "plugin/ui/rack_pad_add.h"

#include "plugin/ui/device_menu.h"
#include "plugin/ui/device_shortcuts.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/notify_tray.h"

namespace feathers::ui {

visage::PopupMenu RackPadAdd::build(int key, std::vector<const device::Descriptor*> offered) {
	visage::PopupMenu menu;
	// THE LANES' OWN BUILDER, with the oscillators first.
	addCategorisedDevices(menu, offered, [](size_t) { return true; }, device::Category::Oscillators);
	const std::weak_ptr<void> alive = alive_;
	menu.onSelection() = [this, alive, key, offered = std::move(offered)](int id) {
		if (!alive.expired()) {
			(void)pick(key, offered, id);
		}
	};
	return menu;
}

size_t RackPadAdd::pick(int key, const std::vector<const device::Descriptor*>& offered, int id) {
	const AddShortcut* shortcut = shortcutOfId(offered.size(), id);
	if (shortcut == nullptr && (id < 0 || static_cast<size_t>(id) >= offered.size())) {
		return kNoChain; // a close, or a heading
	}
	const char* tag = shortcut != nullptr ? shortcut->tag : offered[static_cast<size_t>(id)]->traits.tag;
	const size_t at = model_.addKeyedChain(key, tag, history_);
	if (at == kNoChain) {
		if (notifier_ != nullptr) {
			notifier_->say("That device cannot go in a chain here; nothing added");
		}
		return kNoChain;
	}
	// A SHORTCUT IS THE DEVICE, SET UP ON THE WAY IN (`device_shortcuts.h`).
	if (shortcut != nullptr) {
		model_.setFirstDeviceField(at, shortcut->view, shortcut->viewValue);
		model_.setFirstDeviceField(at, shortcut->mode, shortcut->modeValue);
	}
	model_.viewChain(at);
	if (added_) {
		added_(at);
	}
	if (notifier_ != nullptr) {
		notifier_->say("Chain added");
	}
	return at;
}

void RackPadAdd::open(int key, visage::Frame& frame, const visage::Bounds& anchor) {
	std::vector<const device::Descriptor*> offered = model_.addableDevices();
	if (offered.empty()) {
		return;
	}
	showMenuAt(build(key, std::move(offered)), frame, anchor);
}

visage::PopupMenu RackPadAdd::menuForTest(int key) {
	return build(key, model_.addableDevices());
}

size_t RackPadAdd::pickForTest(int key, int id) {
	return pick(key, model_.addableDevices(), id);
}

} // namespace feathers::ui
