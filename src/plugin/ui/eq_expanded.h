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

// THE EQ'S PANEL. UNCAPPED ON BOTH AXES (arbitrarily large, for precise control): the plot takes every pixel the window gives it — more pixels, finer handle
// placement and a denser grid — and the strip and globals sit in one row beneath it, the output meter
// beside the plot. It holds every control the card does, and the tilt pivot as a box as well.

#include "backplane/device_host.h"
#include "fx/eq/eq_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/eq_controls.h"
#include "plugin/ui/eq_graph.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <string>
#include <vector>

namespace feathers::ui {

class EqExpanded final : public ExpandedView {
public:
	EqExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	EqExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;

	static constexpr float kPad = 12.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kMeterWidth = 22.0f;
	static constexpr float kRowGap = 8.0f;
	/// THE MINIMUM (§6.2, derived from the rows it must hold): the strip's three knob cells, the globals'
	/// row of up to seven cells (Offset, Amount, Tilt, Pivot, Output, Phase, Key Track) at 64 px each — the width
	/// "Amount" and "Output" need to be printed whole — the meter, and a plot at least 180 px tall.
	static constexpr float kMinPlotHeight = 180.0f;
	static constexpr float kStripWidth = 3.0f * 64.0f;
	static constexpr float kGlobalsWidth = 7.0f * 64.0f;

	// TEST SEAMS
	[[nodiscard]] EqGraph* graphForTest() const { return graph_.get(); }
	[[nodiscard]] EqControls* controlsForTest() const { return controls_.get(); }
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;
	[[nodiscard]] visage::Bounds meterBounds() const;

private:
	[[nodiscard]] DeviceControl& control(size_t i) const;
	[[nodiscard]] float bottomHeight() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> adapters_;
	std::unique_ptr<EqGraph> graph_;
	std::unique_ptr<EqControls> controls_;
	std::vector<int> laidOutKey_;
	float drawnPeak_ = -1.0f;
	bool drawnHadReadings_ = false;
	int drawnSelected_ = -1;
};

} // namespace feathers::ui
