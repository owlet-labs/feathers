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

#pragma once

#include <cstdint>

// THE WEAVE PHYSICS MODELS — everything about a WeaveString that depends on WHAT is vibrating: how
// many nodes, how the 32-entry laws land on them, where a window centred at a path position falls, the
// elastic force, the rest-pose system, and the 32-point path the scan reads. WeaveString owns the
// state, the bow, the smoother, the loudness control, the tables and the clock, and calls in here for
// the rest; downstream of scanPath() nothing knows which model ran.
//
// WHAT A MODEL CHANGES, HONESTLY. In scanned synthesis the pitch is the scan rate and the physics
// shapes how the scanned waveform MOVES, at haptic rates. So a "Bar" does not ring with a marimba's
// inharmonic partials and a "Membrane" is not a drum: each model changes how a pluck or a bow spreads,
// bends and settles across the cycle you hear.
//
// ONE STABILITY BOUND FOR ALL. Each stencil's gain is scaled from the laws' coupling so that its
// Gershgorin bound on the per-tick stiffness operator diag(k) + K is at most the Ring's own, k + 4c: Bar's
// bending gain is c/4 (its stencil sums to 16; its home spring is weaker still, below), the Membrane's is
// c/2 (its stencil sums to 8). THE TESTED BOUND IS THE FIRMWARE'S, < 4 (the laws' clamps allow 1.92,
// measured max 1.816) — the edge of ITS integrator, one full-dt step v += F - kx - dv; x += v, whose Jury
// condition is lambda < 4 - 2d. It is conservative here, and deliberately kept: this string takes two
// half-dt sub-steps, v += 0.25 (F - k(x - h)) - 0.5 d v; x += v, whose characteristic polynomial per mode
// of stiffness lambda is z^2 - (2 - lambda/4 - d/2) z + (1 - d/2); Jury's 1 - p + q > 0 is
// 4 - lambda/4 - d > 0, so the TRUE EDGE is lambda < 16 - 4d (>= 15.8 over the laws' damping).
// STIFFENING IS TESTED AGAINST THE TRUE EDGE, the linear models against the firmware's 4. A linear model's
// stiffness is fixed by the laws, so it has a bound it can meet with room and the conservative one costs
// nothing. Stiffening's cubic linearises to a Gershgorin bound of k + 4c + 6 beta (dp^2 + dm^2) at the
// node's neighbour differences, which depends on how far the string is stretched: at beta = 8c (chosen
// to be heard) it is past 4 at stretches the string reaches, so a bound of 4 would forbid that
// choice. What is tested instead is the row at the stretches ACTUALLY REACHED under the
// bounded stress, against 16 - 4d with a stated margin; the displacement rail caps any stretch at 4, and
// past what the tests reach the rails are the net (the tests measure how often they engage).

namespace feathers {

enum class WeaveModel : uint8_t {
	Ring,       ///< the firmware's closed loop: coupled masses round a circle (the default)
	/// A free-free stiff bar: bending, not tension, so disturbances spread dispersively. Held to its home by
	/// a WEAK spring, kBarHomeSpring of the laws' stiffness, so the spring does not swamp the low bending
	/// modes and hold a pluck in place; and bowed at kBarBowGain of the laws' balance, which keeps it off
	/// the rails.
	Bar,
	Tube,       ///< a chain held at both ends: the Ring cut open and pinned, read out and back
	Membrane,   ///< an 8x8 sheet held at its rim, read on a circle: disturbances arrive from across it
	/// The Ring with a cubic spring, beta (d+^3 - d-^3) with beta = 8c (kBetaOverCoupling): the springs
	/// STIFFEN AS THEY STRETCH, so a big pluck or hard bowing moves the shape faster and tighter than a
	/// gentle one, and it relaxes as it decays. For a sine shape of mode m and amplitude A the stiffness
	/// rises by the factor 1 + 3 (beta/c) A^2 sin^2(pi m / 32) (tested). HOW MUCH IS HEARD, MEASURED
	/// 2026-09-26 at a laws' pluck, median over 64 phases: modes 1-3 rise +1.3% / +3.8% / +6.0% in frequency
	/// at 8c (tested: mode 3 >= 4%), against +0.05% / +0.13% / +0.22% at the first draft's c/4, which was
	/// barely moved them. 8c was chosen from a measured table of candidate betas.
	///
	/// WHY NOT "RECURRENCE" (Fermi-Pasta-Ulam), which it was first meant to be — the decision was to
	/// try it with its own laws, measured first, this as the fallback. MEASURED with the real laws at 64
	/// phases (a single Fourier mode displaced from the rest pose; the fraction of linear-mode energy left
	/// in it over 10 s; "recurs" = below 0.5, then back above 0.8): with k x0.05, d x0.1 and beta from c to
	/// 16c, starting in mode 1, 2 or 3, at most 3 of 64 phases recur (5 with the laws averaged round the
	/// ring); energy leaves (to a fraction of 0.01) but returns to a median of only ~0.6. Averaging the
	/// laws round the ring (FPU wants a uniform lattice) and removing damping entirely reaches 27/64 at
	/// beta = 16c — on the rail, median return 0.786 — and is no longer these laws. So it ships as what it
	/// does.
	Stiffening,
};
/// The number of models. The static_assert names the LAST enumerator, so a model appended after it moves
/// that name here; the traits table below must then have its row, in enum order (both checked).
inline constexpr int kWeaveModelCount = 5;
static_assert(kWeaveModelCount == static_cast<int>(WeaveModel::Stiffening) + 1,
              "a model was added: extend kWeaveModelCount and weave::kModelTraits");
inline constexpr int kWeaveMaxNodes = 64;
/// The scan path's length, and so the table's: 32 whatever the model.
inline constexpr int kWeavePathPoints = 32;
/// The Bar's home spring as a fraction of the laws' stiffness. At the full stiffness (>= 0.016) the spring
/// is far above the bending stiffness of the low modes ((c/4)(2 - 2cos q)^2, about 4e-4 c at mode 2 —
/// modelled) and slows a pluck's spread. MEASURED 2026-09-25, a one-node pluck on the bar's middle, the
/// RMS distance of the departure from rest about the plucked node 40 ticks later, at seven phases:
/// 1.68..10.4 nodes at x1, 2.55..10.0 at x0.1 — the low-coupling phases (0.63, 0.8, 0.95), where the
/// spring dominated, go from 1.97 / 1.68 / 1.78 to 3.05 / 2.64 / 2.55 — and x0.03 adds little more
/// (3.16 / 2.74 / 2.69).
inline constexpr float kBarHomeSpring = 0.1f;
/// The Bar's bow balance as a fraction of the laws'. It comes WITH the weak spring: a bar held ten times
/// more loosely swings much further under the same bow and
/// runs into the rails. MEASURED 2026-09-25, 64 phases x 60 s, crossfade swept (displacement rail /
/// velocity rail, fraction of node-sub-steps), bow x2 then x4: full spring and bow 1.0e-3 / 0 and
/// 5.0e-3 / 7.1e-5; spring x0.1 with the full bow 2.0e-2 / 1.4e-3 and 3.5e-2 / 6.6e-3; with the bow x0.2
/// as well, 1.5e-3 / 0 and 5.9e-3 / 4.2e-5 — back where the full-spring bar was (x0.316: 3.9e-3 /
/// 1.3e-5 at x2, over the bounded case's 1e-5).
inline constexpr float kBarBowGain = 0.2f;

/// Stiffening's cubic gain as a fraction of the coupling: beta = kBetaOverCoupling * c. 8 is the chosen
/// value, from the measured table of candidates: the smallest beta that is clearly heard
/// (a median +6.0% on mode 3 at a laws' pluck) while its linearised row stays 6 inside the true edge.
inline constexpr float kBetaOverCoupling = 8.0f;
/// The Bar's bending gain as a fraction of the coupling: the bending stencil 1 -4 6 -4 1 sums to 16 in
/// absolute value, four times the Ring's 1 -2 1, so c/4 puts its Gershgorin bound at the Ring's.
inline constexpr float kBarBendingGain = 0.25f;
/// The Membrane's gain as a fraction of the coupling: its 5-point stencil sums to 8, twice the Ring's.
inline constexpr float kMembraneGridGain = 0.5f;

namespace weave {

/// What a model's nodes are joined into — which stencil, which boundary, which scan geometry.
enum class Topology : uint8_t {
	Loop,        ///< 32 nodes round a circle, neighbours i +- 1 (cyclic); the path is the nodes
	PinnedChain, ///< 32 nodes, x[-1] = x[32] = 0; Laplacian; read out and back
	FreeBar,     ///< 32 nodes, free ends; bending (biharmonic) stencil; read out and back
	Grid,        ///< 8 x 8, held at the rim; 5-point Laplacian; read on a circle
};

/// ONE ROW PER MODEL, and everything about a model that is a number or a flag. A sixth model that reuses
/// a topology is a new enumerator, a new row and kWeaveModelCount; the code dispatches on the row.
struct ModelTraits {
	WeaveModel model; ///< the row's own enumerator: rows are in enum order (checked below)
	const char* name;
	Topology topology;
	int nodes;
	float gainScale;  ///< the stencil's per-node gain as a fraction of the laws' coupling
	float homeScale;  ///< the home spring as a fraction of the laws' stiffness
	float bowScale;   ///< the bow balance as a fraction of the laws'
	bool pathIsNodes; ///< the scan reads the smoothed nodes themselves, in order (no path copy)
	bool nonlinear;   ///< adds Stiffening's cubic on the neighbour differences (a Loop only)
};

inline constexpr ModelTraits kModelTraits[] = {
	{ WeaveModel::Ring, "Ring", Topology::Loop, 32, 1.0f, 1.0f, 1.0f, true, false },
	{ WeaveModel::Bar, "Bar", Topology::FreeBar, 32, kBarBendingGain, kBarHomeSpring, kBarBowGain, false, false },
	{ WeaveModel::Tube, "Tube", Topology::PinnedChain, 32, 1.0f, 1.0f, 1.0f, false, false },
	{ WeaveModel::Membrane, "Membrane", Topology::Grid, 64, kMembraneGridGain, 1.0f, 1.0f, false, false },
	{ WeaveModel::Stiffening, "Stiffening", Topology::Loop, 32, 1.0f, 1.0f, 1.0f, true, true },
};

constexpr const ModelTraits& traits(WeaveModel m) {
	return kModelTraits[static_cast<int>(m)];
}

// What the code relies on of a row: rows in enum order, a Loop's path is its 32 nodes, the chains and loops have 32 and the
// Grid 64, and only a Loop carries the cubic (the Newton rest pose and the force are written for it).
constexpr bool traitsConsistent() {
	for (int i = 0; i < kWeaveModelCount; ++i) {
		const ModelTraits& t = kModelTraits[i];
		if (static_cast<int>(t.model) != i) {
			return false;
		}
		const int expectNodes = (t.topology == Topology::Grid) ? 64 : 32;
		if (t.nodes != expectNodes || t.nodes > kWeaveMaxNodes || t.pathIsNodes != (t.topology == Topology::Loop)
		    || (t.nonlinear && t.topology != Topology::Loop)) {
			return false;
		}
	}
	return true;
}
static_assert(sizeof(kModelTraits) / sizeof(kModelTraits[0]) == kWeaveModelCount, "one kModelTraits row per model");
static_assert(traitsConsistent(), "a kModelTraits row breaks what weave_models.cpp relies on");

inline const char* modelName(WeaveModel m) {
	return traits(m).name;
}
inline int nodeCount(WeaveModel m) {
	return traits(m).nodes;
}

/// The laws on a model's nodes, scaled by its traits row: `gain` is the elastic stencil's per-node gain
/// (the coupling times gainScale, which puts every stencil in the shared bound), `k` the stiffness times
/// homeScale, `bowBal` the balance times bowScale.
struct NodeLaws {
	float k[kWeaveMaxNodes];
	float gain[kWeaveMaxNodes];
	float d[kWeaveMaxNodes];
	float home[kWeaveMaxNodes];
	float bowBal[kWeaveMaxNodes];
};

/// The 32-entry laws (already crossfaded) placed on the model's nodes. Ring, Tube, Bar and Stiffening
/// have 32 nodes and take node i from law i. The Membrane reads stiffness, coupling, damping and bow
/// balance by RADIUS (law 0 at the centre, law 31 at the corner) and the home shape by ANGLE, times a
/// radial profile sin(pi r) that is 0 at the centre and at the rim: a home that depended on radius
/// alone would scan as a constant round the circle.
void mapLaws(WeaveModel m, const float* k, const float* c, const float* d, const float* home, const float* bowBal,
             NodeLaws& out);

/// A raised-cosine window's weight at `node`, centred at path position `pos` (0..1, the same
/// coordinate the scan runs over), half-width 1 / invHalfWidth in path units. Ring and Stiffening: the
/// firmware's circular distance. Tube and Bar: the point the out-and-back path passes at `pos`, distance
/// along the chain (one path unit is two chain lengths). Membrane: the point on the scan circle at
/// angle 2 pi pos, Euclidean distance (one path unit is the circle's circumference).
float window(WeaveModel m, int node, float pos, float invHalfWidth);
/// window() for every node at once (nodeCount entries), with anything the centre costs taken once.
void windows(WeaveModel m, float pos, float invHalfWidth, float* out);

/// The elastic force on every node, from x (nodeCount entries).
void elasticForce(WeaveModel m, const float* x, const float* gain, float* force);

/// The dense solve's working storage: one 64 x 65 double system, 33 KB — too big for the audio thread's
/// stack, where a model switch runs the solve. The string holds one as a member; a caller off the audio
/// thread may put one wherever it likes.
struct RestPoseScratch {
	double m[kWeaveMaxNodes][kWeaveMaxNodes + 1];
};

/// Where the model comes to rest under these laws with no bow: k(x - home) = elasticForce(x).
/// Linear models are solved exactly (dense, double); Stiffening is Newton-refined from its linear part,
/// each Newton system reusing the same scratch.
void restPose(WeaveModel m, const NodeLaws& laws, RestPoseScratch& scratch, float* out);

/// The kNodes-point path the scan reads, from the (smoothed) nodes. Ring and Stiffening read the nodes
/// themselves, in order (so a caller may use the nodes directly); Tube and Bar out and back along the
/// chain (0 -> 31 -> 0); the Membrane 32 bilinear samples on a circle of radius 0.6 of its half-width.
void scanPath(WeaveModel m, const float* xs, float* path);
inline bool pathIsNodes(WeaveModel m) {
	return traits(m).pathIsNodes;
}

/// THE JAWARI BRIDGE'S PLACE ON EACH MODEL (WeaveString::setBridge): the nodes that can touch it and each one's
/// distance, in node spacings, from the point on the scan path `pos` where it sits closest to the string, placed
/// by the model's own geometry as window() places a bow. The Ring, Stiffening, Tube and Bar: every node within
/// kBridgeSpan (4) spacings along the string (9 at most, fewer at a chain's end). The Membrane: every node within
/// kBridgeGridSpan (2) grid spacings of the point on its scan circle (13 at most).
inline constexpr float kBridgeSpan = 4.0f;
inline constexpr float kBridgeGridSpan = 2.0f;
inline constexpr int kBridgeMaxNodes = 16;
/// Fills `nodes` and `dist` (kBridgeMaxNodes each at most) and returns how many.
int bridgeNodes(WeaveModel m, float pos, int* nodes, float* dist);

/// The Membrane's grid: node index of (column, row), 0..7 each.
inline int membraneNode(int col, int row) {
	return row * 8 + col;
}

} // namespace weave
} // namespace feathers
