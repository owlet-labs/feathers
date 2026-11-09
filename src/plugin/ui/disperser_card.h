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
#include "fx/disperser/disperser_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/disperser_plot.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// THE DISPERSER'S CARD — the group-delay plot as its centrepiece, the controls placed by what they change.
// The sketch was approved (2026-09-25) with a "knob list" excluded, and on 2026-09-26 it took the house's own
// controls: Topo and Twist as mode cells, and the derived quantities as the Shaper's pills.
//
//   ┌ DISPERSER ─────────────────────────── (on) ┐
//   │ ┌ group delay ───────────────── 10 ms ┐   │  the plot; a press or drag on it is Freq / Offset
//   │ └─────────────────────────────────────┘   │  stage ticks along its foot = the Spread
//   │ (Key) [ 93.8 Hz ]  [Stages 8]  [γ 0]       │  chip, the centre's box, the count, gamma
//   │     [ 50% ]            [ 12% ]             │  the two MODE CELLS: the value the mode reads,
//   │   ‹ Cascade ›         ‹ Chirp ›            │  and the mode between its arrows (a dropdown)
//   │ [Width][Punch][Chirp][Curve][QTilt]        │  the NINE PILLS, the Shaper's extras: an amount
//   │ [Q][Detune][Emph][2f]                      │  and its power each
//   └─────────────────────────────────────────────┘
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Freq / Offset — the plot's axis, and a compact value box beside Key Track (only the live one shows).
//   - Key Track — a switch chip. Stages, Gamma — compact boxes, each sized by what it prints.
//   - Topo, Twist — a `ModeValueCell` each: the mode's selector, and a box per follower (the position, and
//     Twist's phase), only the live one shown.
//   - the pills — a `FaderPill` each, over its amount and its power.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.

namespace feathers::ui {

class DisperserCard final : public FxCard {
public:
	DisperserCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	              Notifier* notifier = nullptr);
	DisperserCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::disperser::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 8.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kPlotHeight = 122.0f;
	static constexpr float kControlRowHeight = 16.0f;
	/// The mode cells' face — a knob column's footprint, as the Shaper seats its coordinate.
	static constexpr float kModeFace = 22.0f;
	/// The least room between a compact box's caption and its number — the widget case measures it.
	static constexpr float kCaptionGap = 4.0f;
	/// THE PILLS, the Shaper's `kExtraHeight` / `kExtraGap` / `kExtraWidth`.
	static constexpr float kPillHeight = 20.0f;
	static constexpr float kPillGap = 4.0f;
	static constexpr float kPillWidth = 46.0f;
	static constexpr size_t kPillsPerRow = 5;

	[[nodiscard]] static float modeRowHeight();
	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds pillBounds(size_t i) const;

	[[nodiscard]] std::vector<fx::disperser::Param> addressedParamsForTest() const;
	[[nodiscard]] DisperserPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] ValueBox* freqBoxForTest() const { return freq_.get(); }
	[[nodiscard]] ValueBox* offsetBoxForTest() const { return offset_.get(); }
	[[nodiscard]] ValueBox* stagesBoxForTest() const { return stages_.get(); }
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	[[nodiscard]] ModeValueCell* topoForTest() const { return topo_.get(); }
	[[nodiscard]] ModeValueCell* twistForTest() const { return twist_.get(); }
	[[nodiscard]] GammaBox* gammaForTest() const { return gamma_.get(); }
	[[nodiscard]] FaderPill* pillForTest(size_t i) const { return i < pills_.size() ? pills_[i].get() : nullptr; }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::disperser::Param p) const;
	[[nodiscard]] bool tracking() const;
	/// Whether a follower of a mode cell is live now — its gate against the mode's stored value.
	[[nodiscard]] bool relevant(size_t param) const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE KEY TRACK STATE THE WIDGETS WERE LAST LAID OUT FOR — Freq's box or Offset's.
	int laidOutTracking_ = -1;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<DisperserPlot> plot_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::unique_ptr<ValueBox> freq_;
	std::unique_ptr<ValueBox> offset_;
	std::unique_ptr<ValueBox> stages_;
	std::unique_ptr<GammaBox> gamma_;
	std::unique_ptr<ModeValueCell> topo_;
	std::unique_ptr<ModeValueCell> twist_;
	std::vector<std::unique_ptr<FaderPill>> pills_;
};

} // namespace feathers::ui
