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
#include "fx/eroder/eroder_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/eroder_plot.h"
#include "plugin/ui/eroder_flow.h"
#include "plugin/ui/etch_slots.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/pair_cell.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// THE ERODER'S WINDOW — laid out for its room:
//
//   ┌───────────────────────────────────────────────┬──────────────────────────┐
//   │ the picture, large (`EroderPlot`): the spectra │ [   Dust   |   Comb   ]  │
//   │ and the band with its hertz, or the eleven     │  Amount   Freq    Width  │  DUST: every control
//   │ bars with their numbers                        │  Source   Grit    Stereo │
//   ├───────────────────────────────────────────────┤  Reso     Drive   Mix    │
//   │ READ HEAD: left over right, the last seconds   │ (Track)                  │
//   └───────────────────────────────────────────────┴──────────────────────────┘
//                                                      COMB: Tone  Character  Cutoff / Mix, [Gamma]
//
// EVERY CONTROL OF THE RUNNING ENGINE, which is every control the card offers and the three it leaves out
// (Stereo, Resonance, Drive). The other engine's are hidden, as on the card.

namespace feathers::ui {

class EroderExpanded final : public ExpandedView {
public:
	EroderExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	               Notifier* notifier = nullptr);
	EroderExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;

	static constexpr float kPad = 14.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kSideWidth = 504.0f;
	static constexpr float kFace = 44.0f;
	static constexpr float kSwitchHeight = 18.0f;
	static constexpr size_t kPerRow = 6;
	/// THE GROUPS: a box round each, its caption band, the room inside its edge, the gap between them and between rows.
	static constexpr float kGroupCaption = 14.0f;
	static constexpr float kGroupPad = 6.0f;
	static constexpr float kGroupGap = 6.0f;
	static constexpr float kRowGap = 6.0f;
	[[nodiscard]] static constexpr float groupHeight(float content) { return kGroupCaption + content + kGroupPad; }

	/// A CAPTIONED GROUP, in the window's x and y.
	struct Group {
		std::string caption;
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
	};
	[[nodiscard]] const std::vector<Group>& groupsForTest() const noexcept { return groups_; }
	[[nodiscard]] EroderFlow* flowForTest() const noexcept { return flow_.get(); }
	[[nodiscard]] SwitchChip* trackForTest() const { return track_.get(); }
	[[nodiscard]] ZoneCoordCell* toneForTest() const { return tone_.get(); }
	[[nodiscard]] ZoneCoordCell* characterForTest() const { return character_.get(); }
	[[nodiscard]] GammaBox* gammaForTest() const { return gamma_.get(); }

	[[nodiscard]] EroderPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	/// WHAT ONE ENGINE SHOWS AND THE OTHERS DO NOT ALL SHOW: Dust's, the Comb's, Rust's.
	[[nodiscard]] std::vector<visage::Frame*> dustWidgetsForTest() const;
	[[nodiscard]] std::vector<visage::Frame*> combWidgetsForTest() const;
	[[nodiscard]] std::vector<visage::Frame*> rustWidgetsForTest() const;
	/// A plain knob by the control it drives, or null.
	[[nodiscard]] Knob* knobForTest(fx::eroder::Param p) const { return knob(p); }
	[[nodiscard]] Knob* widthForTest() const { return knob(fx::eroder::Param::Width); }
	[[nodiscard]] Knob* driveForTest() const { return drive_.get(); }
	[[nodiscard]] ModeValueCell* driveCellForTest() const { return driveCell_.get(); }
	[[nodiscard]] PairCell* drivePairForTest() const { return drivePair_.get(); }
	[[nodiscard]] Knob* hpWetForTest() const { return knob(fx::eroder::Param::HpWet); }
	[[nodiscard]] SwitchChip* subThruForTest() const { return subThru_.get(); }
	[[nodiscard]] SwitchChip* blendForTest() const { return blend_.get(); }
	[[nodiscard]] SwitchChip* restoreForTest() const { return restore_.get(); }
	[[nodiscard]] SwitchChip* inLoopForTest() const { return inLoop_.get(); }
	[[nodiscard]] SwitchChip* tapForTest() const { return tap_.get(); }
	[[nodiscard]] ZoneSelect* shapeForTest() const { return &emphasisCell_->select(); }
	[[nodiscard]] ModeValueCell* emphasisCellForTest() const { return emphasisCell_.get(); }
	[[nodiscard]] PairCell* emphasisPairForTest() const { return emphasisPair_.get(); }
	[[nodiscard]] PairCell* loopPairForTest() const { return loopPair_.get(); }
	[[nodiscard]] EtchSlots& etchSlotsForTest() { return etch_; }
	[[nodiscard]] Knob* followForTest() const { return knob(fx::eroder::Param::Follow); }
	/// **READ OFF THE WIDGET TREE** (`ControlFace`), every engine's and hidden ones included: what a hand can reach here.
	[[nodiscard]] std::vector<fx::eroder::Param> addressedParamsForTest() const;

private:
	[[nodiscard]] DeviceControl& control(fx::eroder::Param p) const;
	[[nodiscard]] Knob* knob(fx::eroder::Param p) const;
	[[nodiscard]] fx::eroder::Mode engine() const;
	void layOut();
	void refreshHonesty();

	backplane::DeviceHost& dhost_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE ENGINE THE WIDGETS WERE LAST LAID OUT FOR.
	int laidOutEngine_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<EroderPlot> plot_;
	std::unique_ptr<EroderFlow> flow_;
	std::vector<Group> groups_;
	std::unique_ptr<SegmentedControl> mode_;
	/// EVERY PLAIN KNOB, by the control it drives; which are shown is the engine's (`layOut`).
	std::vector<std::pair<fx::eroder::Param, std::unique_ptr<Knob>>> knobs_;
	EtchSlots etch_;
	std::unique_ptr<Knob> drive_;
	std::unique_ptr<ModeValueCell> driveCell_;
	std::unique_ptr<PairCell> drivePair_;
	std::unique_ptr<SwitchChip> subThru_;
	std::unique_ptr<SwitchChip> track_;
	std::unique_ptr<SwitchChip> blend_;
	std::unique_ptr<SwitchChip> restore_;
	std::unique_ptr<SwitchChip> inLoop_;
	std::unique_ptr<SwitchChip> tap_;
	/// THE EMPHASIS'S CELL: the shape's selector with the Offset and the Bell's Width stacked under it.
	std::unique_ptr<ModeValueCell> emphasisCell_;
	std::unique_ptr<PairCell> emphasisPair_;
	std::unique_ptr<PairCell> loopPair_;
	std::unique_ptr<ZoneCoordCell> tone_;
	std::unique_ptr<ZoneCoordCell> character_;
	std::unique_ptr<GammaBox> gamma_;
};

} // namespace feathers::ui
