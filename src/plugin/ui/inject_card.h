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
#include "fx/inject/inject_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// SIDECHAIN INJECT'S CARD (backlog NW) — a Mix knob and a Gain knob, and beside them one bar: what the sidechain is adding,
// as it adds it. Under them ONE LINE that is empty while something sounds on the plugin's Sidechain input and says
// "sidechain silent" when nothing does (the honesty rule, as the vocoder and COMP's Side detector say it).
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word: Mix and Gain are a knob each; the enable is the
// square in the head, drawn by `FxCard`. The bar and the line are readings and reach nothing.

namespace feathers::ui {

class InjectCard final : public FxCard {
public:
	InjectCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — see `GenericCard`.
	InjectCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::inject::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kLineHeight = 16.0f;
	/// THE BAR'S COLUMN, on the right.
	static constexpr float kMeterWidth = 40.0f;
	static constexpr float kMeterGap = 8.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds meterBounds() const;
	[[nodiscard]] visage::Bounds lineBounds() const { return line_; }

	/// ONE READING, BY NAME, or 0 when nothing is published.
	[[nodiscard]] float reading(fx::inject::Reading r) const;
	[[nodiscard]] bool hasReadings() const;
	/// **"SIDECHAIN SILENT"**: the device publishes, and nothing sounds on the sidechain. With no data it is false: the card
	/// claims nothing it was not told.
	[[nodiscard]] bool sidechainSilent() const;
	/// THE BAR'S FRACTION — `drawMeter` reads this and nothing else, so a case asserting it asserts the picture's number.
	[[nodiscard]] float barFraction() const;

	[[nodiscard]] std::vector<fx::inject::Param> addressedParamsForTest() const;
	[[nodiscard]] Knob* knobForTest(fx::inject::Param p) const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::inject::Param p) const;
	[[nodiscard]] float contentRight() const;
	void layOut();
	void drawMeter(visage::Canvas& canvas);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	/// WHAT THE BAR AND THE LINE ON SCREEN WERE DRAWN FROM — `updateContent`'s contract.
	float drawnBar_ = -1.0f;
	bool drawnSilent_ = false;
	bool drawnHadReadings_ = false;
	visage::Bounds line_ {};

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<Knob> mix_;
	std::unique_ptr<Knob> gain_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
