/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/spectral/spectral_envelope.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::spectral {

double envelopeGridHz(int g) noexcept {
	return kEnvelopeLowHz * std::exp2(kEnvelopeOctaves * static_cast<double>(g) / static_cast<double>(kEnvelopePoints - 1));
}

void EnvelopeEstimator::prepare(int maxBins) {
	const auto n = static_cast<size_t>(std::max(maxBins, 1));
	logHz_.assign(n, 0.0);
	binCell_.assign(n, 0);
	binFrac_.assign(n, 0.0);
	count_.assign(n + 1, 0.0);
	sumX_.assign(n + 1, 0.0);
	sumV_.assign(n + 1, 0.0);
	sumXV_.assign(n + 1, 0.0);
	db_.assign(n, 0.0f);
	cachedBins_ = 0;
	cachedBinHz_ = 0.0;
}

void EnvelopeEstimator::ensureBins(int bins, double binHz) noexcept {
	if (bins == cachedBins_ && binHz == cachedBinHz_) {
		return;
	}
	cachedBins_ = bins;
	cachedBinHz_ = binHz;
	first_ = 0;
	while (first_ < bins && static_cast<double>(first_) * binHz < kEnvelopeLowHz) {
		++first_;
	}
	const double lowLog = std::log2(kEnvelopeLowHz);
	const double perPoint = kEnvelopeOctaves / static_cast<double>(kEnvelopePoints - 1);
	count_[0] = 0.0;
	sumX_[0] = 0.0;
	for (int i = 0; i < bins; ++i) {
		const auto u = static_cast<size_t>(i);
		const double hz = static_cast<double>(i) * binHz;
		logHz_[u] = i >= first_ ? std::log2(hz) : 0.0;
		const double pos = hz > kEnvelopeLowHz ? (std::log2(hz) - lowLog) / perPoint : 0.0;
		const double clamped = std::clamp(pos, 0.0, static_cast<double>(kEnvelopePoints - 1));
		binCell_[u] = std::min(static_cast<int>(clamped), kEnvelopePoints - 2);
		binFrac_[u] = clamped - static_cast<double>(binCell_[u]);
		const bool in = i >= first_;
		count_[u + 1] = count_[u] + (in ? 1.0 : 0.0);
		sumX_[u + 1] = sumX_[u] + (in ? logHz_[u] : 0.0);
	}
}

void EnvelopeEstimator::estimate(const float* mag, int bins, double binHz, double bands,
                                 std::array<float, kEnvelopePoints>& gridDb, bool energy) noexcept {
	bins = std::min(bins, static_cast<int>(logHz_.size()));
	ensureBins(bins, binHz);
	const int first = first_;
	const double halfWidth = kEnvelopeOctaves / std::clamp(bands, kMinBands, kMaxBands);
	const double lowLog = std::log2(kEnvelopeLowHz);
	if (first >= bins) {
		gridDb.fill(kEnvelopeFloorDb);
		return;
	}
	// THE BINS' LEVELS (dB, or power for the energy mean), and their PREFIX SUMS — of the level and of log2 Hz times the level —
	// so each grid point's triangle-weighted sum is a handful of differences, not a loop over its kernel (Z1b).
	sumV_[static_cast<size_t>(first)] = 0.0;
	sumXV_[static_cast<size_t>(first)] = 0.0;
	for (int i = first; i < bins; ++i) {
		const auto u = static_cast<size_t>(i);
		const double m = std::max(static_cast<double>(mag[i]), 1e-6);
		db_[u] = static_cast<float>(6.020599913279624 * fastLog2(m)); // 20 log10 m
		const double v = energy ? std::max(m * m, 1e-12) : static_cast<double>(db_[u]);
		sumV_[u + 1] = sumV_[u] + v;
		sumXV_[u + 1] = sumXV_[u] + logHz_[u] * v;
	}
	// EACH GRID POINT: the triangle-weighted mean of the levels within halfWidth octaves, w = 1 - |x - c| / h. Over the left half
	// [lo, mid) the weight is (1 - c/h) + x/h, over the right half [mid, hi) it is (1 + c/h) - x/h, so both halves' sums are
	// prefix differences. The bins are in order and the grid rises, so the three edges only slide up.
	int lo = first;
	int mid = first;
	int hi = first;
	const auto diff = [](const std::vector<double>& p, int a, int b) { return p[static_cast<size_t>(b)] - p[static_cast<size_t>(a)]; };
	for (int g = 0; g < kEnvelopePoints; ++g) {
		const double c = lowLog + kEnvelopeOctaves * static_cast<double>(g) / static_cast<double>(kEnvelopePoints - 1);
		while (lo < bins && logHz_[static_cast<size_t>(lo)] < c - halfWidth) {
			++lo;
		}
		mid = std::max(mid, lo);
		while (mid < bins && logHz_[static_cast<size_t>(mid)] < c) {
			++mid;
		}
		hi = std::max(hi, mid);
		while (hi < bins && logHz_[static_cast<size_t>(hi)] - c <= halfWidth) {
			++hi;
		}
		const double a = 1.0 - c / halfWidth;
		const double b = 1.0 + c / halfWidth;
		const double weight = a * diff(count_, lo, mid) + diff(sumX_, lo, mid) / halfWidth + b * diff(count_, mid, hi)
		                      - diff(sumX_, mid, hi) / halfWidth;
		const double sum = a * diff(sumV_, lo, mid) + diff(sumXV_, lo, mid) / halfWidth + b * diff(sumV_, mid, hi)
		                   - diff(sumXV_, mid, hi) / halfWidth;
		if (hi > lo && weight > 1e-9) {
			gridDb[static_cast<size_t>(g)] =
			    static_cast<float>(energy ? 3.010299956639812 * fastLog2(std::max(sum / weight, 1e-12)) : sum / weight);
			continue;
		}
		// NO BIN UNDER THE KERNEL (a low grid point against wide bins): the nearest bin's level.
		int nearest = first;
		double best = 1e9;
		for (int i = first; i < bins; ++i) {
			const double d = std::fabs(logHz_[static_cast<size_t>(i)] - c);
			if (d < best) {
				best = d;
				nearest = i;
			}
			else {
				break; // the bins are in order: past the nearest, they only get further
			}
		}
		gridDb[static_cast<size_t>(g)] = db_[static_cast<size_t>(nearest)];
	}
}

void EnvelopeEstimator::toBinsFast(const std::array<float, kEnvelopePoints>& gridDb, int bins, double binHz, float* out) noexcept {
	bins = std::min(bins, static_cast<int>(logHz_.size()));
	ensureBins(bins, binHz);
	for (int i = 0; i < bins; ++i) {
		const auto u = static_cast<size_t>(i);
		const auto g = static_cast<size_t>(binCell_[u]);
		const double t = binFrac_[u];
		const double db = static_cast<double>(gridDb[g]) * (1.0 - t) + static_cast<double>(gridDb[g + 1]) * t;
		out[i] = db <= static_cast<double>(kEnvelopeFloorDb) ? 0.0f : static_cast<float>(fastExp2(db * 0.16609640474436813));
	}
}
void EnvelopeEstimator::toBins(const std::array<float, kEnvelopePoints>& gridDb, int bins, double binHz, float* out) noexcept {
	const double lowLog = std::log2(kEnvelopeLowHz);
	const double perPoint = kEnvelopeOctaves / static_cast<double>(kEnvelopePoints - 1);
	for (int i = 0; i < bins; ++i) {
		const double hz = static_cast<double>(i) * binHz;
		const double pos = hz > kEnvelopeLowHz ? (std::log2(hz) - lowLog) / perPoint : 0.0;
		const double clamped = std::clamp(pos, 0.0, static_cast<double>(kEnvelopePoints - 1));
		const auto g = std::min(static_cast<int>(clamped), kEnvelopePoints - 2);
		const double t = clamped - static_cast<double>(g);
		const double db = static_cast<double>(gridDb[static_cast<size_t>(g)]) * (1.0 - t)
		                  + static_cast<double>(gridDb[static_cast<size_t>(g + 1)]) * t;
		out[i] = db <= static_cast<double>(kEnvelopeFloorDb) ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
	}
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

double EnvelopeEstimator::sampleDb(const std::array<float, kEnvelopePoints>& gridDb, double hz) noexcept {
	const double perPoint = kEnvelopeOctaves / static_cast<double>(kEnvelopePoints - 1);
	const double pos = hz > kEnvelopeLowHz ? (std::log2(hz) - std::log2(kEnvelopeLowHz)) / perPoint : 0.0;
	const double clamped = std::clamp(pos, 0.0, static_cast<double>(kEnvelopePoints - 1));
	const auto g = std::min(static_cast<int>(clamped), kEnvelopePoints - 2);
	const double t = clamped - static_cast<double>(g);
	return static_cast<double>(gridDb[static_cast<size_t>(g)]) * (1.0 - t) + static_cast<double>(gridDb[static_cast<size_t>(g + 1)]) * t;
}

} // namespace feathers::fx::spectral
