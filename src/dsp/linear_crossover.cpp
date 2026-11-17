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

#include "dsp/linear_crossover.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace feathers::dsp {

namespace {

float* allocFloats(size_t count) {
	const size_t n = std::max<size_t>(count, 1);
	auto* p = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
	std::fill(p, p + n, 0.0f);
	return p;
}

void freeFloats(float*& p) noexcept {
	pffft_aligned_free(p);
	p = nullptr;
}

/// The zeroth-order modified Bessel function, by its power series (the window is built at prepare only).
double besselI0(double x) noexcept {
	double sum = 1.0, term = 1.0;
	const double q = x * x * 0.25;
	for (int k = 1; k < 64; ++k) {
		term *= q / (static_cast<double>(k) * static_cast<double>(k));
		sum += term;
		if (term < sum * 1.0e-17) {
			break;
		}
	}
	return sum;
}

} // namespace

void LinearFirBank::release() noexcept {
	for (auto& set : kernel_) {
		for (float*& k : set) {
			freeFloats(k);
		}
	}
	for (auto* group : { &inputs_, &previous_, &fdl_, &accum_, &fadeAccum_, &delay_ }) {
		for (float*& p : *group) {
			freeFloats(p);
		}
	}
	for (auto& o : out_) {
		for (float*& p : o) {
			freeFloats(p);
		}
	}
	for (float** p : { &window2B_, &work2B_, &fir_, &spec_, &specWork_, &windowN_ }) {
		freeFloats(*p);
	}
	delete[] half_;
	delete[] halfLo_;
	delete[] kaiser_;
	half_ = halfLo_ = kaiser_ = nullptr;
	if (small_ != nullptr) {
		pffft_destroy_setup(small_);
		small_ = nullptr;
	}
	if (big_ != nullptr) {
		pffft_destroy_setup(big_);
		big_ = nullptr;
	}
}

void LinearFirBank::prepare(double sampleRate, size_t outputs) {
	release();
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	outputs_ = std::min(outputs, kMaxOutputs);
	n_ = linearCrossoverTaps(sampleRate_);
	block_ = linearCrossoverBlock(sampleRate_);
	parts_ = n_ / block_;
	delayLen_ = n_ / 2 + block_;
	const size_t twoB = 2 * block_;
	small_ = pffft_new_setup(static_cast<int>(twoB), PFFFT_REAL);
	for (auto& set : kernel_) {
		for (size_t o = 0; o < outputs_; ++o) {
			set[o] = allocFloats(parts_ * twoB);
		}
	}
	for (size_t c = 0; c < 2; ++c) {
		inputs_[c] = allocFloats(block_);
		previous_[c] = allocFloats(block_);
		fdl_[c] = allocFloats(parts_ * twoB);
		accum_[c] = allocFloats(twoB);
		fadeAccum_[c] = allocFloats(twoB);
		delay_[c] = allocFloats(delayLen_);
		for (size_t o = 0; o < outputs_; ++o) {
			out_[o][c] = allocFloats(block_);
		}
	}
	window2B_ = allocFloats(twoB);
	work2B_ = allocFloats(twoB);
	fir_ = allocFloats(n_);
	spec_ = allocFloats(n_);
	specWork_ = allocFloats(n_);
	windowN_ = allocFloats(kLinearSteepSteps * n_);
	big_ = pffft_new_setup(static_cast<int>(n_), PFFFT_REAL);
	half_ = new double[n_ / 2 + 1]();
	halfLo_ = new double[n_ / 2 + 1]();
	kaiser_ = new double[kLinearSteepSteps * (n_ / 2 + 1)]();
	const double i0 = besselI0(kLinearKaiserBeta);
	const double i0Half = besselI0(kLinearHalfKaiserBeta);
	// ONE WINDOW A STEEPNESS: the same Kaiser shape over a half-length of N/2, N/4, N/8 about the centre, zero outside. The kernel stays N
	// taps centred at N/2, so a gentler kernel is a zero-padded short one and the delay does not move.
	for (size_t st = 0; st < kLinearSteepSteps; ++st) {
		const size_t reach = (n_ / 2) >> st;
		for (size_t m = 0; m <= n_ / 2; ++m) {
			const double r = static_cast<double>(m) / static_cast<double>(reach);
			kaiser_[st * (n_ / 2 + 1) + m] = m < reach ? besselI0(kLinearKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0 : 0.0;
		}
		for (size_t i = 0; i < n_; ++i) {
			const size_t m = i >= n_ / 2 ? i - n_ / 2 : n_ / 2 - i;
			const double r = static_cast<double>(m) / static_cast<double>(reach);
			windowN_[st * n_ + i] = m < reach ? static_cast<float>(besselI0(kLinearHalfKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Half) : 0.0f;
		}
	}
	live_ = 0;
	builds_ = swaps_ = 0;
	stage_ = Stage::Idle;
	std::array<LinearBand, kMaxOutputs> flat {};
	for (size_t o = 0; o < outputs_; ++o) {
		flat[o] = LinearBand { 0.0, 1000.0 };
	}
	buildNow(flat.data());
	builds_ = 0;
	clearAudio();
}

void LinearFirBank::clearAudio() noexcept {
	if (small_ == nullptr) {
		return;
	}
	const size_t twoB = 2 * block_;
	for (size_t c = 0; c < 2; ++c) {
		std::fill(inputs_[c], inputs_[c] + block_, 0.0f);
		std::fill(previous_[c], previous_[c] + block_, 0.0f);
		std::fill(fdl_[c], fdl_[c] + parts_ * twoB, 0.0f);
		std::fill(delay_[c], delay_[c] + delayLen_, 0.0f);
		for (size_t o = 0; o < outputs_; ++o) {
			std::fill(out_[o][c], out_[o][c] + block_, 0.0f);
		}
	}
	pos_ = head_ = delayPos_ = grid_ = 0;
	zeroBlocks_ = static_cast<uint32_t>(parts_) + 3;
}

void LinearFirBank::lowpassHalf(double hz, double* half, size_t steep) const noexcept {
	const double nyquist = 0.5 * sampleRate_;
	const double f = std::clamp(hz, 0.5, 0.499 * sampleRate_ * 1.0);
	(void)nyquist;
	const double wc = 2.0 * std::numbers::pi * f / sampleRate_;
	const double twoCos = 2.0 * std::cos(wc);
	const size_t last = (n_ / 2) >> steep; // the tap at +/- the reach is dropped, so the kernel is exactly symmetric
	const double* kaiser = kaiser_ + steep * (n_ / 2 + 1);
	half[0] = wc / std::numbers::pi * kaiser[0];
	double sum = half[0];
	double sPrev = 0.0, s = std::sin(wc);
	for (size_t m = 1; m < last; ++m) {
		half[m] = s / (std::numbers::pi * static_cast<double>(m)) * kaiser[m];
		sum += 2.0 * half[m];
		const double next = twoCos * s - sPrev;
		sPrev = s;
		s = next;
	}
	std::fill(half + last, half + n_ / 2 + 1, 0.0);
	const double norm = 1.0 / sum; // DC gain exactly 1
	for (size_t m = 0; m < last; ++m) {
		half[m] *= norm;
	}
}

void LinearFirBank::designHalf(const LinearBand& band) noexcept {
	// A RAISED-COSINE CROSSOVER IN THE FREQUENCY DOMAIN. Each corner is a cos^2 / sin^2 ramp `kHalfTransitionBins` bins wide, so the
	// bands' amplitudes telescope to exactly 1 and the half is the square root of the band's: smooth where a windowed sinc's
	// square root is not (it has an infinite slope where the band reaches 0). Zero phase, centred at N/2, windowed.
	const size_t half = n_ / 2;
	const double binHz = sampleRate_ / static_cast<double>(n_);
	const double width = kLinearHalfTransitionBins * static_cast<double>(size_t { 1 } << band.steep) * binHz;
	const auto ramp = [width](double f, double corner) { // cos^2 down through `corner`: 1 below, 0 above
		const double t = std::clamp((f - (corner - 0.5 * width)) / width, 0.0, 1.0);
		const double c = std::cos(0.5 * std::numbers::pi * t);
		return c * c;
	};
	const bool fromDc = band.loHz <= 0.0;
	const bool toTop = band.hiHz >= kLinearOpenTopHz;
	for (size_t k = 0; k <= half; ++k) {
		const double f = static_cast<double>(k) * binHz;
		const double upper = toTop ? 1.0 : ramp(f, band.hiHz);
		const double lower = fromDc ? 0.0 : ramp(f, band.loHz);
		const auto g = static_cast<float>(std::sqrt(std::max(upper - lower, 0.0)));
		if (k == 0) {
			spec_[0] = g;
		}
		else if (k == half) {
			spec_[1] = g;
		}
		else {
			spec_[2 * k] = g;
			spec_[2 * k + 1] = 0.0f;
		}
	}
	pffft_transform_ordered(big_, spec_, spec_, specWork_, PFFFT_BACKWARD);
	const float scale = 1.0f / static_cast<float>(n_);
	for (size_t i = 0; i < n_; ++i) {
		fir_[i] = spec_[(i + half) % n_] * windowN_[band.steep * n_ + i] * scale;
	}
	fir_[0] = 0.0f; // the tap at -N/2 has no partner: dropped, so the kernel is exactly symmetric
}


void LinearFirBank::design(size_t o, const LinearBand& band, size_t set) noexcept {
	const size_t last = n_ / 2;
	if (band.half) {
		designHalf(band);
	}
	else {
	if (band.hiHz >= kLinearOpenTopHz) {
		std::fill(half_, half_ + last + 1, 0.0);
		half_[0] = 1.0;
	}
	else {
		lowpassHalf(band.hiHz, half_, band.steep);
	}
	if (band.loHz <= 0.0) {
		std::fill(halfLo_, halfLo_ + last + 1, 0.0);
	}
	else {
		lowpassHalf(band.loHz, halfLo_, band.steep);
	}
	// fir[N/2 + m] = fir[N/2 - m] = hi[m] - lo[m]; fir[0] (m = -N/2) stays 0.
	fir_[0] = 0.0f;
	fir_[last] = static_cast<float>(half_[0] - halfLo_[0]);
	for (size_t m = 1; m < last; ++m) {
		const auto v = static_cast<float>(half_[m] - halfLo_[m]);
		fir_[last + m] = v;
		fir_[last - m] = v;
	}
	}
	const size_t twoB = 2 * block_;
	float* kernel = kernel_[set][o];
	for (size_t p = 0; p < parts_; ++p) {
		std::memcpy(window2B_, fir_ + p * block_, block_ * sizeof(float));
		std::fill(window2B_ + block_, window2B_ + twoB, 0.0f);
		pffft_transform(small_, window2B_, kernel + p * twoB, work2B_, PFFFT_FORWARD);
	}
}

void LinearFirBank::buildNow(const LinearBand* bands) noexcept {
	if (small_ == nullptr) {
		return;
	}
	for (size_t o = 0; o < outputs_; ++o) {
		design(o, bands[o], live_);
		live_band_[o] = target_[o] = bands[o];
	}
	stage_ = Stage::Idle;
	++builds_;
}

void LinearFirBank::setTargets(const LinearBand* bands) noexcept {
	if (small_ == nullptr || stage_ != Stage::Idle) {
		return;
	}
	const auto moved = [](double a, double b) {
		if (a == b) {
			return false;
		}
		return std::fabs(a - b) > kLinearRebuildTolerance * std::max(std::min(std::fabs(a), std::fabs(b)), 1.0);
	};
	bool any = false;
	for (size_t o = 0; o < outputs_; ++o) {
		changed_[o] = moved(bands[o].loHz, live_band_[o].loHz) || moved(bands[o].hiHz, live_band_[o].hiHz)
		              || bands[o].half != live_band_[o].half;
		any = any || changed_[o];
	}
	if (!any) {
		return;
	}
	for (size_t o = 0; o < outputs_; ++o) {
		target_[o] = bands[o];
	}
	stage_ = Stage::Building;
	next_ = 0;
}

void LinearFirBank::step() noexcept {
	if (stage_ != Stage::Building) {
		return;
	}
	while (next_ < outputs_ && !changed_[next_]) {
		++next_;
	}
	if (next_ < outputs_) {
		design(next_, target_[next_], 1 - live_);
		++next_;
		++builds_;
		return;
	}
	// Every changed kernel is in the spare; the unchanged ones go across so the spare is a whole set.
	const size_t len = parts_ * 2 * block_;
	for (size_t o = 0; o < outputs_; ++o) {
		if (!changed_[o]) {
			std::memcpy(kernel_[1 - live_][o], kernel_[live_][o], len * sizeof(float));
		}
	}
	stage_ = Stage::Ready;
}

void LinearFirBank::blockEdge() noexcept {
	const size_t twoB = 2 * block_;
	const float scale = 1.0f / static_cast<float>(twoB);
	const bool swap = stage_ == Stage::Ready;
	head_ = (head_ + 1) % parts_;
	bool silent = true;
	for (size_t c = 0; c < 2; ++c) {
		for (size_t i = 0; i < block_; ++i) {
			silent = silent && inputs_[c][i] == 0.0f;
		}
	}
	zeroBlocks_ = silent ? (zeroBlocks_ < 0x7fffffffu ? zeroBlocks_ + 1 : zeroBlocks_) : 0;
	for (size_t c = 0; c < 2; ++c) {
		std::memcpy(window2B_, previous_[c], block_ * sizeof(float));
		std::memcpy(window2B_ + block_, inputs_[c], block_ * sizeof(float));
		std::memcpy(previous_[c], inputs_[c], block_ * sizeof(float));
		pffft_transform(small_, window2B_, fdl_[c] + head_ * twoB, work2B_, PFFFT_FORWARD);
		const auto convolve = [&](size_t set, size_t o, float* accum) {
			std::fill(accum, accum + twoB, 0.0f);
			for (size_t p = 0; p < parts_; ++p) {
				const size_t idx = (head_ + parts_ - p) % parts_;
				pffft_zconvolve_accumulate(small_, fdl_[c] + idx * twoB, kernel_[set][o] + p * twoB, accum, scale);
			}
			pffft_transform(small_, accum, accum, work2B_, PFFFT_BACKWARD);
		};
		for (size_t o = 0; o < outputs_; ++o) {
			convolve(live_, o, accum_[c]);
			if (!(swap && changed_[o])) {
				std::memcpy(out_[o][c], accum_[c] + block_, block_ * sizeof(float));
				continue;
			}
			// THE SWAP: both kernels over the same input spectra, crossfaded across the block.
			convolve(1 - live_, o, fadeAccum_[c]);
			const float* was = accum_[c] + block_;
			const float* now = fadeAccum_[c] + block_;
			for (size_t i = 0; i < block_; ++i) {
				const float t = static_cast<float>(i + 1) / static_cast<float>(block_);
				out_[o][c][i] = was[i] + t * (now[i] - was[i]);
			}
		}
	}
	if (swap) {
		live_ = 1 - live_;
		for (size_t o = 0; o < outputs_; ++o) {
			if (changed_[o]) {
				live_band_[o] = target_[o];
			}
		}
		stage_ = Stage::Idle;
		++swaps_;
	}
}

} // namespace feathers::dsp
