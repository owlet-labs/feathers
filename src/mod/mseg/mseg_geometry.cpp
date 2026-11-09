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

#include "mod/mseg/mseg_geometry.h"
#include "mod/mseg/mseg.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::mseg {

namespace {

/// Each live segment's WIDTH, as a share of the box summing to 1.
///
/// Duration first, then a visibility floor applied to the total — iterated, because widening a
/// segment widens the total the floor is a fraction of, and one pass leaves the last one raised
/// against a total that then grew. The DAHDSR's card learned that the hard way and this is the same
/// loop.
std::vector<float> shares(const Settings& s) {
	const size_t count = std::min(s.count, kMaxSegments);
	std::vector<float> w(count, 0.0f);
	float total = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		w[i] = static_cast<float>(timeSeconds(s.segment[i].share));
		total += w[i];
	}
	if (!(total > 0.0f)) {
		// EVERY SEGMENT A STEP. There is no time in this shape at all, so there is no proportion to
		// draw — the honest picture is equal slivers, which is what an editor needs to still have
		// grabbable handles for a shape made entirely of vertical edges.
		std::fill(w.begin(), w.end(), 1.0f / static_cast<float>(std::max<size_t>(count, 1)));
		return w;
	}
	for (int pass = 0; pass < 4; ++pass) {
		bool raised = false;
		for (size_t i = 0; i < count; ++i) {
			const float floorShare = kMinLiveShare * total;
			if (w[i] > 0.0f && w[i] < floorShare) {
				total += floorShare - w[i];
				w[i] = floorShare;
				raised = true;
			}
		}
		if (!raised) {
			break;
		}
	}
	for (float& x : w) {
		x /= total;
	}
	return w;
}

/// The same curve the engine runs, so the picture cannot disagree with the sound. Kept in step with
/// mseg.cpp's `warp` by being the same expression; a second formula here is the class of bug the
/// filter card's own curve had.
float warp(float phase, float shape) {
	if (shape == 0.0f) {
		return phase;
	}
	const float k = std::exp2(-shape * 1.5849625f);
	return std::pow(std::clamp(phase, 0.0f, 1.0f), k);
}

float levelAt(const Settings& s, size_t segment, float phase) {
	const float from = segment == 0 ? s.startLevel : s.segment[segment - 1].level;
	const float to = s.segment[segment].level;
	return from + (to - from) * warp(phase, s.segment[segment].shape);
}

/// The shape's value as the OUTPUT sees it — bipolar rescale included, because the box the editor
/// draws in is the output's range. The DAHDSR's card clamped -1..1 into 0..1 and flattened half the
/// signal onto the floor; this exists so that mistake has one place it could be made and it is not
/// made there.
float plotted(const Settings& s, float level) {
	return s.bipolar ? 2.0f * level - 1.0f : level;
}

} // namespace

Layout layoutOf(const Settings& settings) {
	Settings s = settings;
	sanitize(&s);

	Layout out;
	const size_t count = std::min(s.count, kMaxSegments);
	const std::vector<float> w = shares(s);

	out.handle.push_back(Handle { 0.0f, s.startLevel, kStartHandle });
	out.segmentEnd.reserve(count);

	float at = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		at += w[i];
		out.segmentEnd.push_back(at);
		out.handle.push_back(Handle { at, s.segment[i].level, i });
	}

	// THE MARKERS SIT ON SEGMENT BOUNDARIES, which is what they are. loopStart opens BEFORE its
	// segment runs, so it sits at that segment's left edge; loopEnd closes AFTER its segment, so it
	// sits at the right edge. Getting these two the same way round is the difference between a loop
	// that includes its first segment and one that skips it.
	if (count > 0) {
		out.loopVisible = (s.mode == Mode::Loop || s.mode == Mode::Free || s.mode == Mode::LfoRetrig);
		const size_t start = std::min(s.loopStart, count - 1);
		const size_t end = std::min(s.loopEnd, count - 1);
		out.loopStartX = start == 0 ? 0.0f : out.segmentEnd[start - 1];
		out.loopEndX = out.segmentEnd[end];

		// TWO DIFFERENT QUESTIONS, WHICH IS WHY THERE ARE TWO FLAGS AND ONLY ONE OF THEM CALLS THE
		// PREDICATE.
		//
		// WHETHER THE GATE HOLDS is `sustainInForce`, the SAME function `Shape::advance` asks — so a
		// marker cannot be drawn as live while the engine ignores it, whatever either of them is
		// later edited to believe. That decision used to be `sustainSegment` alone, which is the bug:
		// a looping shape drew a breakpoint the gate never reaches.
		//
		// WHETHER THE MARKER IS ON THE PICTURE AT ALL cannot be asked of that predicate, because it
		// answers kNoSustain for two different shapes — a looping one, where the marker must go, and
		// a resting one with nothing set, where it must stay so the drag has something to pick up.
		// Only `loop` separates them, so this half reads the flag.
		const int sustain = sustainInForce(s);
		out.sustainVisible = (s.mode == Mode::Gate);
		out.sustainActive = sustain >= 0 && static_cast<size_t>(sustain) < count;
		// PARKED AT THE LEFT WALL WHEN THERE IS NONE. See Layout::sustainX: no segment ends at 0, so
		// that x is unambiguously "no sustain point" and is where the drag both drops it and finds it.
		out.sustainX = out.sustainActive ? out.segmentEnd[static_cast<size_t>(sustain)] : 0.0f;
	}
	return out;
}

std::vector<std::pair<float, float>> polyline(const Settings& settings, int pointsPerSegment) {
	Settings s = settings;
	sanitize(&s);

	const size_t count = std::min(s.count, kMaxSegments);
	const std::vector<float> w = shares(s);
	const int per = std::max(pointsPerSegment, 1);

	std::vector<std::pair<float, float>> out;
	out.reserve(count * static_cast<size_t>(per) + 1);
	out.emplace_back(0.0f, plotted(s, s.startLevel));

	float x0 = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		const float x1 = x0 + w[i];
		// A SEGMENT WITH NO DURATION IS A VERTICAL EDGE, and it still gets a share for grabbing — so
		// it is drawn as the edge it is: one point at the same x, at the new level. Sampling it like a
		// ramp would draw a slope the engine does not play.
		if (timeSeconds(s.segment[i].share) <= 0.0) {
			out.emplace_back(x0, plotted(s, s.segment[i].level));
			out.emplace_back(x1, plotted(s, s.segment[i].level));
			x0 = x1;
			continue;
		}
		for (int p = 1; p <= per; ++p) {
			const float t = static_cast<float>(p) / static_cast<float>(per);
			out.emplace_back(x0 + t * (x1 - x0), plotted(s, levelAt(s, i, t)));
		}
		x0 = x1;
	}
	return out;
}

std::vector<std::pair<float, float>> polylineRight(const Settings& settings, int pointsPerSegment) {
	Settings s = settings;
	sanitize(&s);
	const float lead = s.stereoLead();
	if (lead == 0.0f) {
		return {};
	}
	// `polyline`'s walk, point for point — the same xs, so the two lines are read against one axis — with each y the
	// right ear's reading at the place the left ear's point stands for.
	const size_t count = std::min(s.count, kMaxSegments);
	const std::vector<float> w = shares(s);
	const int per = std::max(pointsPerSegment, 1);
	const auto right = [&](size_t segment, float phase) {
		return plotted(s, Shape::rawLeadingAt(s, segment, phase, lead, true));
	};

	std::vector<std::pair<float, float>> out;
	out.reserve(count * static_cast<size_t>(per) + 1);
	out.emplace_back(0.0f, right(0, 0.0f));
	float x0 = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		const float x1 = x0 + w[i];
		if (timeSeconds(s.segment[i].share) <= 0.0) {
			out.emplace_back(x0, right(i, 1.0f));
			out.emplace_back(x1, right(i, 1.0f));
			x0 = x1;
			continue;
		}
		for (int p = 1; p <= per; ++p) {
			const float t = static_cast<float>(p) / static_cast<float>(per);
			out.emplace_back(x0 + t * (x1 - x0), right(i, t));
		}
		x0 = x1;
	}
	return out;
}

float levelAtX(const Settings& settings, float x) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	const std::vector<float> w = shares(s);
	const float at = std::clamp(std::isfinite(x) ? x : 0.0f, 0.0f, 1.0f);
	float level = s.startLevel;
	float x0 = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		const float x1 = x0 + w[i];
		// A STEP, as the polyline draws it: the new level from its x on.
		if (timeSeconds(s.segment[i].share) <= 0.0) {
			if (at < x0) {
				return level;
			}
			level = s.segment[i].level;
			x0 = x1;
			continue;
		}
		if (at < x1 || i + 1 == count) {
			const float t = x1 > x0 ? (at - x0) / (x1 - x0) : 1.0f;
			return levelAt(s, i, std::clamp(t, 0.0f, 1.0f));
		}
		level = s.segment[i].level;
		x0 = x1;
	}
	return level;
}

int handleAt(const Layout& layout, float x, float y, float radiusX, float radiusY, bool bipolar) {
	// A DIVISION PER AXIS, WHICH IS WHAT MAKES THE TWO RADII MEAN ANYTHING. Scaling each difference by
	// its own radius puts both axes in the same units — "fractions of the tolerance" — so the
	// comparison against 1 is an ellipse and the NEAREST test below is measured in a metric where
	// neither axis outvotes the other. A Euclidean distance against one radius, which is what this
	// was, cannot express a tolerance that differs by axis at all.
	const float rx = std::max(radiusX, 1e-6f);
	const float ry = std::max(radiusY, 1e-6f);
	int found = -1;
	float best = 1.0f;
	for (size_t i = 0; i < layout.handle.size(); ++i) {
		const Handle& h = layout.handle[i];
		const float hy = bipolar ? 2.0f * h.level - 1.0f : h.level;
		const float dx = (h.x - x) / rx;
		const float dy = (hy - y) / ry;
		const float d2 = dx * dx + dy * dy;
		// `<=` RATHER THAN `<`, so a later handle wins an exact tie. Breakpoints coincide whenever a
		// segment has no duration, and preferring the earlier one would make the second of a
		// coincident pair permanently ungrabbable — a control that cannot be reached at all.
		if (d2 <= best) {
			best = d2;
			found = static_cast<int>(i);
		}
	}
	return found;
}

int handleAt(const Layout& layout, float x, float y, float radius, bool bipolar) {
	return handleAt(layout, x, y, radius, radius, bipolar);
}

int segmentAt(const Layout& layout, float x) {
	for (size_t i = 0; i < layout.segmentEnd.size(); ++i) {
		// `<=` ON THE RIGHT EDGE, so the last segment owns x == 1 and a click on the far edge lands
		// somewhere rather than nowhere. A boundary belongs to the segment that ENDS there, which is
		// also the one whose handle sits on it.
		if (x <= layout.segmentEnd[i]) {
			return static_cast<int>(i);
		}
	}
	return layout.segmentEnd.empty() ? -1 : static_cast<int>(layout.segmentEnd.size() - 1);
}

Settings withWaypointAdded(const Settings& settings, float x) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	if (count == 0 || count >= kMaxSegments) {
		return s; // no room; see the header on why this refuses rather than making room
	}

	const Layout layout = layoutOf(s);
	const int hit = segmentAt(layout, x);
	if (hit < 0) {
		return s;
	}
	const size_t seg = static_cast<size_t>(hit);

	// WHERE IN THAT SEGMENT, as a fraction of its own width — the same share the layout drew it at,
	// so the split lands under the pointer rather than under an even division of the shape.
	const float left = seg == 0 ? 0.0f : layout.segmentEnd[seg - 1];
	const float span = layout.segmentEnd[seg] - left;
	const float t = span > 0.0f ? std::clamp((x - left) / span, 0.0f, 1.0f) : 0.5f;

	// THE LEVEL THE CURVE ALREADY HAS THERE, so the shape does not move. levelAt walks the segment's
	// own warp, so this is right for a bent segment as well as a straight one.
	const float level = levelAt(s, seg, t);
	const double seconds = timeSeconds(s.segment[seg].share);

	// SHIFTED FROM THE BACK, so no segment's settings are overwritten before they are read.
	for (size_t i = count; i > seg + 1; --i) {
		s.segment[i] = s.segment[i - 1];
	}
	// BOTH HALVES KEEP THE ORIGINAL'S SHAPE. A split of a bent segment into two straight ones would
	// be the gesture changing the sound, which is the one thing adding a handle must not do — and
	// keeping the bend on both is the closest a two-segment approximation gets to the old curve.
	s.segment[seg + 1] = s.segment[seg];
	s.segment[seg].share = timeValueFor(seconds * static_cast<double>(t));
	s.segment[seg].level = level;
	s.segment[seg + 1].share = timeValueFor(seconds * static_cast<double>(1.0f - t));
	s.count = count + 1;

	// THE MARKERS AND THE BREAKPOINT FOLLOW THE SEGMENT THEY NAMED. An index past the split now names
	// the segment after it, and a loop or a sustain point that stayed put would silently move to a
	// different part of the shape.
	if (s.loopStart > seg) {
		++s.loopStart;
	}
	if (s.loopEnd >= seg) {
		++s.loopEnd;
	}
	if (s.sustainSegment >= 0 && static_cast<size_t>(s.sustainSegment) >= seg) {
		s.sustainSegment += 1;
	}
	sanitize(&s);
	return s;
}

CurveTarget curveTargetAt(const Layout& layout, float x, float y, float radiusX, float radiusY,
                          bool bipolar) {
	CurveTarget out;
	const int handle = handleAt(layout, x, y, radiusX, radiusY, bipolar);
	const size_t segments = layout.segmentEnd.size();
	if (segments == 0) {
		return out;
	}

	if (handle >= 0) {
		out.kind = CurveTarget::Kind::Waypoint;
		out.index = static_cast<size_t>(handle);
		// A WAYPOINT SITS BETWEEN TWO SEGMENTS AND BENDS BOTH. Handle 0 is the shape's start and
		// handle N is its end, and each of those has one neighbour rather than two — so the drag
		// bends the one side it has. THAT IS A CHOSEN BEHAVIOUR rather than a specified one; the
		// alternative was to refuse the end waypoints a curvature gesture at all, which would make
		// two of the handles silently different from the rest.
		const size_t before = out.index;             // segments 0..N-1, so handle i follows segment i-1
		out.first = before > 0 ? before - 1 : 0;
		const bool hasBefore = before > 0;
		const bool hasAfter = before < segments;
		out.count = (hasBefore ? 1u : 0u) + (hasAfter ? 1u : 0u);
		return out;
	}

	const int seg = segmentAt(layout, x);
	if (seg < 0) {
		return out;
	}
	out.kind = CurveTarget::Kind::Segment;
	out.index = static_cast<size_t>(seg);
	out.first = out.index;
	out.count = 1;
	return out;
}

CurveTarget curveTargetAt(const Layout& layout, float x, float y, float radius, bool bipolar) {
	return curveTargetAt(layout, x, y, radius, radius, bipolar);
}

Settings withWaypointRemoved(const Settings& settings, size_t handleIndex) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	// NO MERGE PARTNER, NO REMOVAL. Handle 0 is the shape's start rather than a segment boundary, and
	// handle `count` is its end — dissolving either has nothing to give the time to. See the header.
	if (count < 2 || handleIndex == 0 || handleIndex >= count) {
		return s;
	}

	// The handle ENDS segment `handleIndex - 1`; the segment that absorbs it is `handleIndex`.
	const size_t dissolved = handleIndex - 1;
	const double merged =
	    timeSeconds(s.segment[dissolved].share) + timeSeconds(s.segment[dissolved + 1].share);

	s.segment[dissolved] = s.segment[dissolved + 1];
	s.segment[dissolved].share = timeValueFor(merged);
	for (size_t i = dissolved + 1; i + 1 < count; ++i) {
		s.segment[i] = s.segment[i + 1];
	}
	s.count = count - 1;

	// THE MARKERS FOLLOW THE SEGMENT THEY NAMED, the exact inverse of the shift withWaypointAdded
	// does. A marker on either of the two segments that just became one names the merged one; a
	// marker past them slides down by the segment that went away.
	const auto remap = [handleIndex](size_t segment) {
		return segment >= handleIndex ? segment - 1 : segment;
	};
	s.loopStart = remap(s.loopStart);
	s.loopEnd = remap(s.loopEnd);
	if (s.sustainSegment >= 0) {
		s.sustainSegment = static_cast<int>(remap(static_cast<size_t>(s.sustainSegment)));
	}
	sanitize(&s);
	return s;
}

Settings withCurvature(const Settings& settings, const CurveTarget& target, float delta) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	if (target.kind == CurveTarget::Kind::None || count == 0) {
		return s;
	}

	const auto bend = [&](size_t segment, float by) {
		if (segment >= count) {
			return;
		}
		s.segment[segment].shape = std::clamp(s.segment[segment].shape + by, -kShapeMax, kShapeMax);
	};

	if (target.kind == CurveTarget::Kind::Segment) {
		bend(target.index, delta);
		return s;
	}

	// THE INVERSE PAIR. One drag, opposite bends, so the curve leaves the breakpoint the way it
	// arrived — which is what makes a waypoint drag draw an S rather than a corner.
	const size_t before = target.index;
	if (before > 0) {
		bend(before - 1, delta);
	}
	if (before < count) {
		bend(before, -delta);
	}
	return s;
}

Settings withHandleMoved(const Settings& settings, size_t handleIndex, float x, float y) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);

	// THE LEVEL IS THE EASY HALF: the y is in the OUTPUT's domain, so a bipolar shape has to be
	// undone before it is stored. A breakpoint keeps the number the player set, not the number the
	// output showed.
	const float level = std::clamp(s.bipolar ? (y + 1.0f) * 0.5f : y, 0.0f, 1.0f);

	if (handleIndex == 0) {
		// The start handle has no duration before it, so only its level moves.
		s.startLevel = level;
		sanitize(&s);
		return s;
	}

	const size_t segment = handleIndex - 1;
	if (segment >= count) {
		return s;
	}
	s.segment[segment].level = level;

	// THE X HALF: where the handle sits is the END of its segment — and the time it takes is TRADED
	// WITH THE NEXT SEGMENT rather than simply written.
	//
	// **BECAUSE A SHARE IS NOT A DURATION, AND MOVING ONE MOVED EVERY OTHER POINT.** `shares()` normalises by the SUM, so a segment's real
	// length is `share / total × window`. Writing one share changes the total, and therefore:
	//
	//   - every other handle's x moves — the ones before it drift left, the ones after it drift
	//     right — because each is a fixed prefix over a denominator that just grew; and
	//   - **every other segment's PLAYING TIME changes with it**, which is the half that is not about
	//     the picture at all. Dragging one breakpoint retimed the whole shape.
	//
	// **TRADING WITH THE NEIGHBOUR FIXES BOTH AT ONCE.** Take time from this segment and give the same
	// time to the next; their sum is constant, so the total is constant, so every other segment's
	// share of it is untouched — in the drawing and in the sound. The handle lands exactly where the
	// pointer is, because the denominator it is measured against no longer moves while it is moving.
	//
	// WHAT IT COSTS, AND BOTH WERE DECIDED RATHER THAN DISCOVERED: a handle can no longer cross its
	// neighbours (it is clamped to the pair it divides, which is what "does not disturb the others"
	// MEANS), and an interior drag can no longer lengthen the shape. The length is `Rate`; the handles
	// divide it. That is the model `windowSeconds` and `shares` already assume — *"a share never
	// becomes a duration; the WINDOW is what changes"* — and this is the editor finally agreeing.
	//
	// SOLVED, NOT ITERATED. It is not the hit test; that measures
	// symmetric and adequate at both edges. It is THIS.
	//
	// The old line was `seconds = span * totalSeconds(s)`, where `span` is the distance from the
	// previous handle in the UNIT BOX and the total is the shape's CURRENT length. But writing the
	// duration changes the total, and the box is normalised BY the total — so the handle lands
	// somewhere short of the pointer, and the next mouse event repeats from there. A fixed-point
	// iteration, dressed as an assignment.
	//
	// MEASURED, on three even 200 ms segments: dragging handle 1 to x = 0.5 converged after about
	// ten mouse events; to x = 0.9 it took sixty and inflated the shape from 0.6 s to 3.99 s; to
	// x = 0.98 it STALLED at 0.92 and never arrived, leaving the handle ~12px behind the pointer —
	// which is the grab radius, so the handle looks dropped. That is the whole of the reported feel.
	//
	// The closed form. With `prefix` the time before this segment and `suffix` the time after it,
	// both untouched by this drag, the handle sits at (prefix + s) / (prefix + s + suffix), so
	//     s = x * suffix / (1 - x) - prefix
	// which lands the handle exactly where the pointer is, in ONE event, at every reachable x.
	//
	// AND IT MAKES THE REAL LIMIT HONEST. `s` diverges as x approaches 1, so a segment's declared
	// maximum duration is a hard right-hand wall: on that same shape no gesture can take handle 1
	// past x = 0.9615, because doing so would need a segment longer than kTimeLog2SecondsMax. That
	// wall exists either way — the old code merely crawled towards it instead of stopping at it.
	//
	// THE ONE APPROXIMATION, stated because it is real: `shares()` gives a segment that HAPPENS a
	// visibility floor of kMinLiveShare, so on a shape holding a segment below that floor the drawn
	// x is not exactly proportional to time and this solve lands slightly off. It is exact for every
	// shape whose live segments all clear the floor.
	double prefix = 0.0;
	double suffix = 0.0;
	for (size_t i = 0; i < count; ++i) {
		const double seconds = timeSeconds(s.segment[i].share);
		if (i < segment) {
			prefix += seconds;
		}
		else if (i > segment) {
			suffix += seconds;
		}
	}

	const size_t next = segment + 1;
	if (next < count) {
		// THE TOTAL, TAKEN BEFORE THE TRADE AND UNCHANGED BY IT — which is the whole mechanism. Both
		// halves of the pair are rewritten below and their sum is preserved, so this denominator is
		// as true after the drag as before, and every handle that is not this one keeps its x.
		const double mine = timeSeconds(s.segment[segment].share);
		const double pair = mine + timeSeconds(s.segment[next].share);
		const double total = prefix + mine + suffix;

		// WHERE THE POINTER IS, IN TIME. No division by `1 - x` and so no divergence at the right-hand
		// edge: the handle is bounded by its neighbours rather than by how long a segment may be, and
		// the old closed form's hard wall at x = 0.9615 goes with it.
		const double at = std::clamp(static_cast<double>(x), 0.0, 1.0);
		const double wanted = std::clamp(at * total - prefix, 0.0, pair);

		// CLAMPED TO THE PAIR, NOT TO ZERO-AND-INFINITY. Past either neighbour there is no time to
		// take, so the handle stops on it — the two ends of this clamp ARE the two adjacent points.
		s.segment[segment].share = timeValueFor(wanted);
		s.segment[next].share = timeValueFor(pair - wanted);
	}
	// THE LAST HANDLE TAKES NO HORIZONTAL MOVE AT ALL, and that is a decision — see the header.
	// Its x is 1.0 by construction, because the box is normalised to the shape's own length and the
	// last breakpoint IS the end of the shape. The old code let the drag write the final segment's
	// duration anyway, and MEASURED, that was destructive: dragging it to any x below the right edge
	// drove the last segment to zero seconds — 0.200 s to 0.000 s, the shape from 0.6 s to 0.4 s —
	// while the handle did not move a pixel. A gesture that appears dead and is not is worse than one
	// that is dead, so this one is now genuinely dead.

	sanitize(&s);
	return s;
}

Settings withSustainMarkerMoved(const Settings& settings, float x) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	// A LOOPING SHAPE HAS NO SUSTAIN MARKER TO MOVE. Refused rather than clamped, because writing an
	// index the engine has already overruled would be the gesture pretending to have done something.
	if (count == 0 ||
	    s.mode == Mode::Loop || s.mode == Mode::Free || s.mode == Mode::LfoRetrig) {
		return s;
	}
	const Layout layout = layoutOf(s);

	// THE CANDIDATES: the left wall, meaning kNoSustain, and then every live segment's RIGHT edge,
	// meaning that segment. Seeded with the wall so "none" is the answer a drag past the left of the
	// shape gives, and `<` so a genuine boundary has to be strictly nearer to win — no boundary is
	// ever AT the wall, so the tie can only arise on a degenerate shape.
	int nearest = kNoSustain;
	float bestDistance = std::abs(x);
	for (size_t i = 0; i < count; ++i) {
		const float d = std::abs(layout.segmentEnd[i] - x);
		if (d < bestDistance) {
			bestDistance = d;
			nearest = static_cast<int>(i);
		}
	}

	s.sustainSegment = nearest;
	sanitize(&s);
	return s;
}

Settings withLoopMarkerMoved(const Settings& settings, bool start, float x) {
	Settings s = settings;
	sanitize(&s);
	const size_t count = std::min(s.count, kMaxSegments);
	if (count == 0) {
		return s;
	}
	const Layout layout = layoutOf(s);

	// NEAREST BOUNDARY, because a marker IS a segment index. A loop that began halfway through a
	// segment is not something the engine can run, so an editor that let one be dropped there would
	// be offering a position it then silently rounded away — the honesty rule, applied to a drag.
	size_t nearest = 0;
	float bestDistance = 1e9f;
	for (size_t i = 0; i < count; ++i) {
		// Candidate boundaries: for the opening marker, each segment's LEFT edge; for the closing
		// one, each segment's RIGHT edge. Same asymmetry as layoutOf's, and for the same reason.
		const float edge = start ? (i == 0 ? 0.0f : layout.segmentEnd[i - 1]) : layout.segmentEnd[i];
		const float d = std::abs(edge - x);
		if (d < bestDistance) {
			bestDistance = d;
			nearest = i;
		}
	}

	if (start) {
		s.loopStart = nearest;
		s.loopEnd = std::max(s.loopEnd, s.loopStart);
	}
	else {
		s.loopEnd = nearest;
		s.loopStart = std::min(s.loopStart, s.loopEnd);
	}
	sanitize(&s);
	return s;
}

} // namespace feathers::mod::mseg
