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
#include "fx/delay/delay_types.h"
#include "plugin/ui/control_face.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/delay_strip.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/device_preset_provider.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// THE DELAY'S WINDOW (the decided sketch) — the time plane first, the
// controls around it, the FEEDBACK FX lane on the right:
//
//   ┌───────────────────────────────────────────────────────────────┬─────────────┐
//   │ [Sync][Time] [Clean|Granular|Xfade|Tape|Multi-Tap|Reverse] [Mix] [Dub] │ FEEDBACK FX │
//   │ ┌ TIME PLANE: the taps picture, large ─────────────────────┐  │  (a rail;   │
//   │ │  L ▮    ▮     ▮      ▯                                   │  │   a phantom │
//   │ │ ───┼────┼─────┼──────┼──────                              │  │   until its │
//   │ │  R ▮      ▮      ▮                                       │  │   (+) makes │
//   │ └──────────────────────────────────────────────────────────┘  │   the lane) │
//   │ LOOP  [Feedback] [Tone] [Off|Stereo|Classic] [Spread]         │             │
//   │ GRAIN / TAPE / TAPS — the live mode's four, as on the card    │             │
//   └───────────────────────────────────────────────────────────────┴─────────────┘
//
// **STRICTLY A SUPERSET OF THE CARD** (CLAUDE.md): every control the card offers in a state is visible here in that
// state — each gated by its descriptor gate exactly as the card's grid is, so the guard
// (`test_gui_widgets_tooltips`, "a device's window offers every control its card offers") walks the six modes.
//
// Mode and Ping-Pong are `SegmentedControl`s (every choice on the face, the multivalue rule); Sync is a `ValueBox`
// whose press opens the division list; Dub is a `SwitchChip`; the zoned Pattern and Levels are `ZoneCoordCell`s, the
// card's own widget for them. The plot carries handles for the controls that act on it (`DelayPlane`, S3).
//
// **THE LANE IS LAZY** (rule 9): the rail is a PHANTOM — drawn as an empty chain's, its (+) the only thing that
// builds the lane (`fx::delay::ensureFeedbackLane`) — until the device holds one. Opened by default, because it is
// the window's headline; the card's `FB FX` chip comes here.

namespace feathers::ui {

/// **THE TIME PLANE (§2.2, S3): the card's strip at the window's size, and the controls that act on it ON it.**
///
/// What it draws beyond the card's picture: the DECAY LINE through the left repeats' tips — its slope is the loop gain
/// per pass as published (Feedback times the lane's measured pass gain), so a lane that boosts the loop visibly tilts
/// it up; the FLOOR the lane puts under Time, shaded at the left; and the limiter's gain while it is limiting.
///
/// THE HANDLES, each a control drawn where it acts (a `Gesture` road in `ControlFace` terms — the bands keep each
/// control's `Face`, so the card's faces are still faces here):
///
/// | handle | where | drag | control |
/// |---|---|---|---|
/// | Time ◆ | the first left repeat | sideways | `Time` (the axis held still while held) |
/// | Spread ◆ | the first right repeat | sideways | `Spread` |
/// | Feedback ● | the second left repeat's tip | up/down, on the dB scale | `Feedback` |
/// | Tone ◐ | the tail's end | up thinner, down darker | `Tone` |
/// | Taps ■ | Multi-Tap: the earliest tap | left adds, right removes | `Taps` |
/// | Pattern×Levels ○ | Multi-Tap: inside the first pass | x = Pattern, y = Levels, in their zones | both |
///
/// A handle is drawn and taken only while its control's gate is open (the honesty rule, as the bands).
class DelayPlane final : public visage::Frame, public TooltipSource {
public:
	enum class Handle : uint8_t { None, Time, Spread, Feedback, Tone, Taps, Cluster };
	using ControlOf = std::function<DeviceControl&(fx::delay::Param)>;

	DelayPlane(backplane::DeviceHost& host, DeviceInstanceId id, ControlOf control);
	void draw(visage::Canvas& canvas) override;
	/// Whether the picture moved since the last call (and a redraw asked for if so).
	bool update();
	/// THE GEOMETRY THE PICTURE IS DRAWN AT — the whole rectangle, and the span (held still while a handle is held).
	[[nodiscard]] DelayStrip strip() const;
	[[nodiscard]] std::vector<fx::delay::StripTick> ticksForTest() const;

	void offeredControls(std::vector<Offered>& out) const override;
	/// OVER A HANDLE, ITS CONTROL'S OWN DECLARATION; elsewhere nothing (a picture says nothing at a point).
	const char* tooltipAt(visage::Point local) const override;

	/// WHERE A HANDLE IS NOW, local; false while it is not shown (its gate shut, or nothing published).
	[[nodiscard]] bool handleAt(Handle h, visage::Point* out) const;
	/// WHICH HANDLE A PRESS AT `p` TAKES — the nearest within `kGrab`, or None.
	[[nodiscard]] Handle handleNear(visage::Point p) const;
	/// A WHOLE DRAG, press to release, through the same three steps the mouse takes.
	void dragForTest(visage::Point from, visage::Point to);
	/// THE DECAY LINE's slope, dB per pass, as drawn — 20·log10 of the published loop gain per pass.
	[[nodiscard]] double decaySlopeDbForTest() const;
	/// THE FLOOR'S SHADE, local x of its right edge; 0 without a lane.
	[[nodiscard]] float floorRightForTest() const;

	static constexpr float kGrab = 9.0f;
	/// PIXELS PER UNIT on the two relative drags.
	static constexpr float kTonePixels = 120.0f;
	static constexpr float kTapsPixels = 24.0f;

private:
	struct Drag {
		Handle handle = Handle::None;
		visage::Point start {};
		double startValue = 0.0;
		DelayStrip strip {}; ///< the axis as it stood at the press — held still under the hand
		float firstGain = 1.0f;
		float lanePass = 1.0f;
		double unit = 0.0; ///< the hand's left tap at the press, seconds
	};
	[[nodiscard]] bool live(fx::delay::Param p) const;
	[[nodiscard]] double fieldOf(fx::delay::Param p) const;
	/// THE LEFT TAP THE HAND ASKS FOR, seconds (no Spread), and the first repeat's heard gain.
	[[nodiscard]] double handLeftTap(const fx::delay::TapPicture& p) const;
	void press(visage::Point at);
	void move(visage::Point at);
	void release();

	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	ControlOf control_;
	Drag drag_ {};
	bool drawnHad_ = false;
	double drawnSpan_ = -1.0;
	std::vector<fx::delay::StripTick> drawnTicks_;
	std::vector<fx::delay::StripGrain> drawnGrains_;
	std::array<float, 4> drawnExtras_ {};
};

/// **CLEAR, A MOMENTARY** (delay-love S4): pressed, its control goes high; released, low — so each press is one rising
/// edge, one clear, and one undo step. Its control's own face (the card does not draw it: a grid cell would latch it).
class ClearButton final : public visage::Frame, public TooltipSource {
public:
	explicit ClearButton(DeviceControl& control);
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	[[nodiscard]] bool heldForTest() const noexcept { return held_; }
	void pressForTest();
	void releaseForTest();

private:
	DeviceControl& control_;
	bool held_ = false;
};

class DelayExpanded final : public ExpandedView {
public:
	DelayExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	DelayExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;
	~DelayExpanded() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override { return fx::delay::kTraits.name; }
	void modulationBound(ModState* mod, EditHistory* history) override;
	bool updateContent() override;
	void setPresentedOn(SurfaceLayers* host) override;
	/// **THE PRESETS (S5): FEATHERS' OWN BROWSER, THIS DEVICE'S BANK**, in the title row, over `Devices/DELAY` — the one
	/// SKLITTER's, Featherverb's and Resonate's panels hold. A preset carries the FEEDBACK FX lane's body; a load keeps only
	/// the bypass. "Init" is the declared defaults and an empty lane.
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.get(); }
	[[nodiscard]] PresetBrowser& presetBrowserForTest() const { return *presets_; }

	static constexpr float kPad = 14.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kCellGap = 6.0f;
	static constexpr float kBandLabelWidth = 52.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kMinPlotHeight = 150.0f;
	static constexpr float kMinFaceWidth = 560.0f;
	static constexpr float kDubWidth = 44.0f;

	/// THE RAIL'S WORDS.
	static constexpr const char* kLaneLabel = "FEEDBACK FX";
	static constexpr const char* kLaneLine = "in the loop: every repeat passes again";
	static constexpr const char* kLanePasses = "the repeats return as they are";

	[[nodiscard]] FxLaneRail& laneRailForTest() const { return *lane_; }
	[[nodiscard]] DelayPlane& planeForTest() const { return *plane_; }
	[[nodiscard]] SegmentedControl& modeForTest() const { return *mode_; }
	[[nodiscard]] SegmentedControl& pingPongForTest() const { return *pingPong_; }
	[[nodiscard]] ClearButton& clearForTest() const { return *clear_; }
	/// THE WIDGET OVER `p`, whatever kind it is; null for the enable (the overlay's header) and nothing else.
	[[nodiscard]] visage::Frame* widgetForTest(fx::delay::Param p) const { return widgets_[static_cast<size_t>(p)]; }
	/// **READ OFF THE WIDGET TREE** (`ControlFace`), hidden ones included — the controls this window offers.
	[[nodiscard]] std::vector<fx::delay::Param> addressedParamsForTest() const;

protected:
	void onInnerNestChanged() override;

private:
	[[nodiscard]] DeviceControl& control(fx::delay::Param p) const;
	[[nodiscard]] bool gateOpen(fx::delay::Param p) const;
	[[nodiscard]] int modeNow() const;
	void layOut();
	bool applyGates();
	/// THE RAIL OVER THE LANE, OR A PHANTOM WHILE THERE IS NONE — re-asked every tick (trap T9).
	bool followLane(void* held);

	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	Notifier* notifier_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	/// HELD BEFORE THE RAIL IS BUILT OVER IT, AND DECLARED BEFORE IT so it outlives its teardown (backlog JX).
	backplane::DeviceHost::SurfacePin chainPin_;
	std::unique_ptr<DevicePresetProvider> presetProvider_;
	std::unique_ptr<PresetBrowser> presets_; // after its provider, so destroyed before it

	std::unique_ptr<DelayPlane> plane_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<SegmentedControl> pingPong_;
	std::unique_ptr<SwitchChip> dub_;
	std::unique_ptr<SwitchChip> freeze_;
	std::unique_ptr<ClearButton> clear_;
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	std::vector<std::unique_ptr<ZoneCoordCell>> zones_;
	/// EVERY PARAM'S WIDGET, by index — what the layout and the gates walk.
	std::array<visage::Frame*, static_cast<size_t>(fx::delay::Param::kCount)> widgets_ {};
	std::unique_ptr<FxLaneRail> lane_;
	bool phantomShown_ = false;
	bool pendingAddMenu_ = false;
	int laidOutMode_ = -1;
};

} // namespace feathers::ui
