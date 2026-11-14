/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/control_face.h"

#include "backplane/device_host.h"
#include "fx/owltt/owltt_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/owltt_graph.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/vertical_fader.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

// OWLTT'S WINDOW — the analyzer, composed.
//
// THE PICTURE IS THE INSTRUMENT. Everything that describes a band is on the plot or in the table under its bars: the
// threshold and bandwidth rings, the downward and upward ratio thumbs, the base ratio and level strips, and a column of
// Input, Width, Attack and Release for each band (`OwlttGraph::setEditor`). The two crossovers are handles in the plot's
// lane. What is left is what is not one band's, in a few captioned rows in the house form (the Delay window's):
//
//   top row     Key Track, Lookahead, L/R, the crossover type, the Clip choice
//   plot row    Drive, left of the plot; the plot; Output, right of it, its mirror
//   GLOBAL      Threshold, Bandwidth, Ratio, Up/Down, Attack, Release (each a shift or an offset over the bands), Link,
//               Width, Blend
//   TONE        Character and Vibe, one group
//
// THE WINDOW IS A SUPERSET OF THE CARD: every card control is here (Threshold, Ratio, Up/Down, the L/R chip, Link, Width,
// Drive, and the plot's own handles), and each one a frame here drives is declared through `ControlFace`.

namespace feathers::ui {

class OwlttExpanded final : public ExpandedView, public ControlFace {
public:
	void offeredControls(std::vector<Offered>& out) const override;
	OwlttExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	             Notifier* notifier = nullptr);
	OwlttExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	visage::Point minimumSize() const override;
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 8.0f;
	/// The gap between the top row and the plot.
	static constexpr float kGroupGap = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kSwitchHeight = 20.0f;
	/// THE BODY'S WIDTH, between the pads. The plot takes what the two faders leave.
	static constexpr float kBodyWidth = 672.0f;
	/// The plot's height: the crossover lane, the bars and strips, and the table of per-band boxes.
	static constexpr float kGraphHeight = 322.0f;
	/// The row captions' margin, as the Delay window's.
	static constexpr float kBandLabelWidth = 52.0f;
	static constexpr float kBoxGap = 4.0f;
	/// The face diameter the tone cells size their rows by: small, so the group is a row and not a column.
	static constexpr float kToneFace = 24.0f;

	/// THE TOP ROW, left to right.
	static constexpr float kKeyTrackWidth = 64.0f;
	static constexpr float kLookaheadWidth = 112.0f;
	static constexpr float kChipWidth = 36.0f;
	static constexpr float kModeWidth = 108.0f;
	static constexpr float kClipWidth = 104.0f;
	visage::Bounds keyTrackBounds() const;
	visage::Bounds lookaheadBounds() const;
	visage::Bounds stereoBounds() const;
	visage::Bounds modeBounds() const;
	visage::Bounds clipBounds() const;

	std::string modeText() const;
	void showModeMenu();
	void chooseMode(size_t index);

	/// THE PLOT, AND THE TWO FADERS BESIDE IT, mirrored about it: the Drive's rectangle and the Output's are the same size and
	/// sit the same distance from the plot and from the window's edge.
	visage::Bounds graphBounds() const;
	visage::Bounds driveBounds() const;
	visage::Bounds outputBounds() const;
	static constexpr float kDriveWidth = 28.0f;
	static constexpr float kDriveGap = 8.0f;
	float modalBottom() const;

	/// THE TWO CAPTIONED ROWS UNDER THE PLOT.
	struct RowSpec {
		const char* caption;
		visage::Bounds bounds;
	};
	visage::Bounds globalRow() const;
	visage::Bounds toneRow() const;
	static constexpr float kToneHeight = 56.0f;
	/// The controls of the GLOBAL row, in order.
	static std::span<const fx::owltt::Param> globalParams();

	/// BODY WIDTH, for the overlay's sizing.
	float bodyWidth() const { return kBodyWidth; }

	//-----------//
	// For tests //
	//-----------//

	std::vector<fx::owltt::Param> addressedParamsForTest() const;
	[[nodiscard]] fx::owltt::Param paramOfGlobalForTest(const ValueBox* box) const;
	[[nodiscard]] SwitchChip* stereoForTest() const { return stereo_.get(); }
	[[nodiscard]] VerticalFader* driveForTest() const { return drive_.get(); }
	[[nodiscard]] VerticalFader* outputForTest() const { return output_.get(); }
	[[nodiscard]] ValueBox* lookaheadForTest() const { return lookahead_.get(); }
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	[[nodiscard]] ValueBox* clipForTest() const { return clip_.get(); }
	[[nodiscard]] const std::vector<std::unique_ptr<ValueBox>>& globalBoxesForTest() const { return globalBoxes_; }
	[[nodiscard]] const std::vector<std::unique_ptr<ModeValueCell>>& modeCellsForTest() const { return modeCells_; }
	/// Every cell the two rows hold, in draw order.
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }
	OwlttGraph* graph() const noexcept { return graph_.get(); }

	bool updateContent() override;

private:
	DeviceControl& control(fx::owltt::Param param) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::owltt::Param::kCount)>
	    controls_ {};
	std::unique_ptr<OwlttGraph> graph_;
	std::unique_ptr<SwitchChip> stereo_;
	/// DRIVE LEFT OF THE PLOT AND OUTPUT RIGHT OF IT, the same widget at the same size.
	std::unique_ptr<VerticalFader> drive_;
	std::unique_ptr<VerticalFader> output_;
	std::unique_ptr<ValueBox> lookahead_;
	std::unique_ptr<SwitchChip> keyTrack_;
	/// THE CLIP CHOICE, three of them, so a box that opens its menu (every choice seen before it applies).
	std::unique_ptr<ValueBox> clip_;
	std::vector<std::unique_ptr<ValueBox>> globalBoxes_;
	std::vector<std::unique_ptr<ModeValueCell>> modeCells_;
	std::vector<fx::owltt::Param> coordParams_;
	std::vector<visage::Frame*> cells_;
};

} // namespace feathers::ui
