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
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/device_preset_provider.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/resonate_face.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <string>
#include <vector>

// RESONATE'S PANEL — every control, by signal flow left to right: EXCITE, the string large, TONE and the
// DRONE (the sympathetic string), then FILTER and OUT. The card shows only a few of them (by design,
// 2026-09-26); the widgets are `ResonateFace`'s, built for the panel surface.
//
// NO OUTPUT SCOPE: no reusable scope widget exists in the tree (HARM's is a response curve, not a trace),
// and a second one written here for one device is the thing the layout rule says to reuse rather than make.
//
// **AND TWO FX LANES ON THE RIGHT**: DRONE FX then WET FX, SKLITTER's
// `FxLaneRail`s, each a column of devices over one of the device's own chains (resonate.cpp has where they
// sit). Both start shut. `Drone to Wet FX` heads the DRONE FX rail, shown while WET FX holds something — with
// WET FX empty the two routings are the same sound, so the switch would change nothing (the honesty rule).

namespace feathers::ui {

class ResonateExpanded final : public ExpandedView {
public:
	ResonateExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                 Notifier* notifier = nullptr);
	ResonateExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override { return "Resonate"; }
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;

	static constexpr float kPad = 14.0f;

	/// **THE PRESETS: FEATHERS' OWN BROWSER, THIS DEVICE'S BANK** — the one SKLITTER's and Featherverb's panels hold, in the title row, over `Devices/RESONATE`. A load
	/// keeps only the bypass. "Init" is the declared defaults; the factory set ships under
	/// `presets/factory/Devices/RESONATE/`.
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.get(); }
	[[nodiscard]] PresetBrowser& presetBrowserForTest() const { return *presets_; }

	[[nodiscard]] const ResonateFace& face() const { return *face_; }
	/// The FACE's controls. The routing switch is not among them: the DRONE FX rail seats it (`routeChipForTest`).
	[[nodiscard]] std::vector<fx::resonate::Param> addressedParamsForTest() const { return face_->addressedParams(); }

	void setPresentedOn(SurfaceLayers* host) override;

	/// THE TWO RAILS' WORDS — upright labels and captions (`FxLaneRail::Spec`).
	static constexpr const char* kDroneLabel = "DRONE FX";
	static constexpr const char* kWetLabel = "WET FX";
	static constexpr const char* kDroneLine = "the drone, before the Mix";
	static constexpr const char* kWetLine = "tone and filter, before the Mix";
	static constexpr const char* kRouteLabel = "Drone to Wet FX";

	[[nodiscard]] FxLaneRail& droneRailForTest() const { return *droneFx_; }
	[[nodiscard]] FxLaneRail& wetRailForTest() const { return *wetFx_; }
	[[nodiscard]] SwitchChip& routeChipForTest() const { return *routeChip_; }

protected:
	void onInnerNestChanged() override;

private:
	void layOut();
	[[nodiscard]] float railsWidth() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	std::unique_ptr<ResonateFace> face_;
	std::unique_ptr<DevicePresetProvider> presetProvider_;
	std::unique_ptr<PresetBrowser> presets_; // after its provider, so destroyed before it
	/// HELD BEFORE THE RAILS ARE BUILT OVER IT, AND DECLARED BEFORE THEM so it outlives their teardown (backlog JX).
	backplane::DeviceHost::SurfacePin chainPin_;
	/// The routing switch: its control, then the chip over it — both outlive the rail that seats the chip.
	std::unique_ptr<DeviceControl> routeControl_;
	std::unique_ptr<SwitchChip> routeChip_;
	std::unique_ptr<FxLaneRail> droneFx_;
	std::unique_ptr<FxLaneRail> wetFx_;
};

} // namespace feathers::ui
