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
#include "plugin/ui/escape_stack.h"
#include "plugin/ui/fragment_verbs.h"

#include <functional>
#include <optional>
#include <vector>
#include <visage/ui.h>

// **ONE CHAIN'S SELECTION, ITS VERBS, ITS LASSO AND ITS ESC ENTRY** — what a card surface owns for backlog OA, so
// Feathers' fx lane, the chain editor and a rail's column wire the same gestures in the same few lines (D1).
//
// **TWO ROADS REACH IT FROM ONE PRESS, AND THEIR ORDER IS THE WHOLE DIFFICULTY.** visage moves the keyboard focus
// BEFORE it delivers the press (`WindowEventHandler::handleMouseDown`), so a card hears `focusChanged` first — with
// no modifiers, and for a press on any of its controls too — and only then, for a press on its own frame, the
// press itself with the modifiers. The focus road therefore TOUCHES (keep a selection holding the card, else select
// it alone) and remembers what it replaced; the card's own press then puts that back before applying Shift or
// Ctrl. Without the put-back a Ctrl-click on an unselected card would first make it the whole selection and then
// toggle it out again.

namespace feathers::ui {

class LaneSelection {
public:
	struct Hooks {
		/// The chain's devices in order, now.
		std::function<std::vector<DeviceInstanceId>()> order;
		/// The selection changed: re-flag the cards (`FxCard::setFocused`) and repaint.
		std::function<void()> changed;
		/// Where a device's card stands, in the surface's coordinates — for the lasso. Nullopt: not on screen.
		std::function<std::optional<visage::Bounds>(DeviceInstanceId)> cardBounds;
	};

	LaneSelection(rack::FragmentSite& site, EditHistory& history, Notifier* notifier, Hooks hooks);

	[[nodiscard]] CardSelection& selection() noexcept { return selection_; }
	[[nodiscard]] const CardSelection& selection() const noexcept { return selection_; }
	[[nodiscard]] FragmentVerbs& verbs() noexcept { return verbs_; }

	/// THE EDITOR'S ESC STACK (backlog NN): a selection of two or more is one entry on it, and Esc clears it.
	void setEscapeStack(EscapeStack* stack);

	/// THE FOCUS ROAD — any press that reached the card, a knob's included. Modifier-free.
	void focusPress(DeviceInstanceId id);
	/// THE CARD'S OWN PRESS, with the modifier the hand held. A plain press on a card inside a selection of two or
	/// more keeps the selection (so the block can be dragged) and collapses to the card on a release that did not drag.
	void cardPress(DeviceInstanceId id, CardSelection::Press kind);
	/// A RELEASE OF THE CARD'S OWN PRESS THAT DID NOT DRAG.
	void cardClick(DeviceInstanceId id);

	/// THE LASSO, from bare surface background. `add` (Shift) keeps what is already selected.
	void lassoBegin(visage::Point at, bool add);
	void lassoMove(visage::Point at);
	void lassoEnd();
	[[nodiscard]] std::optional<visage::Bounds> lassoRect() const;

	/// THE KEYS: the verbs' (Ctrl/Cmd + A C X V D G), and Delete/Backspace over a selection of two or more. True
	/// when answered; a single selected card's Delete is left to the surface's own road.
	bool key(const visage::KeyEvent& e);

	/// WHAT A CARD'S MENU ACTS ON: the selection when it holds `pressed`, else `pressed` alone — in chain order.
	[[nodiscard]] std::vector<DeviceInstanceId> menuIds(DeviceInstanceId pressed) const;
	/// THE CARD'S MENU, WITH THE VERBS APPENDED (from `firstId`); and the pick, which selects what it made.
	void addVerbs(visage::PopupMenu& menu, int firstId, DeviceInstanceId pressed) const;
	bool chooseVerb(int id, int firstId, DeviceInstanceId pressed);

	/// THESE AND ONLY THESE, and the surface told. After an edit, `prune` drops what the chain no longer holds.
	void select(std::span<const DeviceInstanceId> ids);
	void prune();
	void clear();

	/// THE DRAG OF A SELECTED CARD MOVES THE WHOLE SELECTION — the ids to move when `dragged` is pressed.
	[[nodiscard]] bool movesBlock(DeviceInstanceId dragged) const {
		return selection_.size() > 1 && selection_.contains(dragged);
	}

	[[nodiscard]] bool escapeHeldForTest() const { return ticket_.held(); }

private:
	void after();
	void syncTicket();

	FragmentVerbs verbs_;
	Hooks hooks_;
	CardSelection selection_;
	EscapeStack* escape_ = nullptr;
	EscapeTicket ticket_;
	/// WHAT THE FOCUS ROAD REPLACED, for the card's own press to put back (see the banner).
	/// **AND THE FOCUS ROAD FIRES TWICE FOR ONE PRESS ON THE CARD'S FRAME** — `focusChanged`, then `noteCardPress`
	/// itself calls `onCardPressed` — so a second call for the same card that finds the selection exactly as the first
	/// left it is the same press, and keeps the first one's `selection` (the second would capture the touch's result).
	struct Before {
		DeviceInstanceId id = kNoInstance;
		CardSelection selection;
		CardSelection after;
	};
	std::optional<Before> before_;
	DeviceInstanceId collapseOnClick_ = kNoInstance;
	std::optional<visage::Point> lassoFrom_;
	visage::Point lassoTo_ { 0.0f, 0.0f };
	bool lassoAdd_ = false;
	CardSelection lassoBase_;
};

/// THE LASSO'S RECTANGLE AND THE "N SELECTED" PILL, drawn ABOVE the cards — visage paints children after their
/// parent, so a surface's own draw would be under every card. Ignores the mouse; the surface keeps it last.
class SelectionOverlay final : public visage::Frame {
public:
	explicit SelectionOverlay(const LaneSelection& selection) : selection_(selection) {
		setIgnoresMouseEvents(true, false);
	}
	void draw(visage::Canvas& canvas) override;
	/// WHERE THE PILL STARTS — the lane's own left edge, so it is not drawn over a column beside the cards.
	void setPillLeft(float x) {
		pillLeft_ = x;
		redraw();
	}
	/// **A SECOND CHAIN ON THE SAME SURFACE** (backlog OA: a site panel's POST beside its PRE): its lasso and its pill
	/// are drawn here too. One chain's selection at a time is the surface's rule, so one pill is ever shown.
	void setSecond(const LaneSelection* second) {
		second_ = second;
		redraw();
	}

private:
	const LaneSelection& selection_;
	const LaneSelection* second_ = nullptr;
	float pillLeft_ = 6.0f;
};

} // namespace feathers::ui
