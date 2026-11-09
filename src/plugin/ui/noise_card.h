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
#include "fx/noise/noise_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/noise_plot.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// NOISE'S CARD — composed around its picture (`NoisePlot`), not a knob list.
//
// THE FACE, TOP TO BOTTOM: the Source (both choices on the face) and Poly; the plot, whose handle IS Cutoff and
// Resonance; the filter's response and Key Track; Level, Stereo and the source's own amount (Colour, or
// Density); the seed; and under Impulses, the timing and the hits. **THE CARD IS ONE HEIGHT FOR BOTH SOURCES**:
// Noise has fewer controls, so its plot takes the impulse rows' room rather than leaving a gap.
//
// **A CONTROL IS SHOWN EXACTLY WHERE THE DEVICE READS IT** (the honesty rule): each is asked of `noise::liveIn`,
// which follows the gate chain up — Jitter is gated on Timing and Timing on Source — and the card re-lays itself
// when any gate moves.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Source, Filter Type, Timing, Seed Mode — segmented controls (every choice on the face).
//   - Poly, Polarity — chips (two states).
//   - Level, Colour, Key Track, Stereo, Seed, Density, Sync, Jitter, Cluster, Hit Size, Decay — value boxes (a choice's
//     box opens a menu of all of them before one is applied).
//   - Cutoff, Resonance — the plot's handle.
//   - Trig — the pad on the plot (`TrigPad`), under Impulses.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.

namespace feathers::ui {

class NoiseCard final : public FxCard {
public:
	NoiseCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	NoiseCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::noise::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kChipWidth = 40.0f;
	/// THE PLOT UNDER IMPULSES; under Noise it also takes the three impulse rows. 82, not 92, since Decay's row (backlog
	/// NA): the card must stay inside `kCardUnitHeight`, which the rack hands it (test_rack_editor's KQ sweep).
	static constexpr float kPlotHeight = 82.0f;
	static constexpr size_t kCells = 3;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	[[nodiscard]] bool isLive(fx::noise::Param p) const;
	[[nodiscard]] std::vector<fx::noise::Param> addressedParamsForTest() const;
	/// THE WIDGET ON `p` — box, chip, segmented control or the plot — or null.
	[[nodiscard]] visage::Frame* widgetForTest(fx::noise::Param p) const;
	[[nodiscard]] NoisePlot* plotForTest() const { return plot_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::noise::Param p) const;
	/// THE GATES' VALUES, packed — what `updateContent` compares to decide whether to re-lay the face.
	[[nodiscard]] uint32_t layoutKey() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	uint32_t laidOutKey_ = ~0u;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<NoisePlot> plot_;
	std::unique_ptr<SegmentedControl> source_, filterType_, timing_, seedMode_;
	std::unique_ptr<SwitchChip> poly_, polarity_;
	/// THE TRIG (backlog MZ, MV): a click fires one click; it lights on each fire, from the device's count. On the plot's
	/// top left under Impulses, over the strip of hits it adds to — hidden under Noise, where nothing reads it.
	std::unique_ptr<TrigPad> trigPad_;
	/// EVERY VALUE BOX, indexed by `Param` (null where the control has another widget).
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
