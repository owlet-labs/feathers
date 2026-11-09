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
#include "fx/spectral/spectral_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/spectral_source.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/spectrum_plot.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <vector>

// THE SPECTRAL DEVICE'S CARD: THE ADDITIVE DRAWER'S PLOT (`SpectrumPlot` over the device's source, spectral_source.h — the same
// partial bars, the same curve and the same handles as the drawer's), and little else (C1: a simplified card,
// without the toggles and the cell above them):
//
//   - THE BADGE'S LINE over the plot, while Vocode is on: "sidechain silent" while nothing sounds there.
//   - THE PLOT, its handles carrying what they carry (the filter's corner, the comb's spacing, the tilt).
//   - UNDER THE PLOT: the knobs the handles do not carry — Mix always; the filter's Slope, the comb's Phase, Odd/Even's Balance
//     and Partials' count and Depth while their operator is on.
//
// The operators' switches, the curve's, the Draw tool, the filter's Mode, the Carrier, and Resolution/Key/Poly/Offset are the
// window's alone (spectral_expanded.h), so the window ⊇ card guard holds trivially. THE CARD HIDES what an operator that is off
// makes inert (the window greys it).

namespace feathers::ui {

/// **"SIDECHAIN SILENT"**: drawn while the vocoder is on and nothing sounds on the plugin's Sidechain input (unrouted
/// or quiet: a host cannot tell us which), so the hand knows why Vocode is silent. Empty otherwise. Its tooltip is Vocode's own.
class SidechainBadge final : public visage::Frame, public TooltipSource {
public:
	SidechainBadge();
	void setSilent(bool silent);
	[[nodiscard]] bool silent() const { return silent_; }
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>&) const override {}

private:
	bool silent_ = false;
};

class SpectralCard final : public FxCard {
public:
	SpectralCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — see `GenericCard`.
	SpectralCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::spectral::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kPlotHeight = 150.0f;
	static constexpr float kMinPlotHeight = 60.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	[[nodiscard]] static float openHeight();
	/// The badge's line ("sidechain silent"), while Vocode shows: the plot gives it up.
	[[nodiscard]] static constexpr float vocodeLineHeight() { return SwitchChip::kHeight + kRowGap; }
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	[[nodiscard]] std::vector<fx::spectral::Param> addressedParamsForTest() const;
	/// THE WIDGET ON `p`, or null for a control only the plot carries.
	[[nodiscard]] visage::Frame* widgetForTest(fx::spectral::Param p) const;
	[[nodiscard]] SpectrumPlot& plotForTest() const { return *plot_; }
	[[nodiscard]] SidechainBadge& badgeForTest() const { return *badge_; }
	/// Every widget the card holds, for the geometry sweep.
	[[nodiscard]] std::vector<visage::Frame*> widgetsForTest() const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] bool on(fx::spectral::Param p) const;
	/// Whether the card shows the widget on `p`: an operator's own controls only while it is on.
	[[nodiscard]] bool shown(fx::spectral::Param p) const;
	[[nodiscard]] uint32_t visibilityKey() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE SOURCE FIRST: every widget below binds its controls, so it is built before them and goes after them.
	fx::spectral::DeviceSpectrumSource source_;
	std::unique_ptr<SidechainBadge> badge_;
	std::unique_ptr<SpectrumPlot> plot_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	uint32_t laidOutKey_ = ~0u;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
