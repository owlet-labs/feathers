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

#include "plugin/ui/fader_pill.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/rack_view_model.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

// THE RACK'S TAB BAR AND PAD VIEW (widgets; the model is rack_view_model.h).
//
// **TWO FLOWS.** A rack's chains run top-down or left-right. The tab bar runs ACROSS the top of the card in the first and DOWN its side
// in the second; the page strip lies across the flow too. `RackFlow` is the one switch, and every widget here reads it.
//
// **TABS** — one per EXISTING chain, at most eight, dividing the bar's extent evenly in NATIVE pixels (two chains are two large tabs, eight
// are eight small ones; never small tabs and a gap). A tab is a name and nothing else: no level, mute, solo or select control. A press
// views that chain. In Select routing the PLAYING chain wears a small dot on its tab, distinct from the viewed (raised) tab, and a press
// on another tab's dot makes it the playing chain through the real parameter.
//
// **PADS** — sixteen keys of a page, left to right then bottom to top, a used pad lit with its chain's name and an empty one dimmed with
// its note. A press triggers the key and views its chain; a struck key (a note from anywhere) flashes the pad for about 150 ms, or
// holds a single steady highlight under reduced motion. The page strip is eight thumbnails of the same sixteen cells.

namespace feathers::ui {

enum class RackFlow { TopDown, LeftRight };

/// THE EXPAND ARROW: one idiom for the front chain's "open inline" and the macro row. It points down in a top-down flow and right in a
/// left-right one while closed, and the other way while open.
class RackDrawerArrow final : public visage::Frame, public TooltipSource {
public:
	RackDrawerArrow(RackFlow flow, std::string tip, std::function<bool()> isOpen, std::function<void(bool)> setOpen);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override { return tip_.c_str(); }
	/// It declares nothing: it opens a drawer (editor state) and writes no control.
	void offeredControls(std::vector<Offered>&) const override {}
	/// Repaint after the state moved elsewhere.
	void refresh() { redraw(); }
	/// The flow changed (the card was turned): the arrow points the other way.
	void setFlow(RackFlow flow) {
		if (flow != flow_) {
			flow_ = flow;
			redraw();
		}
	}
	/// A press, as the hand makes it.
	void toggleForTest() { toggle(); }

	static constexpr float kSize = 14.0f;

private:
	void toggle();
	RackFlow flow_;
	std::string tip_;
	std::function<bool()> isOpen_;
	std::function<void(bool)> setOpen_;
};

class RackTabBar final : public visage::Frame, public TooltipSource {
public:
	RackTabBar(RackViewModel& model, RackFlow flow);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point at) const override;
	/// THE PLAYING CHAIN is written through a real parameter, so the bar declares it (the window must offer what the card offers).
	void offeredControls(std::vector<Offered>& out) const override;
	/// Re-read the model and repaint.
	void refresh() { redraw(); }
	/// The flow changed (the card was turned): the bar runs the other way; the owner sets the bounds.
	void setFlow(RackFlow flow) {
		if (flow != flow_) {
			flow_ = flow;
			redraw();
		}
	}

	/// The bar's thickness across its flow: a row of this height (top-down) or a column of this width (left-right).
	static constexpr float kThickness = 26.0f;
	/// The corner a press on which sets the playing chain (Select routing), logical pixels.
	static constexpr float kDotArea = 12.0f;

	// ---- for the cases: what is drawn, as values ----
	/// The slots that have a tab, in order.
	[[nodiscard]] std::vector<size_t> tabsForTest() const { return tabbedChains(model_); }
	/// Tab k's extent along the bar in NATIVE pixels: [begin, end).
	[[nodiscard]] std::pair<int, int> tabNativeExtent(size_t k) const;
	/// A point in tab k (logical, in this frame), and the centre of its dot area.
	[[nodiscard]] visage::Point tabCentre(size_t k) const;
	[[nodiscard]] visage::Point dotCentre(size_t k) const;
	/// Whether tab k wears the playing dot, and whether it is the raised (viewed) tab, as drawn.
	[[nodiscard]] bool dotShown(size_t k) const;
	[[nodiscard]] bool raised(size_t k) const;
	/// A press at `at`, as the hand makes it.
	void pressForTest(visage::Point at) { press(at); }
	/// The pointer over `at` (or off the bar: a point outside it), as a mouse move makes it.
	void hoverForTest(visage::Point at) { hover(at); }
	/// **THE SET-PLAYING TARGET APPEARS ON HOVER**: a lit dot on the playing tab only, and on the tab under the pointer (Select routing) a
	/// faint one where a press sets it playing. Nothing is drawn on the other tabs, so eight tabs keep their room for their names.
	[[nodiscard]] bool hoverDotShown(size_t k) const;
	/// The name tab k draws: shrunk toward the floor size and then cut with an ellipsis ("..."), never clipped mid-glyph, and how wide it may be.
	[[nodiscard]] std::string labelForTest(size_t k) const;
	[[nodiscard]] float labelRoomForTest(size_t k) const;

private:
	[[nodiscard]] bool horizontal() const noexcept { return flow_ == RackFlow::TopDown; }
	[[nodiscard]] int nativeLength() const;
	[[nodiscard]] float scale() const;
	[[nodiscard]] int tabAtNative(float along, size_t count) const;
	void hover(visage::Point at);
	[[nodiscard]] float labelRoom(size_t k) const;
	[[nodiscard]] bool inDotArea(visage::Point at, size_t k) const;
	void press(visage::Point at);

	RackViewModel& model_;
	RackFlow flow_;
	int hover_ = -1;
};

class RackPadGrid final : public visage::Frame, public TooltipSource {
public:
	RackPadGrid(RackViewModel& model, RackStrikePulse& pulse);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point at) const override;
	void refresh() { redraw(); }

	/// It declares NOTHING, and the reason is checked here: a pad press plays a key through the note road and views a chain (editor state),
	/// so it writes no device control.
	void offeredControls(std::vector<Offered>&) const override {}

	/// **A "+" ON EVERY EMPTY PAD** (the window's): pressing it asks the owner to make a chain on that pad's key. Without a handler no
	/// "+" is drawn and an empty pad only plays its key.
	void setAddHandler(std::function<void(int key)> onAdd) {
		onAdd_ = std::move(onAdd);
		redraw();
	}
	/// The "+" of empty pad `index`, as drawn (logical), and the size of its press area.
	[[nodiscard]] visage::Point plusCentre(int index) const;
	static constexpr float kPlusArea = 16.0f;

	// ---- for the cases ----
	/// The pad index 0..15 under a point (logical, in this frame), or -1.
	[[nodiscard]] int padAt(visage::Point at) const;
	/// The centre of pad `index` as it is DRAWN (bottom-left is index 0), logical.
	[[nodiscard]] visage::Point padCentre(int index) const;
	/// Pad column's native extent along x, and the row's along y (row counted from the TOP as drawn): [begin, end).
	[[nodiscard]] std::pair<int, int> columnNativeExtent(int column) const;
	[[nodiscard]] std::pair<int, int> rowNativeExtent(int rowFromTop) const;
	/// What the pad shows: used, its name or its note, its brightness now.
	[[nodiscard]] bool usedForTest(int index) const;
	[[nodiscard]] std::string labelForTest(int index) const;
	[[nodiscard]] float flashForTest(int index) const;
	[[nodiscard]] bool selectedForTest(int index) const;
	/// A press and a release at `at`, as the hand makes them.
	void pressForTest(visage::Point at) { press(at); }
	void releaseForTest() { release(); }

	static constexpr float kInset = 1.5f;

private:
	[[nodiscard]] float scale() const;
	void press(visage::Point at);
	void release();

	RackViewModel& model_;
	RackStrikePulse& pulse_;
	Watch watch_;
	int held_ = -1;
	std::function<void(int)> onAdd_;
};

class RackPadPages final : public visage::Frame, public TooltipSource {
public:
	RackPadPages(RackViewModel& model, RackStrikePulse& pulse, RackFlow flow);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point at) const override;
	/// It declares nothing: choosing a page is editor state, not a control.
	void offeredControls(std::vector<Offered>&) const override {}
	void refresh() { redraw(); }
	void setFlow(RackFlow flow) {
		if (flow != flow_) {
			flow_ = flow;
			redraw();
		}
	}

	/// The strip's thickness across the flow: a row this tall (top-down) or a column this wide (left-right).
	static constexpr float kThickness = 26.0f;

	// ---- for the cases ----
	[[nodiscard]] int pageAt(visage::Point at) const;
	[[nodiscard]] visage::Point pageCentre(int page) const;
	[[nodiscard]] std::pair<int, int> pageNativeExtent(int page) const;
	/// Whether cell `index` of thumbnail `page` is drawn lit (its key has a chain), and its brightness now from the pulse.
	[[nodiscard]] bool cellUsedForTest(int page, int index) const;
	[[nodiscard]] float cellFlashForTest(int page, int index) const;
	[[nodiscard]] bool currentForTest(int page) const;
	void pressForTest(visage::Point at) { press(at); }

private:
	[[nodiscard]] bool horizontal() const noexcept { return flow_ == RackFlow::TopDown; }
	[[nodiscard]] int nativeLength() const;
	[[nodiscard]] float scale() const;
	void press(visage::Point at);

	RackViewModel& model_;
	RackStrikePulse& pulse_;
	RackFlow flow_;
	Watch watch_;
};

/// THE PAD VIEW AS ONE PIECE: the grid, the page strip across the flow and the kit level pill, laid out for either flow. The Rack card
/// hosts it; the expand arrow and the chain beneath belong to the card.
class RackPadsPanel final : public visage::Frame {
public:
	RackPadsPanel(RackViewModel& model, RackStrikePulse& pulse, RackFlow flow);

	void resized() override;
	void refresh();
	/// The flow changed: the strip turns to the other side of the grid.
	void setFlow(RackFlow flow) {
		if (flow != flow_) {
			flow_ = flow;
			pages_.setFlow(flow);
			resized();
		}
	}

	[[nodiscard]] RackPadGrid& gridForTest() { return grid_; }
	[[nodiscard]] RackPadPages& pagesForTest() { return pages_; }
	[[nodiscard]] FaderPill* levelForTest() { return level_.get(); }

	/// A pad is this big and the grid is four of them, so the panel's natural size follows.
	static constexpr float kPad = 46.0f; // four pads and three gaps make 196: a card's width less its two margins
	static constexpr float kGap = 4.0f;
	static constexpr float kMargin = 10.0f;
	static constexpr float kLevelHeight = 22.0f;
	[[nodiscard]] static float gridSide() { return 4.0f * kPad + 3.0f * kGap; }
	[[nodiscard]] visage::Point naturalSize() const;

private:
	[[maybe_unused]] RackViewModel& model_; // held for the view's lifetime; this class reads it through its children
	RackFlow flow_;
	RackPadGrid grid_;
	RackPadPages pages_;
	std::unique_ptr<FaderPill> level_;
};

} // namespace feathers::ui
