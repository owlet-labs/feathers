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

#include "backplane/device_host.h"
#include "fx/smooth/smooth_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/smooth_picture.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <vector>

// THE SMOOTH SHAPER'S CARD — the transfer curve is the card, and each control sits where it acts on it.
//
// The card is built around a visualizer and uses its space rather than listing knobs. The layout:
//
//   [ Mode ▾ 3579              ] [Poly]    WHICH shaper, and WHERE it sits (per voice or on the bus)
//   ┌──────────────────────────────────┐
//   │  transfer curve, full width      │   the real core at these settings (`smooth_picture.h`)
//   │ ╎ ceiling            ceiling ╎   │   DRIVE IS THE CEILING: drag it toward the signal for more
//   │ ◆ DRIVE +0.0 dB ▲in      CLIP ◆  │   ▲ = the live input peak, on the bus
//   └──────────────────────────────────┘
//   (◯) DRIVE   [0.42] HARMONIC   (◯) MIX   how hard, where in the shaper, and how much of it is heard
//   [ 0.30 TWIST ] [ On ] [Width][Evens]       the sweep and its power; the four pills,
//                         [Rect ][Fdbk ]       each a FaderPill, any of them together
//
// THE HARMONIC BARS ARE THE PANEL'S: off the card, their row went to Drive as a real
// control and to a taller curve.
//
// WHAT REACHES WHAT — the reachability guard takes this card's word, so the word is computed:
//   - Mode (a ValueBox with its menu, never a cycling chip), Poly (a SwitchChip: two states), Drive and Mix
//     (knobs with the mod ring; Drive is ALSO the ceiling line on the curve, a drag `SmoothCurveView` answers),
//     Harmonic and Twist (ZoneCoordCells, with the coarse.fine click zones: both are unbounded Number coordinates, so a press on a digit drags by that place; both show
//     their lap's sigil and no dropdown — `makeSmoothCoordCell`, the panel's too), Twist's power (a SwitchChip),
//     the four pills (FaderPills: a drag sets the amount, a click the power — `DisperserCard`'s), Enabled (the head).
//   - H Phase and Gamma are the PANEL's: the card would crowd them, and `expandBounds` opens it.

namespace feathers::ui {

class SmoothCard final : public FxCard {
public:
	SmoothCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	           Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	SmoothCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	/// THE PICTURE'S DELIVERY: recompute when a setting moved, move the input marker when the reading did,
	/// and `redraw()` only then — a still patch costs one comparison a tick and no frame.
	bool updateContent() override;

	const device::Descriptor* descriptor() const override { return &fx::smooth::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	/// 150, NOT 124: the harmonic bars came off the card and part of their row went to the curve.
	static constexpr float kCurveHeight = 150.0f;
	static constexpr float kChipWidth = 40.0f;
	/// One knob cell: Drive, Harmonic and Mix across the row under the curve.
	static constexpr float kKnobCell = 62.0f;
	/// THE PILLS, `DisperserCard`'s size, two by two beside Twist.
	static constexpr float kPillHeight = 20.0f;
	static constexpr float kPillGap = 4.0f;
	static constexpr float kPillWidth = 52.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	//-- test seams ---------------------------------------------------------------------------------//
	[[nodiscard]] std::vector<fx::smooth::Param> addressedParamsForTest() const;
	[[nodiscard]] visage::Frame* widgetForTest(fx::smooth::Param p) const;
	[[nodiscard]] SmoothCurveView* curveForTest() const { return curve_.get(); }
	[[nodiscard]] FaderPill* pillForTest(size_t i) const { return i < pills_.size() ? pills_[i].get() : nullptr; }
	[[nodiscard]] Knob* driveForTest() const { return drive_.get(); }
	[[nodiscard]] const SmoothPictureModel& modelForTest() const { return model_; }
	[[nodiscard]] bool hasReadings() const;
	[[nodiscard]] float inputPeak() const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::smooth::Param p) const;
	[[nodiscard]] fx::smooth::Settings settings() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	SmoothPictureModel model_ {};
	bool drawnHadPeak_ = false;
	float drawnPeak_ = -1.0f;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<ValueBox> mode_;
	std::unique_ptr<SwitchChip> poly_;
	std::unique_ptr<SmoothCurveView> curve_;
	std::unique_ptr<Knob> drive_;
	std::unique_ptr<ZoneCoordCell> harmonic_;
	std::unique_ptr<ZoneCoordCell> twist_;
	std::unique_ptr<SwitchChip> sweepOn_;
	std::vector<std::unique_ptr<FaderPill>> pills_;
	std::unique_ptr<Knob> mix_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
