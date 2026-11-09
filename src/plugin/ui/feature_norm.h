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

#include "plugin/ui/terrain.h"

#include <array>
#include <functional>

// STANDARDISING THE FEATURES, OFFLINE, SO A COLOUR MAP CAN BE FILLED EVENLY.
//
// An offline standardisation pass is part of artifact generation, so it cannot be forgotten as a manual
// step and needs no CI job of its own.
//
// THE PROBLEM IT SOLVES. Every feature this surface measures has its own lumpy distribution — energy
// piles up in a band, the fundamental's share leans hard toward the top of its range, parity clusters
// near a half. Feeding raw values to a two-dimensional colour map would use a corner of the map and
// leave the rest unvisited, so most of the map's expressive range would be spent on values nothing
// ever takes. Standardising first means the map's whole area is reachable, and it means a map can be
// designed against a square rather than against a cloud of unknown shape.
//
// PERCENTILES RATHER THAN MEAN AND VARIANCE, and the reason is the shape of the data rather than a
// preference. A mean/σ standardisation is exactly right for a bell and wrong for anything else: it
// leaves a skewed feature skewed, still bunched, merely re-centred, and it has no defined bounds so a
// map still has to clamp. A percentile map is a monotone function fitted to the ACTUAL distribution —
// it sends the median to 0.5 and the deciles to the deciles by construction, so the standardised
// feature is uniform on [0,1] whatever it looked like before, which is precisely the input a colour
// map wants. It costs a table and a binary search rather than two constants, and the table is baked.
//
// THE FRAME IS PART OF THE ANSWER, so it is stated rather than tuned: both engines, thirty-two homes
// spread over sixty-four home units (some sixty-five thousand zones, several countries), sixteen
// travels across the reachable plane, eight rotations around the circle. ONE STANDARDISATION FOR BOTH
// ENGINES on purpose — the same decision that gave them one set of colour gains: stair really does sit
// lower in RMS, and it should really draw darker, where a per-engine stretch would be the
// view-dependent refit §4a forbids wearing a different hat.

namespace feathers::ui {

/// How many quantile knots each feature's map carries. Thirty-three puts a knot every 3.125% — fine
/// enough that the piecewise-linear fit between them is straight where the distribution is, and small
/// enough to read in a diff when the baked artifact changes.
inline constexpr int kNormKnots = 33;

using NormKnots = std::array<float, kNormKnots>;

/// One standardisation: the quantiles of each feature over the sampling frame, plus the projection
/// that makes the second colour axis independent of the first.
struct FeatureNorm {
	NormKnots energy {};
	NormKnots centroid {};
	NormKnots roughness {};
	NormKnots parity {};
	NormKnots fundamental {};

	/// THE RESIDUAL AXIS, and it lives here because it is measured from the same frame by the same
	/// generator and goes stale for the same reasons — a projection fitted to the world is as much a
	/// property of the world as a quantile is.
	///
	/// WHY IT EXISTS: standardising each feature makes each one uniform on [0,1], and does nothing at
	/// all about their JOINT distribution. Parity and centroid correlate at −0.72, so the pair walks a
	/// diagonal band across the map and two thirds of the square never gets used — which is exactly
	/// what made a two-dimensional map read as a one-dimensional colour map. The
	/// residual is parity with its centroid component subtracted: uncorrelated with the first axis by
	/// construction, so the pair fills the square.
	///
	/// ITS SENTENCE IS STILL TRUE, which is the thing a projection can quietly cost: the axis no longer
	/// says "odd-heavy" outright, it says ODD-HEAVY FOR ITS SPECTRAL TILT. At any fixed tilt it still
	/// tracks the odd/even balance exactly — the case measures that within centroid bins rather than
	/// trusting it.
	float parityResidualSlope = 0.0f;
	float parityResidualIntercept = 0.0f;
	NormKnots parityResidual {};

	friend bool operator==(const FeatureNorm&, const FeatureNorm&) = default;
};

/// The second colour axis as it ships: parity, with the part of it that the first axis already says
/// removed, then standardised so it fills its own range. Both arguments raw, as measured.
float residualParity(const FeatureNorm& norm, float centroid, float parity);

/// A raw feature value, mapped onto [0,1] by where it falls among the knots. Monotone, clamped at both
/// ends, and exactly linear between neighbouring knots.
float standardize(const NormKnots& knots, float raw);

/// THE INVERSE: a [0,1] position among the knots, back to the raw value there (linear between knots, the
/// ends at the ends). standardize then destandardize through ANOTHER table is quantile matching: a value
/// that sits at the p-th quantile of one population comes out as the p-th quantile of the other.
float destandardize(const NormKnots& knots, float u);

/// PER-ENGINE CHANNELS (the Explore topography read dark and dull without them): the features an engine's
/// terrain channels are computed from. Every engine but Resonate reads its features AS MEASURED — the
/// channels' gains were calibrated on the shared Morph/Stair world, so for them nothing moves. Resonate's
/// energy, centroid and roughness are quantile-matched from its own table onto the shared one, so its
/// brightest ground glows as brightly and its roughest carries as much grain as the shared world's do.
TerrainFeatures channelFeaturesFor(OscType engine, const TerrainFeatures& features);

/// WALK THE SAMPLING FRAME, once, visiting every sample's features.
///
/// Exposed so that the knots and the correlation table are measured over the SAME population — a
/// frame written twice is a frame that will disagree with itself the first time either copy is
/// tuned, and the whole point of stating the frame is that it is one thing.
void forEachFrameSample(const std::function<void(const TerrainFeatures&)>& visit);

/// THE RESONATE ENGINE'S FRAME (Resonate gets its own baked normalisation knots): the SAME
/// homes, rotations and travels, walked for OscType::Resonate once per physics model (all five), since a
/// Resonate map may be drawn under any of them. Its own population, so its own table: a Resonate texel is
/// the string's rest pose, a far darker and smoother family of cycles than Morph's or Stair's, and through
/// their knots it drew in one hue. Morph and Stair keep theirs, untouched.
void forEachResonateFrameSample(const std::function<void(const TerrainFeatures&)>& visit);

/// THE ADDITIVE ENGINE'S FRAME (backlog LY, 2026-10-26): the same homes, rotations and travels, walked for OscType::Additive with the
/// default additive picture (the partial sound each row gives at its default Shape, bias and Smoothness). Its own population, so its own
/// table: an Additive texel is a normalised partial sum whose RMS sits near 0.25 where the shared world runs to 0.9, which drew it a third
/// as bright as every other engine through their knots (measured by the MEASURE case "the distribution each engine's terrain colours by").
void forEachAdditiveFrameSample(const std::function<void(const TerrainFeatures&)>& visit);
/// Pluck's own frame (2026-10-31): the same homes, rotations and travels, through the string as heard (ks::heardSpectrum).
void forEachPluckFrameSample(const std::function<void(const TerrainFeatures&)>& visit);
/// The drum's own frame (2026-11-05): the same homes, rotations and travels, once per KIND, through the drum as heard
/// (drum::heardSpectrum) at neutral scalers.
void forEachDrumFrameSample(const std::function<void(const TerrainFeatures&)>& visit);

/// MEASURE the standardisation from the world itself — the generator's engine, and the same call the
/// staleness case makes. Deterministic: no clock, no RNG, no threads, so two runs on two machines
/// produce the same table or something is wrong that we want to hear about.
FeatureNorm measureFeatureNorm();
/// The same over forEachResonateFrameSample.
FeatureNorm measureResonateFeatureNorm();
/// The same over forEachAdditiveFrameSample.
FeatureNorm measureAdditiveFeatureNorm();
/// The same over forEachPluckFrameSample.
FeatureNorm measurePluckFeatureNorm();
/// The same over forEachDrumFrameSample.
FeatureNorm measureDrumFeatureNorm();

/// THE BAKED TABLE, generated by tools/bake-feature-norm and checked in beside its generator.
///
/// WHY CHECKED IN RATHER THAN COMPUTED AT STARTUP: it is a property of the world, not of the session,
/// so recomputing it on every plugin load would spend a hundred milliseconds re-deriving a constant —
/// and, worse, would let the shipped colours drift silently the day somebody changed a bank. Baked, a
/// change to the character layer shows up as a diff in a checked-in file, reviewed like any other.
///
/// AND IT CANNOT GO STALE QUIETLY: `feature-norm is not stale` recomputes it and compares. The suite
/// goes red the moment the world moves under the table, which is the mechanism intended — no
/// remembering, no CI.
extern const FeatureNorm kBakedFeatureNorm;
/// Resonate's own, baked by the same tool from measureResonateFeatureNorm and checked by the same staleness case.
extern const FeatureNorm kBakedResonateFeatureNorm;
/// Additive's own, baked by the same tool from measureAdditiveFeatureNorm and checked by the same staleness case.
extern const FeatureNorm kBakedAdditiveFeatureNorm;
/// Pluck's own (2026-10-31), baked by the same tool from measurePluckFeatureNorm and checked by the same staleness case.
extern const FeatureNorm kBakedPluckFeatureNorm;
/// The drum's own (2026-11-05), baked by the same tool from measureDrumFeatureNorm, every kind pooled.
extern const FeatureNorm kBakedDrumFeatureNorm;

/// THE TABLE AN ENGINE COLOURS BY: Resonate's and Additive's own for themselves, the shared Morph/Stair one for every other
/// engine (V5 included, by decision V10c).
const FeatureNorm& featureNormFor(OscType engine);

} // namespace feathers::ui
