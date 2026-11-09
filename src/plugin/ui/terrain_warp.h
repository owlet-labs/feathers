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

#include <cstddef>
#include <span>
#include <vector>

namespace feathers::ui {


/// **THE WARP — AREA-EQUALISING AND NEVER FOLDING, IN WORLD COORDINATES** (by design: independent of zoom; view only).
///
/// A square region of the plane, `n` cells a side, holds a regular lattice in the VIEW's coordinates and, for each lattice
/// vertex, the WORLD point drawn there. Inside a cell the map is bilinear between its four vertices; outside the region it
/// is the identity, and the region's boundary vertices never move, so the two meet without a seam. Every cell is kept
/// strictly convex and positively oriented by the solver, which is what makes the bilinear map one-to-one: the warp
/// cannot fold.
///
/// A liked region COVERS LESS WORLD per cell, so it is drawn larger: the view gives it room, and the bake, which samples
/// on the warped lattice, gives it detail.
/// **A LENS — THE STRONG HALF OF THE WARP** (20-30x linear at the top). A radial map of a disk onto
/// itself, centred on a liked place, identity outside: a place at radius r of `radius` is drawn at radius R g(r/R), with
/// g' = `magnification` at the centre falling smoothly to kLensRim, so the liked core is drawn that many times larger and
/// the rim gives up the room, never squeezed below kLensRim in either direction. g is strictly increasing, so a lens is a
/// bijection however strong; lenses are kept apart (their disks never overlap), so their rims never compound.
///
/// THE ROOM IS ZERO-SUM, which is why this exists beside the cartogram rather than inside it: a place drawn m times larger
/// takes m^2 of view area from somewhere, so the cartogram, which shares a region among all its cells, can only magnify
/// that much a spot under 1/m^2 of its region (measured: at 25x the cartogram's likes stalled at 1.4-1.6x). A lens takes
/// its room from its own rim, so its strength costs nothing elsewhere.
struct WarpLens {
	WarpPoint centre; ///< in the cartogram's view
	double radius = 0.0;
	double magnification = 1.0;
	/// The core's share of the radius, solved so that g(1) = 1.
	double core = 1.0;
	/// **SHAPED BY THE SCORE** (a round lens enlarges geometry, not similarity): when these are set the
	/// lens is STAR-SHAPED — `rays` directions evenly round the centre, each with its own extent (`extent`, where the room
	/// it gives balances what it takes) and its own normalised profile G over x in [0, 1] (`profile`, kLensProfilePoints a
	/// ray, G(0) = 0, G(1) = 1, slope never below kLensRim), measured from the score along that ray. Between two rays the
	/// extent and the profile are blended linearly by angle, so the map is a radial bijection: each ray onto itself,
	/// monotone in r, continuous in angle, the identity outside. Empty: the round Gaussian lens above.
	std::vector<double> extent;
	std::vector<double> shape;
	[[nodiscard]] bool starShaped() const noexcept { return !extent.empty(); }

	[[nodiscard]] double profile(double x) const noexcept; ///< g
	[[nodiscard]] double slope(double x) const noexcept;   ///< g'
	[[nodiscard]] WarpPoint forward(WarpPoint p) const noexcept;
	[[nodiscard]] WarpPoint inverse(WarpPoint p) const noexcept;
};
/// The lens's slope at its rim: no place in a lens is drawn smaller than this, linearly.
inline constexpr double kLensRim = 0.5;
/// How many points a star lens's profile holds a ray, and how many rays it has.
inline constexpr int kLensProfilePoints = 32;
inline constexpr int kLensRays = 32;
/// A lens of this magnification at this radius, its core solved.
WarpLens makeLens(WarpPoint centre, double radius, double magnification);

struct TerrainWarp {
	double centreX = 0.0;
	double centreY = 0.0;
	double half = 0.0;
	int n = 0;
	/// (n+1)^2 world points, row-major from the region's low corner — the CARTOGRAM, the mild, score-wide half.
	std::vector<WarpPoint> world;
	/// THE LENSES, the strong half, applied after the cartogram in this order (view = lenses(cartogram view)).
	std::vector<WarpLens> lenses;

	[[nodiscard]] bool contains(WarpPoint p) const noexcept;
	/// Where a view point is in the world — what the bake samples.
	[[nodiscard]] WarpPoint worldAt(WarpPoint view) const noexcept;
	/// And back: where a world point is drawn — the puck, the likes, the crumbs.
	[[nodiscard]] WarpPoint viewAt(WarpPoint world) const noexcept;
	/// The smallest corner cross product over every cell as a share of THAT cell's area — > 0 is "no cell folded or went
	/// concave" (the solver keeps kWarpShape) — and the largest world area one view cell covers, over an unwarped cell's.
	[[nodiscard]] double minimumShape() const noexcept;
	[[nodiscard]] double maximumSqueeze() const noexcept;
	/// The cartogram alone, both ways.
	[[nodiscard]] WarpPoint cartogramWorldAt(WarpPoint view) const noexcept;
	[[nodiscard]] WarpPoint cartogramViewAt(WarpPoint world) const noexcept;
};

/// Cells a side of the cartogram's lattice.
inline constexpr int kWarpCells = 32;
/// **THE STRENGTH — THE ONE NUMBER TO TURN**: how many times larger, LINEARLY, a place scoring 1 is drawn — a like's own
/// place always is — by its lens. 25 by design (20-30x at the top). FEATHERS_WARP_STRENGTH overrides it for a capture.
inline constexpr double kWarpStrength = 25.0;
/// THE CARTOGRAM'S OWN STRENGTH, linear, for the score-wide redistribution under the lenses: mild, so the lenses carry the
/// top end.
inline constexpr double kCartogramStrength = 3.0;
/// THE ROOM FLOOR — A FIXED SHARE, not a ratio to the top: the cartogram draws no place smaller than this, linearly, and a
/// lens no smaller than kLensRim, so nothing is drawn below their product (1/4) and every place stays reachable by hand
/// and by Mutate.
inline constexpr double kWarpFloorLinear = 0.5;
/// THE SHAPE GUARD, scale-free: every cartogram corner's cross product at least this share of its own cell's area, so
/// cells stay strictly convex (the map one-to-one) however large or small they get. 1 is a parallelogram.
inline constexpr double kWarpShape = 0.15;
/// A LENS'S RADIUS, in x1 views, and the most lenses a warp holds (likes first, then the support's peaks by score).
inline constexpr double kLensRadiusViews = 2.0;
inline constexpr int kMaxLenses = 16;


/// THE SOLVE: `score` holds (n+1)^2 values at the region's regular world lattice, row-major. A uniform score is the
/// identity, exactly.
TerrainWarp solveTerrainWarp(const LikeRegion& region, int n, std::span<const double> score,
                             double strength = kCartogramStrength);

} // namespace feathers::ui
