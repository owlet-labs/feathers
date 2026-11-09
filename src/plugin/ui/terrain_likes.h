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

#include "plugin/terrain_likes.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// **WHAT THE LIKES SHARE, AND THE ROOM THE MAP GIVES IT** (backlog ON: 2026-10-26) — the arithmetic, with no engine
// in it: the bake (terrain.cpp) measures, this decides. Every function here is a pure function of its arguments,
// deterministic, with no RNG and no clock, so the score and the warp are DERIVED from the stored likes and the sound's
// controls and are never stored.
//
// THE FINGERPRINT is HELLINGER: the square root of the normalised power spectrum, grouped into
// log-spaced bands so the many high harmonics do not outvote the fundamental region. Loudness drops out by construction.
// It is taken from a FINE cycle (kLikeFineCycle samples), and only at the warp's own points — the likes, the pre-pass, the
// support scan and the cartogram lattice — never per texel: the terrain picture keeps its 32-sample lens and its colours.
// (It was twelve log shares of the 32-sample lens, with an energy dimension, until then.)
//
// NO REDUNDANCY (the aim is generality and eliminating redundancy, not one listener's preference). The
// fingerprint keeps only what the POSITION gives that nothing else can:
//   1. WHITEN under the map-wide covariance, so correlated partials do not count twice;
//   2. PROJECT OUT each control's direction where it is CONSISTENT ACROSS THE MAP — a knob that moves every place's
//      envelope the same way is the knob's job, not the place's — measured by nudging each control on an 8x8 pre-pass.
//      No hand list: loudness and tilt fall out if they are global. CAPPED at half the dimensions, most consistent first,
//      and the residual dimensionality is reported, because a flat score is the failure mode;
//   3. WEIGHT what is left by how much the likes AGREE on it — map spread over like spread — so the features the likes
//      share count and the ones they scatter on drop out.

namespace feathers::ui {

/// A PLACE ON A SURFACE'S PLANE — the two position coordinates a fingerprint is a function of.
struct WarpPoint {
	double x = 0.0;
	double y = 0.0;
	friend bool operator==(const WarpPoint&, const WarpPoint&) = default;
};

/// THE FINE CYCLE a like's spectrum is measured from, and the fine grid it is kept on (plugin/terrain_likes.h). 256: harmonics
/// to 127, the grid's top six bins empty. MEASURED derivation (V5, one like): 256 samples 139-145 ms, 512 samples 205-219 ms
/// (the 32-sample fingerprint's was 45-55); pans and redraws pay nothing either way.
inline constexpr int kLikeFineCycle = 256;
inline constexpr int kLikeFineBins = static_cast<int>(kTerrainLikeFine);
using LikeSpectrum = std::array<float, kTerrainLikeFine>;
/// THE BANDS: third-octave, two fine bins each, so 24 dimensions.
inline constexpr int kLikeBands = kLikeFineBins / 2;
inline constexpr int kLikeDims = kLikeBands;
using LikeVector = std::array<double, kLikeDims>;
/// Band b spans fine bins [edges[b], edges[b+1]).
using LikeBandEdges = std::array<int, kLikeBands + 1>;
/// The fixed third-octave layout.
LikeBandEdges fixedLikeBands();

/// THE FINE SPECTRUM of a cycle (any length, a power of two from 32 up): the power of each harmonic below its Nyquist, summed
/// into the sixth-octave bins.
LikeSpectrum likeFineSpectrum(std::span<const float> cycle);
/// THE FINGERPRINT: band powers over `edges`, normalised to sum one, square-rooted — a point on the unit sphere, so the
/// Euclidean distance between two is the Hellinger distance (up to sqrt 2). A silent spectrum is all zero.
LikeVector likeFingerprint(const LikeSpectrum& spectrum, const LikeBandEdges& edges = fixedLikeBands());
/// **ADAPTIVE BANDS** (a contender in the leave-one-out MEASURE, not the default): band edges at
/// equal-mass quantiles of (k logPrior + n pooledLikedPower) / (k + n) over the fine bins, logPrior uniform across the bins
/// (equal mass per sixth-octave), the likes' normalised spectra pooled, n the likes' count, k = kAdaptivePrior. Edges are
/// strictly increasing, every band at least one fine bin.
inline constexpr double kAdaptivePrior = 3.0;
LikeBandEdges adaptiveLikeBands(std::span<const LikeSpectrum> likes);
/// THE OLD FINGERPRINT, kept for the leave-one-out MEASURE only: twelve log shares of the 32-sample lens's harmonic
/// magnitudes, normalised by their norm, in the first twelve dimensions and zero after.
LikeVector logShapeFingerprint(std::span<const float> partials);

/// WHAT THE BAKE'S PRE-PASS MEASURED, and the likes: everything the model is built from.
struct LikeModelInputs {
	/// The fingerprints of the pre-pass grid, at the controls as they are.
	std::vector<LikeVector> map;
	/// For each control the pre-pass nudged, the same grid's fingerprints with that control moved and every other held.
	/// A control that moved nothing (the engine does not read it) is recognised as such and ignored.
	std::vector<std::vector<LikeVector>> nudged;
	/// The likes' fingerprints, from their stored envelopes.
	std::vector<LikeVector> likes;
};

/// THE DERIVED MODEL: `z(f) = T (f - mean)`, and the likes in the same coordinates.
struct LikeModel {
	LikeVector mean {};
	/// `axes` rows of kLikeDims.
	std::vector<LikeVector> transform;
	std::vector<std::vector<double>> likesAt;
	/// THE REPORT the design asks for: how many dimensions were projected out, how many are left, and how many of those
	/// the weights effectively use (a participation ratio, 1..axes) — a flat score shows up here as a small number.
	int removed = 0;
	int axes = 0;
	double effectiveDims = 0.0;
	/// Per control the pre-pass nudged: its consistency across the map (0 position-dependent .. 1 the same everywhere),
	/// -1 where it moved nothing, and whether it was projected out.
	std::vector<double> consistency;
	std::vector<bool> projected;

	[[nodiscard]] bool empty() const noexcept { return likesAt.empty() || axes == 0; }
	/// How like a liked sound this envelope is: 1 at a like's own fingerprint, falling toward 0 with distance (relative to `typical`) in the
	/// model's coordinates; the nearest like decides. 0 with no likes.
	[[nodiscard]] double score(const LikeVector& f) const;
	[[nodiscard]] std::vector<double> coordinates(const LikeVector& f) const;
	/// The squared distance, in the model's coordinates, to the nearest like; and the median of it over the pre-pass,
	/// which is what a score is measured against.
	[[nodiscard]] double nearestSquared(const LikeVector& f) const;
	double typical = 1.0;
};

/// A DIRECTION IS PROJECTED OUT when its consistency reaches this — its mean over the map carries at least half its
/// energy, i.e. more of what it does is the same everywhere than differs from place to place.
inline constexpr double kLikeConsistentAt = 0.5;
/// AT MOST THIS MANY of the kLikeDims dimensions go (the guard rail: at most half).
inline constexpr int kLikeMaxRemoved = kLikeDims / 2;

LikeModel buildLikeModel(const LikeModelInputs& inputs);

//--------//
// Region //
//--------//

/// THE REGION A WARP COVERS, from the likes alone (so a pan or a zoom cannot move it): centred on their mean place,
/// reaching `kLikeRegionPad` beyond the farthest and never less than `kLikeRegionHalfMin` either way. In travel units;
/// `kLikeRegionUnit` is the ×1 view's span (ExploreStage::kBaseTravel).
inline constexpr double kLikeRegionUnit = 64.0 / 1023.0;
inline constexpr double kLikeRegionHalfMin = 1.5 * kLikeRegionUnit;
inline constexpr double kLikeRegionPad = 1.0 * kLikeRegionUnit;
/// The pre-pass's grid, a side.
inline constexpr int kLikePrepass = 8;

struct LikeRegion {
	double centreX = 0.0;
	double centreY = 0.0;
	double half = 0.0;
};
/// From the likes' PLACES — a surface's own coordinates, whatever stores them.
LikeRegion likeRegionFor(std::span<const WarpPoint> places);

} // namespace feathers::ui
