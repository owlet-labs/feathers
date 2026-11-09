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

#include "plugin/ui/value_box.h"

#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <cstdlib>
#include <limits>

namespace feathers::ui {

ValueBox::ValueBox(device::Modulatable& control, std::string label)
    : control_(control), label_(std::move(label)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// SWITCHED OFF, THE HAND IS REFUSED FIRST — before the route menu, the picker and the drag, as `Knob` does.
		// A SWITCHED-OFF BOX TAKES ONLY A ROUTE (QK, by design: greyed controls glow, dimmer): its glow promises
		// the press, so the press is honoured; its value still takes no hand.
		if (!enabled_ && !mappable(e.isAltDown())) {
			return;
		}
		// A PRESS ON THE BOX WHILE ITS FIELD IS OPEN is the house's click-elsewhere (`ui/type_in.h`).
		const bool closedField = typeIn_.closeIfOpen();
		const int clicks = e.repeatClickCount();
		// **THE INBOUND-ROUTE MENU, BEFORE EVERYTHING INCLUDING THE PICKER — backlog DI.** A
		// right-click is a different gesture, not a quiet variant of a press: it must not open a
		// choice list, must not reset a value on its second click, and must not begin a drag.
		//
		// **THREE LINES, BECAUSE THE GESTURE IS SHARED.** It is generic over anything with a
		// modulation implementation; `mod_route_menu.h` is that, and this box needed
		// no new member, no wiring at its seven construction sites, and no chance to forget one.
		if (e.shouldTriggerPopup() && ui::showRouteMenu(*this, control_)) {
			return;
		}
		// **WHILE A SOURCE IS ARMED AND MAY REACH THIS BOX, THE PRESS IS AN ASSIGN — NOT A MENU** (the box glowed,
		// but a click to assign opened the context menu instead). The picker and the
		// choice menu took the press before the mapping drag below could see it, so a list glowed as a target and
		// refused to become one. Asked by `mappable`, the one answer the glow asks too (`draw`), so the two agree.
		const bool assigning = mappable(e.isAltDown());
		// A PICKER TAKES THE PRESS OUTRIGHT, before the double-click and the drag both. A control
		// whose values are a LIST has nothing to drag through, and a menu that only opened on a
		// single click would make the double-click reset a gesture that also opens a menu.
		if (!assigning && picker_) {
			picker_();
			return;
		}
		// AND THE SAME TREATMENT FOR ANY BOX WHOSE CONTROL IS A LIST, asked of the DESCRIPTOR rather
		// than of a flag somebody set — see `choiceMenuAvailable`. This is what makes automod's time
		// base a dropdown without automod knowing anything about menus.
		if (!assigning && showChoiceMenu()) {
			return;
		}
		// **A TRIPLE-CLICK TYPES** (backlog LS) — the knob's rule and reason, `Knob::buildMouse`.
		if (clicks >= 3) {
			if (!closedField) {
				(void)typeIn_.open(typeInBounds());
			}
			return;
		}
		if (clicks == 2) {
			// DOUBLE-CLICK RESETS, and in mapping mode it UNROUTES rather than resetting the value —
			// the same split the knob makes, because in mapping mode the thing under the hand is the
			// amount and resetting the value would answer a gesture nobody made.
			if (mappable(e.isAltDown())) {
				typeIn_.beginReset(control_, true);
				if (control_.dropRoute()) {
					redraw();
				}
				typeIn_.endReset();
				return;
			}
			// A BOX THAT SHOWS THE CONTROL THROUGH A READING OF ITS OWN is not typed in the control's unit —
			// the Phaser's Center prints semitones from a tracked note, and "3" would be read as 3 Hz.
			const bool typeable = !formatter_ || formatterIsSameNumber_ || formatter_(control_.value()) == control_.text();
			typeIn_.beginReset(control_, false, typeable);
			const double before = control_.value();
			// **THE CONTROL'S OWN RESET, BRACKETED AS ONE UNDOABLE STEP** — `Knob`'s. It used to bracket the
			// host's half alone (host slots, T6b review) on the premise that `edited_` records the undo
			// step; none of its three callers does, so a box's reset could not be undone, and the typed
			// value a triple-click writes had no step of the reset's to fold into (backlog LS).
			if (control_.resetToDefault()) {
				redraw();
				if (valueChanged_) {
					valueChanged_();
				}
				if (edited_) {
					edited_(before, control_.value());
				}
			}
			typeIn_.endReset();
			return;
		}
		typeIn_.disarm();
		beginDrag(e);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) { dragTo(e); };
	onMouseUp() = [this](const visage::MouseEvent&) { endDrag(); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void ValueBox::resized() {
	refreshWatch();
	// UNCONDITIONAL, for the scale rule — `place` does nothing visible before the first triple-click.
	typeIn_.place(typeInBounds());
}

visage::Bounds ValueBox::typeInBounds() const {
	// OVER THE READING: the whole of a compact box (its one row is the reading), the value row of a stacked
	// one, so the caption above stays to say what is being typed.
	if (compact_) {
		return { 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) };
	}
	return { 0.0f, kPadY + kLabelHeight, static_cast<float>(width()),
		     std::max(0.0f, static_cast<float>(height()) - kPadY - kLabelHeight) };
}

std::optional<LabelFace> ValueBox::labelFaceAt(visage::Point) const {
	if (label_.empty()) {
		return std::nullopt;
	}
	const float wide = textWidth(label_, font(kKnobLabelSize));
	float room = captionBox().width;
	if (compact_) {
		// THE NUMBER SITS OPPOSITE: a caption running under it is not read whole.
		room = static_cast<float>(width()) - kCompactPadX * 3.0f - textWidth(readout(), font(kKnobReadoutSize));
	}
	const char* name = control_.desc().name;
	return LabelFace { .label = label_, .full = name != nullptr ? name : label_, .cut = wide > room };
}

void ValueBox::setLabel(std::string label) {
	if (label == label_) {
		return;
	}
	label_ = std::move(label);
	redraw();
}

void ValueBox::setReadoutFormatter(std::function<std::string(double)> formatter, bool sameNumber) {
	formatter_ = std::move(formatter);
	formatterIsSameNumber_ = sameNumber;
	if (sameNumber) {
		typeIn_.setRewrite([this](const std::string& typed) { return faceUnitStripped(typed); });
	}
	else {
		typeIn_.setRewrite(nullptr);
	}
}

std::string ValueBox::faceUnitStripped(const std::string& typed) const {
	const auto lowerLetters = [](std::string_view s) {
		std::string out;
		for (const char c : s) {
			out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	};
	const auto isLetter = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
	// THE UNIT THE FACE PRINTS NOW: the letters closing its readout ("2Hz" -> "hz").
	const std::string face = readout();
	size_t faceCut = face.size();
	while (faceCut > 0 && isLetter(face[faceCut - 1])) {
		--faceCut;
	}
	const std::string unit = lowerLetters(std::string_view(face).substr(faceCut));
	// THE UNIT TYPED: the letters closing the text, after any trailing space, and the spaces before them.
	size_t end = typed.size();
	while (end > 0 && typed[end - 1] == ' ') {
		--end;
	}
	size_t cut = end;
	while (cut > 0 && isLetter(typed[cut - 1])) {
		--cut;
	}
	if (unit.empty() || lowerLetters(std::string_view(typed).substr(cut, end - cut)) != unit) {
		return typed;
	}
	while (cut > 0 && typed[cut - 1] == ' ') {
		--cut;
	}
	return typed.substr(0, cut);
}

std::string ValueBox::readout() const {
	// THE VIEW'S FORMATTER FIRST, THE DECLARATION SECOND. See `setReadoutFormatter` — the descriptor
	// still says what the control IS; this says how this particular box shows it.
	if (formatter_) {
		return formatter_(control_.value());
	}
	return formatValue(control_.desc(), control_.value());
}

void ValueBox::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

bool ValueBox::mappable(bool altDown) const {
	// A SWITCHED-OFF BOX IS STILL A TARGET (QK, by design) — it glows dimmer (`draw`), and only a control that changes
	// nothing where it stands (`unavailable`) is not one.
	if (control_.unavailable() != nullptr) {
		return false;
	}
	// ALT BYPASS — held Alt edits the value directly instead of routing. `Knob` asks the same
	// question of the same modifier before taking a press.
	if (altDown) {
		return false;
	}
	// ONE ANSWER, ASKED OF THE CONTROL. This used to be `isDestination && armed`, which is the gate
	// Knob asks PLUS the one it also asked — whether the armed source may actually reach here. A
	// weight source may not reach a puck destination, so the two surfaces could disagree about
	// whether a drag was legal, and the box was the one that would light for a drag the bridge then
	// refused. See ModDisplay::routable.
	//
	// AND THAT CONVERGENCE WAS ONLY HALF OF ONE UNTIL 2026-08-26, WHICH IS WHY THIS NOTE IS HERE.
	// The paragraph above shipped describing the ROUTABLE gate as settled while the MODIFIER gate
	// stayed un-converged: Knob asked about Alt and this class had no parameter to ask with, so an
	// armed source captured every drag on a value box and nothing released it. A confident comment
	// about the half that was fixed is what kept the other half invisible. **Both gates are one
	// answer now — do not let a third one drift in under this paragraph.**
	return control_.display().routable;
}

bool ValueBox::lit() const {
	const device::ModDisplay state = control_.display();
	return state.isDestination && mappable(false); // greyed or not (QK); `draw` dims a greyed one
}

ControlFace::ModLook ValueBox::modLookFor(const device::Modulatable& c) const {
	const device::ModDisplay state = control_.display();
	if (&c != &control_ || !state.isDestination) {
		return {};
	}
	return { true, lit(), barBounds().height() > 0.0f || state.routedElsewhere || state.amount != 0.0f };
}

visage::Bounds ValueBox::barBounds() const {
	const device::ModDisplay state = control_.display();
	if (!state.isDestination) {
		return {};
	}
	const device::Modulatable::Span span = control_.amountSpan();
	if (span.hi <= span.lo) {
		return {};
	}
	// THE FULL HEIGHT OF THE BOX, spanning from the base toward where a full-strength source would
	// carry the value — so the bar's EXTENT is the amount and where it sits says which way.
	//
	// IT USED TO BE A THIN STRIP ALONG THE BOTTOM EDGE, and it went unnoticed for an evening of use:
	// a 3px line under a two-line box is a detail on a surface whose
	// job is to make routing obvious. So it fills the box instead — full width, and as tall as the
	// travel it represents, drawn UNDER the text.
	//
	// Vertical also agrees with the gesture, which was already vertical for both a value and an
	// amount: an indicator that grows along the axis the hand moves needs no explaining.
	const float top = (1.0f - span.hi) * height();
	return { 0.0f, top, width(), (span.hi - span.lo) * height() };
}

ValueBox::ReadoutBox ValueBox::readoutBox() const {
	// **THE THREE PLACES THE NUMBER CAN BE, IN ONE FUNCTION** — see the header. `draw` reads this too,
	// so the picture and the hit test cannot drift apart.
	// A COMPACT BOX THAT OPENS A MENU GIVES THE CARET ITS ROOM ON THE RIGHT OF ITS ONE ROW — see
	// `caretBounds`. A stacked box puts the caret on the CAPTION row instead, so its value keeps the
	// whole width: a time base's "1 barT" is 26px in a 33px box and was the case that said so.
	const float caret = compact_ && opensMenu() ? kCaretWidth + kCaretGap : 0.0f;
	if (compact_) {
		return { kCompactPadX, static_cast<float>(width()) - kCompactPadX * 2.0f - caret, label_.empty() };
	}
	return { 0.0f, static_cast<float>(width()), true };
}

ValueBox::ReadoutBox ValueBox::captionBox() const {
	// THE CAPTION GIVES UP THE CARET'S ROOM in a stacked box that opens a menu; the caption of a box
	// on a card is a short word ("Base", "Mode"), and the fit case measures every one of them.
	const float caret = !compact_ && opensMenu() ? kCompactPadX + kCaretWidth + kCaretGap : 0.0f;
	return { 0.0f, static_cast<float>(width()) - caret, true };
}

visage::Bounds ValueBox::caretBounds() const {
	if (!opensMenu()) {
		return {};
	}
	// ON THE CAPTION ROW OF A STACKED BOX, ON THE ONE ROW OF A COMPACT ONE, inside the right-hand pad.
	const float middle = compact_ ? height() * 0.5f : kPadY + kLabelHeight * 0.5f;
	return { width() - kCompactPadX - kCaretWidth, middle - kCaretHeight * 0.5f, kCaretWidth, kCaretHeight };
}

double ValueBox::placeAt(float x) const {
	const std::string text = readout();
	if (text.empty()) {
		return 0.0;
	}
	// ASCII ONLY, AND THAT IS WHAT A READOUT IS: digits, a sign, a point, and at most a short unit.
	const std::u32string wide(text.begin(), text.end());
	const visage::Font f = font(kKnobReadoutSize);
	const ReadoutBox box = readoutBox();
	const float total = f.stringWidth(wide);
	const float left = box.centred ? box.x + (box.width - total) * 0.5f
	                               : box.x + box.width - total;

	// **PREFIX DIFFERENCES, NOT PER-GLYPH MEASUREMENTS.** A glyph measured alone is not the width it
	// occupies in a string — kerning is between neighbours — so the advance of glyph `i` is the width
	// of the first `i+1` characters less the width of the first `i`. Measuring each character on its
	// own puts the columns progressively out of step with what is drawn.
	size_t hit = wide.size();
	float run = left;
	float previous = 0.0f;
	for (size_t i = 0; i < wide.size(); ++i) {
		const float upTo = f.stringWidth(wide.c_str(), static_cast<int>(i + 1));
		const float advance = upTo - previous;
		previous = upTo;
		if (x >= run && x < run + advance) {
			hit = i;
			break;
		}
		run += advance;
	}
	if (hit >= wide.size() || wide[hit] < U'0' || wide[hit] > U'9') {
		return 0.0;
	}

	// **THE PLACE IS MEASURED AGAINST THE DECIMAL POINT, NOT AGAINST THE END OF THE STRING.** Counting
	// digits to the right is right for an integer and wrong the moment there is a fraction: in
	// "1234.00" the first digit after the point has one digit following it, and a naive count would
	// call it the tens column when it is the tenths. Find the point, then measure from it.
	size_t dot = wide.size();
	for (size_t i = 0; i < wide.size(); ++i) {
		if (wide[i] == U'.') {
			dot = i;
			break;
		}
	}
	const double place = hit < dot
	    // Digits between this one and the point, exclusive: "1|234." is the thousands column.
	    ? std::pow(10.0, static_cast<double>(dot - hit - 1))
	    // Right of the point: the first fractional digit is tenths.
	    : std::pow(10.0, -static_cast<double>(hit - dot));

	// **AND A PLACE IN THE READOUT IS ONLY A PLACE IN THE VALUE WHEN THE READOUT IS THE VALUE.** The
	// smallest printed place is the readout's own rounding, so it is the tolerance both tests get.
	const double finest = dot >= wide.size() ? 1.0
	                                         : std::pow(10.0, -static_cast<double>(wide.size() - dot - 1));
	if (readoutTracksValue(place, finest)) {
		return place;
	}
	// **A `digitBox` CONTROL'S READOUT MAY BE A STRAIGHT-LINE TRANSLATION, AND THEN A DIGIT IS STILL A
	// DISTANCE (backlog IO).** SKLITTER's Groove Amount prints a swing as "62.5%" for a value of 0.375 — a
	// translation, which the check above rightly refuses — but across the control's range the translation
	// is linear, so the grabbed place converts to the value's units by the readout's own slope.
	//
	// ITS TOLERANCE IS THE LAST PRINTED DIGIT, COUNTED AS DIGITS: `finest` above counts every character
	// after the point, so a suffix ("62.0%") makes it ten times too strict for a check that reads three
	// independently rounded prints.
	size_t fractionDigits = 0;
	for (size_t i = dot + 1; i < wide.size() && wide[i] >= U'0' && wide[i] <= U'9'; ++i) {
		++fractionDigits;
	}
	const double scale = scaledReadoutScale(std::pow(10.0, -static_cast<double>(fractionDigits)));
	return scale > 0.0 ? place * scale : 0.0;
}

double ValueBox::pressPlaceAt(float x) const {
	const double strict = placeAt(x);
	if (strict > 0.0) {
		return strict;
	}
	// **THE NEAREST DIGIT, MEASURED THE WAY `placeAt` MEASURES** — prefix differences against the same readout box,
	// so the column chosen here is a column `placeAt` names when asked at its centre.
	const std::string text = readout();
	if (text.empty()) {
		return 0.0;
	}
	const std::u32string wide(text.begin(), text.end());
	const visage::Font f = font(kKnobReadoutSize);
	const ReadoutBox box = readoutBox();
	const float total = f.stringWidth(wide);
	const float left = box.centred ? box.x + (box.width - total) * 0.5f : box.x + box.width - total;
	float bestDistance = std::numeric_limits<float>::infinity();
	float bestCentre = 0.0f;
	bool found = false;
	float previous = 0.0f;
	for (size_t i = 0; i < wide.size(); ++i) {
		const float upTo = f.stringWidth(wide.c_str(), static_cast<int>(i + 1));
		const float start = left + previous;
		const float end = left + upTo;
		previous = upTo;
		if (wide[i] < U'0' || wide[i] > U'9') {
			continue;
		}
		const float distance = x < start ? start - x : (x >= end ? x - end : 0.0f);
		// `<=`: ON A TIE THE LATER — finer — DIGIT WINS, the conservative of two equally near columns.
		if (distance <= bestDistance) {
			bestDistance = distance;
			bestCentre = (start + end) * 0.5f;
			found = true;
		}
	}
	return found ? placeAt(bestCentre) : 0.0;
}

double ValueBox::parseReadout(double at) const {
	const std::string text = formatter_ ? formatter_(at) : formatValue(control_.desc(), at);
	const char* const begin = text.c_str();
	char* end = nullptr;
	const double parsed = std::strtod(begin, &end);
	// NO CONVERSION IS NOT ZERO. "off" and a Choice name parse nothing at all, and reading that
	// as 0.0 would let a readout naming a mode look like a readout of the number zero. `strtod`
	// also happily accepts "-inf", which is a Fader at silence and not a finite value.
	if (end == begin || !std::isfinite(parsed)) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	return parsed;
}

double ValueBox::scaledReadoutScale(double tolerance) const {
	const device::ParamDesc& d = control_.desc();
	if (!d.digitBox) {
		return 0.0;
	}
	// **MEASURED FROM THE FORMATTER, AT THE RANGE'S TWO ENDS AND ITS MIDDLE** — `readoutTracksValue`'s
	// argument for asking rather than declaring. The ends give the slope; the middle is the second point
	// that says the readout is a LINE across the range rather than a curve or a word, and a readout that
	// fails it gets no digit road at all. Asked at every press, because the print may follow another
	// control (Groove Amount prints a swing % or a template share, by Groove Mode).
	const double lo = d.min;
	const double hi = d.max;
	const double atLo = parseReadout(lo);
	const double atHi = parseReadout(hi);
	const double atMid = parseReadout((lo + hi) * 0.5);
	if (!(hi > lo) || !std::isfinite(atLo) || !std::isfinite(atHi) || !std::isfinite(atMid)
	    || !(std::fabs(atHi - atLo) > tolerance)) {
		return 0.0;
	}
	if (!(std::fabs(atMid - (atLo + atHi) * 0.5) <= tolerance * 2.0)) {
		return 0.0;
	}
	return (hi - lo) / std::fabs(atHi - atLo);
}

double ValueBox::finestPlace() const {
	// **THE SMALLEST DIGIT THIS READOUT PRINTS, or 0 where digit dragging does not apply at all.**
	//
	// **IT EXISTS BECAUSE THE DECLARED RANGE IS A LETHAL FALLBACK ON AN UNBOUNDED CONTROL.** A drag
	// with no span and no digit under the press used to reach the whole declared range; that was
	// harmless while every such control was bounded, and automod's collapsed coordinates (BJ piece 3)
	// are ±2²⁴. A press landing on the decimal point rather than a digit would have moved 33 million
	// units in one gesture — the same "far too fast with mouse input" problem the old
	// coarse boxes had, reintroduced through the back door of a missed press.
	//
	// **THE FINEST DIGIT IS THE RIGHT ANSWER RATHER THAN A SAFE ONE.** If the readout is the value at
	// all, its last printed digit is the smallest edit the box can display — so a press that names no
	// column gets the most conservative column there is, and the gesture stays inside what the reader
	// can see happening. A readout that is NOT the value has no columns to fall back to and keeps the
	// declared range, which is what every bounded control has always done.
	const std::string text = readout();
	if (text.empty()) {
		return 0.0;
	}
	const std::u32string wide(text.begin(), text.end());
	size_t dot = wide.size();
	for (size_t i = 0; i < wide.size(); ++i) {
		if (wide[i] == U'.') {
			dot = i;
			break;
		}
	}
	const double finest = dot >= wide.size()
	    ? 1.0
	    : std::pow(10.0, -static_cast<double>(wide.size() - dot - 1));
	return readoutTracksValue(finest, finest) ? finest : 0.0;
}

bool ValueBox::readoutTracksValue(double place, double tolerance) const {
	// **THE HONESTY CHECK BEHIND DIGIT DRAGGING.** Most of this editor's readouts are not the number
	// they scale: a Percent box prints 42 for 0.42, a LogHertz box prints "1.2k" for 10.2, a Fader
	// prints "-inf" for zero, a Choice prints a word. Grabbing the "4" of "42%" and moving ten of
	// them would move the VALUE by ten — a hundredfold overshoot on a control whose whole range is
	// one — so the digit road must decline wherever the readout is a translation rather than a
	// rendering.
	//
	// **NO PER-SITE WIRING, WHICH IS WHY IT IS A MEASUREMENT AND NOT A FLAG.** A `setDigitDrag(true)`
	// at each construction site is the shape that shipped Bus Utility with no arm glow: seven callers
	// each free to forget. The box can simply ASK its own formatter, which is a pure function of a
	// double, and the answer is right by construction for units that do not exist yet.
	const auto parse = [this](double at) -> double { return parseReadout(at); };

	const double v = control_.value();
	const double here = parse(v);
	// THE OFFSET: the readout must name this value, not some scaled cousin of it.
	if (!(std::fabs(here - v) <= tolerance)) { // NaN-safe: a failed parse falls out here
		return false;
	}
	// **AND THE SLOPE, BECAUSE ONE SAMPLE IS NOT A LINE.** A Percent box sitting at 0 prints "0%",
	// which agrees with its value exactly and by coincidence — every scaled readout passes the offset
	// test at the origin. Stepping by the grabbed place is the second point: 0 becomes "100%", a
	// slope of a hundred, and the box declines.
	const double stepped = parse(v + place);
	return std::fabs((stepped - here) - place) <= tolerance * 2.0;
}

void ValueBox::beginDrag(const visage::MouseEvent& e) {
	dragging_ = true;
	// **LATCHED AT THE PRESS, NOT READ DURING THE DRAG.** The readout changes as the value moves, so
	// asking which digit is under the pointer mid-drag would let the granularity shift under the hand
	// — and it would shift fastest exactly when the number is moving fastest.
	// ON A ZONED BOX, THE NEAREST DIGIT, NOT ONLY THE ONE STRICTLY UNDER THE PRESS — see `pressPlaceAt` (backlog QB).
	// Elsewhere a press on padding keeps its declared fallback (SKLITTER's Groove Amount: the whole range per drag).
	pressPlace_ = nearestDigitPress_ ? pressPlaceAt(e.position.x) : placeAt(e.position.x);
	// ALT BYPASS — pass isAltDown so mappable() returns false when Alt is held, editing the value
	// directly instead of creating a route.
	draggingAmount_ = mappable(e.isAltDown());
	fine_ = e.isShiftDown();
	anchorY_ = e.position.y;
	// EVERY GESTURE STARTS AT REST (backlog QB): no speed, no added travel, carried from the last one.
	lastY_ = e.position.y;
	extraTravel_ = 0.0;
	tracker_.begin(drag_accel::nowMs());

	if (draggingAmount_) {
		// BRACKETED, WHICH IT WAS NOT. An amount is a routing and routings have their own gesture on
		// the bridge; the knob opened one around its amount drags and this widget simply did not, for
		// no reason anyone chose. Found while asking whether device::Modulatable could express what
		// Knob already did — it could not, and neither of the two implementations was doing it.
		control_.beginAmountGesture();
		beforeAmount_ = control_.display().amount;
		// A press on an unrouted destination seeds an amount, so the bar is under the hand from the
		// first frame rather than after the first few pixels — the knob's own rule.
		anchorAmount_ = beforeAmount_ != 0.0f ? beforeAmount_ : kRouteSeedAmount;
		control_.setAmount(anchorAmount_);
		return;
	}
	anchorValue_ = control_.value();
	before_ = anchorValue_;
	control_.beginGesture();
}

void ValueBox::dragTo(const visage::MouseEvent& e) {
	if (!dragging_) {
		return;
	}
	const float travel = anchorY_ - e.position.y; // up is more, which is every drag in this editor
	if (draggingAmount_) {
		control_.setAmount(std::clamp(anchorAmount_ + travel / kRouteDragRange, -device::kMaxAmount,
		                              device::kMaxAmount));
		return;
	}
	const device::ParamDesc& desc = control_.desc();
	const double span = desc.max - desc.min;
	// **THE TRAVEL THIS DRAG IS WORTH, AND A DECLARED SPAN OUTRANKS THE DIGIT.**
	//
	// A `dragSpan_` is somebody's decision about how far this particular gesture should travel —
	// Gamma's sixteen laps, a coordinate's one region, the shaper's field step — and six sites make
	// it deliberately. The digit place is the DEFAULT for a box that never said, not an override of a
	// box that did.
	//
	// **THE FIRST DRAFT HAD THIS PRECEDENCE THE OTHER WAY AND IT BROKE BOTH.** MEASURED: Gamma's
	// full drag moved 1 lap where its own case asserts 16, and a coordinate's fine half stopped
	// carrying at all. A new default is allowed to decide what happens where nothing was chosen; it
	// is not allowed to quietly outrank the choices already made.
	//
	// **AND THE LAST RESORT IS THE FINEST DIGIT, NOT THE DECLARED RANGE, WHERE THERE IS ONE.** See
	// `finestPlace`: on an unbounded coordinate the range is 2^25 wide and reaching it from a press
	// that merely missed a digit would be the "far too fast" problem all over again.
	const double finest = finestPlace();
	const double reach =
	    dragSpan_ > 0.0        ? dragSpan_
	    : pressPlace_ > 0.0    ? pressPlace_ * kUnitsPerPlaceDrag
	    : finest > 0.0         ? finest * kUnitsPerPlaceDrag
	                           : span;
	const double scale = (fine_ ? kFineFactor : 1.0f) * reach / kDragRange;
	// **ACCELERATION (backlog QB): the travel since the last event, at the gain the hand's speed earns.** Only the
	// part ABOVE gain 1 is accumulated, so a drag that never speeds up lands exactly where `travel` alone puts it.
	// Shift-fine never accelerates: it is the precise override.
	if (accelerated_ && !fine_) {
		const double dy = static_cast<double>(lastY_ - e.position.y);
		const double gain = tracker_.step(dy, drag_accel::nowMs());
		extraTravel_ += dy * (gain - 1.0);
	}
	lastY_ = e.position.y;
	const double raw = anchorValue_ + (static_cast<double>(travel) + extraTravel_) * scale;
	// **NO WRAPPING AND NO CARRY ANY MORE — backlog BJ, piece 4.** A drag off the top used to come
	// back at the bottom and report a lap for a second control to absorb, which is how "just keep
	// turning past the eighth zone" was expressed while every zoned coordinate was bounded 0..1.
	//
	// The bounded form is gone — by design none of these is bounded — so a coordinate simply
	// continues, and the devices that needed a lap count derive it from the coordinate's own whole
	// part. The last caller went with OWLTT's carry; nothing has wrapped since.
	commit(raw);
}

void ValueBox::endDrag() {
	if (!dragging_) {
		return;
	}
	dragging_ = false;
	if (draggingAmount_) {
		draggingAmount_ = false;
		// The control closes it and writes whatever undo step it owes — see ParamControl's pair.
		control_.endAmountGesture();
		return;
	}
	control_.endGesture();
	const double after = control_.value();
	if (after != before_ && edited_) {
		// ONE CALL PER GESTURE, not one per mouse-move — which is what makes a drag one undo step for
		// whoever is recording them.
		edited_(before_, after);
	}
}

bool ValueBox::choiceMenuAvailable() const {
	const device::ParamDesc& desc = control_.desc();
	if (!desc.stepped || desc.names.data() == nullptr || desc.names.empty()) {
		return false;
	}
	// THE COUNT FROM THE RANGE, NOT FROM THE ARRAY — a stepped choice runs min..max inclusive, so a
	// `names` array that has drifted shorter shows blanks rather than reading off its end. The MSEG
	// card's version said this first and it is the reason this one is written the same way.
	// **THE FLOOR OF THREE IS REALLY A FLOOR OF "NOT `Off`/`On`".** The argument for it — *"a menu to
	// choose between Off and On is two clicks for what a nudge does"* — is an argument about a
	// SWITCH, and it was written when every two-entry choice in the project was one. A two-entry
	// choice whose states are NAMED (the delay's `Clean`/`Granular`) is a picker that happens to be
	// short, and the player should read its options rather than discover them by applying them.
	//
	// **AND A PICKER THAT BECOMES A MENU AT THREE ENTRIES CHANGES SHAPE UNDER THE PLAYER** the day a
	// mode is added — Mode is a dropdown so future modes (Analog, Tape) fit without a change of
	// shape. `device::isOffOnChoice` is the one place that question is answered.
	return entryCount() >= 3 || !device::isOffOnChoice(desc);
}

size_t ValueBox::entryCount() const {
	const device::ParamDesc& desc = control_.desc();
	const double span = desc.max - desc.min;
	if (!(span >= 0.0)) {
		return 0;
	}
	return static_cast<size_t>(std::lround(span)) + 1;
}

bool ValueBox::showChoiceMenu() {
	if (!choiceMenuAvailable()) {
		return false;
	}
	// THROUGH THE SHARED ROAD (`menu_anchor.h`): placed against this box, and a press while it is open
	// closes it. Either way the press is answered, so this reports true.
	showMenuAt(choiceMenu(), *this, { 0.0f, 0.0f, width(), height() });
	return true;
}

visage::PopupMenu ValueBox::choiceMenu() {
	const size_t count = entryCount();
	visage::PopupMenu menu;
	// FLAGGED AS WELL AS MARKED, so `showMenuAt` opens a long list scrolled to the current row (`menu_anchor.h`).
	const auto current = static_cast<long>(std::lround(control_.value() - control_.desc().min));
	for (size_t i = 0; i < count; ++i) {
		menu.addOption(static_cast<int>(i), choiceLabelAt(i)).select(static_cast<long>(i) == current);
	}
	menu.onSelection() = [this](int id) {
		if (id < 0) {
			return;
		}
		const double before = control_.value();
		// BRACKETED AS ONE EDIT, like every other value write here — a choice is still a parameter
		// change, and a host recording automation wants one step rather than a bare set.
		control_.beginGesture();
		commit(control_.desc().min + static_cast<double>(id));
		control_.endGesture();
		// AND THROUGH THE SAME HISTORY CHANNEL A DRAG USES, so a picked choice is undoable exactly as
		// a dragged one is. `commit` does not report — it is the drag's inner step — so this says it,
		// once, at the end of the gesture where the drag also says it.
		if (edited_ && control_.value() != before) {
			edited_(before, control_.value());
		}
		redraw();
	};
	return menu;
}

std::string ValueBox::choiceLabelAt(size_t index) const {
	const device::ParamDesc& desc = control_.desc();
	const char* name = index < desc.names.size() ? desc.names[index] : "";
	// WHERE THE VALUE IS NOW. Rounded rather than truncated: a stepped control holds a whole number,
	// but it arrives here through a double and a modulated read.
	const auto current = static_cast<long>(std::lround(control_.value() - desc.min));
	{
		// **AN "X" ON THE ONE YOU ARE ON — next to the selected timebase in the dropdown.**
		// Written into the LABEL rather than through
		// `visage::PopupMenu::select`, which looks like the right call and is not: `selected()` is
		// read by `visage_ui/macos/menu_bar.mm` and by nothing else, so on the in-editor popup this
		// editor actually draws it would have set a flag nobody reads. Checked before writing it,
		// because a mark that renders on one platform and silently vanishes on the other is worse
		// than no mark at all.
		//
		// THE UNSELECTED ROWS CARRY THE SAME TWO-CHARACTER CELL, so the names line up in a column
		// instead of stepping right by one glyph on whichever row you happen to be standing on.
		return markedChoiceLabel(name != nullptr ? name : "", static_cast<long>(index) == current);
	}
}

void ValueBox::commit(double value) {
	const double before = control_.value();
	control_.setValue(value);
	if (control_.value() != before) {
		redraw();
		if (valueChanged_) {
			valueChanged_();
		}
	}
}

void ValueBox::refreshWatch() {
	ModState* state = routingStateOf(control_);
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

void ValueBox::draw(visage::Canvas& canvas) {
	// **AND AGAIN HERE, BECAUSE A LAYOUT IS NOT THE LAST WORD.** `resized` catches every seated box in
	// a headless fixture; a paint catches the case a layout cannot — a state installed after the last
	// relayout and before the next frame. Once the state is found this is a pointer compare.
	refreshWatch();
	const device::ModDisplay state = control_.display();
	// LIT EXACTLY WHEN A PRESS WOULD ASSIGN (`mappable` without Alt): a box that glowed for an armed source that may not
	// reach it promised a route the press then refused (it read `isDestination && armed`, the gate before `routable`).
	const bool lit = this->lit();
	// DIMMED WHEN SWITCHED OFF: the number in the caption's colour and the caption fainter still, as a disabled knob.
	const visage::Color ink = enabled_ ? kInk : kMuted;
	const visage::Color captionInk = enabled_ ? kMuted : withAlpha(kMuted, 0.5f);

	// THE GROUND. A compact box is seated INSIDE something that already framed it, so it reads as a
	// recess in that surface rather than as a card laid on top of it — same fill, no border below.
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kCornerRadius);

	// THE BAR IS THE GROUND, NOT AN ORNAMENT ON TOP — drawn here, before the label and the number, so
	// a bar that now fills the box does not sit over the text it is about.
	const visage::Bounds fill = barBounds();
	if (fill.height() > 0.0f) {
		canvas.setColor(withAlpha(kModulation, lit ? 0.34f : 0.20f));
		canvas.rectangle(fill.x(), fill.y(), fill.width(), fill.height());
	}
	else if (state.routedElsewhere || (state.isDestination && state.amount != 0.0f)) {
		// A ROUTE WHOSE REACH CLAMPS TO NOTHING (the box at the end its amount pushes toward) is a route too, and drew as
		// nothing until 2026-11-07 (the glow-honesty sweep): the same dot.
		// SOMETHING ELSE REACHES THIS CONTROL. The bar is about the FOCUSED source and is correctly
		// empty here — but "no bar" was the only thing this box could say, and it says exactly that
		// for a control nothing touches. So a control being driven by a source you are not looking at
		// drew identically to one that is not modulated at all.
		//
		// A DOT RATHER THAN A BAR, deliberately: a bar has an extent that means an amount, and this is
		// not a quantity — it is "look elsewhere". Dim, in one corner, so it reads as a footnote.
		//
		// This is what cost an evening of debugging a ten-second envelope period: a self-route on the
		// attack, live and making the sound, drawn as nothing because the focus had moved on.
		const float inset = 2.0f;
		canvas.setColor(withAlpha(kModulation, 0.5f));
		canvas.circle(inset, height() - kBarHeight - inset, kBarHeight);
	}

	// THE BORDER IS THE MAPPING SIGNAL — periwinkle while a source is armed and this control could
	// take it, the ordinary rule otherwise. Same colour and same meaning as the knob's glow, which is
	// the whole point of both reading their state from one interface.
	//
	// A COMPACT BOX DRAWS ONLY THE SIGNAL HALF, and that asymmetry is the point rather than an
	// omission: the resting border is what makes the box a separate object, and the armed border is
	// an affordance the honesty rule will not let go of. See `kCompactHeight`.
	if (lit || !compact_) {
		// A SWITCHED-OFF BOX'S SIGNAL IS DIMMER (QK): plainly lit, plainly not the live box beside it.
		canvas.setColor(lit ? (enabled_ ? kModulation : withAlpha(kModulation, kGreyedSignalAlpha)) : kLine);
		canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), kCornerRadius, 1.0f);
	}

	if (compact_) {
		// ONE ROW: caption left, number right, both centred on the row's middle. Right-aligned
		// because the number is what changes — a value that grew from "3" to "12" would shift its own
		// left edge under a centred layout, and a readout that moves while you drag it is harder to
		// read than one that does not.
		const float textTop = (height() - kValueHeight) * 0.5f;
		// NO CAPTION MEANS THE NUMBER IS THE WHOLE CONTENT, so it takes the middle rather than
		// hugging the right edge against a caption that is not there. Right-alignment earns its keep
		// only when something sits opposite it — see the note below on why the number is the half
		// that moves.
		if (!label_.empty()) {
			canvas.setColor(captionInk);
			canvas.text(label_, font(kKnobLabelSize), visage::Font::kLeft, kCompactPadX, textTop,
			            width() - kCompactPadX * 2.0f, kValueHeight);
		}
		// **THE NUMBER IS DRAWN FROM `readoutBox()` AND ONLY FROM IT** — both compact rows and the
		// stacked one below. The first draft had this call in one branch and the same three literals
		// open-coded in the other two, which is a single source of truth that two callers are free to
		// drift away from; the hit test would then agree with a third of the picture.
		const ReadoutBox rb = readoutBox();
		canvas.setColor(ink);
		canvas.text(readout(), font(kKnobReadoutSize),
		            rb.centred ? visage::Font::kCenter : visage::Font::kRight, rb.x, textTop,
		            rb.width, kValueHeight);
		drawCaret(canvas);
		return;
	}

	canvas.setColor(captionInk);
	const ReadoutBox caption = captionBox(); // `labelFaceAt` measures against this room too
	canvas.text(label_, font(kKnobLabelSize), visage::Font::kCenter, caption.x, kPadY, caption.width,
	            kLabelHeight);

	// THE NUMBER IS THE BASE, ALWAYS — never the modulated result. See the note at the top of the
	// header: the mock's rule, and this project's atlas-corruption reason for keeping it.
	const ReadoutBox stacked = readoutBox();
	canvas.setColor(ink);
	canvas.text(readout(), font(kKnobReadoutSize),
	            stacked.centred ? visage::Font::kCenter : visage::Font::kRight, stacked.x,
	            kPadY + kLabelHeight, stacked.width, kValueHeight);
	drawCaret(canvas);
}

void ValueBox::drawCaret(visage::Canvas& canvas) const {
	const visage::Bounds c = caretBounds();
	if (c.width() <= 0.0f) {
		return;
	}
	// TWO STROKES, NOT A GLYPH — the atlas has no arrows — in the caption's colour, so it reads as a
	// mark on the box rather than as part of the value.
	const float middle = c.x() + c.width() * 0.5f;
	canvas.setColor(kMuted);
	canvas.segment(c.x(), c.y(), middle, c.bottom(), 1.0f, true);
	canvas.segment(middle, c.bottom(), c.right(), c.y(), 1.0f, true);
}

void seatInChipCell(ValueBox& box, const visage::Bounds& cell) {
	box.setBounds(cell.x() + 1.0f, cell.y() + 1.0f, std::max(0.0f, cell.width() - 2.0f),
	              std::max(0.0f, cell.height() - 2.0f));
}

void drawChoiceCell(visage::Canvas& canvas, const visage::Bounds& cell) {
	// THE UNLIT CHIP'S OWN STROKE — the chips beside it draw exactly this when off.
	canvas.setColor(kVoid);
	canvas.roundedRectangle(cell.x(), cell.y(), cell.width(), cell.height(), 3.0f);
	canvas.setColor(withAlpha(kLine, 0.8f));
	canvas.roundedRectangleBorder(cell.x(), cell.y(), cell.width(), cell.height(), 3.0f, 1.0f);
}

} // namespace feathers::ui
