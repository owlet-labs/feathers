/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "fx/owltt/owltt_types.h"
#include "plugin/ui/clip_led.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/control_face.h"
#include "plugin/ui/knob.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE BAND GRAPH — OWLTT'S PICTURE AND ITS PRINCIPAL CONTROL SURFACE, on whichever surface holds it.
//
// EXTRACTED FROM `OwlttCard` BECAUSE TWO SURFACES NEED IT AT TWO SIZES: the pop-out carries a larger
// version of the graph than the card, whose own copy had already grown.
// The alternative was a second copy of the geometry, the hit test, the drag machine and the drawing
// in `OwlttExpanded` — in a file that produced TEN two-places-must-agree bugs in a single week,
// including three of the exact form "drawn here, grabbable there". A copy was never a candidate.
//
// SO THE SIZE IS THE ONLY DIFFERENCE BETWEEN THE TWO. The card holds one 160px tall; the panel holds
// one about twice that. Everything else — where a bar sits for a value, which pixels a strip owns,
// where a ring goes, what a press means — is this class, once.
//
// IT DRAWS AND HIT-TESTS IN ITS OWN COORDINATES, which is what made the extraction clean: the
// geometry was already parameterised on a plot rectangle (`thresholdToY(v, plot)`,
// `stripSpanOf(x0, x1)`), so it needed only to be told that the rectangle is `localBounds()`. The
// card used to pass its own `crossoverBounds()` in card space and translate presses by hand.
//
// THE METERS ARE PULLED, NOT PUSHED. `host_.readingAt(device, i)` is a read, so a second graph needs no
// plumbing and cannot fight the first for a connector — which is the one thing that would have made
// two live graphs expensive.

namespace feathers::ui {

class OwlttGraph final : public visage::Frame, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	OwlttGraph(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr);
	OwlttGraph(backplane::DeviceHost&, size_t, Notifier* = nullptr) = delete; // a lane or a slot is not an id

	void draw(visage::Canvas& canvas) override;

	void setModulation(ModState* mod, EditHistory* history);

	/// ADVANCE THE METERS, AND SAY WHETHER THE SCREEN IS OUT OF DATE — the contract
	/// `PerformStage::update`, `ExploreStage::update` and `NotifyTray::update` already share.
	///
	/// IT LIVES HERE AND NOT ON THE CARD BECAUSE THIS IS THE FRAME THAT DRAWS THE METERS. A parent
	/// told to repaint is not a child told to repaint, and that exact mistake is what
	/// `FrameWatchers::redrawTree` was written to paper over: the plot moved out of `OwlttCard::draw`
	/// into this frame, the lane subscribes the CARD, and the meters died. Putting the update on the
	/// drawing frame is the fix rather than the workaround, and it means both hosts — the card and
	/// the expanded panel, which each own one of these — are served by one implementation.
	///
	/// CONTENT-GATED. A meter that has not moved asks for nothing, so a silent plugin costs nothing;
	/// that is the property no rate throttle can offer. The whole reading is compared, CLIPPING
	/// INCLUDED — `clipping_` is a HELD state with a decay counter on the audio side, so a comparison
	/// that only looked at the three level arrays could sit on a stale clip light for as long as the
	/// levels happened not to move.
	bool update();

	/// OWLTT'S METERS, READ OUT OF THE HOST'S ANONYMOUS READINGS.
	///
	/// **THIS IS THE ONE PLACE THAT KNOWS THE LAYOUT, AND ITS OPPOSITE NUMBER IS
	/// `BridgeDeviceHost::readingAt`.** The seam carries floats it does not understand — see
	/// `DeviceHost::readingAt` for why naming `OwlttMeterReading` there would pin one device into a
	/// shared vocabulary — so the mapping lives at the two ends of the wire and nowhere in between.
	///
	/// TEN READINGS: 0-2 gain reduction, 3-5 input level, 6-8 output level, 9 the clip flag as 0 or 1.
	/// A host with nothing to say answers zero readings and this returns a default, which draws as a
	/// meter at rest — correct for "no data" and for "no device", which a picture cannot distinguish
	/// and should not try to.
	struct Meters {
		std::array<float, 3> grDB { 0.0f, 0.0f, 0.0f };
		std::array<float, 3> inputLevelDB { 0.0f, 0.0f, 0.0f };
		std::array<float, 3> outputLevelDB { 0.0f, 0.0f, 0.0f };
		bool clipping = false;
		/// BACKLOG EL: a light per band while its clipper is working (readings 10..12), and the mean
		/// |gain change| across the bands in dB (reading 13). Absent from a publisher that sends the
		/// old ten, in which case they read off and zero.
		std::array<bool, 3> bandSaturating { false, false, false };
		float combinedGrDb = 0.0f;
		/// BACKLOG LQ: each band's SECOND channel's gain reduction (readings 14..16 — R, or S in M/S); `grDB`
		/// is the first channel's. A publisher that sends fewer has one linked gain, so this copies `grDB`.
		std::array<float, 3> grSecond { 0.0f, 0.0f, 0.0f };
		/// BACKLOG ML: THE CLIP SQUARE'S THREE STATES — `clip_led.h` over the device's courtesy output peak (backlog MK,
		/// `DeviceHost::outputPeakAt`) and OWLTT's own held `clipping`, which alone still makes it a clip. Normal when the
		/// host meters nothing, so the square is then exactly the old two-state one.
		ClipLedState clipState = ClipLedState::Normal;
		friend bool operator==(const Meters&, const Meters&) = default;
	};
	Meters meters() const;

	/// THE FOUR HANDLE RINGS A BAND DRAWS, by the thing each one moves.
	enum class Handle { UpperBar, LowerBar, RatioStrip, LevelStrip };

	/// WHERE A RING'S CENTRE IS — asked by the drawing AND by the cases, so a ring cannot be drawn
	/// anywhere a press at its centre does not reach. A drawn position is not observable headlessly
	/// and this graph has shipped three bugs of the form "drawn here, grabbable there".
	visage::Point ringCentre(size_t band, Handle handle) const;

	/// **WHAT A HANDLE LOOKS LIKE UNDER THE ARMED SOURCE** (the band level bars showed no armed
	/// glow). The strips and bars took a modulation drag when a source was armed, and
	/// the picture never said so: the Ratio strip wore the modulation colour ALWAYS and the Level strip
	/// never, so arming changed nothing a hand could see, and a route once made was invisible on the
	/// strip. A live destination with no signal is the honesty rule failing in the quiet direction.
	///
	/// One answer for the drawing and the cases: `lit` is `ModDisplay::routable` — armed, allowed to
	/// reach this control — and it is the ONLY thing that puts the modulation colour on a handle;
	/// `amount` is the armed source's route, signed, in the control's own range, and zero when nothing
	/// is armed (the knob hides an unarmed route the same way — the mark carries the running value).
	struct HandleLook {
		bool lit = false;
		float amount = 0.0f;
		bool bipolar = false;
	};
	HandleLook lookOf(size_t band, Handle handle) const;

	/// **THE REACH THE ROUTE DRAWS** — the span of values the armed source can push this handle across,
	/// as a rectangle in this frame's coordinates: the strip's width or the band's, from the value to
	/// `value + amount` (both ways for a bipolar source), clamped to the bar area. Height 0 when there
	/// is nothing to draw. Asked by `draw` and by the cases, so the reach cannot be painted anywhere
	/// the value does not put it — the rule `ringCentre` already lives by.
	visage::Bounds reachOf(size_t band, Handle handle) const;

	/// The x where a band ends, from its crossover frequency on the log axis. NOT thirds: the bands
	/// move with the crossovers, and a case that assumed thirds tested the wrong band.
	/// KEY TRACK's note (the device's reading 17), 0 while it is not placing the crossovers; and the corner band `band`'s
	/// upper edge actually runs at, Hz — what the lines and labels draw.
	[[nodiscard]] double noteHz() const;
	[[nodiscard]] double cornerHz(size_t band) const;
	float bandEdge(size_t band) const;

	/// The plot — this frame's own rectangle, in its own coordinates.
	visage::Bounds plot() const;

	/// THE VALUES THE DEVICE IS USING for a band — the per-band controls with the globals folded in,
	/// through `owltt::effectiveBandOf`. Everything this class draws, hit-tests and drags is in these
	/// terms, because a bar drawn at a stored number is a bar drawn at a number the compressor does
	/// not use.
	fx::owltt::EffectiveBand effective(size_t band) const;

	/// How far the relevant global displaces a handle, so a drag can subtract it back out.
	float shiftFor(Handle handle) const;

	/// WHERE A BAND'S TWO BARS SIT ON SCREEN — the one answer, in `barArea()` coordinates.
	///
	/// THIS EXISTS BECAUSE THREE PLACES COMPUTED IT AND ONE OF THEM WAS MISSED. `ringCentre` and
	/// `hitTestThreshold` were converted to effective values; `draw` was not, so the HANDLES followed
	/// the global threshold and the LINES they sit on did not: the handles moved and left their
	/// little blue lines behind.
	///
	/// Correcting the third copy would have left three copies. The bar's height is one fact and it is
	/// answered once.
	/// The handle ring's radius. Public because a case comparing a ring to the line it sits on has to
	/// allow exactly the clamp `ringYInside` applies at the ends of a sweep and no more — otherwise it
	/// fails for the clamp rather than for the bug it is aimed at.
	static constexpr float kRingRadius = 4.0f;
	/// THE GAP BETWEEN A BAND'S TWO GAIN-REDUCTION HALVES (backlog LQ: one per channel, L | R or M | S).
	static constexpr float kMeterSplitGap = 2.0f;

	struct BarYs {
		float upper;
		float lower;
	};
	BarYs barYs(size_t band) const;

	/// OFFER THE CROSSOVER HANDLES. Off by default, and the card leaves it off deliberately.
	///
	/// They belong on the panel graph specifically, and the card is the reason not to generalise: at 160px its two bar hit-bands already claim 20% of the plot, and two
	/// more grab targets would re-crowd exactly what making the graph taller just fixed. The panel's
	/// 320px has room for a reserved lane; the card's does not.
	void setCrossoversDraggable(bool on);
	bool crossoversDraggable() const noexcept { return rail_; }

	/// THE RESERVED LANE AT THE TOP, which the crossover handles own outright. Empty when they are
	/// off.
	///
	/// A LANE RATHER THAN A PRIORITY RULE, and that is the whole design. A crossover line runs the
	/// full height of the plot and so crosses every bar, every strip and the GR meter — ordering it
	/// against them would make one of them unreachable somewhere, which is this file's oldest lesson
	/// (`harm_scope.cpp:400`) and one it has now paid for three times. A lane nothing else may enter
	/// has no ordering question to get wrong.
	///
	/// HARM ALREADY DID THIS AND NAMED THE TRAP IN IT: its notch rail sits at the top of its plot,
	/// and `harm_scope.cpp:324` records that a grip pushed INTO the rail became unreachable. So the
	/// bars are confined below the lane rather than merely losing to it.
	static constexpr float kRailHeight = 16.0f;
	visage::Bounds rail() const;

	/// WHERE THE BARS, STRIPS AND METERS LIVE — the plot minus the lane. Everything that was written
	/// against `plot()` asks this instead, so a bar cannot be drawn into a lane it cannot be grabbed
	/// in.
	visage::Bounds barArea() const;


	/// A FREQUENCY AS A PLAYER READS IT: "200 Hz", "2.0 kHz". The crossover frequency was shown
	/// nowhere: the knobs printed a bare `%.2f` through `device::Unit::Number` and the graph drew
	/// the lines with no label at all, so the one number a multiband compressor is DEFINED by was the
	/// one number nowhere on screen.
	static std::string frequencyText(float hz);

	/// **THE BOTTOM ROW'S TEXT, PLACED ONCE (backlog NB follow-up)** — the two crossover labels and each band's
	/// channel letters (L R, or M S), as the rectangles their glyphs occupy. A label sat on the same row as the
	/// letters and covered the mid band's "L" ("200 Hz" over it), and the narrower card graph beside the Drive bar
	/// ran "2.0 kHz" into the high band's "R". A label that would touch a letter or the other label moves up a
	/// row, as many rows as it takes. `draw` paints from this and the cases assert on it, so the two agree.
	struct BottomText {
		std::array<visage::Bounds, 2> frequency {};
		std::array<std::string, 2> frequencyText {};
		/// The letters drawn, in band order, two per band — none for a band too narrow to letter.
		std::vector<visage::Bounds> letters;
		std::vector<size_t> letterBand;
	};
	[[nodiscard]] BottomText bottomText() const;

	/// A CROSSOVER HANDLE'S CENTRE, in the lane. Asked by the drawing and by the cases.
	visage::Point crossoverHandle(size_t which) const;

	/// What a press at a point has taken hold of. Public because the cases ask it: a drawn handle is
	/// not observable headlessly, so what a case CAN check is which control a press resolves to.
	enum class ThresholdHit {
		kNone,
		kLowCrossover,
		kHighCrossover,
		kLowUpper,
		kLowLower,
		kMidUpper,
		kMidLower,
		kHighUpper,
		kHighLower,
		kLowRatio,
		kMidRatio,
		kHighRatio,
		kLowLevel,
		kMidLevel,
		kHighLevel,
	};
	ThresholdHit hitTestThreshold(visage::Point pos) const;

	/// TOOLTIP FOR A POINT — what the control under the cursor does. Returns nullptr if nothing is
	/// there. The device owns the text (through ParamDesc); this just looks it up. For non-param
	/// controls (the threshold bars), the text is declared here, beside the hit test that resolves
	/// them — not in a UI map, so a tooltip can never describe the wrong control.
	///
	/// ONE HIT TEST, TWO QUESTIONS. `hitTestThreshold` already answers "what is under this point" and
	/// returns a `ThresholdHit`. This function uses the same enum as its key — a second hit test for
	/// hovering would be a second place to drift.
	const char* tooltipAt(visage::Point pos) const;

private:
	/// WHAT WAS LAST DRAWN, so `update()` can tell a moving meter from a still one. Defaults match
	/// `OwlttMeterReading`'s own defaults, which is what a freshly built graph has drawn.
	Meters drawnMeters_ {};

	DeviceControl& control(size_t index) const;
	DeviceControl& controlForHit(ThresholdHit hit) const;
	size_t bandForHit(ThresholdHit hit) const;
	/// The inverse of `bandForHit` × the handle kind — so a handle's control is `controlForHit(hitFor(...))`
	/// and there is one table from a handle to a control, not a second copy of `kBandParams`.
	ThresholdHit hitFor(size_t band, Handle handle) const;
	bool isUpperHit(ThresholdHit hit) const;
	bool isThresholdHit(ThresholdHit hit) const;

	struct DragState {
		bool active = false;
		ThresholdHit hit = ThresholdHit::kNone;
		float startY = 0.0f;
		bool routeDragging = false;
		float routeAnchorAmount = 0.0f;
		float routeDragRange = 70.0f;
		float maxRouteAmount = 2.0f;
	};
	mutable DragState dragState_;

	/// WHERE THE POINTER IS, WHEN IT IS OVER THIS FRAME AND NO BUTTON IS DOWN.
	///
	/// A TOOLTIP IS A HOVER AND THIS EDITOR HAD NO HOVER VOCABULARY. visage delivers `onMouseMove`
	/// for a pointer with no button held and `onMouseDrag` while one is -- a distinction this file
	/// learned the hard way when the bars could not be dragged at all -- so the hover road is the one
	/// the drag road was mistakenly written on first.
	visage::Point hover_ { -1.0f, -1.0f };
	bool hovering_ = false;
	bool rail_ = false;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::owltt::Param::kCount)>
	    controls_ {};
};

} // namespace feathers::ui
