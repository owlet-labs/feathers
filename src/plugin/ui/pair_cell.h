/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#pragma once

#include <visage/ui.h>

#include <string>
#include <vector>

// A PAIR CELL — two cells that are one thing, named once and never parted.
//
//        DRIVE
//   ┌───────────────┐
//   │  (knob)  Soft │     any two cells: a knob and a mode cell, two knobs, a knob and a chip
//   │   40%    35%  │
//   └───────────────┘
//
// **WHY IT IS A WIDGET.** Two controls that only mean something together (how hard, and into what) were two
// neighbouring cells with two captions, and a grid that flows its cells could put one at the end of a row and the
// other at the start of the next. The pair is ONE item to whatever lays it out — it spans two columns and wraps
// whole (`flowCells`) — and its caption names the group, so the two inside need only say which half they are.
//
// **IT OWNS NEITHER CELL AND KNOWS NOTHING ABOUT THEM.** It parents the two frames it is given, places them side by
// side under the caption, and draws the outline. What they are bound to, how they draw, their tooltips and their
// part in the card/window superset guard are their own: the guard walks the frame tree and finds them here.

namespace feathers::ui {

class PairCell final : public visage::Frame {
public:
	/// The caption's band above the two cells.
	static constexpr float kCaptionHeight = 12.0f;
	static constexpr float kRadius = 5.0f;
	/// How tall the pair is round cells of this height.
	static constexpr float heightFor(float cellHeight) { return kCaptionHeight + cellHeight; }

	PairCell(std::string caption, visage::Frame& first, visage::Frame& second);

	/// THE FIRST CELL'S SHARE OF THE WIDTH, 0..1; half by default.
	void setSplit(float firstShare);
	[[nodiscard]] float split() const noexcept { return split_; }
	[[nodiscard]] const std::string& caption() const noexcept { return caption_; }
	[[nodiscard]] visage::Frame& first() const noexcept { return first_; }
	[[nodiscard]] visage::Frame& second() const noexcept { return second_; }

	void draw(visage::Canvas& canvas) override;
	void resized() override;

private:
	std::string caption_;
	visage::Frame& first_;
	visage::Frame& second_;
	float split_ = 0.5f;
};

/// ONE ITEM OF A FLOWED GRID: a frame and how many columns it takes. Null frames and hidden items take no room.
struct FlowItem {
	visage::Frame* frame = nullptr;
	int span = 1;
	bool shown = true;
	float height = 0.0f; ///< this item's own height; a row is as tall as its tallest
};

/// **LAYS ITEMS OUT LEFT TO RIGHT IN `columns` COLUMNS, AND NEVER SPLITS ONE**: an item that does not fit what is left
/// of a row starts the next. Shown items are bounded and made visible; the rest are hidden. Returns the y under the
/// last row.
float flowCells(const std::vector<FlowItem>& items, int columns, float x, float y, float columnWidth, float rowGap);

} // namespace feathers::ui
