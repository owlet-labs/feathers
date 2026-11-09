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

#include "plugin/ui/like_surface.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <numbers>

namespace feathers::ui {

namespace {

/// The view-to-world Jacobian of a warp at a view point, by central differences — how much the warp so far already
/// magnifies there, which a lens's own strength is reckoned against.
double magnificationAt(const TerrainWarp& warp, WarpPoint view, double h) {
	const WarpPoint px = warp.worldAt({ view.x + h, view.y });
	const WarpPoint mx = warp.worldAt({ view.x - h, view.y });
	const WarpPoint py = warp.worldAt({ view.x, view.y + h });
	const WarpPoint my = warp.worldAt({ view.x, view.y - h });
	const double jacobian = ((px.x - mx.x) * (py.y - my.y) - (px.y - mx.y) * (py.x - my.x)) / (4.0 * h * h);
	return jacobian > 0.0 ? 1.0 / std::sqrt(jacobian) : 1.0;
}

/// EACH PEAK GETS THE LARGEST LENS THAT CLEARS THE ONES BEFORE IT — up to kLensRadiusViews, two disks never overlapping
/// in the VIEW however the cartogram squeezes between them (never below kWarpFloorLinear), so no rim compounds another's.
/// Peaks by score, so likes (score 1) come first, and a like shares the room with every other like, kept or not yet.
std::vector<LikePeak> keepPeaks(std::vector<LikePeak> peaks, std::span<const WarpPoint> likePlaces) {
	std::stable_sort(peaks.begin(), peaks.end(), [](const LikePeak& a, const LikePeak& b) { return a.score > b.score; });
	const double lensRadius = kLensRadiusViews * kLikeRegionUnit;
	std::vector<LikePeak> kept;
	for (LikePeak p : peaks) {
		if (static_cast<int>(kept.size()) >= kMaxLenses) {
			break;
		}
		double radius = lensRadius;
		if (p.score >= 1.0) {
			for (const WarpPoint& other : likePlaces) {
				const double apart = std::hypot(p.at.x - other.x, p.at.y - other.y);
				if (apart > 0.0) {
					radius = std::min(radius, 0.5 * apart * kWarpFloorLinear);
				}
			}
		}
		for (const LikePeak& k : kept) {
			radius = std::min(radius, std::hypot(p.at.x - k.at.x, p.at.y - k.at.y) * kWarpFloorLinear - k.radius);
		}
		if (radius >= 0.1 * lensRadius) {
			p.radius = radius;
			kept.push_back(p);
		}
	}
	return kept;
}

/// What a lens at a peak should magnify by, on top of what the warp before it already does there.
double lensMagnification(const TerrainWarp& warp, const LikePeak& peak, WarpPoint centre, double strength) {
	return (1.0 + (strength - 1.0) * peak.score) / magnificationAt(warp, centre, 1e-6 * peak.radius);
}

} // namespace

LikeDerivation deriveLikes(const LikeSurface& surface, std::span<const WarpPoint> likePlaces,
                           std::span<const LikeVector> likeFingerprints, double strength) {
	const auto started = std::chrono::steady_clock::now();
	LikeDerivation d;
	d.strength = strength;
	d.likePlaces.assign(likePlaces.begin(), likePlaces.end());
	d.region = likeRegionFor(likePlaces);
	// THE PRE-PASS: the cell centres of an 8x8 grid over the likes' region — the region, not a view, so the model is the
	// same at every pan and zoom.
	std::vector<WarpPoint> points;
	const double cell = 2.0 * d.region.half / kLikePrepass;
	for (int j = 0; j < kLikePrepass; ++j) {
		for (int i = 0; i < kLikePrepass; ++i) {
			points.push_back({ d.region.centreX - d.region.half + (i + 0.5) * cell,
			                   d.region.centreY - d.region.half + (j + 0.5) * cell });
		}
	}
	LikeModelInputs inputs;
	surface.fingerprintsAt(points, inputs.map);
	d.evaluations += static_cast<int>(points.size());
	for (size_t c = 0; c < surface.controlCount(); ++c) {
		inputs.nudged.emplace_back();
		surface.nudgedFingerprintsAt(c, points, inputs.nudged.back());
		d.evaluations += static_cast<int>(points.size());
	}
	inputs.likes.assign(likeFingerprints.begin(), likeFingerprints.end());
	d.model = buildLikeModel(inputs);
	// **THE PEAKS — THE SCORE'S SUPPORT ACROSS THE SCANNED WORLD** (based on support, by design):
	// every like, and every local maximum of the scan scoring at least kSupportAt. Every local maximum scoring at least
	// kSupportCandidate is kept as a CANDIDATE for the refine to search.
	std::vector<LikePeak> peaks;
	for (const WarpPoint& like : likePlaces) {
		peaks.push_back({ like, 1.0, 0.0 });
	}
	{
		const double span = 2.0 * kSupportReach / kSupportScan;
		std::vector<WarpPoint> scan;
		for (int j = 0; j < kSupportScan; ++j) {
			for (int i = 0; i < kSupportScan; ++i) {
				scan.push_back({ d.region.centreX - kSupportReach + (i + 0.5) * span,
				                 d.region.centreY - kSupportReach + (j + 0.5) * span });
			}
		}
		std::vector<LikeVector> prints;
		surface.fingerprintsAt(scan, prints);
		d.evaluations += static_cast<int>(scan.size());
		std::vector<double> scores(prints.size());
		for (size_t p = 0; p < prints.size(); ++p) {
			scores[p] = d.model.score(prints[p]);
		}
		for (int j = 0; j < kSupportScan; ++j) {
			for (int i = 0; i < kSupportScan; ++i) {
				const double s = scores[static_cast<size_t>(j * kSupportScan + i)];
				if (s < kSupportCandidate) {
					continue;
				}
				d.supportPlaces += s >= kSupportAt ? 1 : 0;
				bool highest = true;
				for (int dj = -1; dj <= 1 && highest; ++dj) {
					for (int di = -1; di <= 1; ++di) {
						const int u = i + di;
						const int v = j + dj;
						if ((di != 0 || dj != 0) && u >= 0 && v >= 0 && u < kSupportScan && v < kSupportScan
						    && scores[static_cast<size_t>(v * kSupportScan + u)] > s) {
							highest = false;
							break;
						}
					}
				}
				if (highest) {
					const LikePeak peak { scan[static_cast<size_t>(j * kSupportScan + i)], s, 0.0 };
					d.candidates.push_back(peak);
					if (s >= kSupportAt) {
						peaks.push_back(peak);
					}
				}
			}
		}
		std::stable_sort(d.candidates.begin(), d.candidates.end(),
		                 [](const LikePeak& a, const LikePeak& b) { return a.score > b.score; });
	}
	d.lensPeaks = keepPeaks(std::move(peaks), likePlaces);
	// THE CARTOGRAM: the score on the region's own regular lattice, then the solve.
	std::vector<WarpPoint> lattice;
	const double step = 2.0 * d.region.half / kWarpCells;
	for (int j = 0; j <= kWarpCells; ++j) {
		for (int i = 0; i <= kWarpCells; ++i) {
			lattice.push_back({ d.region.centreX - d.region.half + i * step, d.region.centreY - d.region.half + j * step });
		}
	}
	std::vector<LikeVector> latticePrints;
	surface.fingerprintsAt(lattice, latticePrints);
	d.evaluations += static_cast<int>(lattice.size());
	std::vector<double> score;
	score.reserve(latticePrints.size());
	for (const LikeVector& f : latticePrints) {
		score.push_back(d.model.score(f));
	}
	// A LIKE'S OWN PLACE IS LIKED BY DEFINITION, and the lattice can miss it: a score peak narrower than a cell falls
	// between vertices and gets no room (measured: one of three likes at 1.01x linear). So each vertex takes at least
	// what its distance to the nearest like gives, over a cell and a half — anti-aliasing the sampled score, not a rule
	// about places: away from the likes it is nothing and the sound's score decides.
	const double reach = 1.5 * step;
	for (size_t v = 0; v < lattice.size(); ++v) {
		for (const WarpPoint& like : likePlaces) {
			const double dx = (lattice[v].x - like.x) / reach;
			const double dy = (lattice[v].y - like.y) / reach;
			score[v] = std::max(score[v], std::exp(-(dx * dx + dy * dy)));
		}
	}
	d.latticeScore = score;
	const auto solveStarted = std::chrono::steady_clock::now();
	d.warp = solveTerrainWarp(d.region, kWarpCells, score);
	d.solveMillis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - solveStarted).count();
	// AND A ROUND LENS AT EACH PEAK, centred where the warp so far draws it — until the refine shapes them.
	for (const LikePeak& p : d.lensPeaks) {
		const WarpPoint centre = d.warp.viewAt(p.at);
		d.warp.lenses.push_back(makeLens(centre, p.radius, lensMagnification(d.warp, p, centre, strength)));
	}
	d.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return d;
}

std::optional<LikeDerivation> refineLikes(const LikeSurface& surface, const LikeDerivation& base,
                                          const std::function<bool()>& abandon) {
	const auto started = std::chrono::steady_clock::now();
	LikeDerivation d = base;
	d.warp.lenses.clear();
	// 1. DISTANT CENTRES, COARSE TO FINE: the best candidates each searched on a fine grid a scan cell either way, the
	//    best place found standing for the candidate; two that land within a scan cell are one.
	const double span = 2.0 * kSupportReach / kSupportScan;
	std::vector<LikePeak> refined;
	for (size_t c = 0; c < d.candidates.size() && static_cast<int>(c) < kRefineCandidates; ++c) {
		if (abandon()) {
			return std::nullopt;
		}
		const LikePeak& candidate = d.candidates[c];
		std::vector<WarpPoint> grid;
		for (int j = 0; j < kRefineGrid; ++j) {
			for (int i = 0; i < kRefineGrid; ++i) {
				grid.push_back({ candidate.at.x - span + (i + 0.5) * 2.0 * span / kRefineGrid,
				                 candidate.at.y - span + (j + 0.5) * 2.0 * span / kRefineGrid });
			}
		}
		std::vector<LikeVector> prints;
		surface.fingerprintsAt(grid, prints);
		d.evaluations += static_cast<int>(grid.size());
		LikePeak best = candidate;
		for (size_t g = 0; g < grid.size(); ++g) {
			const double s = d.model.score(prints[g]);
			if (s > best.score) {
				best = { grid[g], s, 0.0 };
			}
		}
		bool duplicate = false;
		for (const LikePeak& r : refined) {
			duplicate = duplicate || std::hypot(r.at.x - best.at.x, r.at.y - best.at.y) < span;
		}
		if (!duplicate) {
			refined.push_back(best);
		}
	}
	std::vector<LikePeak> peaks;
	for (const WarpPoint& like : d.likePlaces) {
		peaks.push_back({ like, 1.0, 0.0 });
	}
	for (const LikePeak& r : refined) {
		if (r.score >= kSupportAt) {
			peaks.push_back(r);
		}
	}
	d.lensPeaks = keepPeaks(std::move(peaks), d.likePlaces);
	// 2. EVERY LENS SHAPED BY THE SCORE along kLensRays rays, kLensRaySamples a ray. Per ray, with sigma the score along it
	//    over the peak's own: the slope of the view radius against the world radius is kLensRim + (M - kLensRim) sigma(r),
	//    so the magnification follows the score; the extent B is the nearest at which that can reach the lens's target M
	//    while giving back exactly the room it takes (G(B) = B: M = kLensRim + (1 - kLensRim) B / I(B), I the integral of
	//    sigma) — as far as the score stays high — or the lens's radius if it never can. The extents are smoothed round
	//    the circle and each ray's M recomputed from its smoothed extent, so G(B) = B still holds exactly.
	const double c = kLensRim;
	for (const LikePeak& p : d.lensPeaks) {
		if (abandon()) {
			return std::nullopt;
		}
		const WarpPoint centre = d.warp.viewAt(p.at);
		const double target = lensMagnification(d.warp, p, centre, d.strength);
		std::vector<WarpPoint> samples;
		for (int ray = 0; ray < kLensRays; ++ray) {
			const double angle = 2.0 * std::numbers::pi * ray / kLensRays;
			for (int i = 0; i < kLensRaySamples; ++i) {
				const double r = (i + 0.5) / kLensRaySamples * p.radius;
				// The lens's input is the cartogram's view; earlier lenses are the identity on this disk (they never overlap).
				samples.push_back(d.warp.cartogramWorldAt({ centre.x + r * std::cos(angle), centre.y + r * std::sin(angle) }));
			}
		}
		std::vector<LikeVector> prints;
		surface.fingerprintsAt(samples, prints);
		d.evaluations += static_cast<int>(samples.size());
		const double peakScore = std::max(p.score, 1e-6);
		// sigma at radius r along a ray: the peak's own at 0, the samples, linear between them, the last held to the rim.
		std::vector<double> sigma(static_cast<size_t>(kLensRays * kLensRaySamples));
		for (size_t k = 0; k < sigma.size(); ++k) {
			sigma[k] = std::pow(std::min(1.0, d.model.score(prints[k]) / peakScore), kLensSharpness);
		}
		const auto sigmaAt = [&](int ray, double r) {
			const double at = r / p.radius * kLensRaySamples - 0.5;
			if (at <= 0.0) {
				const double t = std::max(0.0, at + 0.5) / 0.5;
				return (1.0 - t) * 1.0 + t * sigma[static_cast<size_t>(ray * kLensRaySamples)];
			}
			const int i = std::min(static_cast<int>(at), kLensRaySamples - 1);
			const double t = std::min(1.0, at - i);
			const double a = sigma[static_cast<size_t>(ray * kLensRaySamples + i)];
			const double b = sigma[static_cast<size_t>(ray * kLensRaySamples + std::min(i + 1, kLensRaySamples - 1))];
			return (1.0 - t) * a + t * b;
		};
		// The integral of sigma from 0 to r, by the trapezoid on a fine step.
		constexpr int kSteps = 4 * kLensRaySamples;
		std::vector<double> integral(static_cast<size_t>(kLensRays * (kSteps + 1)));
		for (int ray = 0; ray < kLensRays; ++ray) {
			double sum = 0.0;
			integral[static_cast<size_t>(ray * (kSteps + 1))] = 0.0;
			for (int k = 1; k <= kSteps; ++k) {
				const double r0 = p.radius * (k - 1) / kSteps;
				const double r1 = p.radius * k / kSteps;
				sum += 0.5 * (sigmaAt(ray, r0) + sigmaAt(ray, r1)) * (r1 - r0);
				integral[static_cast<size_t>(ray * (kSteps + 1) + k)] = sum;
			}
		}
		const auto integralAt = [&](int ray, double r) {
			const double at = std::clamp(r / p.radius, 0.0, 1.0) * kSteps;
			const int k = std::min(static_cast<int>(at), kSteps - 1);
			const double t = at - k;
			return (1.0 - t) * integral[static_cast<size_t>(ray * (kSteps + 1) + k)]
			       + t * integral[static_cast<size_t>(ray * (kSteps + 1) + k + 1)];
		};
		// THE SIMILAR CORE a ray: out to where the score first falls below kLensCoreAt of the peak's ("as far as the score
		// stays high"). THE GIVE-BACK MARGIN beyond it (2026-10-27): the core is magnified by its score, and
		// the room it takes is given back by an annulus of the dissimilar ground past it, drawn at the floor. Along a ray the
		// view length is fixed, so a core magnified M times needs the lens to reach B = (M - c) I / (1 - c), I the score's
		// integral over the core — about M times the core. B is capped at the lens radius; where it caps, M is what fits.
		std::vector<double> core(static_cast<size_t>(kLensRays), p.radius);
		for (int ray = 0; ray < kLensRays; ++ray) {
			for (int k = 1; k <= kSteps; ++k) {
				const double r = p.radius * k / kSteps;
				if (sigmaAt(ray, r) < kLensCoreAt) {
					core[static_cast<size_t>(ray)] = r;
					break;
				}
			}
		}
		std::vector<double> extent(static_cast<size_t>(kLensRays));
		for (int ray = 0; ray < kLensRays; ++ray) {
			const double e = core[static_cast<size_t>(ray)];
			const double wanted = (target - c) * integralAt(ray, e) / (1.0 - c);
			extent[static_cast<size_t>(ray)] = std::clamp(wanted, e, p.radius);
		}
		for (int pass = 0; pass < 2; ++pass) {
			std::vector<double> smoothCore(core.size());
			std::vector<double> smoothExtent(extent.size());
			for (int ray = 0; ray < kLensRays; ++ray) {
				const size_t before = static_cast<size_t>((ray + kLensRays - 1) % kLensRays);
				const size_t after = static_cast<size_t>((ray + 1) % kLensRays);
				smoothCore[static_cast<size_t>(ray)] = 0.25 * core[before] + 0.5 * core[static_cast<size_t>(ray)] + 0.25 * core[after];
				smoothExtent[static_cast<size_t>(ray)] =
				    0.25 * extent[before] + 0.5 * extent[static_cast<size_t>(ray)] + 0.25 * extent[after];
			}
			core.swap(smoothCore);
			extent.swap(smoothExtent);
		}
		WarpLens lens;
		lens.centre = centre;
		lens.radius = p.radius;
		lens.magnification = target;
		lens.extent = extent;
		lens.shape.resize(static_cast<size_t>(kLensRays * (kLensProfilePoints + 1)));
		for (int ray = 0; ray < kLensRays; ++ray) {
			const double b = extent[static_cast<size_t>(ray)];
			const double e = std::min(core[static_cast<size_t>(ray)], b);
			const double ie = std::max(integralAt(ray, e), 1e-12);
			// THE CORE'S M, so the ray gives back exactly what it takes: c B + (M - c) I(e) = B.
			const double m = c + (1.0 - c) * b / ie;
			for (int k = 0; k <= kLensProfilePoints; ++k) {
				const double r = b * k / kLensProfilePoints;
				lens.shape[static_cast<size_t>(ray * (kLensProfilePoints + 1) + k)] =
				    k == kLensProfilePoints ? 1.0 : (c * r + (m - c) * integralAt(ray, std::min(r, e))) / b;
			}
		}
		d.warp.lenses.push_back(std::move(lens));
	}
	d.refined = true;
	d.refineMillis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return d;
}

double LikeDerivation::scoreAt(WarpPoint world) const noexcept {
	const int n = kWarpCells;
	if (latticeScore.size() != static_cast<size_t>(n + 1) * static_cast<size_t>(n + 1) || !(region.half > 0.0)) {
		return 0.0;
	}
	const double cell = 2.0 * region.half / n;
	const double u = (world.x - (region.centreX - region.half)) / cell;
	const double v = (world.y - (region.centreY - region.half)) / cell;
	if (!(u >= 0.0 && v >= 0.0 && u <= n && v <= n)) {
		return 0.0;
	}
	const int i = std::min(static_cast<int>(u), n - 1);
	const int j = std::min(static_cast<int>(v), n - 1);
	const double s = u - i;
	const double t = v - j;
	const auto at = [&](int x, int y) { return latticeScore[static_cast<size_t>(y) * static_cast<size_t>(n + 1) + x]; };
	return (1 - s) * (1 - t) * at(i, j) + s * (1 - t) * at(i + 1, j) + s * t * at(i + 1, j + 1) + (1 - s) * t * at(i, j + 1);
}

} // namespace feathers::ui
