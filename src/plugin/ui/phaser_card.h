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
#include "fx/phaser/phaser_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/phaser_plot.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// THE PHASER/FLANGER'S CARD — the live response as its centrepiece (the reviewed mock, phaser plan Task 9a),
// top to bottom:
//
//   ┌ PHASER/FLANGER ──────────────── (on) ┐
//   │ [   PHASER   |   FLANGER   ]        │  Mode — a SegmentedControl, both choices on the face
//   │ ┌ response (live) ────────────────┐  │  the plot; a press or drag on it is Center / Delay
//   │ └─────────────────────────────────┘  │
//   │ [Center][Stages][Feedback][Mix]      │  the centre box is Center or Delay, whichever the mode reads;
//   │ [Normal|Up|Down] (Key) (TZ)          │  Stages only in Phaser, TZ only in Flanger — the row re-flows
//   │ (LFO 0.30 Hz━━━)(ENV━━━━━━━)          │  the two sources: a FaderPill each, the LFO's rate at its right end
//   │ [Position][Stereo][Drive][Damp]      │
//   └──────────────────────────────────────┘
//
// **WHERE THIS DEPARTS FROM THE TASK'S WORDING, AND WHY.** The centre box is two plain value boxes (Center, Delay)
// with the live one shown, as the disperser's card shows Freq or Offset — not a `ModeValueCell`. A mode cell carries
// its OWN selector for the mode, and the mock (which is the spec for the layout) already puts Mode on the face as a
// segmented control; a cell would draw the Mode a second time and take a knob column's height for a compact row.
// The descriptor's `modeCell` is honoured where it is for: the window seats nothing generic, and this card shows
// exactly the follower the Mode gates live.
//
// **TZ IS HIDDEN IN THE PHASER, NOT DISABLED** — a departure from the design's card section ("TZ disabled outside
// Flanger") and from the mock, which greys it. The task for this card asked for it shown only in the Flanger, the way
// Stages shows only in the Phaser, and the row re-flows (Sweep takes the room) rather than carrying a dead chip. If
// the mock's greyed chip is wanted instead, it is `throughZero_->setVisible(open)` and a disabled look in `applyGates`.
//
// **RATE IS NOT A BOX ON THE CARD.** The mock prints it in the LFO pill, and so does this (`FaderPill::
// setReadout`): "LFO" left and "0.30 Hz" right, or the division when synced. Its box, Sync, the follower's
// Attack/Release/Sens and Rotate are in the window (`PhaserExpanded`), which holds everything here too.
//
// GATES. The descriptor's gates (a control's one `gateControl`) are read here — a control whose gate is closed is
// DISABLED (drawn dim, the hand refused: `ValueBox::setEnabled`, `SegmentedControl::setEnabled`) or, where the mode
// makes it a different control altogether, hidden: Stages and TZ, and the centre box that is not the mode's. One gate
// the descriptor cannot express is added: Rate (in the window) is disabled while the LFO is off, as well as under a
// tempo division.
//
// **CENTER AND DELAY WHILE A NOTE IS TRACKED** — the published `NoteHz` nonzero, not the Key switch alone: the knob is
// the OFFSET from the note (Key Offset was retired), so it stays live and its box prints semitones
// (`phaserOffsetText`); before the first note it is the knob's own Hz or ms.

namespace feathers::ui {

class PhaserCard final : public FxCard {
public:
	PhaserCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	PhaserCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::phaser::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 8.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kPlotHeight = 104.0f;
	static constexpr float kPillHeight = 20.0f;
	static constexpr float kCellGap = 4.0f;
	static constexpr float kKeyChipWidth = 34.0f;
	static constexpr float kTzChipWidth = 30.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	[[nodiscard]] std::vector<fx::phaser::Param> addressedParamsForTest() const;
	[[nodiscard]] PhaserPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	[[nodiscard]] SegmentedControl* sweepForTest() const { return sweep_.get(); }
	[[nodiscard]] ValueBox* centerBoxForTest() const { return center_.get(); }
	[[nodiscard]] ValueBox* delayBoxForTest() const { return delay_.get(); }
	[[nodiscard]] ValueBox* stagesBoxForTest() const { return stages_.get(); }
	[[nodiscard]] ValueBox* boxForTest(fx::phaser::Param p) const;
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	[[nodiscard]] SwitchChip* throughZeroForTest() const { return throughZero_.get(); }
	[[nodiscard]] FaderPill* lfoPillForTest() const { return lfo_.get(); }
	[[nodiscard]] FaderPill* envPillForTest() const { return env_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::phaser::Param p) const;
	[[nodiscard]] fx::phaser::Mode modeNow() const;
	/// The descriptor's gate for `p`, against its gate control's stored value.
	[[nodiscard]] bool gateOpen(fx::phaser::Param p) const;
	/// A note is being tracked — the published `NoteHz` nonzero.
	[[nodiscard]] bool noteTracked() const;
	/// EVERY ENABLE THE GATES DECIDE, applied; whether any changed.
	bool applyGates();
	/// The LFO pill's readout from Rate or Sync; whether it changed.
	bool refreshLfoCaption();
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	/// THE MODE THE WIDGETS WERE LAST LAID OUT FOR — which centre box, and whether Stages or TZ is in its row.
	int laidOutMode_ = -1;
	/// Whether the centre boxes last printed the offset from a tracked note — a change redraws them.
	bool shownTracked_ = false;
	float captionRate_ = -1e9f;
	float captionSync_ = -1e9f;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<PhaserPlot> plot_;
	std::unique_ptr<ValueBox> center_;
	std::unique_ptr<ValueBox> delay_;
	std::unique_ptr<ValueBox> stages_;
	std::unique_ptr<ValueBox> feedback_;
	std::unique_ptr<ValueBox> mix_;
	std::unique_ptr<SegmentedControl> sweep_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::unique_ptr<SwitchChip> throughZero_;
	std::unique_ptr<FaderPill> lfo_;
	std::unique_ptr<FaderPill> env_;
	std::unique_ptr<ValueBox> position_;
	std::unique_ptr<ValueBox> stereo_;
	std::unique_ptr<ValueBox> drive_;
	std::unique_ptr<ValueBox> damping_;
};

} // namespace feathers::ui
