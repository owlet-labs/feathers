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

#include "backplane/device_host.h"
#include "fx/eroder/eroder_types.h"

#include <visage/ui.h>

#include <array>
#include <string>
#include <vector>

// THE ERODER'S BLOCK DIAGRAM — the signal path as it is now set, drawn, with the four switches that change it seated in
// the drawing next to what they change.
//
//        ┌──────────── sub ─ [Sub Thru] ────────────────────┐        the Sub Thru road, over the line
//        │   [In Loop]                                      ▼
//     ┌──┴─┐ ┌────┐   ┌───┐ ┌─────┐ ┌──────┐ ┌─────┐ ┌─────────┐ ┌────┐ ┌────┐
//     │ in ├▶│emph├▶ (⊕) ├▶│smear├▶│ dust ├▶│drive├▶│ restore ├▶│ hp ├▶│mix │   the main line
//     └────┘ └────┘   ▲▲  └─────┘ └──┬───┘ └──┬──┘ └────▲────┘ └────┘ └────┘
//                     │└── reso [Clean] ◀─────┘        │           the two returns, under it
//                     └──── loop ◀─────────────────────┘      [Restore]
//
// **WHAT IT SHOWS.** In order: the input, the emphasis (before the sum, or after it with In Loop), the sum, Smear, the
// engine (Etch: slot A and slot B, in line for Series, split and joined for Parallel), the Drive, the DC stage (an indicator: lit while it runs), the emphasis undone, HP
// Wet and Mix. Two returns arch back under the line to the sum: the inner (Reso) from after the engine (Tap Clean) or
// after the Drive (Tap Driven), the outer (Loop) from after the Drive. The Sub Thru road arches over the line from the
// input to after HP Wet, which is where the engine gives the lows back. **A STAGE THAT IS OFF IS DIM.**
//
// **IT IS NOT A CONTROL.** The drawing is read from the device's row; the chips in it are the window's own, parented
// here (`seat`). The frame passes presses through to them.
//
// **IT REPAINTS ONLY WHEN WHAT IT DRAWS CHANGES**: `update()` compares the path with the last drawn.

namespace feathers::ui {

class EroderFlow final : public visage::Frame {
public:
	/// One part of the path.
	struct Stage {
		std::string label;
		bool lit = true;
		/// Drawn under the stage before it, in the same column (Etch's slot B in Parallel).
		bool beneath = false;
		bool operator==(const Stage&) const = default;
	};
	/// A return to the sum, by the index of the stage it leaves and the stage it joins.
	struct Return {
		size_t from = 0;
		size_t to = 0;
		bool lit = false;
		bool outer = false;
		bool operator==(const Return&) const = default;
	};
	/// THE SUB THRU ROAD: the input's lows, taken by HP Wet and given back after it, from the input stage to HP Wet's.
	struct Bypass {
		size_t from = 0;
		size_t to = 0;
		bool lit = false;
		/// False in the Comb while Blend is off: the dry is whole there, the switch is not shown and the road is not drawn.
		bool shown = true;
		bool operator==(const Bypass&) const = default;
	};
	/// The path for one row: the one function the drawing and its test seam both read.
	struct Path {
		std::vector<Stage> stages;
		std::vector<Return> returns;
		Bypass bypass;
		bool operator==(const Path&) const = default;
	};
	[[nodiscard]] static Path pathFor(const fx::eroder::Row& row);

	struct Rect {
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
	};
	/// THE FIVE SWITCHES, each seated next to what it changes (Dc: the DC block, above the `dc` stage).
	enum class Chip { InLoop, Tap, Restore, SubThru, Dc };
	/// What a chip sits beside: the stage it is about, and the point of the drawing it is placed on.
	struct Anchor {
		size_t stage = 0;
		float x = 0.0f;
		float y = 0.0f;
	};

	static constexpr float kHeight = 124.0f;
	static constexpr float kChipWidth = 72.0f;
	static constexpr float kChipHeight = 16.0f;
	/// How far a chip's centre may be from its anchor point.
	static constexpr float kChipReach = 24.0f;
	/// The main line's y, the two returns', and the Sub Thru road's.
	static constexpr float kMainY = 52.0f;
	static constexpr float kInnerY = 84.0f;
	static constexpr float kOuterY = 98.0f;
	static constexpr float kBypassY = 16.0f;

	EroderFlow(backplane::DeviceHost& host, DeviceInstanceId id);

	/// THE FIVE SWITCHES THAT CHANGE OR ARE DRAWN IN THE PICTURE, seated in the frame (parented here, not owned).
	void seat(visage::Frame& inLoop, visage::Frame& tap, visage::Frame& restore, visage::Frame& subThru, visage::Frame& dc);

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did — the window's `updateContent` contract.
	bool update();

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                     //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] const Path& pathForTest() const noexcept { return path_; }
	[[nodiscard]] std::vector<std::string> labelsForTest() const;
	/// A stage's box, in the frame; and its centre x.
	[[nodiscard]] Rect boxForTest(size_t i) const { return i < boxes_.size() ? boxes_[i] : Rect {}; }
	[[nodiscard]] float stageXForTest(size_t i) const { return i < boxes_.size() ? boxes_[i].x + boxes_[i].w * 0.5f : 0.0f; }
	[[nodiscard]] Anchor anchorForTest(Chip chip) const;
	[[nodiscard]] Rect chipRectForTest(Chip chip) const;
	[[nodiscard]] int recomputesForTest() const noexcept { return recomputes_; }
	/// **WHAT THE LAST DRAW PUT DOWN, IN ORDER** (recorded only while `setRecordingForTest(true)`): each line segment, and each
	/// chip's backing plate (`backing`, its rectangle in x0..y1). A test reads draw order from the sequence.
	struct DrawEvent {
		bool backing = false;
		float x0 = 0.0f;
		float y0 = 0.0f;
		float x1 = 0.0f;
		float y1 = 0.0f;
	};
	void setRecordingForTest(bool on) noexcept { recording_ = on; }
	[[nodiscard]] const std::vector<DrawEvent>& drawnForTest() const noexcept { return events_; }

private:
	void measure();
	[[nodiscard]] float labelWidth(const std::string& text) const;
	[[nodiscard]] size_t indexOf(const char* label) const;
	[[nodiscard]] float returnFromX(const Return& r) const;
	[[nodiscard]] float returnToX(const Return& r) const;
	[[nodiscard]] float bypassToX() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Path path_;
	bool computed_ = false;
	int recomputes_ = 0;
	/// Each stage's box, and the columns they stand in (a column is one stage, or two for Parallel).
	std::vector<Rect> boxes_;
	std::vector<std::vector<size_t>> columns_;
	bool recording_ = false;
	std::vector<DrawEvent> events_;
	std::array<Rect, 5> chipRects_ {};
	std::array<Anchor, 5> anchors_ {};
	visage::Frame* inLoop_ = nullptr;
	visage::Frame* tap_ = nullptr;
	visage::Frame* restore_ = nullptr;
	visage::Frame* subThru_ = nullptr;
	visage::Frame* dc_ = nullptr;
};

} // namespace feathers::ui
