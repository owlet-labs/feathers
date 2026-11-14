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
#include <cstring>
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


/// **A THRESHOLD AS A HEIGHT: THE PARAMETER'S OWN 0..1, bottom to top.** The compressor maps the parameter to its
/// internal `0.2 + 0.8 t` (`BandCompressor::setThresholdDown`), which is 48 dB of range below full scale, so the
/// parameter IS the position on the 48 dB window. This used to read the parameter as if it were already the internal
/// number, which drew the default (0.5) at 37% of the height and could never reach below 0.2.
float thresholdToY(float threshold, const visage::Bounds& xover) {
	const float t = std::clamp(threshold, 0.0f, 1.0f);
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
	float threshold;
	float bandwidth;
};

inline RingPair ringXsOf(float regionX0, float regionX1) {
	const float usable = stripSpanOf(regionX0, regionX1).x0 - regionX0;
	return { regionX0 + usable * 0.20f, regionX0 + usable * 0.42f };
}

/// THE RATIO TRACK the two thumbs slide along, left to right for ratio 0..1: across the bars' part of the band, clear of
/// the strips.
struct TrackSpan {
	float x0;
	float x1;
};

inline TrackSpan ratioTrackOf(float regionX0, float regionX1) {
	const float usable = stripSpanOf(regionX0, regionX1).x0 - regionX0;
	return { regionX0 + usable * 0.08f, regionX0 + usable * 0.94f };
}

/// HOW FAR A THUMB SITS OFF ITS BAR, pixels: the orange one above the threshold bar, the blue one below the bandwidth bar.
constexpr float kThumbOffset = 8.0f;
constexpr float kThumbHalf = 5.0f;


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
///
/// **THE RELATION AS THE DEVICE HAS IT NOW: THE UP THRESHOLD LIES BELOW THE DOWN ONE** (`BandCompressor::setBandwidth`:
/// `thresholdUp = max(0.2, thresholdDown - 0.6 * bandwidth)`, in the internal 0.2..1 units, which is `t - 0.75 bw` in
/// the parameter's). The comment above describes the opposite sign, which an earlier DSP had; the picture kept it, so
/// the bandwidth bar was drawn above the threshold bar and the orange (downward) edge on the wrong side of the blue.
struct BarPair {
	float down; ///< the DOWN threshold: downward compression acts above it
	float up;   ///< the UP threshold: upward compression acts below it
};

inline BarPair barValuesOf(float threshold, float bandwidth) {
	const float down = std::clamp(threshold, 0.0f, 1.0f);
	return { down, std::max(0.0f, down - 0.75f * bandwidth) };
}


float yToThreshold(float y, const visage::Bounds& xover) {
	return std::clamp((xover.bottom() - y) / xover.height(), 0.0f, 1.0f);
}


struct BandParamIndices {
	size_t threshold;
	size_t bandwidth;
	size_t ratio;
	size_t level;
	size_t downRatio;
	size_t upRatio;
};

static constexpr std::array<BandParamIndices, 3> kBandParams = {{
    {static_cast<size_t>(owltt::Param::LowThreshold), static_cast<size_t>(owltt::Param::LowBandwidth),
     static_cast<size_t>(owltt::Param::LowRatio), static_cast<size_t>(owltt::Param::LowLevel),
     static_cast<size_t>(owltt::Param::LowDownRatio), static_cast<size_t>(owltt::Param::LowUpRatio)},
    {static_cast<size_t>(owltt::Param::MidThreshold), static_cast<size_t>(owltt::Param::MidBandwidth),
     static_cast<size_t>(owltt::Param::MidRatio), static_cast<size_t>(owltt::Param::MidLevel),
     static_cast<size_t>(owltt::Param::MidDownRatio), static_cast<size_t>(owltt::Param::MidUpRatio)},
    {static_cast<size_t>(owltt::Param::HighThreshold), static_cast<size_t>(owltt::Param::HighBandwidth),
     static_cast<size_t>(owltt::Param::HighRatio), static_cast<size_t>(owltt::Param::HighLevel),
     static_cast<size_t>(owltt::Param::HighDownRatio), static_cast<size_t>(owltt::Param::HighUpRatio)},
}};


} // namespace

OwlttGraph::OwlttGraph(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)notifier_;
	// **PRESS, DRAG, RELEASE ARE ONE ROAD FOR EVERY HANDLE.** A handle that is a `ModHandle` (the crossovers, each band's
	// threshold and bandwidth rings, and the two ratio thumbs) takes the modulation menu on a secondary press, a route
	// gesture when a source is armed or Alt is held over its stroke, and a value drag otherwise; the strips keep the
	// plot's own vertical route gesture. A double click resets the value (or drops the route when a source is armed).
	onMouseDown() = [this](const visage::MouseEvent& e) {
#ifdef FEATHERS_DIAGNOSTIC
		diagnostic::plotPressed(e.position.x, e.position.y,
		                        static_cast<int>(hitTestThreshold(e.position)), width(), height());
#endif
		const auto hit = hitTestThreshold(e.position);
		if (hit == ThresholdHit::kNone) {
			return;
		}
		auto& ctrl = controlForHit(hit);
		ModHandle* mod = modForHit(hit);
		if (mod != nullptr && e.shouldTriggerPopup() && mod->showMenu(*this)) {
			return;
		}

		if (e.repeatClickCount() >= 2) {
			if (routeGestureWanted(ctrl, e)) {
				ctrl.dropRoute();
			}
			else {
				ctrl.resetToDefault();
			}
			redraw();
			return;
		}

		dragState_.active = true;
		dragState_.hit = hit;
		dragState_.startY = e.position.y;

		if (mod != nullptr) {
			const visage::Point c = centreOfHit(hit);
			const size_t axis = mod->routeAxisFor(e.isAltDown(), e.position, c.x, c.y);
			if (axis != ModHandle::npos) {
				dragState_.routeDragging = true;
				mod->beginRoute(axis, e);
				redraw();
				return;
			}
		}
		if (routeGestureWanted(ctrl, e)) {
			dragState_.routeDragging = true;
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
	};

	// THE HOVER ROAD: repaint only when a tooltip appears, moves or goes (a hover must not repaint the plot every pixel).
	onMouseMove() = [this](const visage::MouseEvent& e) {
		hover_ = e.position;
		hovering_ = true;
		const bool had = tipShown_;
		tipShown_ = tooltipAt(hover_) != nullptr;
		if (had || tipShown_) {
			++hoverRedraws_;
			redraw();
		}
	};
	onMouseExit() = [this](const visage::MouseEvent&) {
		hovering_ = false;
		if (tipShown_) {
			tipShown_ = false;
			++hoverRedraws_;
			redraw();
		}
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!dragState_.active) {
			return;
		}
		auto& ctrl = controlForHit(dragState_.hit);
		const visage::Bounds xover = barArea();
		ModHandle* mod = modForHit(dragState_.hit);

		if (dragState_.routeDragging && mod != nullptr && mod->routing()) {
			mod->dragRoute(e);
			redraw();
			return;
		}
		if (dragState_.hit == ThresholdHit::kLowCrossover || dragState_.hit == ThresholdHit::kHighCrossover) {
			const double hz = static_cast<double>(xToHz(e.position.x, plot()));
			const double note = noteHz();
			ctrl.setValue(note > 0.0 ? owltt::kCrossoverKeyRefHz * hz / note : hz);
			redraw();
			return;
		}

		if (dragState_.routeDragging) {
			const float travel = (dragState_.startY - e.position.y) / dragState_.routeDragRange;
			ctrl.setAmount(std::clamp(dragState_.routeAnchorAmount + travel,
			                           -dragState_.maxRouteAmount, dragState_.maxRouteAmount));
		}
		else if (isBandwidthHit(dragState_.hit)) {
			// THE UP BAR: where it is dragged to is the UP threshold; the bandwidth is how far that lies below the
			// threshold bar, so the threshold bar does not move.
			const size_t band = bandForHit(dragState_.hit);
			const owltt::EffectiveBand eff = effective(band);
			const float wantedUp = yToThreshold(e.position.y, xover);
			const float wantedBandwidth = std::clamp((eff.threshold - wantedUp) / 0.75f, 0.0f, 1.0f);
			ctrl.setValue(owltt::bandValueForEffective(wantedBandwidth, shiftFor(Handle::BandwidthBar)));
		}
		else if (isThresholdHit(dragState_.hit)) {
			ctrl.setValue(owltt::bandValueForEffective(yToThreshold(e.position.y, xover),
			                                          shiftFor(Handle::ThresholdBar)));
		}
		else if (isRatioTrimHit(dragState_.hit)) {
			// A THUMB IS DRAGGED ALONG ITS BAR: the x is the wanted ratio of that direction; the trim stored is what, on top
			// of the band's effective ratio, lands it there.
			const size_t band = bandForHit(dragState_.hit);
			const Track track = ratioTrack(band);
			const float wanted = track.x1 > track.x0 ? std::clamp((e.position.x - track.x0) / (track.x1 - track.x0), 0.0f, 1.0f) : 0.0f;
			ctrl.setValue(owltt::bandValueForEffective(wanted, effective(band).ratio - 0.5f));
		}
		else {
			const float t = std::clamp((xover.bottom() - e.position.y) / xover.height(), 0.0f, 1.0f);
			const Handle which = (dragState_.hit == ThresholdHit::kLowRatio
			                      || dragState_.hit == ThresholdHit::kMidRatio
			                      || dragState_.hit == ThresholdHit::kHighRatio)
			                         ? Handle::RatioStrip
			                         : Handle::LevelStrip;
			ctrl.setValue(owltt::bandValueForEffective(t, shiftFor(which)));
		}
	};

	onMouseUp() = [this](const visage::MouseEvent& /*e*/) {
		if (!dragState_.active) {
			return;
		}
		auto& ctrl = controlForHit(dragState_.hit);
		ModHandle* mod = modForHit(dragState_.hit);
		if (dragState_.routeDragging && mod != nullptr && mod->routing()) {
			mod->endRoute();
		}
		else if (dragState_.routeDragging) {
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
	// **THE READOUT STANDS BESIDE ITS CORNER.** In the window that is beside the handle in the lane: right of the ring, flipping to its
	// left when it would leave the plot or touch the other handle or label. On the card, which has no lane, it is at the top of the
	// plot beside the crossover line, on the line's left, where the bar rings and the saturation lights are not (the lights sit
	// right of a line); it flips right on the same conditions. The obstacles are the handles, the other label, the clip square,
	// the combined-reduction text and the lights, so nothing is drawn over a label.
	std::vector<visage::Bounds> fixed;
	const visage::Bounds lane = rail();
	if (lane.width() > 0.0f) {
		for (size_t h = 0; h < 2; ++h) {
			const visage::Point c = crossoverHandle(h);
			fixed.push_back({ c.x - 8.0f, c.y - 8.0f, 16.0f, 16.0f });
		}
	}
	else {
		fixed.push_back({ whole.right() - 90.0f, whole.y(), 90.0f, 14.0f }); // the clip square and the combined reduction
		for (const float left : { whole.x(), xLow, xHigh }) {
			fixed.push_back({ left + 3.0f, xover.y() + 3.0f, 8.0f, 8.0f }); // a band's saturation light
		}
	}
	for (size_t which = 0; which < 2; ++which) {
		out.frequencyText[which] = frequencyText(static_cast<float>(cornerHz(which)));
		const float w = widthOf(out.frequencyText[which]);
		const float y = lane.width() > 0.0f ? lane.y() + (lane.height() - kRowHeight) * 0.5f : whole.y() + 2.0f;
		const float anchor = lane.width() > 0.0f ? crossoverHandle(which).x : edges[which];
		const float gap = lane.width() > 0.0f ? 10.0f : 4.0f;
		const auto fits = [&](const visage::Bounds& b) {
			if (b.x() < whole.x() + 1.0f || b.right() > whole.right() - 1.0f || b.bottom() > xover.bottom() - 14.0f) {
				return false;
			}
			for (const visage::Bounds& o : fixed) {
				if (touches(b, o)) {
					return false;
				}
			}
			return which == 0 || !touches(b, out.frequency[0]);
		};
		// THE PREFERRED SIDE, THEN THE OTHER, THEN A ROW DOWN on either — a corner crowded against its neighbour steps down rather
		// than over it.
		out.frequency[which] = { anchor, y, w, kRowHeight };
		bool placed = false;
		for (int row = 0; row < 3 && !placed; ++row) {
			for (int side = 0; side < 2 && !placed; ++side) {
				const bool right = (lane.width() > 0.0f) == (side == 0);
				const visage::Bounds b { right ? anchor + gap : anchor - gap - w, y + static_cast<float>(row) * 13.0f, w, kRowHeight };
				if (fits(b)) {
					out.frequency[which] = b;
					placed = true;
				}
			}
		}
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
	case ThresholdHit::kLowBandwidth:
	case ThresholdHit::kMidBandwidth:
	case ThresholdHit::kHighBandwidth:
		return control(kBandParams[band].bandwidth);
	case ThresholdHit::kLowThreshold:
	case ThresholdHit::kMidThreshold:
	case ThresholdHit::kHighThreshold:
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
	case ThresholdHit::kLowDownRatio:
	case ThresholdHit::kMidDownRatio:
	case ThresholdHit::kHighDownRatio:
		return control(kBandParams[band].downRatio);
	case ThresholdHit::kLowUpRatio:
	case ThresholdHit::kMidUpRatio:
	case ThresholdHit::kHighUpRatio:
		return control(kBandParams[band].upRatio);
	default:
		return control(0);
	}
}

size_t OwlttGraph::bandForHit(ThresholdHit hit) const {
	switch (hit) {
	case ThresholdHit::kLowBandwidth:
	case ThresholdHit::kLowThreshold:
	case ThresholdHit::kLowRatio:
	case ThresholdHit::kLowLevel:
	case ThresholdHit::kLowDownRatio:
	case ThresholdHit::kLowUpRatio:
		return 0;
	case ThresholdHit::kMidBandwidth:
	case ThresholdHit::kMidThreshold:
	case ThresholdHit::kMidRatio:
	case ThresholdHit::kMidLevel:
	case ThresholdHit::kMidDownRatio:
	case ThresholdHit::kMidUpRatio:
		return 1;
	case ThresholdHit::kHighBandwidth:
	case ThresholdHit::kHighThreshold:
	case ThresholdHit::kHighRatio:
	case ThresholdHit::kHighLevel:
	case ThresholdHit::kHighDownRatio:
	case ThresholdHit::kHighUpRatio:
		return 2;
	default:
		return 0;
	}
}

owltt::EffectiveBand OwlttGraph::effective(size_t band) const {
	owltt::BandParams params {};
	params.threshold = static_cast<float>(control(kBandParams[band].threshold).value());
	params.ratio = static_cast<float>(control(kBandParams[band].ratio).value());
	params.bandwidth = static_cast<float>(control(kBandParams[band].bandwidth).value());
	params.level = static_cast<float>(control(kBandParams[band].level).value());
	params.downRatio = static_cast<float>(control(kBandParams[band].downRatio).value());
	params.upRatio = static_cast<float>(control(kBandParams[band].upRatio).value());

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
	case Handle::ThresholdBar:
		return owltt::thresholdShiftOf(globals);
	case Handle::BandwidthBar:
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
	return { thresholdToY(values.down, bars), thresholdToY(values.up, bars) };
}

visage::Point OwlttGraph::ringCentre(size_t band, Handle handle) const {
	const visage::Bounds xover = barArea();
	const float x0 = band == 0 ? xover.x() : bandEdge(band - 1);
	const float x1 = band == 2 ? xover.right() : bandEdge(band);

	if (handle == Handle::BandwidthBar || handle == Handle::ThresholdBar) {
		const BarYs ys = barYs(band);
		const RingPair rings = ringXsOf(x0, x1);
		const bool bandwidth = handle == Handle::BandwidthBar;
		return { bandwidth ? rings.bandwidth : rings.threshold,
			     ringYInside(bandwidth ? ys.bandwidth : ys.threshold, xover) };
	}

	if (handle == Handle::DownRatio || handle == Handle::UpRatio) {
		// THE THUMBS: x is the direction's ratio along the track, y is a fixed step off its bar - above the threshold
		// bar for the downward compressor, below the bandwidth bar for the upward one - so the two can never sit on
		// each other, whatever the gap.
		const BarYs ys = barYs(band);
		const owltt::EffectiveBand eff = effective(band);
		const TrackSpan track = ratioTrackOf(x0, x1);
		const bool down = handle == Handle::DownRatio;
		const float ratio = down ? eff.ratioDown : eff.ratioUp;
		return { track.x0 + ratio * (track.x1 - track.x0),
			     ringYInside(down ? ys.threshold - kThumbOffset : ys.bandwidth + kThumbOffset, xover) };
	}

	const StripSpan strip = stripSpanOf(x0, x1);
	const bool ratio = handle == Handle::RatioStrip;
	const owltt::EffectiveBand eff = effective(band);
	const float value = ratio ? eff.ratio : eff.level;
	return { ratio ? (strip.x0 + strip.mid) * 0.5f : (strip.mid + strip.x1) * 0.5f,
		     ringYInside(xover.bottom() - value * xover.height(), xover) };
}

OwlttGraph::ThresholdHit OwlttGraph::hitFor(size_t band, Handle handle) const {
	static constexpr ThresholdHit kTable[3][6] = {
		{ ThresholdHit::kLowBandwidth, ThresholdHit::kLowThreshold, ThresholdHit::kLowRatio, ThresholdHit::kLowLevel,
		  ThresholdHit::kLowDownRatio, ThresholdHit::kLowUpRatio },
		{ ThresholdHit::kMidBandwidth, ThresholdHit::kMidThreshold, ThresholdHit::kMidRatio, ThresholdHit::kMidLevel,
		  ThresholdHit::kMidDownRatio, ThresholdHit::kMidUpRatio },
		{ ThresholdHit::kHighBandwidth, ThresholdHit::kHighThreshold, ThresholdHit::kHighRatio, ThresholdHit::kHighLevel,
		  ThresholdHit::kHighDownRatio, ThresholdHit::kHighUpRatio },
	};
	return kTable[std::min<size_t>(band, 2)][std::min<size_t>(static_cast<size_t>(handle), 5)];
}

OwlttGraph::Handle OwlttGraph::handleOfHit(ThresholdHit hit) const {
	for (size_t band = 0; band < 3; ++band) {
		for (const Handle h : { Handle::BandwidthBar, Handle::ThresholdBar, Handle::RatioStrip, Handle::LevelStrip,
		                        Handle::DownRatio, Handle::UpRatio }) {
			if (hitFor(band, h) == hit) {
				return h;
			}
		}
	}
	return Handle::ThresholdBar;
}

bool OwlttGraph::isRatioTrimHit(ThresholdHit hit) const {
	const Handle h = handleOfHit(hit);
	return (h == Handle::DownRatio || h == Handle::UpRatio) && hit != ThresholdHit::kNone;
}

visage::Point OwlttGraph::centreOfHit(ThresholdHit hit) const {
	if (hit == ThresholdHit::kLowCrossover) {
		return crossoverHandle(0);
	}
	if (hit == ThresholdHit::kHighCrossover) {
		return crossoverHandle(1);
	}
	return ringCentre(bandForHit(hit), handleOfHit(hit));
}

/// **THE MODULATION ROAD OF A HANDLE**: the crossovers and every bar and thumb are `ModHandle`s; the two strips are not.
ModHandle* OwlttGraph::modForHit(ThresholdHit hit) const {
	if (hit == ThresholdHit::kLowCrossover) {
		return &crossoverMod(0);
	}
	if (hit == ThresholdHit::kHighCrossover) {
		return &crossoverMod(1);
	}
	if (hit == ThresholdHit::kNone) {
		return nullptr;
	}
	const Handle h = handleOfHit(hit);
	if (h == Handle::RatioStrip || h == Handle::LevelStrip) {
		return nullptr;
	}
	return &bandMod(bandForHit(hit), h);
}

OwlttGraph::Track OwlttGraph::ratioTrack(size_t band) const {
	const visage::Bounds xover = barArea();
	const float x0 = band == 0 ? xover.x() : bandEdge(band - 1);
	const float x1 = band == 2 ? xover.right() : bandEdge(band);
	const TrackSpan span = ratioTrackOf(x0, x1);
	return { span.x0, span.x1 };
}

/// **EACH BAND HANDLE'S MODULATION, mapped through the same picture the handle is drawn from**: one vertical axis for a
/// bar (the value's height, folded through the global shift and, for the bandwidth, the threshold it hangs from), one
/// horizontal axis for a thumb (the direction's ratio along the track).
ModHandle& OwlttGraph::bandMod(size_t band, Handle handle) const {
	if (!bandModBuilt_) {
		for (size_t b = 0; b < 3; ++b) {
			const auto yOfThreshold = [this, b](double v) {
				const float eff = std::clamp(static_cast<float>(v) + shiftFor(Handle::ThresholdBar), 0.0f, 1.0f);
				return thresholdToY(eff, barArea());
			};
			const auto yOfBandwidth = [this, b](double v) {
				const owltt::EffectiveBand e = effective(b);
				const float bw = std::clamp(static_cast<float>(v) + shiftFor(Handle::BandwidthBar), 0.0f, 1.0f);
				return thresholdToY(barValuesOf(e.threshold, bw).up, barArea());
			};
			const auto xOfDown = [this, b](double v) {
				const owltt::EffectiveBand e = effective(b);
				const Track t = ratioTrack(b);
				const float r = std::clamp(e.ratio + static_cast<float>(v) - 0.5f, 0.0f, 1.0f);
				return t.x0 + r * (t.x1 - t.x0);
			};
			bandMod_[b * 4 + 0] = ModHandle({ ModHandleAxis { &control(kBandParams[b].threshold), false, yOfThreshold } });
			bandMod_[b * 4 + 1] = ModHandle({ ModHandleAxis { &control(kBandParams[b].bandwidth), false, yOfBandwidth } });
			bandMod_[b * 4 + 2] = ModHandle({ ModHandleAxis { &control(kBandParams[b].downRatio), true, xOfDown } });
			bandMod_[b * 4 + 3] = ModHandle({ ModHandleAxis { &control(kBandParams[b].upRatio), true, xOfDown } });
		}
		bandModBuilt_ = true;
	}
	size_t kind = 0;
	switch (handle) {
	case Handle::ThresholdBar: kind = 0; break;
	case Handle::BandwidthBar: kind = 1; break;
	case Handle::DownRatio: kind = 2; break;
	case Handle::UpRatio: kind = 3; break;
	default: break;
	}
	return bandMod_[std::min<size_t>(band, 2) * 4 + kind];
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
	// ONLY THE STRIPS DRAW A REACH RECTANGLE. A bar or a thumb is a `ModHandle`, whose own plan (`bandModPlan`) is the stroke
	// and the dot it draws.
	if (handle != Handle::RatioStrip && handle != Handle::LevelStrip) {
		return {};
	}
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
	const float lane = rail_ ? kRailHeight : 0.0f;
	const float table = editor_ ? kTableHeight : 0.0f;
	// THE BARS ARE CONFINED BELOW THE LANE AND ABOVE THE TABLE, not merely outranked by them: a grip pushed into either
	// became unreachable at the end of its travel, which is where a hand goes to bring it back.
	return { box.x(), box.y() + lane, box.width(), std::max(0.0f, box.height() - lane - table) };
}

visage::Bounds OwlttGraph::tableArea() const {
	if (!editor_) {
		return {};
	}
	const visage::Bounds box = plot();
	return { box.x(), box.bottom() - kTableHeight, box.width(), kTableHeight };
}

fx::owltt::Param OwlttGraph::tableParam(size_t row, size_t band) {
	using P = owltt::Param;
	static constexpr P kTable[kTableRows][3] = {
		{ P::LowInput, P::MidInput, P::HighInput },
		{ P::LowWidth, P::MidWidth, P::HighWidth },
		{ P::LowAttack, P::MidAttack, P::HighAttack },
		{ P::LowRelease, P::MidRelease, P::HighRelease },
	};
	return kTable[std::min(row, kTableRows - 1)][std::min<size_t>(band, 2)];
}

const char* OwlttGraph::tableCaption(size_t row) {
	static constexpr const char* kCaptions[kTableRows] = { "IN", "WIDTH", "ATK", "REL" };
	return kCaptions[std::min(row, kTableRows - 1)];
}

visage::Bounds OwlttGraph::tableCell(size_t row, size_t band) const {
	// THE COLUMNS ARE THE BANDS: a cell is centred in its band's span (the first band's starts after the captions' gutter) and
	// a narrow band narrows its cell down to a floor; cells never overlap - one that would is pushed right of the last.
	const visage::Bounds t = tableArea();
	const float lows[3] = { t.x() + kTableGutter, bandEdge(0), bandEdge(1) };
	const float highs[3] = { bandEdge(0), bandEdge(1), t.right() };
	constexpr float kCellMax = 96.0f;
	constexpr float kCellMin = 46.0f;
	constexpr float kCellGap = 4.0f;
	float previousRight = t.x() + kTableGutter - kCellGap;
	visage::Bounds out {};
	for (size_t b = 0; b <= std::min<size_t>(band, 2); ++b) {
		const float span = std::max(0.0f, highs[b] - lows[b]);
		const float w = std::clamp(span - 8.0f, kCellMin, kCellMax);
		float x = lows[b] + (span - w) * 0.5f;
		x = std::max(x, previousRight + kCellGap);
		x = std::min(x, std::max(previousRight + kCellGap, t.right() - 2.0f - w));
		out = { x, t.y() + kTablePad + static_cast<float>(row) * kTableRowHeight, w, kTableRowHeight - 2.0f };
		previousRight = x + w;
	}
	return out;
}

void OwlttGraph::layoutTable() {
	if (!editor_) {
		return;
	}
	for (size_t row = 0; row < kTableRows; ++row) {
		for (size_t band = 0; band < 3; ++band) {
			if (cells_[row][band] != nullptr) {
				const visage::Bounds c = tableCell(row, band);
				cells_[row][band]->setBounds(c.x(), c.y(), c.width(), c.height());
			}
		}
	}
	tableEdges_ = { bandEdge(0), bandEdge(1) };
}

void OwlttGraph::resized() {
	layoutTable();
}

void OwlttGraph::setEditor(bool on) {
	if (editor_ == on) {
		return;
	}
	editor_ = on;
	rail_ = on || rail_;
	if (on) {
		for (size_t row = 0; row < kTableRows; ++row) {
			for (size_t band = 0; band < 3; ++band) {
				DeviceControl* ctrl = &control(static_cast<size_t>(tableParam(row, band)));
				auto box = std::make_unique<ValueBox>(*ctrl, "");
				box->setCompact(true);
				if (row >= 2) {
					// THE OFFSETS READ SIGNED FROM THEIR CENTRE through the device's own words (`kTextOps`): the box prints the
					// declared unit unless it is told otherwise.
					box->setReadoutFormatter([ctrl](double v) { return ctrl->textAt(v); });
				}
				addChild(box.get());
				cells_[row][band] = std::move(box);
			}
		}
		layoutTable();
	}
	redraw();
}

ModHandle& OwlttGraph::crossoverMod(size_t which) const {
	if (!crossoverModBuilt_) {
		for (size_t i = 0; i < 2; ++i) {
			const auto param = i == 0 ? owltt::Param::LowCrossover : owltt::Param::HighCrossover;
			// A CONTROL VALUE TO AN X: the same road `bandEdge` draws the line by — the corner the value runs at
			// (key-tracked while a note is heard), on the plot's frequency axis. No second formula.
			crossoverMod_[i] = ModHandle({ ModHandleAxis {
			    &control(static_cast<size_t>(param)), true, [this](double value) {
				    const double note = noteHz();
				    return hzToX(static_cast<float>(owltt::trackedCrossoverHz(value, note > 0.0, note)), plot());
			    } } });
		}
		crossoverModBuilt_ = true;
	}
	return crossoverMod_[which];
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

bool OwlttGraph::isBandwidthHit(ThresholdHit hit) const {
	return hit == ThresholdHit::kLowBandwidth || hit == ThresholdHit::kMidBandwidth ||
	       hit == ThresholdHit::kHighBandwidth;
}

bool OwlttGraph::isThresholdHit(ThresholdHit hit) const {
	return hit == ThresholdHit::kLowThreshold || hit == ThresholdHit::kMidThreshold || hit == ThresholdHit::kHighThreshold;
}

OwlttGraph::ThresholdHit OwlttGraph::hitTestThreshold(visage::Point pos) const {
	const visage::Bounds xover = barArea();
	const visage::Bounds whole = plot();
	// THE TABLE IS NOT THE PLOT'S: its boxes are children and take their own presses.
	if (!whole.contains(pos) || (editor_ && tableArea().contains(pos))) {
		return ThresholdHit::kNone;
	}

	// THE LANE FIRST, AND IT OWNS ITS BAND OUTRIGHT. Nothing else is drawn in it, so there is no ordering question of
	// the kind that has cost this file three bugs: the bars are simply not in this rectangle.
	const visage::Bounds r = rail();
	if (r.width() > 0.0f && r.contains(pos)) {
		constexpr float kGrab = 10.0f;
		// THE NEARER OF THE TWO, within a grab radius (`ModHandle::nearest`).
		const std::array<visage::Point, 2> centres { crossoverHandle(0), crossoverHandle(1) };
		const size_t taken = ModHandle::nearest(centres, pos, kGrab);
		if (taken != ModHandle::npos) {
			return taken == 0 ? ThresholdHit::kLowCrossover : ThresholdHit::kHighCrossover;
		}
		return ThresholdHit::kNone;
	}

	const float xLow = bandEdge(0);
	const float xHigh = bandEdge(1);

	constexpr float kBarHitHeight = 8.0f;

	struct BandRegion {
		float x0, x1;
		ThresholdHit bandwidth, threshold, ratio, level, down, up;
	};
	const std::array<BandRegion, 3> regions = {
	    BandRegion { xover.x(), xLow, ThresholdHit::kLowBandwidth, ThresholdHit::kLowThreshold, ThresholdHit::kLowRatio,
	                 ThresholdHit::kLowLevel, ThresholdHit::kLowDownRatio, ThresholdHit::kLowUpRatio },
	    BandRegion { xLow, xHigh, ThresholdHit::kMidBandwidth, ThresholdHit::kMidThreshold, ThresholdHit::kMidRatio,
	                 ThresholdHit::kMidLevel, ThresholdHit::kMidDownRatio, ThresholdHit::kMidUpRatio },
	    BandRegion { xHigh, xover.right(), ThresholdHit::kHighBandwidth, ThresholdHit::kHighThreshold,
	                 ThresholdHit::kHighRatio, ThresholdHit::kHighLevel, ThresholdHit::kHighDownRatio,
	                 ThresholdHit::kHighUpRatio },
	};

	for (const auto& region : regions) {
		if (pos.x < region.x0 || pos.x > region.x1) {
			continue;
		}

		// THE STRIPS ARE ASKED FIRST: they are the narrow control and cannot afford a hole cut across them by a bar. (The
		// rule this file paid for twice - overlapping grab regions cannot be ordered away, one is always unreachable
		// somewhere, so the order says which can afford the loss.)
		const StripSpan strip = stripSpanOf(region.x0, region.x1);
		if (pos.x >= strip.x0 && pos.x <= strip.x1) {
			return pos.x < strip.mid ? region.ratio : region.level;
		}

		const size_t band = bandForHit(region.threshold);

		// THE THUMBS NEXT (the window only): small and sitting just off their bars, they win over the bar's wide band
		// where they overlap it.
		if (editor_) {
			constexpr float kThumbGrab = 8.0f;
			const std::array<visage::Point, 2> thumbs { ringCentre(band, Handle::DownRatio),
				                                        ringCentre(band, Handle::UpRatio) };
			const size_t taken = ModHandle::nearest(thumbs, pos, kThumbGrab);
			if (taken != ModHandle::npos) {
				return taken == 0 ? region.down : region.up;
			}
		}

		const BarYs ys = barYs(band);
		const RingPair rings = ringXsOf(region.x0, region.x1);
		const bool nearThreshold = std::abs(pos.y - ys.threshold) < kBarHitHeight;
		const bool nearBandwidth = std::abs(pos.y - ys.bandwidth) < kBarHitHeight;

		if (nearThreshold && !nearBandwidth) {
			return region.threshold;
		}
		if (nearBandwidth && !nearThreshold) {
			return region.bandwidth;
		}
		if (nearThreshold && nearBandwidth) {
			// THE BARS OVERLAP (a gap of zero, or a threshold at the floor): the left half takes the threshold and the right
			// the bandwidth, which is where their rings sit.
			return pos.x < (rings.threshold + rings.bandwidth) * 0.5f ? region.threshold : region.bandwidth;
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
	// THE TABLE FOLLOWS THE CROSSOVERS: its columns are the bands, which move with a drag, a route or a tracked note.
	if (editor_ && (bandEdge(0) != tableEdges_[0] || bandEdge(1) != tableEdges_[1])) {
		layoutTable();
		redraw();
	}
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

	// THE TABLE UNDER THE BARS: a rule above it, the row captions once at the left, and each band's column heading it by
	// the crossover lines already drawn. The boxes are children and paint themselves.
	if (editor_) {
		const visage::Bounds t = tableArea();
		canvas.setColor(withAlpha(kLine, 0.6f));
		canvas.fill(t.x(), t.y(), t.width(), 1.0f);
		canvas.setColor(withAlpha(kMuted, 0.85f));
		for (size_t row = 0; row < kTableRows; ++row) {
			canvas.text(tableCaption(row), font(kKnobLabelSize), visage::Font::kLeft, t.x() + 7.0f,
			            t.y() + kTablePad + static_cast<float>(row) * kTableRowHeight, kTableGutter - 7.0f,
			            kTableRowHeight - 2.0f);
		}
	}

	// THE CROSSOVER FREQUENCIES, ON THE LINES THEY BELONG TO AND ALWAYS VISIBLE. They were shown
	// nowhere. A multiband compressor is DEFINED by where it splits, and that
	// number was on no surface: the knobs print a bare `%.2f` through `device::Unit::Number` and the
	// lines carried no label at all.
	//
	// INBOARD OF THE LINE, on the side with room, so a label near either edge of the plot stays on the
	// picture rather than being clipped by it.
	// PLACED BY `bottomText`, clear of the channel letters and of each other.

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
			// THE HANDLE STAYS AT THE BASE; its halo and amount stroke go under it, the live dot over it.
			crossoverMod(which).drawUnder(canvas, c.x, c.y, OwlttGraph::kRingRadius);
			canvas.setColor(kSurface);
			ringAt(canvas, c.x, c.y, OwlttGraph::kRingRadius);
			canvas.setColor(kAccent);
			ringOutlineAt(canvas, c.x, c.y, OwlttGraph::kRingRadius, 1.5f);
			crossoverMod(which).drawOver(canvas, c.x, c.y, OwlttGraph::kRingRadius);
		}
		if (editor_) {
			// THE COLOUR KEY for the bars and thumbs: orange is the downward compressor, blue the upward.
			float x = r.x() + 8.0f;
			for (const auto& [text, colour] : { std::pair<const char*, uint32_t> { "DOWN", kValue },
				                                 std::pair<const char*, uint32_t> { "UP", kAccent } }) {
				const float w = std::max(14.0f, 6.5f * static_cast<float>(std::strlen(text)));
				canvas.setColor(colour);
				canvas.fill(x, r.y() + 5.0f, 5.0f, 5.0f);
				canvas.setColor(withAlpha(kMuted, 0.9f));
				canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, x + 8.0f, r.y(), w, r.height());
				x += w + 14.0f;
			}
		}
	}
	// THE CROSSOVER READOUTS, LAST OF THE LANE'S PAINT: drawn before the lane's fill they were veiled by it.
	const BottomText bottom = bottomText();
	for (size_t which = 0; which < 2; ++which) {
		const visage::Bounds label = bottom.frequency[which];
		canvas.setColor(withAlpha(kMuted, 0.85f));
		canvas.text(bottom.frequencyText[which], font(kKnobReadoutSize), visage::Font::kCenter, label.x() - 2.0f,
		            label.y(), label.width() + 4.0f, label.height());
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
			// THE UPPER LINE IS THE DOWN THRESHOLD (orange): downward compression acts above it. THE LOWER IS THE UP THRESHOLD
			// (blue): upward compression acts below it. Between them the band is left alone.
			const float yDown = ys.threshold;
			const float yUp = ys.bandwidth;

			canvas.setColor(withAlpha(kValue, 0.07f));
			canvas.rectangle(region.x0, xover.y(), regionW, std::max(0.0f, yDown - xover.y()));
			canvas.setColor(withAlpha(kAccent, 0.07f));
			canvas.rectangle(region.x0, yUp, regionW, std::max(0.0f, xover.bottom() - yUp));

			canvas.setColor(kValue);
			canvas.segment(region.x0, yDown, region.x1, yDown, 2.5f, true);
			canvas.setColor(kAccent);
			canvas.segment(region.x0, yUp, region.x1, yUp, 2.5f, true);

			// A HANDLE THE ARMED SOURCE MAY REACH WEARS THE MODULATION COLOUR AND A HALO — the knob's face glow in this
			// picture's terms (`lookOf`, the same answer the cases read). The strips use it; the bars and thumbs go through
			// their `ModHandle`, which draws the halo, the amount stroke and the live dot itself.
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
			const auto modRing = [&](Handle h, uint32_t colour) {
				const visage::Point c = ringCentre(b, h);
				bandMod(b, h).drawUnder(canvas, c.x, c.y, OwlttGraph::kRingRadius);
				canvas.setColor(kSurface);
				ringAt(canvas, c.x, c.y, OwlttGraph::kRingRadius);
				canvas.setColor(colour);
				ringOutlineAt(canvas, c.x, c.y, OwlttGraph::kRingRadius, 1.5f);
				bandMod(b, h).drawOver(canvas, c.x, c.y, OwlttGraph::kRingRadius);
			};
			// THE RATIO THUMBS: a triangle pointing at its bar from outside the band between the bars, sliding along the
			// ratio track. Orange above the threshold bar (the downward ratio), blue below the bandwidth bar (the upward).
			const auto drawThumbs = [&]() {
				const std::array<std::pair<Handle, uint32_t>, 2> thumbs { std::pair { Handle::DownRatio, kValue },
					                                                    std::pair { Handle::UpRatio, kAccent } };
				const Track track = ratioTrack(b);
				canvas.setColor(withAlpha(kLine, 0.7f));
				for (const auto& [h, colour] : thumbs) {
					const visage::Point c = ringCentre(b, h);
					canvas.fill(track.x0, c.y, track.x1 - track.x0, 1.0f);
					(void)colour;
				}
				for (const auto& [h, colour] : thumbs) {
					const visage::Point c = ringCentre(b, h);
					const bool down = h == Handle::DownRatio;
					bandMod(b, h).drawUnder(canvas, c.x, c.y, kThumbHalf);
					const float tip = down ? c.y + 5.0f : c.y - 5.0f;
					const float base = down ? c.y - 4.0f : c.y + 4.0f;
					canvas.setColor(kSurface);
					canvas.triangle(c.x - kThumbHalf - 1.5f, base - (down ? 1.5f : -1.5f), c.x + kThumbHalf + 1.5f,
					                base - (down ? 1.5f : -1.5f), c.x, tip + (down ? 1.5f : -1.5f));
					canvas.setColor(colour);
					canvas.triangle(c.x - kThumbHalf, base, c.x + kThumbHalf, base, c.x, tip);
					bandMod(b, h).drawOver(canvas, c.x, c.y, kThumbHalf);
				}
			};
			if (!editor_) {
				modRing(Handle::ThresholdBar, kValue);
				modRing(Handle::BandwidthBar, kAccent);
			}

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
			// THE STRIPS ARE NOT BLUE OR ORANGE: those two colours mean upward and downward on this plot (the bars and
			// the thumbs), and the base ratio and the level belong to neither.
			canvas.setColor(withAlpha(tokens::kGammaAxis, 0.6f));
			const float ratioH = ratio * xover.height();
			canvas.rectangle(stripX0, xover.bottom() - ratioH, stripX1 - stripX0, ratioH);

			// Level strip (right).
			canvas.setColor(withAlpha(kSurface, 0.3f));
			canvas.rectangle(stripX1, xover.y(), stripX2 - stripX1, xover.height());
			canvas.setColor(withAlpha(kMuted, 0.55f));
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
			handleRing(ringCentre(b, Handle::RatioStrip), Handle::RatioStrip, tokens::kGammaAxis);
			handleRing(ringCentre(b, Handle::LevelStrip), Handle::LevelStrip, kMuted);

			if (editor_) {
				// THE STRIPS' CAPTIONS, once per band, at the top of each strip.
				canvas.setColor(withAlpha(kMuted, 0.9f));
				canvas.text("RATIO", font(kKnobLabelSize), visage::Font::kCenter, stripX0 - 4.0f, xover.y() + 2.0f,
				            stripX1 - stripX0 + 8.0f, 10.0f);
				canvas.text("LEVEL", font(kKnobLabelSize), visage::Font::kCenter, stripX1 - 4.0f, xover.y() + 2.0f,
				            stripX2 - stripX1 + 8.0f, 10.0f);
			}

			if (editor_) {
				// THE RING AND THUMB HANDLES LAST, over the meter and the strips, so nothing darkens them.
				modRing(Handle::ThresholdBar, kValue);
				modRing(Handle::BandwidthBar, kAccent);
				drawThumbs();
			}

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
	case ThresholdHit::kLowBandwidth:
	case ThresholdHit::kMidBandwidth:
	case ThresholdHit::kHighBandwidth:
		// THE UPPER BAR IS THRESHOLD + 0.6*BANDWIDTH — it widens the compressed band upward.
		return "Upward compression threshold — quiet signals below this are amplified";
	case ThresholdHit::kLowThreshold:
	case ThresholdHit::kMidThreshold:
	case ThresholdHit::kHighThreshold:
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
	// THE THUMBS' TEXT IS THEIR PARAMETERS' OWN.
	case ThresholdHit::kLowDownRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::LowDownRatio)].tooltip;
	case ThresholdHit::kMidDownRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::MidDownRatio)].tooltip;
	case ThresholdHit::kHighDownRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::HighDownRatio)].tooltip;
	case ThresholdHit::kLowUpRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::LowUpRatio)].tooltip;
	case ThresholdHit::kMidUpRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::MidUpRatio)].tooltip;
	case ThresholdHit::kHighUpRatio:
		return owltt::kParamDescs[static_cast<size_t>(owltt::Param::HighUpRatio)].tooltip;
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
	// THE WINDOW'S THUMBS offer the six ratio trims; the card draws none, so it offers none. (The table's boxes are
	// children and declare themselves.)
	if (editor_) {
		for (const owltt::Param p : { owltt::Param::LowUpRatio, owltt::Param::LowDownRatio, owltt::Param::MidUpRatio,
		                              owltt::Param::MidDownRatio, owltt::Param::HighUpRatio,
		                              owltt::Param::HighDownRatio }) {
			out.push_back({ &control(static_cast<size_t>(p)), Road::Gesture });
		}
	}
}

} // namespace feathers::ui
