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

#include "character/phi_triangle.hpp"
#include "character/util.hpp"

#include <cmath>

namespace feathers::fx::sklitter {

/// **A COORDINATE'S LAP IS ITS PHASE, AND ITS FRACTION IS ITS POSITION.** Our zone controls are
/// unbounded (none of these should be bounded) while the reference's are a
/// bounded 0..1 knob plus a separate phase offset. `owltt.cpp` already joins the two the same way --
/// `std::floor(character)` is the phase and the fraction drives the zones -- and that is exactly what
/// "each lap is the same eight somewhere else in the field" means.
struct Coordinate {
	float position; ///< 0..1 across the eight zones
	float lap;      ///< how many whole turns, which becomes a phase offset
};

[[nodiscard]] inline Coordinate splitCoordinate(float value) {
	const float lap = std::floor(value);
	return { value - lap, lap };
}

/// **THE MACRO'S WHOLE EFFECT — EIGHT SCALARS. FIVE ARE `macro · f(cfg) · K`; THREE CARRY THE
/// REFERENCE'S `macro > 0.01f` DEAD-ZONE GATE.**
///
/// The five are linear in the macro and nonlinear only in the config, which is what makes blending four
/// waypoints a weighted sum rather than DSP work (design §1). The three probability pushes — `dReverse`,
/// `dPitch`, `dDelay` — are that too above the gate, and exactly zero below it, so they STEP where the
/// macro crosses 0.01. TWO OF THE EIGHT LIVE IN `offsetsFor`, not `planFor`, and they are the large
/// ones. **THE 71% IS A COMPARISON BETWEEN OUR `planFor` AND THE REFERENCE REFERENCE, and which side the
/// push was removed from is the whole of it** — `offsetsFor`'s note has the full statement, measured over
/// 153,664 plans on a zone x macro x bar x slice grid: **with the phase push removed from the REFERENCE's
/// side the two are identical in every field; with it IN, 71% of plans differ.** So the push is what
/// makes the two disagree — it is the reference's one-knob move across the whole plan, and ours only
/// nudged three probabilities until Task 1. Spelled out in that direction because the short form ("71%
/// differ without them") reads both ways and WAS read backwards once, in the design doc's §1.
///
/// Everything else that guards the macro — the `> 0.5` dry vote, the delay withheld on a Repeat, the
/// gate pull's zone-B guard, the clamps — stays with the plan, so a blend of these is still thresholded
/// and clamped exactly once. **The 0.01 gate is the one guard that moved INTO the push**, and that is a
/// decision (2026-09-17), not a convenience: THE GATE STAYS PER PUSH, which once pushes are blended
/// means per waypoint, rather than applied once after the blend. The rule is that a point between
/// two waypoints is a mixture of THOSE waypoints' effects; a gate applied after the blend would make one
/// waypoint's contribution depend on the others'. The cost is a step of at most about 0.01 · 0.5 = 0.005
/// in reverse probability (0.003 for pitch, 0.004 for delay) where a waypoint's own macro crosses 0.01 —
/// judged inaudible, NOT measured.
struct MacroPush {
	float dryThreshold = 0.0f; ///< > 0.5 votes dry — thresholded ONCE, after any blend
	float dReverse = 0.0f;
	float dPitch = 0.0f;
	float dDelay = 0.0f; ///< before `macroPushesDelay`, which is the plan's to apply
	float gatePull = 0.0f;
	float subdivMix = 0.0f;
	float zoneAPush = 0.0f;
	float zoneBPush = 0.0f;
	friend bool operator==(const MacroPush&, const MacroPush&) = default;
};

// perform.h blends the push term by term through `detail::eachTerm`, which names all eight — a ninth field
// would compile, blend as its default, and silently drop out of every point on the circle but the centre.
static_assert(sizeof(MacroPush) == 8 * sizeof(float), "a new term must be added to detail::eachTerm in perform.h");

/// Each term is its site's expression in the site's own product order and type — bit identity with the
/// inline code is what lets `planFor` take the push in its place.
[[nodiscard]] inline MacroPush macroPushOf(float macro, const Coordinate& cfg, float gamma) {
	namespace phi = owlet::dsp::phi;
	using owlet::dsp::triangleFloat;
	using owlet::dsp::triangleSimpleUnipolar;
	// planFor's sum, same order, same type — see the long note there on why it is double and unwrapped
	const double norm =
	    static_cast<double>(cfg.position) + (static_cast<double>(cfg.lap) * 0.1 + static_cast<double>(gamma));
	MacroPush p;
	p.dryThreshold = macro * triangleSimpleUnipolar(phi::wrapPhase(norm * static_cast<double>(phi::kPhi)), 0.5f);
	if (macro > 0.01f) {
		p.dReverse = macro * triangleFloat(phi::wrapPhase(norm * static_cast<double>(phi::kPhi125)), 0.6f) * 0.5f;
		p.dPitch = macro * triangleFloat(phi::wrapPhase(norm * static_cast<double>(phi::kPhi200)), 0.6f) * 0.3f;
		p.dDelay = macro * triangleFloat(phi::wrapPhase(norm * static_cast<double>(phi::kPhi075)), 0.6f) * 0.4f;
	}
	p.gatePull =
	    macro * triangleSimpleUnipolar(phi::wrapPhase(norm * static_cast<double>(phi::kPhi100)), 0.6f) * 0.75f;
	p.subdivMix = macro * triangleSimpleUnipolar(phi::wrapPhase(norm * static_cast<double>(phi::kPhi225)), 0.5f);
	// offsetsFor reads the POSITION only: no lap, no gamma, no wrap — the reference's own shape
	constexpr float kMacroPhaseMax = 0.3f;
	p.zoneAPush = macro * triangleSimpleUnipolar(cfg.position * phi::kPhi050, 0.5f) * kMacroPhaseMax;
	p.zoneBPush = macro * triangleSimpleUnipolar(cfg.position * phi::kPhi075, 0.5f) * kMacroPhaseMax;
	return p;
}

} // namespace feathers::fx::sklitter
