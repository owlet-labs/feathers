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

#include "plugin/ui/harm_scope.h"

#include "plugin/fx_chain.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace feathers::ui {

namespace hrm = feathers::fx::harm;

namespace {

constexpr float kPad = 8.0f;

/// HOW MANY COLOURS THE CURVE IS ALLOWED TO SPEND, and the reason is the filter card's: a per-point
/// colour puts one gradient-atlas entry per point on every redraw, and a redraw happens on every
/// frame of a knob drag.
constexpr int kColourBands = 24;
constexpr float kCurveDesaturation = 0.75f;

} // namespace

/// HOW BIG A GRIP IS. Bigger than the mark it sits on, deliberately: a 3px dot is a target nobody
/// can hit, and this editor's own complaint history is about things being hard to grab.
constexpr float kGripRadius = 8.0f;

/// THE NOTCH'S OWN LANE, along the top of the plot. Nothing else is allowed in it, which is what
/// stops the four handles from covering one another — see gripBounds.
constexpr float kRailHeight = 12.0f;

HarmScope::HarmScope(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		held_ = gripAt(e.position);
		if (held_ == Grip::None) {
			return;
		}
		// ARMED AND NOT BYPASSED: this drag is about how far a source reaches, not where the value
		// sits. `Knob`'s rule and `Knob`'s modifier — Alt edits the value underneath without
		// disarming, because mapping is sticky and otherwise every value edit in a mapping session
		// costs two clicks somewhere else.
		routeDrag_ = gripMappable(held_) && !e.isAltDown();
		anchor_ = e.position;
		// HOW FAR THE NOTCH IS FROM THE POINTER WHEN THE GRIP IS TAKEN, so a horizontal drag moves it
		// BY the hand rather than TO the hand. The stem's ring is offset from the stem it holds — and
		// on whichever side has room — so a mapping that put the notch under the pointer would jump
		// it sideways the moment a grip was touched.
		anchorNotchDx_ = notchX() - e.position.x;
		anchorValue_ = routeDrag_
		                   ? static_cast<double>(control(paramFor(held_)).display().amount)
		                   : control(paramFor(held_)).value();
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (held_ == Grip::None) {
			return;
		}
		const fx::harm::Param param = paramFor(held_);
		if (routeDrag_) {
			// THE AMOUNT, ON THE SAME ROAD A KNOB'S ROUTE DRAG TAKES — vertical travel over the
			// knob's own drag range, so a hand that learned one learned the other.
			const float travel = (anchor_.y - e.position.y) / Knob::kDragRange;
			control(param).setAmount(
			    std::clamp(static_cast<float>(anchorValue_) + travel, -1.0f, 1.0f));
			redraw();
			return;
		}
		commit(param, valueForDrag(held_, e.position, e.isShiftDown()));
		// AND THE STEM CARRIES X AS WELL AS Y: the level handle duplicates the pong handle and also carries
		// an x component. The ring that proved easy to find does both jobs, and the rail (the pong handle)
		// stays as a second way to the same control.
		//
		// ONE GESTURE, TWO CONTROLS, and that is deliberate rather than a shortcut: a hand dragging
		// the oscillator's head around the picture is moving the sub-oscillator, and its pitch and its
		// loudness are the two things that mark is.
		if (held_ == Grip::Stem) {
			commit(hrm::partialParam(partial_, hrm::PartialField::Harmonic), harmonicForX(e.position.x + anchorNotchDx_));
		}
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		endGesture();
		held_ = Grip::None;
		routeDrag_ = false;
	};
}

DeviceControl& HarmScope::control(hrm::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, hrm::kDescriptor, index, mod_,
		                                               history_);
	}
	return *controls_[index];
}

void HarmScope::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// EVERY ADAPTER, INCLUDING ONES NOT YET BUILT — the built ones are told and the rest are born
	// with it. `HarmCard` shipped the other spelling and HARM could not be modulated at all.
	for (size_t i = 0; i < controls_.size(); ++i) {
		if (controls_[i] != nullptr) {
			controls_[i]->setModulation(mod, history);
		}
	}
}

bool HarmScope::gripMappable(Grip grip) const {
	if (grip == Grip::None) {
		return false;
	}
	// ONE QUESTION, ASKED OF THE CONTROL — `Knob::mappable`'s exact test, so the picture and the knob
	// cannot disagree about whether a source may reach the same control.
	return control(paramFor(grip)).display().routable;
}

hrm::Param HarmScope::paramOf(Grip grip) {
	switch (grip) {
		case Grip::Notch:
			return hrm::Param::Harmonic;
		case Grip::Trough:
			return hrm::Param::Depth;
		case Grip::Stem:
			return hrm::Param::Level;
		case Grip::Shoulder:
			return hrm::Param::Width;
		case Grip::None:
			break;
	}
	return hrm::Param::kCount;
}

hrm::Param HarmScope::paramFor(Grip grip) const {
	// THE SELECTED PARTIAL'S OWN CONTROL for the grip: the same four meanings, at the partial's own index.
	switch (grip) {
		case Grip::Notch:
			return hrm::partialParam(partial_, hrm::PartialField::Harmonic);
		case Grip::Trough:
			return hrm::partialParam(partial_, hrm::PartialField::Depth);
		case Grip::Stem:
			return hrm::partialParam(partial_, hrm::PartialField::Level);
		case Grip::Shoulder:
			return hrm::partialParam(partial_, hrm::PartialField::Width);
		case Grip::None:
			break;
	}
	return hrm::Param::kCount;
}

void HarmScope::setSelectedPartial(int partial) {
	partial = std::clamp(partial, 0, hrm::kPartialCount - 1);
	if (partial == partial_) {
		return;
	}
	partial_ = partial;
	held_ = Grip::None;
	routeDrag_ = false;
	redraw();
}

void HarmScope::commit(hrm::Param param, double value) {
	DeviceControl& c = control(param);
	if (c.value() == value) {
		return;
	}
	// THE GESTURE OPENS ON THE FIRST VALUE THAT MOVES, not on the press — `Knob::commit`'s rule, so a
	// click that grabs a handle and does not drag spends no undo step.
	//
	// AND IT BUYS NOTHING TODAY, WHICH A KNOWN-BAD PROVED RATHER THAN A READING. Deleting this
	// bracket left the case GREEN: `DeviceControl::beginGesture` and `endGesture` are EMPTY, because a
	// swallowed device's control is not a parameter and no host knows it exists. There is no undo
	// step and no host latch to spend.
	//
	// IT IS KEPT ANYWAY, and that is a decision rather than an oversight. The grips go through the
	// same `Modulatable` the knobs do — that is the constraint this whole feature rests on — and a
	// widget that skipped the bracket would be the ONE that does not, so the day `DeviceControl` grows a
	// real gesture (undo for swallowed devices is the obvious one) it would be the one that missed
	// it. This morning's lesson with the opposite answer: when a guard is redundant against the
	// value, find what else it is buying — and here, honestly, nothing yet.
	const size_t at = static_cast<size_t>(param);
	if (!gestureOpen_[at]) {
		c.beginGesture();
		gestureOpen_[at] = true;
	}
	c.setValue(value);
	redraw();
}

void HarmScope::endGesture() {
	// EVERY CONTROL THIS DRAG OPENED, not just the grip's own. The stem's drag writes Level AND
	// Harmonic, and a single flag would have closed whichever was committed last and left the other
	// open — the unmatched-gesture shape, which is host-facing wherever the gesture is real.
	for (size_t i = 0; i < gestureOpen_.size(); ++i) {
		if (gestureOpen_[i]) {
			control(static_cast<hrm::Param>(i)).endGesture();
			gestureOpen_[i] = false;
		}
	}
}

hrm::Settings HarmScope::settings() const {
	// THE SELECTED PARTIAL, seen as the first (`partialSlice`): every function below that asks "the notch" or "the oscillator"
	// asks about the partial the window has open, and answers exactly as it did when there was one.
	return hrm::partialSlice(fullSettings(), partial_);
}

hrm::Settings HarmScope::fullSettings() const {
	// ONE READ OF THE DEVICE'S OWN STATE, not a rebuild field by field. `fxFilterAt`'s note records
	// what the other shape costs: the filter card assembled its curve out of published values and
	// shipped a picture of a filter the engine was not running twice, once per field somebody
	// forgot. There is no list to forget here.
	// THE SETTINGS, READ THROUGH THE SEAM. `fxHarmAt` is `fromDeviceRow` plus `stateOf` behind a chain
	// type only the parameter road has; the seam gives the same row a field at a time, so this is the
	// same computation with the host-specific container taken out of the middle.
	hrm::Row row {};
	const size_t fields = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < fields; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	hrm::Settings s = hrm::stateOf(row, fields).settings;
	// THE PICTURE DRAWS THE DEVICE WHETHER OR NOT IT IS SWITCHED IN, which is the filter card's rule
	// and the only field overridden here. A player reaching for the Enabled switch wants to see what
	// they are about to switch on.
	s.enabled = true;
	// THE PER-RENDER FIELDS STAY AT THEIR DEFAULTS AND IT DOES NOT MATTER, which is the axis's whole
	// claim: note, bend and gate are not in the row (harm_types.h says so) and `notchRatio` divides
	// the note out. A case asserts that moving them moves nothing here.
	return s;
}

visage::Bounds HarmScope::plotBounds() const {
	return { kPad, kPad, std::max(0.0f, width() - 2.0f * kPad),
		     std::max(0.0f, height() - 2.0f * kPad) };
}

size_t HarmScope::curvePointCount() const {
	const float scale = std::max(1.0f, dpiScale());
	const float box = std::max(1.0f, plotBounds().width());
	const auto wanted = static_cast<size_t>(std::lround(box * scale * kPointsPerPixel));
	return std::min(kMaxCurvePoints, std::max(kMinCurvePoints, wanted));
}

size_t HarmScope::curveGains(float* out, size_t capacity) const {
	if (out == nullptr || capacity == 0) {
		return 0;
	}
	const size_t points = std::min(capacity, curvePointCount());
	hrm::ratioProfile(settings(), nullptr, out, points);
	return points;
}

float HarmScope::xForRatio(double ratio) const {
	const visage::Bounds box = plotBounds();
	if (!(ratio > 0.0)) {
		return box.x();
	}
	// LOG-SPACED ACROSS THE HARMONIC TABLE'S OWN ENDS, which is the same mapping `ratioProfile` uses
	// to lay its points down — so point `i` of the curve lands where `xForRatio` of point `i`'s ratio
	// says it should, rather than at a position two functions agreed about by hand.
	const double span = std::log2(hrm::kProfileHiRatio / hrm::kProfileLoRatio);
	const double at = std::log2(ratio / hrm::kProfileLoRatio) / span;
	return box.x() + box.width() * static_cast<float>(at);
}

float HarmScope::notchX() const {
	return xForRatio(hrm::notchRatio(settings()));
}

float HarmScope::oscillatorX() const {
	// THE OSCILLATOR SITS AT ITS SELECTED HARMONIC TIMES THE FINE DETUNE, which is `notchRatio` with
	// the level fallback taken out — the notch follows the oscillator, so where the oscillator IS is
	// the un-fallen-back version of where the notch goes.
	const hrm::Settings s = settings();
	return xForRatio(static_cast<double>(hrm::ratioAt(s.harmonic))
	                 * std::exp2(static_cast<double>(s.fineSemitones) / 12.0));
}

float HarmScope::oscillatorStem() const {
	return std::clamp(settings().level, 0.0f, 1.0f);
}

float HarmScope::shoulderX() const {
	// WALKED OUTWARD FROM THE CENTRE until the curve has recovered to about -3 dB. That IS the
	// shoulder, so the handle sits on the shape rather than beside it — and it moves with Width by
	// construction rather than by a formula that has to be kept in step with the filter.
	const visage::Bounds box = plotBounds();
	const hrm::Settings s = settings();
	const double centre = hrm::notchRatio(s);
	const float from = notchX();
	if (!(centre > 0.0) || box.width() <= 0.0f) {
		return from;
	}
	// AND NEVER CLOSER THAN A HANDLE'S WIDTH TO THE NOTCH, which is a compromise and is stated as
	// one. At the default width the Q is about 23, so the true -3 dB point is 0.03 of an octave out —
	// UNDER ONE PIXEL on this axis. A ring drawn there sits on the notch's own line, reads as part of
	// the curve rather than as something to take hold of, and is what the render caught.
	//
	// SO THE HANDLE MARKS WHERE YOU DRAG TO CHANGE WIDTH, not where the shoulder exactly is. It is on
	// the shoulder wherever the shoulder is far enough out to be grabbable, and at the floor
	// otherwise. The alternative is a control that is technically correct and physically unusable.
	const float floor = from + 2.5f * kGripRadius;
	const float limit = box.right();
	for (float x = from + 2.0f; x < limit; x += 1.0f) {
		const double span = std::log2(hrm::kProfileHiRatio / hrm::kProfileLoRatio);
		const double t = (x - box.x()) / static_cast<double>(box.width());
		const double ratio = hrm::kProfileLoRatio * std::exp2(span * t);
		if (hrm::notchMagnitudeAtRatio(s, ratio) >= 0.707) {
			return std::max(x, floor);
		}
	}
	return std::min(limit, floor);
}

double HarmScope::harmonicForX(float x) const {
	// THE RATIO AT `x`, SNAPPED TO THE TABLE. The axis IS the harmonic series, so dragging sideways is
	// choosing a harmonic — and the 102 entries are the only legal positions, so it snaps rather than
	// sliding through indices the device cannot hold. `Harm Harmonic` is a stepped control.
	const visage::Bounds box = plotBounds();
	const double span = std::log2(hrm::kProfileHiRatio / hrm::kProfileLoRatio);
	const double t = std::clamp((x - box.x()) / std::max(1.0f, box.width()), 0.0f, 1.0f);
	const double ratio = hrm::kProfileLoRatio * std::exp2(span * t);
	size_t best = 0;
	double bestDistance = 1.0e30;
	for (size_t i = 0; i < hrm::kHarmonicCount; ++i) {
		const double distance = std::abs(std::log2(static_cast<double>(hrm::ratioAt(i)) / ratio));
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return static_cast<double>(best);
}

visage::Bounds HarmScope::gripBounds(Grip grip) const {
	const visage::Bounds box = plotBounds();
	if (box.width() <= 0.0f || grip == Grip::None) {
		return {};
	}
	const hrm::Settings s = settings();
	const float notchX = this->notchX();
	// CLAMPED INSIDE THE PLOT, AND BELOW THE RAIL.
	//
	// BOTH HALVES WERE BUGS THE SWEEP FOUND, and neither shows at the default settings. A handle at
	// the extreme of its range — Level 0, Depth 1 — sat half outside the plot, and `gripAt` refuses a
	// point the plot does not contain, so THE CONTROL WAS UNREACHABLE EXACTLY WHERE A HAND MOST WANTS
	// IT: at the end of its travel, which is where you go to bring it back. And Depth 0 put the
	// trough at the top of the plot, inside the notch's rail, where the rail wins.
	const auto square = [&](float cx, float cy) {
		const float top = box.y() + kRailHeight + kGripRadius;
		const float bottom = box.bottom() - kGripRadius;
		const float x = std::clamp(cx, box.x() + kGripRadius, box.right() - kGripRadius);
		const float y = bottom >= top ? std::clamp(cy, top, bottom) : top;
		return visage::Bounds { x - kGripRadius, y - kGripRadius, 2.0f * kGripRadius,
			                    2.0f * kGripRadius };
	};
	// NO TROUGH WITHOUT A NOTCH — width zero is a wire, `refreshCoefficients`' own rule, so there is
	// no trough to take hold of and Depth changes nothing while it stays that way.
	//
	// BUT THE SHOULDER SURVIVES, AND THAT IS THE WHOLE POINT OF IT. Dragging it OUT is how a notch is
	// created; hiding it at width 0 left the picture with no way to make one, so the scope stopped
	// being a control exactly when a hand most needed it to be one. "No notch is drawn" is literally
	// true at the defaults — Width is 0, so there IS no notch — and with
	// the shoulder gone there was nothing on the picture to fix that with.
	//
	// THE HONESTY RULE POINTS THE OTHER WAY HERE, which is worth stating because it usually does not:
	// a control is enabled iff it changes sound, and dragging the shoulder at width 0 DOES change
	// sound. It is the trough that would be the dead control.
	if (grip == Grip::Trough && s.width <= 0.0f) {
		return {};
	}
	// AND NEITHER IN HIGH-PASS (backlog NL): the brickwall has one shape, so Depth and Width reach nothing and their
	// handles would be the dead controls this comment is about.
	if (s.highpass && (grip == Grip::Trough || grip == Grip::Shoulder)) {
		return {};
	}
	switch (grip) {
		case Grip::Trough:
			// AT THE BOTTOM OF THE TROUGH, WHICH IS ALREADY DRAWN AT `1 - depth`. The y-axis is linear
			// amplitude precisely so this is true; nothing here converts anything.
			return square(notchX, box.bottom() - box.height() * (1.0f - std::clamp(s.depth, 0.0f, 1.0f)));
		case Grip::Stem:
			// BESIDE THE STEM, NOT ON IT, and one grip-diameter to the left.
			//
			// THE STEM AND THE TROUGH SHARE AN X BY DESIGN — the notch tracks the oscillator, which is
			// the coupling that makes HARM one device — so their handles coincide whenever
			// `level == 1 - depth`. That is not a corner of the space, it is a whole surface through
			// the middle of it, and the sweep found it. Ordering cannot fix a permanent coincidence:
			// whichever is tested first, the other is unreachable across that surface.
			//
			// So the ring sits beside the stem's head and a short tick joins it, which is a HANDLE FOR
			// a mark rather than a claim about where the mark is. The alternative — leaving Level
			// grabbable only where Depth happens not to be — is worse and invisible.
			//
			// AND OFFSET FROM THE TROUGH'S *CLAMPED* POSITION, ON WHICHEVER SIDE HAS ROOM.
			//
			// TWO FURTHER COLLISIONS THE SWEEP FOUND, both at the ends of the harmonic table and
			// neither visible at any default. Offsetting from the UNCLAMPED x and then clamping both
			// does not preserve the separation — at 32:1 the trough clamps left to the plot edge and
			// the stem lands half a diameter away, back inside it. **A separation applied before a
			// clamp is not a separation.** So the clamp is applied first and the offset measured from
			// the result.
			{
				const float lo = box.x() + kGripRadius;
				const float hi = box.right() - kGripRadius;
				const float anchor = std::clamp(oscillatorX(), lo, hi);
				const float step = 2.0f * kGripRadius;
				const float cx = anchor - step >= lo ? anchor - step : anchor + step;
				return square(cx, box.bottom() - box.height() * oscillatorStem());
			}
		case Grip::Shoulder: {
			// WHERE THE NOTCH HAS HALF RECOVERED — found by walking the curve outward rather than by
			// an offset somebody chose, so the handle is literally on the shoulder and moves visibly
			// when Width does.
			//
			// A FIXED OFFSET WAS THE FIRST TRY AND IT COLLAPSED. `log2(1 + 1/Q)` over this axis is
			// about 1.2 px at the default width, so the handle clamped to its floor and stopped
			// tracking Width at all — a control whose picture does not move when it moves.
			return square(shoulderX(), box.y() + box.height() * 0.5f);
		}
		case Grip::Notch:
			// IN A RAIL OF ITS OWN AT THE TOP, NOT DOWN THE COLUMN.
			//
			// THE COLUMN WAS THE FIRST TRY AND THE HANDLES ATE EACH OTHER: the shoulder sits at mid
			// height ON the column, so at any narrow width its box covered the column's centre and
			// `gripAt` answered Shoulder for a press aimed at the notch. Overlapping grab regions
			// cannot be ordered out of existence — one of them is always unreachable somewhere in the
			// parameter space — so the horizontal drag gets a lane nothing else may enter.
			return { std::clamp(notchX, box.x() + kGripRadius, box.right() - kGripRadius)
			             - kGripRadius,
				     box.y(), 2.0f * kGripRadius, kRailHeight };
		case Grip::None:
			break;
	}
	return {};
}

void HarmScope::setGripsEnabled(bool enabled) {
	if (gripsEnabled_ == enabled) {
		return;
	}
	gripsEnabled_ = enabled;
	// A GRIP HELD WHEN THE SWITCH GOES OFF WOULD BE A DRAG NOTHING CAN END.
	held_ = Grip::None;
	routeDrag_ = false;
	redraw();
}

HarmScope::Grip HarmScope::gripAt(visage::Point point) const {
	// THE PICTURE IS DISPLAY ONLY UNLESS SOMEBODY TURNS IT ON. One question, asked once, and asked
	// HERE — so the drawing, the drag handlers and the hit test all go quiet together and none of
	// them can disagree about whether the grips exist.
	if (!gripsEnabled_) {
		return Grip::None;
	}
	if (!plotBounds().contains(point)) {
		return Grip::None;
	}
	// THE RAIL FIRST, because it is the one lane nothing else may enter — and then the rest, smallest
	// target first, which is the rule every hit test in this editor follows.
	//
	// TROUGH BEFORE STEM IS A REAL TIE-BREAK, not an accident of the list: both sit at the notch's x
	// when the oscillator is sounding, and they coincide whenever `depth + level` is about 1. The
	// trough wins, because Depth is the one the curve under the pointer is drawn from.
	for (const Grip grip : { Grip::Notch, Grip::Trough, Grip::Stem, Grip::Shoulder }) {
		if (gripBounds(grip).contains(point)) {
			return grip;
		}
	}
	return Grip::None;
}

double HarmScope::valueForDrag(Grip grip, visage::Point at, bool fine) const {
	const visage::Bounds box = plotBounds();
	const device::ParamDesc& desc = hrm::kParamDescs[static_cast<size_t>(paramFor(grip))];
	switch (grip) {
		case Grip::Notch:
			(void)fine;
			return harmonicForX(at.x + anchorNotchDx_);
		case Grip::Trough: {
			// DEPTH IS `1 - y`, which is what the linear amplitude axis buys: the handle is already
			// drawn at the height it means.
			const double up = std::clamp((box.bottom() - at.y) / std::max(1.0f, box.height()), 0.0f,
			                             1.0f);
			return std::clamp(1.0 - up, desc.min, desc.max);
		}
		case Grip::Stem: {
			const double up = std::clamp((box.bottom() - at.y) / std::max(1.0f, box.height()), 0.0f,
			                             1.0f);
			return std::clamp(up, desc.min, desc.max);
		}
		case Grip::Shoulder: {
			// WIDER IS FURTHER OUT. The distance from the notch in octaves maps onto the width knob's
			// own 0..1, so the shape follows the hand rather than the hand following a scale factor.
			const double octaves =
			    std::abs(static_cast<double>(at.x - notchX())) / std::max(1.0f, box.width()) * 8.0;
			return std::clamp(octaves / 0.75, desc.min, desc.max);
		}
		case Grip::None:
			break;
	}
	return 0.0;
}

void HarmScope::draw(visage::Canvas& canvas) {
	const visage::Bounds box = plotBounds();
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 3.0f);

	// THE OCTAVE GRID, and 1:1 drawn brighter than the rest. The fundamental is the landmark the
	// whole axis is relative to; without it "3:2" is a curve at an arbitrary place on a box.
	for (double ratio = hrm::kProfileLoRatio; ratio <= hrm::kProfileHiRatio * 1.0001; ratio *= 2.0) {
		const bool unison = std::abs(ratio - 1.0) < 1e-9;
		canvas.setColor(withAlpha(unison ? kAccent : kLine, unison ? 0.55f : 0.35f));
		canvas.fill(xForRatio(ratio), box.y(), 1.0f, box.height());
	}

	// UNITY, so a notch's depth is read against the level it cuts from rather than against the top
	// of a box. It is the top edge here — the axis is linear amplitude and nothing exceeds 1.
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.fill(box.x(), box.y(), box.width(), 1.0f);

	// EVERY ACTIVE PARTIAL, the selected one emphasised and the others dimmer, and the selected one LAST so it is on top.
	// Each is its own notch (its curve) and its own sine (its stem, a dot at the top, and its number): the plan is
	// `drawPlan()`, the same list a case reads, so what is asserted is what is drawn.
	const std::vector<PlanEntry> plan = drawPlan();
	const hrm::Settings full = fullSettings();
	const auto stemOf = [&](const PlanEntry& e) { return box.bottom() - box.height() * e.stem; };
	for (const PlanEntry& e : plan) {
		const float emphasis = e.selected ? 1.0f : 0.45f;
		const float alpha = !e.active ? 0.2f : emphasis;
		if (e.stem > 0.0f && std::isfinite(e.oscillatorX)) {
			canvas.setColor(withAlpha(kValue, 0.45f * alpha));
			canvas.fill(e.oscillatorX - 0.5f, stemOf(e), 1.5f, box.height() * e.stem);
		}
	}

	// A FIXED BUFFER RATHER THAN AN ALLOCATION PER FRAME — draw() runs on every frame of a knob drag.
	std::array<float, kMaxCurvePoints> gainStore {};
	for (const PlanEntry& e : plan) {
		const size_t points = curveGainsFor(e.partial, gainStore.data(), gainStore.size());
		if (points < 2) {
			continue;
		}
		const std::span<const float> gains(gainStore.data(), points);
		const auto pointAt = [&](size_t i) {
			const float t = static_cast<float>(i) / static_cast<float>(points - 1);
			const float magnitude = std::clamp(gains[i], 0.0f, 1.0f);
			return visage::Point { box.x() + box.width() * t, box.bottom() - box.height() * magnitude };
		};
		int lastBand = -1;
		for (size_t i = 1; i < points; ++i) {
			if (e.selected && e.active) {
				const float t = static_cast<float>(i) / static_cast<float>(points - 1);
				const int band = static_cast<int>(std::lround(t * (kColourBands - 1)));
				if (band != lastBand) {
					lastBand = band;
					const float banded = static_cast<float>(band) / static_cast<float>(kColourBands - 1);
					canvas.setColor(blendColour(desaturate(kAccent, kCurveDesaturation),
					                            desaturate(kValue, kCurveDesaturation), banded));
				}
			}
			else if (lastBand != 0) {
				lastBand = 0;
				canvas.setColor(withAlpha(kValue, e.active ? 0.4f : 0.15f));
			}
			const visage::Point from = pointAt(i - 1);
			const visage::Point to = pointAt(i);
			// EVERY COMPUTED POINT FINITE-CHECKED BEFORE THE RENDERER — wave_display's rule, and this is
			// exactly the case it exists for: these come from an evaluated response rather than from a
			// clamped parameter, and a non-finite one hands the renderer a garbage quad.
			if (!std::isfinite(from.y) || !std::isfinite(to.y)) {
				continue;
			}
			canvas.segment(from.x, from.y, to.x, to.y, e.selected ? 1.5f : 1.0f, true);
		}
	}
	(void)full;

	// THE GRIPS, LAST AND ON TOP OF EVERYTHING.
	//
	// DRAWN AS RINGS, NOT DOTS, AND THAT IS THE WHOLE OF WHY THEY READ AS HANDLES. A filled mark on a
	// graph is a data point; a ring is a thing to take hold of. The distinction matters here more
	// than usual because the oscillator's head IS a filled dot two pixels away and means something
	// else — this editor's own rule, that a different verb gets a different KIND of mark, and the
	// morning's expand button proved what happens when two marks converge on the glass.
	//
	// AND THEY LIGHT WHEN A SOURCE IS ARMED, exactly as a knob's ring does — because they ARE
	// modulation destinations, and a destination that did not say so would be one no hand knows to
	// aim at.
	for (const Grip grip : { Grip::Notch, Grip::Trough, Grip::Stem, Grip::Shoulder }) {
		// NOT DRAWN WHILE THEY CANNOT BE TAKEN. A handle a hand cannot grab is decoration that looks
		// like a control, which is the sharper half of the same rule that put them here.
		if (!gripsEnabled_) {
			break;
		}
		const visage::Bounds g = gripBounds(grip);
		if (g.width() <= 0.0f) {
			continue;
		}
		// THE NOTCH GRIP IS A COLUMN and gets a bar rather than a ring: a ring the height of the plot
		// would be an ellipse round the whole picture.
		const bool armed = gripMappable(grip);
		canvas.setColor(withAlpha(armed ? kModulation : kMuted, held_ == grip ? 0.95f : 0.55f));
		if (grip == Grip::Notch) {
			// A SCRUBBER BAR IN THE RAIL, not two hairlines. The first drawing was a pair of 1px
			// ticks and the render showed them reading as part of the grid they sit among — a handle
			// nobody would try to grab. A filled bar in a lane of its own says "drag me sideways",
			// which is the one thing this grip does.
			const float bar = 3.0f;
			canvas.fill(g.x() + 1.0f, g.y() + (g.height() - bar) * 0.5f, g.width() - 2.0f, bar);
			continue;
		}
		const float ring = 4.5f;
		const float cx = g.x() + g.width() * 0.5f;
		const float cy = g.y() + g.height() * 0.5f;
		canvas.roundedRectangleBorder(cx - ring, cy - ring, 2.0f * ring, 2.0f * ring, ring, 1.4f);
		// AND A TICK JOINING THE STEM'S RING TO THE STEM, because the ring is offset from the mark it
		// takes hold of and an unattached circle beside a line is two things rather than one.
		if (grip == Grip::Stem) {
			canvas.segment(cx + ring, cy, oscillatorX(), cy, 1.0f, true);
		}
	}

	// AND EACH OSCILLATOR'S HEAD LAST, ON TOP OF THE CURVES.
	//
	// BECAUSE THE TWO MARKS COINCIDE BY DESIGN AND ONE THEREFORE HIDES THE OTHER. MEASURED on the
	// first render: at Level 0.7 and Depth 0.8 the stem was drawn to its full 70% and only the
	// bottom 24% of it was visible, because the notch's own line descends the same column and is
	// drawn after it. Every geometry assertion passed — the stem was at the right x and the right
	// height — and the picture still did not show Level at all. A dot at the stem's top is a mark a
	// 1.5 px line cannot swallow. THE PARTIAL'S NUMBER SITS ABOVE IT, so several heads can be told apart.
	for (const PlanEntry& e : plan) {
		if (!(e.stem > 0.0f) || !std::isfinite(e.oscillatorX)) {
			continue;
		}
		const float emphasis = !e.active ? 0.2f : (e.selected ? 1.0f : 0.55f);
		constexpr float kHead = 3.0f;
		canvas.setColor(withAlpha(kValue, emphasis));
		canvas.circle(e.oscillatorX - kHead * 0.5f, stemOf(e) - kHead * 0.5f, kHead);
		canvas.setColor(withAlpha(e.selected ? kInk : kMuted, std::max(emphasis, 0.7f)));
		canvas.text(std::to_string(e.partial + 1), font(kKnobLabelSize), visage::Font::kLeft, e.oscillatorX + 4.0f,
		            std::max(box.y() + 3.0f, stemOf(e) - 2.0f), 12.0f, 10.0f);
	}
}

std::vector<HarmScope::PlanEntry> HarmScope::drawPlan() const {
	// THE ACTIVE PARTIALS, IN ORDER, WITH THE SELECTED ONE LAST — plus the selected one when it is switched off, as a
	// ghost, so the picture says where the partial would sit if it were turned on. A partial that is neither active
	// nor selected is not drawn at all: it is not in the sound.
	const hrm::Settings full = fullSettings();
	std::vector<PlanEntry> out;
	out.reserve(hrm::kPartialCount);
	for (int pass = 0; pass < 2; ++pass) {
		for (int k = 0; k < hrm::kPartialCount; ++k) {
			const bool selected = k == partial_;
			if ((pass == 1) != selected) {
				continue;
			}
			const hrm::Settings slice = hrm::partialSlice(full, k);
			if (!slice.active && !selected) {
				continue;
			}
			PlanEntry e;
			e.partial = k;
			e.selected = selected;
			e.active = slice.active;
			e.notchX = xForRatio(hrm::notchRatio(slice));
			e.oscillatorX = xForRatio(static_cast<double>(hrm::ratioAt(slice.harmonic))
			                          * std::exp2(static_cast<double>(slice.fineSemitones) / 12.0));
			e.stem = std::clamp(slice.level, 0.0f, 1.0f);
			out.push_back(e);
		}
	}
	return out;
}

size_t HarmScope::curveGainsFor(int partial, float* out, size_t capacity) const {
	if (out == nullptr || capacity == 0) {
		return 0;
	}
	const size_t points = std::min(capacity, curvePointCount());
	hrm::ratioProfile(hrm::partialSlice(fullSettings(), partial), nullptr, out, points);
	return points;
}

void HarmScope::offeredControls(std::vector<Offered>& out) const {
	// THE NOTCH AND ITS HARMONIC, dragged (`HarmScope`'s header): side roads, for the selected partial.
	// DEPTH AND WIDTH ONLY WHILE THEIR GRIPS EXIST — in High-pass `gripBounds` gives them none (backlog NL).
	const bool highpass = settings().highpass;
	for (const hrm::PartialField f : { hrm::PartialField::Harmonic, hrm::PartialField::Depth, hrm::PartialField::Width,
	                                    hrm::PartialField::Level }) {
		if (highpass && (f == hrm::PartialField::Depth || f == hrm::PartialField::Width)) {
			continue;
		}
		out.push_back({ &control(hrm::partialParam(partial_, f)), Road::Gesture });
	}
}

} // namespace feathers::ui
