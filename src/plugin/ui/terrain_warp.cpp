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

#include "plugin/ui/terrain_warp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <cstdio>
#include <cstdlib>

namespace feathers::ui {

namespace {

/// HOW STRONGLY A VERTEX IS PULLED TO THE MIDDLE OF ITS NEIGHBOURS, against how strongly it is pushed toward its cells' areas.
constexpr double kWarpSmoothing = 0.5;

double cross(WarpPoint o, WarpPoint a, WarpPoint b) {
	return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/// The smallest of a quad's four corner cross products (counter-clockwise a, b, c, d): positive for a strictly convex,
/// positively oriented quad.
double cornerMin(WarpPoint a, WarpPoint b, WarpPoint c, WarpPoint d) {
	return std::min({ cross(a, b, d), cross(b, c, a), cross(c, d, b), cross(d, a, c) });
}

WarpPoint bilinear(WarpPoint a, WarpPoint b, WarpPoint c, WarpPoint d, double s, double t) {
	return { (1 - s) * (1 - t) * a.x + s * (1 - t) * b.x + s * t * c.x + (1 - s) * t * d.x,
		     (1 - s) * (1 - t) * a.y + s * (1 - t) * b.y + s * t * c.y + (1 - s) * t * d.y };
}

} // namespace

bool TerrainWarp::contains(WarpPoint p) const noexcept {
	return n > 0 && p.x > centreX - half && p.x < centreX + half && p.y > centreY - half && p.y < centreY + half;
}

WarpPoint TerrainWarp::cartogramWorldAt(WarpPoint view) const noexcept {
	if (!contains(view)) {
		return view;
	}
	const double cell = 2.0 * half / n;
	const double u = (view.x - (centreX - half)) / cell;
	const double v = (view.y - (centreY - half)) / cell;
	const int i = std::clamp(static_cast<int>(std::floor(u)), 0, n - 1);
	const int j = std::clamp(static_cast<int>(std::floor(v)), 0, n - 1);
	const auto at = [this](int x, int y) { return world[static_cast<size_t>(y) * static_cast<size_t>(n + 1) + x]; };
	return bilinear(at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), u - i, v - j);
}

WarpPoint TerrainWarp::cartogramViewAt(WarpPoint p) const noexcept {
	if (!contains(p)) {
		return p;
	}
	const double cell = 2.0 * half / n;
	const auto at = [this](int x, int y) { return world[static_cast<size_t>(y) * static_cast<size_t>(n + 1) + x]; };
	// THE CELL THAT HOLDS IT. The cells tile the region without overlap (none folds, the boundary is fixed), so exactly
	// one does, up to the shared edges; the nearest by its worst edge test is taken so an edge point is never lost.
	int bestI = 0;
	int bestJ = 0;
	double best = -std::numeric_limits<double>::infinity();
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			const WarpPoint a = at(i, j);
			const WarpPoint b = at(i + 1, j);
			const WarpPoint c = at(i + 1, j + 1);
			const WarpPoint d = at(i, j + 1);
			const double inside = std::min({ cross(a, b, p), cross(b, c, p), cross(c, d, p), cross(d, a, p) });
			if (inside > best) {
				best = inside;
				bestI = i;
				bestJ = j;
			}
			if (inside >= 0.0) {
				j = n; // found
				break;
			}
		}
	}
	const WarpPoint a = at(bestI, bestJ);
	const WarpPoint b = at(bestI + 1, bestJ);
	const WarpPoint c = at(bestI + 1, bestJ + 1);
	const WarpPoint d = at(bestI, bestJ + 1);
	// NEWTON ON THE BILINEAR, from the cell's middle: one-to-one on a convex quad, so it converges to the one answer.
	double s = 0.5;
	double t = 0.5;
	for (int step = 0; step < 24; ++step) {
		const WarpPoint q = bilinear(a, b, c, d, s, t);
		const double ex = q.x - p.x;
		const double ey = q.y - p.y;
		const double dxs = (1 - t) * (b.x - a.x) + t * (c.x - d.x);
		const double dys = (1 - t) * (b.y - a.y) + t * (c.y - d.y);
		const double dxt = (1 - s) * (d.x - a.x) + s * (c.x - b.x);
		const double dyt = (1 - s) * (d.y - a.y) + s * (c.y - b.y);
		const double det = dxs * dyt - dxt * dys;
		if (det == 0.0) {
			break;
		}
		const double ds = (ex * dyt - ey * dxt) / det;
		const double dt = (ey * dxs - ex * dys) / det;
		s -= ds;
		t -= dt;
		if (std::abs(ds) + std::abs(dt) < 1e-14) {
			break;
		}
	}
	return { centreX - half + (bestI + s) * cell, centreY - half + (bestJ + t) * cell };
}


WarpPoint TerrainWarp::worldAt(WarpPoint view) const noexcept {
	for (auto lens = lenses.rbegin(); lens != lenses.rend(); ++lens) {
		view = lens->inverse(view);
	}
	return cartogramWorldAt(view);
}

WarpPoint TerrainWarp::viewAt(WarpPoint world) const noexcept {
	WarpPoint at = cartogramViewAt(world);
	for (const WarpLens& lens : lenses) {
		at = lens.forward(at);
	}
	return at;
}

//--------//
// Lenses //
//--------//

namespace {

/// The integral of exp(-s^2) from 0 to t.
double gaussIntegral(double t) {
	return 0.886226925452758 * std::erf(t);
}

} // namespace

double WarpLens::profile(double x) const noexcept {
	return kLensRim * x + (magnification - kLensRim) * core * gaussIntegral(x / core);
}

double WarpLens::slope(double x) const noexcept {
	return kLensRim + (magnification - kLensRim) * std::exp(-(x / core) * (x / core));
}

WarpLens makeLens(WarpPoint centre, double radius, double magnification) {
	WarpLens lens;
	lens.centre = centre;
	lens.radius = radius;
	lens.magnification = std::max(magnification, 1.0);
	// THE CORE, so that the rim lands on itself: kLensRim + (m - kLensRim) core E(1/core) = 1. A fixed point that settles in
	// a few steps, since E(1/core) is all but its limit once the core is small.
	if (lens.magnification > 1.0) {
		double core = (1.0 - kLensRim) / ((lens.magnification - kLensRim) * 0.886226925452758);
		for (int step = 0; step < 50; ++step) {
			core = (1.0 - kLensRim) / ((lens.magnification - kLensRim) * gaussIntegral(1.0 / core));
		}
		lens.core = core;
	}
	return lens;
}

namespace {

/// A STAR LENS'S RAY BLEND at an angle: the two rays either side and the weight of the second.
struct RayBlend {
	int a = 0;
	int b = 0;
	double t = 0.0;
};
RayBlend rayBlend(const WarpLens& lens, double dx, double dy) {
	const int rays = static_cast<int>(lens.extent.size());
	double turn = std::atan2(dy, dx) / (2.0 * std::numbers::pi);
	if (turn < 0.0) {
		turn += 1.0;
	}
	const double at = turn * rays;
	RayBlend blend;
	blend.a = std::min(static_cast<int>(at), rays - 1);
	blend.b = (blend.a + 1) % rays;
	blend.t = at - blend.a;
	return blend;
}

/// The blended normalised profile at x in [0, 1].
double starProfile(const WarpLens& lens, const RayBlend& r, double x) {
	const double at = std::clamp(x, 0.0, 1.0) * kLensProfilePoints;
	const int i = std::min(static_cast<int>(at), kLensProfilePoints - 1);
	const double s = at - i;
	const auto g = [&](int ray, int k) { return lens.shape[static_cast<size_t>(ray * (kLensProfilePoints + 1) + k)]; };
	const double ga = (1 - s) * g(r.a, i) + s * g(r.a, i + 1);
	const double gb = (1 - s) * g(r.b, i) + s * g(r.b, i + 1);
	return (1 - r.t) * ga + r.t * gb;
}

} // namespace

WarpPoint WarpLens::forward(WarpPoint p) const noexcept {
	if (starShaped()) {
		const double dx = p.x - centre.x;
		const double dy = p.y - centre.y;
		const double r = std::hypot(dx, dy);
		if (r == 0.0) {
			return p;
		}
		const RayBlend blend = rayBlend(*this, dx, dy);
		const double reach = (1 - blend.t) * extent[static_cast<size_t>(blend.a)] + blend.t * extent[static_cast<size_t>(blend.b)];
		if (r >= reach) {
			return p;
		}
		const double scale = reach * starProfile(*this, blend, r / reach) / r;
		return { centre.x + dx * scale, centre.y + dy * scale };
	}
	const double dx = p.x - centre.x;
	const double dy = p.y - centre.y;
	const double r = std::hypot(dx, dy);
	if (magnification <= 1.0 || r >= radius || r == 0.0) {
		return p;
	}
	const double scale = radius * profile(r / radius) / r;
	return { centre.x + dx * scale, centre.y + dy * scale };
}

WarpPoint WarpLens::inverse(WarpPoint p) const noexcept {
	if (starShaped()) {
		const double dx = p.x - centre.x;
		const double dy = p.y - centre.y;
		const double r = std::hypot(dx, dy);
		if (r == 0.0) {
			return p;
		}
		const RayBlend blend = rayBlend(*this, dx, dy);
		const double reach = (1 - blend.t) * extent[static_cast<size_t>(blend.a)] + blend.t * extent[static_cast<size_t>(blend.b)];
		if (r >= reach) {
			return p;
		}
		// THE BLENDED PROFILE IS PIECEWISE LINEAR ON ITS GRID AND INCREASING, so its inverse is exact: find the segment, solve it.
		const double want = r / reach;
		int lo = 0;
		int hi = kLensProfilePoints;
		while (hi - lo > 1) {
			const int mid = (lo + hi) / 2;
			if (starProfile(*this, blend, static_cast<double>(mid) / kLensProfilePoints) <= want) {
				lo = mid;
			}
			else {
				hi = mid;
			}
		}
		const double g0 = starProfile(*this, blend, static_cast<double>(lo) / kLensProfilePoints);
		const double g1 = starProfile(*this, blend, static_cast<double>(lo + 1) / kLensProfilePoints);
		const double x = (lo + (g1 > g0 ? (want - g0) / (g1 - g0) : 0.0)) / kLensProfilePoints;
		const double scale = reach * x / r;
		return { centre.x + dx * scale, centre.y + dy * scale };
	}
	const double dx = p.x - centre.x;
	const double dy = p.y - centre.y;
	const double r = std::hypot(dx, dy);
	if (magnification <= 1.0 || r >= radius || r == 0.0) {
		return p;
	}
	// g IS STRICTLY INCREASING (g' >= kLensRim), so g(x) = r/R has one root in [0, 1]: Newton from below, kept in the
	// bracket by bisection.
	const double want = r / radius;
	double lo = 0.0;
	double hi = 1.0;
	double x = want / magnification;
	for (int step = 0; step < 60; ++step) {
		const double g = profile(x) - want;
		if (g > 0.0) {
			hi = x;
		}
		else {
			lo = x;
		}
		double next = x - g / slope(x);
		if (!(next > lo && next < hi)) {
			next = 0.5 * (lo + hi);
		}
		if (std::abs(next - x) < 1e-16) {
			x = next;
			break;
		}
		x = next;
	}
	const double scale = radius * x / r;
	return { centre.x + dx * scale, centre.y + dy * scale };
}
double TerrainWarp::minimumShape() const noexcept {
	if (n <= 0) {
		return 1.0;
	}
	const auto at = [this](int x, int y) { return world[static_cast<size_t>(y) * static_cast<size_t>(n + 1) + x]; };
	double least = std::numeric_limits<double>::infinity();
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			const WarpPoint a = at(i, j);
			const WarpPoint b = at(i + 1, j);
			const WarpPoint c = at(i + 1, j + 1);
			const WarpPoint d = at(i, j + 1);
			const double area = 0.5 * (cross(a, b, c) + cross(a, c, d));
			least = std::min(least, area > 0.0 ? cornerMin(a, b, c, d) / area : -1.0);
		}
	}
	return least;
}

double TerrainWarp::maximumSqueeze() const noexcept {
	if (n <= 0) {
		return 1.0;
	}
	const double cell = 2.0 * half / n;
	const auto at = [this](int x, int y) { return world[static_cast<size_t>(y) * static_cast<size_t>(n + 1) + x]; };
	double most = 0.0;
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			most = std::max(most, 0.5 * (cross(at(i, j), at(i + 1, j), at(i + 1, j + 1))
			                             + cross(at(i, j), at(i + 1, j + 1), at(i, j + 1)))
			                          / (cell * cell));
		}
	}
	return most;
}

TerrainWarp solveTerrainWarp(const LikeRegion& region, int n, std::span<const double> score, double strength) {
	const double gain = strength * strength - 1.0;
	TerrainWarp warp;
	warp.centreX = region.centreX;
	warp.centreY = region.centreY;
	warp.half = region.half;
	warp.n = n;
	const auto stride = static_cast<size_t>(n + 1);
	const double cell = 2.0 * region.half / n;
	const double x0 = region.centreX - region.half;
	const double y0 = region.centreY - region.half;
	warp.world.resize(stride * stride);
	for (int j = 0; j <= n; ++j) {
		for (int i = 0; i <= n; ++i) {
			warp.world[static_cast<size_t>(j) * stride + i] = { x0 + i * cell, y0 + j * cell };
		}
	}
	if (score.size() != stride * stride) {
		return warp;
	}
	// THE SCORE AT ANY WORLD POINT of the region, bilinear over the lattice it was measured on.
	const auto density = [&](WarpPoint p) {
		if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
			return 1.0;
		}
		const double u = std::clamp((p.x - x0) / cell, 0.0, static_cast<double>(n));
		const double v = std::clamp((p.y - y0) / cell, 0.0, static_cast<double>(n));
		const int i = std::min(static_cast<int>(u), n - 1);
		const int j = std::min(static_cast<int>(v), n - 1);
		const double s = u - i;
		const double t = v - j;
		const auto at = [&](int x, int y) { return score[static_cast<size_t>(y) * stride + x]; };
		const double value = (1 - s) * (1 - t) * at(i, j) + s * (1 - t) * at(i + 1, j) + s * t * at(i + 1, j + 1)
		                     + (1 - s) * t * at(i, j + 1);
		return 1.0 + gain * std::clamp(value, 0.0, 1.0);
	};
	// A UNIFORM SCORE IS THE IDENTITY, exactly — every cell already holds its share, so nothing would move; said here
	// rather than left to the arithmetic, so the claim does not rest on rounding.
	const auto [low, high] = std::minmax_element(score.begin(), score.end());
	if (*high - *low < 1e-12) {
		return warp;
	}

	// THE SOLVE. Each cell wants to cover world in inverse proportion to the density of the world it covers. The vertices
	// descend an energy — the squared log-ratio of every cell's area to its share, plus a pull of each vertex toward its
	// four neighbours' middle — and a round is kept only if the energy fell (else the step halves and the round is undone).
	// The boundary does not move, and no vertex may take any of its cells below the room floor: that is the no-fold
	// guarantee, kept at every move.
	//
	// THE PULL IS NOT DECORATION: area is one constraint a cell against two freedoms a vertex, and without it cells slid into
	// slivers that met their area and looked nothing like it (MEASURED: 6x linear in a sliver against 3.5x asked). THE SHARES
	// ARE RE-READ EVERY kHold ROUNDS, not every round: a target that moved with each step made the descent chase itself.
	// An earlier solver pushed corners out from centres and balanced short of the target (1.4x linear against 3.5x).
	const auto cells = static_cast<size_t>(n) * static_cast<size_t>(n);
	const double regionArea = 4.0 * region.half * region.half;
	const double maxArea = cell * cell / (kWarpFloorLinear * kWarpFloorLinear); // the room floor, as world per view cell
	constexpr int kHold = 40;
	std::vector<double> share(cells, 1.0);
	std::vector<double> ratio(cells);
	std::vector<WarpPoint> gradient(warp.world.size());
	std::vector<WarpPoint> before(warp.world.size());
	const auto at = [&](int x, int y) -> WarpPoint& { return warp.world[static_cast<size_t>(y) * stride + x]; };
	const auto quadArea = [&](int i, int j) {
		const WarpPoint a = at(i, j);
		const WarpPoint b = at(i + 1, j);
		const WarpPoint c = at(i + 1, j + 1);
		const WarpPoint d = at(i, j + 1);
		return 0.5 * ((c.x - a.x) * (d.y - b.y) - (c.y - a.y) * (d.x - b.x));
	};
	const auto neighbourGap = [&](int i, int j) {
		const WarpPoint v = at(i, j);
		const WarpPoint l = at(i - 1, j);
		const WarpPoint r = at(i + 1, j);
		const WarpPoint dn = at(i, j - 1);
		const WarpPoint up = at(i, j + 1);
		return WarpPoint { (l.x + r.x + dn.x + up.x) * 0.25 - v.x, (l.y + r.y + dn.y + up.y) * 0.25 - v.y };
	};
	// The energy, and each cell's log-ratio as a side effect.
	const auto energy = [&](double inverseTotal) {
		double e = 0.0;
		for (int j = 0; j < n; ++j) {
			for (int i = 0; i < n; ++i) {
				const size_t k = static_cast<size_t>(j) * static_cast<size_t>(n) + i;
				const double a = quadArea(i, j);
				ratio[k] = a > 0.0 ? std::log(a / (share[k] / inverseTotal * regionArea)) : 0.0;
				e += ratio[k] * ratio[k];
			}
		}
		for (int j = 1; j < n; ++j) {
			for (int i = 1; i < n; ++i) {
				const WarpPoint g = neighbourGap(i, j);
				e += kWarpSmoothing * (g.x * g.x + g.y * g.y) / (cell * cell);
			}
		}
		return e;
	};
	double step = 0.25;
	int ran = 0;
	double lastWorst = 0.0;
	double inverseTotal = 1.0;
	for (int round = 0; round < 250 && step > 1e-6; ++round, ++ran) {
		if (round % kHold == 0) {
			inverseTotal = 0.0;
			for (int j = 0; j < n; ++j) {
				for (int i = 0; i < n; ++i) {
					// THE DENSITY OF THE WORLD THE CELL COVERS, meaned over a 4x4 spread of its interior rather than read at its
					// centre: a peak narrower than a cell otherwise slips between centres, no cell sees it, and it gets no room
					// (MEASURED on a toy surface: the like's own place squeezed to 5x world per view).
					double mean = 0.0;
					for (int sv = 0; sv < 4; ++sv) {
						for (int su = 0; su < 4; ++su) {
							mean += density(bilinear(at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), (su + 0.5) / 4.0,
							                         (sv + 0.5) / 4.0)) / 16.0;
						}
					}
					share[static_cast<size_t>(j) * static_cast<size_t>(n) + i] = 1.0 / mean;
					inverseTotal += share[static_cast<size_t>(j) * static_cast<size_t>(n) + i];
				}
			}
		}
		const double was = energy(inverseTotal);
		lastWorst = 0.0;
		for (const double r : ratio) {
			lastWorst = std::max(lastWorst, std::abs(r));
		}
		if (lastWorst < 1e-3) {
			if (round % kHold == 0) {
				break;
			}
			continue;
		}
		// THE GRADIENT, exact for the area term through each quad's area as half its diagonals' cross product.
		std::fill(gradient.begin(), gradient.end(), WarpPoint {});
		for (int j = 0; j < n; ++j) {
			for (int i = 0; i < n; ++i) {
				const size_t k = static_cast<size_t>(j) * static_cast<size_t>(n) + i;
				const double a = quadArea(i, j);
				if (a <= 0.0) {
					continue;
				}
				const double g = 2.0 * ratio[k] / a;
				const WarpPoint p { at(i + 1, j + 1).x - at(i, j).x, at(i + 1, j + 1).y - at(i, j).y };
				const WarpPoint q { at(i, j + 1).x - at(i + 1, j).x, at(i, j + 1).y - at(i + 1, j).y };
				const auto add = [&](int x, int y, double gx, double gy) {
					WarpPoint& v = gradient[static_cast<size_t>(y) * stride + x];
					v.x += g * gx;
					v.y += g * gy;
				};
				add(i, j, -0.5 * q.y, 0.5 * q.x);
				add(i + 1, j + 1, 0.5 * q.y, -0.5 * q.x);
				add(i + 1, j, 0.5 * p.y, -0.5 * p.x);
				add(i, j + 1, -0.5 * p.y, 0.5 * p.x);
			}
		}
		before = warp.world;
		for (int j = 1; j < n; ++j) {
			for (int i = 1; i < n; ++i) {
				WarpPoint& v = at(i, j);
				const WarpPoint& g = gradient[static_cast<size_t>(j) * stride + i];
				const WarpPoint pull = neighbourGap(i, j);
				// PRECONDITIONED BY THE VERTEX'S OWN CELLS: the area term's gradient grows as a cell shrinks, so it is scaled by the
				// area around the vertex rather than an unwarped cell's — small cells take small steps, large ones large.
				const double local = std::max(0.25 * (quadArea(i - 1, j - 1) + quadArea(i, j - 1) + quadArea(i - 1, j) + quadArea(i, j)),
				                              1e-12 * cell * cell);
				double dx = step * (-local * g.x + 2.0 * kWarpSmoothing * pull.x);
				double dy = step * (-local * g.y + 2.0 * kWarpSmoothing * pull.y);
				const double length = std::hypot(dx, dy);
				const double reach = 0.2 * std::sqrt(local); // a fifth of the vertex's own cells, at any scale
				if (length > reach) {
					dx *= reach / length;
					dy *= reach / length;
				}
				const WarpPoint from = v;
				v = { from.x + dx, from.y + dy };
				for (const auto& [ci, cj] : { std::pair { i - 1, j - 1 }, std::pair { i, j - 1 }, std::pair { i - 1, j },
				                              std::pair { i, j } }) {
					const double area = quadArea(ci, cj);
					if (area > maxArea
					    || cornerMin(at(ci, cj), at(ci + 1, cj), at(ci + 1, cj + 1), at(ci, cj + 1)) < kWarpShape * area) {
						v = from; // this vertex sits the round out rather than dent a cell or squeeze one past the floor
						break;
					}
				}
			}
		}
		if (energy(inverseTotal) < was) {
			step = std::min(0.5, step * 1.2);
		}
		else {
			warp.world = before;
			step *= 0.5;
		}
	}
	// FEATHERS_TRACE_WARP: how far the solve got — rounds run and the worst log-ratio of area to share left.
	if (std::getenv("FEATHERS_TRACE_WARP") != nullptr) {
		std::fprintf(stderr, "[WARP] rounds %d, worst log area ratio %.4f\n", ran, lastWorst);
	}
	return warp;
}

} // namespace feathers::ui
