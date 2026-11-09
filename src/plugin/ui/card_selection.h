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

#include "backplane/device_instance_id.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

// **WHICH CARDS OF ONE CHAIN A HAND HAS SELECTED** — backlog OA, D1. The
// rule, once, for every card surface (Feathers' fx lane, the chain editor, a rail's column):
//   - a PLAIN press on a card selects it alone, and it becomes the ANCHOR (this is the old focus, renamed);
//   - SHIFT selects the run from the anchor to it, in chain order, and leaves the anchor where it was;
//   - CTRL/CMD toggles it, and an added card becomes the anchor;
//   - a press that reaches a card through one of its CONTROLS (a knob) keeps a selection that already holds the
//     card, and otherwise selects it alone — so turning a knob of a selected card never drops the others;
//   - a lasso replaces the selection with what it touches, or adds to it with Shift.
// It holds ids, never positions: a device is its id wherever it stands, and a selection survives a reorder.

namespace feathers::ui {

class CardSelection {
public:
	enum class Press : uint8_t { Plain, Range, Toggle };

	/// A PRESS ON THE CARD ITSELF (its head or bare body), with the modifier the hand held. `order` is the chain's
	/// devices in order, asked now.
	void press(DeviceInstanceId id, Press kind, std::span<const DeviceInstanceId> order) {
		if (id == kNoInstance) {
			return;
		}
		switch (kind) {
		case Press::Plain:
			ids_.assign(1, id);
			anchor_ = id;
			break;
		case Press::Toggle:
			if (contains(id)) {
				std::erase(ids_, id);
				if (anchor_ == id) {
					anchor_ = ids_.empty() ? kNoInstance : ids_.back();
				}
			}
			else {
				ids_.push_back(id);
				anchor_ = id;
			}
			break;
		case Press::Range: {
			const auto a = std::find(order.begin(), order.end(), anchor_);
			const auto b = std::find(order.begin(), order.end(), id);
			if (a == order.end() || b == order.end()) {
				press(id, Press::Plain, order); // no anchor in this chain: a range from nothing is the card alone
				return;
			}
			const auto [lo, hi] = std::minmax(a, b);
			ids_.assign(lo, hi + 1);
			break;
		}
		}
	}

	/// A PRESS THAT REACHED THE CARD THROUGH ONE OF ITS CONTROLS: keep a selection holding it, else select it alone.
	void touch(DeviceInstanceId id) {
		if (id != kNoInstance && !contains(id)) {
			ids_.assign(1, id);
			anchor_ = id;
		}
	}

	/// WHAT A LASSO TOUCHED, in chain order — the selection, or (`add`) added to it.
	void lasso(std::span<const DeviceInstanceId> touched, bool add) {
		if (!add) {
			ids_.clear();
		}
		for (const DeviceInstanceId id : touched) {
			if (!contains(id)) {
				ids_.push_back(id);
			}
		}
		if (!touched.empty() && (anchor_ == kNoInstance || !contains(anchor_))) {
			anchor_ = touched.front();
		}
		if (ids_.empty()) {
			anchor_ = kNoInstance;
		}
	}

	/// THESE, AND ONLY THESE — what a verb hands back (the pasted or duplicated copies, a group's container).
	void set(std::span<const DeviceInstanceId> ids) {
		ids_.assign(ids.begin(), ids.end());
		anchor_ = ids_.empty() ? kNoInstance : ids_.back();
	}

	void clear() noexcept {
		ids_.clear();
		anchor_ = kNoInstance;
	}

	/// DROP WHAT THE CHAIN NO LONGER HOLDS — after any edit, an undo included.
	bool prune(std::span<const DeviceInstanceId> order) {
		const size_t before = ids_.size();
		std::erase_if(ids_, [&](DeviceInstanceId id) { return std::find(order.begin(), order.end(), id) == order.end(); });
		if (anchor_ != kNoInstance && !contains(anchor_)) {
			anchor_ = ids_.empty() ? kNoInstance : ids_.back();
		}
		return ids_.size() != before;
	}

	friend bool operator==(const CardSelection&, const CardSelection&) = default;

	[[nodiscard]] bool contains(DeviceInstanceId id) const noexcept {
		return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
	}
	[[nodiscard]] size_t size() const noexcept { return ids_.size(); }
	[[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
	[[nodiscard]] DeviceInstanceId anchor() const noexcept { return anchor_; }
	/// IN THE ORDER THE HAND PICKED THEM. A verb wants `inOrder`.
	[[nodiscard]] const std::vector<DeviceInstanceId>& ids() const noexcept { return ids_; }
	/// IN CHAIN ORDER — what every verb acts on.
	[[nodiscard]] std::vector<DeviceInstanceId> inOrder(std::span<const DeviceInstanceId> order) const {
		std::vector<DeviceInstanceId> out;
		for (const DeviceInstanceId id : order) {
			if (contains(id)) {
				out.push_back(id);
			}
		}
		return out;
	}

private:
	std::vector<DeviceInstanceId> ids_;
	DeviceInstanceId anchor_ = kNoInstance;
};

} // namespace feathers::ui
