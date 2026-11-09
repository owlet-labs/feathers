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

#include "plugin/ui/terrain_likes.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace feathers::ui {

namespace {

constexpr int D = kLikeDims;
using Matrix = std::array<std::array<double, D>, D>;

/// HOW SHARPLY A LIKE'S CLOSENESS FALLS OFF, against the MAP'S OWN typical distance to its nearest like (the median over the
/// pre-pass, `LikeModel::typical`): the median place scores exp(-3), about 0.05, whatever the engine or how many axes are
/// left. Relative rather than absolute, so a map whose envelopes barely vary does not score flat.
constexpr double kScoreSharpness = 3.0;
/// The relative floor under a harmonic's share of the envelope (-80 dB) and under the envelope's own norm.
constexpr double kShapeFloor = 1e-4;
constexpr double kEnergyFloor = 1e-6;
/// THE COVARIANCE IS REGULARISED by this share of its mean variance before it is whitened: the twelve shares are tied
/// to one another (their squares sum to one), so the raw covariance is close to singular and a direction the map never
/// moves along would otherwise be magnified without limit.
constexpr double kRidgeShare = 1e-2;
/// The pseudo-observations at the map's own spread a like spread is shrunk toward, so one like weighs every axis alike
/// and two do not yet claim an axis they happen to share.
constexpr double kLikePrior = 2.0;
/// A like spread below this (in whitened units) is not resolved any finer: the heaviest weight is 1 / sqrt of it.
constexpr double kLikeSpreadFloor = 0.01;

/// THE SYMMETRIC EIGENPROBLEM, by cyclic Jacobi: deterministic, no allocation, exact enough at 13x13. On return `a`'s
/// diagonal holds the eigenvalues and `v`'s COLUMNS the eigenvectors.
void jacobi(Matrix& a, Matrix& v, int n) {
	for (int i = 0; i < n; ++i) {
		for (int j = 0; j < n; ++j) {
			v[i][j] = i == j ? 1.0 : 0.0;
		}
	}
	for (int sweep = 0; sweep < 64; ++sweep) {
		double off = 0.0;
		for (int i = 0; i < n; ++i) {
			for (int j = i + 1; j < n; ++j) {
				off += a[i][j] * a[i][j];
			}
		}
		if (off < 1e-30) {
			return;
		}
		for (int p = 0; p < n; ++p) {
			for (int q = p + 1; q < n; ++q) {
				if (std::abs(a[p][q]) < 1e-300) {
					continue;
				}
				const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
				const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
				const double c = 1.0 / std::sqrt(t * t + 1.0);
				const double s = t * c;
				for (int k = 0; k < n; ++k) {
					const double akp = a[k][p];
					const double akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < n; ++k) {
					const double apk = a[p][k];
					const double aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < n; ++k) {
					const double vkp = v[k][p];
					const double vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
		}
	}
}

LikeVector multiply(const Matrix& m, const LikeVector& x) {
	LikeVector out {};
	for (int i = 0; i < D; ++i) {
		double sum = 0.0;
		for (int j = 0; j < D; ++j) {
			sum += m[i][j] * x[j];
		}
		out[i] = sum;
	}
	return out;
}

double dot(const LikeVector& a, const LikeVector& b) {
	double sum = 0.0;
	for (int i = 0; i < D; ++i) {
		sum += a[i] * b[i];
	}
	return sum;
}

LikeVector minus(const LikeVector& a, const LikeVector& b) {
	LikeVector out {};
	for (int i = 0; i < D; ++i) {
		out[i] = a[i] - b[i];
	}
	return out;
}

} // namespace

LikeBandEdges fixedLikeBands() {
	LikeBandEdges edges {};
	for (int b = 0; b <= kLikeBands; ++b) {
		edges[static_cast<size_t>(b)] = 2 * b;
	}
	return edges;
}

LikeSpectrum likeFineSpectrum(std::span<const float> cycle) {
	LikeSpectrum fine {};
	const int n = static_cast<int>(cycle.size());
	if (n < 32 || (n & (n - 1)) != 0) {
		return fine;
	}
	// ONE REAL FFT A CYCLE (pffft): a setup and SIMD-aligned buffers per length, built once per thread and freed with it, so
	// the terrain thread never contends and a test's thread cleans up after itself.
	struct Plan {
		int n = 0;
		PFFFT_Setup* setup = nullptr;
		float* in = nullptr;
		float* out = nullptr;
		float* work = nullptr;
	};
	struct Plans {
		std::vector<Plan> plans;
		~Plans() {
			for (const Plan& p : plans) {
				pffft_destroy_setup(p.setup);
				pffft_aligned_free(p.in);
				pffft_aligned_free(p.out);
				pffft_aligned_free(p.work);
			}
		}
	};
	thread_local Plans plans;
	Plan* plan = nullptr;
	for (Plan& p : plans.plans) {
		if (p.n == n) {
			plan = &p;
		}
	}
	if (plan == nullptr) {
		const size_t bytes = sizeof(float) * static_cast<size_t>(n);
		plans.plans.push_back({ n, pffft_new_setup(n, PFFFT_REAL), static_cast<float*>(pffft_aligned_malloc(bytes)),
		                        static_cast<float*>(pffft_aligned_malloc(bytes)), static_cast<float*>(pffft_aligned_malloc(bytes)) });
		plan = &plans.plans.back();
	}
	std::copy(cycle.begin(), cycle.end(), plan->in);
	pffft_transform_ordered(plan->setup, plan->in, plan->out, plan->work, PFFFT_FORWARD);
	const float* out = plan->out;
	// Ordered real output: out[0] DC, out[1] Nyquist, then (re, im) of harmonic k at out[2k], out[2k+1].
	for (int k = 1; k < n / 2; ++k) {
		const double re = out[2 * k];
		const double im = out[2 * k + 1];
		const int bin = static_cast<int>(std::floor(6.0 * std::log2(static_cast<double>(k)) + 1e-9));
		if (bin >= kLikeFineBins) {
			break;
		}
		fine[static_cast<size_t>(bin)] += static_cast<float>((re * re + im * im) / (static_cast<double>(n) * n));
	}
	return fine;
}

LikeVector likeFingerprint(const LikeSpectrum& spectrum, const LikeBandEdges& edges) {
	LikeVector f {};
	double total = 0.0;
	for (int b = 0; b < kLikeBands; ++b) {
		double band = 0.0;
		for (int i = edges[static_cast<size_t>(b)]; i < edges[static_cast<size_t>(b + 1)] && i < kLikeFineBins; ++i) {
			band += std::max(0.0, static_cast<double>(spectrum[static_cast<size_t>(i)]));
		}
		f[static_cast<size_t>(b)] = band;
		total += band;
	}
	for (double& v : f) {
		v = total > 0.0 ? std::sqrt(v / total) : 0.0;
	}
	return f;
}

LikeBandEdges adaptiveLikeBands(std::span<const LikeSpectrum> likes) {
	// THE DENSITY: the uniform log prior's k pseudo-observations and the likes' pooled normalised power.
	std::array<double, kTerrainLikeFine> density {};
	const auto n = static_cast<double>(likes.size());
	for (int i = 0; i < kLikeFineBins; ++i) {
		density[static_cast<size_t>(i)] = kAdaptivePrior / kLikeFineBins;
	}
	for (const LikeSpectrum& like : likes) {
		double total = 0.0;
		for (const float p : like) {
			total += std::max(0.0f, p);
		}
		if (total > 0.0) {
			for (int i = 0; i < kLikeFineBins; ++i) {
				density[static_cast<size_t>(i)] += std::max(0.0f, like[static_cast<size_t>(i)]) / total;
			}
		}
	}
	double mass = 0.0;
	for (double& d : density) {
		d /= kAdaptivePrior + n;
		mass += d;
	}
	// EQUAL-MASS QUANTILES over the cumulative density, each band at least one fine bin and the last ending at the grid's end.
	LikeBandEdges edges {};
	edges[0] = 0;
	double running = 0.0;
	int bin = 0;
	for (int b = 1; b < kLikeBands; ++b) {
		const double want = mass * b / kLikeBands;
		while (bin < kLikeFineBins && running + density[static_cast<size_t>(bin)] <= want) {
			running += density[static_cast<size_t>(bin)];
			++bin;
		}
		bin = std::max(bin, edges[static_cast<size_t>(b - 1)] + 1);
		bin = std::min(bin, kLikeFineBins - (kLikeBands - b));
		running = 0.0;
		for (int i = 0; i < bin; ++i) {
			running += density[static_cast<size_t>(i)];
		}
		edges[static_cast<size_t>(b)] = bin;
	}
	edges[static_cast<size_t>(kLikeBands)] = kLikeFineBins;
	return edges;
}

LikeVector logShapeFingerprint(std::span<const float> partials) {
	LikeVector f {};
	double energy = 0.0;
	for (const float p : partials) {
		energy += static_cast<double>(p) * static_cast<double>(p);
	}
	energy = std::sqrt(energy);
	for (size_t k = 0; k < partials.size() && k < f.size(); ++k) {
		f[k] = std::log(std::max(energy > kEnergyFloor ? static_cast<double>(partials[k]) / energy : 0.0, kShapeFloor));
	}
	return f;
}

std::vector<double> LikeModel::coordinates(const LikeVector& f) const {
	std::vector<double> z(transform.size(), 0.0);
	const LikeVector centred = minus(f, mean);
	for (size_t d = 0; d < transform.size(); ++d) {
		z[d] = dot(transform[d], centred);
	}
	return z;
}

double LikeModel::nearestSquared(const LikeVector& f) const {
	if (empty()) {
		return 0.0;
	}
	const std::vector<double> z = coordinates(f);
	double nearest = std::numeric_limits<double>::infinity();
	for (const std::vector<double>& like : likesAt) {
		double d2 = 0.0;
		for (size_t d = 0; d < z.size(); ++d) {
			d2 += (z[d] - like[d]) * (z[d] - like[d]);
		}
		nearest = std::min(nearest, d2);
	}
	return nearest;
}

double LikeModel::score(const LikeVector& f) const {
	if (empty()) {
		return 0.0;
	}
	return std::exp(-kScoreSharpness * nearestSquared(f) / typical);
}

LikeModel buildLikeModel(const LikeModelInputs& inputs) {
	LikeModel model;
	const size_t count = inputs.map.size();
	if (inputs.likes.empty() || count < 2) {
		return model;
	}

	// 1. THE MAP'S MEAN AND COVARIANCE, and the whitening it implies.
	for (const LikeVector& f : inputs.map) {
		for (int i = 0; i < D; ++i) {
			model.mean[i] += f[i];
		}
	}
	for (double& m : model.mean) {
		m /= static_cast<double>(count);
	}
	Matrix covariance {};
	for (const LikeVector& f : inputs.map) {
		const LikeVector c = minus(f, model.mean);
		for (int i = 0; i < D; ++i) {
			for (int j = 0; j < D; ++j) {
				covariance[i][j] += c[i] * c[j];
			}
		}
	}
	double trace = 0.0;
	for (int i = 0; i < D; ++i) {
		for (int j = 0; j < D; ++j) {
			covariance[i][j] /= static_cast<double>(count);
		}
		trace += covariance[i][i];
	}
	const double ridge = std::max(kRidgeShare * trace / D, 1e-12);
	Matrix vectors {};
	jacobi(covariance, vectors, D);
	Matrix whiten {};
	for (int i = 0; i < D; ++i) {
		for (int j = 0; j < D; ++j) {
			double sum = 0.0;
			for (int k = 0; k < D; ++k) {
				sum += vectors[i][k] * vectors[j][k] / std::sqrt(std::max(covariance[k][k], 0.0) + ridge);
			}
			whiten[i][j] = sum;
		}
	}

	// 2. EACH CONTROL'S DIRECTION, and how much of it is the same everywhere.
	struct Candidate {
		size_t control;
		double consistency;
		LikeVector direction;
	};
	std::vector<Candidate> candidates;
	model.consistency.assign(inputs.nudged.size(), -1.0);
	model.projected.assign(inputs.nudged.size(), false);
	for (size_t c = 0; c < inputs.nudged.size(); ++c) {
		if (inputs.nudged[c].size() != count) {
			continue;
		}
		LikeVector meanDelta {};
		double energy = 0.0;
		for (size_t p = 0; p < count; ++p) {
			const LikeVector delta = multiply(whiten, minus(inputs.nudged[c][p], inputs.map[p]));
			energy += dot(delta, delta);
			for (int i = 0; i < D; ++i) {
				meanDelta[i] += delta[i];
			}
		}
		energy /= static_cast<double>(count);
		for (double& m : meanDelta) {
			m /= static_cast<double>(count);
		}
		// A CONTROL THAT MOVED NOTHING — the engine does not read it — says nothing about any direction.
		if (energy < 1e-12) {
			continue;
		}
		const double consistency = dot(meanDelta, meanDelta) / energy;
		model.consistency[c] = consistency;
		if (consistency >= kLikeConsistentAt) {
			candidates.push_back({ c, consistency, meanDelta });
		}
	}
	// MOST CONSISTENT FIRST, and the cap is what stops the projection taking everything.
	std::stable_sort(candidates.begin(), candidates.end(),
	                 [](const Candidate& a, const Candidate& b) { return a.consistency > b.consistency; });
	std::vector<LikeVector> removed;
	for (const Candidate& candidate : candidates) {
		if (static_cast<int>(removed.size()) >= kLikeMaxRemoved) {
			break;
		}
		LikeVector v = candidate.direction;
		const double norm0 = std::sqrt(dot(v, v));
		for (double& x : v) {
			x /= norm0;
		}
		for (int pass = 0; pass < 2; ++pass) {
			for (const LikeVector& u : removed) {
				const double along = dot(v, u);
				for (int i = 0; i < D; ++i) {
					v[i] -= along * u[i];
				}
			}
		}
		const double norm = std::sqrt(dot(v, v));
		// ALREADY SPANNED by directions removed before it: nothing new to take away.
		if (norm < 1e-3) {
			continue;
		}
		for (double& x : v) {
			x /= norm;
		}
		removed.push_back(v);
		model.projected[candidate.control] = true;
	}
	model.removed = static_cast<int>(removed.size());

	// 3. WHAT IS LEFT: an orthonormal basis of the complement, from the projector's own eigenvectors.
	Matrix projector {};
	for (int i = 0; i < D; ++i) {
		for (int j = 0; j < D; ++j) {
			double p = i == j ? 1.0 : 0.0;
			for (const LikeVector& u : removed) {
				p -= u[i] * u[j];
			}
			projector[i][j] = p;
		}
	}
	Matrix basis {};
	jacobi(projector, basis, D);
	std::vector<LikeVector> residual; // rows: residual axis . whitened
	for (int k = 0; k < D; ++k) {
		if (projector[k][k] > 0.5) {
			LikeVector axis {};
			for (int i = 0; i < D; ++i) {
				axis[i] = basis[i][k];
			}
			// THE AXIS COMPOSED WITH THE WHITENING, so one row takes a raw difference to a residual coordinate.
			LikeVector row {};
			for (int j = 0; j < D; ++j) {
				double sum = 0.0;
				for (int i = 0; i < D; ++i) {
					sum += axis[i] * whiten[i][j];
				}
				row[j] = sum;
			}
			residual.push_back(row);
		}
	}
	const int k = static_cast<int>(residual.size());
	model.axes = k;
	if (k == 0) {
		return model;
	}

	// 4. WHAT THE LIKES AGREE ON: their spread along each residual axis against the map's, which whitening made one.
	std::vector<std::vector<double>> likes;
	for (const LikeVector& f : inputs.likes) {
		std::vector<double> y(static_cast<size_t>(k));
		const LikeVector centred = minus(f, model.mean);
		for (int d = 0; d < k; ++d) {
			y[static_cast<size_t>(d)] = dot(residual[static_cast<size_t>(d)], centred);
		}
		likes.push_back(std::move(y));
	}
	const auto n = static_cast<double>(likes.size());
	std::vector<double> likeMean(static_cast<size_t>(k), 0.0);
	for (const auto& y : likes) {
		for (int d = 0; d < k; ++d) {
			likeMean[static_cast<size_t>(d)] += y[static_cast<size_t>(d)] / n;
		}
	}
	Matrix spread {};
	for (const auto& y : likes) {
		for (int a = 0; a < k; ++a) {
			for (int b = 0; b < k; ++b) {
				spread[a][b] += (y[static_cast<size_t>(a)] - likeMean[static_cast<size_t>(a)])
				                * (y[static_cast<size_t>(b)] - likeMean[static_cast<size_t>(b)]);
			}
		}
	}
	for (int a = 0; a < k; ++a) {
		for (int b = 0; b < k; ++b) {
			spread[a][b] = (spread[a][b] + (a == b ? kLikePrior : 0.0)) / (n - 1.0 + kLikePrior);
		}
	}
	Matrix agree {};
	jacobi(spread, agree, k);
	std::vector<double> weight(static_cast<size_t>(k));
	double meanSquare = 0.0;
	for (int d = 0; d < k; ++d) {
		weight[static_cast<size_t>(d)] = 1.0 / std::sqrt(std::max(spread[d][d], kLikeSpreadFloor));
		meanSquare += weight[static_cast<size_t>(d)] * weight[static_cast<size_t>(d)] / k;
	}
	double sum2 = 0.0;
	double sum4 = 0.0;
	for (double& w : weight) {
		w /= std::sqrt(meanSquare);
		sum2 += w * w;
		sum4 += w * w * w * w;
	}
	model.effectiveDims = sum4 > 0.0 ? sum2 * sum2 / sum4 : 0.0;

	// 5. ONE TRANSFORM: weight x (like-agreement axis) x residual x whitening.
	model.transform.assign(static_cast<size_t>(k), LikeVector {});
	for (int d = 0; d < k; ++d) {
		LikeVector row {};
		for (int a = 0; a < k; ++a) {
			for (int j = 0; j < D; ++j) {
				row[j] += agree[a][d] * residual[static_cast<size_t>(a)][j];
			}
		}
		for (double& x : row) {
			x *= weight[static_cast<size_t>(d)];
		}
		model.transform[static_cast<size_t>(d)] = row;
	}
	for (const LikeVector& f : inputs.likes) {
		model.likesAt.push_back(model.coordinates(f));
	}
	// THE MAP'S OWN SCALE: the median pre-pass place's distance to its nearest like.
	std::vector<double> distances;
	for (const LikeVector& f : inputs.map) {
		distances.push_back(model.nearestSquared(f));
	}
	std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(distances.size() / 2),
	                 distances.end());
	const double median = distances[distances.size() / 2];
	model.typical = median > 1e-12 ? median : 1.0;
	return model;
}

LikeRegion likeRegionFor(std::span<const WarpPoint> likes) {
	LikeRegion region;
	if (likes.empty()) {
		return region;
	}
	for (const WarpPoint& like : likes) {
		region.centreX += like.x / static_cast<double>(likes.size());
		region.centreY += like.y / static_cast<double>(likes.size());
	}
	double reach = 0.0;
	for (const WarpPoint& like : likes) {
		reach = std::max({ reach, std::abs(like.x - region.centreX), std::abs(like.y - region.centreY) });
	}
	region.half = std::max(kLikeRegionHalfMin, reach + kLikeRegionPad);
	return region;
}
} // namespace feathers::ui
