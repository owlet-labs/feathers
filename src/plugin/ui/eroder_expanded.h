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
	static constexpr float kSideWidth = 252.0f;
	static constexpr float kFace = 44.0f;
	static constexpr float kSwitchHeight = 18.0f;
	static constexpr size_t kPerRow = 3;

	[[nodiscard]] EroderPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	[[nodiscard]] std::vector<visage::Frame*> dustWidgetsForTest() const;
	[[nodiscard]] std::vector<visage::Frame*> combWidgetsForTest() const;
	[[nodiscard]] Knob* widthForTest() const;
	[[nodiscard]] Knob* driveForTest() const { return drive_.get(); }
	[[nodiscard]] ModeValueCell* driveCellForTest() const { return driveCell_.get(); }
	[[nodiscard]] PairCell* drivePairForTest() const { return drivePair_.get(); }
	[[nodiscard]] Knob* hpWetForTest() const { return hpWet_.get(); }
	[[nodiscard]] SwitchChip* subThruForTest() const { return subThru_.get(); }
	[[nodiscard]] Knob* followForTest() const { return follow_.get(); }

private:
	[[nodiscard]] DeviceControl& control(fx::eroder::Param p) const;
	[[nodiscard]] bool dust() const;
	void layOut();
	void refreshHonesty();

	backplane::DeviceHost& dhost_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	int laidOutDust_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<EroderPlot> plot_;
	std::unique_ptr<SegmentedControl> mode_;
	/// DUST'S SEVEN PLAIN KNOBS, in the grid's order. Follow and Mix come after them and are the Comb's too.
	std::vector<std::unique_ptr<Knob>> dustKnobs_;
	std::unique_ptr<Knob> follow_;
	std::unique_ptr<Knob> drive_;
	std::unique_ptr<ModeValueCell> driveCell_;
	std::unique_ptr<PairCell> drivePair_;
	std::unique_ptr<Knob> hpWet_;
	std::unique_ptr<SwitchChip> subThru_;
	std::unique_ptr<Knob> mix_;
	std::unique_ptr<SwitchChip> track_;
	std::unique_ptr<ZoneCoordCell> tone_;
	std::unique_ptr<ZoneCoordCell> character_;
	std::unique_ptr<Knob> cutoff_;
	std::unique_ptr<GammaBox> gamma_;
};

} // namespace feathers::ui
