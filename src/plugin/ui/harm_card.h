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

#include "plugin/ui/widget_registry.h"
#include "fx/harm/harm_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fx_card.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <vector>


// HARM'S CARD — the fx lane's second tenant on the glass.
//
// THE CARD IS A TABBED PANEL OVER A ROW OF MASTERS. The four partial tabs sit on top; the FRAME directly under them belongs to
// the selected tab and is drawn as its continuation (the tab's fill runs down through the frame's top edge). In the frame are the
// selected partial's own three controls: its Active chip, its harmonic box and its Level. Each partial has its own set of three,
// bound to that partial's parameters, and a tab press shows that partial's set, so no control is ever re-pointed. Below the frame,
// outside it, under a GLOBAL caption, are the master controls: Master, Depth, Attack, Release. The rest of the envelope (Delay,
// Hold, Decay, Sustain), Poly, Env Depth, Porta and the partial's Mode, Fine, Width, Depth, Phase, Spread live in the window.
//
// THREE THINGS THIS CARD MUST NOT DO:
//
//   - IT MUST NOT SAY "HPF". The firmware calls the notch's width knob `hpf`; it is WIDTH, and it is not on this card.
//   - IT MUST NOT DRAW A CONTROL IT CANNOT ACT THROUGH. Each partial's Level and harmonic grey with the partial's own Active
//     switch (a partial that is off sounds nothing), and everything greys with the device.
//   - IT MUST NOT CUT A CAPTION. Every label is whole at the card's cell width; the tooltips suite measures it.

namespace feathers::ui {

/// THE PARTIAL THE HARM CARD AND ITS WINDOW HAVE OPEN. It is view state, not a parameter: it is never stored in a patch and never
/// automated. It is shared so the card's tabs and the window's strip are one selection, whichever one a hand presses.
struct HarmView {
	int partial = 0;
};

/// THE VIEW STATE FOR ONE DEVICE INSTANCE. Keyed by its serial (`DeviceInstanceId` is monotonic, so an id is never reused and
/// nothing can be aliased into a stale selection), and held weakly: the card and any window for the device share one object
/// while either is alive, and a removed device's entry expires with its last surface.
std::shared_ptr<HarmView> harmViewFor(DeviceInstanceId id);

class HarmCard final : public FxCard {
public:
	HarmCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	HarmCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	const device::Descriptor* descriptor() const override { return &fx::harm::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool updateContent() override;
	void resized() override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kPad = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// THE PARTIAL TABS: one row of four, each two lines tall (its number and ratio, then On/Off).
	static constexpr float kTabRowHeight = 36.0f;
	static constexpr float kTabGap = 4.0f;
	/// THE GLOBAL CAPTION over the masters.
	static constexpr float kCaptionBand = 12.0f;
	/// THE FRAME under the tabs: its padding, and the two rows inside it (Active chip + harmonic box, then Level).
	static constexpr float kFramePad = 14.0f;
	static constexpr float kFrameRowGap = 12.0f;
	static constexpr float kFrameLevelHeight = 28.0f;
	static constexpr float kChipWidth = 52.0f;
	static constexpr float kFrameHeight =
	    kFramePad + HarmonicRow::kHeight + kFrameRowGap + kFrameLevelHeight + kFramePad;
	/// The tab a point is over, or -1. The partial it selects is `HarmView::partial`.
	int tabAt(visage::Point position) const;
	visage::Bounds tabBounds(int partial) const;
	/// THE SELECTED TAB'S FRAME, in the card's coordinates: its top edge is the tab row's bottom edge.
	visage::Bounds frameBounds() const;
	int selectedPartial() const { return view_->partial; }
	void selectPartial(int partial);
	std::string tabTextForTest(int partial) const;

	struct CardKnob {
		fx::harm::Param param;
		/// Spelled out, not derived from the descriptor's qualified name. Short enough to fit a cell whole.
		const char* label;
	};

	/// THE MASTER KNOBS, in reading order, one row under the frame: the two masters, then the envelope's ends.
	static constexpr CardKnob kCardKnobs[] = {
	    { fx::harm::Param::MasterLevel, "Master" },
	    { fx::harm::Param::MasterDepth, "Depth" },
	    { fx::harm::Param::Attack, "Attack" },
	    { fx::harm::Param::Release, "Release" },
	};
	static constexpr size_t kCardKnobCount = std::size(kCardKnobs);

	/// FOUR CELLS A ROW. Five cells cut "Attack" and "Release" at this card's width (tests/gui_widgets/test_gui_widgets_tooltips).
	static constexpr int kRowCells = 4;

	static float openHeight();
	static constexpr float foldedHeight() { return kHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;
	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }
	void setModulation(ModState* state, EditHistory* history) override;

	/// Geometry asked rather than photographed.
	visage::Bounds headBounds() const;

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	/// THE FRAME'S THREE CONTROLS FOR A PARTIAL, whether or not it is the one shown.
	SwitchChip* activeChipForTest(int partial) const { return sets_[static_cast<size_t>(partial)].active.get(); }
	HarmonicRow* harmonicRowForTest(int partial) const { return sets_[static_cast<size_t>(partial)].row.get(); }
	FaderPill* levelPillForTest(int partial) const { return sets_[static_cast<size_t>(partial)].level.get(); }
	/// Where the masters start, in the card's coordinates.
	float knobsTopForTest() const { return knobsTop(); }

	/// EVERY CONTROL THIS CARD PUTS A HAND ON, the card's own answer. The window must offer each of these too.
	std::vector<fx::harm::Param> addressedParamsForTest() const;

	enum class Part { None, Fold, Enable, Expand, SlotMenu };
	Part partAt(visage::Point position) const;

private:
	void layOut();
	/// Where the master row starts: under the frame and the GLOBAL caption.
	float knobsTop() const;
	void drawPartialTabs(visage::Canvas& canvas, bool deviceOn);
	/// Show the selected partial's set and hide the other three (and all of them while folded).
	void applySelection();
	bool partialActive(int partial) const;
	device::Modulatable* enableModulatable() override;
	void toggleEnabled();
	DeviceControl& control(fx::harm::Param param) const;

	/// ONE PARTIAL'S FRAME CONTROLS, bound to that partial's own parameters.
	struct PartialSet {
		std::unique_ptr<SwitchChip> active;
		std::unique_ptr<HarmonicRow> row;
		std::unique_ptr<FaderPill> level;
	};

	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;
	bool drawnEnabled_ = false;
	bool haveDrawnEnabled_ = false;
	std::shared_ptr<HarmView> view_;
	int drawnPartial_ = -1;
	std::array<int, fx::harm::kPartialCount> drawnActive_ { -1, -1, -1, -1 };

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::harm::Param> knobParams_;
	std::array<PartialSet, fx::harm::kPartialCount> sets_;
};

} // namespace feathers::ui
