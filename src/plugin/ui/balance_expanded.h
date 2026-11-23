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
#include "fx/balance/balance_types.h"
#include "plugin/ui/balance_view.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/wet_highpass_control.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <string>
#include <vector>

// STEREO BALANCE'S WINDOW — the card's picture large, every control the card has, and the three it leaves out (Comb Sharp, and HP Wet with Sub Thru, which keep the lows out of the
// split and the detectors).
//
//   [ 2 bands | 3 bands ]   [Key Track] [Sharp]                                  [Sub Thru]
//   ┌─────────────────────────────────────────────────────────────────────────┐
//   │  the comb, and one imbalance meter per band, large                      │
//   └─────────────────────────────────────────────────────────────────────────┘
//   (◯) SPACING (◯) SPACING 2 (◯) STRENGTH (◯) ATTACK (◯) RELEASE (◯) PAN (◯) HP WET

namespace feathers::ui {

class BalanceExpanded final : public ExpandedView {
public:
	BalanceExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	BalanceExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	bool updateContent() override;
	visage::Point minimumSize() const override;
	visage::Point maximumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kSwitchHeight = 18.0f;
	static constexpr float kPictureWidth = 560.0f;
	static constexpr float kPictureHeight = 250.0f;
	static constexpr float kChipWidth = 64.0f;
	static constexpr size_t kCells = 7;

	//-- test seams ---------------------------------------------------------------------------------//
	[[nodiscard]] std::vector<fx::balance::Param> addressedParamsForTest() const;
	[[nodiscard]] visage::Frame* widgetForTest(fx::balance::Param p) const;
	[[nodiscard]] BalanceView* viewForTest() const { return view_.get(); }
	[[nodiscard]] WetHighpassControl* hpForTest() const { return hp_.get(); }

private:
	[[nodiscard]] DeviceControl& control(fx::balance::Param p) const;
	[[nodiscard]] bool isLive(fx::balance::Param p) const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	int laidOutBands_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<SegmentedControl> bands_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::unique_ptr<SwitchChip> sharp_;
	std::unique_ptr<BalanceView> view_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<WetHighpassControl> hp_;
};

} // namespace feathers::ui
