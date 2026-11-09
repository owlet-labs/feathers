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

#include "plugin/ui/mseg_card.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/plot_grid.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trace_marker.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace mseg = mod::mseg;

namespace {

/// HOW BIG THE PLAYHEAD'S GLYPH IS. `starGlyph` draws a halo three times this and arms nearly twice
/// it, so the real footprint is about 3.5x the number — the automod strip's reasoning, and its
/// number, because this editor is the same height class.
constexpr float kPlayheadSize = 3.0f;

constexpr std::array<const char*, MsegCard::kBoxCount> kCaptions { "Base", "Rate", "Sm", "Amt", "Ph", "St" };

/// How near, IN PIXELS, a click has to land to grab a breakpoint.
///
/// THIRTEEN, AND IT WAS NINE — on the installed build waypoints were hard to grab. Nine was
/// not the whole of the problem, though: the card asked the geometry for ONE radius in unit-box terms
/// and computed it as `max(9/width, 9/height)`, which on a 198x72 editor is 9/72 = 0.125. Applied to
/// both axes that is 24.7px sideways and 9px vertically — a flat ellipse whose tight axis is the one
/// a hand moves a breakpoint along. `handleAt` takes two radii now, so this is a genuine circle on
/// the glass, and the number is a target a fingertip can hit rather than a target a mouse can.
constexpr float kGrabPixels = 13.0f;

/// The markers' own tolerance, in pixels, and it is measured on x ALONE because a marker is a
/// full-height line rather than a point. Narrower than a handle's for the reason the order below
/// changed: the two share an x whenever a loop boundary sits on a breakpoint.
constexpr float kMarkerGrabPixels = 7.0f;

/// HOW FAR AN OPT-DRAG TRAVELS FOR A FULL BEND, as a share of the editor's height. Half the box, so a
/// gesture that starts in the middle and ends at an edge covers one sign's whole range — and a
/// gesture that starts at an edge can still reach the other end by crossing the box.
constexpr float kCurveTravelForFullBend = 0.5f;

/// **HOW CLOSE A DRAG HAS TO BE TO A SUBDIVISION BEFORE IT STICKS, IN PIXELS.**
///
/// Three, which is about the width of the breakpoint's own outline — close enough that a hand aiming
/// at a line lands on it, small enough that the two thirds of a division between the lines are still
/// freely reachable. The intent is "a little stickiness", and the adjective is the specification:
/// a radius wide enough to feel like quantisation would be a different feature.
///
/// IN PIXELS AND CONVERTED AT THE CALL SITE, not in unit x — see `plot::stickToDivision`. This card's
/// width moves with the window, and a snap stated as a fraction of it would grab harder the narrower
/// the card got.
constexpr float kStickyPixels = 3.0f;

/// The grab radii in UNIT-BOX terms: `kGrabPixels` on each axis, converted by the box that axis
/// spans. One place, because the mouse road and the test seams must ask the same question.
std::pair<float, float> grabRadii(const visage::Bounds& box) {
	return { kGrabPixels / std::max(box.width(), 1.0f), kGrabPixels / std::max(box.height(), 1.0f) };
}

/// A whole-settings edit, which is what a shape drag is.
///
/// NOT A PER-FIELD RECORD. Dragging one breakpoint sideways changes a duration; dragging it up
/// changes a level; dragging a loop marker changes an index and may drag the other marker with it
/// through sanitize. Recording those as separate field edits would make one gesture into three undo
/// steps that have to be replayed in order — and the order is not something the card knows. The
/// settings are one value and the gesture moved it from one to another.
class MsegShapeEdit : public EditRecord {
public:
	MsegShapeEdit(backplane::DeviceHost& host, DeviceInstanceId id, mseg::Settings before, mseg::Settings after)
	    : host_(host), device_(id), before_(before), after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Shape"; }

private:
	/// THE ROW ENCODER IS THE ONE PLACE THAT KNOWS THE SHAPE — `rowOf` — so undo writes the same way
	/// the card does rather than restating it.
	void write(const mod::mseg::Settings& settings) const {
		const mod::mseg::Row row = mod::mseg::rowOf(settings);
		backplane::ControlGestureScope gesture(host_, device_); // one host gesture per undo (host slots, T6b)
		for (size_t i = 0; i < row.size(); ++i) {
			gesture.setField(i, row[i]);
		}
	}

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	mseg::Settings before_;
	mseg::Settings after_;
};

} // namespace

const std::array<mseg::Param, MsegCard::kBoxCount>& MsegCard::layout() {
	// THE CONTROLS THAT ARE NOT THE SHAPE. Rate is the device's own modulatable handle on time, Amount on depth;
	// Smoothing is about what the destination does with a fast shape. Everything else a hand touches
	// on this card is dragged in the editor above.
	static const std::array<mseg::Param, kBoxCount> kLayout {
		// THE WINDOW'S TWO HALVES, SIDE BY SIDE, because they only mean anything together: the shape
		// fits inside `Base x Rate`, and reading one without the other says nothing about how long
		// the shape takes.
		mseg::Param::TimeBase,
		mseg::Param::TimeScale,
		mseg::Param::Smoothing,
		// THE AMOUNT (backlog DH), the host's scale on every output — and the only depth since the shape's own Depth
		// was removed (backlog FH), as on the DAHDSR.
		mseg::Param::Amount,
		// PHASE — where in the shape it starts, 0..1.
		mseg::Param::Phase,
		// AND STEREO PHASE (backlog PA) beside it: how far ahead the right ear reads the shape, in the same window.
		mseg::Param::StereoPhase,
	};
	return kLayout;
}

const std::array<MsegCard::Chip, MsegCard::kChipCount>& MsegCard::chips() {
	static const std::array<Chip, kChipCount> kChips {
		Chip { mseg::Param::PingPong, "P-P" },
		Chip { mseg::Param::Bipolar, "±" },
		// **POLY — one shape per note, or one for the device**. A switch, so a chip: two states,
		// both visible by being the only two.
		Chip { mseg::Param::Poly, "Poly" },
		// **SMOOTH THROUGH RETRIGGER** (backlog MM): a new note's smoothing starts where the last note's output stood.
		// Live only where it changes something — see `chipLive`.
		Chip { mseg::Param::SmoothCarry, "Carry" },
		// **AND "Sm" IS GONE AGAIN, WITH THE PARAM BEHIND IT.** The reachability guard was right that
		// the Smoothing box did nothing; a switch was the wrong repair, because it left a box whose
		// effect depended on a chip beside it. The bottom of the box's own travel carries "off" now
		// — one control, one fact.
		// MODE IS NOT A CHIP ANY MORE — see `choices()` (backlog GG). A chip is a switch.
	};
	return kChips;
}

const std::array<mseg::Param, MsegCard::kChoiceCount>& MsegCard::choices() {
	static const std::array<mseg::Param, kChoiceCount> k { mseg::Param::Mode };
	return k;
}

// `~MsegCard` IS GONE. It unwatched this card and its four boxes by hand; `ui::Watch` holds those
// five subscriptions now and releases them, so there is nothing to remember and nothing to forget.

MsegCard::MsegCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history)
    : host_(host), device_(id), mod_(mod), history_(history) {
	for (size_t i = 0; i < kBoxCount; ++i) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, mod::mseg::kDescriptor,
		                                              static_cast<size_t>(layout()[i]), &mod_, &history_);
		boxes_[i] = std::make_unique<ValueBox>(*controls_[i], kCaptions[i]);
		// **THE TIME BASE'S MENU IS NOT WIRED HERE ANY MORE, AND THAT IS THE POINT.** This card set a
		// picker on the one box it knew was a list — *"the same three lines serve any Choice box that
		// wants a menu"*, said the note, of three lines that served exactly one. Automod's four choice
		// cells stayed draggable for a fortnight because nobody came back to add them.
		//
		// `ValueBox` opens the menu itself now, from the DESCRIPTOR: stepped, named, three or more
		// entries: a value box that selects among several values is a dropdown. Nothing opts in and nothing can forget. See `ValueBox::showChoiceMenu`.
		addChild(boxes_[i].get());
		bindBox(i);
	}

	// EACH BOX FOLLOWS THE ARMED FLAG ITSELF, and the card as well. Repainting a card does not
	// repaint the boxes inside it — visage's redraw() marks one frame — which the envelope card had
	// to learn from a glow that never arrived.
	watches_.push_back(mod_.watch(this));
	for (size_t i = 0; i < kBoxCount; ++i) {
		watches_.push_back(mod_.watch(boxes_[i].get()));
	}

	// THE TRIG PAD (backlog MV): the device's Trig control, and the fire count it lights from.
	trigControl_ = std::make_unique<DeviceControl>(host_, device_, mod::mseg::kDescriptor,
	                                               static_cast<size_t>(mseg::Param::Trig), &mod_, &history_);
	trigPad_ = std::make_unique<TrigPad>(*trigControl_, [this](uint32_t& fires) {
		return host_.trigFiresAt(device_, fires);
	});
	// AND ITS NORMAL (backlog NU): the right-click's "Notes fire Trig", and the dot that shows it made.
	notesTrigControl_ = std::make_unique<DeviceControl>(host_, device_, mod::mseg::kDescriptor,
	                                                    static_cast<size_t>(mseg::Param::NotesTrig), &mod_, &history_);
	trigPad_->setNormal({ notesTrigControl_.get() });
	addChild(trigPad_.get());
	watches_.push_back(mod_.watch(trigPad_.get())); // its ring is the mapping signal too

	// THE CHOICE — compact, uncaptioned, in the chip row. Its menu is the box's own, from the
	// descriptor, as the time base's is; the box subscribes itself to the armed flag (`refreshWatch`).
	for (size_t i = 0; i < kChoiceCount; ++i) {
		choiceControls_[i] = std::make_unique<DeviceControl>(host_, device_, mod::mseg::kDescriptor,
		                                                    static_cast<size_t>(choices()[i]), &mod_,
		                                                    &history_);
		choiceBoxes_[i] = std::make_unique<ValueBox>(*choiceControls_[i], "");
		choiceBoxes_[i]->setCompact(true);
		choiceBoxes_[i]->onValueChanged([this]() { redraw(); });
		addChild(choiceBoxes_[i].get());
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **THE HEAD'S MARKS FIRST, BECAUSE THIS CARD IS DRAWN OVER THEM.** The unified frame put the
		// card's rectangle across the whole head, and a child frame takes the press — the exact bug
		// this card's own note records from a first version. Handed back rather than hit-tested
		// through; see `FxCard::headMarkPressed`.
		// **THROUGH THE SHARED GESTURE, NOT PAST IT.** This used to call `headMarkPressed` directly and
		// return; `noteCardPress` now dispatches that mark itself, and — since the `(-)` mark was retired
		// (double click made it redundant) — a head press is also
		// how this card folds. Handling the mark without the gesture would have left these two cards
		// with no fold at all the moment the mark went.
		noteCardPress(e);
		// **THE SLOT MENU, WHEN A HOST BINDS ONE** — Feathers' fx lane, which holds modulators since 2026-10-04, and a
		// chain's lane (`onSlotMenu`). Every audio card makes this call after `noteCardPress`; the modulator cards did
		// not, so in a lane their dots were drawn and dead (the head-verb sweep found all five). The strip binds no
		// `onSlotMenu` — it opens its menu from the head mark — so there this is a no-op.
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		// **THE ENABLE SQUARE, TESTED BEFORE ANY EDGE ARITHMETIC — because the head is not always on
		// top.** Disable on the MSEG once did nothing, and then, after a first
		// fix, the same square was still dead in the RACK.
		//
		// The first fix put this inside `if (e.position.y < bodyTopEdge())`, which is the head test for
		// a card whose head is a TOP row — Feathers' modulator strip. **The rack lays its cards along
		// x with a LEFT head edge** (`CardHeadEdge::Left`), so that branch is false there and the press
		// fell through to the body. One card class, two hosts, two head geometries: the sweep over
		// every rack card is what caught it, and asking `enableBounds()` directly is what makes the
		// question independent of which edge the head is on.
		// **THE BASE OWNS THIS NOW, AND THAT FIXED A DEFECT AS WELL AS A DUPLICATE (BM).** This arm
		// used to write the field straight through the host — `host_.setField(...)` — which is where
		// this card's other writes go, so it looked right. It skips `DeviceControl`, and
		// `DeviceControl` is where the undo step is recorded, so turning a modulator off was NOT
		// undoable while turning an audio effect off was. `enableModulatable()` below puts the write
		// back on the road that records history.
		if (enablePressAt(e.position)) {
			return;
		}
		if (e.position.y < bodyTopEdge()) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			return;
		}
		for (size_t chip = 0; chip < kChipCount; ++chip) {
			if (chipBounds(chip).contains(e.position)) {
				if (chipLive(chip)) {
					toggleChip(chip);
				}
				return;
			}
		}
		if (!editorGrabBounds().contains(e.position)) {
			return;
		}

		before_ = settings();
		const auto [ux, uy] = toUnit(e.position);
		const mseg::Layout laid = layoutNow();
		const visage::Bounds box = editorBounds();
		const auto [grabX, grabY] = grabRadii(box);

		// OPT FIRST, BECAUSE IT IS A DIFFERENT GESTURE RATHER THAN A DIFFERENT TARGET. Opt (on macOS)
		// selects either a waypoint, which applies an inverse curve on either side, or a segment. With
		// Opt down the hand is bending, never moving, so a
		// breakpoint under the pointer means "bend both my sides" and not "drag me".
		if (e.isAltDown()) {
			curveTarget_ = mseg::curveTargetAt(laid, ux, uy, grabX, grabY, before_.bipolar);
			if (curveTarget_.kind != mseg::CurveTarget::Kind::None) {
				curveBefore_ = before_;
				curveAnchorY_ = e.position.y;
				redraw();
			}
			return;
		}

		// DOUBLE-CLICK ADDS A WAYPOINT, OR REMOVES THE ONE IT LANDED ON. There was once no way to add
		// a waypoint; the affordance is the one every shape editor uses, and the same click on an
		// existing breakpoint is its inverse. Taken BEFORE the marker test so a hand can add a
		// breakpoint inside a loop, which is where a hand editing a looped shape spends its time.
		if (e.repeatClickCount() >= 2) {
			const int handle = mseg::handleAt(laid, ux, uy, grabX, grabY, before_.bipolar);
			const mseg::Settings next =
			    handle >= 0 ? mseg::withWaypointRemoved(before_, static_cast<size_t>(handle))
			                : mseg::withWaypointAdded(before_, ux);
			if (!(next == before_)) {
				writeSettings( next);
				history_.record(std::make_unique<MsegShapeEdit>(host_, device_, before_, next));
				redraw();
			}
			return;
		}

		// A BREAKPOINT FIRST, THEN A MARKER, AND THAT ORDER IS THE OPPOSITE OF WHAT IT WAS.
		//
		// The two overlap — a loop marker sits on a segment boundary and so does a handle — and the
		// old order gave the marker first refusal within its tolerance at ANY height, which made every
		// breakpoint on a loop boundary permanently ungrabbable. That is the same defect as report
		// #4, in its total form rather than its degraded one, and it is exactly the failure the
		// tie-breaking note on `handleAt` exists to prevent.
		//
		// The marker is still reachable, and reachable more honestly: it is drawn as a full-height
		// line, so a hand aiming at it anywhere OTHER than the breakpoint's dot still catches it. The
		// dot is 13px across; the line is 72px tall.
		const int handle = mseg::handleAt(laid, ux, uy, grabX, grabY, before_.bipolar);
		if (handle >= 0) {
			draggingHandle_ = handle;
			return;
		}

		const float markerGrab = kMarkerGrabPixels / std::max(box.width(), 1.0f);
		if (laid.loopVisible) {
			if (std::abs(laid.loopStartX - ux) < markerGrab) {
				draggingMarker_ = kMarkerLoopStart;
				return;
			}
			if (std::abs(laid.loopEndX - ux) < markerGrab) {
				draggingMarker_ = kMarkerLoopEnd;
				return;
			}
		}

		// AND THE SUSTAIN MARKER, ON THE SAME TOLERANCE AND AFTER THE BREAKPOINTS FOR THE SAME REASON.
		// It drags to a different location. It is never on the picture at the
		// same time as the loop markers, so this is an else in effect and the order above is not a
		// precedence.
		//
		// GRABBABLE WHILE PARKED, which is the half that makes the gesture reversible: with no sustain
		// point the marker sits dim at x = 0, so the same drag that cleared it picks it up again. It
		// yields to handle 0 within 13px of that handle's dot — a hand aiming at the wall anywhere
		// else along the box's 72px height still catches the line.
		if (laid.sustainVisible && std::abs(laid.sustainX - ux) < markerGrab) {
			draggingMarker_ = kMarkerSustain;
			return;
		}
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		// **CHAINED FIRST.** `FxCard::noteCardMove` is what reports `onCardDrag`, which is how a host
		// reorders — and this handler returns early on exactly the path a head drag takes. Calling it
		// after the return would have been calling it never, which is why reorder did nothing here.
		noteCardMove(e);
		const auto [ux, uy] = toUnit(e.position);
		if (curveTarget_.kind != mseg::CurveTarget::Kind::None) {
			// FROM THE ANCHOR, NOT FROM THE LAST EVENT — see curveBefore_'s note. Up is fuller, which
			// is what a positive shape means everywhere in this project; see mod::kShapeMax.
			const visage::Bounds box = editorBounds();
			const float span = std::max(box.height() * kCurveTravelForFullBend, 1.0f);
			const float delta = (curveAnchorY_ - e.position.y) / span * mod::kShapeMax;
			writeSettings( mseg::withCurvature(curveBefore_, curveTarget_, delta));
			redraw();
			return;
		}
		if (draggingMarker_ == kMarkerSustain) {
			writeSettings( mseg::withSustainMarkerMoved(settings(), ux));
			redraw();
			return;
		}
		if (draggingMarker_ >= 0) {
			writeSettings(
			    mseg::withLoopMarkerMoved(settings(), draggingMarker_ == kMarkerLoopStart, ux));
			redraw();
			return;
		}
		if (draggingHandle_ >= 0) {
			// **A LITTLE STICKINESS ON THE TIME SUBDIVISIONS**. The tolerance is
			// stated in PIXELS and converted here, so the pull feels the same however wide the card is
			// — a radius in unit x would grab harder on a narrow one, and this card's width moves with
			// the window. The LEVEL is left alone: there is no horizontal grid to stick to but the
			// mid-amplitude line, and a shape snapping its level to the middle is not what was asked.
			const float width = std::max(editorBounds().width(), 1.0f);
			const float stuck = plot::stickToDivision(ux, kStickyPixels / width);
			writeSettings(
			    mseg::withHandleMoved(settings(), static_cast<size_t>(draggingHandle_), stuck, uy));
			redraw();
		}
	};

	onMouseUp() = [this](const visage::MouseEvent& e) {
		// **CHAINED FIRST, BECAUSE THE EARLY RETURN BELOW IS THE HEAD-PRESS PATH.**
		// `FxCard::noteCardRelease` carries the drag anchor, `armed_`, and the FOLD; this handler
		// returns immediately when no shape handle was being dragged, which is exactly what a press on
		// the head looks like. Calling it after the return would have been calling it never.
		noteCardRelease(e);
		const bool wasCurving = curveTarget_.kind != mseg::CurveTarget::Kind::None;
		if (draggingHandle_ < 0 && draggingMarker_ < 0 && !wasCurving) {
			return;
		}
		draggingHandle_ = -1;
		draggingMarker_ = -1;
		curveTarget_ = {};
		const mseg::Settings after = settings();
		// ONE STEP PER GESTURE, and none at all for a drag that moved nothing — the same rule the
		// knob's value road follows.
		if (!(after == before_)) {
			history_.record(std::make_unique<MsegShapeEdit>(host_, device_, before_, after));
		}
		if (wasCurving) {
			redraw(); // the highlight goes out with the gesture
		}
	};

	// THE HOVER HALF OF THE HIGHLIGHT, which is what makes waypoints easy to grab rather
	// than merely widening the tolerance. The affected segments are highlighted
	// *while editing*; showing the same highlight under an Opt-hover means a hand can see which of
	// two adjacent targets it caught BEFORE it commits to a drag, which is the disambiguation a
	// bigger radius on its own cannot give.
	const auto hover = [this](const visage::MouseEvent& e) {
		mseg::CurveTarget found;
		if (e.isAltDown() && editorGrabBounds().contains(e.position)) {
			found = curveTargetAtCardPoint(e.position);
		}
		if (!(found == hoverTarget_)) {
			hoverTarget_ = found;
			redraw();
		}
	};
	onMouseMove() = hover;
	onMouseEnter() = hover;
	onMouseExit() = [this](const visage::MouseEvent&) {
		if (hoverTarget_.kind != mseg::CurveTarget::Kind::None) {
			hoverTarget_ = {};
			redraw();
		}
	};
}

mseg::Settings MsegCard::settings() const {
	// THE DEVICE DECODES ITS OWN ROW — absent fields land on their DECLARED defaults, not zero.
	mod::mseg::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return mod::mseg::settingsOf(row, present);
}

void MsegCard::bindBox(size_t index) {
	ValueBox* box = boxes_[index].get();
	box->onValueChanged([this]() { redraw(); });
	// The card records the undo step, not the box — see ValueBox::onEdited. A whole-settings record
	// here too, so a box edit and a shape drag undo the same way.
	box->onEdited([](double, double) {});
}

visage::Bounds MsegCard::editorBounds() const {
	return { bodyLeft() + kPadX + kTrigColumn, bodyTop() + kGap, bodyWidth() - 2.0f * kPadX - kTrigColumn,
		     kEditorHeight };
}

visage::Bounds MsegCard::trigPadBounds() const {
	const visage::Bounds box = editorBounds();
	return { bodyLeft() + kPadX, box.y() + (box.height() - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
		     TrigPad::kDiameter };
}

visage::Bounds MsegCard::editorGrabBounds() const {
	const visage::Bounds box = editorBounds();
	// **CLAMPED TO THE BODY, because the card grew a head above it and a strip below.**
	//
	// The grab region deliberately reaches past the drawn editor — that is what makes the far handle
	// catchable — and while the editor started at the card's top edge, the part that reached above it
	// fell outside the card and was clipped by whatever sat there. It no longer does: the head is
	// inside this frame now, so an unclamped region would reach INTO it and a drag beside the fold
	// would grab a breakpoint.
	//
	// MEASURED: the middle handle's selectable area went from ~1568 square pixels back to a figure in
	// line with the extremes' ~700. A case comparing the extremes AGAINST the middle is what caught
	// it — the extremes are clipped by the card's sides, so the middle silently swallowing the head
	// showed up as a ratio rather than as anything visible.
	const float top = std::max(bodyTop(), box.y() - kGrabPixels);
	const float bottom = std::min(box.bottom() + kGrabPixels, bodyTop() + height());
	return { box.x() - kGrabPixels, top, box.width() + 2.0f * kGrabPixels,
		     std::max(0.0f, bottom - top) };
}


visage::Bounds MsegCard::boxBounds(size_t index) const {
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float each = (usable - kRowGap * (kBoxCount - 1)) / static_cast<float>(kBoxCount);
	return { bodyLeft() + kPadX + static_cast<float>(index) * (each + kRowGap),
		     bodyTop() + kGap + kEditorHeight + kGap, each, ValueBox::kHeight };
}

visage::Bounds MsegCard::cellBounds(size_t cell) const {
	// **A CHOICE CELL IS TWO UNITS WIDE AND A CHIP ONE** — the LFO card's rule, taken when Poly made this row three
	// chips and a choice (the Mode box's "LFO Retrig" no longer fit a quarter). The chips come first here.
	constexpr size_t kCells = kChipCount + kChoiceCount;
	// **AND A CHIP THREE QUARTERS OF ONE** since Carry made the chips four (backlog MM): at a whole unit each the
	// Mode box lost the width "LFO Retrig" needs. Four chips at three quarters take the room three took at one.
	constexpr float kChipUnits = 0.75f;
	constexpr float kUnits = kChipUnits * static_cast<float>(kChipCount) + 2.0f * static_cast<float>(kChoiceCount);
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float unit = (usable - kRowGap * (kCells - 1)) / kUnits;
	float x = bodyLeft() + kPadX;
	for (size_t before = 0; before < cell; ++before) {
		x += (before < kChipCount ? kChipUnits : 2.0f) * unit + kRowGap;
	}
	return { x, bodyTop() + kGap + kEditorHeight + kGap + ValueBox::kHeight + kGap,
		     (cell < kChipCount ? kChipUnits : 2.0f) * unit, kChipHeight };
}

visage::Bounds MsegCard::chipBounds(size_t index) const {
	return cellBounds(index);
}

visage::Bounds MsegCard::choiceBounds(size_t index) const {
	return cellBounds(kChipCount + index);
}

void MsegCard::resized() {
	for (size_t i = 0; i < kBoxCount; ++i) {
		const visage::Bounds b = boxBounds(i);
		boxes_[i]->setBounds(b.x(), b.y(), b.width(), b.height());
	}
	for (size_t i = 0; i < kChoiceCount; ++i) {
		seatInChipCell(*choiceBoxes_[i], choiceBounds(i));
	}
	const visage::Bounds trig = trigPadBounds();
	trigPad_->setBounds(trig.x(), trig.y(), trig.width(), trig.height());
}

std::pair<float, float> MsegCard::toUnit(visage::Point point) const {
	const visage::Bounds box = editorBounds();
	const float x = std::clamp((point.x - box.x()) / std::max(box.width(), 1.0f), 0.0f, 1.0f);
	// Y IS FLIPPED AND IS IN THE SHAPE'S OWN RANGE, not in 0..1. A bipolar shape's box runs -1..1, so
	// the mapping has to know the polarity — the same fact the envelope card's plotUnit carries, and
	// the same one it got wrong by clamping.
	const float up = 1.0f - std::clamp((point.y - box.y()) / std::max(box.height(), 1.0f), 0.0f, 1.0f);
	const bool bipolar = settings().bipolar;
	return { x, bipolar ? up * 2.0f - 1.0f : up };
}

visage::Point MsegCard::fromUnit(float x, float y) const {
	const visage::Bounds box = editorBounds();
	const bool bipolar = settings().bipolar;
	const float up = bipolar ? (y + 1.0f) * 0.5f : y;
	return { box.x() + std::clamp(x, 0.0f, 1.0f) * box.width(),
		     box.bottom() - std::clamp(up, 0.0f, 1.0f) * box.height() };
}

mseg::Layout MsegCard::layoutNow() const {
	return mseg::layoutOf(settings());
}

std::vector<std::pair<float, float>> MsegCard::curveRight() const {
	return mseg::polylineRight(settings());
}

std::vector<std::pair<float, float>> MsegCard::curve() const {
	return mseg::polyline(settings());
}

bool MsegCard::updateContent() {
	// **CONTENT-GATED, EXACTLY AS `OwlttGraph::update()` IS.** One read, one comparison against what
	// was drawn, a `redraw()` only when they differ — so a rack with nothing playing costs a
	// comparison and a shape mid-flight costs one frame per change and no more.
	//
	// THE WHOLE READING IS COMPARED, ORDINAL INCLUDED. A shape that steps from the end of one segment
	// to the start of the next can land on a phase it has just had; comparing only the phase would
	// sit on a stale segment for as long as the numbers happened to agree, which is the defect
	// `OwlttGraph` records about comparing only its level arrays and not the clip bit.
	// A READING, NOT A SETTING — see `EnvCard::playhead`. Declines where nobody publishes, so the
	// marker is absent rather than frozen at segment zero.
	const bool trigChanged = trigPad_->poll();
	const mod::Playhead now = playhead();
	if (now == drawnPlayhead_) {
		return trigChanged;
	}
	drawnPlayhead_ = now;
	redraw();
	return true;
}

MsegCard::Marker MsegCard::marker() const {
	const mod::Playhead where = playhead();
	// A SHAPE THAT IS NOT RUNNING HAS NO POSITION, and a marker parked at the end would be saying
	// something the device is not doing. `mod::Playhead` carries idle as a state rather than as a
	// coordinate for exactly this.
	if (!where.running()) {
		return {};
	}
	const mseg::Layout laid = layoutNow();
	if (where.ordinal >= laid.segmentEnd.size()) {
		return {};
	}
	// **THE SEGMENT'S OWN SPAN IN THE UNIT BOX**, from the geometry the card already draws its
	// segment bands and its loop markers from. Segment 0 starts at the left edge; every later one
	// starts where its predecessor ended.
	const float from = (where.ordinal == 0) ? 0.0f : laid.segmentEnd[where.ordinal - 1];
	const float to = laid.segmentEnd[where.ordinal];
	const float unitX = from + (to - from) * where.phase;

	// **AND THE Y COMES OFF THE DRAWN LINE, not off a fresh evaluation of the shape.** The curve is
	// drawn as chords between polyline points; evaluating the segment again here would put the marker
	// off the line by however much the sampling rounded a bend — worst exactly where the shape bends,
	// which is where a reader is looking. This is what `TraceMarker` is for.
	const std::vector<std::pair<float, float>> points = curve();
	const TraceMarker::Landing on =
	    TraceMarker::locateBy(points.size(), [&points](size_t i) { return points[i].first; }, unitX);
	if (!on.valid) {
		return {};
	}
	const visage::Point a = fromUnit(points[on.lo].first, points[on.lo].second);
	const visage::Point b = fromUnit(points[on.hi].first, points[on.hi].second);
	return { { TraceMarker::at(on, a.x, b.x), TraceMarker::at(on, a.y, b.y) }, true };
}

bool MsegCard::leftOfEditorReach(visage::Point point) const {
	// **THE TRIG PAD'S COLUMN IS NOT THE FIRST HANDLE'S** (backlog MV). `toUnit` clamps a point left of the editor to
	// x = 0, so everything between the card's edge and the editor grabbed handle 0 — fine while that was the card's
	// 9px margin, and 29px of it once the pad's column moved the editor right, which made the first handle three
	// times as catchable as the last. So the left reach mirrors the right one: the card's margin past the editor's
	// right edge, measured, and nothing nearer the pad.
	const visage::Bounds box = editorBounds();
	const float rightReach = static_cast<float>(width()) - box.right();
	return point.x < box.x() - rightReach;
}

int MsegCard::handleAtCardPoint(visage::Point point) const {
	if (leftOfEditorReach(point)) {
		return -1;
	}
	const auto [ux, uy] = toUnit(point);
	const auto [grabX, grabY] = grabRadii(editorBounds());
	return mseg::handleAt(layoutNow(), ux, uy, grabX, grabY, settings().bipolar);
}

mseg::CurveTarget MsegCard::curveTargetAtCardPoint(visage::Point point) const {
	const auto [ux, uy] = toUnit(point);
	const auto [grabX, grabY] = grabRadii(editorBounds());
	return mseg::curveTargetAt(layoutNow(), ux, uy, grabX, grabY, settings().bipolar);
}

mseg::CurveTarget MsegCard::highlightForTest() const {
	// THE DRAG WINS OVER THE HOVER, because during a drag the pointer wanders and the target does
	// not. A highlight that followed the pointer mid-gesture would light segments the drag is not
	// bending, which is worse than no highlight: it would be the picture disagreeing with the values.
	return curveTarget_.kind != mseg::CurveTarget::Kind::None ? curveTarget_ : hoverTarget_;
}

std::vector<mseg::Param> MsegCard::addressedParamsForTest() const {
	std::vector<mseg::Param> out;
	for (const mseg::Param param : layout()) {
		out.push_back(param);
	}
	for (const mseg::Param param : choices()) {
		out.push_back(param);
	}
	for (const Chip& chip : chips()) {
		out.push_back(chip.param);
	}

	// AND THE EDITOR'S OWN, one line per GESTURE rather than one per control, because the gesture is
	// the thing that either exists or does not. Each of these is exercised by a case that asserts the
	// value it writes — this list is a claim about which roads exist, and those cases are what stops
	// the claim being a wish.
	out.push_back(mseg::Param::StartLevel);     // handle 0, dragged vertically
	out.push_back(mseg::Param::SegmentCount);   // double-click, adding or removing a waypoint
	out.push_back(mseg::Param::LoopStart);      // the opening marker, dragged
	out.push_back(mseg::Param::LoopEnd);        // the closing marker, dragged
	out.push_back(mseg::Param::SustainSegment); // the sustain marker, dragged; kNoSustain at x = 0
	out.push_back(mseg::Param::Trig);           // its own pad (backlog MV)
	out.push_back(mseg::Param::NotesTrig);      // the same pad's right-click (backlog NU)
	for (size_t i = 0; i < mseg::kMaxSegments; ++i) {
		out.push_back(mseg::segmentParam(i, mseg::SegmentField::Time));  // a handle, dragged sideways
		out.push_back(mseg::segmentParam(i, mseg::SegmentField::Level)); // a handle, dragged up
		out.push_back(mseg::segmentParam(i, mseg::SegmentField::Shape)); // an Opt-drag
	}

	// SUSTAINSEGMENT USED TO BE DELIBERATELY ABSENT HERE, named in the reachability guard's own table
	// as "drawn as a marker, draggable by nothing — a real gap". It is a gesture now, and the gap
	// entry came out of that table in the same commit.
	// **AND THE ENABLE, WHICH THE SHARED HEAD DRAWS.** The card does not build a widget for it — the
	// square comes from `FxCard::drawEnableSwitch` and the press from `FxCard::enableBounds` — but a
	// hand reaches it here, so this list must say so. Declaring what is drawn is the whole contract
	// this seam has: the reachability sweep takes a surface at its word, and a card that drew a
	// control it did not declare is the shape that let shaper's pills pass as panel-only.
	if (hasEnable()) {
		out.push_back(static_cast<mseg::Param>(enableControl()));
	}
	return out;
}

bool MsegCard::chipLive(size_t index) const {
	if (index >= kChipCount) {
		return false;
	}
	const mseg::Settings s = settings();
	switch (chips()[index].param) {
	// AND NOT UNDER LFO RETRIG WITH THE NORMAL BROKEN (backlog NU), which is Free less the song lock: `mod::polyRuns`.
	case mseg::Param::Poly: return mod::polyRuns(s.mode, s.notesTrig);
	// CARRY CHANGES SOUND ONLY UNDER POLY (mono already smooths through), NOT IN FREE (which is mono whatever Poly
	// says), and only with some smoothing to carry — `mod::PolyShape::carrySmoothing`.
	case mseg::Param::SmoothCarry: return s.poly && mod::polyRuns(s.mode, s.notesTrig) && mod::smoothingTau(s.smoothing) > 0.0;
	default: return true;
	}
}

bool MsegCard::chipOn(size_t index) const {
	if (index >= kChipCount) {
		return false;
	}
	return mseg::valueOf(settings(), chips()[index].param) >= 0.5f;
}

std::string MsegCard::chipLabel(size_t index) const {
	if (index >= kChipCount) {
		return {};
	}
	return chips()[index].label;
}

void MsegCard::toggleChip(size_t index) {
	if (index >= kChipCount) {
		return;
	}
	const mseg::Param param = chips()[index].param;
	mseg::Settings s = settings();
	const mseg::Settings was = s;
	const double before = mseg::valueOf(s, param);
	// A SWITCH, and only a switch: a control with more than two states is a `choices()` box.
	mseg::setValueOf(&s, param, before >= 0.5 ? 0.0f : 1.0f);
	writeSettings( s);
	history_.record(std::make_unique<MsegShapeEdit>(host_, device_, was, s));
	redraw();
}

const std::string& MsegCard::boxCaption(size_t index) const {
	static const std::string kNone;
	return index < kBoxCount ? boxes_[index]->label() : kNone;
}

const visage::Frame* MsegCard::boxFrame(size_t index) const {
	return index < kBoxCount ? boxes_[index].get() : nullptr;
}

void MsegCard::writeSettings(const mod::mseg::Settings& settings) {
	// THE WHOLE ROW, THROUGH THE ENCODER THE DEVICE READS. `rowOf` is the one place that knows the
	// row's shape; writing fields by hand here would be a second.
	const mod::mseg::Row row = mod::mseg::rowOf(settings);
	// ONE USER ACTION, ONE HOST GESTURE per control it moves (host slots, T6b review).
	backplane::ControlGestureScope gesture(host_, device_);
	for (size_t i = 0; i < row.size(); ++i) {
		gesture.setField(i, row[i]);
	}
}

mod::Playhead MsegCard::playhead() const {
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase)) {
		return {};
	}
	return { ordinal, phase };
}

bool MsegCard::headSourceFocused() const {
	// FOCUSED IF ANY OF THIS DEVICE'S OUTPUTS IS THE FOCUSED SOURCE, not just its first — the card
	// belongs to the DEVICE, and output 0 stood in for it only while every device declared exactly one.
	const device::Descriptor* desc = descriptor();
	const size_t outputs = desc != nullptr ? desc->outputCount() : 0;
	for (size_t out = 0; out < outputs; ++out) {
		if (mod_.source() == sourceOfOutput(out)) {
			return true;
		}
	}
	return false;
}

bool MsegCard::headArmed() const { return headSourceFocused() && mod_.armed(); }

void MsegCard::foldedChanged() {
	for (auto& box : boxes_) {
		if (box != nullptr) {
			box->setVisible(!folded_);
		}
	}
	for (auto& box : choiceBoxes_) {
		if (box != nullptr) {
			box->setVisible(!folded_);
		}
	}
	if (trigPad_ != nullptr) {
		trigPad_->setVisible(!folded_);
	}
}

void MsegCard::drawContent(visage::Canvas& canvas) {
	// **NO FRAME, NO HEAD, NO NAME — `FxCard::draw` painted them before calling this.**
	// The body is clamped to its own rectangle, so this cannot reach the head even by
	// getting its own arithmetic wrong: the card cannot draw over the header, by coordinate
	// constraints.


	const visage::Bounds box = editorBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);

	// **THE GRID, UNDER EVERYTHING** — the same one the LFO's scope draws, from `plot_grid.h`. This
	// card had none at all: a shape with breakpoints at no stated time, over an empty box. The
	// verticals are also the lines a drag sticks to (see `onMouseDrag`), which is why they are worth
	// drawing here rather than only where a wave is.
	plot::drawGrid(canvas, box);

	const mseg::Settings s = settings();
	const mseg::Layout laid = layoutNow();

	// ZERO, WHEN THERE IS A BELOW-ZERO TO HAVE. A bipolar shape crossing the middle of an empty box
	// is ambiguous about which half is which, and "sits at zero" and "sits halfway up" are different
	// claims about what the modulation is doing.
	if (s.bipolar) {
		const visage::Point zero = fromUnit(0.0f, 0.0f);
		canvas.setColor(withAlpha(kLine, 0.55f));
		canvas.segment(box.x() + 2.0f, zero.y, box.right() - 2.0f, zero.y, 1.0f, true);
	}

	// THE LOOP'S EXTENT, UNDER THE CURVE. Two markers and the band between them — the thing a plain
	// looping flag cannot say, and the reason this device exists beside the envelope.
	if (laid.loopVisible) {
		const visage::Point from = fromUnit(laid.loopStartX, 0.0f);
		const visage::Point to = fromUnit(laid.loopEndX, 0.0f);
		canvas.setColor(withAlpha(kAccent, 0.12f));
		canvas.rectangle(from.x, box.y(), std::max(to.x - from.x, 1.0f), box.height());
		canvas.setColor(withAlpha(kAccent, 0.7f));
		canvas.segment(from.x, box.y(), from.x, box.bottom(), 1.0f, true);
		canvas.segment(to.x, box.y(), to.x, box.bottom(), 1.0f, true);
	}

	// THE AFFECTED SEGMENTS, WHILE EDITING. That phrase is the whole of the
	// disambiguation: one band for a segment target, two for a waypoint with a neighbour either side,
	// one for a waypoint at an end. Drawn over the loop band and under the curve, so the curve it is
	// about stays the brightest thing in the box.
	const mseg::CurveTarget highlight = highlightForTest();
	for (size_t k = 0; k < highlight.count; ++k) {
		const size_t seg = highlight.first + k;
		if (seg >= laid.segmentEnd.size()) {
			break;
		}
		const float leftX = seg == 0 ? 0.0f : laid.segmentEnd[seg - 1];
		const visage::Point from = fromUnit(leftX, 0.0f);
		const visage::Point to = fromUnit(laid.segmentEnd[seg], 0.0f);
		canvas.setColor(withAlpha(kAccent, 0.20f));
		canvas.rectangle(from.x, box.y(), std::max(to.x - from.x, 1.0f), box.height());
	}

	// THE SUSTAIN BREAKPOINT — where the gate holds, and the handle for choosing where.
	//
	// ABSENT ENTIRELY WHILE THE SHAPE LOOPS, because a looping shape does not hold: `sustainVisible`
	// is decided by the same `sustainInForce` the engine reads. DIM WHEN PARKED at the left wall,
	// because "no sustain point" is a state the drag has to be able to return from — an off control
	// still needs somewhere to be.
	if (laid.sustainVisible) {
		const visage::Point at = fromUnit(laid.sustainX, 0.0f);
		canvas.setColor(withAlpha(kValue, laid.sustainActive ? 0.5f : 0.18f));
		canvas.segment(at.x, box.y(), at.x, box.bottom(), 1.0f, true);
	}

	// THE SHAPE. **AND THE RIGHT EAR'S WHILE STEREO PHASE IS NOT 0** (backlog PA), in the stereo ring's two colours: the
	// right ear's line first, so the left one — the line the handles and the playhead star sit on — is on top.
	const std::vector<std::pair<float, float>> points = curve();
	const std::vector<std::pair<float, float>> right = curveRight();
	rightStrokes_ = 0;
	if (!right.empty()) {
		canvas.setColor(kModulationR);
		for (size_t i = 1; i < right.size(); ++i) {
			const visage::Point a = fromUnit(right[i - 1].first, right[i - 1].second);
			const visage::Point b = fromUnit(right[i].first, right[i].second);
			canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
			++rightStrokes_;
		}
	}
	canvas.setColor(right.empty() ? kValue : kModulationL);
	for (size_t i = 1; i < points.size(); ++i) {
		const visage::Point a = fromUnit(points[i - 1].first, points[i - 1].second);
		const visage::Point b = fromUnit(points[i].first, points[i].second);
		canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
	}

	// **AND WHERE THE SHAPE IS RIGHT NOW.** `celestial.h`'s `starGlyph` — the same glyph the automod
	// strip marks its phase with and the same one the perform stage draws a SOUND with. Borrowed
	// rather than redrawn: a second star would be two places that must agree about what a star looks
	// like. Drawn after the shape so the thing that MOVES is never under the thing that does not.
	if (const Marker mark = marker(); mark.drawn) {
		starGlyph(canvas, mark.at.x, mark.at.y, kPlayheadSize, kAccent, 1.0f);
	}

	// AND THE HIGHLIGHTED SEGMENTS' OWN LINE, RESTROKED IN THE ACCENT. The band says WHERE; this says
	// which curve the bend is about, which on a shape whose segments meet at a shallow angle is the
	// half a band alone cannot carry.
	for (size_t k = 0; k < highlight.count; ++k) {
		const size_t seg = highlight.first + k;
		if (seg >= laid.segmentEnd.size()) {
			break;
		}
		const float leftX = seg == 0 ? 0.0f : laid.segmentEnd[seg - 1];
		const float rightX = laid.segmentEnd[seg];
		canvas.setColor(kAccent);
		for (size_t i = 1; i < points.size(); ++i) {
			const float midX = 0.5f * (points[i - 1].first + points[i].first);
			if (midX < leftX || midX > rightX) {
				continue;
			}
			const visage::Point a = fromUnit(points[i - 1].first, points[i - 1].second);
			const visage::Point b = fromUnit(points[i].first, points[i].second);
			canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
		}
	}

	// AND THE HANDLES, WHICH ARE THE CONTROLS. Drawn last so they sit on top of the line they move.
	for (const mseg::Handle& h : laid.handle) {
		const visage::Point at = fromUnit(h.x, s.bipolar ? h.level * 2.0f - 1.0f : h.level);
		canvas.setColor(kInk);
		canvas.circle(at.x - 2.5f, at.y - 2.5f, 5.0f);
	}

	// THE CHIPS.
	for (size_t chip = 0; chip < kChipCount; ++chip) {
		const visage::Bounds b = chipBounds(chip);
		const bool on = chipOn(chip);
		// A DEAD CHIP (`chipLive`) IS DRAWN FAINT and does not take a press.
		const float live = chipLive(chip) ? 1.0f : 0.35f;
		canvas.setColor(on ? withAlpha(kAccent, 0.22f * live) : kVoid);
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
		canvas.setColor(on ? withAlpha(kAccent, live) : withAlpha(kLine, 0.8f * live));
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
		canvas.setColor(withAlpha(on ? kInk : kMuted, live));
		canvas.text(chipLabel(chip), font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y(),
		            b.width(), b.height());
	}
	// THE CHOICE'S CELL — see `drawChoiceCell`; the box inside draws the value.
	for (size_t choice = 0; choice < kChoiceCount; ++choice) {
		drawChoiceCell(canvas, choiceBounds(choice));
	}
}


const char* MsegCard::contentTooltipAt(visage::Point local) const {
	// THE CHIPS ARE DRAWN BY THIS CARD, not seated as widgets — the press road's own hit test.
	if (folded()) {
		return nullptr;
	}
	for (size_t chip = 0; chip < kChipCount; ++chip) {
		if (chipBounds(chip).contains(local)) {
			return controlTooltip(static_cast<size_t>(chips()[chip].param));
		}
	}
	return nullptr;
}

device::Modulatable* MsegCard::enableModulatable() {
	if (!hasEnable()) {
		return nullptr;
	}
	// BUILT ON FIRST ASK, and kept: a control holds the gesture state the history entry is
	// paired with, so a fresh one per press would open a gesture on one object and close it
	// on another.
	if (enableControl_ == nullptr) {
		enableControl_ = std::make_unique<DeviceControl>(host_, device_, mod::mseg::kDescriptor,
		                                                enableControl(), &mod_, &history_);
	}
	return enableControl_.get();
}

} // namespace feathers::ui
