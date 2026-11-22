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
#include "plugin/ui/text_field.h"

namespace feathers::ui {

visage::PopupMenu RackPadAdd::build(int key, std::vector<const device::Descriptor*> offered) {
	visage::PopupMenu menu;
	// THE LANES' OWN BUILDER, in the lanes' own category order (a heading never moves between menus).
	addCategorisedDevices(menu, offered, [](size_t) { return true; });
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

RackChainRename::RackChainRename(RackViewAdapter& model, visage::Frame& surface, Notifier* notifier)
    : model_(model), surface_(surface), notifier_(notifier) {
	field_ = std::make_unique<TextField>();
	field_->onCommit([this](std::string text) {
		(void)commit(text);
		field_->setVisible(false);
	});
	field_->onCancel([this] { field_->setVisible(false); });
	field_->setVisible(false);
	// **ABOVE EVERY SIBLING AND WHAT THEY HOLD.** The toolkit paints later siblings over earlier ones and a frame hits the same way, so a field
	// added once at construction sat UNDER the tab bar, the window pane and the cards that were added after it: typing landed, the box was unseen.
	field_->setOnTop(true);
	surface_.addChild(field_.get());
}

RackChainRename::~RackChainRename() {
	field_->onCommit(nullptr);
	field_->onCancel(nullptr);
	if (field_->isVisible()) {
		field_->setVisible(false);
	}
	surface_.removeChild(field_.get());
}

bool RackChainRename::renaming() const {
	return field_->editing();
}

bool RackChainRename::commit(const std::string& text) {
	const size_t slot = model_.slotOfChainId(chain_);
	if (slot == kNoChain) {
		return false; // the chain went while the field was open
	}
	// THE NAME AS TYPED, trimmed; a name equal to the derived one is the same as none to a reader, but is kept as the user's (it is theirs).
	size_t a = 0;
	size_t b = text.size();
	while (a < b && static_cast<unsigned char>(text[a]) <= ' ') {
		++a;
	}
	while (b > a && static_cast<unsigned char>(text[b - 1]) <= ' ') {
		--b;
	}
	const bool changed = model_.renameChain(slot, std::string_view(text).substr(a, b - a), history_);
	if (changed && notifier_ != nullptr) {
		notifier_->say("Chain renamed");
	}
	return changed;
}

visage::PopupMenu RackChainRename::build(size_t slot, visage::Frame* source, visage::Bounds anchor) {
	visage::PopupMenu menu;
	menu.addOption(1, "Rename...");
	menu.addOption(2, "Clear name").enable(!model_.chainUserName(slot).empty());
	const uint16_t id = model_.chainIdAt(slot);
	const std::weak_ptr<void> alive = alive_;
	menu.onSelection() = [this, alive, id, source, anchor](int row) {
		if (alive.expired()) {
			return;
		}
		const size_t at = model_.slotOfChainId(id);
		if (at == kNoChain) {
			return;
		}
		if (row == 1 && source != nullptr) {
			begin(at, *source, anchor);
		}
		else if (row == 2) {
			chain_ = id;
			(void)commit(std::string {});
		}
	};
	return menu;
}

void RackChainRename::showMenu(size_t slot, visage::Frame& source, const visage::Bounds& anchor) {
	if (slot >= model_.chainSlots()) {
		return;
	}
	showMenuAt(build(slot, &source, anchor), source, anchor);
}

visage::PopupMenu RackChainRename::menuForTest(size_t slot) {
	return build(slot, nullptr, {});
}

void RackChainRename::begin(size_t slot, visage::Frame& source, const visage::Bounds& anchor) {
	if (slot >= model_.chainSlots()) {
		return;
	}
	chain_ = model_.chainIdAt(slot);
	const visage::Point from = source.positionInWindow();
	const visage::Point to = surface_.positionInWindow();
	// ONE LINE TALL whatever the anchor is: a tab in the window's column is a whole card's height, and a field that tall is a slab.
	constexpr float kFieldHeight = 24.0f;
	const float h = kFieldHeight;
	field_->setBounds(from.x - to.x + anchor.x(), from.y - to.y + anchor.y() + (anchor.height() - h) * 0.5f, std::max(anchor.width(), 90.0f), h);
	field_->setVisible(true);
	field_->begin(model_.chainUserName(slot).empty() ? model_.chainLabel(slot) : model_.chainUserName(slot));
}

} // namespace feathers::ui
