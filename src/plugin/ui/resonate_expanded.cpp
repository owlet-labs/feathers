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

#include "plugin/ui/resonate_expanded.h"

#include "fx/resonate/resonate_lanes.h"
#include "rack/inner_chain.h"

#include <algorithm>

namespace feathers::ui {

namespace {

namespace rs = fx::resonate;

rack::InnerChain* droneChainOf(void* surface) {
	return rs::laneChainOf(surface, rs::Lane::Drone);
}
rack::InnerChain* wetChainOf(void* surface) {
	return rs::laneChainOf(surface, rs::Lane::Wet);
}

/// EVERY FX RAIL THE PANEL HAS, so no forwarder below can reach one and miss the other (SKLITTER's `forEachFxRail`).
template <typename Fn> void forEachRail(FxLaneRail* drone, FxLaneRail* wet, Fn&& fn) {
	for (FxLaneRail* rail : { drone, wet }) {
		if (rail != nullptr) {
			fn(*rail);
		}
	}
}

} // namespace

ResonateExpanded::ResonateExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                   Notifier* notifier)
    : host_(host), id_(id), chainPin_(host) {
	face_ = std::make_unique<ResonateFace>(ResonateFace::Surface::Panel, host, id, registry, notifier, *this);
	// THE PRESETS, as SKLITTER's and Featherverb's panels build them. The bank is read now so the first draw lists it;
	// selecting nothing on open means opening the panel changes no sound. A load keeps only the bypass.
	const device::Descriptor& desc = rs::kDescriptor;
	presets_ = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                           std::vector<PresetData> { PresetData { initDevicePreset(desc) } },
	                                           devicePresetBank(desc), PresetBrowser::kPanelStyle);
	presets_->setBounds(0.0f, 0.0f, presets_->barWidth(), PresetBrowser::kPanelStyle.height);
	presets_->loadBankFromDisk();
	presetProvider_ = std::make_unique<DevicePresetProvider>(host, desc, id, std::span<const uint8_t> {}, notifier);
	presets_->setProvider(presetProvider_.get());
	presets_->setNotifier(notifier);
	presets_->onSelected([this](int) { redrawAll(); });
	// THE PANEL OPENS ON THE PRESET THIS DEVICE CAME FROM, and names every later one into the patch (backlog QC).
	bindPresetIdentity(
	    *presets_, [&host, id] { return host.devicePreset(id); },
	    [&host, id](const PresetIdentity& identity) { host.setDevicePreset(id, identity); });

	// THE TWO LANES, SKLITTER's rails over this device's two chains. Bound now rather than on the first tick, so a panel
	// opened over a running device draws its columns at once; one handle, asked once, given to both.
	droneFx_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kDroneLabel, kDroneLine, &droneChainOf }, registry, notifier);
	wetFx_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kWetLabel, kWetLine, &wetChainOf }, registry, notifier);
	routeControl_ = std::make_unique<DeviceControl>(host, id, desc, static_cast<size_t>(rs::Param::DroneToWetFx));
	routeChip_ = std::make_unique<SwitchChip>(*routeControl_, kRouteLabel);
	droneFx_->setAccessory(routeChip_.get(), SwitchChip::kHeight);
	void* const held = host.containerSurfaceOf(id);
	(void)droneFx_->follow(held);
	(void)wetFx_->follow(held);
	chainPin_.hold(held);
	const rack::InnerChain* wet = wetChainOf(held);
	(void)droneFx_->setAccessoryGate(wet != nullptr && wet->bridge() != nullptr && wet->bridge()->chainLength() > 0);
	forEachRail(droneFx_.get(), wetFx_.get(), [this](FxLaneRail& rail) {
		addChild(&rail);
		// THE RAIL ANNOUNCES ITS WIDTH; THIS PANEL PLACES IT, and tells the host its minimum moved (`rail.h`'s rule).
		rail.onRailWidthChanged([this]() {
			resized();
			redraw();
			minimumSizeChanged();
		});
	});
}

float ResonateExpanded::railsWidth() const {
	return (droneFx_ != nullptr ? droneFx_->railWidth() : 0.0f) + (wetFx_ != nullptr ? wetFx_->railWidth() : 0.0f);
}

visage::Point ResonateExpanded::minimumSize() const {
	const visage::Point row = ResonateFace::panelMinimum();
	visage::Point p { row.x + 2.0f * kPad + railsWidth(), row.y + 2.0f * kPad };
	// AN OPEN RAIL IS A FLOOR ON THE HEIGHT, never a term in it (SKLITTER's rule): a panel tall enough for either is
	// tall enough for both.
	forEachRail(droneFx_.get(), wetFx_.get(), [&](const FxLaneRail& rail) {
		if (!rail.collapsed()) {
			p.y = std::max(p.y, rail.openMinimumHeight());
		}
	});
	return p;
}

void ResonateExpanded::modulationBound(ModState* mod, EditHistory* history) {
	face_->setModulation(mod, history);
	routeControl_->setModulation(mod, history);
	// BOTH LANES FOLLOW THE OUTER STATE; each keeps its own for what is armed inside it (SKLITTER's rails).
	forEachRail(droneFx_.get(), wetFx_.get(), [&](FxLaneRail& rail) {
		rail.setOuterModulation(mod, history, [this] { return watchedDevice(); });
	});
}

void ResonateExpanded::setPresentedOn(SurfaceLayers* host) {
	forEachRail(droneFx_.get(), wetFx_.get(), [&](FxLaneRail& rail) { rail.setPresentedOn(host); });
}

void ResonateExpanded::onInnerNestChanged() {
	// ONE NEST, BOTH RAILS: the nest is the holder's, and the lanes share one instance counter (resonate.cpp).
	forEachRail(droneFx_.get(), wetFx_.get(), [this](FxLaneRail& rail) { rail.setNest(innerNest()); });
}

void ResonateExpanded::layOut() {
	const float rails = railsWidth();
	face_->layOutPanel({ kPad, kPad, std::max(0.0f, static_cast<float>(width()) - rails - 2.0f * kPad),
	                   std::max(0.0f, static_cast<float>(height()) - 2.0f * kPad) });
	// FROM THE RIGHT EDGE INWARD, so left to right the panel reads face | DRONE FX | WET FX — the signal's order.
	float right = static_cast<float>(width());
	for (FxLaneRail* rail : { wetFx_.get(), droneFx_.get() }) {
		const float wide = rail->railWidth();
		right -= wide;
		rail->setBounds(right, 0.0f, wide, static_cast<float>(height()));
	}
}

void ResonateExpanded::resized() { layOut(); }

bool ResonateExpanded::updateContent() {
	const ResonateFace::Changes c = face_->refresh();
	bool moved = c.any();
	if (c.gates) {
		layOut();
		redraw();
	}
	// THE CHAINS, RE-ASKED EVERY TICK: the handle is contemporaneous (trap T9), and a body arriving rebuilds the
	// instance. Asked once for both rails.
	void* const held = host_.containerSurfaceOf(id_);
	forEachRail(droneFx_.get(), wetFx_.get(), [&](FxLaneRail& rail) {
		moved = rail.follow(held) || moved;
		moved = rail.updateCards() > 0 || moved;
	});
	chainPin_.hold(held); // held now, the old let go — after both rails dropped what they built over it (backlog JX)
	// THE ROUTING SWITCH IS SHOWN WHILE WET FX HOLDS SOMETHING — the record's answer, on this thread.
	const rack::InnerChain* wet = wetChainOf(held);
	if (droneFx_->setAccessoryGate(wet != nullptr && wet->bridge() != nullptr && wet->bridge()->chainLength() > 0)) {
		minimumSizeChanged();
		moved = true;
	}
	return moved;
}

void ResonateExpanded::draw(visage::Canvas& canvas) {
	// NO GROUND OF ITS OWN — the overlay's panel is already a raised surface (`HarmExpanded::draw`).
	face_->drawCaptions(canvas);
}

} // namespace feathers::ui
