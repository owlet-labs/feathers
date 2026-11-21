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

#include "plugin/ui/device_rename.h"

#include "plugin/ui/menu_anchor.h"

#include <algorithm>

namespace feathers::ui {

namespace {

/// ONE UNDO STEP FOR A RENAME: the name before and after, on the host that holds the instance.
class DeviceNameEdit final : public EditRecord {
public:
	DeviceNameEdit(backplane::DeviceHost& host, DeviceInstanceId device, std::string before, std::string after)
	    : host_(host), device_(device), before_(std::move(before)), after_(std::move(after)) {}
	void undo() const override { (void)host_.setUserName(device_, before_); }
	void redo() const override { (void)host_.setUserName(device_, after_); }
	const char* verb() const override { return "Rename"; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	std::string before_;
	std::string after_;
};

} // namespace

DeviceRename::DeviceRename(visage::Frame& surface, Notifier* notifier) : surface_(surface), notifier_(notifier) {
	field_ = std::make_unique<TextField>();
	field_->onCommit([this](std::string text) {
		(void)commit(text);
		field_->setVisible(false);
	});
	field_->onCancel([this] { field_->setVisible(false); });
	field_->setVisible(false);
	// ABOVE EVERY SIBLING: a field added once sat under the cards added after it (the chain rename's lesson).
	field_->setOnTop(true);
	surface_.addChild(field_.get());
}

DeviceRename::~DeviceRename() {
	field_->onCommit(nullptr);
	field_->onCancel(nullptr);
	if (field_->isVisible()) {
		field_->setVisible(false);
	}
	surface_.removeChild(field_.get());
}

void DeviceRename::addRows(visage::PopupMenu& menu, const backplane::DeviceHost& host, DeviceInstanceId device) const {
	menu.addOption(kRenameRow, "Rename...");
	menu.addOption(kClearRow, "Clear name").enable(!host.userNameOf(device).empty());
}

bool DeviceRename::pick(int row, backplane::DeviceHost& host, DeviceInstanceId device, visage::Frame& source, const visage::Bounds& anchor) {
	if (row == kRenameRow) {
		begin(host, device, source, anchor);
		return true;
	}
	if (row == kClearRow) {
		host_ = &host;
		device_ = device;
		(void)commit(std::string {});
		return true;
	}
	return false;
}

bool DeviceRename::commit(const std::string& text) {
	if (host_ == nullptr || device_ == kNoInstance) {
		return false;
	}
	// TRIMMED; an empty name clears. The host clamps.
	size_t a = 0;
	size_t b = text.size();
	while (a < b && static_cast<unsigned char>(text[a]) <= ' ') {
		++a;
	}
	while (b > a && static_cast<unsigned char>(text[b - 1]) <= ' ') {
		--b;
	}
	const std::string next = text.substr(a, b - a);
	const std::string before = host_->userNameOf(device_);
	if (!host_->setUserName(device_, next)) {
		return false; // unchanged, or the device is gone
	}
	if (history_ != nullptr) {
		history_->record(std::make_unique<DeviceNameEdit>(*host_, device_, before, host_->userNameOf(device_)));
	}
	if (notifier_ != nullptr) {
		notifier_->say(next.empty() ? "Name cleared" : "Renamed");
	}
	return true;
}

void DeviceRename::begin(backplane::DeviceHost& host, DeviceInstanceId device, visage::Frame& source, const visage::Bounds& anchor) {
	host_ = &host;
	device_ = device;
	const visage::Point from = source.positionInWindow();
	const visage::Point to = surface_.positionInWindow();
	constexpr float kFieldHeight = 24.0f; // one line, whatever the header is
	// INSIDE THE SURFACE WHATEVER THE ANCHOR: a card at an edge, or a wide head, must not put the field half outside the window.
	const float sw = static_cast<float>(surface_.width());
	const float sh = static_cast<float>(surface_.height());
	const float fw = std::min(std::max(anchor.width(), 120.0f), sw);
	const float fx = std::clamp(from.x - to.x + anchor.x(), 0.0f, std::max(0.0f, sw - fw));
	const float fy = std::clamp(from.y - to.y + anchor.y() + (anchor.height() - kFieldHeight) * 0.5f, 0.0f, std::max(0.0f, sh - kFieldHeight));
	field_->setBounds(fx, fy, fw, kFieldHeight);
	field_->setVisible(true);
	const device::Descriptor* desc = host.descriptorOf(device);
	const std::string current = host.userNameOf(device);
	field_->begin(current.empty() ? std::string(desc != nullptr && desc->traits.name != nullptr ? desc->traits.name : "") : current);
}

} // namespace feathers::ui
