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
#include "fx/clip/clip_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// CLIP'S CARD — the mode, the three globals, the mode's own controls, and IN/OUT bars with an OVER
// LED. `CompCard` is its sibling and `BusUtilCard` the template both follow.
//
// **THE MODE IS A BOX WITH A MENU, NOT A SEGMENTED ROW**: five named tops, and every one is visible in
// the menu before it is applied (CLAUDE.md's toggle rule). Oversample is two states, so a chip.
//
// **A MODE'S CONTROLS ARE HIDDEN OUTSIDE IT** — each is declared live exactly where the curve reads it
// (`clip_types.h`'s gate masks), asked with `device::relevantFor` and re-asked when the Mode field moves.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Mode — a value box. Oversample — a chip. Bits — a segmented control.
//   - Drive, Ceiling, Output, Knee, Freq, Foldback, Grit, Depth — a knob each, the last five by mode.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.
// The meter and the LED reach nothing: they are readings.

namespace feathers::ui {

class ClipCard final : public FxCard {
public:
	ClipCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	ClipCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::clip::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// The knob's own column at this face — see `CompCard::kCellHeight`.
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kSwitchHeight = 16.0f;
	/// THREE TO A ROW — the globals are three, and no mode has more than three controls of its own
	/// (Texture's Knee, Freq and Foldback; Bits' Knee and a two-cell segmented control).
	static constexpr size_t kKnobsPerRow = 3;
	/// THE METER COLUMN: the LED over two bars.
	static constexpr float kMeterWidth = 40.0f;
	static constexpr float kMeterGap = 8.0f;
	static constexpr float kLedRadius = 6.0f;
	/// THE OVERSAMPLE CHIP'S WIDTH — enough for "4x" and "Off" at the chip's type.
	static constexpr float kChipWidth = 36.0f;
	/// HOW FAR BITS' SEGMENTED CONTROL SITS IN FROM EACH EDGE OF ITS TWO CELLS, so it does not touch the
	/// knob beside it.
	static constexpr float kBitsInset = 4.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds headBounds() const;
	[[nodiscard]] visage::Bounds meterBounds() const;

	[[nodiscard]] float reading(fx::clip::Reading r) const;
	[[nodiscard]] bool hasReadings() const;
	[[nodiscard]] bool isLive(fx::clip::Param p) const;

	/// **IS THE OVER LED LIT?** Iff something is published and the held Over reading is at least a half —
	/// the reading is 0 or 1, and a half is the threshold a ramped value would be read against.
	[[nodiscard]] bool ledLit() const;

	/// THE TWO BAR FRACTIONS THE METER DRAWS — `drawMeter` reads these and nothing else.
	struct MeterFractions {
		float in = 0.0f;
		float out = 0.0f;
	};
	[[nodiscard]] MeterFractions meterForTest() const { return meterFractions(); }

	[[nodiscard]] std::vector<fx::clip::Param> addressedParamsForTest() const;
	/// THE WIDGET ON `p` — knob, box, chip or segmented control — or null.
	[[nodiscard]] visage::Frame* widgetForTest(fx::clip::Param p) const;
	[[nodiscard]] ValueBox* modeForTest() const { return mode_.get(); }
	[[nodiscard]] SwitchChip* oversampleForTest() const { return oversample_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::clip::Param p) const;
	[[nodiscard]] MeterFractions meterFractions() const;
	[[nodiscard]] int modeNow() const;

	[[nodiscard]] float contentRight() const;
	void layOut();
	void drawMeter(visage::Canvas& canvas);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	int laidOutMode_ = -1;
	float drawnIn_ = -1.0f;
	float drawnOut_ = -1.0f;
	float drawnOver_ = -1.0f;
	/// WHETHER THE METER ON SCREEN WAS DRAWN FROM READINGS AT ALL — see `CompCard::updateContent`.
	bool drawnHadReadings_ = false;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	/// THE GLOBALS' KNOBS, then the mode knobs — see `kGlobals` and `kModeKnobs` in the .cpp.
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<ValueBox> mode_;
	std::unique_ptr<SwitchChip> oversample_;
	std::unique_ptr<SegmentedControl> bits_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
