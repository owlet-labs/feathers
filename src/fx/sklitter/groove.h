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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// GROOVE — WHEN A SLICE STARTS, SWUNG..                                            //
//--------------------------------------------------------------------------------------------------//
//
// Groove is another zone control, for grooving the slices, with zones such as swing8, groove8, 16 and 4,
// and Groove itself a phi-derived timing template.
//
// **NOBODY ELSE SWINGS FOR US, WHICH IS WHY THIS EXISTS.** CLAP's `clap_event_transport` carries no
// swing or groove, and Bitwig's groove moves note and clip timing only — it never reaches an effect's
// idea of where the bar is. So the device swings itself.
//
// **ONE MECHANISM FOR BOTH FAMILIES: A MONOTONIC TIME WARP OF BAR PHASE.** A fixed MUSICAL grid — G
// steps to the bar, a quarter, an eighth or a sixteenth — has each grid point j shifted LATE by
// `offset(j)` grid steps; the warp is piecewise-linear through the shifted points, the downbeat and the
// next downbeat pinned where they are. A slice boundary at straight phase `k/N` is cut at
// `warp(k/N)` instead, so any Rate works: a boundary between two grid points lands between their two
// shifted positions. **The grid is the bar's, not the slice's** — the zone names say "8" and "16" of
// the BAR, and a grid tied to Rate would change the feel every time the cut did.
//
// **ONLY WHEN A SLICE STARTS MOVES.** What a slice reads — its source offset in the buffer — stays on the
// straight grid, so a swung slice starts its own material late: the REX-style swing of a sliced loop. The
// ring's write head stays on straight song time too, because recording is in song time.
//
// **THE SHAPES ARE DECIDED HERE AND THE NUMBERS ARE THE EAR'S.** `kSwingStraightRatio` and
// `kSwingHeavyRatio` are the numbers to move, each one constant; `kGrooveMaxSteps` follows the second
// (backlog IO), and the template's two shares (`kGrooveOffBeatFloor`, `kGrooveOnBeatShare`) stand alone.
//
// **TWO CONTROLS SINCE BACKLOG IO — A MODE AND AN AMOUNT.** A mode plus a phase offset is the model,
// with 'free' variants (e.g. free_gv16) whose amount gives phi-varied, sensible offsets for each beat.
// So the feel is a
// Choice (`Groove Mode`) and the amount a separate 0..1 control (`Groove Amount`): an amount can never roll
// into another feel, because it is not the same number. What the amount MEANS is the mode's: a swing %, a
// share of the one template, or — Free — a phase through the φ sequence.

/// THE TEN FEELS, in the order, Free appended.
enum class GrooveMode : uint8_t {
	Straight = 0,
	Swing4,
	Swing8,
	Swing16,
	Groove4,
	Groove8,
	Groove16,
	Free4,
	Free8,
	Free16,
	kCount
};

inline constexpr int32_t kGrooveModeCount = static_cast<int32_t>(GrooveMode::kCount);

/// **SWING'S TRAVEL, AS THE SHARE OF A PAIR OF GRID STEPS THE FIRST ONE TAKES — THE MPC'S SCALE.** Biased,
/// 2026-09-22, towards the MPC's groove templates. The MPC's swing % is exactly this
/// quantity: 50% straight, 66% the triplet, 75% its maximum. Linear, and the readout prints the % the MPC
/// would.
///
/// **THE MPC'S 75% CEILING, KEPT BY DECISION (backlog IO).** A widening to 5:1 (83⅓%) was built and tried
/// on 2026-09-23 and set aside: 75% is fine. It is this one
/// constant, and `kGrooveMaxSteps` follows it.
///
/// **LINEAR, NOT BIASED, AND THAT WAS CHECKED RATHER THAN ASSUMED.** The MPC's classic settings — 50, 54,
/// 58, 62, 66, 71, 75% — fall at amounts 0, .16, .32, .48, .64, .84, 1.0: evenly across the travel with no
/// cluster a bias would help reach. The readout prints a tenth of a percent (the digit a fine drag grabs),
/// so each of them is a displayed value a drag lands on exactly; one percent is 4% of the amount's travel,
/// and the TRIPLET (2/3) sits at two thirds of it. No detents: the control is continuous, as Rate is.
inline constexpr double kSwingStraightRatio = 0.5;
inline constexpr double kSwingHeavyRatio = 0.75;

/// **HOW FAR LATE A GROOVE STEP MAY LAND AT FULL AMOUNT, IN GRID STEPS — THE CEILING HEAVY SWING REACHES,
/// `2·ratio − 1`, AND DERIVED FROM IT** so the two families span the same distance and differ only in which
/// steps move: half a step. **ANYTHING BELOW ONE KEEPS THE WARP STRICTLY MONOTONIC**: neighbouring points
/// are one step apart and each moves by at most this, so no step can reach its neighbour; at ½ the
/// narrowest a warped step can become is half its straight width.
inline constexpr double kGrooveMaxSteps = 2.0 * kSwingHeavyRatio - 1.0;
static_assert(kGrooveMaxSteps < 1.0, "a grid step that can move a whole step can cross its neighbour");

/// **THE TEMPLATE'S MPC BIAS — THE OFF-BEATS CARRY THE LATENESS (bias to mpc groove templates).**
///
/// An MPC groove is a swing: the off-beat of each pair lands late and the on-beat stays put. So the phi
/// template is shaped to that rather than spread evenly over every step:
///
/// - **An OFF-BEAT** (odd step) is late by `kGrooveMaxSteps · (floor + (1 − floor)·frac(jφ))` at full
///   amount — never less than `kGrooveOffBeatFloor` of the envelope, so every off-beat swings, and phi
///   decides by how much above that. In the MPC's own terms an off-beat at `d` steps late sits at
///   `50 + 50·d` % of its pair: **62.5% to 75%** at full amount, never past the MPC's 75% ceiling.
/// - **An ON-BEAT** (even step, not the downbeat) moves by at most `kGrooveOnBeatShare` of that envelope —
///   a push the MPC's swing does not have, kept small so the pair's first note still reads as the beat.
///
/// Both are a draft's numbers, for the ear.
inline constexpr double kGrooveOffBeatFloor = 0.5;
inline constexpr double kGrooveOnBeatShare = 0.25;
static_assert(2.0 * kSwingHeavyRatio - 1.0 < 1.0, "the same bound, for swing's late step");

/// THE GOLDEN RATIO, for the template. `frac(j·φ)` is the most evenly spread sequence there is (the
/// three-distance theorem): every prefix of it leaves no two offsets clustered, so a bar's steps get
/// delays that differ from each other as much as a sequence can — which is what makes a template read as
/// a FEEL rather than as noise or as a ramp.
inline constexpr double kGoldenRatio = 1.6180339887498948482;

/// **FREE — THE AMOUNT IS A PHASE THROUGH THE φ SEQUENCE, NOT A DEPTH (backlog IO).**
///
/// Each step j's lateness is a TRIANGLE WITH A DUTY CYCLE, walked by the amount at the step's OWN rate —
/// not `frac(jφ)`, a sawtooth that JUMPS from 1 to 0 — scaled by the step's class:
///
///     p_j(a) = frac(|a|·r_j·d_j)                     r_j = φ^(1/j),  d_j = kFreeDutyBase^level(j)
///     w_j    = 0 while p < 1 − d_j, then up to 1 and back to 0 over the last d_j of the cycle
///     late_j = kFreeMaxSteps · m_j · w_j             m_j = 1 for an off-beat, kGrooveOnBeatShare otherwise
///
/// **PHASE 0 IS EXACTLY STRAIGHT** (the defect: Free driven up to about 140 and reset to zero still
/// left a groove; by design phase 0 is straight). Every wave starts
/// its cycle at 0, at the head of its zero segment, and nothing is added to it: there is no per-step
/// starting phase and no off-beat floor. **THE FIRST DRAFT HAD BOTH AND WAS NEVER STRAIGHT** — each wave
/// started at `j·φ` into its cycle, and every off-beat was late by at least a quarter of a step whatever
/// the wave said — so phase 0 was already a full groove, and resetting the knob could not remove it.
/// Variety along the walk now comes from the incommensurate RATES alone, which is all non-recurrence needs.
///
/// **NEAR 0 A SMALL PHASE IS A SMALL GROOVE.** An off-beat (duty 1) has no zero segment, so its triangle
/// rises from 0 at once and linearly: late = ½ · 2·r_j·a for a < 1/(2·r_j). Every stronger step sits in
/// its zero segment until `|a|·r_j·d_j` reaches `1 − d_j`, so near 0 only the off-beats move, and by at
/// most `kFreeMaxSlope · |a|`.
///
/// **NEGATIVE PHASE MIRRORS: `s_j(−a) = s_j(a)`.** The walk is read through `|a|`, so 0 is straight from
/// both sides and has the same shape either way — a flat stretch for every stronger step, a linear rise
/// for the off-beats. Continuing each wave backwards instead would enter a stronger step's triangle from
/// its falling edge, so below 0 the strong steps would move first — the hierarchy upside down. The mirror
/// costs nothing the walk needed: it is as unbounded below 0 as above, and never recurs on either side.
///
/// **THE DUTY IS METRICAL WEIGHT** — duty cycles for the beat-warping triangles, set by a beat hierarchy
/// of subdivision in the selected mode, chosen on a principle. `level(j)` is the number of trailing zero bits of j on the mode's grid: the weakest
/// subdivision (the odd steps) is level 0, the next up level 1, and so on to the half bar. The principle is
/// the metrical grid's (Lerdahl & Jackendoff): expressive timing deviation — swing above all — lands on the
/// WEAK positions and the strong ones stay tight, which is why the MPC swings only the weakest subdivision.
/// So a step's wave is nudged `kFreeDutyBase` as often per level it stands up the hierarchy: the weakest
/// is a full triangle and moves all the time, the eighths of a sixteenth grid rise half the time, the
/// quarters a quarter, the half bar an eighth; between rises the step sits exactly on its grid. The base is
/// the one arbitrary number, for the ear.
///
/// **THE ENVELOPE KEEPS ITS TWO CLASSES, WITHOUT GROOVE'S FLOOR:** an off-beat (level 0) may be late by the
/// whole envelope, half a step — the MPC's 75% — and every stronger step by `kGrooveOnBeatShare` of it, an
/// eighth. **THE OFF-BEATS STILL CARRY MOST OF THE LATENESS, AND NOW BY THE DUTIES AND THE MAXIMA ALONE:**
/// an off-beat reaches four times as far as any stronger step and moves all the time, while a stronger step
/// sits exactly on its grid for `1 − d` of the walk — so over a long walk an off-beat is late by ¼ step on
/// average and a stronger step by at most `⅛ · ½ · d ≤ 1/32`. Groove's floor (`kGrooveOffBeatFloor`) is
/// the one thing Free does not share with it: a floor is lateness at every phase, 0 included. Scaling the
/// envelope by level too would count the hierarchy twice.
///
/// **THE RATE IS SCALED BY THE DUTY**, so a rise lasts the same stretch of amount at every level and only
/// the gaps between rises lengthen: the triangle's slope, 2/d per unit phase, times the phase rate r·d, is
/// 2r whatever the duty. A narrow triangle at the full rate would be 1/d times as steep.
///
/// **NON-PERIODIC — in free since we're driving nonperiodic phi groove mutations, it should not
/// wrap.** One shared rate made the whole pattern recur at every whole amount. Each step walks at its own
/// rate, and every two are INCOMMENSURATE: `r_j / r_k = φ^(1/j − 1/k)`, and a nonzero rational power of φ
/// is irrational (were `φ^(p/q)` rational, so would be `φ^p = F_p·φ + F_(p−1)`, which it is not for p ≠ 0).
/// So the pattern vector never returns for any amount > 0: the walk is quasi-periodic. **`1 + frac(j·φ²)`
/// WAS THE FIRST DRAFT AND IS NOT THIS** — it is `j·φ + m_j` for an integer `m_j`, and steps 2 and 3 came out
/// at `2φ − 2` and `3φ − 3`, a ratio of exactly 2/3; the case that checks the pairs caught it. The rates
/// run from φ (step 1) down towards 1 (step 15), all in [1, 2); the duty multiplies each by a power of two,
/// which keeps every ratio irrational (`φ^q·2^n` is rational only if `φ^q` is). Each position is a different
/// pattern and the NEIGHBOURING position the neighbouring pattern, so a route or an LFO sweeps it without a
/// seam. **THE BOUND, DERIVED:** `|d w/da| = 2·r_j ≤ 2φ`, so an off-beat moves at most
/// `kFreeMaxSteps·2φ` = φ ≈ 1.618 grid steps per whole unit of amount (`kFreeMaxSlope`), a stronger step at
/// most a quarter of that — 0.0016 steps for a thousandth. Twice the floored draft's φ/2, because the same
/// ceiling is now reached from 0 rather than from the floor; `|a|` has a kink at 0 but no jump, and the
/// bound holds across it.
///
/// **INSIDE THE MPC'S OWN ENVELOPE — the same ceiling Swing and Groove reach at full (`kGrooveMaxSteps`).**
/// Free has no depth to turn down, so its envelope is its whole range: every off-beat at 50–75% of its pair,
/// every on-beat within an eighth of a step. Free's amount is WHICH pattern, and every one of them is one
/// the MPC could have played — phase 0 its straight one.
inline constexpr double kFreeMaxSteps = kGrooveMaxSteps;
static_assert(kFreeMaxSteps < 1.0, "a grid step that can move a whole step can cross its neighbour");

/// **THE MOST AN OFF-BEAT'S OFFSET MOVES PER UNIT OF AMOUNT, IN GRID STEPS** — the bound derived above; an
/// on-beat's is a quarter. Stated as a constant so the case that asserts it reads the same number the
/// derivation gives rather than restating it.
inline constexpr double kFreeMaxSlope = kFreeMaxSteps * 2.0 * kGoldenRatio;

/// **HOW MUCH LESS OFTEN EACH LEVEL UP THE METRICAL HIERARCHY IS NUDGED** — `d = base^level`. ½: the weakest
/// subdivision always, the next half the time, then a quarter, then an eighth. The one number in the rule
/// that is a choice rather than a principle — the ear sets it.
inline constexpr double kFreeDutyBase = 0.5;
static_assert(kFreeDutyBase > 0.0 && kFreeDutyBase <= 1.0, "a duty is a share of the cycle");

/// A FEEL AND HOW MUCH OF IT — or, for Free, where along the walk.
struct GrooveFeel {
	GrooveMode mode = GrooveMode::Straight;
	double amount = 0.0; ///< 0..1: a depth for Swing and Groove, a phase for Free
	friend bool operator==(const GrooveFeel&, const GrooveFeel&) = default;
};

/// **THE MODE CONTROL, DECODED** — the nearest step, clamped, a NaN to Straight. The stored value of a
/// stepped control is not rounded where it is stored (`device::coerceControl`), so every reader rounds.
[[nodiscard]] inline GrooveMode grooveModeOf(double value) {
	if (!std::isfinite(value)) {
		return GrooveMode::Straight;
	}
	const auto v = static_cast<int32_t>(std::lround(std::clamp(value, 0.0, static_cast<double>(kGrooveModeCount - 1))));
	return static_cast<GrooveMode>(v);
}

/// HOW MANY GRID STEPS A BAR HOLDS FOR THIS FEEL — 4, 8 or 16 — or 0 for Straight.
[[nodiscard]] inline int32_t grooveGridOf(GrooveMode mode) {
	switch (mode) {
	case GrooveMode::Swing4:
	case GrooveMode::Groove4:
	case GrooveMode::Free4:
		return 4;
	case GrooveMode::Swing8:
	case GrooveMode::Groove8:
	case GrooveMode::Free8:
		return 8;
	case GrooveMode::Swing16:
	case GrooveMode::Groove16:
	case GrooveMode::Free16:
		return 16;
	default:
		return 0;
	}
}

[[nodiscard]] inline bool isSwing(GrooveMode mode) {
	return mode == GrooveMode::Swing4 || mode == GrooveMode::Swing8 || mode == GrooveMode::Swing16;
}

[[nodiscard]] inline bool isFree(GrooveMode mode) {
	return mode == GrooveMode::Free4 || mode == GrooveMode::Free8 || mode == GrooveMode::Free16;
}

/// **WHETHER THE FEEL MOVES ANYTHING.** Straight, or a Swing or Groove at zero amount, is the identity —
/// and the device takes its ORIGINAL arithmetic for it rather than the warp's, which is what makes the
/// zero-change guarantee bit-exact instead of equal-to-a-rounding. **FREE AT PHASE 0 IS THE IDENTITY TOO**
/// (backlog IO addendum): every wave starts at 0, so its offsets are all zero there, and it takes the same
/// original arithmetic rather than a warp that happens to move nothing. Any other phase, either side of 0,
/// is a pattern; a phase that is not a number reads as 0, as `grooveStepOffset` reads it.
[[nodiscard]] inline bool grooveActive(const GrooveFeel& feel) {
	if (feel.mode == GrooveMode::Straight || grooveGridOf(feel.mode) <= 0) {
		return false;
	}
	if (isFree(feel.mode)) {
		return std::isfinite(feel.amount) && feel.amount != 0.0;
	}
	return feel.amount > 0.0;
}

/// **STEP `step`'S OWN RATE ALONG FREE'S WALK** — `φ^(1/j)`, in [1, 2) and pairwise incommensurate. See
/// `kFreeMaxSteps`.
[[nodiscard]] inline double freeRate(int32_t step) {
	return step > 0 ? std::pow(kGoldenRatio, 1.0 / static_cast<double>(step)) : 1.0;
}

/// **STEP `step`'S PLACE IN THE METRICAL HIERARCHY** — its trailing zero bits: odd steps 0, the next
/// subdivision up 1, and so on. The downbeat (0) never moves and is not asked.
[[nodiscard]] inline int32_t metricalLevel(int32_t step) {
	int32_t level = 0;
	for (auto s = static_cast<uint32_t>(step); s != 0 && (s & 1u) == 0; s >>= 1) {
		++level;
	}
	return level;
}

/// **STEP `step`'S DUTY — `kFreeDutyBase^level`.** See `kFreeMaxSteps`.
[[nodiscard]] inline double freeDuty(int32_t step) {
	return std::pow(kFreeDutyBase, static_cast<double>(metricalLevel(step)));
}

/// **FREE'S WAVE FOR STEP `step` AT `amount`** — the duty-cycled triangle, in 0..1, starting its cycle at
/// amount 0 on its zero segment and mirrored below 0. See `kFreeMaxSteps`.
[[nodiscard]] inline double freeTerm(int32_t step, double amount) {
	const double duty = freeDuty(step);
	const double walked = std::fabs(amount) * freeRate(step) * duty;
	const double p = walked - std::floor(walked);
	if (p < 1.0 - duty) {
		return 0.0; // BETWEEN RISES THE STEP SITS ON ITS GRID
	}
	const double q = (p - (1.0 - duty)) / duty;
	return 1.0 - std::fabs(2.0 * q - 1.0);
}

/// SWING'S RATIO AT THIS AMOUNT — the share of a pair the first step takes.
[[nodiscard]] inline double swingRatioOf(double amount) {
	return kSwingStraightRatio + (kSwingHeavyRatio - kSwingStraightRatio) * std::clamp(amount, 0.0, 1.0);
}

/// **GROOVE'S TEMPLATE, BEFORE THE AMOUNT: `frac(j·φ)`, AND THE DOWNBEAT PINNED AT ZERO.** Indexed within
/// the bar — step `G` is the next downbeat and is pinned too, which is what keeps the bar the bar.
[[nodiscard]] inline double grooveTemplate(int32_t step, int32_t grid) {
	if (grid <= 0 || step <= 0 || step >= grid) {
		return 0.0;
	}
	const double x = static_cast<double>(step) * kGoldenRatio;
	return x - std::floor(x);
}

/// **HOW LATE GRID POINT `step` LANDS, IN GRID STEPS.** Swing: every odd step by `2·ratio − 1`. Groove:
/// the MPC-biased phi template (see `kGrooveOffBeatFloor`) times the amount times `kGrooveMaxSteps`. Free:
/// `freeTerm` at the amount's phase, times the step's class share (1 or `kGrooveOnBeatShare`, no floor),
/// times `kFreeMaxSteps`. The downbeat never moves.
[[nodiscard]] inline double grooveStepOffset(const GrooveFeel& feel, int32_t step) {
	const int32_t grid = grooveGridOf(feel.mode);
	if (!grooveActive(feel) || step <= 0 || step >= grid) {
		return 0.0;
	}
	if (isSwing(feel.mode)) {
		return (step % 2 == 1) ? 2.0 * swingRatioOf(feel.amount) - 1.0 : 0.0;
	}
	if (isFree(feel.mode)) {
		// FREE'S AMOUNT IS NOT CLAMPED HERE — it is a position along a walk that does not wrap, and the
		// control's declared range is what bounds it. NO FLOOR: a wave at 0 is a step on its grid.
		const double t = freeTerm(step, std::isfinite(feel.amount) ? feel.amount : 0.0);
		return kFreeMaxSteps * ((step % 2 == 1) ? t : kGrooveOnBeatShare * t);
	}
	const double t = grooveTemplate(step, grid);
	const double shape = (step % 2 == 1) ? kGrooveOffBeatFloor + (1.0 - kGrooveOffBeatFloor) * t
	                                     : kGrooveOnBeatShare * t;
	return std::clamp(feel.amount, 0.0, 1.0) * kGrooveMaxSteps * shape;
}

/// **THE WARP: STRAIGHT BAR PHASE IN, SWUNG BAR PHASE OUT.** Piecewise-linear through the shifted grid
/// points; 0 → 0 and 1 → 1; strictly increasing (see `kGrooveMaxSteps`). The identity for an inactive feel.
[[nodiscard]] inline double grooveWarp(const GrooveFeel& feel, double phase) {
	const int32_t grid = grooveGridOf(feel.mode);
	if (!grooveActive(feel)) {
		return phase;
	}
	const double x = std::clamp(phase, 0.0, 1.0) * static_cast<double>(grid);
	const auto j = std::min(static_cast<int32_t>(std::floor(x)), grid - 1);
	const double f = x - static_cast<double>(j);
	const double lo = static_cast<double>(j) + grooveStepOffset(feel, j);
	const double hi = static_cast<double>(j + 1) + grooveStepOffset(feel, j + 1);
	return (lo + (hi - lo) * f) / static_cast<double>(grid);
}

/// **WHERE SLICE BOUNDARY `k` OF A BAR CUT INTO `numSlices` FALLS, IN FRAMES OF THAT BAR.**
///
/// **STATED AS THE STRAIGHT GRID PLUS THE WARP'S DISPLACEMENT, NOT AS `warp(k/N)·bar`.** The straight grid
/// the device has always cut is `k · (bar / N)` with the integer division's remainder handed to the LAST
/// slice (`scheduleFor`), which `llround(k/N · bar)` is not — so an inactive feel returns that grid exactly
/// and the zero-change guarantee is a property of the arithmetic rather than of a rounding coincidence.
/// `k >= numSlices` is the bar's end, which is where the last slice's remainder goes.
[[nodiscard]] inline size_t grooveBoundary(const GrooveFeel& feel, int32_t k, int32_t numSlices,
                                           size_t barFrames) {
	if (numSlices <= 0 || barFrames == 0) {
		return 0;
	}
	if (k >= numSlices) {
		return barFrames;
	}
	const size_t base = barFrames / static_cast<size_t>(numSlices);
	const size_t straight = static_cast<size_t>(std::max(k, 0)) * base;
	if (!grooveActive(feel) || k <= 0) {
		return straight;
	}
	const double phase = static_cast<double>(k) / static_cast<double>(numSlices);
	const double shift = (grooveWarp(feel, phase) - phase) * static_cast<double>(barFrames);
	const auto moved = static_cast<long long>(straight) + std::llround(shift);
	return static_cast<size_t>(std::clamp<long long>(moved, 0, static_cast<long long>(barFrames)));
}

/// **THE INVERSE, FOR THE TRANSPORT LOCK: WHICH SLICE HOLDS FRAME `framesIn` OF THE BAR, AND HOW FAR IN.**
/// The largest `k` whose boundary is at or before the frame, clamped to the last slice — the same clamp
/// `Sklitter::lockedCursor` applies to the straight grid, and for the same reason: the last slice holds
/// the bar's remainder. A binary search, because `grooveBoundary` is monotonic and N is at most 64.
[[nodiscard]] inline int32_t grooveSliceAt(const GrooveFeel& feel, size_t framesIn, int32_t numSlices,
                                           size_t barFrames, size_t* within) {
	if (numSlices <= 0 || barFrames == 0) {
		*within = 0;
		return 0;
	}
	int32_t lo = 0;
	int32_t hi = numSlices - 1;
	while (lo < hi) {
		const int32_t mid = lo + (hi - lo + 1) / 2;
		if (grooveBoundary(feel, mid, numSlices, barFrames) <= framesIn) {
			lo = mid;
		}
		else {
			hi = mid - 1;
		}
	}
	const size_t start = grooveBoundary(feel, lo, numSlices, barFrames);
	*within = framesIn >= start ? framesIn - start : 0;
	return lo;
}

} // namespace feathers::fx::sklitter
