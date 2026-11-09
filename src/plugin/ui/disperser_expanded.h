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
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/notify_tray.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE DISPERSER'S WINDOW — laid out for its room, not the card scaled up (the approved sketch):
//
//   ┌────────────────────────────────────────────┬───────────────────┐
//   │ group delay, large: axis labels, per-stage  │ TOPO        [0.00] │
//   │ Q as tick height, the centre's hertz, the    │  all 8 names, lit  │
//   │ handle (its height the Q offset)             │ TWIST       [0.00] │
//   ├────────────────────────────────────────────┤ γ [0]  Stages [8] │
//   │ FEEDBACK: Punch ▮▯  Chirp ▮▮▯  2f ▮  loop ┃ │ Key ●  Freq/Offset│
//   │                                             │ Q Offset          │
//   └────────────────────────────────────────────┴───────────────────┘
//
// **THE FEEDBACK STRIP READS WHAT THE DEVICE PUBLISHES** (`fx::disperser::Reading`) — the per-block write
// gains after the cap, and the loop's bound against the cap's ceiling — and repaints only when one moves:
// at rest they do not, so the window repaints zero times.

namespace feathers::ui {

class DisperserExpanded final : public ExpandedView {
public:
	DisperserExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                  Notifier* notifier = nullptr);
	DisperserExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;

	static constexpr float kPad = 14.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kSideWidth = 220.0f;
	static constexpr float kPillHeight = 26.0f;
	static constexpr float kPillGap = 10.0f;
	static constexpr size_t kPillsPerRow = 3;
	static constexpr float kCellFace = 44.0f;
	static constexpr float kStripHeight = 46.0f;
	static constexpr float kControlRow = 18.0f;

	/// THE STRIP'S NUMBERS, as drawn — so a case asserts the picture's values.
	struct Strip {
		bool live = false; ///< something is publishing
		float punch = 0.0f;
		float chirp = 0.0f;
		float chirp2F = 0.0f;
		float loop = 0.0f;
		float echoMs = 0.0f;
	};
	[[nodiscard]] Strip stripForTest() const { return stripNow(); }
	[[nodiscard]] DisperserPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] ModeValueCell* topoForTest() const { return topo_.get(); }
	[[nodiscard]] ModeValueCell* twistForTest() const { return twist_.get(); }
	[[nodiscard]] FaderPill* pillForTest(size_t i) const { return i < pills_.size() ? pills_[i].get() : nullptr; }
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	[[nodiscard]] ValueBox* qOffsetBoxForTest() const { return qOffset_.get(); }
	/// EVERY CONTROL THE WINDOW SEATS, read off its widget tree (the card's seam, the same way) — the reachability
	/// guard takes the union with the card's, as it does for EQ, because the Q offset is seated only here.
	[[nodiscard]] std::vector<fx::disperser::Param> addressedParamsForTest() const;
	[[nodiscard]] int stripRepaintsForTest() const noexcept { return stripRepaints_; }

private:
	[[nodiscard]] DeviceControl& control(fx::disperser::Param p) const;
	[[nodiscard]] bool tracking() const;
	[[nodiscard]] Strip stripNow() const;
	[[nodiscard]] visage::Bounds stripBounds() const;
	void layOut();
	/// The mode cells and the pills, top of the side column — what the control rows sit under.
	[[nodiscard]] float sideTopHeight() const;

	backplane::DeviceHost& dhost_;
	DeviceInstanceId device_ = kNoInstance;
	int laidOutTracking_ = -1;
	Strip drawnStrip_ {};
	int stripRepaints_ = 0;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<DisperserPlot> plot_;
	std::unique_ptr<ModeValueCell> topo_;
	std::unique_ptr<ModeValueCell> twist_;
	std::vector<std::unique_ptr<FaderPill>> pills_;
	Notifier* notifier_ = nullptr;
	std::unique_ptr<GammaBox> gamma_;
	std::unique_ptr<ValueBox> stages_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::unique_ptr<ValueBox> freq_;
	std::unique_ptr<ValueBox> offset_;
	std::unique_ptr<ValueBox> qOffset_;
};

} // namespace feathers::ui
