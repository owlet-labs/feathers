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

// THE EQ'S CARD. Composed around the curve, not a knob
// list: the plot with its handles on top, the output meter beside it (R14), then ONE strip for the
// selected band, then the four globals. The tilt pivot is the diamond on the plot.

#include "backplane/device_host.h"
#include "fx/eq/eq_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/eq_controls.h"
#include "plugin/ui/eq_graph.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

namespace feathers::ui {

class EqCard final : public FxCard {
public:
	EqCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	EqCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;

	const device::Descriptor* descriptor() const override { return &fx::eq::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 8.0f;
	static constexpr float kGraphHeight = 116.0f;
	static constexpr float kKnobDiameter = 30.0f;
	static constexpr float kMeterWidth = 16.0f;
	static constexpr float kRowGap = 6.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds meterBounds() const;
	[[nodiscard]] bool hasReadings() const;

	// TEST SEAMS
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;
	[[nodiscard]] EqGraph* graphForTest() const { return graph_.get(); }
	[[nodiscard]] EqControls* controlsForTest() const { return controls_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(size_t i) const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_adapters_;
	std::unique_ptr<EqGraph> graph_;
	std::unique_ptr<EqControls> controls_;
	std::vector<int> laidOutKey_;
	float drawnPeak_ = -1.0f;
	bool drawnHadReadings_ = false;
};

} // namespace feathers::ui
