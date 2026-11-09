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

#include "plugin/ui/env_card.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trace_marker.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace dsr = mod::dahdsr;

namespace {

/// HOW BIG THE PLAYHEAD'S GLYPH IS. The automod strip's number and its reasoning: `starGlyph` draws
/// a halo three times this and arms nearly twice it, so the real footprint is about 3.5x — and this
/// curve box is 44px tall, shorter than the strip's 62, so it has less room rather than more.
constexpr float kPlayheadSize = 3.0f;

/// The captions, in layout order. SINGLE LETTERS ON THE TOP ROW because A/D/S/R is a word every
/// player already knows and four of them have to fit in 46px each; spelled out on the second, where
/// three boxes have 63px and the names are less familiar.
constexpr std::array<const char*, EnvCard::kBoxCount> kCaptions {
	"A", "D", "S", "R", "Dly", "Hold", "Sm", "Amt",
};

/// The nominal rate the PREVIEW envelope runs at. It is only half the story — see the rate
/// multiplier in rebuildCurve, which is what actually decides how far a tick carries each stage.
constexpr double kPreviewRate = 6000.0;

/// HOW MANY SAMPLES EACH STAGE GETS, whatever its duration. Enough that a bent stage reads as a
/// curve; the polyline is resampled down to kPointsPerStage for drawing anyway.
constexpr double kTicksPerStage = 48.0;

/// How much of the curve's width the SUSTAIN plateau takes. Sustain is a level rather than a time,
/// so it has no duration to be proportional to — but a shape with no plateau reads as a decay
/// running straight into a release, which is not what an envelope with a sustain does.
constexpr float kSustainShare = 0.16f;

/// A stage's share of the width, from its position in its own declared range.
///
/// THE WIDTH OF A STAGE IS THE POSITION OF ITS CONTROL, which is the whole trick that makes this
/// axis work. Real time is unusable: the stages span 0.49 ms to 10 s, so a ten-second release beside
/// a ten-millisecond attack would give the attack a fifth of a pixel on a 198px card, and that is an
/// ordinary patch rather than a corner case. The declared range is already logarithmic, so a
/// normalised position IS a log-time measure — longer stages are always wider, every live stage is
/// visible, and dragging a time box widens its own segment in proportion. The picture and the
/// control agree by construction rather than by a mapping that could be wrong.
/// WHERE A STAGE SITS IN `stageEndX_`, and the one place that order is written.
///
/// It is the order the stages occur in, which is also the order stageAtX walks — so a stage's index
/// and its position on the card are the same fact. -1 for Idle, which is not a segment of anything.
int stageSlot(dsr::Stage stage) {
	switch (stage) {
	case dsr::Stage::Delay:
		return 0;
	case dsr::Stage::Attack:
		return 1;
	case dsr::Stage::Hold:
		return 2;
	case dsr::Stage::Decay:
		return 3;
	case dsr::Stage::Sustain:
		return 4;
	case dsr::Stage::Release:
		return 5;
	case dsr::Stage::Idle:
		break;
	}
	return -1;
}

/// The SMALLEST share a stage that actually happens is allowed to take, as a fraction of the whole
/// shape. Purely so a millisecond attack against a five-second decay is still a visible edge rather
/// than a column of pixels — see shareOf for why the axis is otherwise honest about time.
constexpr double kMinLiveShare = 0.04;

float shareOf(const dsr::Settings& s, dsr::Param param) {
	// A STAGE'S WIDTH IS ITS DURATION, IN SECONDS. It used to be the control's POSITION IN ITS RANGE,
	// which is log2 seconds — and that makes the x axis logarithmic in time, which is the one thing
	// the picture must not be.
	//
	// WHAT IT COST. With a=197ms d=240ms a loop period appeared to be about 10 seconds long — and 10 s is
	// exactly the top of a stage's range. On a log axis a 10 s hold draws only 1.67x the width of a
	// 197 ms attack, so a stage that owns 96% of the period looks like a slightly wide one and there
	// is nothing on the card to say otherwise. The whole point of drawing a shape is to see which
	// part of it takes the time.
	//
	// It is also the older defect — the drawn curve disagreeing with the actual mod output —
	// which was two bugs and this, and only two of them were in what the curve DID.
	//
	// The floor below is the one concession, and it is a concession about PIXELS rather than about
	// time: without it a 1 ms attack against a 5 s decay is 0.02% of the width and vanishes. Every
	// stage that happens gets at least a visible edge; a stage that does not happen still gets
	// nothing, which is the convention device.h states and what the case for it asserts.
	const double seconds = dsr::stageSeconds(dsr::valueOf(s, param));
	return seconds > 0.0 ? static_cast<float>(seconds) : 0.0f;
}

/// THE THREE CURVES, in the order the bend's undo record carries them.
constexpr std::array<dsr::Param, 3> kCurveParams { dsr::Param::AttackShape, dsr::Param::DecayShape,
	                                               dsr::Param::ReleaseShape };
/// The stage each of those bends, index for index.
constexpr std::array<dsr::Stage, 3> kCurveStages { dsr::Stage::Attack, dsr::Stage::Decay, dsr::Stage::Release };

/// ONE BEND, ONE UNDO STEP (backlog MN; the MSEG's `MsegShapeEdit`, and backlog JB's rule). The drag wrote the
/// curves with no record at all, so a bend could not be undone while every box beside it could. It carries the
/// three curves rather than the whole row, so undoing a bend cannot put back a time the hand changed since, and
/// it opens each control's gesture unconditionally for `DeviceValueEdit`'s reason: an undo is a hand.
class EnvBendEdit : public EditRecord {
public:
	EnvBendEdit(backplane::DeviceHost& host, DeviceInstanceId id, const dsr::Settings& before,
	            const dsr::Settings& after)
	    : host_(host), device_(id) {
		for (size_t i = 0; i < kCurveParams.size(); ++i) {
			before_[i] = dsr::valueOf(before, kCurveParams[i]);
			after_[i] = dsr::valueOf(after, kCurveParams[i]);
		}
	}

	void undo() const override { write(before_, after_); }
	void redo() const override { write(after_, before_); }
	const char* verb() const override { return "Curve"; }

private:
	/// Only the curves the bend moved — a peak moves two, a stage one.
	void write(const std::array<float, 3>& to, const std::array<float, 3>& from) const {
		for (size_t i = 0; i < kCurveParams.size(); ++i) {
			if (to[i] == from[i]) {
				continue;
			}
			const auto control = static_cast<size_t>(kCurveParams[i]);
			host_.beginControlGesture(device_, control);
			host_.setField(device_, control, to[i]);
			host_.endControlGesture(device_, control);
		}
	}

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	std::array<float, 3> before_ {};
	std::array<float, 3> after_ {};
};

} // namespace

const std::array<dsr::Param, EnvCard::kBoxCount>& EnvCard::layout() {
	// THE ROWS. The top four are the familiar envelope and nothing else; the bottom three are the
	// stages an ADSR does not have, plus the smoothing time, and all three default to out of the way.
	// AND THE AMOUNT (backlog DH), last on the bottom row — the row had three boxes at 63px and the top
	// four at 46px, so a fourth here costs no card height and no new widget; a bar beside the
	// curve was the fallback for a card with no room.
	static const std::array<dsr::Param, kBoxCount> kLayout {
		dsr::Param::Attack,    dsr::Param::Decay, dsr::Param::Sustain, dsr::Param::Release,
		dsr::Param::Delay,     dsr::Param::Hold,  dsr::Param::Smoothing, dsr::Param::Amount,
	};
	return kLayout;
}

// `~EnvCard` IS GONE. It unwatched this card and its four boxes by hand; `ui::Watch` holds those
// five subscriptions now and releases them, so there is nothing to remember and nothing to forget.

EnvCard::EnvCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history)
    : host_(host), device_(id), mod_(mod), history_(history),
      preview_(std::make_unique<dsr::Envelope>()) {
	preview_->prepare(kPreviewRate);

	for (size_t i = 0; i < kBoxCount; ++i) {
		// THE HISTORY IS FOR THE AMOUNT DRAG'S UNDO STEP, and only that — the value edits are recorded
		// by this card through ValueBox::onEdited, because an EditRecord outlives the widget that made
		// it. See RackControl's constructor.
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, dsr::kDescriptor,
		                                              static_cast<size_t>(layout()[i]), &mod_, &history_);
		boxes_[i] = std::make_unique<ValueBox>(*controls_[i], kCaptions[i]);
		addChild(boxes_[i].get());
		bindBox(i);
	}

	// THE TRIG PAD (backlog MV): the device's Trig control, and the fire count it lights from.
	trigControl_ = std::make_unique<DeviceControl>(host_, device_, dsr::kDescriptor,
	                                               static_cast<size_t>(dsr::Param::Trig), &mod_, &history_);
	trigPad_ = std::make_unique<TrigPad>(*trigControl_, [this](uint32_t& fires) {
		return host_.trigFiresAt(device_, fires);
	});
	// AND ITS NORMAL (backlog NU): the right-click's "Notes fire Trig", and the dot that shows it made.
	notesTrigControl_ = std::make_unique<DeviceControl>(host_, device_, dsr::kDescriptor,
	                                                    static_cast<size_t>(dsr::Param::NotesTrig), &mod_, &history_);
	trigPad_->setNormal({ notesTrigControl_.get() });
	addChild(trigPad_.get());

	// THE BOXES' BORDERS ARE THE MAPPING SIGNAL, SO EACH BOX FOLLOWS THE ARMED FLAG ITSELF.
	//
	// This line used to watch the CARD alone and say the same sentence, on the assumption that
	// repainting a card repaints the boxes inside it. visage does not promise that — redraw() marks
	// one frame, and the note in CLAUDE.md about repaints being unobservable is the same fact from
	// the other side. Knob got this right (knob.cpp watches per widget); the value box was written
	// later and inherited the assumption instead of the pattern.
	//
	// THE DEFECT: the param boxes did not show the 'modulable' glow. The glow was computed
	// correctly the whole time — display() reported armed and isDestination — and the box simply was
	// not asked to draw again when the flag moved.
	watches_.push_back(mod_.watch(this));
	for (size_t i = 0; i < kBoxCount; ++i) {
		watches_.push_back(mod_.watch(boxes_[i].get()));
	}
	watches_.push_back(mod_.watch(trigPad_.get())); // its ring is the mapping signal too

	// THE CHOICE — compact, uncaptioned, in the chip row. Its menu is the box's own, from the
	// descriptor; the box subscribes itself to the armed flag (`ValueBox::refreshWatch`).
	for (size_t i = 0; i < kChoiceCount; ++i) {
		choiceControls_[i] = std::make_unique<DeviceControl>(host_, device_, dsr::kDescriptor,
		                                                    static_cast<size_t>(choices()[i]), &mod_,
		                                                    &history_);
		choiceBoxes_[i] = std::make_unique<ValueBox>(*choiceControls_[i], "");
		choiceBoxes_[i]->setCompact(true);
		// WHAT THE MODE CHIP'S PRESS DID AFTER ITS WRITE, now done after the pick: the curve is a
		// function of the mode, and so is the Sustain box's caption (`followMode`).
		choiceBoxes_[i]->onValueChanged([this]() {
			haveCurve_ = false;
			followMode();
			redraw();
		});
		addChild(choiceBoxes_[i].get());
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **THE HEAD'S MARKS FIRST, BECAUSE THIS CARD IS DRAWN OVER THEM.** The unified frame put the
		// card's rectangle across the whole head, and a child frame takes the press — the exact bug
		// this card's own note records from a first version. Handed back rather than hit-tested
		// through; see `FxCard::headMarkPressed`.
		// **THROUGH THE SHARED GESTURE, NOT PAST IT.** This used to call `headMarkPressed` directly and
		// return; `noteCardPress` now dispatches that mark itself, and — since the `(-)` rule was retired
		// (the double-click fold replaces it) — a head press is also
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
		// top.** The defect: disable on the MSEG did nothing, and then, after a first
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
		// A DRAG ON THE CURVE BENDS THE STAGE UNDER THE POINTER — the curve is draggable, which is the
		// reason the three shape controls need no boxes. Which stage is decided
		// by x, because that is what the eye is already using to tell the segments apart. With Opt on the
		// peak it bends Attack and Decay together, the MSEG's waypoint gesture (backlog MN) — see
		// `bendTargetAt`, which also says why a press over a flat stage catches nothing.
		bend_ = bendTargetAt(e.position, e.isAltDown());
		if (bend_.kind == BendTarget::Kind::None) {
			return;
		}
		bendBefore_ = settings();
		bendAnchorY_ = e.position.y;
		redraw(); // the highlight comes on with the press
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		// **CHAINED FIRST.** `FxCard::noteCardMove` is what reports `onCardDrag`, which is how a host
		// reorders — and this handler returns early on exactly the path a head drag takes. Calling it
		// after the return would have been calling it never, which is why reorder did nothing here.
		noteCardMove(e);
		if (bend_.kind == BendTarget::Kind::None) {
			return;
		}
		// Up is fuller, which is what a positive shape means on every stage — see kShapeMax. The
		// travel is the curve's own height, so the gesture is bounded by the thing being dragged.
		// FROM THE ANCHOR, the MSEG's rule: the bend is where the pointer is, not a sum of moves.
		const float span = std::max(curveBounds().height() * kBendTravelForFullBend, 1.0f);
		const float delta = (bendAnchorY_ - e.position.y) / span * dsr::kShapeMax;
		const dsr::Settings bent = withBend(bendBefore_, bend_, delta);
		// ONLY THE CURVES THE BEND OWNS go out, onto the settings as they are NOW — anything else that moved
		// during the drag (a time box under automation, say) is not put back to what it was at the press.
		dsr::Settings settings = this->settings();
		for (size_t i = 0; i < kCurveParams.size(); ++i) {
			if (bend_.bends(kCurveStages[i])) {
				dsr::setValueOf(&settings, kCurveParams[i], dsr::valueOf(bent, kCurveParams[i]));
			}
		}
		writeSettings(settings);
		haveCurve_ = false;
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent& e) {
		if (bend_.kind != BendTarget::Kind::None) {
			bend_ = {};
			// ONE STEP PER BEND, and none for a press that moved nothing — the knob's rule and the MSEG's.
			const dsr::Settings after = settings();
			const bool moved = std::any_of(kCurveParams.begin(), kCurveParams.end(), [&](dsr::Param curve) {
				return dsr::valueOf(after, curve) != dsr::valueOf(bendBefore_, curve);
			});
			if (moved) {
				history_.record(std::make_unique<EnvBendEdit>(host_, device_, bendBefore_, after));
			}
			redraw(); // the highlight goes out with the gesture
		}
		// **CHAINED, NOT REPLACING.** `FxCard::noteCardRelease` carries the drag anchor, `armed_`, and
		// the FOLD — assigning over it took the whole seat and left this card unable to collapse once
		// the press-time fold road was retired.
		noteCardRelease(e);
	};

	// THE HOVER HALF OF THE HIGHLIGHT, the MSEG's: with Opt held, what a press would catch is lit before the
	// button goes down, which is how a hand tells the peak from the attack beside it. A modifier pressed
	// without moving the mouse lights nothing — visage reports modifiers on mouse events (`MsegCard`'s note).
	const auto hover = [this](const visage::MouseEvent& e) {
		const BendTarget found = e.isAltDown() ? bendTargetAt(e.position, true) : BendTarget {};
		if (!(found == hover_)) {
			hover_ = found;
			redraw();
		}
	};
	onMouseMove() = hover;
	onMouseEnter() = hover;
	onMouseExit() = [this](const visage::MouseEvent&) {
		if (hover_.kind != BendTarget::Kind::None) {
			hover_ = {};
			redraw();
		}
	};
}

EnvCard::BendTarget EnvCard::bendTargetAt(visage::Point point, bool opt) const {
	const visage::Bounds curve = curveBounds();
	if (!curve.contains(point)) {
		return {};
	}
	(void)curvePoints(); // the spans are the curve's, rebuilt with it
	const float width = std::max(1.0f, curve.width());
	const float height = std::max(1.0f, curve.height());
	const float ux = (point.x - curve.x()) / width;
	const float uy = (curve.bottom() - point.y) / height;

	const auto present = [this](dsr::Stage stage) {
		return std::any_of(spans_.begin(), spans_.end(), [stage](const Span& sp) { return sp.stage == stage; });
	};

	if (opt) {
		// THE PEAK, ON THE MSEG'S RADII: the top of the box (the envelope's 1, wherever the polarity puts it), at
		// the end of an attack or the start of a decay. A looping shape draws two cycles and has two peaks.
		const float peakY = plotUnit(1.0f, settings().bipolar);
		const float radiusX = kBendGrabPixels / width;
		const float radiusY = kBendGrabPixels / height;
		bool onPeak = false;
		if (std::abs(uy - peakY) <= radiusY) {
			for (const Span& sp : spans_) {
				onPeak = onPeak || (sp.stage == dsr::Stage::Attack && std::abs(sp.to - ux) <= radiusX)
				         || (sp.stage == dsr::Stage::Decay && std::abs(sp.from - ux) <= radiusX);
			}
		}
		if (onPeak) {
			// ONLY THE SIDES THAT ARE THERE, as the MSEG's end waypoint bends its one neighbour: a skipped stage's
			// curve is not on the picture, and bending it would be an edit nobody can see.
			BendTarget peak { BendTarget::Kind::Peak, 0 };
			for (const dsr::Stage side : { dsr::Stage::Attack, dsr::Stage::Decay }) {
				if (present(side)) {
					peak.stages |= BendTarget::bit(side);
				}
			}
			return peak;
		}
	}

	for (const Span& sp : spans_) {
		if (ux <= sp.to) {
			switch (sp.stage) {
			case dsr::Stage::Attack:
			case dsr::Stage::Decay:
			case dsr::Stage::Release:
				return { BendTarget::Kind::Stage, BendTarget::bit(sp.stage) };
			default:
				// Delay, hold and sustain are FLAT. There is no curve to bend, so the press does nothing
				// rather than bending a neighbour — a drag that silently edited the stage next door would
				// be worse than one that does nothing.
				return {};
			}
		}
	}
	return {};
}

dsr::Settings EnvCard::withBend(const dsr::Settings& settings, const BendTarget& target, float delta) {
	dsr::Settings s = settings;
	const auto bend = [&](dsr::Stage stage, float& shape) {
		if (target.bends(stage)) {
			shape = std::clamp(shape + delta, -dsr::kShapeMax, dsr::kShapeMax);
		}
	};
	bend(dsr::Stage::Attack, s.attackShape);
	bend(dsr::Stage::Decay, s.decayShape);
	bend(dsr::Stage::Release, s.releaseShape);
	return s;
}

EnvCard::BendTarget EnvCard::highlightForTest() const {
	// THE BEND IN PROGRESS WINS, as the MSEG's does: a highlight that followed the pointer mid-drag would light
	// what the drag is not bending.
	return bend_.kind != BendTarget::Kind::None ? bend_ : hover_;
}

std::vector<dsr::Param> EnvCard::addressedParamsForTest() const {
	std::vector<dsr::Param> out;
	for (const dsr::Param param : layout()) {
		out.push_back(param);
	}
	for (const dsr::Param param : choices()) {
		out.push_back(param);
	}
	for (const Chip& chip : chips()) {
		out.push_back(chip.param);
	}
	// AND THE THREE THE CURVE REACHES, which have no boxes on purpose — the curve itself is draggable.
	// A drag on a stage bends that stage; see `bendTargetAt`, which is the only
	// road to any of them.
	out.push_back(dsr::Param::AttackShape);
	out.push_back(dsr::Param::DecayShape);
	out.push_back(dsr::Param::ReleaseShape);
	// AND THE TRIG, ON ITS OWN PAD (backlog MV).
	out.push_back(dsr::Param::Trig);
	// AND ITS NORMAL, ON THE SAME PAD'S RIGHT-CLICK (backlog NU).
	out.push_back(dsr::Param::NotesTrig);
	// **AND THE ENABLE, WHICH THE SHARED HEAD DRAWS.** The card does not build a widget for it — the
	// square comes from `FxCard::drawEnableSwitch` and the press from `FxCard::enableBounds` — but a
	// hand reaches it here, so this list must say so. Declaring what is drawn is the whole contract
	// this seam has: the reachability sweep takes a surface at its word, and a card that drew a
	// control it did not declare is the shape that let shaper's pills pass as panel-only.
	if (hasEnable()) {
		out.push_back(static_cast<dsr::Param>(enableControl()));
	}
	return out;
}

const std::array<EnvCard::Chip, EnvCard::kChipCount>& EnvCard::chips() {
	// THE SWITCHES, as chips rather than as boxes. A switch has no value to print and no range to
	// drag, so a value box would be a box whose number never changed — and four more of those would
	// have taken a third row for controls that are on or off.
	//
	// MODE IS A CHOICE RATHER THAN A SWITCH and rode here, cycling through its five — so reaching
	// Once from Loop applied Free on the way. It is a menu box in the same cell now: see `choices()`
	// (backlog GG: a multivalue toggle that cycles on click is an antipattern, because a control must
	// show what is being chosen before the choice is applied).
	static const std::array<Chip, kChipCount> kChips {
		// **LEGATO CAME BACK, BECAUSE MODE DID NOT SWALLOW IT.** The collapse folded `Loop` into the
		// five-way dropdown and rightly deleted its chip; `Leg` was standing beside it in this array
		// and went with it, and nothing folded. `Settings::singleTrigger` is still its own field,
		// `"Env Legato"` is still in `kParamDescs`, and `dahdsr.cpp` still reads it twice — in
		// `mayRetrigger`'s first term and in the note-on site's legato take-up. With no chip there was
		// no writer at all: not the card, not `src/plugin`, not `src/rack`, and `modulatable: false`
		// keeps it off every route. The engine could do it and nobody could ask for it, which is the
		// honesty rule read backwards.
		//
		// CALLED "LEGATO" RATHER THAN "1 TRIG" — a legato toggle that does not retrigger the envelope on
		// a new note is the player's word for it; Bitwig's "Single Trigger" names the same thing less
		// directly, so "Leg" is what belongs on a 36px chip.
		Chip { dsr::Param::SingleTrigger, "Leg" },
		Chip { dsr::Param::Bipolar, "±" },
		// **POLY — one shape per note, or one for the device**. A switch, so a chip: two states,
		// both visible by being the only two.
		Chip { dsr::Param::Poly, "Poly" },
		// **SMOOTH THROUGH RETRIGGER** (backlog MM): a new note's smoothing starts where the last note's output stood.
		// Live only where it changes something — see `chipLive`.
		Chip { dsr::Param::SmoothCarry, "Carry" },
		// "Sm" IS GONE WITH THE PARAM BEHIND IT: no smoothing enable, just Smooth = 0. Turning Smooth up
		// while its enable was off did nothing and was confusing. A chip that decides whether the box
		// next to it does anything is a chip that can make
		// the box lie, and this one did.
		// MODE IS NOT A CHIP ANY MORE — see `choices()` (backlog GG). A chip is a switch.
	};
	return kChips;
}

const std::array<dsr::Param, EnvCard::kChoiceCount>& EnvCard::choices() {
	// THE FULL DECLARED NAMES FIT NOW. The chip shortened "LFO Retrig" to "LFO" for a 36px chip; the
	// cell is a third of the row, and the fit case in `test_gui_widgets_devices` measures every name
	// against the box's readout width rather than trusting this sentence.
	static const std::array<dsr::Param, kChoiceCount> k { dsr::Param::Mode };
	return k;
}

bool EnvCard::chipLive(size_t index) const {
	if (index >= kChipCount) {
		return false;
	}
	const dsr::Settings s = settings();
	switch (chips()[index].param) {
	// AND NOT UNDER LFO RETRIG WITH THE NORMAL BROKEN (backlog NU), which is Free less the song lock: `mod::polyRuns`.
	case dsr::Param::Poly: return mod::polyRuns(s.mode, s.notesTrig);
	// CARRY CHANGES SOUND ONLY UNDER POLY (mono already smooths through), NOT IN FREE (which is mono whatever Poly
	// says), and only with some smoothing to carry — `mod::PolyShape::carrySmoothing`. Legato's take-up of a held
	// note wins over it, but a Legato note struck with nothing held starts fresh and carries, so Leg does not gate it.
	case dsr::Param::SmoothCarry: return s.poly && mod::polyRuns(s.mode, s.notesTrig) && mod::smoothingTau(s.smoothing) > 0.0;
	default: return true;
	}
}

bool EnvCard::chipOn(size_t index) const {
	if (index >= kChipCount) {
		return false;
	}
	return dsr::valueOf(settings(), chips()[index].param) >= 0.5f;
}

std::string EnvCard::chipLabel(size_t index) const {
	if (index >= kChipCount) {
		return {};
	}
	return chips()[index].label;
}

void EnvCard::toggleChip(size_t index) {
	if (index >= kChipCount) {
		return;
	}
	const dsr::Param param = chips()[index].param;
	dsr::Settings settings = this->settings();
	const double before = dsr::valueOf(settings, param);
	// A SWITCH, and only a switch: a control with more than two states is a `choices()` box.
	dsr::setValueOf(&settings, param, before >= 0.5 ? 0.0f : 1.0f);
	writeSettings(settings);
	history_.record(std::make_unique<DeviceValueEdit>(host_, device_, static_cast<size_t>(param), before,
	                                                dsr::valueOf(settings, param)));
	haveCurve_ = false;
	followMode();
	redraw();
}

void EnvCard::bindBox(size_t index) {
	ValueBox* box = boxes_[index].get();

	// The curve is a function of the settings, so a box that moved one has invalidated it. Redrawing
	// the whole card rather than the box alone is what keeps the picture and the number in the SAME
	// frame — a curve that lagged its own control by a frame is the flicker every bridge fix in this
	// project has been about.
	box->onValueChanged([this]() {
		haveCurve_ = false;
		redraw();
	});

	// **THE CONTROL RECORDS THE UNDO STEP NOW, AND THE CARD MUST NOT ALSO DO IT.**
	//
	// This used to read "THE CARD RECORDS THE UNDO STEP, NOT THE BOX", and that was true of
	// `RackControl`, which recorded only ROUTE-amount gestures and left values to its owner.
	// `DeviceControl::endGesture` records a `DeviceValueEdit` itself — so after the port BOTH were
	// recording and one drag became TWO undo steps, caught as `CHECK( 2 == 1 )`.
	//
	// A hand-written note that a mechanical port left standing: the old comment was still accurate
	// about the OLD control while describing the wrong one. The box's value road is the control's
	// business; what remains the card's is the chip toggle above, which writes settings DIRECTLY and
	// so passes through no control at all.
	box->onEdited([this](double, double) {
		haveCurve_ = false;
		redraw();
	});
}

visage::Bounds EnvCard::curveBounds() const {
	return { bodyLeft() + kPadX + kTrigColumn, bodyTop() + kGap, bodyWidth() - 2.0f * kPadX - kTrigColumn,
		     kCurveHeight };
}

visage::Bounds EnvCard::trigPadBounds() const {
	const visage::Bounds curve = curveBounds();
	return { bodyLeft() + kPadX, curve.y() + (curve.height() - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
		     TrigPad::kDiameter };
}

visage::Bounds EnvCard::boxBounds(size_t index) const {
	if (index >= kBoxCount) {
		return {};
	}
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float top = bodyTop() + kGap + kCurveHeight + kGap;
	const bool bottomRow = index >= kTopRow;
	const size_t columns = bottomRow ? kBottomRow : kTopRow;
	const size_t column = bottomRow ? index - kTopRow : index;

	// EVENLY DIVIDED, GAPS INCLUDED, rather than a fixed width per box — the two rows have different
	// column counts, so a shared constant would leave one row short of the card's edge and the eye
	// reads that as a mistake rather than as a layout.
	const float gap = kRowGap;
	const float boxWidth = (usable - gap * static_cast<float>(columns - 1))
	                       / static_cast<float>(columns);
	const float y = top + (bottomRow ? ValueBox::kHeight + kRowGap : 0.0f);
	return { bodyLeft() + kPadX + static_cast<float>(column) * (boxWidth + gap), y, boxWidth,
		     ValueBox::kHeight };
}

visage::Bounds EnvCard::cellBounds(size_t cell) const {
	// **A CHOICE CELL IS TWO UNITS WIDE AND A CHIP ONE** — the LFO card's rule, taken when Poly made this row three
	// chips and a choice: four equal quarters left the Mode box 29.5px of text against "LFO Retrig"'s 44 (the fit
	// case in `test_gui_widgets_devices` measures it). The chips come first here.
	constexpr size_t kCells = kChipCount + kChoiceCount;
	if (cell >= kCells) {
		return {};
	}
	// **AND A CHIP THREE QUARTERS OF ONE** since Carry made the chips four (backlog MM): at a whole unit each the
	// Mode box lost the width "LFO Retrig" needs. Four chips at three quarters take the room three took at one.
	constexpr float kChipUnits = 0.75f;
	constexpr float kUnits = kChipUnits * static_cast<float>(kChipCount) + 2.0f * static_cast<float>(kChoiceCount);
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float gap = kRowGap;
	const float unit = (usable - gap * (kCells - 1)) / kUnits;
	float x = bodyLeft() + kPadX;
	for (size_t before = 0; before < cell; ++before) {
		x += (before < kChipCount ? kChipUnits : 2.0f) * unit + gap;
	}
	const float y = bodyTop() + kGap + kCurveHeight + kGap + 2.0f * ValueBox::kHeight + kRowGap
	                + kGap;
	return { x, y, (cell < kChipCount ? kChipUnits : 2.0f) * unit, kChipHeight };
}

visage::Bounds EnvCard::chipBounds(size_t index) const {
	return index < kChipCount ? cellBounds(index) : visage::Bounds {};
}

visage::Bounds EnvCard::choiceBounds(size_t index) const {
	return index < kChoiceCount ? cellBounds(kChipCount + index) : visage::Bounds {};
}

void EnvCard::resized() {
	for (size_t i = 0; i < kBoxCount; ++i) {
		const visage::Bounds b = boxBounds(i);
		boxes_[i]->setBounds(b.x(), b.y(), b.width(), b.height());
	}
	for (size_t i = 0; i < kChoiceCount; ++i) {
		seatInChipCell(*choiceBoxes_[i], choiceBounds(i));
	}
	const visage::Bounds trig = trigPadBounds();
	trigPad_->setBounds(trig.x(), trig.y(), trig.width(), trig.height());
	haveCurve_ = false;
}

float EnvCard::stageEndX(dsr::Stage stage) const {
	// Rebuilt on demand for the same reason curvePoints does it: the shares are a function of the
	// settings, and a caller asking about geometry should not have to have painted first.
	curvePoints();
	static constexpr std::array<dsr::Stage, 6> kOrder { dsr::Stage::Delay,   dsr::Stage::Attack,
		                                               dsr::Stage::Hold,    dsr::Stage::Decay,
		                                               dsr::Stage::Sustain, dsr::Stage::Release };
	for (size_t i = 0; i < kOrder.size(); ++i) {
		if (kOrder[i] == stage) {
			return stageEndX_[i];
		}
	}
	return 0.0f;
}

mod::dahdsr::Stage EnvCard::stageAtX(float x) const {
	// stageEndX_ is filled by rebuildCurve, in stage order, as cumulative fractions across the width.
	static constexpr std::array<dsr::Stage, 6> kOrder { dsr::Stage::Delay,  dsr::Stage::Attack,
		                                               dsr::Stage::Hold,   dsr::Stage::Decay,
		                                               dsr::Stage::Sustain, dsr::Stage::Release };
	for (size_t i = 0; i < kOrder.size(); ++i) {
		if (x <= stageEndX_[i]) {
			return kOrder[i];
		}
	}
	return dsr::Stage::Release;
}

void EnvCard::rebuildCurve() {
	const dsr::Settings s = settings();

	// THE SHAPE IS A LIST OF SEGMENTS, and a LOOPING envelope is a different list rather than the same
	// list drawn differently.
	//
	// THE DEFECT: looping appeared not to work, and the card drew a wrong curve. The engine was looping
	// correctly the whole time — measured, holding a note: the value cycles 0.55 → 1.0 → 0.55 for as
	// long as the gate is up. What was wrong was the PICTURE. The card built one fixed run —
	// delay, attack, hold, decay, a sustain plateau, release — and dropped the gate the moment it saw
	// sustain, so a looping envelope drew EXACTLY the same curve as a non-looping one. Toggling Loop
	// changed nothing on screen, which reads as the feature being dead.
	//
	// So: while it loops there is no rest and no release to draw — there is a CYCLE. Two of them, so
	// the picture says "this repeats" rather than leaving the eye to infer it from one.
	struct Segment {
		dsr::Stage stage;
		float share;
	};
	std::vector<Segment> segments;

	const auto pushCycle = [&]() {
		segments.push_back({ dsr::Stage::Delay, shareOf(s, dsr::Param::Delay) });
		segments.push_back({ dsr::Stage::Attack, shareOf(s, dsr::Param::Attack) });
		segments.push_back({ dsr::Stage::Hold, shareOf(s, dsr::Param::Hold) });
		segments.push_back({ dsr::Stage::Decay, shareOf(s, dsr::Param::Decay) });
	};

	pushCycle();
	if (s.mode == dsr::Mode::Loop || s.mode == dsr::Mode::Free || s.mode == dsr::Mode::LfoRetrig) {
		// A SECOND CYCLE AND NOTHING ELSE. Sustain is not a rest while looping — it is the floor the
		// cycle falls to and turns at — and the release does not happen until the gate drops, which is
		// not what this picture is about.
		pushCycle();
	}
	else {
		// SUSTAIN HAS NO DURATION, so its width cannot come from one. It takes a fixed FRACTION of
		// the shape, which now means a fraction of the seconds the timed stages add up to rather than
		// a share among stages each measured 0..1.
		float timed = 0.0f;
		for (const Segment& seg : segments) {
			timed += seg.share;
		}
		segments.push_back({ dsr::Stage::Sustain,
		                     std::max(timed, 0.001f) * kSustainShare / (1.0f - kSustainShare) });
		segments.push_back({ dsr::Stage::Release, shareOf(s, dsr::Param::Release) });
	}

	// A SEGMENT WITH NO WIDTH IS NOT A SEGMENT — dropped here, before the run, rather than skipped
	// while drawing.
	//
	// THIS IS THE SAME BUG ONE LAYER UP, and it cost a second round of testing. A zero-width
	// segment is a stage the envelope steps straight past, so the run would sit on it with a rate
	// multiplier of 1 — computed from a duration of zero — and the NEXT stage would complete inside
	// that single tick at full speed. With the shipping default, delay is off, so segment 0 had no
	// duration, the 10 ms attack finished in the one tick spent on it, and THE ATTACK SEGMENT GOT NO
	// SAMPLES AT ALL. The curve ran from the origin straight to the first decay point, exactly as it
	// did the first time, for a different reason.
	//
	// Removing them makes the first segment always a real one, which is what the rate multiplier
	// needs to be meaningful.
	segments.erase(std::remove_if(segments.begin(), segments.end(),
	                              [](const Segment& seg) { return !(seg.share > 0.0f); }),
	               segments.end());

	float total = 0.0f;
	for (const Segment& seg : segments) {
		total += seg.share;
	}
	// THE VISIBILITY FLOOR, applied now because it is a fraction of the WHOLE and the whole is only
	// known here. Widening a stage widens the total, so this is iterated to a fixed point rather than
	// applied once — one pass would leave the last stage raised against a total that then grew.
	for (int pass = 0; pass < 4 && total > 0.0f; ++pass) {
		bool raised = false;
		for (Segment& seg : segments) {
			const float floorShare = static_cast<float>(kMinLiveShare) * total;
			if (seg.share > 0.0f && seg.share < floorShare) {
				total += floorShare - seg.share;
				seg.share = floorShare;
				raised = true;
			}
		}
		if (!raised) {
			break;
		}
	}
	if (!(total > 0.0f)) {
		// Every stage skipped and nothing to rest at: a vertical edge is the honest picture.
		segments.assign(1, { dsr::Stage::Sustain, 1.0f });
		total = 1.0f;
	}

	// Cumulative right-hand edge of each segment, in 0..1 across.
	std::vector<float> endX(segments.size(), 0.0f);
	float at = 0.0f;
	for (size_t i = 0; i < segments.size(); ++i) {
		at += segments[i].share / total;
		endX[i] = at;
	}

	// EVERY SEGMENT'S SPAN, both cycles of a loop included — what the bend hit-tests and lights (backlog MN).
	spans_.clear();
	for (size_t i = 0; i < segments.size(); ++i) {
		spans_.push_back({ segments[i].stage, i == 0 ? 0.0f : endX[i - 1], endX[i] });
	}

	// stageEndX_ still answers per STAGE, for the cases and the marker. With two cycles a stage ends
	// twice; the LAST one is the answer, because that is where its region of the picture stops.
	stageEndX_.fill(0.0f);
	for (size_t i = 0; i < segments.size(); ++i) {
		const int slot = stageSlot(segments[i].stage);
		if (slot >= 0) {
			stageEndX_[static_cast<size_t>(slot)] = endX[i];
		}
	}
	// A stage with no segment of its own inherits its predecessor's edge, so stageAtX stays monotone
	// and a zero-width stage cannot swallow a coordinate.
	for (size_t i = 1; i < stageEndX_.size(); ++i) {
		stageEndX_[i] = std::max(stageEndX_[i], stageEndX_[i - 1]);
	}

	const auto secondsOf = [&s](dsr::Stage st) -> double {
		switch (st) {
		case dsr::Stage::Delay:
			return dsr::stageSeconds(s.delay);
		case dsr::Stage::Attack:
			return dsr::stageSeconds(s.attack);
		case dsr::Stage::Hold:
			return dsr::stageSeconds(s.hold);
		case dsr::Stage::Decay:
			return dsr::stageSeconds(s.decay);
		case dsr::Stage::Release:
			return dsr::stageSeconds(s.release);
		default:
			return 0.0;
		}
	};

	// RUN THE REAL ENVELOPE — A FIXED BUDGET OF TICKS PER SEGMENT, and no reading of which stage it
	// says it is in.
	//
	// THE SCHEME GOES RATHER THAN GETS A THIRD PATCH. Advancing on an observed stage change asks the
	// envelope a question it cannot always answer. At the seam of a loop it runs decay -> sustain ->
	// attack -> decay, and the single tick that crosses the seam still carries the OUTGOING segment's
	// rate multiplier — so whenever the attack is short against the decay, the whole attack completes
	// inside that tick and the stage reads Decay both before and after. Nothing appears to have
	// happened, the run sits on that segment until the tick cap, and every later segment gets no
	// samples at all — so depending on the time settings the curve spanned either the whole x domain
	// or only its first half. Measured, at a 1 ms attack against a 5 s decay: 49 points
	// ending at x=0.5, exactly one cycle of two.
	//
	// That was the third bug of this shape in one card — the first tick read as Idle, then the
	// zero-width segment, now the seam — and all three are the same mistake, which is inferring where
	// the run has got to from what the envelope reports rather than from what was asked of it.
	//
	// A BUDGET CANNOT FAIL THAT WAY, because it does not depend on anything the envelope reports. Nor
	// is it an approximation: rateMul is CHOSEN so a stage advances by exactly 1/kTicksPerStage per
	// tick, so kTicksPerStage ticks is precisely what that stage takes. And the picture's widths come
	// from the controls, never from the sample count, so a fixed budget per segment is only the
	// resampling grid — it makes no claim about duration.
	// **THE PREVIEW RUNS WITH THE SMOOTHING OFF — the picture is the SHAPE, not the route** (the defect:
	// R sometimes drew as off when it was not, and sustain as 100% when it was not).
	// It drew `tick()`'s return, which is the SMOOTHED value, and this preview gives every stage the same 48
	// ticks whatever its length, so a one-pole of real seconds applied per preview tick smeared the picture
	// by an amount that belongs to neither: MEASURED at 250 ms smoothing, attack 10 ms, sustain 0.3, release
	// 500 ms — peak drawn at 0.72, the plateau 0.22 above the sustain, the release ending at 0.13 rather than
	// the floor; at 1 s, a peak of 0.38. "Sometimes" was whenever the Smoothing box was off its bottom.
	// `Envelope::rawValue`'s own contract says what a card draws; running the same envelope with the smoothing
	// at OFF is that, with the envelope's own bipolar rescale kept. `curveFrom_` still records `s`, so moving
	// the smoothing still rebuilds (to the same picture).
	dsr::Settings shape = s;
	shape.smoothing = mod::kSmoothingLog2SecondsMin;
	preview_->reset();
	std::vector<std::vector<float>> samples(segments.size());
	const double tickSeconds = static_cast<double>(dsr::kBlockSamples) / kPreviewRate;
	bool gate = true;

	for (size_t seg = 0; seg < segments.size(); ++seg) {
		const double seconds = secondsOf(segments[seg].stage);
		const float rateMul =
		    seconds > 0.0 ? static_cast<float>(seconds / (kTicksPerStage * tickSeconds)) : 1.0f;

		// THE GATE DROPS WHERE THE RELEASE BEGINS, which is the only place it can go: a release is what
		// the envelope does when the note is let go, so drawing one with the gate still up would be
		// drawing a stage that does not run. A looping shape has no release segment and never gets here,
		// which is the whole of the difference.
		if (segments[seg].stage == dsr::Stage::Release) {
			gate = false;
		}

		samples[seg].reserve(static_cast<size_t>(kTicksPerStage));
		for (int t = 0; t < static_cast<int>(kTicksPerStage); ++t) {
			// UNKNOWN, ON PURPOSE: the DAHDSR reads the clock only for Free on a playing transport (song-locked
			// by design), and a picture drawn from a preview should not depend on the song in any case —
			// the widths come from the controls, so the preview always runs the stopped machine.
			samples[seg].push_back(preview_->tick(shape, gate, false, mod::TransportClock::unknown(), rateMul));
		}
	}

	// RESAMPLED INTO EACH SEGMENT'S OWN WIDTH. A segment with no samples — one the shape declared and
	// the envelope skipped — takes no width either, so the polyline steps over it.
	curve_.clear();
	// SEEDED AT THE VALUE THE ENVELOPE RESTS AT, which is not always zero. dahdsr.cpp states the
	// convention where it applies it: a bipolar envelope is 2v-1, so idle reads -1 rather than 0. A
	// literal zero here drew a bipolar shape starting from its own midpoint.
	curve_.push_back({ 0.0f, s.bipolar ? -1.0f : 0.0f });
	float x0 = 0.0f;
	for (size_t i = 0; i < segments.size(); ++i) {
		const float x1 = endX[i];
		const std::vector<float>& run = samples[i];
		if (x1 <= x0 || run.empty()) {
			x0 = x1;
			continue;
		}
		constexpr int kPointsPerStage = 24;
		for (int p = 1; p <= kPointsPerStage; ++p) {
			const float t = static_cast<float>(p) / kPointsPerStage;
			const auto index = std::min(run.size() - 1,
			                            static_cast<size_t>(t * static_cast<float>(run.size() - 1)));
			curve_.push_back({ x0 + t * (x1 - x0), run[index] });
		}
		x0 = x1;
	}
	if (curve_.size() < 2) {
		curve_.push_back({ 1.0f, 0.0f });
	}

	curveFrom_ = s;
	haveCurve_ = true;
}

const visage::Frame* EnvCard::boxFrame(size_t index) const {
	return index < kBoxCount ? boxes_[index].get() : nullptr;
}

const std::string& EnvCard::boxCaption(size_t index) const {
	static const std::string kNone;
	return index < kBoxCount ? boxes_[index]->label() : kNone;
}

void EnvCard::followMode() {
	// SUSTAIN IS NOT A SUSTAIN WHILE IT LOOPS, and the declaration in dahdsr_types.h says so in as
	// many words: "Sustain stops being a level to rest at and becomes the bottom the cycle falls to."
	// A caption that goes on saying "Sus" is describing the other mode.
	//
	// SOME CONTROLS CHANGE MEANING WHEN LOOP TURNS ON, so their captions do too — and the reason
	// it is worth more than tidiness turned up in the measurement below. With the shipping defaults,
	// a full-depth route from a LOOPING envelope does not move its destination at all: the cycle's
	// floor is the sustain level, so half the modulation depth is spent as a DC pedestal before the
	// cycle starts, and base 0.5 plus a 0.5 pedestal is already the ceiling. Measured on Level:
	// swing 0.00, pinned against the ceiling 100% of the time, while the envelope underneath cycled
	// at 4 Hz. That reads exactly like a loop that does not work.
	//
	// The name is the honest half of the remedy that costs no sound: a control called "Floor" invites
	// the question "floor of what, and why is it halfway up". Whether the DEFAULT should also change
	// is a question about sound and is a design decision, not this function's.
	const bool looping = settings().mode == dsr::Mode::Loop || settings().mode == dsr::Mode::Free ||
	                     settings().mode == dsr::Mode::LfoRetrig;
	if (looping == labelledForLoop_) {
		return;
	}
	labelledForLoop_ = looping;
	for (size_t i = 0; i < kBoxCount; ++i) {
		if (layout()[i] == dsr::Param::Sustain) {
			boxes_[i]->setLabel(looping ? "Floor" : kCaptions[i]);
		}
	}
}

bool EnvCard::updateContent() {
	// CONTENT-GATED, and the WHOLE reading is compared — the ordinal with the phase. An envelope
	// stepping from one stage to the next can land on a phase it has just had, and a gate that looked
	// only at the phase would sit on a stale stage for as long as the numbers happened to agree.
	// A PLAYHEAD IS A READING OF THE DEVICE, NOT A SETTING OF IT — published by the engine as it
	// ticks, like OWLTT's meters. So this does NOT follow the settings port: a host with no publisher
	// declines, and the card then reports nothing to redraw rather than a marker frozen at stage zero.
	// A stuck marker is a plausible picture of a stopped envelope, which is the worst kind of wrong.
	const bool trigChanged = trigPad_->poll();
	const mod::Playhead now = playhead();
	if (now == drawnPlayhead_) {
		return trigChanged;
	}
	drawnPlayhead_ = now;
	redraw();
	return true;
}

EnvCard::Marker EnvCard::marker() const {
	const dsr::Settings s = settings();
	// **THE LOOP IS DECLINED, NOT MISHANDLED** — see the declaration. Two drawn cycles, one position.
	if (s.mode == dsr::Mode::Loop || s.mode == dsr::Mode::Free || s.mode == dsr::Mode::LfoRetrig) {
		return {};
	}
	const mod::Playhead where = playhead();
	if (!where.running()) {
		return {};
	}
	const auto stage = static_cast<dsr::Stage>(where.ordinal);
	if (stage == dsr::Stage::Idle) {
		return {};
	}

	// **THE STAGE'S OWN SPAN, FROM THE CARD'S OWN TABLE.** `stageEndX` is what the drag and the hit
	// test already use, so a marker placed from it cannot disagree with the region a press resolves
	// to. A stage begins where its predecessor ended; Delay begins at the left edge.
	static constexpr std::array<dsr::Stage, 6> kOrder { dsr::Stage::Delay,   dsr::Stage::Attack,
		                                               dsr::Stage::Hold,    dsr::Stage::Decay,
		                                               dsr::Stage::Sustain, dsr::Stage::Release };
	size_t index = kOrder.size();
	for (size_t i = 0; i < kOrder.size(); ++i) {
		if (kOrder[i] == stage) {
			index = i;
		}
	}
	if (index >= kOrder.size()) {
		return {};
	}
	const float from = (index == 0) ? 0.0f : stageEndX(kOrder[index - 1]);
	const float to = stageEndX(stage);
	const float unitX = from + (to - from) * where.phase;

	// **AND THE Y COMES OFF THE DRAWN LINE.** The curve is stroked as chords between `curvePoints()`;
	// re-running the envelope here would put the mark off the line by however much the sampling
	// rounded a bend. `plot` below is `draw`'s own mapping, restated in the one place that must agree
	// with it — `plotUnit` is public precisely because a second copy of it was once a real bug.
	const std::vector<visage::Point> points = curvePoints();
	const TraceMarker::Landing on =
	    TraceMarker::locateBy(points.size(), [&points](size_t i) { return points[i].x; }, unitX);
	if (!on.valid) {
		return {};
	}
	const visage::Bounds curve = curveBounds();
	const auto plot = [&curve, &s](const visage::Point& p) {
		return visage::Point { curve.x() + std::clamp(p.x, 0.0f, 1.0f) * curve.width(),
			                   curve.bottom() - plotUnit(p.y, s.bipolar) * curve.height() };
	};
	const visage::Point a = plot(points[on.lo]);
	const visage::Point b = plot(points[on.hi]);
	// **EXCEPT WHERE THE LINE CANNOT KNOW THE LEVEL** (the defect: with a long attack released early, the line
	// jumped all the way to the sustain level before releasing even though the attack never got there). The
	// release is drawn from the sustain level and the attack (and the delay before it) from zero, but the envelope
	// releases from wherever the gate fell and re-attacks from wherever it was restarted. So in those three stages
	// the star stands at the level the device publishes, at the stage's x. MEASURED before: a key up 0.5 s into a
	// 2 s attack put the star on the release line at the 0.5 sustain while the envelope stood at 0.25.
	const bool lineKnowsLevel = !(stage == dsr::Stage::Delay || stage == dsr::Stage::Attack
	                              || stage == dsr::Stage::Release);
	if (where.haveLevel && !lineKnowsLevel) {
		return { { TraceMarker::at(on, a.x, b.x), plot({ unitX, where.level }).y }, true };
	}
	return { { TraceMarker::at(on, a.x, b.x), TraceMarker::at(on, a.y, b.y) }, true };
}

std::vector<visage::Point> EnvCard::curvePoints() const {
	auto* self = const_cast<EnvCard*>(this);
	if (!haveCurve_ || !(curveFrom_ == settings())) {
		self->rebuildCurve();
		self->followMode();
	}
	return curve_;
}

dsr::Settings EnvCard::settings() const {
	// THE DEVICE DECODES ITS OWN ROW. `settingsOf` puts ABSENT fields at their DECLARED defaults
	// rather than at zero — `device_row.h`'s rule — which is why this is a decode and not a copy.
	dsr::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return dsr::settingsOf(row, present);
}

void EnvCard::writeSettings(const dsr::Settings& settings) {
	// THE WHOLE ROW, THROUGH THE SAME ENCODER THE DEVICE READS. Writing individual fields by hand
	// here would be a second place that knows the row's shape, and `rowOf` is the first.
	const dsr::Row row = dsr::rowOf(settings);
	// ONE USER ACTION, ONE HOST GESTURE per control it moves (host slots, T6b review).
	backplane::ControlGestureScope gesture(host_, device_);
	for (size_t i = 0; i < row.size(); ++i) {
		gesture.setField(i, row[i]);
	}
}

mod::Playhead EnvCard::playhead() const {
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase)) {
		// NOBODY PUBLISHES ONE HERE. Idle rather than stage zero: `running()` is then false and the
		// marker draws nothing, which is the honest picture of "this host cannot see the playhead".
		return {};
	}
	mod::Playhead where { ordinal, phase };
	// AND WHERE IT STANDS, when the host publishes it — see `marker`.
	where.haveLevel = host_.playheadLevelAt(device_, where.level);
	return where;
}

bool EnvCard::headSourceFocused() const {
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

bool EnvCard::headArmed() const { return headSourceFocused() && mod_.armed(); }

void EnvCard::foldedChanged() {
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

void EnvCard::drawContent(visage::Canvas& canvas) {
	// **NO FRAME, NO HEAD, NO NAME — `FxCard::draw` painted them before calling this.**
	// The body is clamped to its own rectangle, so this cannot reach the head even by
	// getting its own arithmetic wrong: the card cannot draw over the header, by coordinate constraint.


	// THE SHAPE.
	const visage::Bounds curve = curveBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(curve.x(), curve.y(), curve.width(), curve.height(), 4.0f);

	// THE CHIPS. Lit means the switch is on.
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

	const std::vector<visage::Point> points = curvePoints();
	// A COPY, NOT A REFERENCE, AND THE DIFFERENCE WAS UNDEFINED BEHAVIOUR.
	//
	// **THE HAZARD IS GONE WITH THE ROAD, AND THE NOTE STAYS BECAUSE THE LESSON DOES NOT.**
	// `settings()` now returns BY VALUE from the device's own field row, so there is no temporary to
	// bind a reference into. What used to be here: `modRack()` returned a ModRack by value, and
	// `.envelope[device_]` went through `std::array::operator[]` — lifetime extension DOES NOT SURVIVE A
	// FUNCTION CALL, so the rack died at the end of the statement and `s` dangled for the rest of the
	// function. Anyone reintroducing a `const auto&` here should read on before doing it.
	//
	// IT WAS READ, AND THE FIELD IT WAS READ FOR IS THE ONE IN THE BUG REPORT. The plot lambda below
	// captures `s` by reference and asks `s.bipolar` for every point, and the zero line is drawn on it
	// — inside the fix for "in bipolar mode, the display truncates the bottom half and is otherwise
	// wrong". A heisenbug living in the repair for a reported bug: correct whenever the stack happens
	// to still hold the bytes, which is nearly always, and wrong for reasons having nothing to do with
	// envelopes. GCC named the field in as many words: `...envelope[<unknown>]...bipolar` may be used
	// uninitialized.
	//
	// The copy costs one Settings on the stack, once per paint.
	const dsr::Settings s = settings();

	// THE BOX HOLDS THE ENVELOPE'S WHOLE RANGE, and what that range IS depends on the envelope.
	//
	// THE DEFECT: in bipolar mode the display truncated the bottom half and was otherwise wrong. That was
	// right, and the previous comment here talked itself out of it — it noted that a bipolar envelope
	// reads -1..1 "so half its travel is below this box's floor" and then clamped it flat against the
	// floor anyway. That is not containing an overshoot, it is DISCARDING HALF THE SIGNAL: every
	// negative value drew on the same line, so the shape below zero was gone and the shape above it
	// sat in the wrong half of the box.
	//
	// So the mapping comes from the envelope's own polarity: a bipolar shape puts zero across the
	// middle and uses the full height for -1..1, a unipolar one keeps the floor at zero. The clamp
	// stays, AFTER the mapping rather than instead of it, because the other way a value can leave the
	// box is real and is not about polarity — settings moved mid-shape, a decay shortened while it is
	// decaying, can hand back a value above where its stage expects to end. That one is an overshoot
	// and belongs against the ceiling. On screen it looked like "sometimes the curve escapes the box".
	const auto plot = [&curve, &s](const visage::Point& p) {
		const float x = curve.x() + std::clamp(p.x, 0.0f, 1.0f) * curve.width();
		return visage::Point { x, curve.bottom() - plotUnit(p.y, s.bipolar) * curve.height() };
	};

	if (s.bipolar) {
		// WHERE ZERO IS, drawn, because a bipolar curve that crosses the middle of an empty box is
		// ambiguous about which half is which — and "sits at zero" and "sits halfway up" are different
		// claims about what the modulation is doing.
		const float zeroY = plot({ 0.0f, 0.0f }).y;
		canvas.setColor(withAlpha(kLine, 0.55f));
		canvas.segment(curve.x() + 2.0f, zeroY, curve.right() - 2.0f, zeroY, 1.0f, true);
	}

	// WHAT THE BEND CAUGHT, LIT — the MSEG's highlight (backlog MN): a band behind each caught span, under the
	// curve so the curve stays the brightest thing in the box, and the caught spans' own line restroked in the
	// accent after it. A peak lights both of its sides.
	const BendTarget lit = highlightForTest();
	const auto litSpan = [&](float x) {
		return std::find_if(spans_.begin(), spans_.end(), [&](const Span& sp) {
			return lit.bends(sp.stage) && x >= sp.from && x <= sp.to;
		}) != spans_.end();
	};
	for (const Span& sp : spans_) {
		if (lit.bends(sp.stage)) {
			const float from = plot({ sp.from, 0.0f }).x;
			const float to = plot({ sp.to, 0.0f }).x;
			canvas.setColor(withAlpha(kAccent, 0.20f));
			canvas.rectangle(from, curve.y(), std::max(to - from, 1.0f), curve.height());
		}
	}

	canvas.setColor(kValue);
	for (size_t i = 1; i < points.size(); ++i) {
		const visage::Point a = plot(points[i - 1]);
		const visage::Point b = plot(points[i]);
		canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
	}
	if (lit.kind != BendTarget::Kind::None) {
		canvas.setColor(kAccent);
		for (size_t i = 1; i < points.size(); ++i) {
			if (!litSpan(0.5f * (points[i - 1].x + points[i].x))) {
				continue;
			}
			const visage::Point a = plot(points[i - 1]);
			const visage::Point b = plot(points[i]);
			canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
		}
	}

	// **AND WHERE THE ENVELOPE IS RIGHT NOW.** `celestial.h`'s `starGlyph`, the same glyph the automod
	// strip and the MSEG card mark with and the same one the perform stage draws a SOUND with —
	// borrowed rather than redrawn, because a second star would be two places that must agree about
	// what a star looks like. Last, so the thing that MOVES is never under the thing that does not.
	if (const Marker mark = marker(); mark.drawn) {
		starGlyph(canvas, mark.at.x, mark.at.y, kPlayheadSize, kAccent, 1.0f);
	}
}


const char* EnvCard::contentTooltipAt(visage::Point local) const {
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

device::Modulatable* EnvCard::enableModulatable() {
	if (!hasEnable()) {
		return nullptr;
	}
	// BUILT ON FIRST ASK, and kept: a control holds the gesture state the history entry is
	// paired with, so a fresh one per press would open a gesture on one object and close it
	// on another.
	if (enableControl_ == nullptr) {
		enableControl_ = std::make_unique<DeviceControl>(host_, device_, mod::dahdsr::kDescriptor,
		                                                enableControl(), &mod_, &history_);
	}
	return enableControl_.get();
}

} // namespace feathers::ui
