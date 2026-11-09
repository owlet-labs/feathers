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
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/phaser_pad.h"
#include "plugin/ui/phaser_plot.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE PHASER/FLANGER'S WINDOW — the mock's expanded view, and STRICTLY A SUPERSET OF THE CARD (by design:
// the first version of this window had only the plot, the pad and the six controls the card leaves out, so opening
// it cost every control the card had):
//
//   ┌──────────────────────────────────────┬───────────────────┐
//   │ [      PHASER      |     FLANGER    ]│ ┌ pad ─── [Rot] ┐ │  Mode, both choices on the face (the card's)
//   │ the response, large: L bright, R     │ │       ●       │ │  the explore pad, Rotate in its corner
//   │ faint, the sweep band, the Position  │ │               │ │  (the shaper's arrangement)
//   │ tick, the centre handle              │ └───────────────┘ │
//   ├──────────────────────────────────────┤ explore — …       │
//   │ [Center][Stages][Feedback][Mix]      │                   │  the card's rows, in the card's order, wider:
//   │ [Normal|Up|Down]    (Key)     (TZ)   │                   │  each source's pill beside the controls that
//   │ (LFO 0.30 Hz━━━━━━━━━) [Rate] [Sync] │                   │  shape it — the LFO's rate and sync, the
//   │ (ENV━━━━) [Attack] [Release] [Sens]  │                   │  follower's shape — which the card has no
//   │ [Position][Stereo][Drive][Damp]      │                   │  room for
//   └──────────────────────────────────────┴───────────────────┘
//
// Sync is a value box whose press opens the division list (`ValueBox::showChoiceMenu`, from the descriptor: 28 names,
// every one visible before it applies); Mode and Sweep are segmented controls, Key and TZ switch chips — the card's
// widgets for the card's controls, so a control reads the same in both places.
//
// GATES, AS THE CARD'S (`PhaserCard`'s header has the reasons): the centre box is Center or Delay by the Mode, Stages
// only in the Phaser, TZ only in the Flanger (hidden, and the row re-flows); Center/Delay print the offset from a
// tracked note in semitones; Rate disabled under a division AND while the LFO is off; Sync while the LFO is off; the
// follower's three while it is off; Stereo by its descriptor gate; Sweep is never greyed (it has no gate).
//
// **THE EXPLORE PILLS (round 2, Task 11) — WINDOW ONLY, UNDER THE PAD.** Ten FaderPills, two columns of five, one per
// explore group (`phase_space::Group`): Spread, Stagger, Shape, Colour, Bias, Skew, Rate skew, Fb tilt, Pairing, Curve —
// each a knob in front of that group's fields — the house `fieldWithOverride`, the shaper's extras' law AND look: no
// readout, the caption alone, the rail's 50% tick the neutral (the terrain's own value), the empty end the field forced
// to 0, the full end forced to 1 (since 2026-10-04; round 2 had a 0..2 multiplier) — and its power, off = no effect.
// The card has none (the superset rule allows a window-only control). GATES, each decided against the engine
// (`applyGates`):
//   - Pairing: its descriptor gate, Mode = Phaser (the flanger has no stages to pair);
//   - Stagger, Shape: their descriptor gate, LFO On (each reaches the sweep only times the LFO's amount);
//   - Rate skew: live with the LFO on OR under the barberpole (ψ_R's rate moves R's windows with the LFO off, heard
//     once the voices differ — the Fb tilt's per-voice loops, the spread's per-tap delays) — no descriptor gate says that;
//   - Colour: grey while Damping is open (`dampingOpen`: the colour moves a RUNNING lowpass, and an open loop stays
//     open) — a gate the descriptor cannot say, Damping being continuous;
//   - Bias: grey at Drive 0 (the bias ramps in with Drive: `biasTanhOf`) — likewise;
//   - Spread, Skew, Fb tilt, Curve: always live (both engines, both sweeps; the curve at any sweep but exactly 0).
// Grey means `FaderPill::setActive(false)`: "not in the sound", the hand still takes it, as every gated pill here.
//
// **IT DOES ZERO WORK AT REST** — the plot's own rule, and the pad's terrain is cached on its key.

namespace feathers::ui {

class PhaserExpanded final : public ExpandedView {
public:
	PhaserExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	               Notifier* notifier = nullptr);
	PhaserExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override;
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;

	static constexpr float kPad = 14.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kSideWidth = 240.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kCellGap = 6.0f;
	static constexpr float kRotateWidth = 110.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kPillHeight = 20.0f;
	static constexpr float kKeyChipWidth = 40.0f;
	static constexpr float kTzChipWidth = 34.0f;
	/// THE PLOT'S LEAST HEIGHT — the control rows take what they need and the plot takes the rest.
	static constexpr float kMinPlotHeight = 120.0f;

	[[nodiscard]] PhaserPlot* plotForTest() const { return plot_.get(); }
	[[nodiscard]] PhaserPad* padForTest() const { return pad_.get(); }
	[[nodiscard]] GammaBox* rotateForTest() const { return rotate_.get(); }
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	[[nodiscard]] SegmentedControl* sweepForTest() const { return sweep_.get(); }
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	[[nodiscard]] SwitchChip* throughZeroForTest() const { return throughZero_.get(); }
	[[nodiscard]] FaderPill* lfoPillForTest() const { return lfo_.get(); }
	[[nodiscard]] FaderPill* envPillForTest() const { return env_.get(); }
	/// THE EXPLORE GROUP'S PILL, and whether the window shows it live (`applyGates`).
	[[nodiscard]] FaderPill* explorePillForTest(fx::phaser::phase_space::Group g) const {
		return xPills_[static_cast<size_t>(g)].get();
	}
	[[nodiscard]] bool explorePillActiveForTest(fx::phaser::phase_space::Group g) const {
		return xActive_[static_cast<size_t>(g)];
	}
	/// THE PILLS' BLOCK: two columns, under the pad's caption.
	static constexpr float kXPillGap = 6.0f;
	static constexpr size_t kXPillRows = 5;
	[[nodiscard]] ValueBox* boxForTest(fx::phaser::Param p) const;
	[[nodiscard]] std::vector<fx::phaser::Param> addressedParamsForTest() const;

private:
	[[nodiscard]] DeviceControl& control(fx::phaser::Param p) const;
	[[nodiscard]] bool gateOpen(fx::phaser::Param p) const;
	[[nodiscard]] fx::phaser::Mode modeNow() const;
	[[nodiscard]] visage::Bounds padBounds() const;
	/// The height the control rows under the plot take, Mode's row above it included.
	[[nodiscard]] static float rowsHeight();
	bool applyGates();
	/// The LFO pill's readout from Rate or Sync — the card's; whether it changed.
	bool refreshLfoCaption();
	void layOut();

	backplane::DeviceHost& dhost_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	int laidOutMode_ = -1;
	/// Whether the centre boxes last printed the offset from a tracked note — a change redraws them.
	bool shownTracked_ = false;
	float captionRate_ = -1e9f;
	float captionSync_ = -1e9f;

	/// THE CONTROLS, BEFORE EVERY WIDGET THAT READS THEM — so they are destroyed after (CLAUDE.md's UAF history).
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<PhaserPlot> plot_;
	std::unique_ptr<PhaserPad> pad_;
	/// The pad's drag, held as one undo step (backlog JB), into the history `modulationBound` hands over.
	DeviceFieldsGesture padGesture_;
	EditHistory* history_ = nullptr;
	std::unique_ptr<GammaBox> rotate_;
	std::unique_ptr<SegmentedControl> sweep_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::unique_ptr<SwitchChip> throughZero_;
	std::unique_ptr<FaderPill> lfo_;
	std::unique_ptr<FaderPill> env_;
	std::array<std::unique_ptr<FaderPill>, fx::phaser::phase_space::kGroupCount> xPills_;
	std::array<bool, fx::phaser::phase_space::kGroupCount> xActive_ {};
	/// EVERY VALUE BOX, by the control it draws — the centre pair, Stages, Feedback, Mix; Rate, Sync; the
	/// follower's three; Position, Stereo, Drive, Damping.
	std::vector<std::pair<fx::phaser::Param, std::unique_ptr<ValueBox>>> boxes_;
};

} // namespace feathers::ui
