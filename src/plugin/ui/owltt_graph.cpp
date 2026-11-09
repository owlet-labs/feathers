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

#include "plugin/ui/owltt_graph.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace feathers::ui {

namespace {

namespace owltt = feathers::fx::owltt;

/// THE FREQUENCY AXIS, BOTH WAYS, FROM ONE PAIR OF LINES.
///
/// The forward map existed and the inverse did not, which is what made the crossovers knobs. Writing
/// the inverse FORWARDS -- deriving a second formula that ought to agree -- is the exact seam that
/// has cost this file three bugs, so `xToHz` is `hzToX` read backwards and the round trip is pinned
/// by a case rather than by inspection.
constexpr float kAxisLoHz = 20.0f;
constexpr float kAxisHiHz = 20000.0f;

float axisPosition(float hz) {
	return std::clamp(std::log2(hz / kAxisLoHz) / std::log2(kAxisHiHz / kAxisLoHz), 0.0f, 1.0f);
}

float hzToX(float hz, const visage::Bounds& box) {
	return box.x() + axisPosition(hz) * box.width();
}

float xToHz(float x, const visage::Bounds& box) {
	const float t =
	    box.width() > 0.0f ? std::clamp((x - box.x()) / box.width(), 0.0f, 1.0f) : 0.0f;
	return kAxisLoHz * std::pow(2.0f, t * std::log2(kAxisHiHz / kAxisLoHz));
}


/// Map a threshold value (0.2..1.0) to a y-position within the crossover box.
/// 0.2 = bottom, 1.0 = top.
float thresholdToY(float threshold, const visage::Bounds& xover) {
	const float t = std::clamp((threshold - 0.2f) / 0.8f, 0.0f, 1.0f);
	return xover.bottom() - t * xover.height();
}

/// Map a y-position within the crossover box to a threshold value (0.2..1.0).
/// WHERE A BAND'S RATIO AND LEVEL STRIPS LIVE — asked once, by both the drawing and the hit test.
///
/// THESE WERE TWO SETS OF NUMBERS AND THEY DISAGREED. The draw put the strips at 0.70..0.85 and
/// 0.85..1.00 of the band; the hit test looked for them at 0.33..0.67. So the visible strips hit
/// NOTHING -- the yellow bars did not seem draggable -- and the middle of the band,
/// where the GR meter is drawn, silently dragged ratio and level instead. One bug the hand can see
/// and one it cannot.
///
/// Sixth instance this week of two places that must agree about one fact, and the fix is the one
/// that has held every time: not making the numbers match, but having ONE number.
struct StripSpan {
	float x0;
	float mid;
	float x1;
};

/// IS THIS PRESS ABOUT THE ROUTE OR ABOUT THE VALUE — asked once, by the drag and by the
/// double-click both.
///
/// `Knob::mappable`'s question in this file's terms: armed, allowed to reach this control, and Alt
/// not held to bypass. It was written inline in the drag branch, and the double-click added beside
/// it needed the same answer — which is how a second copy gets written, and this file has already
/// paid for two of those in one week. One predicate, so the two gestures cannot come to disagree
/// about whether a source is armed.
inline bool routeGestureWanted(const device::Modulatable& ctrl, const visage::MouseEvent& e) {
	return !e.isAltDown() && ctrl.display().routable;
}


inline StripSpan stripSpanOf(float regionX0, float regionX1) {
	const float w = regionX1 - regionX0;
	return { regionX0 + w * 0.70f, regionX0 + w * 0.85f, regionX0 + w * 1.00f };
}

/// WHERE A THRESHOLD BAR'S HANDLE RING SITS — one definition, asked by the drawing AND the hit test.
///
/// TWO NUMBERS THAT MUST AGREE, so they are one number. They arrived as two identical copies, and
/// this file has now produced that shape eight times in a week; the copies are never wrong on the
/// day they are written, they are wrong on the day one of them is edited.
///
/// AND THE RINGS STAY OUT OF THE STRIPS' SPAN, which is a correctness constraint rather than a
/// tidiness one. `hitTestThreshold` gives the strips the band's right 30% at EVERY height, so a ring
/// drawn inside that span is drawn and ungrabbable — the hit test has already given its pixels away
/// before the bar is asked. As written, the lower ring sat at 0.75 of the band and the strips begin
/// at 0.70. So the pair is placed inside what the bars actually keep, derived from `stripSpanOf`
/// rather than from a remembered 0.70: if the strips ever widen, the rings move with them.
struct RingPair {
	float upper;
	float lower;
};

inline RingPair ringXsOf(float regionX0, float regionX1) {
	const float usable = stripSpanOf(regionX0, regionX1).x0 - regionX0;
	return { regionX0 + usable * 0.30f, regionX0 + usable * 0.70f };
}


/// A HANDLE RING AT A CENTRE — because `visage::Canvas::circle` does not take one.
///
/// `circle(x, y, width)` and `ring(x, y, width, thickness)` take the TOP-LEFT of the bounding box.
/// Every other caller in this editor knows that and writes `circle(cx - r, cy - r, r * 2)`:
/// celestial.cpp, value_box.cpp, harm_scope.cpp, mseg_card.cpp, perform_stage.cpp. The OWLTT rings
/// arrived passing the centre straight through, so all twelve of them drew four pixels down and
/// right of the thing they sit on.
///
/// IT SHOWED AS mystery handles below the band view that did not seem to move or control anything,
/// and both halves of that are the one offset. A strip's ring sits at its VALUE, so a strip
/// near the bottom put its ring at `xover.bottom()` — and drawn from there as a top-left, the whole
/// ring rendered BELOW the graph. Outside the box, `hitTestThreshold` returns `kNone` before it looks
/// at anything, so the ring was also unreachable. Drawn out of the box it is grabbable in.
///
/// A FUNCTION RATHER THAN THE SUBTRACTION REPEATED EIGHT TIMES. The convention is the trap; writing
/// it once is the only fix that does not depend on the next person having read this comment.
inline void ringAt(visage::Canvas& canvas, float cx, float cy, float radius) {
	canvas.circle(cx - radius, cy - radius, radius * 2.0f);
}

inline void ringOutlineAt(visage::Canvas& canvas, float cx, float cy, float radius, float thickness) {
	canvas.ring(cx - radius, cy - radius, radius * 2.0f, thickness);
}

/// KEEP A HANDLE'S CENTRE FAR ENOUGH INSIDE THE PLOT THAT ALL OF IT IS REACHABLE.
///
/// HARM ALREADY PAID FOR THIS AND SAID SO (`harm_scope.cpp:324`): *"the extreme of its range -- Level
/// 0, Depth 1 -- sat half outside the plot, and `gripAt` refuses a point the plot does not contain,
/// so THE CONTROL WAS UNREACHABLE EXACTLY WHERE A HAND MOST WANTS IT: at the end of its travel, which
/// is where you go to bring it back."*
///
/// The same thing is true here for the same reason. A strip's ring sits at its VALUE, so at 0 the
/// centre lands on `xover.bottom()` and at 1 on `xover.y()` — and `hitTestThreshold` opens with
/// `xover.contains(pos)`, which a point on the bottom edge fails. A control that cannot be grabbed at
/// either end of its travel is one you cannot drag back from the end you just dragged it to.
inline float ringYInside(float y, const visage::Bounds& box) {
	return std::clamp(y, box.y() + OwlttGraph::kRingRadius, box.bottom() - OwlttGraph::kRingRadius);
}

/// WHERE A BAND'S TWO BARS SIT, as threshold values — the DSP's own relation, written once.
///
/// `owltt.cpp` computes `thresholdUp = thresholdDown + 0.6 * bandwidth`. So the param spelled
/// `Threshold` IS the DOWN threshold and belongs at the LOWER edge, and the card had it the other
/// way up: it drew Threshold at the TOP and subtracted to find the bottom, which made the picture
/// claim the wrong edge was the thing the parameter names.
///
/// THE MERGE THAT BROUGHT THE FIX IS WHY THIS IS A FUNCTION. The swap arrived from another session
/// and landed in `draw` and in the two drag branches; `hitTestThreshold` kept the OLD relation,
/// because the conflict there was about something else entirely and resolving it preserved the half
/// nobody was looking at. The bars were then DRAWN from one formula and GRABBED by another — the
/// ninth instance of two-places-must-agree in this file this week, and the first one produced by a
/// merge rather than by an edit.
///
/// Clamped exactly where the drawing clamped: the lower edge floors at 0.2 (the bottom of the box's
/// threshold sweep) and the upper ceilings at 1.0, so a wide gap at a high threshold does not draw
/// off the top.
struct BarPair {
	float lower;
	float upper;
};

inline BarPair barValuesOf(float threshold, float bandwidth) {
	const float lower = std::clamp(threshold, 0.2f, 1.0f);
	return { lower, std::min(1.0f, lower + 0.6f * bandwidth) };
}


float yToThreshold(float y, const visage::Bounds& xover) {
	const float t = std::clamp((xover.bottom() - y) / xover.height(), 0.0f, 1.0f);
	return 0.2f + t * 0.8f;
}


struct BandParamIndices {
	size_t threshold;
	size_t bandwidth;
	size_t ratio;
	size_t level;
};

static constexpr std::array<BandParamIndices, 3> kBandParams = {{
    {static_cast<size_t>(owltt::Param::LowThreshold), static_cast<size_t>(owltt::Param::LowBandwidth),
     static_cast<size_t>(owltt::Param::LowRatio), static_cast<size_t>(owltt::Param::LowLevel)},
    {static_cast<size_t>(owltt::Param::MidThreshold), static_cast<size_t>(owltt::Param::MidBandwidth),
     static_cast<size_t>(owltt::Param::MidRatio), static_cast<size_t>(owltt::Param::MidLevel)},
    {static_cast<size_t>(owltt::Param::HighThreshold), static_cast<size_t>(owltt::Param::HighBandwidth),
     static_cast<size_t>(owltt::Param::HighRatio), static_cast<size_t>(owltt::Param::HighLevel)},
}};


} // namespace

OwlttGraph::OwlttGraph(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)notifier_;
	onMouseDown() = [this](const visage::MouseEvent& e) {
#ifdef FEATHERS_DIAGNOSTIC
		// SAYS IT WAS REACHED AT ALL, which is the fact no test could give me: the fixture says this
		// line runs and the host says nothing in the rectangle responds, so one of those two is
		// answering a different question and this is which.
		diagnostic::plotPressed(e.position.x, e.position.y,
		                        static_cast<int>(hitTestThreshold(e.position)), width(), height());
#endif
	// THE PRESS ROAD, WHICH IS NOW THE WHOLE OF THIS FRAME'S MOUSE HANDLING.
	//
	// On the card these branches sat under the head's slot menu, enable square, expand mark and fold,
	// and had to be ordered against them -- `hitTestThreshold(...) == kNone` gated `noteCardPress` so
	// one drag could not both move a threshold AND pick the card out of the lane. A frame that owns
	// only the plot has none of that: everything here is the graph's, and the card's head is a
	// sibling that never sees these events.
	// THRESHOLD BAR HIT TEST — check before the head's fold swallows it.
	const auto hit = hitTestThreshold(e.position);
	if (hit != ThresholdHit::kNone) {
		auto& ctrl = controlForHit(hit);

		// DOUBLE-CLICK, WHICH THE BARS SIMPLY DID NOT HAVE. A double-click on a bar did not reset
		// it to its default, and nothing was broken — the
		// branch had never been written. `Knob::onMouseDown` asks `repeatClickCount()` twice
		// before it anchors anything; this handler reimplemented the DRAG faithfully, down to
		// borrowing Knob's own constants, and did not reimplement the gesture that is not a
		// drag. A press that was meant to reset simply started a second drag from where the
		// bar already sat, which looks exactly like a double-click doing nothing.
		//
		// KNOB'S ORDER, NOT A CONVENIENT ONE: the mapping question is asked FIRST, so while a
		// source is armed a double-click drops the ROUTE and leaves the value where the hand
		// put it. Resetting the value there would throw away an edit the hand did not aim at,
		// and worse, would be the opposite of what the same gesture does on every knob in the
		// window six pixels away.
		//
		// Both gestures come from `device::Modulatable` so they ARE Knob's, rather than
		// matching Knob's today. A bar and a dial pointing at the same parameter cannot
		// disagree about what its default is.
		if (e.repeatClickCount() >= 2) {
			if (routeGestureWanted(ctrl, e)) {
				ctrl.dropRoute();
			}
			else {
				ctrl.resetToDefault();
			}
			// NO DRAG ARMED — and this branch sits ABOVE the three lines that arm one for
			// a reason paid for immediately. It was first written below them, with this same
			// comment claiming the flag stayed false while the line setting it true was four
			// lines up. The reset landed and was then dragged straight back off the default
			// by the very next motion, so the case's first assertion passed and only the
			// subcase failed. A comment stating a rule is not the rule being followed.
			redraw();
			return;
		}

		dragState_.active = true;
		dragState_.hit = hit;
		dragState_.startY = e.position.y;

		// MODULATION ROAD — copy Knob's branch exactly (knob.cpp:358-366).
		// Anchor on the existing amount (or seed 0.4 if no route), then drag relative.
		if (routeGestureWanted(ctrl, e)) {
			dragState_.routeDragging = true;
			// TAKEN FROM KNOB, NOT COPIED FROM IT. These were three local floats carrying
			// the right values under a comment reading "Same as Knob" — which is a promise
			// no compiler checks, and this week has four instances of exactly that promise
			// being broken by an edit to one side. `Knob`'s are public and this file already
			// includes `knob.h`; `device::kMaxAmount` is the shared ceiling `setAmount`
			// already clamps to. One definition each, so a change to the gesture reaches
			// every surface that uses it.
			const float routeBefore = ctrl.display().amount;
			dragState_.routeAnchorAmount =
			    (routeBefore != 0.0f) ? routeBefore : Knob::kRouteSeedAmount;
			dragState_.routeDragRange = Knob::kRouteDragRange;
			dragState_.maxRouteAmount = device::kMaxAmount;
			ctrl.beginAmountGesture();
			ctrl.setAmount(dragState_.routeAnchorAmount);
		}
		else {
			dragState_.routeDragging = false;
			ctrl.beginGesture();
		}
	}
	};

	// THE HOVER ROAD, which is what a tooltip is. `onMouseMove` is delivered with NO button held --
	// the distinction this file learned the hard way when the bars could not be dragged, because the
	// drag was written on this slot by mistake. Here it is the right one.
	onMouseMove() = [this](const visage::MouseEvent& e) {
		hover_ = e.position;
		hovering_ = true;
		redraw();
	};
	onMouseExit() = [this](const visage::MouseEvent&) {
		hovering_ = false;
		redraw();
	};

	// onMouseDrag, NOT onMouseMove, AND ADDED RATHER THAN ASSIGNED.
	//
	// THIS WAS `onMouseMove() = ...` AND NOTHING IN THE GRAPH COULD BE DRAGGED. visage delivers
	// `onMouseMove` for a HOVER -- pointer moving with no button held -- and `onMouseDrag` while a
	// button is down. Every other draggable thing in this editor uses the latter: `FxCard`'s own
	// card-reorder handler and `Knob`'s value drag both. The bars received the press and the release
	// and never a single motion between them.
	//
	// AND `+=` RATHER THAN `=`, because `FxCard`'s constructor already put the card-reorder gesture
	// on this slot. `CallbackList` appends, so both run; assigning would have traded one broken
	// gesture for another, which is the shape of half the fixes in this file's history.
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!dragState_.active) {
			return;
		}
		auto& ctrl = controlForHit(dragState_.hit);
		// THE BAR AREA, for the same reason `ringCentre` uses it: a bar is DRAWN in this rectangle and
		// HIT-TESTED in it, so it must be VALUED in it too. Left as `plot()` the drag mapped the
		// pointer against a rectangle 16px taller than the one the bar lives in — off by a lane, in
		// the one place where being off by anything means the value the hand sets is not the value it
		// aimed at. Third rectangle in one function to get this wrong, which is why they are now one
		// call rather than three literals.
		const visage::Bounds xover = barArea();

		// THE CROSSOVERS MOVE SIDEWAYS, the only gesture on this plot that does. A frequency split IS
		// a position on the axis, so the pointer's x is the value — read back through the SAME
		// mapping `bandEdge` uses forwards, never a second formula derived to agree with it.
		//
		// CLAMPED BY THE CONTROL'S OWN DECLARATION: `setValue` rounds and clamps by the descriptor,
		// so 20..2000 and 200..20000 are enforced where they are declared rather than restated here.
		if (dragState_.hit == ThresholdHit::kLowCrossover
		    || dragState_.hit == ThresholdHit::kHighCrossover) {
			// THE FULL PLOT for the axis, not the bar area: the lane is a horizontal strip, so it
			// removes height and never width, and the frequency sweep spans the whole picture.
			// KEY TRACK: the corner under the hand is the note moved by the knob's offset, so the knob written is the one
			// whose offset puts it there (`trackedCrossoverHz` read backwards).
			const double hz = static_cast<double>(xToHz(e.position.x, plot()));
			const double note = noteHz();
			ctrl.setValue(note > 0.0 ? owltt::kCrossoverKeyRefHz * hz / note : hz);
			redraw();
			return;
		}

		if (dragState_.routeDragging) {
			// ROUTE DRAGGING — vertical travel relative to anchor (Knob's approach).
			const float travel = (dragState_.startY - e.position.y) / dragState_.routeDragRange;
			ctrl.setAmount(std::clamp(dragState_.routeAnchorAmount + travel,
			                           -dragState_.maxRouteAmount, dragState_.maxRouteAmount));
		}
		else if (isUpperHit(dragState_.hit)) {
			// THE UPPER BAR IS NOT THE THRESHOLD — IT IS THRESHOLD + 0.6*BANDWIDTH.
			//
			// The device computes `thresholdUp = thresholdDown + 0.6 * bandwidth`, so the param called
			// `Threshold` IS the DOWN threshold and belongs at the LOWER edge. The card drew it at the
			// top and subtracted; the picture claimed the wrong edge was the threshold.
			//
			// Swapping which bar writes what: the upper bar now writes bandwidth (resizing the gap
			// upward), and the lower bar writes the threshold itself.
			// AND EVERY QUANTITY HERE IS EFFECTIVE, then converted back at the last step. The pointer
			// says where the EDGE should be on screen; the edge is drawn from the effective values;
			// so the gap is solved in effective terms and only the write is in the band's own.
			const size_t band = bandForHit(dragState_.hit);
			const owltt::EffectiveBand eff = effective(band);
			const float wanted = yToThreshold(e.position.y, xover);
			// Upper edge = lower + 0.6*bandwidth, so the effective bandwidth is (wanted - lower)/0.6.
			const float wantedBandwidth = std::clamp((wanted - eff.threshold) / 0.6f, 0.0f, 1.0f);
			ctrl.setValue(owltt::bandValueForEffective(wantedBandwidth,
			                                          shiftFor(Handle::UpperBar)));
		}
		else if (isThresholdHit(dragState_.hit)) {
			// THE LOWER BAR IS THE THRESHOLD ITSELF — so the pointer's height is the EFFECTIVE value,
			// and what gets stored is that minus whatever the global is contributing. Without the
			// subtraction the bar jumps away from the pointer by the global's displacement the moment
			// the global is anywhere but neutral, which is the same bug as drawing the stored value:
			// the picture and the device disagreeing about what a threshold is.
			ctrl.setValue(owltt::bandValueForEffective(yToThreshold(e.position.y, xover),
			                                          shiftFor(Handle::LowerBar)));
		}
		else {
			// A RATIO OR LEVEL STRIP — the pointer's height is the effective value, converted back the
			// same way. Level's shift is zero, so this reads identically for both and neither needs a
			// branch saying which.
			const float t = std::clamp((xover.bottom() - e.position.y) / xover.height(), 0.0f, 1.0f);
			const Handle which = (dragState_.hit == ThresholdHit::kLowRatio
			                      || dragState_.hit == ThresholdHit::kMidRatio
			                      || dragState_.hit == ThresholdHit::kHighRatio)
			                         ? Handle::RatioStrip
			                         : Handle::LevelStrip;
			ctrl.setValue(owltt::bandValueForEffective(t, shiftFor(which)));
		}
	};

	// APPENDED for the same reason as the drag: `FxCard` ends its reorder gesture here, and
	// replacing this slot dropped that.
	onMouseUp() = [this](const visage::MouseEvent& /*e*/) {
		if (!dragState_.active) {
			return;
		}
		auto& ctrl = controlForHit(dragState_.hit);
		if (dragState_.routeDragging) {
			// Commit the route — amount dragged back to nothing is not a route at 0%, it is gone.
			const float amount = std::abs(ctrl.display().amount) < 0.05f ? 0.0f : ctrl.display().amount;
			if (amount != ctrl.display().amount) {
				ctrl.setAmount(amount);
			}
			ctrl.endAmountGesture();
		}
		else {
			ctrl.endGesture();
		}
		dragState_.active = false;
		dragState_.hit = ThresholdHit::kNone;
		dragState_.routeDragging = false;
	};
}

std::string OwlttGraph::frequencyText(float hz) {
	// HOW A PLAYER READS A CROSSOVER, not how a float prints. Below a kHz the integer is what matters
	// and a decimal is noise; above it, one decimal is what distinguishes 2.0 from 2.4 kHz.
	char out[24] = {};
	if (hz < 1000.0f) {
		std::snprintf(out, sizeof(out), "%d Hz", static_cast<int>(std::lround(hz)));
	}
	else {
		std::snprintf(out, sizeof(out), "%.1f kHz", static_cast<double>(hz) / 1000.0);
	}
	return out;
}

OwlttGraph::BottomText OwlttGraph::bottomText() const {
	BottomText out;
	const visage::Bounds xover = barArea();
	const visage::Bounds whole = plot();
	const visage::Font face = font(kKnobReadoutSize);
	const auto widthOf = [&face](const std::string& s) { return face.stringWidth(std::u32string(s.begin(), s.end())); };
	constexpr float kRowHeight = 12.0f;
	constexpr float kRowStep = 13.0f;

	// THE LETTERS, where the meters put them: under each half of the band's gain-reduction column (the central third
	// of the band), centred — and only when a half is wide enough to hold one, as before.
	const bool ms = control(static_cast<size_t>(owltt::Param::StereoMode)).value() >= 0.5;
	const float xLow = bandEdge(0);
	const float xHigh = bandEdge(1);
	const std::array<std::pair<float, float>, 3> bands { std::pair { xover.x(), xLow }, std::pair { xLow, xHigh },
		                                                 std::pair { xHigh, xover.right() } };
	for (size_t b = 0; b < bands.size(); ++b) {
		const float regionW = bands[b].second - bands[b].first;
		if (regionW <= 0.0f) {
			continue;
		}
		const float grX0 = bands[b].first + regionW * 0.33f;
		const float grW = regionW * 0.34f;
		const float halfW = std::max(0.0f, (grW - kMeterSplitGap) * 0.5f);
		if (halfW < 8.0f) {
			continue;
		}
		for (size_t c = 0; c < 2; ++c) {
			const float glyph = widthOf(ms ? (c == 0 ? "M" : "S") : (c == 0 ? "L" : "R"));
			const float cell = grX0 + static_cast<float>(c) * (halfW + kMeterSplitGap);
			out.letters.push_back({ cell + (halfW - glyph) * 0.5f, xover.bottom() - 13.0f, glyph, kRowHeight });
			out.letterBand.push_back(b);
		}
	}

	// THE LABELS: inboard of their line on the side with room, on the bottom row — and up a row at a time while
	// they would touch a letter or the label already placed. A pixel of air either side counts as touching.
	const auto touches = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() - 1.0f < b.right() && b.x() - 1.0f < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};
	const std::array<float, 2> edges { xLow, xHigh };
	for (size_t which = 0; which < 2; ++which) {
		const auto param = which == 0 ? owltt::Param::LowCrossover : owltt::Param::HighCrossover;
		(void)param;
		out.frequencyText[which] = frequencyText(static_cast<float>(cornerHz(which)));
		const float w = widthOf(out.frequencyText[which]);
		const bool room = edges[which] + 4.0f + w <= whole.right();
		visage::Bounds label { room ? edges[which] + 4.0f : edges[which] - 4.0f - w, whole.bottom() - 14.0f, w,
			                   kRowHeight };
		for (int row = 0; row < 8; ++row) {
			bool clear = true;
			for (const visage::Bounds& letter : out.letters) {
				clear = clear && !touches(label, letter);
			}
			if (which == 1) {
				clear = clear && !touches(label, out.frequency[0]);
			}
			if (clear || label.y() - kRowStep < whole.y()) {
				break;
			}
			label = { label.x(), label.y() - kRowStep, label.width(), label.height() };
		}
		out.frequency[which] = label;
	}
	return out;
}

visage::Bounds OwlttGraph::plot() const {
	// ITS OWN RECTANGLE, IN ITS OWN COORDINATES. The card used to pass `plot()` in CARD
	// space and translate every press by hand; a frame that owns the plot has nothing to translate.
	return { 0.0f, 0.0f, width(), height() };
}

DeviceControl& OwlttGraph::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, owltt::kDescriptor, index);
	}
	return *controls_[index];
}

void OwlttGraph::setModulation(ModState* mod, EditHistory* history) {
	// EVERY ADAPTER, including the ones no widget holds — the graph's handles ARE its widgets, and
	// each reaches its control through one of these. HARM shipped without the equivalent line and
	// could not be modulated at all.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
}

DeviceControl& OwlttGraph::controlForHit(ThresholdHit hit) const {
	const size_t band = bandForHit(hit);
	switch (hit) {
	// SWAPPED WITH THE SIGN FIX, AND IT IS THE HALF THAT WAS MISSED. The drag branches were rewritten
	// so the UPPER bar sizes the gap and the LOWER bar carries the threshold; this mapping was not,
	// so the upper bar computed a BANDWIDTH and wrote it into the THRESHOLD control and the lower did
	// the reverse. Each control received the other's units.
	//
	// It is invisible to any check that only asks whether the drag moved something -- both roads move
	// something, both move plausibly, and the numbers stay inside 0..1. `(0.933 - 0.5) / 0.6 = 0.722`
	// is what a threshold looked like coming out of the bandwidth formula, and the only reason it was
	// caught is that a case asserted the VALUE the drag implies rather than that a value changed.
	case ThresholdHit::kLowUpper:
	case ThresholdHit::kMidUpper:
	case ThresholdHit::kHighUpper:
		return control(kBandParams[band].bandwidth);
	case ThresholdHit::kLowLower:
	case ThresholdHit::kMidLower:
	case ThresholdHit::kHighLower:
		return control(kBandParams[band].threshold);
	case ThresholdHit::kLowCrossover:
		return control(static_cast<size_t>(owltt::Param::LowCrossover));
	case ThresholdHit::kHighCrossover:
		return control(static_cast<size_t>(owltt::Param::HighCrossover));
	case ThresholdHit::kLowRatio:
	case ThresholdHit::kMidRatio:
	case ThresholdHit::kHighRatio:
		return control(kBandParams[band].ratio);
	case ThresholdHit::kLowLevel:
	case ThresholdHit::kMidLevel:
	case ThresholdHit::kHighLevel:
		return control(kBandParams[band].level);
	default:
		return control(0);
	}
}

size_t OwlttGraph::bandForHit(ThresholdHit hit) const {
	switch (hit) {
	case ThresholdHit::kLowUpper:
	case ThresholdHit::kLowLower:
	case ThresholdHit::kLowRatio:
	case ThresholdHit::kLowLevel:
		return 0;
	case ThresholdHit::kMidUpper:
	case ThresholdHit::kMidLower:
	case ThresholdHit::kMidRatio:
	case ThresholdHit::kMidLevel:
		return 1;
	case ThresholdHit::kHighUpper:
	case ThresholdHit::kHighLower:
	case ThresholdHit::kHighRatio:
	case ThresholdHit::kHighLevel:
		return 2;
	default:
		return 0;
	}
}

owltt::EffectiveBand OwlttGraph::effective(size_t band) const {
	// THE DEVICE'S OWN ARITHMETIC, asked of the device's own header. The graph reads the twelve
	// per-band controls and the three globals off the same rows the engine renders from, hands them
	// to `effectiveBandOf`, and draws the answer — so a bar is at the threshold the compressor is
	// USING rather than at the number its band happens to store.
	owltt::BandParams params {};
	params.threshold = static_cast<float>(control(kBandParams[band].threshold).value());
	params.ratio = static_cast<float>(control(kBandParams[band].ratio).value());
	params.bandwidth = static_cast<float>(control(kBandParams[band].bandwidth).value());
	params.level = static_cast<float>(control(kBandParams[band].level).value());

	owltt::Settings globals {};
	globals.threshold = static_cast<float>(control(static_cast<size_t>(owltt::Param::Threshold)).value());
	globals.ratio = static_cast<float>(control(static_cast<size_t>(owltt::Param::Ratio)).value());
	globals.bandwidth = static_cast<float>(control(static_cast<size_t>(owltt::Param::Bandwidth)).value());
	return owltt::effectiveBandOf(params, globals);
}

float OwlttGraph::shiftFor(Handle handle) const {
	// WHICH GLOBAL DISPLACES THIS HANDLE, so a drag can subtract it back out. Asked of the same three
	// functions the engine's shift comes from rather than restated as `value - 0.5`.
	owltt::Settings globals {};
	globals.threshold = static_cast<float>(control(static_cast<size_t>(owltt::Param::Threshold)).value());
	globals.ratio = static_cast<float>(control(static_cast<size_t>(owltt::Param::Ratio)).value());
	globals.bandwidth = static_cast<float>(control(static_cast<size_t>(owltt::Param::Bandwidth)).value());
	switch (handle) {
	case Handle::LowerBar:
		return owltt::thresholdShiftOf(globals);
	case Handle::UpperBar:
		return owltt::bandwidthShiftOf(globals);
	case Handle::RatioStrip:
		return owltt::ratioShiftOf(globals);
	case Handle::LevelStrip:
	default:
		// LEVEL HAS NO GLOBAL PARTNER, so its drag is unshifted. Returned rather than special-cased
		// at the call site, so the drag reads the same for all four.
		return 0.0f;
	}
}

OwlttGraph::BarYs OwlttGraph::barYs(size_t band) const {
	const owltt::EffectiveBand eff = effective(band);
	const BarPair values = barValuesOf(eff.threshold, eff.bandwidth);
	const visage::Bounds bars = barArea();
	return { thresholdToY(values.upper, bars), thresholdToY(values.lower, bars) };
}

visage::Point OwlttGraph::ringCentre(size_t band, Handle handle) const {
	// THE BAR AREA, NOT THE WHOLE PLOT — the one line that keeps a handle out of the crossover lane.
	//
	// This read `plot()`, so a threshold at the top of its sweep put its ring INSIDE the lane, where
	// the lane's hit test answers first and the bar becomes unreachable at exactly the end of its
	// travel. HARM recorded that failure (`harm_scope.cpp:324`) and the lane was designed against it;
	// the design was then undone here by a rectangle that had not been updated. Measured: ring at
	// y=4 with the lane ending at 16.
	const visage::Bounds xover = barArea();
	const float x0 = band == 0 ? xover.x() : bandEdge(band - 1);
	const float x1 = band == 2 ? xover.right() : bandEdge(band);
	// `bandEdge` is an x on the full plot; the bar area shares its x span, so only y differs.

	if (handle == Handle::UpperBar || handle == Handle::LowerBar) {
		const BarYs ys = barYs(band);
		const RingPair rings = ringXsOf(x0, x1);
		const bool upper = handle == Handle::UpperBar;
		return { upper ? rings.upper : rings.lower,
			     ringYInside(upper ? ys.upper : ys.lower, xover) };
	}

	// THE STRIPS. A strip's ring sits ON its value, which is what makes it a handle rather than a
	// decoration -- and is exactly why the ends of its travel land on the box's edges.
	const StripSpan strip = stripSpanOf(x0, x1);
	const bool ratio = handle == Handle::RatioStrip;
	const owltt::EffectiveBand eff = effective(band);
	const float value = ratio ? eff.ratio : eff.level;
	return { ratio ? (strip.x0 + strip.mid) * 0.5f : (strip.mid + strip.x1) * 0.5f,
		     ringYInside(xover.bottom() - value * xover.height(), xover) };
}

OwlttGraph::ThresholdHit OwlttGraph::hitFor(size_t band, Handle handle) const {
	static constexpr ThresholdHit kTable[3][4] = {
		{ ThresholdHit::kLowUpper, ThresholdHit::kLowLower, ThresholdHit::kLowRatio, ThresholdHit::kLowLevel },
		{ ThresholdHit::kMidUpper, ThresholdHit::kMidLower, ThresholdHit::kMidRatio, ThresholdHit::kMidLevel },
		{ ThresholdHit::kHighUpper, ThresholdHit::kHighLower, ThresholdHit::kHighRatio, ThresholdHit::kHighLevel },
	};
	return kTable[std::min<size_t>(band, 2)][std::min<size_t>(static_cast<size_t>(handle), 3)];
}

OwlttGraph::HandleLook OwlttGraph::lookOf(size_t band, Handle handle) const {
	// THE CONTROL'S OWN ANSWER, the same `ModDisplay` a knob draws from — `routable` is the gate and the
	// glow asked once (armed, may reach here), `amount` the armed source's route, `bipolar` what that
	// source is publishing now. Nothing here is computed from the mod state directly, because the
	// control's seating is where a fact about the control's modulation is described.
	const device::ModDisplay shown = controlForHit(hitFor(band, handle)).display();
	HandleLook look;
	look.lit = shown.routable;
	look.amount = shown.armed && shown.isDestination ? shown.amount : 0.0f;
	look.bipolar = shown.bipolar;
	return look;
}

visage::Bounds OwlttGraph::reachOf(size_t band, Handle handle) const {
	const HandleLook look = lookOf(band, handle);
	if (look.amount == 0.0f) {
		return {};
	}
	const visage::Bounds xover = barArea();
	const float x0 = band == 0 ? xover.x() : bandEdge(band - 1);
	const float x1 = band == 2 ? xover.right() : bandEdge(band);
	const owltt::EffectiveBand eff = effective(band);

	// THE TWO ENDS OF THE REACH, each in the handle's own mapping — the strips are linear over the box,
	// the bars sweep 0.2..1.0 through `thresholdToY` — and each restated through the SAME functions
	// the value itself is drawn with, so the reach starts exactly where the ring sits.
	float yAt = 0.0f;
	float yUp = 0.0f;
	float yDown = 0.0f;
	float x = x0;
	float width = x1 - x0;
	if (handle == Handle::RatioStrip || handle == Handle::LevelStrip) {
		const StripSpan strip = stripSpanOf(x0, x1);
		const bool ratio = handle == Handle::RatioStrip;
		const float value = ratio ? eff.ratio : eff.level;
		const auto y = [&](float v) { return xover.bottom() - std::clamp(v, 0.0f, 1.0f) * xover.height(); };
		yAt = y(value);
		yUp = y(value + look.amount);
		yDown = look.bipolar ? y(value - look.amount) : yAt;
		x = ratio ? strip.x0 : strip.mid;
		width = ratio ? strip.mid - strip.x0 : strip.x1 - strip.mid;
	}
	else {
		// THE LOWER BAR IS THE THRESHOLD AND THE UPPER IS THE BANDWIDTH ABOVE IT — the controls
		// `controlForHit` names for them — so the amount moves that control and the bar follows through
		// `barValuesOf`, the same arithmetic `barYs` draws with.
		const bool upper = handle == Handle::UpperBar;
		const auto y = [&](float delta) {
			const BarPair values = upper ? barValuesOf(eff.threshold, eff.bandwidth + delta)
			                             : barValuesOf(eff.threshold + delta, eff.bandwidth);
			return thresholdToY(upper ? values.upper : values.lower, xover);
		};
		yAt = y(0.0f);
		yUp = y(look.amount);
		yDown = look.bipolar ? y(-look.amount) : yAt;
	}
	const float top = std::min({ yAt, yUp, yDown });
	const float bottom = std::max({ yAt, yUp, yDown });
	return { x, top, width, bottom - top };
}

double OwlttGraph::noteHz() const {
	constexpr size_t kNoteHz = 17; // `Owltt::kReadingCount - 1`: the note Key Track places the crossovers on, or 0
	return host_.readingCountAt(device_) > kNoteHz ? static_cast<double>(host_.readingAt(device_, kNoteHz)) : 0.0;
}

double OwlttGraph::cornerHz(size_t band) const {
	// THE CORNER THE DEVICE RUNS AT: the knob, or — Key Track with a note — the note moved by its offset.
	const auto param = band == 0 ? owltt::Param::LowCrossover : owltt::Param::HighCrossover;
	const double note = noteHz();
	return owltt::trackedCrossoverHz(control(static_cast<size_t>(param)).value(), note > 0.0, note);
}

float OwlttGraph::bandEdge(size_t band) const { return hzToX(static_cast<float>(cornerHz(band)), plot()); }

void OwlttGraph::setCrossoversDraggable(bool on) {
	rail_ = on;
	redraw();
}

visage::Bounds OwlttGraph::rail() const {
	if (!rail_) {
		return {};
	}
	const visage::Bounds box = plot();
	return { box.x(), box.y(), box.width(), kRailHeight };
}

visage::Bounds OwlttGraph::barArea() const {
	const visage::Bounds box = plot();
	if (!rail_) {
		return box;
	}
	// THE BARS ARE CONFINED BELOW THE LANE, not merely outranked by it. HARM's scope records what the
	// other choice costs: a grip pushed into its rail became unreachable at the end of its travel,
	// which is where a hand goes to bring it back.
	return { box.x(), box.y() + kRailHeight, box.width(),
		     std::max(0.0f, box.height() - kRailHeight) };
}

visage::Point OwlttGraph::crossoverHandle(size_t which) const {
	const visage::Bounds r = rail();
	if (r.width() <= 0.0f) {
		return {};
	}
	// ON THE LINE IT MOVES, at the lane's midline. A handle ON the thing beats a handle NEAR it, and
	// held a radius inside the plot so the ends of the sweep stay grabbable.
	const float x = std::clamp(bandEdge(which), r.x() + OwlttGraph::kRingRadius, r.right() - OwlttGraph::kRingRadius);
	return { x, r.y() + r.height() * 0.5f };
}

bool OwlttGraph::isUpperHit(ThresholdHit hit) const {
	return hit == ThresholdHit::kLowUpper || hit == ThresholdHit::kMidUpper ||
	       hit == ThresholdHit::kHighUpper;
}

bool OwlttGraph::isThresholdHit(ThresholdHit hit) const {
	return hit == ThresholdHit::kLowUpper || hit == ThresholdHit::kMidUpper ||
	       hit == ThresholdHit::kHighUpper || hit == ThresholdHit::kLowLower ||
	       hit == ThresholdHit::kMidLower || hit == ThresholdHit::kHighLower;
}

OwlttGraph::ThresholdHit OwlttGraph::hitTestThreshold(visage::Point pos) const {
	const visage::Bounds xover = plot();
	if (!xover.contains(pos)) {
		return ThresholdHit::kNone;
	}

	// THE LANE FIRST, AND IT OWNS ITS BAND OUTRIGHT. Nothing else is drawn in it, so there is no
	// ordering question of the kind that has cost this file three bugs — the crossovers do not
	// out-rank the bars, the bars are simply not in this rectangle.
	const visage::Bounds r = rail();
	if (r.width() > 0.0f && r.contains(pos)) {
		const float dLow = std::abs(pos.x - crossoverHandle(0).x);
		const float dHigh = std::abs(pos.x - crossoverHandle(1).x);
		constexpr float kGrab = 10.0f;
		// THE NEARER OF THE TWO, and only if it is near at all: a press in the lane far from both
		// should move neither, rather than jumping the closest one across the spectrum.
		if (std::min(dLow, dHigh) <= kGrab) {
			return dLow <= dHigh ? ThresholdHit::kLowCrossover : ThresholdHit::kHighCrossover;
		}
		return ThresholdHit::kNone;
	}

	const float xLow = bandEdge(0);
	const float xHigh = bandEdge(1);

	constexpr float kBarHitHeight = 8.0f;

	struct BandRegion { float x0, x1; ThresholdHit upper; ThresholdHit lower; ThresholdHit ratio; ThresholdHit level; };
	const std::array<BandRegion, 3> regions = {
	    BandRegion{xover.x(), xLow, ThresholdHit::kLowUpper, ThresholdHit::kLowLower, ThresholdHit::kLowRatio, ThresholdHit::kLowLevel},
	    BandRegion{xLow, xHigh, ThresholdHit::kMidUpper, ThresholdHit::kMidLower, ThresholdHit::kMidRatio, ThresholdHit::kMidLevel},
	    BandRegion{xHigh, xover.right(), ThresholdHit::kHighUpper, ThresholdHit::kHighLower, ThresholdHit::kHighRatio, ThresholdHit::kHighLevel},
	};

	for (const auto& region : regions) {
		if (pos.x < region.x0 || pos.x > region.x1) {
			continue;
		}

		// THE STRIPS ARE ASKED FIRST, AND THAT IS THE WHOLE DECISION: the level slider's hit box sits
		// on top of the threshold's.
		//
		// TWO THINGS OVERLAP HERE AND ONLY ONE OF THEM CAN LOSE GRACEFULLY. The bars span the band's
		// full width; the strips are the right 30% of it and run the FULL HEIGHT. So wherever a bar
		// crosses a strip the two want the same pixel, and with the bars asked first the strip had a
		// dead band 16px tall cut across it — at exactly the height the threshold happens to sit,
		// which is to say it MOVES as you use the thing, and lands on the strip's middle at the
		// default. That is unusable in a way the reverse is not: the bar surrenders 30% of a wide
		// target it keeps everywhere else, and the strip stops having a hole in it.
		//
		// THE GENERAL FORM, WHICH THIS FILE HAS NOW PAID FOR TWICE: overlapping grab regions cannot be
		// ordered out of existence — one of them is always unreachable somewhere (harm_scope.cpp:400
		// says the same thing about a notch and a shoulder). So the order is not a tie-break to be
		// argued from geometry, it is a decision about WHICH CONTROL CAN AFFORD THE LOSS. The narrow
		// one cannot.
		const StripSpan strip = stripSpanOf(region.x0, region.x1);
		if (pos.x >= strip.x0 && pos.x <= strip.x1) {
			// Ratio on the left half of the strip, level on the right.
			return pos.x < strip.mid ? region.ratio : region.level;
		}

		// The threshold bars, over the band's remaining width.
		const size_t band = bandForHit(region.upper);
		const owltt::EffectiveBand eff = effective(band);
		const BarPair values = barValuesOf(eff.threshold, eff.bandwidth);
		// THE BAR AREA — the fourth and last rectangle in this class that had to be told the lane
		// exists. `ringCentre` puts a handle here, `draw` paints it here, the drag values it here,
		// and this decides what a press means. Left as `plot()` it disagreed with the other three by
		// exactly the lane's height: a threshold at the top of its sweep had its ring at y=20 and its
		// hit band at y=0, so the control was drawn, visible, and unhittable at the end of its travel
		// — which is the failure the lane was designed to prevent, reintroduced by the lane itself.
		const visage::Bounds plotBars = barArea();
		const float yUpper = thresholdToY(values.upper, plotBars);
		const float yLower = thresholdToY(values.lower, plotBars);

		// KILO'S TIE-BREAK, KEPT — it answers a case the flat ordering could not.
		//
		// AT BANDWIDTH 0 BOTH BARS SIT AT ONE Y and no vertical test can separate them. The old
		// comment here said "break tie at bandwidth 0 by giving the lower bar priority", which is a
		// coin toss wearing a rule: the upper bar simply became unreachable at the one setting where
		// a hand most wants to open the gap. Two sessions reordered this function independently on
		// the same afternoon and only one of them noticed that; the rings are offset either side of
		// centre and x decides.
		//
		// THE RING POSITIONS COME FROM `ringXsOf`, NOT FROM ARITHMETIC RESTATED HERE. They arrived
		// written twice — once for this test and once for the drawing, identical, unconnected — which
		// is the EIGHTH instance of that shape in this file this week and the third in this one
		// function. The two copies had already been given different jobs; they had not yet been given
		// different values.
		const RingPair rings = ringXsOf(region.x0, region.x1);
		const bool nearUpperY = std::abs(pos.y - yUpper) < kBarHitHeight;
		const bool nearLowerY = std::abs(pos.y - yLower) < kBarHitHeight;

		if (nearUpperY && !nearLowerY) {
			return region.upper;
		}
		if (nearLowerY && !nearUpperY) {
			return region.lower;
		}
		if (nearUpperY && nearLowerY) {
			return pos.x < (rings.upper + rings.lower) * 0.5f ? region.upper : region.lower;
		}
	}

	return ThresholdHit::kNone;
}


OwlttGraph::Meters OwlttGraph::meters() const {
	Meters m;
	if (host_.readingCountAt(device_) < 10) {
		return m;
	}
	for (size_t i = 0; i < 3; ++i) {
		m.grDB[i] = host_.readingAt(device_, i);
		m.inputLevelDB[i] = host_.readingAt(device_, 3 + i);
		m.outputLevelDB[i] = host_.readingAt(device_, 6 + i);
	}
	m.clipping = host_.readingAt(device_, 9) != 0.0f;
	// THE SQUARE'S NEAR STATE (backlog ML) needs a peak, and OWLTT publishes only its flag: the courtesy output meter's
	// (backlog MK) is the device's output peak, measured by the engine. Declined, the square keeps its two states.
	float outputPeak = 0.0f;
	m.clipState = clipLedStateFor(host_.outputPeakAt(device_, outputPeak) ? outputPeak : 0.0f, m.clipping);
	// THE EXTRAS (backlog EL), only when the publisher sent them — a device that publishes the old ten
	// leaves the lights off and the readout at zero rather than reading past its count.
	if (host_.readingCountAt(device_) >= 14) {
		for (size_t i = 0; i < 3; ++i) {
			m.bandSaturating[i] = host_.readingAt(device_, 10 + i) != 0.0f;
		}
		m.combinedGrDb = host_.readingAt(device_, 13);
	}
	// EACH BAND'S SECOND CHANNEL (backlog LQ), or the first again from a publisher with one linked gain.
	m.grSecond = m.grDB;
	if (host_.readingCountAt(device_) >= 17) {
		for (size_t i = 0; i < 3; ++i) {
			m.grSecond[i] = host_.readingAt(device_, 14 + i);
		}
	}
	return m;
}

bool OwlttGraph::update() {
	const Meters now = meters();
	// FIELD BY FIELD, and the clip bit with them. A memcmp would be tempting and wrong: the struct
	// has padding after the bool, and padding is not required to hold any particular value.
	const bool moved = now.grDB != drawnMeters_.grDB || now.inputLevelDB != drawnMeters_.inputLevelDB
	                   || now.outputLevelDB != drawnMeters_.outputLevelDB
	                   || now.clipping != drawnMeters_.clipping || now.clipState != drawnMeters_.clipState
	                   || now.bandSaturating != drawnMeters_.bandSaturating
	                   || now.combinedGrDb != drawnMeters_.combinedGrDb || now.grSecond != drawnMeters_.grSecond;
	if (!moved) {
		return false;
	}
	drawnMeters_ = now;
	redraw();
	return true;
}

void OwlttGraph::draw(visage::Canvas& canvas) {
	// THE BARS, STRIPS AND METERS ARE DRAWN IN `barArea()`, WHICH IS WHAT THE HIT TEST USES. With the
	// crossover lane on, that is the plot minus the lane at the top; with it off the two are the same
	// rectangle. Naming the local `xover` keeps the block below unchanged — and the point is that
	// there is ONE answer to "where does a bar go", not that the name is pretty.
	const visage::Bounds xover = barArea();
	// THE PLOT'S FRAME AND ITS TWO CROSSOVER LINES, which the card used to draw around this block.
	// They belong to the picture rather than to whatever holds it.
	const visage::Bounds whole = plot();
	canvas.setColor(withAlpha(kSurface, 0.45f));
	canvas.roundedRectangle(whole.x(), whole.y(), whole.width(), whole.height(), 5.0f);
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(whole.x(), whole.y(), whole.width(), whole.height(), 5.0f, 1.0f);

	const float xLow = bandEdge(0);
	const float xHigh = bandEdge(1);
	canvas.setColor(withAlpha(kLine, 0.8f));
	canvas.segment(xLow, whole.y(), xLow, whole.bottom(), 1.0f, true);
	canvas.segment(xHigh, whole.y(), xHigh, whole.bottom(), 1.0f, true);

	// THE CROSSOVER FREQUENCIES, ON THE LINES THEY BELONG TO AND ALWAYS VISIBLE. They were shown
	// nowhere. A multiband compressor is DEFINED by where it splits, and that
	// number was on no surface: the knobs print a bare `%.2f` through `device::Unit::Number` and the
	// lines carried no label at all.
	//
	// INBOARD OF THE LINE, on the side with room, so a label near either edge of the plot stays on the
	// picture rather than being clipped by it.
	// PLACED BY `bottomText`, clear of the channel letters and of each other.
	const BottomText bottom = bottomText();
	for (size_t which = 0; which < 2; ++which) {
		const visage::Bounds label = bottom.frequency[which];
		canvas.setColor(withAlpha(kMuted, 0.85f));
		canvas.text(bottom.frequencyText[which], font(kKnobReadoutSize), visage::Font::kCenter, label.x() - 2.0f,
		            label.y(), label.width() + 4.0f, label.height());
	}

	// THE CROSSOVER LANE AND ITS TWO HANDLES, when this graph offers them. Drawn against the WHOLE
	// plot because the lane is the strip the bars were pushed out of.
	const visage::Bounds r = rail();
	if (r.width() > 0.0f) {
		canvas.setColor(withAlpha(kSurface, 0.7f));
		canvas.rectangle(r.x(), r.y(), r.width(), r.height());
		canvas.setColor(withAlpha(kLine, 0.5f));
		canvas.fill(r.x(), r.bottom() - 1.0f, r.width(), 1.0f);
		for (size_t which = 0; which < 2; ++which) {
			const visage::Point c = crossoverHandle(which);
			canvas.setColor(kSurface);
			ringAt(canvas, c.x, c.y, OwlttGraph::kRingRadius);
			canvas.setColor(kAccent);
			ringOutlineAt(canvas, c.x, c.y, OwlttGraph::kRingRadius, 1.5f);
		}
	}
	// PER-BAND GRAPH CONTROLS — threshold bars, GR meters, level strips.
	{
		const Meters meters = this->meters();
		const float centerY = xover.y() + xover.height() * 0.5f;

		struct BandRegion { float x0, x1; const float& gr; const float& level; size_t band; };
		const std::array<BandRegion, 3> regions = {
		    BandRegion{xover.x(), xLow, meters.grDB[0], meters.inputLevelDB[0], 0},
		    BandRegion{xLow, xHigh, meters.grDB[1], meters.inputLevelDB[1], 1},
		    BandRegion{xHigh, xover.right(), meters.grDB[2], meters.inputLevelDB[2], 2},
		};

		for (const auto& region : regions) {
			const float regionW = region.x1 - region.x0;
			if (regionW <= 0.0f) continue;

			const size_t b = region.band;

			// THE VALUES THE DEVICE IS USING, for the strips. The bars come from `barYs` below, which
			// asks this same function -- so the whole band is drawn from one reading.
			const owltt::EffectiveBand eff = effective(b);
			const float ratio = eff.ratio;
			const float level = eff.level;

			// WHERE THE BARS GO — asked, not computed. This block used to read the STORED per-band
			// threshold while `ringCentre` and the hit test read the EFFECTIVE one, so the handles
			// followed the global and the lines under them did not.
			const BarYs ys = barYs(b);
			const float yLower = ys.lower;
			const float yUpper = ys.upper;

			// DRAW the dead zone (region between bars) — tinted background.
			canvas.setColor(withAlpha(kAccent, 0.08f));
			canvas.rectangle(region.x0, yUpper, regionW, yLower - yUpper);

			// DRAW threshold bars — horizontal lines spanning the band.
			canvas.setColor(kAccent);
			canvas.segment(region.x0, yUpper, region.x1, yUpper, 3.0f, true);
			canvas.segment(region.x0, yLower, region.x1, yLower, 3.0f, true);

			// HANDLE RINGS on the threshold bars — small handle circles on the things they move.
			// A handle ON the thing it moves beats a handle NEAR it (HARM's measured result).
			// OFFSET LEFT/RIGHT so in the degenerate case where bandwidth=0 and both bars are at the
			// same y value, you can still grab them separately.
			// FROM `ringXsOf`, the same call the hit test makes. These were five lines of arithmetic
			// here and four more in `hitTestThreshold`, computing one pair of positions twice — so a
			// ring could be moved on screen without moving where it can be grabbed, which is the
			// failure this card shipped twice already with the strips.
			// FROM `ringCentre`, which the cases ask too — so a ring cannot be drawn anywhere a press
			// at its centre does not reach. The positions used to be computed here and again in
			// `hitTestThreshold`, and the rings were held inside the box in neither.
			// THE ROUTED REACH ON A BAR — a translucent band of the modulation colour across the region,
			// from the bar to where the armed source can push it (`reachOf`). Drawn under the rings.
			for (const Handle h : { Handle::UpperBar, Handle::LowerBar }) {
				const visage::Bounds reach = reachOf(b, h);
				if (reach.height() > 0.0f) {
					canvas.setColor(withAlpha(kModulation, 0.35f));
					canvas.rectangle(reach.x(), reach.y(), reach.width(), reach.height());
				}
			}
			const visage::Point cUpper = ringCentre(b, Handle::UpperBar);
			const visage::Point cLower = ringCentre(b, Handle::LowerBar);
			// A HANDLE THE ARMED SOURCE MAY REACH WEARS THE MODULATION COLOUR AND A HALO — the knob's face
			// glow in this picture's terms (`lookOf`, the same answer the cases read); otherwise the
			// bar's own colour. HARM's grips do the same (`harm_scope.cpp`).
			const auto handleRing = [&](const visage::Point& c, Handle h, uint32_t restColour) {
				const bool lit = lookOf(b, h).lit;
				if (lit) {
					canvas.setColor(withAlpha(kModulation, 0.35f));
					ringOutlineAt(canvas, c.x, c.y, OwlttGraph::kRingRadius + 3.0f, 2.5f);
				}
				canvas.setColor(kSurface);
				ringAt(canvas, c.x, c.y, OwlttGraph::kRingRadius);
				canvas.setColor(lit ? kModulation : restColour);
				ringOutlineAt(canvas, c.x, c.y, OwlttGraph::kRingRadius, 1.5f);
			};
			handleRing(cUpper, Handle::UpperBar, kAccent);
			handleRing(cLower, Handle::LowerBar, kAccent);

			// DRAW GR meter — narrowed to central third of band.
			const float grX0 = region.x0 + regionW * 0.33f;
			const float grX1 = region.x0 + regionW * 0.67f;
			const float grW = grX1 - grX0;

			// GR meter background.
			canvas.setColor(withAlpha(kSurface, 0.3f));
			canvas.rectangle(grX0, xover.y(), grW, xover.height());

			// GR meter fill — accent for reduction (down), muted for boost (up). **ONE HALF PER CHANNEL
			// (backlog LQ)**: L | R, or M | S in M/S, each with its own gain since the channels came apart. At
			// Link 100% in L/R the halves are the same reading, which is the truth about a linked band.
			const std::array<float, 2> grs { region.gr, meters.grSecond[b] };
			const float halfW = std::max(0.0f, (grW - kMeterSplitGap) * 0.5f);
			for (size_t c = 0; c < 2; ++c) {
				const float x = grX0 + static_cast<float>(c) * (halfW + kMeterSplitGap);
				const float clampedGR = std::clamp(grs[c], -1.0f, 1.0f);
				const float barH = clampedGR * (xover.height() * 0.5f);
				if (barH < 0.0f) {
					canvas.setColor(kAccent);
					canvas.rectangle(x, centerY, halfW, -barH);
				}
				else if (barH > 0.0f) {
					canvas.setColor(withAlpha(kAccent, 0.4f));
					canvas.rectangle(x, centerY - barH, halfW, barH);
				}
			}
			// WHICH HALF IS WHICH, under each — "L R" or "M S" from the device's own switch, so the meter
			// cannot call Mid "L". Only when there is room for a letter.
			// The rectangles are `bottomText`'s, which the crossover labels were placed clear of.
			{
				const bool ms = control(static_cast<size_t>(owltt::Param::StereoMode)).value() >= 0.5;
				static constexpr std::array<const char*, 2> kLr { "L", "R" };
				static constexpr std::array<const char*, 2> kMs { "M", "S" };
				canvas.setColor(withAlpha(kMuted, 0.85f));
				size_t c = 0;
				for (size_t i = 0; i < bottom.letters.size(); ++i) {
					if (bottom.letterBand[i] != region.band) {
						continue;
					}
					const visage::Bounds g = bottom.letters[i];
					canvas.text(ms ? kMs[c] : kLr[c], font(kKnobReadoutSize), visage::Font::kCenter, g.x() - 2.0f,
					            g.y(), g.width() + 4.0f, g.height());
					c = std::min<size_t>(c + 1, 1);
				}
			}

			// DRAW input level bar — full height, faint, behind the GR meter.
			// SAME READING COLOUR AS THE GR BAR (kAccent) — both are readouts, not controls.
			// `kValue` is the colour this editor uses for things that ARE values; a meter wearing
			// it reads as a control, which is what misled on the earlier render.
			const float levelH = region.level * xover.height();
			canvas.setColor(withAlpha(kAccent, 0.25f));
			canvas.rectangle(region.x0, xover.bottom() - levelH, regionW, levelH);

			// DRAW ratio and level strips — vertical, over the band.
			const StripSpan strip = stripSpanOf(region.x0, region.x1);
			const float stripX0 = strip.x0;
			const float stripX1 = strip.mid;
			const float stripX2 = strip.x1;

			// Ratio strip (left). THE ACCENT, NOT THE MODULATION COLOUR: this strip wore `kModulation` at
			// rest, so it looked lit whether or not anything was armed — and the real glow on the band
			// level bars could not be found. The modulation colour now means one thing on this picture.
			canvas.setColor(withAlpha(kSurface, 0.3f));
			canvas.rectangle(stripX0, xover.y(), stripX1 - stripX0, xover.height());
			canvas.setColor(withAlpha(kAccent, 0.85f));
			const float ratioH = ratio * xover.height();
			canvas.rectangle(stripX0, xover.bottom() - ratioH, stripX1 - stripX0, ratioH);

			// Level strip (right).
			canvas.setColor(withAlpha(kSurface, 0.3f));
			canvas.rectangle(stripX1, xover.y(), stripX2 - stripX1, xover.height());
			canvas.setColor(kValue);
			const float levelStripH = level * xover.height();
			canvas.rectangle(stripX1, xover.bottom() - levelStripH, stripX2 - stripX1, levelStripH);

			// THE ROUTED REACH ON A STRIP — from the value to where the armed source can push it.
			for (const Handle h : { Handle::RatioStrip, Handle::LevelStrip }) {
				const visage::Bounds reach = reachOf(b, h);
				if (reach.height() > 0.0f) {
					canvas.setColor(withAlpha(kModulation, 0.45f));
					canvas.rectangle(reach.x(), reach.y(), reach.width(), reach.height());
				}
			}

			// HANDLE RINGS on the ratio and level strips — sitting on the value they represent, lit
			// with the halo while the armed source may reach them (see the bars above).
			handleRing(ringCentre(b, Handle::RatioStrip), Handle::RatioStrip, kAccent);
			handleRing(ringCentre(b, Handle::LevelStrip), Handle::LevelStrip, kValue);

			// Center line.
			canvas.setColor(withAlpha(kLine, 0.4f));
			canvas.segment(grX0, centerY, grX1, centerY, 1.0f, true);
		}

		// Clip indicator — filled square in the corner when clipping, and in the near colour within 2 dB of full scale
		// (backlog ML; `clip_led.h`).
		const float dotSize = 6.0f;
		if (meters.clipState != ClipLedState::Normal) {
			canvas.setColor(meters.clipState == ClipLedState::Clip ? kAccent : kMeterNear);
			canvas.rectangle(xover.right() - dotSize - 4.0f, xover.y() + 4.0f, dotSize, dotSize);
		}
		// EACH BAND'S OWN LIGHT (backlog EL): the same square, at the top-left of the band's region,
		// while that band's clipper is working. The output square above says the SUM clipped; these
		// say which band is driving it — the picture the three band meters were always short of.
		{
			const std::array<float, 3> lefts = { xover.x(), xLow, xHigh };
			for (size_t b = 0; b < 3; ++b) {
				if (meters.bandSaturating[b]) {
					canvas.setColor(kAccent);
					canvas.rectangle(lefts[b] + 4.0f, xover.y() + 4.0f, dotSize, dotSize);
				}
			}
		}
		// AND THE COMBINED GAIN REDUCTION, as a number beside the clip square: the mean |gain change|
		// across the bands, which is what the firmware's front panel showed as its one GR figure. Drawn
		// only when it is a number worth reading — below a tenth of a dB the compressor is idle.
		if (meters.combinedGrDb >= 0.1f) {
			char text[16];
			std::snprintf(text, sizeof(text), "%.1f dB", meters.combinedGrDb);
			const float textW = 52.0f;
			canvas.setColor(kMuted);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight,
			            xover.right() - dotSize - 8.0f - textW, xover.y() + 2.0f, textW, 12.0f);
		}
	}

	// THE TOOLTIP, LAST, so nothing paints over it — and drawn BY THIS FRAME rather than handed to
	// the editor's tray.
	//
	// THAT IS THE WHOLE POINT: no coupling to the Feathers plugin, so tooltips still work when the
	// effects are standalone. A graph that asked a `Notifier` to
	// show its text would be a graph that knows the Feathers window exists. This one asks the DEVICE
	// what a point means and draws the answer on itself, so a standalone OWLTT keeps its tooltips by
	// construction rather than by porting anything.
	//
	// `tooltipAt` ALREADY EXISTED AND NOTHING CALLED IT. It shipped with the text and the hit test
	// and no reader, which is the same shape as `travelToValue` next door — a road built and never
	// driven. It was found the only way it can be found: the graph showed no tooltips.
	if (hovering_ && !dragState_.active) {
		if (const char* text = tooltipAt(hover_)) {
			if (text[0] != '\0') {
				const visage::Bounds box = plot();
				constexpr float kW = 190.0f;
				constexpr float kH = 20.0f;
				// FOLLOWS THE POINTER BUT STAYS ON THE PICTURE, and sits ABOVE the cursor so it does
				// not cover the thing being described — a tooltip under the hand is a tooltip over
				// the control.
				const float x = std::clamp(hover_.x - kW * 0.5f, box.x() + 2.0f,
				                           std::max(box.x() + 2.0f, box.right() - kW - 2.0f));
				const float y = hover_.y - kH - 8.0f < box.y() ? hover_.y + 12.0f : hover_.y - kH - 8.0f;
				canvas.setColor(withAlpha(kSurface, 0.96f));
				canvas.roundedRectangle(x, y, kW, kH, 3.0f);
				canvas.setColor(withAlpha(kLine, 0.7f));
				canvas.roundedRectangleBorder(x, y, kW, kH, 3.0f, 1.0f);
				canvas.setColor(kInk);
				// BRIEF OR FULL, as every other control's tooltip — `tooltip.h`.
				canvas.text(displayedTooltip(text), font(kKnobReadoutSize), visage::Font::kCenter, x + 5.0f, y,
				            kW - 10.0f, kH);
			}
		}
	}
}

const char* OwlttGraph::tooltipAt(visage::Point pos) const {
	// ONE HIT TEST, TWO QUESTIONS. `hitTestThreshold` already answers "what is under this point" —
	// use the same enum as the key for tooltips.
	const ThresholdHit hit = hitTestThreshold(pos);
	switch (hit) {
	case ThresholdHit::kNone:
		return nullptr;
	case ThresholdHit::kLowCrossover:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::LowCrossover)].tooltip;
	case ThresholdHit::kHighCrossover:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::HighCrossover)].tooltip;

	// PER-BAND TOOLTIPS — the threshold bars are NOT params, so their text lives here, beside the
	// hit test that resolves them. The strips ARE params (LowRatio, LowLevel, etc.), so their text
	// comes from ParamDesc.
	case ThresholdHit::kLowUpper:
	case ThresholdHit::kMidUpper:
	case ThresholdHit::kHighUpper:
		// THE UPPER BAR IS THRESHOLD + 0.6*BANDWIDTH — it widens the compressed band upward.
		return "Upward compression threshold — quiet signals below this are amplified";
	case ThresholdHit::kLowLower:
	case ThresholdHit::kMidLower:
	case ThresholdHit::kHighLower:
		// THE LOWER BAR IS THE THRESHOLD ITSELF — loud signals above this are compressed.
		return "Downward compression threshold — loud signals above this are attenuated";
	case ThresholdHit::kLowRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::LowRatio)].tooltip;
	case ThresholdHit::kMidRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::MidRatio)].tooltip;
	case ThresholdHit::kHighRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::HighRatio)].tooltip;
	case ThresholdHit::kLowLevel:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::LowLevel)].tooltip;
	case ThresholdHit::kMidLevel:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::MidLevel)].tooltip;
	case ThresholdHit::kHighLevel:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::HighLevel)].tooltip;
	}
	return nullptr;
}

void OwlttGraph::offeredControls(std::vector<Offered>& out) const {
	// THE TWO CROSSOVERS and every band's handles (`kBandControls`' table), dragged: side roads.
	out.push_back({ &control(static_cast<size_t>(owltt::Param::LowCrossover)), Road::Gesture });
	out.push_back({ &control(static_cast<size_t>(owltt::Param::HighCrossover)), Road::Gesture });
	for (const owltt::Param p : { owltt::Param::LowThreshold, owltt::Param::LowBandwidth, owltt::Param::LowRatio,
	                              owltt::Param::LowLevel, owltt::Param::MidThreshold, owltt::Param::MidBandwidth,
	                              owltt::Param::MidRatio, owltt::Param::MidLevel, owltt::Param::HighThreshold,
	                              owltt::Param::HighBandwidth, owltt::Param::HighRatio, owltt::Param::HighLevel }) {
		out.push_back({ &control(static_cast<size_t>(p)), Road::Gesture });
	}
}

} // namespace feathers::ui
