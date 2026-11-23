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
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// STEREO BALANCE'S CARD — the picture is the card: the comb's band curves over one imbalance meter per band, and the controls round it.
//
//   [ 2 bands | 3 bands ]            [Key Track]    how many bands, and whether the combs follow the played note
//   ┌──────────────────────────────────────┐
//   │ the comb, Spacing handle under it    │   `BalanceView`: the comb; a goniometer per band (colour) over a grey ghost of the input,
//   │ goniometer │ a meter and correlation │   with each band's meter and correlation beside it; and STRENGTH, WIDEN and PAN as handles on its foot
//   │ STRENGTH  WIDEN  PAN  (handles)      │
//   └──────────────────────────────────────┘
//   (◯) SPLIT 2   (◯) FOCUS   (◯) ATTACK   (◯) RELEASE      the second split, how much of the correction is per band, and how the measurement follows
//
// Spacing 2 is drawn only in three bands (it cuts the on-teeth band; two bands have no second split): hidden, not greyed. Mix is the slot's,
// and Comb Sharp, Tolerance, HP Wet, Sub Thru and the four handled controls as knobs are the window's (`BalanceExpanded`).
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Bands — a segmented control. Key Track — a chip. Spacing 2, Focus, Attack, Release — a knob each.
//   - Spacing, Strength, Widen, Pan — a handle on the picture (`BalanceView`: a gesture road); their knobs are the window's, where a modulation ring sits on each.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.
// The picture reaches nothing: it reads.

namespace feathers::ui {

class BalanceCard final : public FxCard {
public:
	BalanceCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — see `GenericCard`.
	BalanceCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;

	const device::Descriptor* descriptor() const override { return &fx::balance::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kSwitchHeight = 18.0f;
	static constexpr float kPictureHeight = 222.0f;
	static constexpr float kChipWidth = 58.0f;
	static constexpr size_t kKnobsPerRow = 4;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	//-- test seams ---------------------------------------------------------------------------------//
	[[nodiscard]] std::vector<fx::balance::Param> addressedParamsForTest() const;
	[[nodiscard]] visage::Frame* widgetForTest(fx::balance::Param p) const;
	[[nodiscard]] BalanceView* viewForTest() const { return view_.get(); }

private:
	device::Modulatable* enableModulatable() override;
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
	std::unique_ptr<BalanceView> view_;
	/// THE KNOBS, in flow order (`kKnobs` in the .cpp).
	std::vector<std::unique_ptr<Knob>> knobs_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
