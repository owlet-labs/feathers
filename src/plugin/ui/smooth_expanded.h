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
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/smooth_picture.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <string>
#include <vector>

// THE SMOOTH SHAPER'S PANEL — the complete device around the same two pictures, large.
//
// By design the expanded view holds EVERY control, so nothing needs the card. The layout:
//
//   [ Mode ▾ 3579 ] [Poly] [On]
//   ┌──── TRANSFER (annotated, the ceiling is Drive) ───┐ ┌──── HARMONICS ─────┐
//   │                                                   │ │ H1 … H9            │
//   │                                                   │ ├────────────────────┤
//   │                                                   │ │[0.42]HARMONIC      │
//   └───────────────────────────────────────────────────┘ │        H PHASE     │
//   (◯) DRIVE   (◯) MIX   │ [0.30] TWIST ‹ Fdbk ›  GAMMA  └────────────────────┘
//
// Drive is here as a KNOB as well as the ceiling handle: the knob carries the modulation ring and the route
// drag in the form every other surface uses. Harmonic sits with H Phase because both move the harmonic blend;
// Twist sits with Gamma, which is 1024 of its laps (T Phase, 128 of them, was folded into it — 2026-09-26).
// Harmonic and Twist are ZoneCoordCells, as on the card, so a press on a digit drags by that place.

namespace feathers::ui {

class SmoothExpanded final : public ExpandedView {
public:
	SmoothExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	               Notifier* notifier = nullptr);
	SmoothExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	bool updateContent() override;
	visage::Point minimumSize() const override;
	/// CAPPED BOTH WAYS at the natural size plus one `kPad` (backlog EI): every cell is fixed.
	visage::Point maximumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kCurveWidth = 420.0f;
	static constexpr float kCurveHeight = 250.0f;
	static constexpr float kSideWidth = 236.0f;
	static constexpr float kCaptionHeight = 12.0f;
	static constexpr float kPillHeight = 20.0f;
	static constexpr float kSweepChipWidth = 64.0f;

	//-- test seams ---------------------------------------------------------------------------------//
	[[nodiscard]] std::vector<fx::smooth::Param> addressedParamsForTest() const;
	[[nodiscard]] SmoothCurveView* curveForTest() const { return curve_.get(); }
	[[nodiscard]] SmoothHarmonicsView* barsForTest() const { return bars_.get(); }
	[[nodiscard]] FaderPill* pillForTest(size_t i) const { return i < pills_.size() ? pills_[i].get() : nullptr; }
	[[nodiscard]] const SmoothPictureModel& modelForTest() const { return model_; }

private:
	[[nodiscard]] DeviceControl& control(fx::smooth::Param p) const;
	[[nodiscard]] fx::smooth::Settings settings() const;
	[[nodiscard]] bool hasReadings() const;
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
	std::unique_ptr<SmoothHarmonicsView> bars_;
	std::unique_ptr<Knob> drive_;
	std::unique_ptr<Knob> mix_;
	std::unique_ptr<ZoneCoordCell> harmonic_;
	std::unique_ptr<GammaBox> harmonicPhase_;
	std::unique_ptr<GammaBox> gamma_;
	std::unique_ptr<ZoneCoordCell> twist_;
	std::unique_ptr<SwitchChip> sweepOn_;
	std::vector<std::unique_ptr<FaderPill>> pills_;
};

} // namespace feathers::ui
