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

#include "dsp/slice_basis.h"
#include "character/phi_triangle.hpp"
#include "fx/automod/automod_types.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::automod {

/// **AUTOMOD'S PHASE SPACE: ONE COORDINATE PER PHI TRIANGLE.**
///
/// BY DESIGN, Automod has the N required to drive all of its phi triangles, with the
/// plane sliced through that space by a basis, the puck's chart anchored at the current position and
/// re-anchoring on release, and rotate turning the plane THROUGH the space.
///
/// **AND THE MECHANISM IS NOT NEW — IT IS `SliceBasis<N>`**, already written for Explore in
/// `dsp/slice_basis.h`. Included directly rather than wrapped, by design; the alternative was a
/// device-layer facade over a type nobody else spells, which is the duplication the consolidation
/// survey exists to complain about. `src/fx/shaper/shaper.cpp` already includes dsp, so this opens no
/// edge a device has not opened.
///
/// **WHAT THE DEVICE FORM DROPS: the residue.** `SliceResidue` exists because Explore's travel
/// PERSISTS — a drifted sound is out in the plane and turning the plane would move it. A device that
/// re-anchors on release has zero travel at rest, so `travelTerm` is exactly +/-0.0 and `phaseFor`
/// returns home bit for bit whatever the orientation. The guarantee the residue restores is already
/// in the arithmetic here, so the residue stays default-constructed and unused.
namespace phase_space {

/// **THIRTY-NINE, AND THE COUNT IS MEASURED FROM THE DERIVE RATHER THAN ASSUMED.**
///
///     Type    3 type scalars + 3 filter mix                        =  6
///     Flavour 9 flavour scalars + 3 LFO response + 3 phase offsets = 15
///     Mod     8 mod scalars + 1 LFO start phase + 9 LFO shape bank = 18
///
/// **THE NINE UNDER MOD ARE INSIDE `lfoShapeAt`** (`character/phi_lfo_shape.hpp:184`), which
/// `automod_derived.h` does not show — counting the derive alone gives 30 and silently misses a fifth
/// of the space. That is the same shape as every other miscount this week: a check, or a count,
/// cannot see what it does not enumerate.
inline constexpr size_t kTypeTriangles = 6;
inline constexpr size_t kFlavourTriangles = 15;
inline constexpr size_t kModTriangles = 18;
inline constexpr size_t kTriangles = kTypeTriangles + kFlavourTriangles + kModTriangles;
static_assert(kTriangles == 39, "the phase space is one coordinate per phi triangle automod drives");

/// **WHERE EACH BANK'S TRIANGLES SIT IN THE VECTOR.** Declared once, here, because the derive and the
/// space have to agree about it and a silent disagreement would evaluate the right triangle at the
/// wrong coordinate — a bug that changes the sound and breaks no build.
inline constexpr size_t kTypeScalars = 0;      ///< 3 — comb feedback, comb mix, SVF feedback
inline constexpr size_t kFilterMix = 3;        ///< 3 — the three-band blend
inline constexpr size_t kFlavourScalars = 6;   ///< 9
inline constexpr size_t kFilterLfoResponse = 15; ///< 3
inline constexpr size_t kFilterPhaseOffset = 18; ///< 3
inline constexpr size_t kModScalars = 21;      ///< 8
inline constexpr size_t kLfoInitialPhase = 29; ///< 1
inline constexpr size_t kLfoShape = 30;        ///< 9 — inside `lfoShapeAt`, invisible from the derive
static_assert(kLfoShape + 9 == kTriangles, "the layout must account for every triangle");

using Basis = SliceBasis<kTriangles>;
using Phases = std::array<double, kTriangles>;

/// **A CONTROL'S DIRECTION IN THE SPACE: THE VECTOR ITS 1-D VALUE TRANSLATES ALONG.**
///
/// THE DESIGN: three decode matrices, each of the dimension of the set of params behind its
/// knob. The knob value (the 1-D phase offset, gamma, etc.) drives translation along the unit
/// vector in that sub-matrix; equivalently, multiply by the identity matrix for the
/// parameter set in question.
///
/// **THIS REPLACES A HARDCODED SWITCH WITH THE THING THE SWITCH WAS A SPECIAL CASE OF.** The first
/// version asked `groupOf(i)` and assigned one of three phases — correct, and it could only ever
/// express axis-aligned contiguous groups. A control is a VECTOR; the switch was the identity matrix
/// for a parameter set, spelled as a conditional. Saying so directly costs nothing now and is what
/// lets a later control point somewhere the groups do not.
///
/// CONTIGUOUS BECAUSE THE LAYOUT IS, not because a control must be. A `ControlAxis` naming a `first`
/// and a `count` IS the identity restricted to that parameter set; a control whose direction is not a
/// run of the layout would carry its own 39-vector instead, and nothing above this line would change.
struct ControlAxis {
	size_t first;
	size_t count;
};

/// THE FOUR CONTROLS THE LIGHT UI EXPOSES, as directions. we can just have 1d gamma and phase
/// offset and phase push in the n-dim unit vector.
///
/// **THE RANKS ARE THE ANALOGY.** The units of travel — the modulation amount is walking around,
/// the underlying phase axis is driving on the highway, and gamma is flying — are not three scales
/// on one axis. They are three RANKS: a
/// modulation reaches one parameter, a coordinate's phase reaches its group, gamma reaches all
/// thirty-nine. At N = 1 all three collapse to addition, which is why `sliceHome` cannot show the
/// difference and this space can.
inline constexpr ControlAxis kTypeAxis { kTypeScalars, kTypeTriangles };
inline constexpr ControlAxis kFlavourAxis { kFlavourScalars, kFlavourTriangles };
inline constexpr ControlAxis kModAxis { kModScalars, kModTriangles };
inline constexpr ControlAxis kGammaAxis { 0, kTriangles };

/// TRANSLATE ALONG A CONTROL'S UNIT VECTOR — `phases += amount * unit(axis)`.
inline void pushAlong(Phases& phases, const ControlAxis& axis, double amount) {
	for (size_t i = 0; i < axis.count; ++i) {
		phases[axis.first + i] += amount;
	}
}

/// **HOME AS A SUM OF CONTROL PUSHES**, which is the formulation above rather than a restatement of
/// the derive. Every existing patch reads identically because the four pushes at a given setting add
/// to exactly what the three grouped sums produced.
[[nodiscard]] inline Phases homeFor(double typePhase, double flavourPhase, double modPhase) {
	Phases home {};
	pushAlong(home, kTypeAxis, typePhase);
	pushAlong(home, kFlavourAxis, flavourPhase);
	pushAlong(home, kModAxis, modPhase);
	return home;
}

/// **THE PLANE, DERIVED FROM THE ROTATE GENERATOR AND THE HOME — EXPLORE'S OWN, NOT A NEW ONE.**
///
/// DECIDED: the whole topological complex, adopted in place of the
/// Type-across/Flavour-up plane first proposed.
///
/// `sliceBasisFor` is the tuned mechanism — turn rates spaced on phi, phase and rate spreads set
/// against a measured guard (a rate near zero FREEZES a triangle and inflates an octave span without
/// the ladder having widened), and the rate rows turning faster than the phase rows so a triangle's
/// "how far it moves" and "how fast it varies" are not one control. None of that is worth
/// re-deriving, and a second spelling of it would be the duplication the consolidation survey exists
/// to complain about.
///
/// **BORN OPEN, WHICH IS WHY THE AXES ARE NOT "TYPE ACROSS, FLAVOUR UP".** At rotate 0 this is already
/// a real plane — a DIFFERENT one at every home — because the section 3 decision says a fresh sound
/// must have a second dimension without anyone turning anything, or travel is a second gamma for
/// every unrotated patch. So X does not move Type's six triangles; it moves all thirty-nine in a
/// phi-spaced pattern. **The earlier "axes are Type x Flavour" decision was made before
/// the basis was in view**, when the question was which two named things a square shows; once the square
/// is a slice of a 39-cube the axes are the plane's own directions and no coordinate owns one.
///
/// HOME IS AN ARGUMENT rather than folded into the turn, because M is a function of both.
[[nodiscard]] inline Basis basisFor(double rotate, double home) {
	return sliceBasisFor<kTriangles>(rotate, home);
}

/// THE HOME THE PLANE IS TAKEN AT. The basis wants one number, and the space has thirty-nine; the mean
/// is the honest reduction — every coordinate contributes and none is privileged. At a rank-1 home
/// (the only kind anything builds today) it is exactly the common value.
[[nodiscard]] inline double planeHomeOf(const Phases& home) {
	double sum = 0.0;
	for (const double v : home) {
		sum += v;
	}
	return sum / static_cast<double>(kTriangles);
}

/// WHERE EVERY TRIANGLE IS EVALUATED, given a home, a plane and a position on it.
///
/// AT ZERO TRAVEL THIS IS `home`, BIT FOR BIT, whatever the basis — `SliceBasis::phaseFor` guarantees
/// it by construction (both products are exactly +/-0.0 and adding that to a finite value is exact),
/// and the case beside this asserts it rather than trusting the comment.
[[nodiscard]] inline Phases phasesAt(const Phases& home, const Basis& basis, double travelX,
                                     double travelY) {
	Phases out {};
	for (size_t i = 0; i < kTriangles; ++i) {
		out[i] = basis.phaseFor(i, home[i], travelX, travelY);
	}
	return out;
}

/// EVALUATE A BANK WITH ONE PHASE PER TRIANGLE.
///
/// **A STRICT GENERALISATION OF `evalTriangleBank`, NOT AN ALTERNATIVE TO IT.** That function's scalar
/// path is `results[i] = evalTriangle(phase, freqMult, configs[i])` and `PHI_TRIANGLE_USE_NEON`
/// defaults to 0 and is defined nowhere in this repo — so handing every triangle the same phase here
/// reproduces the bank bit for bit. That is what lets the derive move onto per-triangle phases without
/// changing a sound.
template <size_t Bank>
[[nodiscard]] inline std::array<float, Bank> evalAt(
    const Phases& phases, size_t first,
    const std::array<owlet::dsp::phi::PhiTriConfig, Bank>& configs) {
	std::array<float, Bank> out {};
	for (size_t i = 0; i < Bank; ++i) {
		out[i] = owlet::dsp::phi::evalTriangle(phases[first + i], 1.0f, configs[i]);
	}
	return out;
}

} // namespace phase_space
} // namespace feathers::fx::automod
