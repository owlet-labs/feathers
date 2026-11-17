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

// THE ERODER'S TOPOLOGY PICTURE — the signal path as it is now set, drawn, with the three switches that change it
// seated inside its frame.
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │      ┌──────────────────────────────────────────┐            │
//   │      │      ┌──────────────┐                    │            │   the two returns, over the line
//   │      ▼      │              │                    │            │
//   │  in  ⊕  emph  smear  dust  drive  restore  hp  mix          │
//   │  [ In Loop ]          [ Driven ]   [ Restore ]               │   the chips that move parts of it
//   └──────────────────────────────────────────────────────────────┘
//
// **WHAT IT SHOWS.** In order: the input, the emphasis (before the sum, or after it with In Loop), the sum, Smear, the
// engine (Etch: slot A and slot B, in series or one over the other when parallel), the Drive, the emphasis undone, HP
// Wet and Mix. Two returns arch back to the sum: the inner one from after the engine (Tap Clean) or after the Drive
// (Tap Driven), the outer from after the Drive. **A STAGE THAT IS OFF IS DIM**, as is a return at zero.
//
// **IT IS NOT A CONTROL.** The picture is read from the device's row and is drawn from it; the chips on it are the
// window's own, parented here (`seat`) so it is plain that they change the drawing. The frame passes presses through
// to them.
//
// **IT REPAINTS ONLY WHEN WHAT IT DRAWS CHANGES**: `update()` compares the row's inputs with the last drawn.

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
	/// The path for one row: the one function the picture and its test seam both read.
	/// THE SUB THRU ROAD: the input's lows, taken by HP Wet and given back after it, from the input stage to HP Wet's.
	struct Bypass {
		size_t from = 0;
		size_t to = 0;
		bool lit = false;
		bool operator==(const Bypass&) const = default;
	};
	struct Path {
		std::vector<Stage> stages;
		std::vector<Return> returns;
		Bypass bypass;
		bool operator==(const Path&) const = default;
	};
	[[nodiscard]] static Path pathFor(const fx::eroder::Row& row);

	static constexpr float kHeight = 76.0f;
	static constexpr float kPad = 6.0f;
	/// Where the row of switches starts, from the frame's top.
	static constexpr float kChipTop = 56.0f;
	/// The bypass runs along this y, between the labels and the switches.
	static constexpr float kBypassY = 50.0f;

	EroderFlow(backplane::DeviceHost& host, DeviceInstanceId id);

	/// THE THREE SWITCHES THAT CHANGE THE DRAWING (and Sub Thru, which is a road of it), seated in the frame (parented here, not owned).
	void seat(visage::Frame& inLoop, visage::Frame& tap, visage::Frame& restore, visage::Frame& subThru);

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did — the window's `updateContent` contract.
	bool update();

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                     //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] const Path& pathForTest() const noexcept { return path_; }
	[[nodiscard]] std::vector<std::string> labelsForTest() const;
	/// The column's centre, in the frame's x, of stage `i`.
	[[nodiscard]] float stageXForTest(size_t i) const { return i < columns_.size() ? columns_[i] : 0.0f; }
	[[nodiscard]] int recomputesForTest() const noexcept { return recomputes_; }

private:
	void measure();
	[[nodiscard]] float labelWidth(const std::string& text) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Path path_;
	bool computed_ = false;
	int recomputes_ = 0;
	/// Each stage's centre x and half-width, measured for the labels now drawn.
	std::vector<float> columns_;
	std::vector<float> halves_;
	visage::Frame* inLoop_ = nullptr;
	visage::Frame* tap_ = nullptr;
	visage::Frame* restore_ = nullptr;
	visage::Frame* subThru_ = nullptr;
};

} // namespace feathers::ui
