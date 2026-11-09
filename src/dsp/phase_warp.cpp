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

#include "dsp/phase_warp.h"

#include "character/phi_triangle.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace feathers {

namespace {

namespace phi = owlet::dsp::phi;

double wrapDiff(double a, double b) {
	double d = a - b;
	d -= std::floor(d + 0.5);
	return d;
}
double wrap01(double a) {
	return a - std::floor(a);
}

/// THE THREAD'S CONTEXT: the warp the sound being built names, a recorder if one listens, and the kind being read.
struct Context {
	const PhaseWarp* warp = nullptr;
	std::array<float, kAngleSites>* record = nullptr;
	std::array<uint16_t, kAngleKinds> recorded {};
	int kind = -1;
	uint16_t index = 0;
};
thread_local Context context;

struct Hook final : phi::AngleHook {
	float at(float wrapped) noexcept override {
		if (context.kind < 0) {
			return wrapped;
		}
		const auto k = static_cast<size_t>(context.kind);
		const uint16_t i = context.index++;
		if (i >= kAngleSitesOf[k]) {
			return wrapped;
		}
		const size_t site = kAngleOffsetOf[k] + i;
		if (context.record != nullptr) {
			(*context.record)[site] = wrapped;
			context.recorded[k] = std::max<uint16_t>(context.recorded[k], static_cast<uint16_t>(i + 1));
			return wrapped;
		}
		return context.warp != nullptr ? context.warp->at(site, wrapped) : wrapped;
	}
};
thread_local Hook hook;

/// ONE CIRCLE: w0 uniform + wrapped Cauchys fitted by circular EM, K by BIC (1..4), each width floored by the k-NN rule,
/// then the mixture's CDF inverted to the LUT. The phase-warp study's fit (tests/study_phase_warp_models.h), at 1024 bins.
void fitCircle(std::span<const double> x, double strength, const PhaseWarpParams& params, PhaseWarpSite& out) {
	const size_t n = x.size();
	out.live = false;
	if (n == 0) {
		return;
	}
	constexpr double kTau = 6.283185307179586;
	const size_t knn = std::min<size_t>(static_cast<size_t>(std::max(params.neighbours, 1)), n - 1);
	double widthSum = 0.0;
	std::vector<double> dists;
	for (size_t i = 0; i < n; ++i) {
		dists.clear();
		for (size_t j = 0; j < n; ++j) {
			if (j != i) {
				dists.push_back(std::abs(wrapDiff(x[i], x[j])));
			}
		}
		std::sort(dists.begin(), dists.end());
		widthSum += knn > 0 ? std::max(dists[knn - 1], params.widthFloor) : 0.05;
	}
	const double h = std::clamp(widthSum / static_cast<double>(n), 0.003, 0.2);
	const double rhoCap = std::exp(-0.5 * (kTau * h) * (kTau * h));
	struct Comp {
		double w;
		double mu;
		double rho;
	};
	const auto pdf = [&](double t, const Comp& c) {
		return (1.0 - c.rho * c.rho) / (1.0 + c.rho * c.rho - 2.0 * c.rho * std::cos(kTau * (t - c.mu)));
	};
	std::vector<Comp> best;
	double bestScore = 1e300;
	std::vector<double> r;
	for (int K = 1; K <= std::min<int>(std::max(params.maxComponents, 1), static_cast<int>(n)); ++K) {
		std::vector<size_t> centres { 0 };
		while (static_cast<int>(centres.size()) < K) {
			size_t pick = 0;
			double far = -1.0;
			for (size_t i = 0; i < n; ++i) {
				double near = 1e300;
				for (const size_t c : centres) {
					near = std::min(near, std::abs(wrapDiff(x[i], x[c])));
				}
				if (near > far) {
					far = near;
					pick = i;
				}
			}
			centres.push_back(pick);
		}
		std::vector<Comp> comps;
		for (const size_t c : centres) {
			comps.push_back({ 1.0 / K, x[c], rhoCap });
		}
		double ll = 0.0;
		r.assign(n * static_cast<size_t>(K), 0.0);
		for (int iter = 0; iter < params.iterations; ++iter) {
			ll = 0.0;
			for (size_t i = 0; i < n; ++i) {
				double sum = 0.0;
				for (int k = 0; k < K; ++k) {
					r[i * K + k] = comps[static_cast<size_t>(k)].w * pdf(x[i], comps[static_cast<size_t>(k)]);
					sum += r[i * K + k];
				}
				for (int k = 0; k < K; ++k) {
					r[i * K + k] /= std::max(sum, 1e-300);
				}
				ll += std::log(std::max(sum, 1e-300));
			}
			for (int k = 0; k < K; ++k) {
				double nk = 0.0;
				double cs = 0.0;
				double sn = 0.0;
				for (size_t i = 0; i < n; ++i) {
					nk += r[i * K + k];
					cs += r[i * K + k] * std::cos(kTau * x[i]);
					sn += r[i * K + k] * std::sin(kTau * x[i]);
				}
				Comp& c = comps[static_cast<size_t>(k)];
				c.w = nk / static_cast<double>(n);
				if (nk > 1e-9) {
					c.mu = wrap01(std::atan2(sn, cs) / kTau);
					c.rho = std::min(std::sqrt(cs * cs + sn * sn) / nk, rhoCap);
				}
			}
		}
		const double score = -2.0 * ll + (3.0 * K - 1.0) * std::log(static_cast<double>(n));
		if (score < bestScore) {
			bestScore = score;
			best = comps;
		}
	}
	const double w0 = params.prior / (params.prior + strength * static_cast<double>(n));
	constexpr int kGrid = 1024;
	std::array<double, kGrid + 1> cdf {};
	for (int i = 0; i < kGrid; ++i) {
		const double t = (i + 0.5) / kGrid;
		double p = w0;
		for (const Comp& c : best) {
			p += (1.0 - w0) * c.w * pdf(t, c);
		}
		cdf[static_cast<size_t>(i + 1)] = cdf[static_cast<size_t>(i)] + p / kGrid;
	}
	for (double& v : cdf) {
		v /= cdf.back();
	}
	for (int k = 0; k < kPhaseWarpLut; ++k) {
		const double want = static_cast<double>(k) / (kPhaseWarpLut - 1);
		const auto it = std::lower_bound(cdf.begin(), cdf.end(), want);
		const size_t b = std::clamp<size_t>(static_cast<size_t>(it - cdf.begin()), 1, kGrid);
		const double c0 = cdf[b - 1];
		const double c1 = cdf[b];
		out.lut[static_cast<size_t>(k)] =
		    static_cast<float>((static_cast<double>(b - 1) + (c1 > c0 ? (want - c0) / (c1 - c0) : 0.0)) / kGrid);
	}
	out.lut.front() = 0.0f;
	out.lut.back() = 1.0f;
	out.live = true;
}

double lutAt(const PhaseWarpSite& s, double t) {
	const double at = std::clamp(t, 0.0, 1.0) * (kPhaseWarpLut - 1);
	const int k = std::min(static_cast<int>(at), kPhaseWarpLut - 2);
	const double f = at - k;
	return (1.0 - f) * s.lut[static_cast<size_t>(k)] + f * s.lut[static_cast<size_t>(k + 1)];
}

} // namespace

void fitPhaseWarpSite(std::span<const double> angles, double strength, const PhaseWarpParams& params, PhaseWarpSite& out) {
	fitCircle(angles, strength, params, out);
}

double phaseWarpLutAt(const PhaseWarpSite& site, double theta) noexcept {
	return lutAt(site, theta);
}

void pinPhaseWarpSite(PhaseWarpSite& site, float at, float heard) noexcept {
	site.pinned = true;
	site.pinAt = at;
	site.pinTo = heard;
	site.pinShift = static_cast<float>(wrapDiff(heard, lutAt(site, at)));
}

float phaseWarpSiteAt(const PhaseWarpSite& s, float theta) noexcept {
	if (!s.live) {
		return theta;
	}
	// THE PIN, EXACT: the newest like's own angle maps to what it was heard as, to the float.
	if (s.pinned && theta == s.pinAt) {
		return s.pinTo;
	}
	double out = lutAt(s, theta);
	if (s.pinned) {
		const double d = wrapDiff(theta, s.pinAt) / kPhaseWarpPinWidth;
		out += std::exp(-0.5 * d * d) * s.pinShift;
	}
	const auto f = static_cast<float>(wrap01(out));
	return f < 1.0f ? f : 0.0f;
}

float PhaseWarp::at(size_t site, float theta) const noexcept {
	if (site >= kAngleSites) {
		return theta;
	}
	return phaseWarpSiteAt(sites[site], theta);
}

uint64_t PhaseWarp::contentKey() const noexcept {
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](const void* p, size_t bytes) {
		const auto* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < bytes; ++i) {
			h = (h ^ b[i]) * 1099511628211ull;
		}
	};
	mix(&likes, sizeof(likes));
	for (const PhaseWarpSite& s : sites) {
		mix(&s.live, sizeof(s.live));
		if (s.live) {
			mix(s.lut.data(), sizeof(s.lut));
			mix(&s.pinned, sizeof(s.pinned));
			mix(&s.pinAt, sizeof(s.pinAt));
			mix(&s.pinTo, sizeof(s.pinTo));
			mix(&s.pinShift, sizeof(s.pinShift));
		}
	}
	return h;
}

PhaseWarp buildPhaseWarp(std::span<const std::array<float, kAngleSites>> likes, std::span<const bool> liveSites, double strength,
                         const PhaseWarpParams& params) {
	// THE CHAIN RESUMED: the warp after the first m likes is a pure function of them, so the last derivation's is kept and a
	// list that extends it pays only for its new likes (one like at the heart is one step, not the whole chain again).
	struct Memo {
		std::vector<std::array<float, kAngleSites>> likes;
		std::vector<bool> live;
		double strength = 0.0;
		PhaseWarpParams params {};
		std::unique_ptr<PhaseWarp> warp;
	};
	thread_local Memo memo;
	const std::vector<bool> live(liveSites.begin(), liveSites.end());
	PhaseWarp warp;
	size_t start = 1;
	if (memo.warp != nullptr && memo.strength == strength && memo.params == params && memo.live == live && memo.likes.size() <= likes.size()
	    && std::equal(memo.likes.begin(), memo.likes.end(), likes.begin())) {
		warp = *memo.warp;
		start = memo.likes.size() + 1;
	}
	std::vector<double> x;
	for (size_t n = start; n <= likes.size(); ++n) {
		const std::array<float, kAngleSites>& newest = likes[n - 1];
		PhaseWarp next;
		next.likes = static_cast<uint16_t>(n);
		for (size_t site = 0; site < kAngleSites; ++site) {
			if (site >= liveSites.size() || !liveSites[site]) {
				continue;
			}
			x.clear();
			for (size_t i = 0; i < n; ++i) {
				x.push_back(likes[i][site]);
			}
			PhaseWarpSite& s = next.sites[site];
			fitCircle(x, strength, params, s);
			// THE NEWEST LIKE'S PIN: its angle maps to what it was heard as under the warp before it.
			const float heard = warp.at(site, newest[site]);
			pinPhaseWarpSite(s, newest[site], heard);
		}
		warp = next;
	}
	memo.likes.assign(likes.begin(), likes.end());
	memo.live = live;
	memo.strength = strength;
	memo.params = params;
	memo.warp = std::make_unique<PhaseWarp>(warp);
	return warp;
}

AngleWarpScope::AngleWarpScope(const PhaseWarp* warp) noexcept : previous_(context.warp) {
	context.warp = warp != nullptr && !warp->identity() ? warp : nullptr;
}
AngleWarpScope::~AngleWarpScope() {
	context.warp = previous_;
}

AngleKindScope::AngleKindScope(AngleKind kind) noexcept
    : previousHook_(phi::angleHook), previousKind_(context.kind), previousIndex_(context.index) {
	if (context.warp != nullptr || context.record != nullptr) {
		phi::angleHook = &hook;
		context.kind = static_cast<int>(kind);
		context.index = 0;
	}
}
AngleKindScope::~AngleKindScope() {
	phi::angleHook = static_cast<phi::AngleHook*>(previousHook_);
	context.kind = previousKind_;
	context.index = previousIndex_;
}

AngleRecorder::AngleRecorder(std::array<float, kAngleSites>& out) noexcept : previous_(context.record) {
	out.fill(0.0f);
	context.record = &out;
	context.recorded = {};
}
AngleRecorder::~AngleRecorder() {
	context.record = previous_;
}
std::array<uint16_t, kAngleKinds> AngleRecorder::counts() const noexcept {
	return context.recorded;
}

} // namespace feathers
