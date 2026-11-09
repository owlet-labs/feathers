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

#include "mod/mseg/mseg_types.h"

#include <cstddef>
#include <vector>

// WHERE THE SHAPE'S PARTS SIT, IN A UNIT BOX — the whole of the curve editor except the drawing.
//
// SEPARATED FROM THE WIDGET DELIBERATELY, which the DAHDSR's card did not do and paid for. That card
// computes its curve inside `EnvCard`, so every claim about the picture needs a GUI host, a bridge
// and a frame to ask — and four separate bugs in it were found only because someone thought to render
// it. An MSEG's picture is not a readout, it is the EDITOR: breakpoints get dragged, markers get
// moved, and a hit test decides which. That is arithmetic, and arithmetic that decides what a drag
// does should be answerable without a window.
//
// EVERYTHING HERE IS IN A UNIT BOX: x from 0 at the shape's start to 1 at its end, y in the shape's
// own value domain — 0..1, or -1..1 when Settings::bipolar. The widget maps that box to pixels and
// owns nothing else. Same split as `Modulatable::plotUnit`, and for the same reason: a mapping that
// lives in a paint function is a mapping no case can reach.
//
// THE X AXIS IS PROPORTIONAL TO TIME, and this is the one inherited lesson rather than a new one. The
// DAHDSR's card originally sized each stage by its control's POSITION IN ITS RANGE, which is log2
// seconds — so a stage taking fifty times as long drew 1.66x as wide, and an evening went on a
// ten-second period the picture could not show. A shape editor whose x axis is not time is not a
// shape editor.

namespace feathers::mod::mseg {

/// The smallest share of the width a segment that HAPPENS may take. A concession about pixels rather
/// than about time: proportional alone makes a 1 ms segment 0.02% of the width, which is not a
/// handle anyone can grab. A segment that does NOT happen — a duration at the bottom of its range —
/// still gets nothing, because that is a vertical step and drawing it wide would be a lie.
inline constexpr float kMinLiveShare = 0.04f;

/// A draggable point on the shape. There is one per segment END, plus one at the start.
struct Handle {
	float x = 0.0f;
	/// The LEVEL, in 0..1 — not the plotted y. Bipolar is a property of the output, not of the
	/// breakpoint the user placed, so a handle keeps the number the user set and the plot maps it.
	float level = 0.0f;
	/// Which segment this handle ENDS, or kStartHandle for the one at x = 0 — which is also how a
	/// reader tells the start handle: it is `handle.front()`, and it wears the sentinel. (A separate
	/// `isStart` flag rode here too, read by one test and by no production code — backlog EH, shape 1.)
	size_t segment = 0;
};

inline constexpr size_t kStartHandle = static_cast<size_t>(-1);

/// Where everything sits. Recomputed from Settings whenever they move; there is no incremental
/// update, because a cached layout keyed on anything but the settings themselves is the staleness
/// this project keeps paying for.
struct Layout {
	std::vector<Handle> handle;
	/// x of the loop's opening and closing marker. Only meaningful when `loopVisible`.
	float loopStartX = 0.0f;
	float loopEndX = 0.0f;
	bool loopVisible = false;
	/// x of the sustain marker.
	///
	/// PARKED AT 0 WHEN THERE IS NO SUSTAIN POINT, which is a position and not a placeholder: a
	/// marker sits on a segment's RIGHT edge, and no segment's right edge is ever 0, so x = 0 is the
	/// one place on the shape that cannot mean a breakpoint. That is what makes it available to mean
	/// kNoSustain — and it is what gives the drag somewhere to put "none" and somewhere to pick it up
	/// from again. A marker that vanished when cleared would be a one-way gesture.
	float sustainX = 0.0f;
	/// The marker is ON THE PICTURE and can be grabbed.
	///
	/// FALSE WHILE THE SHAPE LOOPS, and that is the honesty rule rather than a decluttering: a
	/// looping shape does not hold at a breakpoint (see `sustainInForce`), so a marker drawn there
	/// would be offering something it cannot do. Asked of the same function the ENGINE asks, so the
	/// picture cannot promise a hold the sound does not perform.
	bool sustainVisible = false;
	/// The gate ACTUALLY HOLDS at `sustainX`. False when the marker is parked at kNoSustain — where
	/// it is drawn dim rather than absent, because the off state still needs a handle.
	bool sustainActive = false;
	/// Each live segment's right-hand edge, in order. `segmentEnd[i]` is where segment i finishes.
	std::vector<float> segmentEnd;
};

Layout layoutOf(const Settings& settings);

/// The polyline the editor draws: x across the unit box, y in the shape's own value domain. Sampled
/// per segment so a curved one reads as a curve; a segment with no duration contributes an edge.
std::vector<std::pair<float, float>> polyline(const Settings& settings, int pointsPerSegment = 12);

/// **THE RIGHT EAR'S LINE** (backlog PA): at each of `polyline`'s xs, where the right ear stands when the left is there —
/// the engine's own walk run `Settings::stereoLead` of the window on (`Shape::rawLeadingAt`), with the gate held, so it
/// wraps through the loop and waits at the sustain point and the end as the sound does. Empty while Stereo Phase is 0.
std::vector<std::pair<float, float>> polylineRight(const Settings& settings, int pointsPerSegment = 12);

/// **THE SHAPE'S LEVEL AT A PICTURE X** — 0..1, the handle domain, not the bipolar plot — in the SAME x
/// the editor draws (the live shares with their visibility floor, `layoutOf`). For a shape that is used
/// AS its picture rather than played in time: Featherverb's IR envelope (backlog JD) multiplies the IR by exactly the line
/// drawn over it, so the drawing cannot disagree with the sound. x outside 0..1 reads the nearer end.
float levelAtX(const Settings& settings, float x);

/// WHICH HANDLE IS UNDER A POINT, or -1. The radii are in unit-box terms, so a caller converts from
/// pixels once rather than this file knowing about them.
///
/// TWO RADII, AND THAT IS FINDING #4 RATHER THAN A GENERALISATION FOR ITS OWN SAKE. The unit
/// box is not square: the editor is 198px across and 72px tall, so ONE radius in unit terms is two
/// different distances on the glass. The card used to pass `max(grabPx/width, grabPx/height)`, which
/// takes the LARGER — 9/72 = 0.125 — and applies it to both axes, so a breakpoint could be caught
/// from 24.7px away sideways and only 9px away vertically. Nine pixels is a fingernail, and a shape
/// editor's handles move mostly VERTICALLY, so the tight axis was the one a hand was aiming along.
/// Separate radii let the card ask for a circle in PIXELS, which is the shape a hand expects.
///
/// A hit is `(dx/radiusX)^2 + (dy/radiusY)^2 <= 1` — an ellipse in the unit box, a circle on the
/// glass when the caller scales each axis by the same pixel count. Nearest wins, measured in that
/// same normalised metric so neither axis outvotes the other.
///
/// Ties go to the LATER handle, which matters more than it sounds: breakpoints can coincide exactly
/// when a segment has no duration, and a hit test that preferred the earlier one would make the
/// second of a coincident pair permanently ungrabbable.
int handleAt(const Layout& layout, float x, float y, float radiusX, float radiusY, bool bipolar);

/// The circular form, for callers whose box IS square or who genuinely mean one distance in unit
/// terms. Kept because the geometry's own cases speak the unit box rather than pixels.
int handleAt(const Layout& layout, float x, float y, float radius, bool bipolar);

/// Move a handle, returning the settings that result. Clamped so the shape stays a shape.
///
/// `x` and `y` are in the unit box. **THE X MOVE TRADES TIME BETWEEN THE SEGMENT BEFORE THE HANDLE
/// AND THE ONE AFTER IT.** Their sum is held constant, so the total is held constant, so every OTHER
/// segment keeps both its share of the window and its place on the screen.
///
/// **THIS NOTE USED TO CLAIM THE OPPOSITE, AND THE CLAIM WAS TRUE OF THE FIELD AND FALSE OF THE
/// SOUND.** It read: *"one drag still edits one number, and the neighbours move on screen only
/// because the box is normalised, not because anything of theirs was touched."* Under normalisation
/// that last clause is exactly backwards. A segment's real length is `share / total × window`, so
/// changing the total IS touching theirs — every other segment was silently retimed by a gesture
/// aimed at one breakpoint, and the drawing was reporting that faithfully rather than distorting it.
///
/// **THE OLD NOTE'S OBJECTION IS WHAT ACTUALLY SETTLES IT, once it is applied to both candidates.**
/// It said an editor becomes unpredictable when "you cannot lengthen anything without shortening
/// something you were not touching". Under a fixed window that is not avoidable — it is arithmetic.
/// The only choice is WHOSE time is taken, and the old behaviour took it from EVERYBODY in
/// proportion. Taking it from the adjacent segment is the local, visible, predictable answer to a
/// question that has no answer where nothing else moves.
///
/// CONSEQUENCES, BOTH DELIBERATE: a handle cannot cross its neighbours, and an interior drag cannot
/// lengthen the shape. The length is `Rate`; the handles divide it — which is what `windowSeconds`
/// and `shares` have always assumed.
Settings withHandleMoved(const Settings& settings, size_t handleIndex, float x, float y);

/// MOVE THE SUSTAIN MARKER, OR CLEAR IT. The same snapping as the loop markers', for the same reason
/// — the value is a segment INDEX, so there is no meaning to a sustain point halfway through a
/// segment and a marker that appeared to allow one would be a lie the engine then rounded away.
///
/// AND `kNoSustain` IS ONE OF THE POSITIONS IT SNAPS TO, at x = 0. The candidates are every live
/// segment's right edge plus the left wall of the box, nearest wins, and the left wall means "none".
/// A drag that could only ever choose a segment would make the sustain point unclearable once set —
/// no control on this card writes `SustainSegment` any other way — so "off" has to be somewhere the
/// gesture can reach, and the one x no breakpoint can occupy is where it belongs.
///
/// REFUSED WHILE THE SHAPE LOOPS, returning the settings untouched: there is no marker to move. The
/// card does not offer the gesture there either, but the refusal lives here so the two cannot drift.
Settings withSustainMarkerMoved(const Settings& settings, float x);

/// Move a loop marker to the nearest segment boundary. Markers snap because they ARE segment indices
/// — there is no meaning to a loop that starts halfway through a segment, and a marker that appeared
/// to allow it would be a control promising something the engine cannot do.
Settings withLoopMarkerMoved(const Settings& settings, bool start, float x);

/// WHICH SEGMENT A POINT IS OVER, or -1 past either end. Normalised x, the same domain the handles
/// use.
int segmentAt(const Layout& layout, float x);

/// ADD A WAYPOINT WHERE THE HAND POINTED, splitting the segment it landed in.
///
/// THE SHAPE MUST NOT MOVE WHEN A WAYPOINT IS ADDED, which is the whole contract. The new breakpoint
/// takes the level the curve ALREADY HAS at that x, and the two halves take the durations that sum
/// to the one they replace — so the drawn line and the played signal are identical either side of
/// the gesture, and what the hand has bought is a handle rather than a change.
///
/// Refused, returning the settings untouched, when the shape is already at kMaxSegments or the x is
/// not over a segment. A shape that silently dropped its last segment to make room would lose work
/// a hand had done.
Settings withWaypointAdded(const Settings& settings, float x);

/// WHAT AN OPT-DRAG CAUGHT, and therefore which segments it bends.
///
/// The target is part
/// of the geometry rather than of the widget, because it decides both what the drag does and what is
/// drawn while it happens. The card asks once and uses the answer for both.
struct CurveTarget {
	enum class Kind { None, Segment, Waypoint };
	Kind kind = Kind::None;
	/// The segment, or the handle index for a waypoint — the same index handleAt returns.
	size_t index = 0;
	/// THE SEGMENTS THIS DRAG BENDS, which is what the card highlights. One for a segment; two for a
	/// waypoint with a neighbour either side; ONE for a waypoint at either end of the shape, which
	/// has only one neighbour to bend.
	size_t first = 0;
	size_t count = 0;

	friend bool operator==(const CurveTarget&, const CurveTarget&) = default;
};

/// A WAYPOINT IF THE POINT IS ON ONE, ELSE THE SEGMENT UNDER IT. The waypoint wins ties for the
/// reason the highlight exists: the two are adjacent targets, and a hand aiming at a breakpoint that
/// got the segment instead would bend one side where it meant to bend both.
///
/// THE SAME RADII THE PLAIN DRAG USES, which is a property worth keeping rather than an accident: a
/// hand that could have grabbed a breakpoint bends both its sides, and a hand that could not bends
/// the one segment. One tolerance, two gestures, no place for them to disagree about what was
/// caught.
CurveTarget curveTargetAt(const Layout& layout, float x, float y, float radiusX, float radiusY,
                          bool bipolar);
CurveTarget curveTargetAt(const Layout& layout, float x, float y, float radius, bool bipolar);

/// REMOVE THE WAYPOINT A HANDLE NAMES, dissolving the segment that ends there into the one after it.
///
/// NOT ASKED FOR AND ADDED ANYWAY — see the commit message. `withWaypointAdded` alone gives a hand a
/// shape it can fill to kMaxSegments and never empty again, and eight is a number a hand reaches in
/// eight double-clicks. An editor that only adds is a trap, so removal ships beside it.
///
/// THE TOTAL DURATION IS PRESERVED: the dissolved segment's seconds are added to the one that follows
/// it, so removing a breakpoint changes the CURVE between its neighbours and nothing else about when
/// the shape ends. The junction's level and bend become the following segment's, which they already
/// were at that point.
///
/// REFUSED, returning the settings untouched, for handle 0 and for the LAST handle. Handle 0 is where
/// the shape starts and is not a segment boundary at all; the last handle is where it ends, and
/// dissolving it would have no following segment to give the time to, so it would silently shorten
/// the shape. Both are the same rule — a removal that has no merge partner is not a removal.
Settings withWaypointRemoved(const Settings& settings, size_t handleIndex);

/// BEND WHAT THE TARGET NAMES. A segment takes `delta`; a waypoint takes `+delta` on the segment
/// BEFORE it and `-delta` on the segment AFTER, which is the inverse pair that makes one drag draw
/// an S through the breakpoint.
Settings withCurvature(const Settings& settings, const CurveTarget& target, float delta);

} // namespace feathers::mod::mseg
