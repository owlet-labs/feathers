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

#include "plugin/ui/rack_view_adapter.h"

#include <visage/ui.h>

#include <cstddef>
#include <memory>

// THE "+" OF AN EMPTY PAD: WHICH DEVICE, ASKED BEFORE ANYTHING IS MADE.
//
// A press on the "+" opens the add menu every lane uses (`addCategorisedDevices`, one submenu per category) over what a chain made now could
// hold, in the lanes' category order. A pick makes the chain on that pad's key holding the picked device
// (one undo step); closing the menu makes nothing. The Rack card and the Rack window each own one, over their own adapter, so the two
// surfaces cannot disagree about what "+" does.

namespace feathers::ui {

class EditHistory;
class Notifier;
class TextField;

class RackPadAdd {
public:
	RackPadAdd(RackViewAdapter& model, Notifier* notifier) : model_(model), notifier_(notifier) {}

	void setHistory(EditHistory* history) noexcept { history_ = history; }
	/// Called after a pick made a chain (the owner follows its rails and repaints); `slot` is the new chain.
	void onAdded(std::function<void(size_t slot)> callback) { added_ = std::move(callback); }

	/// OPEN THE MENU FOR THE PAD ON `key`, hung off `anchor` (in `frame`'s coordinates).
	void open(int key, visage::Frame& frame, const visage::Bounds& anchor);
	/// THE MENU AS BUILT, and a pick as the menu's selection delivers it (the id is the menu row's): the chain's slot, or `kNoChain` when the
	/// pick named nothing or the chain refused the device.
	[[nodiscard]] visage::PopupMenu menuForTest(int key);
	size_t pickForTest(int key, int id);
	/// The device tags the menu offers, in the order its rows are numbered.
	[[nodiscard]] std::vector<const device::Descriptor*> offeredForTest() const { return model_.addableDevices(); }

private:
	[[nodiscard]] visage::PopupMenu build(int key, std::vector<const device::Descriptor*> offered);
	size_t pick(int key, const std::vector<const device::Descriptor*>& offered, int id);

	RackViewAdapter& model_;
	Notifier* notifier_ = nullptr;
	EditHistory* history_ = nullptr;
	std::function<void(size_t)> added_;
	/// The menu outlives a press and is parented on the root, so its pick holds this weakly.
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
};

/// **THE CHAIN'S RENAME, from its tab, its pad or its strip**: a context menu ("Rename…", and "Clear name" while it has one) and the inline field the
/// macro rename uses, opened over the thing that was pressed. The name goes to the rack (`RackViewAdapter::renameChain`: clamped, one undo step), is
/// kept with the chain's id while the field is open (a reorder under it renames the same chain), and every surface then reads it back through the
/// chain's one label. The field is the surface's child, kept for its whole life and only hidden (`text_field.h`'s rule).
class RackChainRename {
public:
	RackChainRename(RackViewAdapter& model, visage::Frame& surface, Notifier* notifier);
	~RackChainRename();

	void setHistory(EditHistory* history) noexcept { history_ = history; }
	/// THE MENU for chain `slot`, hung off `anchor` in `source`'s coordinates.
	void showMenu(size_t slot, visage::Frame& source, const visage::Bounds& anchor);
	/// THE FIELD over `anchor` (in `source`'s coordinates), holding the chain's current label.
	void begin(size_t slot, visage::Frame& source, const visage::Bounds& anchor);
	[[nodiscard]] bool renaming() const;
	[[nodiscard]] TextField& field() const noexcept { return *field_; }
	[[nodiscard]] visage::PopupMenu menuForTest(size_t slot);
	/// A commit as the field's Enter makes it, for the chain the field was opened on.
	bool commitForTest(const std::string& text) { return commit(text); }
	/// The chain id the open field belongs to (0xFFFF: none).
	[[nodiscard]] uint16_t chainForTest() const noexcept { return chain_; }

private:
	[[nodiscard]] visage::PopupMenu build(size_t slot, visage::Frame* source, visage::Bounds anchor);
	bool commit(const std::string& text);

	RackViewAdapter& model_;
	visage::Frame& surface_;
	Notifier* notifier_ = nullptr;
	EditHistory* history_ = nullptr;
	std::unique_ptr<TextField> field_;
	uint16_t chain_ = 0xFFFF;
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
};

} // namespace feathers::ui
