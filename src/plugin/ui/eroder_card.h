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
#include "plugin/ui/fx_card.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <vector>

// THE ERODER'S CARD — the picture as its centrepiece (`eroder_plot.h`), the engine's own controls under it.
//
//   ┌ ERODER ────────────────────────────── (on) ┐
//   │ ┌ spectrum + band ──────────── 0.12 ms ┐   │  DUST: in, out, and the band; its handle is Freq / Amount,
//   │ └──────────────────────────────────────┘   │  the wheel Width. COMB: the eleven bars.
//   │ [   Dust   |   Comb   ]                     │  the engine, both choices on the face
//   │  Amount    Freq     Width     Mix           │  DUST's knobs          COMB: Tone  Char  Cutoff  Mix
//   │ [Source 100%] [Grit 0%] (Track)             │  DUST's small row      COMB: [Gamma]
//   └─────────────────────────────────────────────┘
//
// **ONLY THE RUNNING ENGINE'S CONTROLS ARE DRAWN**: the other engine's are hidden, not greyed, since nothing but
// the engine switch could enable them. Stereo, Resonance and Drive are the window's.
// **THE KNOBS STAY BESIDE THE PLOT'S HANDLE**: Amount and Width have no route gesture on the plot, so the knob is
// where a route to them is made.

namespace feathers::ui {

class EroderCard final : public FxCard {
public:
	EroderCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	EroderCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::eroder::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 8.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kPlotHeight = 112.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kSmallRowHeight = 16.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	[[nodiscard]] EroderPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	/// THE ENGINE'S OWN WIDGETS, in the order they are laid out: the knob row, then the small row.
	[[nodiscard]] std::vector<visage::Frame*> dustWidgetsForTest() const;
	[[nodiscard]] std::vector<visage::Frame*> combWidgetsForTest() const;
	[[nodiscard]] Knob* mixForTest() const { return mix_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::eroder::Param p) const;
	[[nodiscard]] bool dust() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE ENGINE THE WIDGETS WERE LAST LAID OUT FOR.
	int laidOutDust_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<EroderPlot> plot_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<Knob> amount_;
	std::unique_ptr<Knob> freq_;
	std::unique_ptr<Knob> width_;
	std::unique_ptr<Knob> mix_;
	std::unique_ptr<ValueBox> source_;
	std::unique_ptr<ValueBox> grit_;
	std::unique_ptr<SwitchChip> track_;
	std::unique_ptr<ZoneCoordCell> tone_;
	std::unique_ptr<ZoneCoordCell> character_;
	std::unique_ptr<Knob> cutoff_;
	std::unique_ptr<GammaBox> gamma_;
};

} // namespace feathers::ui
