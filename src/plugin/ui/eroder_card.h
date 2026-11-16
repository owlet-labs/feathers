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
#include "plugin/ui/etch_slots.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/pair_cell.h"
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
//   │                                             │  RUST: Bias where Dust has Amount
//   │ [Source 100%] [Grit 0%] (Track)             │  DUST's small row      COMB: [Gamma]
//   │ ETCH: [A knob|Type S M] [~] [B knob|Type S M]│  the slots on the knob row; [Freq] [Mix] (Track) under it
//   │ ┌─ DRIVE ─────────┐                         │
//   │ │ Amount Soft 0%  │  Emph     Reso          │  the shaper pair, Emphasis and the feedback are every engine's
//   │ └─────────────────┘                         │
//   └─────────────────────────────────────────────┘
//
// **ONLY THE RUNNING ENGINE'S CONTROLS ARE DRAWN**: the other engine's are hidden, not greyed, since nothing but
// the engine switch could enable them. The rest of each engine's controls are the window's.
// **DRIVE IS A PAIR** (`pair_cell.h`): how hard, and into what — the Type and its own Shape in one mode cell.
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
	static constexpr float kPlotHeight = 98.0f;
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
	/// Rust's own: its Bias, where Dust has Amount.
	[[nodiscard]] std::vector<visage::Frame*> rustWidgetsForTest() const;
	/// What Dust and Rust share and the Comb does not show: the shaker's controls.
	[[nodiscard]] std::vector<visage::Frame*> shakerWidgetsForTest() const;
	/// ETCH'S OWN: the slot row (two slots and their route, on one line), and Freq and Mix as compact boxes in the small row
	/// (the knob row is the slots' there).
	[[nodiscard]] std::vector<visage::Frame*> etchWidgetsForTest() const;
	[[nodiscard]] EtchSlots& etchSlotsForTest() { return etch_; }
	[[nodiscard]] ValueBox* freqBoxForTest() const { return freqBox_.get(); }
	[[nodiscard]] ValueBox* mixBoxForTest() const { return mixBox_.get(); }
	[[nodiscard]] Knob* mixForTest() const { return mix_.get(); }
	[[nodiscard]] Knob* widthForTest() const { return width_.get(); }
	[[nodiscard]] Knob* driveForTest() const { return drive_.get(); }
	[[nodiscard]] ModeValueCell* driveCellForTest() const { return driveCell_.get(); }
	[[nodiscard]] PairCell* drivePairForTest() const { return drivePair_.get(); }
	/// THE SHARED SECTION'S CELLS ON THE CARD: the emphasis knob, the feedback knob (Resonance in the shakers, the outer
	/// Loop in the Comb), and the Comb's two chips. HP Wet and Follow are the window's.
	[[nodiscard]] Knob* emphasisForTest() const { return emphasis_.get(); }
	[[nodiscard]] Knob* resonanceForTest() const { return resonance_.get(); }
	[[nodiscard]] Knob* loopForTest() const { return loop_.get(); }
	[[nodiscard]] SwitchChip* tapForTest() const { return tap_.get(); }
	[[nodiscard]] SwitchChip* restoreForTest() const { return restore_.get(); }
	/// **READ OFF THE WIDGET TREE** (`ControlFace`), every engine's and hidden ones included: what a hand can reach here.
	[[nodiscard]] std::vector<fx::eroder::Param> addressedParamsForTest() const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::eroder::Param p) const;
	[[nodiscard]] fx::eroder::Mode engine() const;
	void layOut();
	/// What is drawn out of the signal because another control makes it so (the honesty rule).
	void refreshHonesty();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE ENGINE THE WIDGETS WERE LAST LAID OUT FOR.
	int laidOutEngine_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<EroderPlot> plot_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<Knob> drive_;
	std::unique_ptr<ModeValueCell> driveCell_;
	std::unique_ptr<PairCell> drivePair_;
	std::unique_ptr<Knob> emphasis_;
	std::unique_ptr<Knob> resonance_;
	std::unique_ptr<Knob> loop_;
	std::unique_ptr<SwitchChip> tap_;
	std::unique_ptr<SwitchChip> restore_;
	std::unique_ptr<Knob> amount_;
	std::unique_ptr<Knob> bias_;
	std::unique_ptr<Knob> freq_;
	std::unique_ptr<Knob> width_;
	std::unique_ptr<Knob> mix_;
	EtchSlots etch_;
	std::unique_ptr<ValueBox> freqBox_;
	std::unique_ptr<ValueBox> mixBox_;
	std::unique_ptr<ValueBox> source_;
	std::unique_ptr<ValueBox> grit_;
	std::unique_ptr<SwitchChip> track_;
	std::unique_ptr<ZoneCoordCell> tone_;
	std::unique_ptr<ZoneCoordCell> character_;
	std::unique_ptr<Knob> cutoff_;
	std::unique_ptr<GammaBox> gamma_;
};

} // namespace feathers::ui
