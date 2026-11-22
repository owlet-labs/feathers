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

#include "backplane/device_host.h"
#include "device/subscribers.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <functional>
#include <memory>

// WHAT IS IN FRONT OF A RACK CARD IN ITS TABS AND PADS VIEWS: the REAL cards of one chain, the viewed one.
//
// **THE SAME LANE AS EVERY OTHER CHAIN, NOT A SECOND CARD HOST.** The Rack's window shows each chain as an `FxLaneRail` over a `ChainColumn`
// (the column that builds one card per device of an inner chain, with its own modulation state, its undo forwarding, its panels escalated
// to the outermost host and its drag, slot menu and (+)). This holds the same column over the viewed chain, with the rail's chrome taken
// away and the host's say over it (`ChainColumn::setPadding`, `setShownLimit`, `setAddShown`): the chain CLOSED shows its first card alone,
// OPEN all of them, and a card is a card — its controls, its modulation targets, its presets, its expand to a window.
//
// **THE CHAIN IS KEPT BY ITS ID AND ITS BRIDGE, NEVER BY ITS PLACE.** `follow` asks the rack afresh every tick for the chain at the viewed
// index and compares the chain's stable id and its bridge's address with what the column was built over; either differing drops the column
// and builds another, so a tab switch, a removal under the view, an undone removal (the same id on a new bridge) or a load can never leave
// a card bound to another chain's device. The cards inside are keyed by their DEVICE'S id by the column itself.

namespace feathers::ui {

class RackFrontColumn final : public visage::Frame {
public:
	RackFrontColumn(backplane::DeviceHost& host, DeviceInstanceId rack, WidgetRegistry& registry, Notifier* notifier);
	~RackFrontColumn() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// BIND TO THE CHAIN AT `viewed`, ALL OF IT WHEN `open` (else its first card). True when anything changed (a column built or dropped, the
	/// shown count moved).
	bool follow(size_t viewed, bool open, bool shown = true);
	/// Advance the cards (the column syncs to the chain first); how many frames said the screen is out of date.
	size_t updateCards();
	/// The rack card's modulation state and history (the column's own state is a child of it), who holds the chain, where panels go, which nest.
	void setOuterModulation(ModState* mod, EditHistory* history, std::function<DeviceInstanceId()> holder);
	void setPresentedOn(SurfaceLayers* host);
	void setNest(DeviceNest nest);

	/// **THE FRONT RUNS WITH THE CARD'S FLOW**: down a column in a top-down lane, along a row in a left-right one (the cards then take the
	/// card's height as their cross extent).
	void setHorizontal(bool horizontal);
	/// How far the cards need to run ALONG the flow when the card gives them `cross` across it: the shown cards and their gaps, the (+) of an
	/// empty chain, or a line saying why not.
	[[nodiscard]] float naturalExtent(float cross) const;
	/// How many devices the viewed chain holds (the arrow is offered when it holds more than the one in front), and how many cards are shown.
	[[nodiscard]] size_t deviceCount() const;
	[[nodiscard]] size_t shownCount() const;
	/// The stable id of the chain it is bound to (0xFFFF: none) and its column, for the cases.
	[[nodiscard]] uint16_t chainIdForTest() const noexcept { return chainId_; }
	[[nodiscard]] ChainColumn* columnForTest() noexcept { return column_.get(); }
	/// A key meant for the viewed chain (Ctrl+C / X / V ...): handed to its column, whose selection acts on the chain. False when there is none.
	bool processKeyForChain(const visage::KeyEvent& e) { return column_ != nullptr && column_->processKeyPress(e); }
	/// The padding between the cards (and none around them: a member card is as wide as the rack card's content).
	static constexpr float kGap = 6.0f;

private:
	void dropColumn();
	void bindHistory();

	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	WidgetRegistry& registry_;
	Notifier* notifier_;
	/// THE COLUMN'S OWN STATE, declared BEFORE the column so they outlive it (its knobs watch the state and unwatch in their destructors).
	ModState mod_;
	EditHistory history_;
	EditHistory* outerHistory_ = nullptr;
	SurfaceLayers* presenter_ = nullptr;
	DeviceNest nest_ {};
	rack::RackBridge* bridge_ = nullptr;
	uint16_t chainId_ = 0xFFFF;
	bool open_ = false;
	bool shown_ = true;
	bool horizontal_ = false;
	std::unique_ptr<ChainColumn> column_;
	/// **THE RACK'S CHAINS REPLACED UNDER THE COLUMN** (a preset, an undo, a load): the column over the old chain's bridge is let go the moment it
	/// happens, while that chain still stands, not at the next tick when it may have been freed. Keyed on the Rack's serial, never its address.
	device::Subscription replaced_;
	uint64_t subscribedTo_ = 0;
};

} // namespace feathers::ui
