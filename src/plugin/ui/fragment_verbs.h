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

#include "plugin/ui/card_selection.h"
#include "plugin/ui/edit_history.h"
#include "rack/fragment.h"

#include <functional>
#include <span>
#include <string>
#include <vector>
#include <visage/ui.h>

namespace feathers::ui {

class Notifier;

/// **THE VERBS A SELECTION OFFERS, ONCE FOR EVERY CARD SURFACE** (backlog OA) — copy, cut, paste, duplicate, remove,
/// Group into Chain, Group into Rack and the block move, each ONE undo step that marks the host dirty (through the
/// site's `fragmentChanged`), each saying in the tray what it could not carry. A surface owns one of these over its
/// bridge, hands it its selection, and builds its menu and its keys from it, so the three surfaces cannot offer three
/// different sets of verbs.
class FragmentVerbs {
public:
	/// `order` answers the chain's devices in order, now. `notifier` may be null (a case).
	FragmentVerbs(rack::FragmentSite& site, EditHistory& history, Notifier* notifier,
	              std::function<std::vector<DeviceInstanceId>()> order)
	    : site_(site), history_(history), notifier_(notifier), order_(std::move(order)) {}

	[[nodiscard]] std::vector<DeviceInstanceId> order() const { return order_(); }

	/// Each returns what the selection should become: the copies, the container, nothing.
	void copy(std::span<const DeviceInstanceId> ids);
	std::vector<DeviceInstanceId> cut(std::span<const DeviceInstanceId> ids);
	/// After `after` (an id this chain holds), or at the end when it is `kNoInstance`.
	std::vector<DeviceInstanceId> paste(DeviceInstanceId after);
	std::vector<DeviceInstanceId> duplicate(std::span<const DeviceInstanceId> ids);
	size_t remove(std::span<const DeviceInstanceId> ids);
	/// **DEVICES ALREADY ADDED, RECORDED AS ONE STEP** (backlog JB: the rack editor's (+) and "Add before…"): undo removes
	/// them, redo puts them back where they stood under the same ids. Nothing is recorded for an id the chain lacks.
	void recordAdded(std::span<const DeviceInstanceId> ids, const char* verb);
	std::vector<DeviceInstanceId> group(std::span<const DeviceInstanceId> ids, rack::GroupKind kind);
	bool moveBlock(std::span<const DeviceInstanceId> ids, size_t gap);
	/// **A MOVE INTO ANOTHER CHAIN** (backlog NZ: a site's PRE and POST, which share one id counter, so every device
	/// keeps its id). The devices leave this chain and land in `to` at `position`, as ONE undo step in this history.
	/// Routes among them travel; a route from an outer source (the host's) is kept where `to`'s gate admits it; the
	/// rest cannot be spelled in `to` and are said, not dropped silently. All or nothing: if `to` refuses a device,
	/// nothing moves. Returns the ids as they landed.
	std::vector<DeviceInstanceId> moveInto(rack::FragmentSite& to, std::span<const DeviceInstanceId> ids, size_t position);

	[[nodiscard]] std::string groupRefusal(std::span<const DeviceInstanceId> ids) const {
		return rack::groupRefusal(site_, ids);
	}
	[[nodiscard]] static bool canPaste() noexcept { return rack::pasteboardHolds(); }

	/// THE VERBS AS A MENU — appended after whatever the card's own menu already holds, acting on `ids` (the
	/// selection when the pressed card is in it, else that card alone). `select` hands back what the selection
	/// becomes. "Remove N" is offered only for two or more: one card keeps the surface's own Remove and its road.
	struct MenuHooks {
		std::function<void(std::vector<DeviceInstanceId>)> select;
	};
	/// The option ids this adds start at `firstId`; `choose` runs the one picked.
	void addTo(visage::PopupMenu& menu, int firstId, std::span<const DeviceInstanceId> ids) const;
	bool choose(int id, int firstId, std::span<const DeviceInstanceId> ids, const MenuHooks& hooks);

	/// THE KEYS (a bonus: a DAW may keep them): Ctrl/Cmd + C, X, V, D, G, Shift+G, A. True when one was answered.
	/// `selection` is the surface's, edited in place (A selects every card; a verb selects what it made).
	bool key(const visage::KeyEvent& e, CardSelection& selection);

	/// THE TRAY'S LAST WORD, for a case.
	[[nodiscard]] const std::string& lastSaidForTest() const noexcept { return lastSaid_; }

private:
	void say(std::string text);

	rack::FragmentSite& site_;
	EditHistory& history_;
	Notifier* notifier_ = nullptr;
	std::function<std::vector<DeviceInstanceId>()> order_;
	std::string lastSaid_;
};

/// THE MENU'S LABELS, in the order `addTo` adds them — for a case that picks one by name.
enum class FragmentVerb : int { Copy = 0, Cut, Paste, Duplicate, GroupChain, GroupRack, Remove, Count };

} // namespace feathers::ui
