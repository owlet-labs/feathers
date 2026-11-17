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
// hold, with the Oscillators listed first so a Drum is one click away. A pick makes the chain on that pad's key holding the picked device
// (one undo step); closing the menu makes nothing. The Rack card and the Rack window each own one, over their own adapter, so the two
// surfaces cannot disagree about what "+" does.

namespace feathers::ui {

class EditHistory;
class Notifier;

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

} // namespace feathers::ui
