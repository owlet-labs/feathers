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

#include "plugin/ui/terrain_likes.h"
#include "plugin/ui/terrain_warp.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// **LIKES ON ANY SURFACE** (backlog ON: 2026-10-26): the score and the warp do not know what they are scoring.
// A surface is a plane of positions, a fixed-length fingerprint (LikeVector, kLikeDims long) at each, and the
// non-position controls that also move those fingerprints; given that and the likes, `deriveLikes` builds the model
// (whitening, the controls' consistent directions projected out, the likes' agreement weights) and the warp. The
// character terrain (terrain.cpp's CharacterSurface) is the only surface today.

namespace feathers::ui {

class LikeSurface {
public:
	virtual ~LikeSurface() = default;
	/// The fingerprint at each of `points`, in order, appended to `out`.
	virtual void fingerprintsAt(std::span<const WarpPoint> points, std::vector<LikeVector>& out) const = 0;
	/// THE NON-POSITION CONTROLS THAT FEED THE FINGERPRINT — every one, with no opinion about which matter: the
	/// derivation measures what each does. A control the surface's current state does not read moves nothing and is
	/// ignored.
	[[nodiscard]] virtual size_t controlCount() const = 0;
	[[nodiscard]] virtual const char* controlName(size_t control) const = 0;
	/// The same, with `control` nudged a small step and every other held.
	virtual void nudgedFingerprintsAt(size_t control, std::span<const WarpPoint> points,
	                                  std::vector<LikeVector>& out) const = 0;
};

/// THE SUPPORT SCAN: kSupportScan places a side over kSupportReach either way of the likes' middle (twelve x1 views). A
/// scanned local maximum scoring at least kSupportCandidate is a CANDIDATE; refined (below), one scoring at least
/// kSupportAt gets a lens.
inline constexpr int kSupportScan = 24;
inline constexpr double kSupportReach = 12.0 * kLikeRegionUnit;
inline constexpr double kSupportAt = 0.25;
inline constexpr double kSupportCandidate = 0.15;
/// THE REFINE: the best kRefineCandidates candidates, each searched on a kRefineGrid-a-side grid over a scan cell either
/// way; and a shaped lens's samples a ray.
inline constexpr int kRefineCandidates = 8;
inline constexpr int kRefineGrid = 16;
inline constexpr int kLensRaySamples = 24;
/// How sharply a shaped lens's magnification follows the score: the slope goes with (score / peak)^this, so a place half
/// as liked gets half the room at 1 (the core is magnified by its score; no sharper exponent, by design).
inline constexpr double kLensSharpness = 1.0;
/// THE SIMILAR CORE of a shaped lens's ray ends where the score first falls below this share of the peak's; past it is
/// the give-back margin.
inline constexpr double kLensCoreAt = 0.5;

/// A PLACE A LENS IS CENTRED ON (a like, or a peak of the support), how high it scores, and the lens radius it was given.
struct LikePeak {
	WarpPoint at;
	double score = 0.0;
	double radius = 0.0;
};

/// WHAT THE LIKES MAKE OF A SURFACE: the model a fingerprint is scored with, the region the likes span, the warp solved
/// from the score on it, and what deriving them cost (wall time, fingerprints measured).
struct LikeDerivation {
	LikeModel model;
	LikeRegion region;
	TerrainWarp warp;
	double millis = 0.0;
	/// Of which the solve; and how many scanned places scored into the support.
	double solveMillis = 0.0;
	int supportPlaces = 0;
	/// THE SCORE ON THE CARTOGRAM'S LATTICE (its region, (kWarpCells+1)^2, row-major) — what the warp was solved from, and what a
	/// texel's score is read off (`scoreAt`), so the fine fingerprint is never taken per texel.
	std::vector<double> latticeScore;
	/// The lattice score at a real place, bilinear; 0 outside the region.
	[[nodiscard]] double scoreAt(WarpPoint world) const noexcept;
	int evaluations = 0;
	/// THE PEAKS the lenses are centred on, in lens order, and the scan's candidates for distant ones; the likes' places;
	/// the strength asked for. What the refine works from.
	std::vector<LikePeak> lensPeaks;
	std::vector<LikePeak> candidates;
	std::vector<WarpPoint> likePlaces;
	double strength = kWarpStrength;
	/// WHETHER THE REFINE HAS RUN: distant centres searched coarse to fine, every lens shaped by the score (`refineLikes`).
	bool refined = false;
	double refineMillis = 0.0;
};

/// **THE DERIVATION**: the likes' own square (from their places), an 8x8 pre-pass over it at the surface as it is and with
/// each control nudged in turn, the model from that and the likes' fingerprints; the score on the square's kWarpCells
/// lattice and the cartogram solved from it; and a ROUND lens at every like and every peak of the support scan. Fast,
/// so the first map after the likes change does not wait on the refine. Pure and deterministic.
LikeDerivation deriveLikes(const LikeSurface& surface, std::span<const WarpPoint> likePlaces,
                           std::span<const LikeVector> likeFingerprints, double strength = kWarpStrength);

/// **THE REFINE** (a round lens enlarges geometry, not similarity): the scan's best candidates searched
/// coarse to fine (a kRefineGrid grid each, duplicates merged), then EVERY lens shaped from the score measured along its
/// rays — each ray's extent as far as the room it gives balances the room it takes, its magnification following the
/// score along it (WarpLens::extent, ::shape). Run when nothing else is waiting; `abandon` is asked between steps and a
/// true answer gives up (nothing, so the round lenses stand). Pure and deterministic otherwise.
std::optional<LikeDerivation> refineLikes(const LikeSurface& surface, const LikeDerivation& base,
                                          const std::function<bool()>& abandon);

} // namespace feathers::ui
