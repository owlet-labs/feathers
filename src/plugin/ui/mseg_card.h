/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "mod/mseg/mseg_geometry.h"
#include "mod/mseg/mseg_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/mseg_control.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <memory>
#include "plugin/ui/rack_card.h"

#include <visage/ui.h>

// THE MSEG'S CARD, AND THE CASE device.h WAS DESCRIBING WHEN IT SAID A DEVICE DRAWS ITSELF.
//
// "a filter has a response curve, an envelope has a shape, an MSEG has an editor" — and the third is
// different in kind from the first two. A filter's curve and an envelope's shape are READOUTS: they
// show what the knobs have produced and a hand changes them elsewhere. An MSEG's curve is the
// CONTROL. The breakpoints are dragged, the loop markers are dragged, and the value boxes below hold
// only the things that are not the shape.
//
// WHICH IS WHY THE GEOMETRY IS NOT IN THIS FILE. Everything about where a breakpoint sits, which one
// a click lands on, and what a drag does to the settings lives in `mod/mseg/mseg_geometry.h`, where
// it is answerable without a GUI host. The DAHDSR's card computes its curve inside itself, and four
// separate bugs in it surfaced only because somebody rendered it. This card maps a unit box to pixels
// and forwards the mouse; that is all it is allowed to know.
//
// THE CONTROLS BELOW ARE NOT THE SHAPE. Rate and Amount are the MSEG's modulatable handles on itself (Depth was removed, backlog FH),
// Smoothing is about the destination, and the chips are the switches. The declared controls are
// a handful of globals and the breakpoints — which are edited by
// dragging them, because a list of twenty-four numbers is not an editor.

namespace feathers::ui {

class MsegCard : public FxCard {
public:
	/// TAKES THE DEVICE SEAM — see `EnvCard`'s note. It took a `ParamBridge&`, which the rack has
	/// not got, so this card could not be built there at all.
	MsegCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	MsegCard(backplane::DeviceHost&, size_t, ModState&, EditHistory&) = delete;

	/// **OFF THE MODULATION LIST BEFORE THE MEMORY GOES.** This card puts itself and its boxes on
	/// `ModState`'s watcher list in its constructor and had NO DESTRUCTOR AT ALL, so they stayed on it
	/// after the card died — dangling pointers that the next `notify()` walks.
	///
	/// `EnvCard` — the sibling card, with the identical registration two lines apart — has always
	/// unwatched. One lane guarded and the other not, which is the shape that keeps producing real
	/// defects here.
	///
	/// FOUND BY RUNNING THE RACK SUITES UNDER ASan, where `~FrameWatchers`'s assert is LIVE. `ctest`
	/// was 72/72 straight through it and structurally could not see it: `NDEBUG` makes that assert
	/// compile to nothing in the ordinary build.

	/// THIS CARD'S BODY. The frame, the head and the name are `FxCard::draw`'s — see the note there on
	/// why the standardized header is the default rather than a convention.
	void drawContent(visage::Canvas& canvas) override;

	/// WHICH SOURCE THIS CARD PUBLISHES, so the shared frame can draw its GLOW — not its border,
	/// which is selection's and is the base class's flag. See `FxCard::setFocused`.
	bool headSourceFocused() const override;
	bool headArmed() const override;
	void resized() override;

	/// **THE TRIG PAD** (backlog MV), at the drawn shape's left, where it starts — the DAHDSR's seat. See `trig_pad.h`.
	[[nodiscard]] visage::Bounds trigPadBounds() const;
	[[nodiscard]] TrigPad* trigPadForTest() const noexcept { return trigPad_.get(); }
	/// THE SLOT MENU'S DOTS, WHEN A HOST BINDS ONE (a lane: Feathers' fx lane holds modulators since 2026-10-04) —
	/// `drawSlotMenu` draws nothing where none is bound, so the strip, which draws its own head mark, is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }

	/// The strip asks this rather than the static height() below, because it holds cards of
	/// more than one device now and cannot name their types. See rack_card.h.
	/// WHERE THE BODY STARTS — the card frames itself, so its head sits above everything it draws.
	/// WHERE THE BODY STARTS — **no longer a constant, because the head moved.** In a v-scroll lane
	/// the head is the top row and this is its height; in an h-flow lane the head is the LEFT spine and
	/// the body starts at the card's top with `bodyLeft()` taking the offset instead.
	float bodyTop() const { return bodyTopEdge(); }

	/// THE WHOLE CARD: head, body, and the outputs strip its host fills.
	/// FOLDED, THIS CARD IS ITS HEAD AND NOTHING ELSE — see `EnvCard::laneHeight` and backlog CZ.
	/// **THE FOOT IS AS MANY ROWS AS THE CHIPS NEED AT THE WIDTH BEING ASKED ABOUT** — see
	/// `EnvCard::laneHeightAt`, which carries the whole note for all three modulator cards, including
	/// why the old "one declared output needs one row at every width" precondition was a claim nothing
	/// checked rather than a guarantee.
	float laneHeightAt(float cardWidth) const override {
		return folded() ? kCardHeadHeight
		                : kCardHeadHeight + height() + outputStripHeightAt(cardWidth);
	}
	void foldedChanged() override;

	/// WHICH DEVICE THIS CARD IS — the shared frame reads its name and output count from here.
	const device::Descriptor* descriptor() const override { return &mod::mseg::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// THE SOURCE ONE OF ITS OUTPUTS PUBLISHES — its id and the output's own index (see `EnvCard`).
	backplane::Source sourceOfOutput(size_t output) const {
		return backplane::Source::output(device_, output);
	}



	/// ITS DESIGN WIDTH IN A HORIZONTAL LANE — **NOT the default, which would answer `laneHeight()`
	/// and hand a height back as a width.** The strip lays this card out at `kModCardWidth`; a lane
	/// that let the default stand drew it at whatever its height happened to be, which is how it
	/// arrived in the rack too narrow with its last row of chips clipped off the edge.
	/// **AND IT ANSWERED THAT WIDTH ON THE VERTICAL AXIS TOO** — see `EnvCard::laneExtent`, which
	/// carries the whole note for all three modulator cards.
	float laneExtent(float cross) const override {
		// **A FOLDED CARD IS ITS SPINE, AND THIS LINE IS WHY MSEG WOULD NOT COLLAPSE.** It answered `kModCardWidth` unconditionally, so a folded card kept
		// its full 216 and the fold changed nothing a lane could see — the state flipped, the head
		// redrew, and the geometry never moved. The fold is not a card's own business to remember:
		// the extent has to report it.
		if (folded()) {
			return kCardHeadHeight;
		}
		if (laneAxis() == LaneAxis::Horizontal) {
			return kModCardWidth;
		}
		// THE BASE ANSWERS THE VERTICAL CASE — `laneHeightAt(cross)`. See `EnvCard::laneExtent`.
		return FxCard::laneExtent(cross);
	}

	/// THE BODY'S HEIGHT, head row excluded — the strip owns the head, exactly as it does for the
	/// envelope's card. Taller than the DAHDSR's because the editor needs room to grab things in: a
	/// breakpoint in a 44px box is a target the size of a fingernail.
	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	/// The Trig pad's column at the shape's left — the pad and a gap.
	static constexpr float kTrigColumn = TrigPad::kDiameter + 6.0f;
	static constexpr float kRowGap = 4.0f;
	static constexpr float kEditorHeight = 72.0f;
	static constexpr float kChipHeight = 16.0f;

	static constexpr float height() {
		return kGap + kEditorHeight + kGap + ValueBox::kHeight + kGap + kChipHeight + kGap;
	}

	/// The four value boxes: Time Base, Rate, Smoothing, Amount.
	///
	/// THE FOURTH IS THE TIME BASE AND IT ARRIVED BECAUSE A CASE DEMANDED IT. A time base dropdown was
	/// planned, and the moment `TimeBase` was declared the device-control reachability
	/// guard went red naming it — a control the engine reads and no hand could reach. That guard was
	/// written in this repo one arc earlier for exactly this, and it caught the first parameter added
	/// after it existed.
	///
	/// A BOX RATHER THAN A CHIP, because the chips are switches and this is a choice among nine. The
	/// box already knows how: `formatValue` renders `Unit::Choice` by name, so it reads "1 bar"
	/// rather than "6".
	static constexpr size_t kBoxCount = 6; ///< Base, Rate, Sm, Amount, Phase, Stereo Phase
	static const std::array<mod::mseg::Param, kBoxCount>& layout();

	/// The switches. Same grammar as the envelope card's, minus the ones an MSEG has no use for.
	struct Chip {
		mod::mseg::Param param;
		const char* label;
	};
	/// THREE. It was four: `SmoothingOn` was added here after the reachability guard found the box had
	/// no switch, and it is deleted again — along with the param — because the switch was the wrong
	/// answer to that finding: Smoothing at 0 is the off, with no separate enable. The bottom of the Smoothing box is its off, which is what the box should have meant
	/// all along. And then TWO: `Mode` left for `choices()` (backlog GG).
	static constexpr size_t kChipCount = 4;
	static const std::array<Chip, kChipCount>& chips();

	bool chipOn(size_t index) const;
	/// WHETHER A CHIP CHANGES ANYTHING NOW — the honesty rule. Poly is dead in Free, which is one shape whatever
	/// the switch says (`mod::PolyShape::poly`): drawn dim and not pressable there.
	bool chipLive(size_t index) const;
	std::string chipLabel(size_t index) const;
	void toggleChip(size_t index);

	/// **THE CHOICE: Mode, as a value box that opens a menu (backlog GG).** It was a chip that showed
	/// the current mode and advanced on a press, so reaching Once from Loop went THROUGH Free, applied
	/// — a multivalue toggle, which the UI rules out. The box opens its
	/// list from the descriptor, exactly as the time base's does. It keeps the chip's cell, the last of
	/// the row's three.
	static constexpr size_t kChoiceCount = 1;
	static const std::array<mod::mseg::Param, kChoiceCount>& choices();
	const ValueBox* choiceBoxForTest(size_t index) const {
		return index < kChoiceCount ? choiceBoxes_[index].get() : nullptr;
	}
	ValueBox* choiceBoxForTest(size_t index) {
		return index < kChoiceCount ? choiceBoxes_[index].get() : nullptr;
	}
	/// The cell a choice sits in; its box is inset one pixel so the cell's outline shows.
	visage::Bounds choiceBounds(size_t index) const;
	/// The cell a chip sits in — public so the fit case can measure its caption against it.
	visage::Bounds chipBounds(size_t index) const;

	//-------------------------------//
	// What a case reads instead of pixels //
	//-------------------------------//

	/// WHERE THE EDITOR DRAWS, in this card's own coordinates. The one box everything below maps into.
	visage::Bounds editorBounds() const;

	/// WHERE A PRESS COUNTS AS THE EDITOR'S — `editorBounds()` grown by the grab radius on every side.
	///
	/// A THIRD FACE OF WAYPOINTS BEING HARD TO GRAB, and the one that was total rather than merely
	/// tight. A breakpoint at level 0 is drawn centred on the box's BOTTOM EDGE, and `Bounds::contains`
	/// excludes it — so a press on the dot itself fell outside the editor and did nothing, and the
	/// handle was reachable only from above, at half the tolerance in the direction a shape editor's
	/// handles actually move. Level 1, x = 0 and x = 1 are the same story on the other three sides,
	/// and every shape in this project ends at level 0.
	///
	/// `toUnit` already clamps into the unit box, so a press in the grown margin lands on the nearest
	/// point of the real one. The chips are tested before this and the value boxes are CHILDREN, which
	/// take the event first — so growing the editor's catchment takes nothing from either.
	visage::Bounds editorGrabBounds() const;

	/// The shape's layout in the unit box — handles, markers, segment edges. Straight from the
	/// geometry, so a case can ask the card what it would draw without a canvas.
	mod::mseg::Layout layoutNow() const;

	/// ADVANCE THE PLAYHEAD, AND SAY WHETHER THE SCREEN IS OUT OF DATE. `OwlttGraph::update()` and
	/// `AutomodStrip::update()` are the shipped instances of this contract; this is the third.
	bool updateContent() override;

	/// WHERE THE PLAYHEAD IS DRAWN, in card coordinates.
	///
	/// **A DRAWN POSITION IS NOT OBSERVABLE HEADLESSLY**, and `draw()` asks THIS rather than computing
	/// its own, so a case comparing the marker with the shape is comparing the marker that is drawn.
	struct Marker {
		visage::Point at { 0.0f, 0.0f };
		bool drawn = false;
	};
	[[nodiscard]] Marker marker() const;

	/// THE UNIT-BOX TO PIXEL MAPPING, exposed so a case can restate where a point lands WITHOUT
	/// copying the arithmetic. A case that recomputed `fromUnit` itself would agree with a wrong
	/// version of it, which is the whole failure mode of an independent restatement done by hand.
	[[nodiscard]] visage::Point fromUnitForTest(float x, float y) const { return fromUnit(x, y); }

	/// The polyline, in the unit box: x across, y in the shape's own range.
	std::vector<std::pair<float, float>> curve() const;
	/// **AND THE RIGHT EAR'S** (backlog PA) — `mseg::polylineRight`, at the same xs; empty while Stereo Phase is 0, and
	/// then nothing but the left ear is drawn.
	std::vector<std::pair<float, float>> curveRight() const;
	/// HOW MANY STROKES THE LAST PAINT GAVE THE RIGHT EAR'S LINE — counted by the paint itself, so a case asks whether
	/// the trace was drawn rather than whether it could have been (a repaint is not observable headlessly).
	[[nodiscard]] size_t rightStrokesDrawnForTest() const noexcept { return rightStrokes_; }

	/// Which handle a point in CARD coordinates would grab, or -1. The hit test the mouse uses, so a
	/// case drives the same road a hand does rather than a parallel one.
	int handleAtCardPoint(visage::Point point) const;
	/// Left of where the editor's grab reaches — the Trig pad's column, which no handle claims (backlog MV).
	[[nodiscard]] bool leftOfEditorReach(visage::Point point) const;

	/// WHAT AN OPT-DRAG STARTING HERE WOULD CATCH — the same call the mouse makes, for the same
	/// reason `handleAtCardPoint` exists. A case that computed the target itself would be checking
	/// the geometry twice and the card not at all.
	mod::mseg::CurveTarget curveTargetAtCardPoint(visage::Point point) const;

	/// WHAT THE EDITOR IS HIGHLIGHTING RIGHT NOW, which is "the affected segments are
	/// highlighted while editing" made answerable without a canvas. Kind::None when nothing is
	/// targeted. During a drag this is the drag's target; otherwise it is what an Opt-hover found,
	/// which is the disambiguation half — it tells a hand what it caught BEFORE it commits.
	mod::mseg::CurveTarget highlightForTest() const;

	/// EVERY CONTROL A HAND CAN REACH THROUGH THIS CARD, in the device's own vocabulary. Same seam as
	/// `FilterCard::addressedParamsForTest`, and it exists for the same reason: the reachability walk
	/// runs from the DECLARATIONS inward, so it needs the card to say what it addresses rather than
	/// guessing from a frame tree that cannot see a gesture.
	std::vector<mod::mseg::Param> addressedParamsForTest() const;

	/// What a box's caption reads.
	const std::string& boxCaption(size_t index) const;

	/// One of the boxes, as a frame — for the claim that each follows the armed flag itself. The
	/// envelope card needed this and so does this one, for the same reason: repainting a card does
	/// not repaint the boxes inside it.
	const visage::Frame* boxFrame(size_t index) const;

	/// THE BOX ITSELF, for the one question `boxFrame` cannot answer: whether it opens a menu. A
	/// `Frame*` says where a control is; this says what kind of control it is.
	const ValueBox* boxForTest(size_t index) const {
		return index < kBoxCount ? boxes_[index].get() : nullptr;
	}

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST, into
	/// lists that are still alive.
	std::vector<Watch> watches_;
	void bindBox(size_t index);
	visage::Bounds boxBounds(size_t index) const;

	/// Open a menu of a stepped box's declared names. See the picker note in `value_box.h`.
	/// ONE ROW OF `kChipCount + kChoiceCount` EQUAL CELLS — the chips first, then the choice.
	visage::Bounds cellBounds(size_t cell) const;

	/// A point in this card's pixels, as the unit box the geometry speaks. The one mapping, in one
	/// place — see plotUnit on the envelope card for why a mapping that lives in a paint function is
	/// a mapping no case can reach.
	std::pair<float, float> toUnit(visage::Point point) const;
	visage::Point fromUnit(float x, float y) const;

	mod::mseg::Settings settings() const;

	/// WHAT PLAYHEAD WAS LAST DRAWN, so `update()` can tell a moving shape from a still one. The
	/// default matches what a fresh bridge answers — idle — which is what a freshly built card drew.
	mod::Playhead drawnPlayhead_ {};
	/// See `rightStrokesDrawnForTest`.
	size_t rightStrokes_ = 0;

	backplane::DeviceHost& host_;

	/// WRITE THE WHOLE ROW BACK — replaces `bridge_.setMsegSettings(device_, s)`.
	void writeSettings(const mod::mseg::Settings& settings);

	/// WHERE THE SHAPE IS, OR IDLE. See `DeviceHost::playheadAt`: a playhead is a READING, so a host
	/// with no publisher declines and the marker is not drawn rather than frozen at segment zero.
	mod::Playhead playhead() const;
	DeviceInstanceId device_ = kNoInstance;
	ModState& mod_;
	EditHistory& history_;

	std::array<std::unique_ptr<DeviceControl>, kBoxCount> controls_;
	/// **THE ENABLE SQUARE'S OWN CONTROL, WHICH THIS CARD DID NOT HAVE (BM).** `controls_` covers
	/// only the visible boxes, so the head's switch was toggled by writing the field straight
	/// through the host — which skips `DeviceControl`, and `DeviceControl` is where the undo step
	/// is recorded. Turning a modulator off was therefore not undoable while turning an audio
	/// effect off was. Built lazily because a device with no declared enable has none.
	mutable std::unique_ptr<DeviceControl> enableControl_;

public:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt.
	device::Modulatable* enableModulatable() override;
	/// The hand-drawn parts' tooltips — see `FxCard::contentTooltipAt`.
	const char* contentTooltipAt(visage::Point local) const override;

private:
	std::array<std::unique_ptr<ValueBox>, kBoxCount> boxes_;
	std::array<std::unique_ptr<DeviceControl>, kChoiceCount> choiceControls_;
	std::array<std::unique_ptr<ValueBox>, kChoiceCount> choiceBoxes_;
	std::unique_ptr<DeviceControl> trigControl_;
	/// "NOTES FIRE TRIG" (backlog NU), the pad's normal — declared before the pad, which holds it.
	std::unique_ptr<DeviceControl> notesTrigControl_;
	std::unique_ptr<TrigPad> trigPad_;

	/// Which handle is under the hand, or -1. A drag is a handle plus the settings it started from,
	/// so an undo step is the whole gesture rather than one mouse-move.
	int draggingHandle_ = -1;
	/// Which marker is under the hand: `kMarkerLoopStart`, `kMarkerLoopEnd`, `kMarkerSustain`, or -1
	/// for none. Separate from the handle because they move different things.
	int draggingMarker_ = -1;
	/// THE THREE MARKERS, NAMED. `draggingMarker_ == 0` was readable while there were two of them and
	/// a bool decided which; a third makes the bare number a place to make a mistake.
	///
	/// LOOP AND SUSTAIN NEVER BOTH EXIST — `loopVisible` is true for Loop/Free/LFO Retrig modes and
	/// `sustainVisible` is its negation (true only for Gate) — so the two grabs below cannot compete for
	/// the same pixel, and the mouse-down order between them says nothing.
	static constexpr int kMarkerLoopStart = 0;
	static constexpr int kMarkerLoopEnd = 1;
	static constexpr int kMarkerSustain = 2;
	mod::mseg::Settings before_ {};

	/// What the Opt-drag in progress is bending, and the settings it started from.
	///
	/// THE DRAG IS ABSOLUTE FROM ITS ANCHOR RATHER THAN INCREMENTAL, which is why `curveBefore_` is
	/// kept rather than just applying each mouse-move to the current value. `withCurvature` ADDS, so
	/// integrating mouse-moves would make the bend depend on how many events the platform delivered —
	/// a drag that reached the same pixel by a longer route would bend further. Every move recomputes
	/// from the anchor, so the shape is a function of where the pointer IS.
	mod::mseg::CurveTarget curveTarget_ {};
	mod::mseg::Settings curveBefore_ {};
	float curveAnchorY_ = 0.0f;

	/// What an Opt-hover found, so the highlight can be drawn before a button goes down.
	///
	/// A MODIFIER PRESSED WITHOUT MOVING THE MOUSE LIGHTS NOTHING, and that is a known limit rather
	/// than an oversight: visage delivers modifier state on mouse events, and a stationary pointer
	/// generates none. The highlight appears on the first pixel of movement with Opt held.
	mod::mseg::CurveTarget hoverTarget_ {};
};

} // namespace feathers::ui
