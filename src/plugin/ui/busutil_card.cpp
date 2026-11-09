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
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/busutil_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace bu = fx::busutil;

namespace {

/// THE KNOBS, IN LAYOUT ORDER. One table, read by `layOut` AND by `addressedParamsForTest`, which is
/// what stops two functions disagreeing about which knob is which. `Enabled` is not here: the head
/// draws it, and a device listing its enable as a knob would have one field with two controls.
constexpr std::array<bu::Param, 9> kKnobs {
	bu::Param::Gain,     bu::Param::Pan,      bu::Param::PanL,     bu::Param::PanR,
	bu::Param::Width,    bu::Param::Highpass, bu::Param::Clip,     bu::Param::MidTrim,
	bu::Param::SideTrim,
};

/// THE CELL EACH KNOB SITS IN: the first seven fill the cells before the polarity pair, the trims the two after it.
[[nodiscard]] constexpr size_t knobCell(size_t knob) {
	return knob < BusUtilCard::kKnobCells ? knob : BusUtilCard::kMidCell + (knob - BusUtilCard::kKnobCells);
}

/// THE POLARITY PAIR, WHICH ARE BUTTONS AND NOT KNOBS — see `kPolarityCell`.
constexpr std::array<bu::Param, 2> kPolarity { bu::Param::InvertL, bu::Param::InvertR };

/// **THE CAPTIONS ARE THE CARD'S, NOT THE DESCRIPTOR'S, AND THE FIRST VERSION GOT THIS WRONG.**
///
/// A `ParamDesc::name` is QUALIFIED because a host's parameter list is flat — `filter_types.h` states
/// the rule, and four controls called "Pan" would be four rows nobody can tell apart in a DAW. On a
/// CARD the device's name is already on the head, so the qualifier is pure width: rendered, the row
/// read `BUS PAN BUS PAN L BUS PAN R` with the captions overlapping each other outright, and
/// `BUS WIDTH BUS INVER BUS INVER` truncated on top of that. Most of the card's sloppiness was this.
///
/// **CAUGHT BY PHOTOGRAPHING THE CARD, WHICH NOTHING ELSE COULD HAVE DONE.** The reachability guard
/// was green, every control had a knob, every value was right. The project rule is to render geometry
/// before believing it; this is the fourth time it caught one.
constexpr std::array<const char*, 9> kKnobCaptions {
	"Gain", "Pan", "Pan L", "Pan R", "Width", "Highpass", "Clip", "Mid", "Side",
};

/// **"POLARITY", NOT "INV".** The buttons were captioned `INV L` / `INV R`, and on the shipped card
/// polarity switches were requested as a NEW feature — they were already there and were not
/// recognisable. An abbreviation that saves four pixels and costs a control its identity is not a
/// saving.
/// **TWO LINES, SPELLED OUT** ("Invert" over "Pol"): the unit control spot has room for a more explicit
/// label. A knob's cell is taller than one caption line, and this
/// pair is buying that room back by being buttons — so it can afford to say what it does rather than
/// abbreviate it. `INV L` was the version that could not be read at all.
constexpr std::array<const char*, 2> kPolarityCaption { "invert", "pol" };

/// **THE OVER COLOUR. A LOCAL CONSTANT, BECAUSE THIS PALETTE HAS NO DANGER TOKEN.**
///
/// The first version of this LED drew in `kValue` — the palette's answer rather than an invented red,
/// following OWLTT's clip square. The LED now distinguishes the two: red when the signal is FULLY
/// clipped, the current colour while the clipping is soft. Two states
/// that mean different things need two colours, and the theme offers only one of them.
///
/// **SO THE GAP IS REAL AND THIS IS A LOCAL ANSWER TO IT, NOT A NEW TOKEN.** A `theme.h` entry is a
/// promise to every surface in the editor that a danger colour exists and is this one; that is a
/// decision about the whole editor and it is not this card's to make. If a second surface ever wants
/// it, that is the moment it becomes a token.
constexpr uint32_t kOverRed = 0xffD9483B;

/// THE CAPTION TYPE — smaller than a knob's readout, because a caption names a thing and a readout
/// IS the thing. Local rather than a theme token: no other surface has captions this small, and a
/// token is a promise to every card that one exists.
constexpr float kMeterCaptionSize = 7.5f;

/// LINEAR AMPLITUDE TO dBFS, floored rather than run to negative infinity.
[[nodiscard]] float dbOf(float linear, float floorDb) {
	if (!(linear > 0.0f)) {
		return floorDb;
	}
	return std::max(floorDb, 20.0f * std::log10(linear));
}

} // namespace

BusUtilCard::BusUtilCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                         Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	// TAKEN AND NOT KEPT — see `WaypointCard`'s constructor for the full reasoning: every knob here
	// is on the device-control road, which repaints through `ModState` rather than through the
	// parameter registry.
	(void)registry;
	controls_.resize(static_cast<size_t>(bu::Param::kCount));
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), kKnobCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// MONITOR, A NAMED LIST: a value box opens it as a menu from the descriptor alone (`ValueBox::choiceMenuAvailable`).
	monitor_ = std::make_unique<ValueBox>(control(bu::Param::Monitor), "Monitor");
	addChild(monitor_.get());

	// **THE HEAD'S FOUR OCCUPANTS, IN THE ORDER A PRESS IS TESTED AGAINST THEM.** This card shipped
	// without any of it for one build and three suites said so at once: the reorder drag reported
	// `cardDragging() == false`, the enable square did not toggle, and the head-verb sweep found an x
	// that answered nothing. **All three were the same absence** — a card that installs no
	// `onMouseDown` has no head at all.
	//
	// `=` HERE AND NOTHING ON THE DRAG OR RELEASE LISTS. `FxCard`'s constructor installs the
	// card-reorder handlers on `onMouseDrag`/`onMouseUp`, and `CallbackList::operator=` CLEARS —
	// so a card that assigned to those would silently replace the base's reorder with its own. This
	// card has nothing draggable in its body (the meter is a reading and the knobs are child frames
	// that consume their own presses), so it touches neither list and the base's gesture survives
	// intact. `WaypointCard` carries the long form of this note; it learned it the expensive way.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **THE BUTTONS ARE TESTED BEFORE `noteCardPress`, AND THAT ORDER IS THE WHOLE OF IT.** They
		// are drawn BY THIS CARD rather than being child frames, so nothing else consumes a press on
		// them — and `noteCardPress` is what arms the reorder drag. Without this the same gesture
		// would flip the polarity and pick the card out of the lane. `WaypointCard` gates its dots the
		// same way and says so; the knobs need no gate because a child frame takes its own presses.
		if (polarityPressAt(e.position)) {
			return;
		}
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		// THE BASE OWNS THE TOGGLE — `FxCard::enablePressAt`. This card adds its own voice, which the
		// base holds no notifier to speak with.
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Bus Utility on" : "Bus Utility off");
			}
		}
		// THE HEAD'S OWN VERB IS FOLD, LAST because it is the whole rectangle the others sit in — a
		// head tested first swallows every button inside it, and the bug reads as "the button does
		// nothing". **THE FOLD ITSELF HAPPENS ON RELEASE** (`FxCard::onMouseUp`); this arm exists only
		// so the press is not dispatched onward as a body press.
		else if (headBounds().contains(e.position)) {
		}
	};
}

DeviceControl& BusUtilCard::control(bu::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, bu::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

const char* BusUtilCard::contentTooltipAt(visage::Point local) const {
	// THE POLARITY PAIR IS DRAWN BY THIS CARD — `polarityPressAt`'s own hit test.
	if (folded()) {
		return nullptr;
	}
	for (size_t i = 0; i < kPolarity.size(); ++i) {
		if (polarityBounds(i == 1).contains(local)) {
			return controlTooltip(static_cast<size_t>(kPolarity[i]));
		}
	}
	if (swapBounds().contains(local)) {
		return controlTooltip(static_cast<size_t>(bu::Param::Swap));
	}
	return nullptr;
}

device::Modulatable* BusUtilCard::enableModulatable() {
	return &control(bu::Param::Enabled);
}

bool BusUtilCard::headEnabled() const {
	return control(bu::Param::Enabled).value() >= 0.5;
}

void BusUtilCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// **THE KNOBS, AND THIS LINE IS THE WHOLE OF THE MISSING-GLOW DEFECT.** The arm highlight is
	// drawn by the WIDGET, not by the control behind it: a `Knob` that was never handed the
	// `ModState` has nothing to subscribe to and stays dark while every other destination in the
	// editor lights up. The card forwarded to its controls only, which is enough to READ and WRITE a
	// modulated value and not enough to be seen.
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the polarity pair is edited by a gesture
	// this card hit-tests rather than by a widget, and an adapter that never learned the panel's
	// state reports itself unarmed forever. `WaypointCard` states the rule and calls it "cheap,
	// total, and it cannot fall behind a widget list"; asked of every control rather than of the ones
	// that happen to exist, so a lazily-built one cannot be missed.
	for (size_t i = 0; i < static_cast<size_t>(bu::Param::kCount); ++i) {
		control(static_cast<bu::Param>(i)).setModulation(mod, history);
	}
}

float BusUtilCard::openHeight() {
	const float rows = static_cast<float>((kCellCount + kKnobsPerRow - 1) / kKnobsPerRow);
	return kSlotHeadHeight + kMeterHeight + kRowGap + rows * (Knob::kHeight + kRowGap) + kPad;
}

float BusUtilCard::laneHeightAt(float) const {
	// A CARD THAT CAN FOLD ANSWERS WITH ITS CURRENT HEIGHT — `FxCard::laneHeight`'s rule, and what a
	// lane stacks by.
	return folded() ? foldedHeight() : openHeight();
}

float BusUtilCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	if (laneAxis() == LaneAxis::Horizontal) {
		// TWO UNITS WIDE, for six cells across at the width a vertical card gives four.
		return 2.0f * kCardUnitWidth;
	}
	return FxCard::laneExtent(cross);
}

visage::Bounds BusUtilCard::headBounds() const {
	return ownFrame().head;
}

visage::Bounds BusUtilCard::meterBounds() const {
	return { bodyLeft() + kPad, kSlotHeadHeight, bodyWidth() - 2.0f * kPad, kMeterHeight };
}

void BusUtilCard::resized() { layOut(); }

visage::Bounds BusUtilCard::cellBounds(size_t index) const {
	// SIX ACROSS IN A HORIZONTAL LANE (the rack), whose height is fixed and holds two rows, not three (backlog DM).
	const size_t perRow = laneAxis() == LaneAxis::Horizontal ? kKnobsPerRowHorizontal : kKnobsPerRow;
	const float cell = (bodyWidth() - 2.0f * kPad) / static_cast<float>(perRow);
	const size_t row = index / perRow;
	const size_t col = index % perRow;
	return { bodyLeft() + kPad + cell * static_cast<float>(col),
		     kSlotHeadHeight + kMeterHeight + kRowGap
		         + static_cast<float>(row) * (Knob::kHeight + kRowGap),
		     cell, Knob::kHeight };
}

visage::Bounds BusUtilCard::polarityBounds(bool right) const {
	// TWO BUTTONS IN ONE CELL, side by side, with the caption's line left under them — so the pair
	// occupies exactly the footprint of one knob and the row keeps its rhythm.
	const visage::Bounds cell = cellBounds(kPolarityCell);
	const float gap = 4.0f;
	const float w = std::min(22.0f, (cell.width() - gap - 8.0f) * 0.5f);
	const float h = 18.0f;
	const float left = cell.x() + (cell.width() - (2.0f * w + gap)) * 0.5f;
	return { right ? left + w + gap : left, cell.y() + 6.0f, w, h };
}

visage::Bounds BusUtilCard::swapBounds() const {
	// ONE BUTTON, centred in its cell at the polarity pair's height, so the row keeps the same rhythm.
	const visage::Bounds cell = cellBounds(kSwapCell);
	const float w = std::min(30.0f, cell.width() - 8.0f);
	return { cell.x() + (cell.width() - w) * 0.5f, cell.y() + 6.0f, w, 18.0f };
}

void BusUtilCard::layOut() {
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const visage::Bounds where = cellBounds(knobCell(i));
		knobs_[i]->setBounds(where.x(), where.y(), where.width(), where.height());
	}
	const visage::Bounds box = cellBounds(kMonitorCell);
	monitor_->setBounds(box.x() + 2.0f, box.y() + 6.0f, box.width() - 4.0f, ValueBox::kHeight);
}

bool BusUtilCard::polarityPressAt(visage::Point position) {
	if (folded()) {
		return false;
	}
	for (size_t i = 0; i < kPolarity.size(); ++i) {
		if (!polarityBounds(i == 1).contains(position)) {
			continue;
		}
		DeviceControl& c = control(kPolarity[i]);
		const bool on = c.value() >= 0.5;
		// BRACKETED, WHICH IS THE UNDO STEP — `FxCard::toggleEnable`'s reasoning for the enable
		// square, applied to a control that is not it: a bare `setValue` writes the field and forgets
		// it, and the history entry is what `beginGesture` records.
		c.beginGesture();
		c.setValue(on ? 0.0 : 1.0);
		c.endGesture();
		if (notifier_ != nullptr) {
			notifier_->say(i == 1 ? (on ? "Right polarity normal" : "Right polarity inverted")
			                      : (on ? "Left polarity normal" : "Left polarity inverted"));
		}
		redraw();
		return true;
	}
	// SWAP, the same toggle on the same road.
	if (swapBounds().contains(position)) {
		DeviceControl& c = control(bu::Param::Swap);
		const bool on = c.value() >= 0.5;
		c.beginGesture();
		c.setValue(on ? 0.0 : 1.0);
		c.endGesture();
		if (notifier_ != nullptr) {
			notifier_->say(on ? "Left and right normal" : "Left and right swapped");
		}
		redraw();
		return true;
	}
	return false;
}

void BusUtilCard::drawSwap(visage::Canvas& canvas) {
	const visage::Bounds b = swapBounds();
	const bool on = control(bu::Param::Swap).value() >= 0.5;
	canvas.setColor(on ? kAccent : kMuted);
	if (on) {
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
	}
	canvas.setColor(on ? kVoid : kMuted);
	canvas.text("L/R", font(kKnobReadoutSize), visage::Font::kCenter, b.x(), b.y(), b.width(), b.height());
	const visage::Bounds cell = cellBounds(kSwapCell);
	canvas.setColor(kDisabled);
	canvas.text("swap", font(kMeterCaptionSize), visage::Font::kCenter, cell.x(), cell.y() + 27.0f, cell.width(),
	            10.0f);
}

void BusUtilCard::drawPolarity(visage::Canvas& canvas) {
	static constexpr std::array<const char*, 2> kCaps { "L", "R" };
	for (size_t i = 0; i < kPolarity.size(); ++i) {
		const visage::Bounds b = polarityBounds(i == 1);
		const bool on = control(kPolarity[i]).value() >= 0.5;
		// **FILLED WHEN ON, OUTLINED WHEN OFF** — the enable square's own two states, in the same
		// language, so a switch reads as a switch wherever it is on this editor.
		canvas.setColor(on ? kAccent : kMuted);
		if (on) {
			canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
		}
		else {
			canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
		}
		canvas.setColor(on ? kVoid : kMuted);
		canvas.text(kCaps[i], font(kKnobReadoutSize), visage::Font::kCenter, b.x(), b.y(), b.width(),
		            b.height());
	}
	// THE CAPTION, ON THE LINE A KNOB'S NAME WOULD BE — the row's rhythm is what makes a pair of
	// buttons read as one control rather than as loose chrome.
	const visage::Bounds cell = cellBounds(kPolarityCell);
	canvas.setColor(kDisabled);
	for (size_t line = 0; line < kPolarityCaption.size(); ++line) {
		canvas.text(kPolarityCaption[line], font(kMeterCaptionSize), visage::Font::kCenter, cell.x(),
		            cell.y() + 27.0f + static_cast<float>(line) * 9.0f, cell.width(), 10.0f);
	}
}

void BusUtilCard::foldedChanged() {
	// A FOLDED CARD DRAWS NO BODY, and `Frame` keeps the bounds it was last given — so a knob left
	// visible would go on drawing over the head. `GenericCard` says the same thing in as many words.
	for (auto& knob : knobs_) {
		knob->setVisible(!folded());
	}
	monitor_->setVisible(!folded());
}

//------------------------------------------------------------------------------------------------//
// THE READINGS                                                                                    //
//------------------------------------------------------------------------------------------------//

bool BusUtilCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(bu::Reading::kCount);
}

float BusUtilCard::reading(bu::Reading r) const {
	if (!hasReadings()) {
		// **NOTHING PUBLISHED IS NOT SILENCE, AND THE TWO MUST NOT DRAW ALIKE.** `device_host.h` is
		// explicit that a caller cannot tell an unpublished reading from a zero one, so the card asks
		// `hasReadings()` before drawing anything at all; this is the value it uses if it draws
		// anyway, and the loudness floor is the honest nothing rather than 0 LUFS, which is loud.
		switch (r) {
		case bu::Reading::Momentary:
		case bu::Reading::ShortTerm:
		case bu::Reading::Integrated:
			return bu::kSilenceLufs;
		case bu::Reading::ClipAge:
			return bu::kClipAgeMax;
		default:
			return 0.0f;
		}
	}
	return host_.readingAt(device_, static_cast<size_t>(r));
}

bool BusUtilCard::ledCoreLit() const {
	return hasReadings() && reading(bu::Reading::ClipAge) < kCoreHoldSeconds;
}

ClipLedState BusUtilCard::ledState() const {
	if (!hasReadings()) {
		return ClipLedState::Normal;
	}
	return clipLedStateFor(reading(bu::Reading::RawPeak), ledCoreLit());
}

float BusUtilCard::ledGlow() const {
	if (!hasReadings()) {
		return 0.0f;
	}
	const float age = reading(bu::Reading::ClipAge);
	if (age >= bu::kClipAgeMax) {
		return 0.0f;
	}
	// **EXPONENTIAL, NOT LINEAR.** A linear fade reads as a light being turned down by a hand; a
	// decay reads as something that HAPPENED. The curve is the drawing's alone — the audio thread
	// published a time, and nothing about this line could have been decided there.
	return std::clamp(std::exp(-age / kGlowFallSeconds), 0.0f, 1.0f);
}

float BusUtilCard::barFractionForDb(float db) {
	return std::clamp((db - kMeterFloorDb) / (0.0f - kMeterFloorDb), 0.0f, 1.0f);
}

void BusUtilCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE SLOT MENU IS DRAWN HERE, OUTSIDE THE BODY'S CLAMP** — `FxCard::drawHeadExtras` defaults to
	// nothing, and a card that does not override it has a working "..." press with nothing drawn to
	// press, so the header's "..." item went missing. See `FilterCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

bool BusUtilCard::updateContent() {
	// **ASK FOR A FRAME ONLY WHEN THE PICTURE MOVED.** A meter that redrew every tick would repaint
	// the whole card at the host's timer rate whether or not anything had changed — automod's strip
	// records the same rule from the other side, where a card that compared correctly was RIGHT to
	// report nothing had changed and the bug was upstream.
	if (!hasReadings()) {
		return false;
	}
	const float peakL = reading(bu::Reading::PeakL);
	const float peakR = reading(bu::Reading::PeakR);
	const float momentary = reading(bu::Reading::Momentary);
	const float age = reading(bu::Reading::ClipAge);
	// **THE CLIP AGE IS IN THE COMPARISON, and it has to be**: an over during an otherwise steady
	// passage moves nothing else on this card. Without it the LED would light on whatever frame
	// something else happened to change.
	if (peakL == drawnPeakL_ && peakR == drawnPeakR_ && momentary == drawnMomentary_
	    && age == drawnClipAge_) {
		return false;
	}
	drawnPeakL_ = peakL;
	drawnPeakR_ = peakR;
	drawnMomentary_ = momentary;
	drawnClipAge_ = age;
	// **ASK FOR THE FRAME HERE, AND THIS LINE IS THE WHOLE OF THE FROZEN-METERING DEFECT.**
	//
	// `FxLane::updateCards` only COUNTS the cards that report movement — it repaints none of them.
	// `OwlttGraph` calls `redraw()` on itself and that is why OWLTT's bars move; this card returned
	// `true` and asked for nothing, so the meter computed correctly, published correctly, compared
	// correctly, and never drew.
	//
	// **AND THE TEST THAT SHOULD HAVE CAUGHT IT ASSERTED THE RETURN VALUE**, which is the REPORT and
	// not the DELIVERY. CLAUDE.md states the rule — *"the line that DELIVERS one must report that it
	// did"* — and a `bool` that a card can return without having redrawn is exactly the counter
	// standing beside the call that the rule refuses. `visage::Frame` keeps `redrawing_` private
	// with no getter, so this cannot be asserted headlessly at all; the honest position is that the
	// suite proves the numbers and an eye proves the motion.
	redraw();
	return true;
}

//------------------------------------------------------------------------------------------------//
// DRAWING                                                                                         //
//------------------------------------------------------------------------------------------------//

float BusUtilCard::kneeDepth() const {
	if (!hasReadings()) {
		return 0.0f;
	}
	const int index = static_cast<int>(std::lround(control(bu::Param::Clip).value()));
	if (!bu::clipEnabled(index)) {
		return 0.0f;
	}
	const float knee = bu::clipKnee(index);
	const float peak = reading(bu::Reading::RawPeak);
	// **A HARD CLAMP HAS NO KNEE TO BE PART-WAY INTO**, so its only states are "not clipping" and
	// "clipping" — depth is the step function rather than a ramp. Dividing by a knee of zero is the
	// arithmetic reason; that it has no soft region to report is the real one.
	if (knee <= 0.0f) {
		return peak >= 1.0f ? 1.0f : 0.0f;
	}
	const float start = 1.0f - knee;
	return std::clamp((peak - start) / knee, 0.0f, 1.0f);
}

void BusUtilCard::drawLed(visage::Canvas& canvas, visage::Bounds where) {
	const float cx = where.x() + where.width() * 0.5f;
	const float cy = where.y() + where.height() * 0.5f;
	// **RED MEANS THE CEILING WAS REACHED; THE ACCENT MEANS THE KNEE IS WORKING.** The two are
	// different facts and so they look different: one says the signal went past full
	// scale, the other says the saturation is doing something to it on the way there.
	const bool over = ledCoreLit();
	const uint32_t live = over ? kOverRed : kMeterValue;

	// **`kValue` IS THE OVER COLOUR, WHICH IS THE PALETTE'S ANSWER RATHER THAN AN INVENTED RED.**
	// OWLTT's clip square draws in the accent for the same reason: this theme has no danger token, and
	// adding one for a single indicator would be a card inventing a colour role the rest of the editor
	// does not have.
	//
	// THE GLOW FIRST, UNDER THE CORE — a halo drawn over its own centre would dim the thing it is
	// there to draw attention to.
	const float glow = ledGlow();
	if (glow > 0.0f) {
		// **A FALLOFF, NOT A DISC — AND THE FIRST VERSION WAS A DISC.** On the shipped build the glow
		// seemed absent. It was being drawn, and rendering: one circle at 45% alpha, which over
		// a dark ground is a flat maroon blob with a HARD EDGE. Nothing about that reads as light; it
		// reads as a darker shape sitting behind the LED, and at a second old it is barely separable
		// from the card.
		//
		// **WHAT MAKES A GLOW IS THE GRADIENT, WHICH ONE SHAPE CANNOT HAVE.** Concentric circles
		// drawn largest-first at a low alpha each ACCUMULATE toward the centre, so the alpha falls off
		// with radius without needing a gradient brush the canvas does not offer.
		//
		// **EIGHT LAYERS, BECAUSE FOUR BANDED.** Rendered at four the steps were visible as concentric
		// rings at full intensity — a gradient with contour lines in it, which is its own kind of
		// wrong. The count and the per-layer alpha trade against each other; these two were set by
		// looking at the three ages side by side, not derived.
		constexpr int kLayers = 8;
		constexpr float kReach = 2.6f;   ///< outermost radius, in LED radii, at full glow
		constexpr float kPerLayer = 0.12f; ///< alpha each layer contributes before accumulation
		for (int i = kLayers; i >= 1; --i) {
			const float t = static_cast<float>(i) / static_cast<float>(kLayers);
			const float radius = kLedRadius * (1.0f + (kReach - 1.0f) * t * glow);
			canvas.setColor(withAlpha(kOverRed, kPerLayer * glow));
			canvas.circle(cx - radius, cy - radius, radius * 2.0f);
		}
	}

	// THE CORE. Lit means AT or over 0 dBFS within the hold; unlit is the seated ring, so the
	// indicator has a position on the card whether or not anything has clipped — an LED that appears
	// out of nowhere is one nobody knows to look for.
	if (over) {
		canvas.setColor(live);
		canvas.circle(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f);
	}
	else {
		// **THE SOFT STATE: THE ACCENT, AT AN OPACITY THAT IS HOW FAR INTO THE KNEE THE SIGNAL IS.**
		// A ring underneath either way, so the indicator keeps its place on the card when nothing is
		// happening — an LED that appears out of nowhere is one nobody knows to look for.
		canvas.setColor(kMuted);
		canvas.ring(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f, 1.5f);
		const float depth = kneeDepth();
		if (depth > 0.0f) {
			canvas.setColor(withAlpha(kMeterValue, depth));
			canvas.circle(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f);
		}
		// **ALMOST CLIPPING (backlog ML)**: within 2 dB of full scale with no knee at work, the near colour. A knee
		// that is working says more about the same peak, so it keeps the LED.
		else if (ledState() == ClipLedState::Near) {
			canvas.setColor(kMeterNear);
			canvas.circle(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f);
		}
	}
}

void BusUtilCard::drawMeter(visage::Canvas& canvas) {
	const visage::Bounds box = meterBounds();

	// **NO DATA DRAWS NO METER.** Not a meter of zeros, which is a plausible picture of a wrong
	// thing — `device_host.h`'s rule, and the one OWLTT's frozen bars taught the hard way.
	if (!hasReadings()) {
		canvas.setColor(kDisabled);
		canvas.text("no signal", font(kKnobReadoutSize), visage::Font::kCenter, box.x(), box.y(),
		            box.width(), box.height());
		return;
	}

	// **THE LAYOUT STACKS, BECAUSE HEIGHT IS THE FREE AXIS AND WIDTH IS NOT.** This card sits in a
	// VERTICAL lane, where the cross axis is the width the lane hands out and the flow axis is the
	// card's own. The first version spent the fixed axis: bars, a three-figure readout and the LED
	// all in one row, which squeezed the numbers into a column too narrow to read or identify.
	// Everything below runs the full width
	// and takes another line when it needs one.
	// **WIDE ENOUGH FOR "COR".** It was 11 px and the correlation bar was captioned `C`, which left the
	// bottom meter unidentifiable — the same failure as `INV L` two changes
	// ago, and the second time an abbreviation here cost a control its identity. The bars give up
	// nine pixels; nothing else on this card needed them.
	const float capWidth = 20.0f;
	const float ledWidth = kLedRadius * 3.2f;
	const float barsLeft = box.x() + capWidth;
	const float barsWidth = std::max(20.0f, box.width() - capWidth - ledWidth - 6.0f);
	const float barHeight = 7.0f;
	const float barGap = 4.0f;

	const auto caption = [&](const char* what, float y) {
		canvas.setColor(kDisabled);
		canvas.text(what, font(kMeterCaptionSize), visage::Font::kLeft, box.x(), y - 1.5f, capWidth,
		            barHeight + 3.0f);
	};

	// THE TWO PEAK BARS, each with its channel named on the left.
	const std::array<float, 2> peaks { reading(bu::Reading::PeakL), reading(bu::Reading::PeakR) };
	static constexpr std::array<const char*, 2> kChannelCaps { "L", "R" };
	for (size_t ch = 0; ch < peaks.size(); ++ch) {
		const float y = box.y() + 4.0f + static_cast<float>(ch) * (barHeight + barGap);
		caption(kChannelCaps[ch], y);
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(barsLeft, y, barsWidth, barHeight, 2.0f);
		const float fraction = barFractionForDb(dbOf(peaks[ch], kMeterFloorDb));
		if (fraction > 0.0f) {
			// **THE BAR TURNS AT 0 dBFS AND NOWHERE ELSE.** A gradient through amber at some invented
			// -6 would be the card inventing a threshold the device does not have; the one threshold
			// here is full scale, and the LED is what announces it.
			canvas.setColor(peaks[ch] >= 1.0f ? kMeterValue : kMeterAccent);
			canvas.roundedRectangle(barsLeft, y, barsWidth * fraction, barHeight, 2.0f);
		}
	}

	// CORRELATION, ON ITS OWN ROW — a centred bar, because the quantity is signed and its ZERO is the
	// thing it is read against. The centre tick makes that zero visible rather than inferred from the
	// bar being absent.
	const float corrY = box.y() + 4.0f + 2.0f * (barHeight + barGap);
	{
		const float mid = barsLeft + barsWidth * 0.5f;
		caption("COR", corrY);
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(barsLeft, corrY, barsWidth, barHeight, 2.0f);
		canvas.setColor(kLine);
		canvas.rectangle(mid - 0.5f, corrY - 1.0f, 1.0f, barHeight + 2.0f);
		const float c = std::clamp(reading(bu::Reading::Correlation), -1.0f, 1.0f);
		const float half = barsWidth * 0.5f * std::fabs(c);
		if (half > 0.0f) {
			// NEGATIVE CORRELATION IS A WARNING AND POSITIVE IS NOT — an out-of-phase pair is the
			// thing this meter exists to show, so the two directions do not read alike.
			canvas.setColor(c < 0.0f ? kMeterValue : kMeterAccent);
			canvas.roundedRectangle(c < 0.0f ? mid - half : mid, corrY, half, barHeight, 2.0f);
		}
	}

	// **THE LED, SPANNING THE THREE BARS AND CAPTIONED WITH ITS THRESHOLD.** An unlabelled ring could not be
	// identified; "0dB" is the whole of what it means. It sits beside the bars
	// rather than above or below them because it is about the same quantity they are.
	{
		const float top = box.y() + 2.0f;
		const float tall = corrY + barHeight - top;
		drawLed(canvas, { box.right() - ledWidth, top, ledWidth, tall });
		canvas.setColor(kDisabled);
		canvas.text("0dB", font(kMeterCaptionSize), visage::Font::kCenter, box.right() - ledWidth,
		            top + tall, ledWidth, 9.0f);
	}

	// **THE LOUDNESS FIGURES, ON THEIR OWN FULL-WIDTH LINE, EACH NAMED AND WITH THE UNIT PRESENT.**
	// Three bare numbers in a column was the unreadable version. Momentary, Short-term, Integrated.
	{
		const float y = corrY + barHeight + 13.0f;
		const float lineHeight = 11.0f;
		canvas.setColor(kDisabled);
		canvas.text("LUFS", font(kMeterCaptionSize), visage::Font::kLeft, box.x(), y, 26.0f,
		            lineHeight);

		// **THE RAW AMPLITUDE, ON THE SAME LINE AND IN LINEAR UNITS.** It is an
		// instantaneous raw amplitude readout, and raw means raw: the largest sample of either
		// channel, no ballistics, printed as the number it is rather than converted to decibels. It
		// sits beside the loudness figures because it is the other half of "how big is this signal" —
		// one answer weighted for hearing, one not.
		canvas.setColor(kDisabled);
		canvas.text("AMP", font(kMeterCaptionSize), visage::Font::kLeft, box.x(), y + 12.0f, 26.0f,
		            lineHeight);
		{
			char amp[16] = {};
			std::snprintf(amp, sizeof(amp), "%.3f",
			              static_cast<double>(reading(bu::Reading::RawPeak)));
			canvas.setColor(reading(bu::Reading::RawPeak) >= 1.0f ? kMeterValue : kInk);
			canvas.text(amp, font(kKnobReadoutSize), visage::Font::kLeft, box.x() + 26.0f, y + 12.0f,
			            60.0f, lineHeight);
		}

		// **NOT `M` / `S` / `I`, AND THE REASON IS THE CONTEXT THIS CARD IS IN.** On a STEREO UTILITY
		// `M` and `S` mean Mid and Side — that is the standard reading, and it was read that way: a mono
		// signal showed a nonzero "S" LUFS, which looked like a Side reading that should be silent and
		// was in fact a perfectly correct Short-term loudness. **An abbreviation that is merely terse costs
		// legibility; one that names a DIFFERENT PLAUSIBLE QUANTITY costs trust in the number.** The
		// windows are spelled out.
		static constexpr std::array<const char*, 3> kWindowCaps { "MOM", "SHORT", "INT" };
		const std::array<float, 3> values { reading(bu::Reading::Momentary),
			                                reading(bu::Reading::ShortTerm),
			                                reading(bu::Reading::Integrated) };
		const float fieldLeft = box.x() + 26.0f;
		const float fieldWidth = (box.width() - 26.0f) / 3.0f;
		const float capColumn = 26.0f;
		for (size_t i = 0; i < values.size(); ++i) {
			const float x = fieldLeft + static_cast<float>(i) * fieldWidth;
			canvas.setColor(kDisabled);
			canvas.text(kWindowCaps[i], font(kMeterCaptionSize), visage::Font::kLeft, x, y, capColumn,
			            lineHeight);
			char text[24] = {};
			// **A FLOOR READS AS A DASH, NOT AS A NUMBER.** "-200.0" is arithmetic leaking onto the
			// card; nothing has been measured yet, and the honest glyph for that is not a figure.
			if (values[i] <= bu::kSilenceLufs + 1.0f) {
				std::snprintf(text, sizeof(text), "--");
			}
			else {
				std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(values[i]));
			}
			canvas.setColor(kInk);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight, x, y, fieldWidth - 3.0f,
			            lineHeight);
		}
	}
}

void BusUtilCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawMeter(canvas);
	drawPolarity(canvas);
	drawSwap(canvas);
}

std::vector<bu::Param> BusUtilCard::addressedParamsForTest() const {
	// EVERY CONTROL THIS CARD REACHES. The knob table plus the enable, which the HEAD draws — the
	// same division of labour every sibling card describes, and the reachability guard takes this
	// list at its word.
	std::vector<bu::Param> out(kKnobs.begin(), kKnobs.end());
	// THE POLARITY PAIR IS REACHED BY BUTTONS THIS CARD DRAWS AND HIT-TESTS ITSELF — see
	// `polarityPressAt`. Listed here because the reachability guard asks what this card REACHES, not
	// what it builds widgets for.
	out.insert(out.end(), kPolarity.begin(), kPolarity.end());
	out.push_back(bu::Param::Monitor);
	out.push_back(bu::Param::Swap);
	out.push_back(bu::Param::Enabled);
	return out;
}

void BusUtilCard::offeredControls(std::vector<Offered>& out) const {
	FxCard::offeredControls(out);
	// THE THREE BUTTONS this card draws itself (the polarity pair and Swap): each IS its control.
	out.push_back({ &control(bu::Param::InvertL), Road::Face });
	out.push_back({ &control(bu::Param::InvertR), Road::Face });
	out.push_back({ &control(bu::Param::Swap), Road::Face });
}

} // namespace feathers::ui
