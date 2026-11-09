/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/sklitter/macro_push.h"
// FOR `Settings` ALONE, which `performPushOf` below reads. **THE DIRECTION IS ONE-WAY AND MUST STAY SO:**
// the circle's maths depends on the types, never the other way round — `sklitter_types.h` is what every
// card, the registry and the tests include, and a back-edge from it to here would be a cycle.
#include "fx/sklitter/sklitter_types.h"

// **AND THE RULE ABOVE NOW ENFORCES ITSELF, because a rule that only a comment states is one a future
// include will break silently.** `sklitter_types.h` defines this marker at its END. On the legal path
// that header completes before this line is reached and the check costs nothing. On a BACK-EDGE —
// somebody adds `#include "fx/sklitter/perform.h"` to `sklitter_types.h` — a TU that includes the types
// header first re-enters it here, `#pragma once` makes the re-entry empty, the marker is still undefined
// because that header has not reached its end, and this fires. **The alternative is what made this worth
// wiring:** without it the compiler reports `Settings` as an incomplete or unknown type somewhere inside
// a function body below, naming neither include and reading like a bug in this file.
#ifndef FEATHERS_SKLITTER_TYPES_H_COMPLETE
#error "sklitter/perform.h was entered while sklitter_types.h was still being included — the one-way include rule is broken. perform.h may depend on sklitter_types.h; sklitter_types.h must NOT include perform.h."
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <tuple> // std::tuple_size_v, for the Corner-width asserts below
#include <utility> // std::pair, for `waypointAxis`

// THE PERFORM CIRCLE'S ARITHMETIC — design §2, as amended 2026-09-19. Four waypoints fixed at the
// DIAGONAL rim points (`kWaypointAxis`), the centre neutral. Pure functions, so the device, the card
// and the panel cannot disagree about a weight.
namespace feathers::fx::sklitter {

/// **THE FOUR WAYPOINTS, BY POSITION IN THE ORDER THEY ARE DRAWN — NOT BY COMPASS POINT.**
///
/// They were `kN`, `kE`, `kS`, `kW` and the compass is GONE, deliberately and entirely (2026-09-19): the
/// `Cfg N`…`Cfg W` captions on the perform rail were clutter, and rotating the names 45° with the
/// geometry would have kept exactly what was being removed — the north/east/south/west labels go away.
///
/// **SO THIS IS A DELETION RATHER THAN A ROTATION.** Renaming to `kNW`…`kSW` would have preserved in
/// two hundred places the exact vocabulary being removed — and it would have been *more* wrong than
/// what it replaced, because a compass name that has been turned is a name a reader has to un-turn.
///
/// **THE ORDER IS THE PICTURE'S: clockwise from the top-left**, which is where the four `Cfg` cells sit
/// on the rail and where `waypointAxis` puts the four rim marks. 1 is top-left, 2 top-right, 3
/// bottom-right, 4 bottom-left. **The indices did not move** — `kCfg1` is still 0 — so
/// `Settings::cornerCfg[0..3]`, `Param::Cfg1..Cfg4`'s ids 17-20 and the state block are all where they
/// were, and no default came off its slot. This was names only.
enum Corner : size_t { kCfg1 = 0, kCfg2 = 1, kCfg3 = 2, kCfg4 = 3, kCorners = 4 };

/// **WHERE A WAYPOINT SITS ON THE RIM, ON ITS OWN AXIS — and the reason it is √2 rather than 1.**
///
/// With the four `Cfg` cells at the corners of the PERFORM rail, the new cardinals sit 45 degrees round to
/// match the cells' locations. So the four
/// waypoints are at the DIAGONAL rim points — `kCfg1` at the top-LEFT, then clockwise — and each one is
/// beside the cell that configures it. **That is what makes "which waypoint is this" a fact about the
/// picture rather than a convention to be learned**, which is the thing the cardinal arrangement had
/// and the first draft of the corner one gave away.
///
/// **THE ROTATION IS WRITTEN WITHOUT ITS COSINE, AND THAT IS DELIBERATE RATHER THAN A SAVED MULTIPLY.**
/// The honest 45° rotation is `u = (x − y)/√2`, `v = (x + y)/√2`. Dropping both divides scales every
/// one of the five weights by exactly √2, and the normalise at the end of `performWeightsOf` divides
/// by their total — so a common factor cannot survive it. **The blend is therefore EXACTLY today's
/// blend read at a rotated point, not an approximation of it.**
///
/// **WHAT THE DIVIDES WOULD HAVE COST IS THE BIT-EXACTNESS AT A RIM POINT.** `fl(1/√2)²·2` is
/// 0.99999997, so a puck at a waypoint would leak a 3e-8 centre share and hand back 0.99999997 of that
/// waypoint's push instead of the push — quietly turning *"at depth 1 a rim point IS that waypoint's
/// push"* (below, and its case) from a bit-identity into an approximation. Written this way the
/// identity survives, and it survives for a reason worth stating: **`fl(√2)` is bit-for-bit
/// `2·fl(1/√2)`** — same significand, exponent one apart, so the rounding is the same rounding — and a
/// puck at `(−s, −s)` with `s = fl(1/√2)` gives `x − y == 0` and `x + y == −2s` EXACTLY. The centre
/// then clamps to a hard zero, the total IS the one corner, and `c / total` is exactly 1.0f.
inline constexpr float kWaypointAxis = 1.41421356237309505f;

/// **WHERE THE PUCK GOES TO PLAY ONE WAYPOINT, AS A UNIT DIRECTION — THE ONLY DECLARATION OF IT.**
///
/// `performWeightsOf` says what a puck position MEANS; this says where a waypoint IS, and they are two
/// readings of one rotation. The pad draws its marks from this (`perform_pad::waypointPoint`) and every
/// fixture that wants "the puck on E" places it from this, so a rotation cannot reach the weights and
/// miss the picture — or reach both and leave the tests asserting the old country.
///
/// **IT IS `fl(1/√2)` AND IT HAS TO BE THAT EXACT FLOAT.** `2·fl(1/√2)` is bit-for-bit `fl(√2)`, which
/// is `kWaypointAxis` — so a puck placed here gives `x − y` or `x + y` exactly equal to the constant the
/// centre share is measured against, the centre clamps to a hard zero, and the waypoint's weight is
/// exactly 1.0f. Round this to 0.707f and that identity is gone, silently.
///
/// **THE ORDER IS THE CELLS' ORDER, WHICH IS THE WHOLE POINT OF THE ROTATION** — `kCfg1` top-left, `kCfg2`
/// top-right, `kCfg3` bottom-right, `kCfg4` bottom-left, clockwise, each beside the square that configures it.
[[nodiscard]] inline constexpr std::pair<float, float> waypointAxis(Corner c) {
	constexpr float d = 0.70710678118654752f;
	switch (c) {
	case kCfg1:
		return { -d, -d };
	case kCfg2:
		return { d, -d };
	case kCfg3:
		return { d, d };
	default:
		return { -d, d };
	}
}

/// **THE ONE ARITHMETIC FACT THE RIM IDENTITY RESTS ON, CHECKED RATHER THAN ARGUED.**
///
/// Everything above explains why a puck on a waypoint gives a weight of exactly 1.0f: `fl(√2)` is
/// bit-for-bit `2·fl(1/√2)`, so `x ± y` lands EXACTLY on the constant the centre share is measured
/// against, the centre clamps to a hard zero, and the total IS the one live corner. **An explanation is
/// not a check.** If someone rounds the literal in `waypointAxis`, edits `kWaypointAxis`, or a future
/// toolchain rounds either differently, the identity goes — and it goes SILENTLY, in the safe-looking
/// direction: every case still passes at `Approx`, the weight reads 0.99999997, and the bit-exact
/// guarantee that `perform.h` and its memcmp case are built on has quietly become approximate.
///
/// These two lines fail the BUILD instead. They are written as the exact expressions `performWeightsOf`
/// computes — `x + y` for the `kCfg1`/`kCfg3` axis and `x − y` for `kCfg2`/`kCfg4` — so what is pinned
/// is the arithmetic that runs, not a restatement of it.
static_assert(waypointAxis(kCfg1).first + waypointAxis(kCfg1).second == -kWaypointAxis,
              "a puck on waypoint 1 must give x + y == -kWaypointAxis EXACTLY, or the centre share "
              "does not clamp to zero there and a rim point stops being that waypoint's push to the "
              "bit — see the comments above kWaypointAxis");
static_assert(waypointAxis(kCfg2).first - waypointAxis(kCfg2).second == kWaypointAxis,
              "a puck on waypoint 2 must give x - y == kWaypointAxis EXACTLY, for the same reason");

/// **HOW SHARPLY `Perf Depth`'s TRAVEL CURVES — A BY-EAR NUMBER, NOT A DERIVED ONE.**
///
/// Played, full macro strength was usually too strong: the useful region of a linear strength knob was
/// concentrated in its first 1-10%. So the curve goes on the STRENGTH knob, which is `Perf Depth`. The amount becomes `depth^k` rather than `depth`; `performAmountOf` is
/// the one place it is applied, and the puck's radius is untouched.
///
/// **DECISION: CURVE THE TRAVEL, DO NOT CAP THE TOP.** Chosen over lowering the ceiling, so the knob at
/// full still reaches the blend WHOLE — what changes is that full stops occupying most of the travel.
///
/// **SIX IS A STARTING POINT AND THE ARITHMETIC BEHIND IT IS STATED SO IT CAN BE ARGUED WITH**, not
/// because it is right: against "the first 5-10%", `0.7^6 ≈ 0.12` puts the knob at seven tenths at about
/// a tenth of full, and half-way at `0.5^6 ≈ 1.6%`. That is a reading of one sentence, not a measurement
/// of anything — **it is a setting to tune, exactly as the four waypoint configurations are, and it is listed
/// with them in backlog FP.**
///
/// **AND THE NUMBERS INTERACT, WHICH IS WHY THEY ARE FILED TOGETHER.** FP records that two of the four
/// default configurations sit on triangle nulls, so half the rim is quieter than the design assumes; a
/// curve judged against that rim may want re-judging once the configurations move. The choice was curve
/// first and corners later knowing that.
inline constexpr float kDepthCurve = 6.0f;

struct PerformWeights {
	/// **THE CENTRE'S SHARE — THE PART OF THE PUCK'S TRAVEL THAT IS SPENT ON NOTHING.** It was named
	/// `knob` and was the `Macro` / `Macro Cfg` knobs' own share; those two controls are DELETED
	/// (2026-09-19, we will most likely get rid of the center macro and have it be zero), so the
	/// centre is a constant zero push and this weight multiplies nothing.
	///
	/// **IT SURVIVES WITH NOTHING TO MULTIPLY, AND THAT IS WHAT MAKES THE RADIUS THE AMOUNT.** It is what
	/// the normalise below divides by, so it is the difference between a puck part-way to E playing SOME
	/// of E and playing E whole. Delete it and every point on the disk would play a rim waypoint at full
	/// strength. `blendPush` does not read it — the sum it is missing from IS its effect.
	///
	/// **AND IT IS LINEAR, WHICH WAS RE-DECIDED RATHER THAN LEFT ALONE.** A curve was briefly applied to
	/// `1 − centre` on 2026-09-19 and taken out the same day: the control meant was the STRENGTH
	/// KNOB, not the radius. See `kDepthCurve` and `performAmountOf`.
	float centre = 1.0f;
	std::array<float, kCorners> corner {}; ///< indexed by `Corner`

	/// **THE AXES THESE WEIGHTS WERE ACTUALLY BUILT FROM — de-NaN'd and clamped, as `performWeightsOf`
	/// left them.** They travel WITH the weights rather than beside them, and that is the whole point:
	/// the device publishes `Perf X`/`Perf Y` as aux outputs (design §4) and would otherwise have to
	/// sanitise the puck a second time to do it — a second road, which is the copy that goes stale.
	/// Reading them off the same `PerformWeights` value means the published axis and the published
	/// weight cannot disagree by construction, whatever the caller handed in.
	///
	/// **THE SANITISER DOES NOT NORMALISE THE SIGN OF A ZERO, and that is said here rather than
	/// guarded against.** `std::clamp(-0.0f, -1.0f, 1.0f)` returns its argument, so a puck handed in
	/// at -0.0f is published at -0.0f. It compares equal to 0.0f, so no case and no destination can
	/// tell — unlike the CORNERS, whose zeros come out +0 because `std::max(0.0f, -0.0f)` returns its
	/// first argument, which is a guarantee `blendPush` leans on and this pair is not part of.
	///
	/// **THE CORNERS' +0 STILL MATTERS AFTER THE CENTRE WENT TO ZERO, for a slightly different reason
	/// than it used to.** It bought bit-identity with the knobs' push before; now it buys bit-identity
	/// with the WAYPOINT's push at a rim point, because `+0 · anything` never carries a magnitude into
	/// the sum and `v + ±0 == v`.
	float x = 0.0f;
	float y = 0.0f;
};

/// x right, y DOWN, as every screen space in this editor (perform_stage.h). **THE WAYPOINTS ARE ON THE
/// DIAGONALS SINCE 2026-09-19** (`kWaypointAxis`): `kCfg1` is the top-LEFT rim point, `kCfg2` the top-right,
/// `kCfg3` the bottom-right, `kCfg4` the bottom-left — clockwise, each one beside the cell that configures
/// it. Clamped to the square, so a routed offset cannot throw the weights out of [0, 1].
///
/// **STRAIGHT OUT TO THE RIGHT IS NOW A CROSSFADE RATHER THAN A WAYPOINT, and the halves are exact.**
/// The puck at (1, 0) sits exactly between `kCfg2` and `kCfg3`, so it reads 0.5/0.5 — where before the
/// rotation that point WAS a waypoint, whole. That is the rotation showing, not a bug, and it is the
/// one place a reader of the old numbers will trip.
///
/// **THE CENTRE IS EXACT TO THE BIT, AND `blendPush` DEPENDS ON IT.** At (0, 0) `centre` is 1.0f and every
/// corner is +0.0f — not -0.0f: `std::max(0.0f, -0.0f)` returns its FIRST argument when the two compare
/// equal, so the sign of a zero input never reaches a weight. **SINCE THE CENTRE BECAME ZERO (2026-09-19)
/// THE CLAIM THIS BUYS IS DIFFERENT AND STRONGER**: it used to make the centre reproduce the knobs' push
/// bit for bit; it now makes the centre the ZERO push — every term exactly +0.0f, so `MacroPush {}` to the
/// bit — and makes a rim point the waypoint's own push to the bit as well, because the one weight in the
/// sum is exactly 1.0f and the other four contribute signed zeros.
///
/// **THE HEADER'S CONTRACT ON A NON-FINITE PUCK: A NaN AXIS READS AS CENTRED — it is replaced by 0, exactly
/// as if that axis were at rest, and the OTHER axis is untouched.** So a NaN on both axes, or on one with the
/// other at rest, is the centre: this surface's own bypass, which makes a broken modulation source leave the
/// circle inert rather than silent. A NaN X with a live Y is the puck at (0, y), not the centre. Without
/// the de-NaN below the arithmetic is worse than it looks: `std::clamp` PASSES a NaN through, then every
/// `std::max(0.0f, NaN)` returns its FIRST argument, so all four corners and the centre come out 0, the total
/// is 0, and the normalise divides 0/0 — five NaN weights, and every blended term NaN with them. Depth 0
/// does not save it either: `blendPush` multiplies the blended sum by depth and `NaN · 0` is NaN. (It did
/// not save it under the old lerp form for the same reason, `knob + (NaN − knob)·0`.) This is the same rule
/// `character/util.hpp`'s `!(x < duty)` makes one directory over: a NaN lands in the dead zone, not in a
/// ramp. Infinities need no special case and get none — (+inf, −inf) clamps to (1, −1), the square's
/// top-right corner, which since the rotation is `kCfg2`'s own axis: that waypoint WHOLE, where under
/// the pre-rotation arrangement that corner fell between two waypoints and was half of each.
///
/// **WHAT THAT GUARANTEE STILL RESTS ON, AND IT IS ONE THING.** "The centre is neutral" and "depth 0 is
/// inert" are both `0 × corner == 0`, so both fail if a CORNER's push is itself non-finite. Nothing can
/// reach that today — design §3 makes the waypoints' Macro and Cfg plain parameters, not modulation
/// destinations — and a guard here could not fix it anyway; it would have to be built where a push is built.
[[nodiscard]] inline PerformWeights performWeightsOf(float x, float y) {
	// **THESE FOUR LINES ARE THE ONE THING HERE THAT `-ffinite-math-only` WOULD SILENTLY DELETE — AND IT
	// WOULD NOT FAIL TO A NaN, IT WOULD FAIL TO A STUCK RIM.** That flag lets the compiler fold
	// `std::isnan(x)` to false and then assume no operand is NaN, which re-decides every `std::max` below.
	// **MEASURED 2026-09-17, g++-14 -O3 -ffinite-math-only, ON THE PRE-ROTATION EXPRESSION** — which read
	// `x` and `y` where the four `std::max` lines below now read `u` and `v`: a NaN x returned weight
	// **1.0 on W** (not the centre, not a NaN); a NaN y returned 1.0 on N; a NaN on both returned 0.5 N
	// and 0.5 W. Every weight finite, the total 1, nothing downstream non-finite — a wrong sound with no
	// witness. **THE MECHANISM IS WHAT CARRIES OVER AND THE FOUR NUMBERS ARE NOT.** Which operand a
	// folded `std::max` keeps is the compiler's choice, so the outcome cannot be re-derived from this
	// source — and the rotation (2026-09-19) changed the source those four numbers were a choice about.
	// **They are kept as the shape of the failure, not as this function's answer, which has not been
	// re-measured.** What is unchanged, and is the whole reason the note exists: the flag deletes the
	// guard and fails to a STUCK RIM rather than to a NaN. Note the
	// contrast with the paragraph above, which is about DELETING the guard rather than compiling it away:
	// plain IEEE arithmetic without it divides 0/0 and hands back five NaN weights, which at least shows.
	// **NO TARGET IN THIS TREE USES THAT FLAG TODAY** — the repo's only float-model setting is
	// `FEATHERS_FP_EXACT` in the root `CMakeLists.txt`, `-ffp-contract=off`, which is orthogonal. This note is
	// here in the same spirit as that block naming `/fp:precise` out loud: a guarantee that rests on a flag
	// being ABSENT says so, rather than leaning on a default anyone could change.
	if (std::isnan(x)) {
		x = 0.0f;
	}
	if (std::isnan(y)) {
		y = 0.0f;
	}
	x = std::clamp(x, -1.0f, 1.0f);
	y = std::clamp(y, -1.0f, 1.0f);
	PerformWeights w;
	// **THE SANITISED AXES GO OUT WITH THE WEIGHTS — THIS IS THE ONLY PLACE THE PUCK IS SANITISED, AND
	// THAT IS A DECISION**: the device publishes `Perf X`/`Perf Y` as aux
	// outputs, and a second clamp at the publish would be a second road that could hand a patch an axis
	// the weights beside it were not built from. Recorded HERE, in the lines right after the de-NaN and
	// the clamp, so the two cannot be separated by an edit that only reads one of them.
	w.x = x;
	w.y = y;
	// **THE WAYPOINT AXES, 45° OFF THE SCREEN'S — and `u`, `v` are the SAME PAIR the four lines below
	// used to read straight off `x`, `y`.** See `kWaypointAxis` for why they carry a √2 in them.
	const float u = x - y;
	const float v = x + y;
	w.corner[kCfg1] = std::max(0.0f, -v);
	w.corner[kCfg3] = std::max(0.0f, v);
	w.corner[kCfg2] = std::max(0.0f, u);
	w.corner[kCfg4] = std::max(0.0f, -u);
	// **AND THE 1 BECOMES `kWaypointAxis` FOR THE SAME REASON THE AXES DID.** It is the only constant in
	// the five that is not already homogeneous in `u`, `v`, so it is the only one that has to be told
	// about the scale; leave it at 1 and the centre region shrinks to half its radius, which is a real
	// change of feel rather than a rounding.
	w.centre = std::max(0.0f, kWaypointAxis - (std::fabs(u) + std::fabs(v)));
	// **INSIDE THE CENTRE REGION THE TOTAL IS `kWaypointAxis`, NOT 1 — so the divide now runs almost
	// everywhere, and that is what makes the ends exact rather than what threatens them.** Every one of
	// the five is √2 times the honestly-rotated weight (`kWaypointAxis`), so the normalise is what puts
	// the scale back; the branch below is the saved divide it always was, and it now saves one almost
	// nowhere. **Both ends survive it because a quotient of equals is exact:** at the centre the total IS
	// the centre share (√2/√2 == 1.0f), and at a waypoint the total IS that one corner (2s/2s == 1.0f),
	// with the other four +0 divided by a positive number, which is +0.
	//
	// The centre region is the same diamond as before, rotated with everything else — in SCREEN space
	// `|u| + |v| == 2·max(|x|, |y|)`, so it is the square `max(|x|, |y|) < √2/2`. Its corners are the
	// four waypoints, at radius exactly 1, which is the property `perform_pad.h`'s `axesAt` leans on.
	// Outside it the centre share has clamped to 0 and the two adjacent waypoints are brought back to a
	// whole, so the rim is a pure two-waypoint crossfade.
	//
	// **`total` IS NEVER 0, AND THAT RESTS ENTIRELY ON THE DE-NaN ABOVE.** For a finite (x, y): inside the
	// centre region that share alone is positive, and outside it at least one of |u|, |v| exceeds it, so
	// at least one corner is positive. A NaN axis is the only input that makes all five zero, and it no
	// longer arrives.
	const float total = w.centre + w.corner[kCfg1] + w.corner[kCfg2] + w.corner[kCfg3] + w.corner[kCfg4];
	if (total != 1.0f) {
		w.centre /= total;
		for (float& c : w.corner) {
			c /= total;
		}
	}

	// **THE PUCK'S RADIUS IS LINEAR AND STAYS LINEAR — DECIDED 2026-09-19, and a curve was built here and
	// taken out again.** The concentrated useful region (the first 1-10%) was first read as being about
	// the RADIUS; the control meant was the strength knob, `Perf Depth`. So the corners keep
	// carrying both WHICH waypoint and HOW MUCH, half-way to a waypoint is half that waypoint's push,
	// and the curve lives one layer up in `performAmountOf`. Recorded because "the weights look
	// suspiciously linear" is a thing a later reader might try to fix.
	return w;
}

namespace detail {

/// Builds a push from one expression per term. EVERY FIELD IS NAMED HERE BY HAND, and `macro_push.h`'s
/// size assertion is what fails when a ninth is added to the struct without being added here.
template <typename F>
[[nodiscard]] MacroPush eachTerm(F&& f) {
	MacroPush p;
	p.dryThreshold = f(&MacroPush::dryThreshold);
	p.dReverse = f(&MacroPush::dReverse);
	p.dPitch = f(&MacroPush::dPitch);
	p.dDelay = f(&MacroPush::dDelay);
	p.gatePull = f(&MacroPush::gatePull);
	p.subdivMix = f(&MacroPush::subdivMix);
	p.zoneAPush = f(&MacroPush::zoneAPush);
	p.zoneBPush = f(&MacroPush::zoneBPush);
	return p;
}

} // namespace detail

/// **THE PUCK AT REST CARRIES A RATCHET: the centre's `subdivMix` — PROVISIONAL (2026-09-23).** Built this
/// way pending a later decision on whether it stays.
///
/// Why it exists: the reference gates the ratchet on its Macro and Macro Cfg knobs (`dsp/scatter.hpp`,
/// `subdivMix = macroNorm · subdivInfluence`, *"No base floor - macro gates ratchet entirely"*). Those
/// knobs were deleted on 2026-09-19 and the circle's centre became a zero push, so a player who never
/// moved the puck never heard a ratchet — MEASURED: 0 in 12,800 plans. Every mode should be
/// reachable without displacing the puck, depending on zone settings.
///
/// What 0.5 does: `subdivisions = 1 + ⌊(min(2·baseSub, 12) − 1) · 0.5⌋`, which is Zone A's hashed count
/// itself — 2, 3, 4, 6 for baseSub 2, 3, 4, 6 — and 6 for baseSub 8 and 12. Everything else about the
/// ratchet is the plan's and unchanged: Zone A below 2/128 draws none, and a slot that draws a repeat draws
/// no ratchet. It lives ONLY in the perform push, so `planFor`'s macro/config road stays the reference's
/// to the bit. No control carries it.
inline constexpr float kCentreSubdivMix = 0.5f;

/// The push the circle's centre stands for: zero in every term but `subdivMix`. See `kCentreSubdivMix`.
[[nodiscard]] inline MacroPush centrePush() {
	MacroPush p;
	p.subdivMix = kCentreSubdivMix;
	return p;
}

/// DESIGN §2.2 AS AMENDED 2026-09-19: `depth · Σ wᵢ·cornerᵢ`, per term. Each waypoint's push already carries
/// its own 0.01 gate (`macro_push.h`), so this is a mixture of THOSE waypoints' effects and nothing is gated
/// again; `dryThreshold` is blended as a number and thresholded once, later, by the plan (design §1.2).
///
/// **THE CENTRE IS A ZERO PUSH, NOT THE KNOBS', AND THAT IS WHY THIS TAKES FOUR CORNERS AND NOT FIVE.** The
/// centre macro went on 2026-09-19 and became zero. `Macro` and
/// `Macro Cfg` are deleted, so there is no fifth pair to weight — `w.centre` is the share the puck has not
/// spent and there is nothing behind it. **A PARAMETER EVERY CALLER WOULD PASS AS `MacroPush {}` IS AN
/// INVITATION TO PASS SOMETHING ELSE**, which is why the argument went rather than being defaulted; the
/// arithmetic is unchanged (`0 + (blended − 0)·depth` IS `blended·depth`, exactly, in IEEE).
///
/// **WHAT THE WEIGHTS' EXACT ZEROS BUY NOW.** At the centre every `wᵢ` is exactly +0 (`performWeightsOf`),
/// so each term is `0.0f` plus four signed-zero products — `+0.0f` in every field, which is `MacroPush {}`
/// to the BIT and not merely by `operator==`. At a rim point one weight is exactly 1.0f and the others +0,
/// so the term is `0.0f + 1·cᵢ` and then `± 0` four times: that waypoint's own push, to the bit, and the
/// `·depth` at depth 1 is exact too. **BOTH ENDS ARE NOW BIT-EXACT**, where the old lerp form had to choose
/// (it kept the centre exact and left the rim approximate, because the centre was the shipped sound).
/// The one thing the zeros CAN move is the sign of a zero term — a negative-zero corner times +0 is −0, and
/// the running sum starts at +0.0f, so the sum comes back +0. Nothing downstream can hear a signed zero;
/// the centre case measures it with `memcmp` rather than assuming it.
///
/// **`depth` MULTIPLIES THE WHOLE BLEND, WHICH IS WHAT `Perf Depth` NOW MEANS**: how far the circle pulls
/// away from NOTHING. Depth 0 is the centre from anywhere on the disk, exactly as it was — the sentence is
/// the same and the value it names changed from "the knobs' push" to "no push".
///
/// **EVERY GUARANTEE ABOVE IS `performWeightsOf`'S, NOT THIS FUNCTION'S.** This takes a `PerformWeights`,
/// which is a plain struct anyone can fill in: the exact centre, the clamp, the de-NaN and "the weights sum
/// to 1" are properties of how the weights were BUILT, and nothing here re-checks them. So the contract is
/// that the device obtains weights only through `performWeightsOf`, which is what makes it checkable rather
/// than a hope. **`performPushOf` below used to be the single call site; it is not any more**, because the
/// weights are its ARGUMENT now (see its own note) and the device builds them at each of the TWO places
/// that need one — `Sklitter::targetPush()`, from `target_`, and `Sklitter::setPerformFields`, from the
/// row. Both are `performWeightsOf` calls, which is the half of the contract that matters here; that the
/// two ROADS agree about the puck is a separate claim, and it is pinned by a case rather than by this line.
///
/// **IT SAID FOUR UNTIL `targetPush()` EXISTED, and the count is a maintenance hazard worth naming.** The
/// three plan sites each spelled the build out, `targetPush()` collapsed them into one — and this sentence
/// was left behind by the very commit that improved the number, two commits after it was last corrected
/// for the same reason. `performPushOf`'s own note below still holds through it and needs no arithmetic:
/// every plan site calls that one function and none builds a push of its own.
/// **`amount` IS THE MULTIPLIER, NOT THE KNOB, AND THE TWO STOPPED BEING THE SAME NUMBER ON 2026-09-19.**
/// The parameter was called `depth` and callers handed it `Perf Depth` directly. `Perf Depth`'s travel is
/// CURVED now (`performAmountOf`), so what arrives here is `depth^k` — and the rename is the whole guard:
/// a second caller passing the raw knob would silently play a linear depth beside a curved one, and no
/// type could tell them apart. `performPushOf` is the converter and the only site that needs to be.
///
/// **AMENDED 2026-09-23: THE CENTRE IS NO LONGER A ZERO PUSH IN ONE TERM — `subdivMix` IS
/// `kCentreSubdivMix`.** See the constant. Every other term's centre is still zero, and for those the
/// arithmetic below is the old `amount · Σ wᵢ·cornerᵢ` (the centre's `w.centre · 0` adds +0, and
/// `0 + amount·(x − 0)` is `amount·x`). What the paragraphs above say about "the zero push" is now true of
/// seven terms of eight.
///
/// **AND `amount` SCALES THE DISPLACEMENT FROM THE CENTRE, NOT THE CENTRE ITSELF** — the lead's intent,
/// chosen and recorded: `centre + amount · (Σ over all five − centre)`. So depth 0 is the CENTRE's push from
/// anywhere on the disk (`subdivMix` 0.5, the ratchet asked to hear at rest) rather than no push at
/// all; the other reading, `amount · Σ`, would make Perf Depth 0 switch the ratchets off, a control reaching
/// a sound its own name does not describe. The three anchors stay bit-exact: at the centre the sum IS the
/// centre (`1·c + four +0`), so `c + amount·0 = c`; at depth 0 it is `c + 0 = c`; and at depth 1 the blend
/// is returned as summed rather than through `c + 1·(Σ − c)`, which could miss a waypoint's own push by an
/// ulp and move a truncated ratchet count at the rim.
[[nodiscard]] inline MacroPush blendPush(const std::array<MacroPush, kCorners>& corners,
                                         const PerformWeights& w, float amount) {
	const MacroPush centre = centrePush();
	return detail::eachTerm([&](float MacroPush::*term) {
		// THE CENTRE'S SHARE FIRST, THEN THE FOUR — the share the puck has not spent now multiplies the
		// centre's push, which is zero in seven terms, so for those the sum is the old four terms.
		float blended = w.centre * (centre.*term);
		for (size_t i = 0; i < kCorners; ++i) {
			blended += w.corner[i] * (corners[i].*term);
		}
		if (amount == 1.0f) {
			return blended;
		}
		return (centre.*term) + amount * (blended - (centre.*term));
	});
}

/// **THE STRENGTH KNOB'S TRAVEL, CURVED — `Perf Depth` in, the multiplier `blendPush` wants out.**
///
/// Played, full macro strength was usually too strong and the useful region of a linear strength knob
/// sat in its first 1-10%, so the knob carries a curve. See `kDepthCurve` for where the exponent comes
/// from and how it is to be set.
///
/// **BOTH ENDS ARE EXACT BY CONSTRUCTION RATHER THAN BY ROUNDING.** `0^k` and `1^k` are exact in IEEE,
/// but the guard skips the call anyway so the guarantee does not depend on a library's edge cases —
/// depth 0 is still the zero push and depth 1 is still the blend untouched, and those are the two
/// properties every other case in this header leans on. (Asserted as well as argued: the depth cases in
/// `test_fx_sklitter_perform.cpp` read them as values.)
///
/// **IT CURVES THE RESOLVED VALUE, WHICH IS A DECISION ABOUT THE MOD ROAD.** `Perf Depth` is a modulation
/// destination, so "the knob's position" and "the amount" are two different things a route could drive.
/// This curves the value the device ACTUALLY HOLDS after modulation — `Settings::perfDepth`, decoded from
/// the resolved row — so a route moves exactly the control a hand moves and the printed value and the
/// routed effect cannot disagree. The alternative, a linear multiplier fed by the route, would make an
/// envelope's sweep feel unlike the same sweep made by hand, which is the kind of split this device has
/// already paid for four times in other forms.
///
/// **THE DECLARED RANGE AND THE NAME ARE UNTOUCHED.** The curve is in how the value is USED, not in what
/// the host sees, so automation stays a plain 0..1 and a DAW's lane means what it did.
[[nodiscard]] inline float performAmountOf(float depthKnob) {
	const float d = std::clamp(depthKnob, 0.0f, 1.0f);
	if (d <= 0.0f || d >= 1.0f) {
		return d;
	}
	return std::pow(d, kDepthCurve);
}

// **THE ARRAY IS `Corner`-WIDE, SAID TO THE COMPILER RATHER THAN IN A COMMENT.** `performPushOf` below
// and `sklitter_types.cpp`'s `stateOf`/`rowOf` index `cornerCfg` by `kCfg1`/`kCfg2`/`kCfg3`/`kCfg4`, and the two
// declarations that have to agree about "four" live in different headers — this one owns the enum,
// `sklitter_types.h` owns the storage. A fifth waypoint added to one of them is a silent out-of-bounds
// write otherwise. **It asserts HERE because this is now the header that has both**, which is what the
// include at the top bought; it was in `sklitter_types.cpp` while that was the only translation unit that
// did, and moving it puts the check in every TU that builds a push rather than in one.
//
// **THERE WERE TWO OF THESE AND THERE IS ONE, because `cornerMacro` is deleted (2026-09-19)** — a waypoint
// is a configuration alone and its push is built at macro = 1.0.
static_assert(kCorners == std::tuple_size_v<decltype(Settings::cornerCfg)>,
              "a waypoint configuration per Corner, or the Corner-indexed reads below run off the end");

/// **THE ONE BUILDER OF THE PUSH THE DEVICE PLAYS.** Every plan site in `sklitter.cpp` calls this and none
/// builds a push of its own: `slice_plan.h` records what three hand-built callers cost last time, a term
/// that went missing from one of them and from nothing else.
///
/// **THE WEIGHTS ARE AN ARGUMENT, NOT SOMETHING THIS BUILDS FROM `Settings` — BY DECISION, and the reason is a
/// second copy of the puck.** SKLITTER now publishes the puck, the four weights and a speed as aux mod
/// outputs (design §4), which the device decodes from its row on a SEPARATE road: the audio road owns
/// `target_` and the mod road owns `puckX_`/`puckY_`, because a second writer of `target_` is a second
/// writer however equal the values look. While this function read `s.perfX`/`s.perfY` itself, that made ONE
/// PURE FUNCTION over TWO PIECES OF STATE with nothing forcing them to agree — CLAUDE.md's pointer-identity
/// family with the shared thing being the FUNCTION and the unshared thing the STATE. Taking the weights in
/// narrows the seam so both roads pass through one `performWeightsOf` result when the device wants them to,
/// and a case (`test_fx_sklitter_perform.cpp`) pins that after one resolved row the published axes and
/// weights are the ones the blend used.
///
/// **THE CAVEAT IS RECORDED, NOT SOLVED:** the waypoints and the gamma still come from `Settings`, so if the
/// mod road ever gets its own copy of THOSE, this is two pieces of state again and the corners belong in the
/// signature too.
///
/// **THE KNOB WAS THE FIFTH WAYPOINT AND THERE IS NO FIFTH WAYPOINT ANY MORE (2026-09-19).** `Macro` and
/// `Macro Cfg` built one push exactly as each corner pair does, and `performWeightsOf` gave it the share the
/// puck had not spent. Both controls are deleted; the centre is a constant zero push, so **the puck's RADIUS
/// is the amount** — centred is no macro at all, half-way to a waypoint is half that waypoint's push, the rim
/// is that waypoint whole. It is a simplification of the same arithmetic and not a special case: nothing
/// below tests for the centre, and `w.centre` is still the hole in the sum that makes the radius mean that.
///
/// **AND THE RADIUS IS LINEAR IN IT — the curve asked for is on `Perf Depth`, not here** (2026-09-19,
/// `performAmountOf`). A curve was briefly built on the corner weights and taken out the same day: the
/// curve belongs on the knob. Half-way still means half.
///
/// **GAMMA IS SHARED BY ALL FOUR AND THAT IS DELIBERATE.** A waypoint is a (Macro, Macro Cfg) pair —
/// design §2 — so the field it is read against is the patch's, not the waypoint's. Giving each corner its
/// own gamma would make the rim four different fields rather than four places in one.
///
/// **RE-MEASURED 2026-09-19, AFTER THE KNOB LEG WENT: `macroPushOf` 33.2-39.0 ns, this 154.6-176.3 ns,
/// a ratio of 4.52-4.84x** (g++-14 -O3 -ffp-contract=off, load average 3.5, five runs of three
/// alternating A/B reps, `tests/bench_perform_push.cpp` with its A side now building one WAYPOINT's push
/// — macro 1.0, one corner's config — because `s.macro` and `s.macroConfig` no longer exist).
///
/// **AND AGAIN WITH THE DEPTH CURVE ON THE PATH, THE SAME DAY: 30.8-38.5 ns against 157.3-178.7 ns,
/// ratio 4.64-5.21x** at load 3.6. `performAmountOf`'s `std::pow` is the difference and it is small —
/// about 3 ns on the B side, a couple of percent — which is worth having measured rather than waved
/// through, because it is a transcendental on the audio thread and that is the kind of line a later
/// reader is right to be suspicious of. **THE BENCH HAD TO BE MADE TO MEASURE IT**: `performAmountOf`
/// skips the call at exactly 0 and 1, and the bench sat at the declared default of 1.0, so the first
/// run after the curve landed timed the guard. Its depth is 0.63 now, and the comment there says why.
///
/// **THE RATIO TRACKS THE CALL COUNT AND SITS CONSISTENTLY ABOVE IT, BY ABOUT THE SAME MARGIN AT BOTH
/// COUNTS**: 5.0-5.9x over five calls, 4.52-4.84x over four — each roughly 10-15% over the count, which
/// is the weight build inside the timed loop plus the blend's own thirty-two multiply-adds. So the model
/// ("the ratio is the call count") is right about SHAPE and low by a tenth about MAGNITUDE, at both
/// points; take the shape from it and the number from the bench, as CLAUDE.md says and as the 3.2-3.5x
/// paragraph below is the cautionary tale for.
///
/// **THE ABSOLUTE ns FELL BY MORE THAN THE CALL COUNT DID, AND THAT IS THE LOAD, NOT THE CODE.** This run
/// was at load 3.5 against the committed run's 6.5-9, and the load-4.05 row below already showed
/// `macroPushOf` at 40.7-42.7 ns on the OLD code. Everything in the table below is the FIVE-call snapshot
/// at that heavier load; it is kept because the SHAPE of each row is unchanged — which clock, which
/// direction, which guard — but **every ns in it is high on two counts at once**, so re-run the bench
/// rather than quoting them, exactly as the paragraph after the table has always said.
///
/// **WHAT IT COSTS, AND WHERE THAT IS AN INCREASE — MEASURED 2026-09-17** by `tests/bench_perform_push.cpp`,
/// which is the instrument for every number below: **re-run it rather than trusting these from another
/// day**, because this is a shared host and its own header says how to read it. g++-14 -O3
/// -ffp-contract=off, load 6.5-9, five runs of three alternating A/B reps: **`macroPushOf` 49-67 ns, this
/// 271-350 ns — a ratio of 5.0-5.9x, which is simply the call count.**
///
/// **THE RATIO IS WHAT TRAVELS; THE ns AND EVERYTHING DERIVED FROM THEM ARE A SNAPSHOT OF ONE HOST AT ONE
/// LOAD** — so every percentage-of-a-clock figure below is a snapshot too, because each is a ns over a
/// fixed denominator. THREE RUNS ON THREE DIFFERENT LOADS SAY IT FROM BOTH SIDES:
///   - load 4.05: `macroPushOf` 40.7-42.7 ns, this 230.7-239.2 ns, **ratio 5.48-5.67x** — a LIGHTER load
///     than the committed run, so both ns fall below its band while the ratio stays inside it.
///   - load 6.5-9: 49-67 ns and 271-350 ns, **ratio 5.0-5.9x** — the committed run, and the one every
///     number below is taken from, so that the rows are one internally consistent snapshot.
///   - load 7.41/10.98: 90.6-103.6 ns and 532.7-597.8 ns, **ratio 5.25-6.60x** (five of six reps inside
///     the committed band) — ABOVE it, both sides scaled together by about 1.7x.
/// So the absolute figures are load-dependent across a 2.5x spread (`macroPushOf` 40.7-103.6 ns, this
/// 230.7-597.8 ns) while the ratio held at 5.0-6.6x throughout. That is the reason and not just the rule:
/// the ratio is the call count, which contention cannot change, while an absolute ns is this core's
/// throughput at that minute. The conclusion survived it — the still-device row reads 0.0200-0.0224% of a
/// core at the heaviest load against 0.0102-0.0131% below, the same verdict — but a reading that lands near
/// a threshold must come from a re-run, not from here.
///
/// **AN EARLIER READING OF 3.2-3.5x WAS THE INSTRUMENT, NOT THE CODE**, and it is worth recording because
/// it came with an explanation that sounded like a finding: "under 5x because the five calls share one
/// loop". They do not. That bench streamed a 22 MB input array, so memory traffic was a large share of
/// both sides and the blend's extra ARITHMETIC was diluted; with the input set made cache-resident the
/// ratio went straight to the count. A plausible mechanism attached to a wrong number is the exact shape
/// CLAUDE.md warns about — take the shape from the model, the number from the instrument.
///
/// **EVERY RANGE HERE IS PER REP, NOT THE QUOTIENT OR MULTIPLE OF THE OUTER RANGES**, and the difference
/// shows: the headline ratio is 5.0-5.9x where dividing the endpoints of [271,350] by [49,67] would give
/// 4.0-7.1x, and each row below is likewise narrower than its endpoints imply (slice ≈153-199 ns against
/// 3x[49,67] = [147,201]). A paired statistic is the right one — the two sides of a rep meet the same
/// machine at the same instant — but it has to SAY it is paired, or a reader recomputing from the
/// endpoints gets a wider answer and concludes one of the two is wrong.
///
/// Each site below names its own denominator, because they are four different clocks:
///   - **SLICE — UP, per slice.** `advanceSlice` made 3 `macroPushOf` calls (offsets, plan, the one-ahead
///     peek) ≈153-199 ns; it makes one blend ≈271-350 ns, so about +120-150 ns. A sixteenth at 120 BPM is
///     125 ms, so this is ~0.0001% of it. (The earlier table called this row a wash; that was the same
///     diluted measurement.)
///   - **GRAIN — UP, per grain, and this is the tightest clock.** `grainPlanFor` made 2 calls ≈102-133 ns
///     and makes one blend ≈271-350 ns: **about +190 ns against the 21.3 ms of the shortest grain** (1024
///     frames, the literal in `refreshGrainGeometry`, which is the one that wins during play — `armGrain`'s
///     same-named constant is a DURATION, about 1114 frames at 48 kHz), 0.00048-0.00062% -> 0.00127-0.00164%
///     of it. **Grain runs TWO voices, each drawing its own plan**, so per second of audio the device pays
///     twice the BLEND'S OWN COST — 2 x 46.875 plans x 271-350 ns ≈ 25-33 µs — rather than twice the
///     increment.
///   - **PICTURE WALK — DOWN, per REBUILD.** The walk made 2 calls per step over up to 64 steps ≈6.5-8.5 µs;
///     it makes one blend ≈271-350 ns. A rebuild is the rare case by `RestKey`'s own design.
///   - **A STILL DEVICE — UP FROM ZERO, PER BLOCK **THAT DRAWS A PICTURE**, and this is the row the first
///     draft of this table left out.** `publishReadings` builds the push BEFORE comparing the key (it is
///     IN the key), so a device nobody is touching now pays ≈271-350 ns where it paid nothing. At
///     128-frame blocks (2.667 ms at 48 kHz) that is 0.0102-0.0131% of a core. It cannot move back inside
///     the guard: the comparison needs the value.
///
///     **"EVERY BLOCK" WAS TOO STRONG AND IS NARROWED HERE RATHER THAN RE-MEASURED** — the number is
///     unchanged, the POPULATION it applies to is smaller. `publishReadings` returns immediately when
///     `sink_.publish == nullptr`, so a device with no picture sink pays NOTHING; and the `targetPush()`
///     call sits inside `if (steps > 0 && slicesPerBar > 0)` (`sklitter.cpp`), so a block that draws no
///     steps does not reach it either. Every path that RECOMPUTES `slicesPerBar` there lands on at
///     least 1 (Burst takes the literal 1; the looper branches clamp with `std::max`), and
///     the playing path takes `numSlices_` as the engage branch settled it — so for a device with a
///     sink the two readings coincide in the ordinary case. **The point is not that the guards bite
///     often; it is that a per-block cost stated flatly is a claim a profiler can contradict, and
///     these two guards are what "per block" actually means here.**
[[nodiscard]] inline MacroPush performPushOf(const Settings& s, const PerformWeights& w, float depth) {
	// **FOUR BUILDS, NOT FIVE.** The knob leg was `macroPushOf(s.macro, splitCoordinate(s.macroConfig),
	// s.gamma)` and it is gone with the two controls it read; the centre is a zero push, which `blendPush`
	// expresses by leaving `w.centre` out of the sum rather than by multiplying a zero.
	//
	// **AND THE MACRO IS THE LITERAL 1.0f, WHICH IS THE OTHER HALF OF THE SAME DECISION (2026-09-19).** The
	// four `Macro N/E/S/W` controls are deleted; a waypoint is its CHARACTER at full strength and the
	// puck's radius is the amount, which `w.corner[i]` already is. The substitution is exact rather than
	// approximate because every term of a push is linear in the macro above the 0.01 gate, and at 1.0 the
	// gate cannot bite — `Settings::cornerCfg` carries the full argument and the one behavioural
	// consequence.
	constexpr float kWaypointMacro = 1.0f;
	std::array<MacroPush, kCorners> corners;
	for (size_t i = 0; i < kCorners; ++i) {
		corners[i] = macroPushOf(kWaypointMacro, splitCoordinate(s.cornerCfg[i]), s.gamma);
	}
	// **THE KNOB IS CONVERTED HERE AND NOWHERE ELSE.** `depth` is `Perf Depth` as the device holds it
	// after modulation; `blendPush` wants a MULTIPLIER, and since 2026-09-19 those are different numbers
	// (`performAmountOf`, `kDepthCurve`). One converter means the curve cannot be applied twice and
	// cannot be missed by a second plan site.
	return blendPush(corners, w, performAmountOf(depth));
}

//--------------------------------------------------------------------------------------------------------------------//
// ANCHOR — backlog HT                                                                                                 //
//--------------------------------------------------------------------------------------------------------------------//

/// THE STORED ANCHOR AS A PUSH: `Settings::anchor`'s first eight floats, in `MacroPush`'s declaration order.
[[nodiscard]] inline MacroPush anchorPushOf(const std::array<float, kAnchorFloats>& a) {
	return MacroPush { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7] };
}
/// Whether the device is anchored at all: the ninth float, 1 once a press has written the eight.
[[nodiscard]] inline bool anchored(const std::array<float, kAnchorFloats>& a) {
	return a[kAnchorFloats - 1] >= 0.5f;
}

/// **THE PUSH THE PLAN TAKES.** Unanchored, it IS the puck's push, returned untouched, so a device that was never
/// anchored computes exactly what it did before the anchor existed. Anchored, it is the anchored push plus the puck's
/// DEPARTURE FROM THE CENTRE (`puck − centrePush()`): at the centre that departure is `x − x`, +0 in every term, so a
/// centred puck plays the anchored push exactly. Stored as the whole push rather than as a delta because the centre
/// is not a zero push (it carries the ratchet, `kCentreSubdivMix`), and `(a + b − c) + c` is not `a + b` in floats.
[[nodiscard]] inline MacroPush totalPushOf(const Settings& s, const MacroPush& puck) {
	if (!anchored(s.anchor)) {
		return puck;
	}
	const MacroPush home = anchorPushOf(s.anchor);
	const MacroPush centre = centrePush();
	return detail::eachTerm([&](float MacroPush::*term) { return home.*term + (puck.*term - centre.*term); });
}

/// **WHAT AN ANCHOR PRESS WRITES, FROM THE HAND'S SETTINGS** — the BASE rows, not the modulated ones (HT's
/// recommendation: the only reading under which "keeping the sound" holds beyond the instant, because the routes keep
/// swinging around the new home as they swung around the old). The anchored push becomes the push the plan takes
/// now, and the puck goes to the centre, so the plan's push for the result IS the one for `base`, term for term.
struct AnchorPress {
	std::array<float, kAnchorFloats> anchor {};
	float perfX = 0.0f;
	float perfY = 0.0f;
};
[[nodiscard]] inline AnchorPress anchorPressOf(const Settings& base) {
	const MacroPush now =
	    totalPushOf(base, performPushOf(base, performWeightsOf(base.perfX, base.perfY), base.perfDepth));
	AnchorPress press;
	press.anchor = { now.dryThreshold, now.dReverse, now.dPitch, now.dDelay, now.gatePull,
		             now.subdivMix,    now.zoneAPush, now.zoneBPush, 1.0f };
	return press;
}

/// **WHETHER A PRESS WOULD CHANGE ANYTHING** — the honesty rule for the button: the puck off the centre. At the centre
/// the departure is zero whatever the depth, so a press there would write what is already playing.
[[nodiscard]] inline bool anchorWouldMove(const Settings& base) {
	return base.perfX != 0.0f || base.perfY != 0.0f;
}

} // namespace feathers::fx::sklitter
