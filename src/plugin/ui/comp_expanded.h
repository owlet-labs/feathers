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
#include "fx/comp/comp_types.h"
#include "plugin/ui/comp_plot.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// COMP'S WINDOW (backlog QG) — every control the card has, the Compander's Link and second half, the transfer plot
// large, and the Compander's LANE on the right: the Delay's FEEDBACK FX rail, the same component (`FxLaneRail` over a
// `rack::LazyLane`), shown only in Compander mode.
//
//   ┌──────────────────────────────────────────────────────────────┬────────────┐
//   │ [Compress | Limit | Compander]                       [L/R]   │ LANE       │
//   │ [Compress | Expand | Compander | Excomper]           [Link]  │ (a rail;   │
//   │ ┌ TRANSFER ───────────┐  (◯) (◯) (◯) (◯)                     │  a phantom │
//   │ │ first, second, net  │  (◯) (◯) (◯) (◯)                     │  until its │
//   │ └─────────────────────┘  (◯) (◯) (◯) (◯)                     │  (+) makes │
//   │ [Peak|RMS] [SC HPF ▾] [Self|Side]                            │  the lane) │
//   └──────────────────────────────────────────────────────────────┴────────────┘
//
// **STRICTLY A SUPERSET OF THE CARD** (CLAUDE.md): every control the card shows in a state is shown here in that state,
// each by `fx::comp::liveIn` — the card's own rule, the nested gates included.
//
// **LINK, UNLINKED, STARTS WHERE IT WAS**: the press that turns Link off first writes the first half's Ratio,
// Attack and Release into the second half's, so the sound does not move when the switch does (`unlink`).

namespace feathers::ui {

/// THE TRANSFER PLOT AS A FRAME — `comp_plot::paint`, the card's picture at the window's size.
class CompTransferView final : public visage::Frame, public TooltipSource {
public:
	/// A PICTURE SAYS NOTHING AT A POINT (`DelayPlane`'s rule), and offers no control: it has no handles.
	const char* tooltipAt(visage::Point) const override { return nullptr; }
	void offeredControls(std::vector<Offered>&) const override {}
	CompTransferView(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), id_(id) {}
	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] comp_plot::Curves curves() const;

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
};

/// **THE METER, AS IN THE CARD**: IN, OUT and GR from the same published readings through the same
/// road (`comp_plot::meterOf` / `paintMeter`). A reading, not a control: it offers nothing and says nothing at a point.
class CompMeterView final : public visage::Frame, public TooltipSource {
public:
	CompMeterView(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), id_(id) {}
	const char* tooltipAt(visage::Point) const override { return nullptr; }
	void offeredControls(std::vector<Offered>&) const override {}
	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] comp_plot::Meter meter() const;

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
};

class CompExpanded final : public ExpandedView {
public:
	CompExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	CompExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;
	~CompExpanded() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override { return fx::comp::kTraits.name; }
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;
	void setPresentedOn(SurfaceLayers* host) override;

	static constexpr float kPad = 14.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kCellWidth = 72.0f;
	static constexpr size_t kKnobsPerRow = 4;
	static constexpr size_t kKnobRows = 3;
	static constexpr float kPlotWidth = 240.0f;
	static constexpr float kChipWidth = 44.0f;
	/// THE METER COLUMN — the card's width, to the right of the knob grid.
	static constexpr float kMeterWidth = 54.0f;

	/// THE RAIL'S WORDS.
	static constexpr const char* kLaneLabel = "LANE";
	static constexpr const char* kLaneLine = "between the halves";
	static constexpr const char* kLaneEmpty = "the halves meet directly";

	//-- test seams ---------------------------------------------------------------------------------//
	[[nodiscard]] FxLaneRail& laneRailForTest() const { return *lane_; }
	[[nodiscard]] CompTransferView& plotForTest() const { return *plot_; }
	[[nodiscard]] CompMeterView& meterForTest() const { return *meter_; }
	[[nodiscard]] SegmentedControl& modeForTest() const { return *mode_; }
	[[nodiscard]] SegmentedControl& topologyForTest() const { return *topology_; }
	[[nodiscard]] SwitchChip& linkForTest() const { return *link_; }
	[[nodiscard]] Knob* knobForTest(fx::comp::Param p) const;
	/// A PRESS ON LINK, as the mouse makes one: the unlink copy (when it turns Off), then the toggle.
	void pressLinkForTest();
	/// **READ OFF THE WIDGET TREE** (`ControlFace`), hidden ones included — the controls this window offers.
	[[nodiscard]] std::vector<fx::comp::Param> addressedParamsForTest() const;

protected:
	void onInnerNestChanged() override;

private:
	[[nodiscard]] DeviceControl& control(fx::comp::Param p) const;
	[[nodiscard]] bool live(fx::comp::Param p) const;
	[[nodiscard]] bool compander() const;
	/// THE SECOND HALF STARTS AT THE FIRST HALF'S VALUES — one gesture per control, so one undo step each.
	void unlink();
	void layOut();
	bool applyGates();
	/// THE RAIL OVER THE LANE, OR A PHANTOM WHILE THERE IS NONE — re-asked every tick (trap T9).
	bool followLane(void* held);
	[[nodiscard]] float railWidth() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	Notifier* notifier_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	/// HELD BEFORE THE RAIL IS BUILT OVER IT, AND DECLARED BEFORE IT so it outlives its teardown (backlog JX).
	backplane::DeviceHost::SurfacePin chainPin_;

	std::unique_ptr<CompTransferView> plot_;
	std::unique_ptr<CompMeterView> meter_;
	comp_plot::Meter drawnMeter_ {};
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<SegmentedControl> topology_;
	std::unique_ptr<SwitchChip> link_;
	std::unique_ptr<SwitchChip> stereo_;
	std::unique_ptr<SegmentedControl> detect_;
	std::unique_ptr<SegmentedControl> detector_;
	std::unique_ptr<ValueBox> scHpf_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<FxLaneRail> lane_;
	bool phantomShown_ = false;
	bool pendingAddMenu_ = false;
	int laidOutMode_ = -1;
	std::array<float, 16> drawnCurve_ {};
};

} // namespace feathers::ui
