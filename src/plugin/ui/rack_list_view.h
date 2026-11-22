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

#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/rack_pad_add.h"
#include "plugin/ui/rack_view_adapter.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// THE RACK'S LIST VIEW: ONE MINI STRIP PER CHAIN, IN A COLUMN THAT SCROLLS.
//
// It works as Tabs does (one chain is the viewed one, and its cards show beside the list), with a row in place of a tab: the chain's name, its
// mute, solo and level. A row IS a `ChainStrip` (the same controls over the same parameters, the same meter), so nothing about a chain's strip
// is written twice. A press on a row's own surface views that chain, a right press asks for the rename menu, a double-click renames; in Select
// routing the gutter at the row's left holds the playing dot, which a press there sets, as the tab's dot does.
//
// **THE LIST HAS A FIXED FOOTPRINT AND SCROLLS** when its rows do not fit: by the wheel, and by dragging a row's surface. A change of the viewed
// chain from elsewhere (the window's Viewing box, a played note that follows) scrolls the viewed row into view. The scroll position is EDITOR
// STATE, never saved: it is a view of a view, and a patch that reloads shows the list from its top, as the viewed chain asks.
// It holds no cap on rows: the rack's own count is the count.

namespace feathers::ui {

class EditHistory;
class ModState;

class RackListView final : public visage::Frame, public TooltipSource {
public:
	RackListView(RackViewAdapter& model, backplane::DeviceHost& host, DeviceInstanceId rack, RackChainRename& rename);
	~RackListView() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	bool mouseWheel(const visage::MouseEvent& e) override;
	const char* tooltipAt(visage::Point at) const override;
	/// THE PLAYING CHAIN is written through a real parameter, so the list declares it (the window must offer what the card offers).
	void offeredControls(std::vector<Offered>& out) const override;

	/// Where the rows' controls record their undo steps (and the panel state their routes are armed from).
	void setModulation(ModState* mod, EditHistory* history);
	/// What a row says besides the name, by chain slot (the owner knows the rack's role); null: the chain's label alone.
	void setSummaryFn(std::function<std::string(size_t slot)> fn) { summaryFn_ = std::move(fn); }
	/// Re-read the rack: rows follow the chains, names, the viewed chain (scrolled into view when it moved by other means), the meters.
	/// True when anything changed.
	bool refresh();

	/// The row height and the gap between rows, the gutter that holds the playing dot, and the width the list is laid out for in a column.
	static constexpr float kRowHeight = ChainStrip::kHeight;
	static constexpr float kRowGap = 4.0f;
	static constexpr float kPitch = kRowHeight + kRowGap;
	static constexpr float kGutter = 14.0f;
	static constexpr float kBarWidth = 4.0f;
	static constexpr float kColumnWidth = 250.0f;
	/// How many rows a list shows before it scrolls, where its height is the card's to choose (a card in a top-down lane).
	static constexpr size_t kCardRows = 5;
	/// The height of `rows` rows, and the most a view of this many shows.
	[[nodiscard]] static float heightFor(size_t rows) { return rows == 0 ? 0.0f : static_cast<float>(rows) * kPitch - kRowGap; }

	// ---- for the cases: what is drawn, as values ----
	[[nodiscard]] size_t rowCountForTest() const noexcept { return rows_.size(); }
	[[nodiscard]] ChainStrip* rowForTest(size_t slot) const noexcept { return slot < rows_.size() ? rows_[slot].strip.get() : nullptr; }
	[[nodiscard]] float scrollForTest() const noexcept { return scroll_; }
	void setScrollForTest(float y) { setScroll(y); }
	[[nodiscard]] float maxScrollForTest() const { return maxScroll(); }
	/// Row k's rectangle in this frame's own coordinates, as laid out now (it may lie outside the frame while scrolled away).
	[[nodiscard]] visage::Bounds rowBoundsForTest(size_t slot) const;
	/// Whether row k lies wholly inside the frame (a press on its centre reaches it).
	[[nodiscard]] bool rowVisibleForTest(size_t slot) const;
	/// The playing dot's centre for row k, and whether it is lit.
	[[nodiscard]] visage::Point dotCentreForTest(size_t slot) const;
	[[nodiscard]] bool dotLitForTest(size_t slot) const;
	/// A press and release on the gutter at `at`, as the hand makes them.
	void pressGutterForTest(visage::Point at) { pressGutter(at); }

private:
	struct Row {
		uint16_t chain = 0;
		std::unique_ptr<ChainStrip> strip;
	};
	[[nodiscard]] float maxScroll() const;
	void setScroll(float y);
	void layoutRows();
	void rebuild(const std::vector<uint16_t>& ids);
	void scrollIntoView(size_t slot);
	void surface(size_t slot, ChainStrip::Surface phase, const visage::MouseEvent& e);
	void pressGutter(visage::Point at);
	[[nodiscard]] int rowAt(visage::Point at) const;
	[[nodiscard]] bool inDot(visage::Point at, size_t slot) const;

	RackViewAdapter& model_;
	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	RackChainRename& rename_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	std::vector<Row> rows_;
	std::function<std::string(size_t)> summaryFn_;
	float scroll_ = 0.0f;
	size_t lastViewed_ = kNoChain;
	int hover_ = -1;
	// the drag that scrolls: where the hand went down, the scroll it went down at, and whether it has moved far enough to be a drag
	float downY_ = 0.0f;
	float downScroll_ = 0.0f;
	bool dragging_ = false;
	bool surfaceDown_ = false;
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
};

} // namespace feathers::ui
