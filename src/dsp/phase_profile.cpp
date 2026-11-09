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

#include "dsp/phase_profile.h"

#include <algorithm>
#include <limits>
#include <numbers>

namespace feathers::partials {

void schroederPhases(const float* amp, int count, float* phase) noexcept {
	count = std::clamp(count, 0, kMaxPartials);
	double total = 0.0;
	for (int k = 0; k < count; ++k) {
		const double a = static_cast<double>(amp[k]);
		if (std::isfinite(a)) {
			total += a * a;
		}
	}
	// THE BANK'S OWN LAW (PartialBank::reset(power, count)), in turns reduced before the angle: φ_k = −2π·(k·S0 − S1), S0 and
	// S1 the running sums of the power shares and of k times them over the partials below k. No power: Newman's flat law.
	double s0 = 0.0, s1 = 0.0;
	for (int k = 0; k < count; ++k) {
		if (!(total > 0.0)) {
			phase[k] = static_cast<float>(std::numbers::pi * k * k / std::max(1, count));
			continue;
		}
		double turns = static_cast<double>(k) * s0 - s1;
		turns -= std::floor(turns);
		phase[k] = static_cast<float>(-2.0 * std::numbers::pi * turns);
		const double a = static_cast<double>(amp[k]);
		if (std::isfinite(a)) {
			const double p = a * a / total;
			s0 += p;
			s1 += static_cast<double>(k) * p;
		}
	}
}

double periodPeak(const float* amp, const float* phase, int count, int points) {
	double peak = 0.0;
	for (int m = 0; m < points; ++m) {
		const double theta = 2.0 * std::numbers::pi * m / points;
		double x = 0.0;
		for (int k = 0; k < count; ++k) {
			x += static_cast<double>(amp[k]) * std::sin((k + 1) * theta + static_cast<double>(phase[k]));
		}
		peak = std::max(peak, std::abs(x));
	}
	return peak;
}

PhaseOptimiser::PhaseOptimiser() {
	const auto n = static_cast<size_t>(sizeFor(kMaxPartials));
	re_.assign(n, 0.0);
	im_.assign(n, 0.0);
	amp_.assign(static_cast<size_t>(kMaxPartials), 0.0);
	phase_.assign(static_cast<size_t>(kMaxPartials), 0.0);
	best_.assign(static_cast<size_t>(kMaxPartials), 0.0);
}

int PhaseOptimiser::sizeFor(int count) noexcept {
	int n = 512;
	while (n < 8 * (count + 1)) {
		n *= 2;
	}
	return n;
}

void PhaseOptimiser::fft(double* re, double* im, int n, bool inverse) noexcept {
	// IN PLACE, RADIX 2, UNSCALED both ways: the caller writes the spectrum at the scale that makes the inverse the signal.
	for (int i = 1, j = 0; i < n; ++i) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(re[i], re[j]);
			std::swap(im[i], im[j]);
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi / len;
		const double wr = std::cos(angle), wi = std::sin(angle);
		for (int i = 0; i < n; i += len) {
			double cr = 1.0, ci = 0.0;
			for (int k = 0; k < len / 2; ++k) {
				const int a = i + k, b = i + k + len / 2;
				const double tr = re[b] * cr - im[b] * ci;
				const double ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				const double next = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = next;
			}
		}
	}
}

void PhaseOptimiser::optimise(const float* amp, int count, PhaseProfile& out, int rounds) {
	count = std::clamp(count, 0, kMaxPartials);
	double rss = 0.0;
	for (int k = 0; k < count; ++k) {
		const double a = std::isfinite(amp[k]) ? std::abs(static_cast<double>(amp[k])) : 0.0;
		amp_[static_cast<size_t>(k)] = a;
		rss += a * a;
	}
	rss = std::sqrt(rss);
	out.count = count;
	if (!(rss > 0.0) || count == 0) {
		out.amp.fill(0.0f);
		out.phase.fill(0.0f);
		out.peakPerRss = 0.0f;
		out.crestDb = 0.0f;
		return;
	}
	for (int k = 0; k < count; ++k) {
		amp_[static_cast<size_t>(k)] /= rss; // unit RSS: the peak below is then the peak PER unit RSS
		out.amp[static_cast<size_t>(k)] = static_cast<float>(amp_[static_cast<size_t>(k)]);
	}
	std::fill(out.amp.begin() + count, out.amp.end(), 0.0f);
	// FROM THE CLOSED FORM, so the worst this can return is what the note-on law already gives.
	schroederPhases(out.amp.data(), count, out.phase.data());
	for (int k = 0; k < count; ++k) {
		phase_[static_cast<size_t>(k)] = static_cast<double>(out.phase[static_cast<size_t>(k)]);
	}
	const int n = sizeFor(count);
	double* re = re_.data();
	double* im = im_.data();
	double bestPeak = std::numeric_limits<double>::infinity();
	for (int round = 0; round <= rounds; ++round) {
		// THE PERIOD: A sin(hθ + φ) = A cos(hθ + φ − π/2), so bin h carries (A/2)·e^{i(φ − π/2)} and bin n − h its conjugate,
		// and the unscaled inverse is the signal itself.
		std::fill_n(re, n, 0.0);
		std::fill_n(im, n, 0.0);
		for (int k = 0; k < count; ++k) {
			const int h = k + 1;
			const double half = 0.5 * amp_[static_cast<size_t>(k)];
			const double theta = phase_[static_cast<size_t>(k)] - 0.5 * std::numbers::pi;
			re[h] = half * std::cos(theta);
			im[h] = half * std::sin(theta);
			re[n - h] = re[h];
			im[n - h] = -im[h];
		}
		fft(re, im, n, true);
		double peak = 0.0;
		for (int m = 0; m < n; ++m) {
			peak = std::max(peak, std::abs(re[m]));
		}
		if (peak < bestPeak) {
			bestPeak = peak;
			std::copy_n(phase_.begin(), count, best_.begin());
		}
		if (round == rounds) {
			break;
		}
		// CLIP AT 0.9 OF THE PEAK, then take each harmonic's phase back from the clipped period (its amplitude is kept).
		const double level = 0.9 * peak;
		for (int m = 0; m < n; ++m) {
			re[m] = std::clamp(re[m], -level, level);
			im[m] = 0.0;
		}
		fft(re, im, n, false);
		for (int k = 0; k < count; ++k) {
			const int h = k + 1;
			phase_[static_cast<size_t>(k)] = std::atan2(im[h], re[h]) + 0.5 * std::numbers::pi;
		}
	}
	for (int k = 0; k < count; ++k) {
		double p = best_[static_cast<size_t>(k)];
		p -= 2.0 * std::numbers::pi * std::floor((p + std::numbers::pi) / (2.0 * std::numbers::pi));
		out.phase[static_cast<size_t>(k)] = static_cast<float>(p);
	}
	std::fill(out.phase.begin() + count, out.phase.end(), 0.0f);
	out.peakPerRss = static_cast<float>(bestPeak);
	out.crestDb = static_cast<float>(20.0 * std::log10(bestPeak * std::numbers::sqrt2)); // RMS of a unit-RSS sum: 1/√2
}

float profileDistance(const PhaseProfile& p, const float* amp, int count) noexcept {
	count = std::clamp(count, 0, kMaxPartials);
	double norm = 0.0;
	for (int k = 0; k < count; ++k) {
		norm += static_cast<double>(amp[k]) * static_cast<double>(amp[k]);
	}
	if (!(norm > 0.0) || p.count == 0) {
		return 2.0f;
	}
	norm = std::sqrt(norm);
	const int span = std::max(count, p.count);
	double d = 0.0;
	for (int k = 0; k < span; ++k) {
		const double a = k < count ? static_cast<double>(amp[k]) / norm : 0.0;
		const double q = static_cast<double>(p.amp[static_cast<size_t>(k)]);
		d += (a - q) * (a - q);
	}
	return static_cast<float>(std::sqrt(d));
}

void PhaseRequestSlot::post(uint64_t serial, const float* amp, int count) noexcept {
	count = std::clamp(count, 0, kMaxPartials);
	const uint32_t s = sequence_.load(std::memory_order_relaxed);
	sequence_.store(s + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	serial_.store(serial, std::memory_order_relaxed);
	count_.store(count, std::memory_order_relaxed);
	for (int k = 0; k < count; ++k) {
		amp_[static_cast<size_t>(k)].store(amp[k], std::memory_order_relaxed);
	}
	std::atomic_thread_fence(std::memory_order_release);
	sequence_.store(s + 2, std::memory_order_release);
	posted_.fetch_add(1, std::memory_order_relaxed);
}

bool PhaseRequestSlot::take(Request& out) noexcept {
	const uint32_t before = sequence_.load(std::memory_order_acquire);
	if ((before & 1u) != 0u) {
		return false;
	}
	const uint64_t serial = serial_.load(std::memory_order_relaxed);
	if (serial == 0 || serial == taken_) {
		return false;
	}
	const int count = std::clamp(count_.load(std::memory_order_relaxed), 0, kMaxPartials);
	for (int k = 0; k < count; ++k) {
		out.amp[static_cast<size_t>(k)] = amp_[static_cast<size_t>(k)].load(std::memory_order_relaxed);
	}
	std::atomic_thread_fence(std::memory_order_acquire);
	if (sequence_.load(std::memory_order_relaxed) != before) {
		return false; // the writer moved under the read: the next poll takes the newer one
	}
	out.serial = serial;
	out.count = count;
	taken_ = serial;
	takenCount_.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void PhaseProfileStore::publish(const PhaseProfile& p) noexcept {
	const uint32_t s = sequence_.load(std::memory_order_relaxed);
	sequence_.store(s + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	count_.store(p.count, std::memory_order_relaxed);
	peakPerRss_.store(p.peakPerRss, std::memory_order_relaxed);
	crestDb_.store(p.crestDb, std::memory_order_relaxed);
	for (size_t k = 0; k < static_cast<size_t>(kMaxPartials); ++k) {
		amp_[k].store(p.amp[k], std::memory_order_relaxed);
		phase_[k].store(p.phase[k], std::memory_order_relaxed);
	}
	serial_.store(p.serial, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	sequence_.store(s + 2, std::memory_order_release);
}

bool PhaseProfileStore::read(PhaseProfile& out) const noexcept {
	const uint32_t before = sequence_.load(std::memory_order_acquire);
	if ((before & 1u) != 0u) {
		return false;
	}
	const uint64_t serial = serial_.load(std::memory_order_relaxed);
	if (serial == 0) {
		return false;
	}
	out.count = std::clamp(count_.load(std::memory_order_relaxed), 0, kMaxPartials);
	out.peakPerRss = peakPerRss_.load(std::memory_order_relaxed);
	out.crestDb = crestDb_.load(std::memory_order_relaxed);
	for (size_t k = 0; k < static_cast<size_t>(kMaxPartials); ++k) {
		out.amp[k] = amp_[k].load(std::memory_order_relaxed);
		out.phase[k] = phase_[k].load(std::memory_order_relaxed);
	}
	std::atomic_thread_fence(std::memory_order_acquire);
	if (sequence_.load(std::memory_order_relaxed) != before) {
		return false;
	}
	out.serial = serial;
	return true;
}

} // namespace feathers::partials
