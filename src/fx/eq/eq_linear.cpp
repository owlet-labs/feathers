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

#include "fx/eq/eq_linear.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace feathers::fx::eq {

namespace {

/// Magnitude bins a grid step: 5 steps a path at 48 kHz (8193 bins).
constexpr size_t kBinsPerSlice = 2048;
/// Partition transforms a grid step.
constexpr size_t kPartsPerSlice = 4;

float* allocFloats(size_t count) {
	auto* p = static_cast<float*>(pffft_aligned_malloc(std::max<size_t>(count, 1) * sizeof(float)));
	std::fill(p, p + std::max<size_t>(count, 1), 0.0f);
	return p;
}

void freeFloats(float*& p) noexcept {
	pffft_aligned_free(p);
	p = nullptr;
}

[[nodiscard]] bool onPath(Channel band, int path) noexcept {
	return band == Channel::Stereo || band == (path == 0 ? Channel::Mid : Channel::Side);
}

} // namespace

void LinearPhase::release() noexcept {
	for (auto& set : kernel_) {
		for (float*& k : set) {
			freeFloats(k);
		}
	}
	for (auto* group : { &inputs_, &previous_, &fdl_, &accum_, &fadeAccum_, &outputs_, &magnitude_, &fir_ }) {
		for (float*& p : *group) {
			freeFloats(p);
		}
	}
	for (float** p : { &window2B_, &work2B_, &spectrumN_, &workN_, &windowN_ }) {
		freeFloats(*p);
	}
	if (big_ != nullptr) {
		pffft_destroy_setup(big_);
		big_ = nullptr;
	}
	if (small_ != nullptr) {
		pffft_destroy_setup(small_);
		small_ = nullptr;
	}
}

void LinearPhase::prepare(double sampleRate) {
	release();
	sampleRate_ = sampleRate;
	n_ = linearFirLength(sampleRate);
	block_ = linearBlockLength(sampleRate);
	parts_ = n_ / block_;
	const size_t twoB = 2 * block_;
	big_ = pffft_new_setup(static_cast<int>(n_), PFFFT_REAL);
	small_ = pffft_new_setup(static_cast<int>(twoB), PFFFT_REAL);
	for (auto& set : kernel_) {
		for (float*& k : set) {
			k = allocFloats(parts_ * twoB);
		}
	}
	for (int c = 0; c < 2; ++c) {
		const auto i = static_cast<size_t>(c);
		inputs_[i] = allocFloats(block_);
		previous_[i] = allocFloats(block_);
		outputs_[i] = allocFloats(block_);
		fdl_[i] = allocFloats(parts_ * twoB);
		accum_[i] = allocFloats(twoB);
		fadeAccum_[i] = allocFloats(twoB);
		magnitude_[i] = allocFloats(n_ / 2 + 1);
		fir_[i] = allocFloats(n_);
	}
	window2B_ = allocFloats(twoB);
	work2B_ = allocFloats(twoB);
	spectrumN_ = allocFloats(n_);
	workN_ = allocFloats(n_);
	windowN_ = allocFloats(n_);
	// BLACKMAN, PERIODIC: symmetric about N/2 (w[N/2 + k] = w[N/2 - k]) and exactly 1 there, so the centred
	// kernel keeps its symmetry and its peak.
	const double twoPi = 2.0 * kPi;
	for (size_t i = 0; i < n_; ++i) {
		const double x = static_cast<double>(i) / static_cast<double>(n_);
		windowN_[i] = static_cast<float>(0.42 - 0.5 * std::cos(twoPi * x) + 0.08 * std::cos(2.0 * twoPi * x));
	}
	live_ = 0;
	builds_ = 0;
	swaps_ = 0;
	buildNow(Target {});
	builds_ = 0;
	clearAudio();
}

void LinearPhase::clearAudio() noexcept {
	if (big_ == nullptr) {
		return;
	}
	const size_t twoB = 2 * block_;
	for (int c = 0; c < 2; ++c) {
		const auto i = static_cast<size_t>(c);
		std::fill(inputs_[i], inputs_[i] + block_, 0.0f);
		std::fill(previous_[i], previous_[i] + block_, 0.0f);
		std::fill(outputs_[i], outputs_[i] + block_, 0.0f);
		std::fill(fdl_[i], fdl_[i] + parts_ * twoB, 0.0f);
	}
	pos_ = 0;
	head_ = 0;
}

bool LinearPhase::pathsAlike() const noexcept {
	for (size_t b = 0; b < target_.bands.size(); ++b) {
		if (!target_.bands[b].bypass && target_.channel[b] != Channel::Stereo) {
			return false;
		}
	}
	return true;
}

void LinearPhase::expand() noexcept {
	const auto add = [this](int path, const Biquad& q) {
		const auto p = static_cast<size_t>(path);
		// |b0 + b1 e^-jw + b2 e^-2jw|² = b0² + b1² + b2² + 2 b1 (b0 + b2) cos w + 2 b0 b2 cos 2w; a0 = 1.
		sections_[p][sectionCount_[p]++] = { q.b0 * q.b0 + q.b1 * q.b1 + q.b2 * q.b2, 2.0 * q.b1 * (q.b0 + q.b2),
			                                 2.0 * q.b0 * q.b2, 1.0 + q.a1 * q.a1 + q.a2 * q.a2,
			                                 2.0 * q.a1 * (1.0 + q.a2), 2.0 * q.a2 };
	};
	for (int path = 0; path < 2; ++path) {
		const auto p = static_cast<size_t>(path);
		sectionCount_[p] = 0;
		pathGain_[p] = target_.tilt.bypass ? 1.0 : target_.tilt.gain * target_.tilt.gain;
		if (!target_.tilt.bypass) {
			for (const Biquad& s : target_.tilt.sections) {
				add(path, s);
			}
		}
		for (size_t b = 0; b < target_.bands.size(); ++b) {
			const BandDesign& d = target_.bands[b];
			if (d.bypass || !onPath(target_.channel[b], path)) {
				continue;
			}
			for (int i = 0; i < d.count; ++i) {
				add(path, d.sections[static_cast<size_t>(i)].z);
			}
		}
	}
}

void LinearPhase::magnitudes(int path, size_t first, size_t last) noexcept {
	const auto p = static_cast<size_t>(path);
	float* out = magnitude_[p];
	const Section2* s = sections_[p].data();
	const size_t count = sectionCount_[p];
	for (size_t k = first; k < last; ++k) {
		const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(n_);
		const double c1 = std::cos(w), c2 = std::cos(2.0 * w);
		double m = pathGain_[p];
		for (size_t i = 0; i < count; ++i) {
			m *= (s[i].nA + s[i].nB * c1 + s[i].nC * c2) / (s[i].dA + s[i].dB * c1 + s[i].dC * c2);
		}
		out[k] = static_cast<float>(std::sqrt(std::max(m, 0.0)));
	}
}

void LinearPhase::inverse(int path) noexcept {
	const float* mag = magnitude_[static_cast<size_t>(path)];
	// ZERO PHASE, in pffft's ordered real layout: [DC, Nyquist, re1, im1, re2, im2, ...].
	spectrumN_[0] = mag[0];
	spectrumN_[1] = mag[n_ / 2];
	for (size_t k = 1; k < n_ / 2; ++k) {
		spectrumN_[2 * k] = mag[k];
		spectrumN_[2 * k + 1] = 0.0f;
	}
	pffft_transform_ordered(big_, spectrumN_, spectrumN_, workN_, PFFFT_BACKWARD);
	// CENTRED AT N/2 AND WINDOWED; pffft's inverse is unscaled.
	float* fir = fir_[static_cast<size_t>(path)];
	const float scale = 1.0f / static_cast<float>(n_);
	for (size_t i = 0; i < n_; ++i) {
		fir[i] = spectrumN_[(i + n_ / 2) % n_] * windowN_[i] * scale;
	}
}

void LinearPhase::partition(int path, size_t set, size_t first, size_t last) noexcept {
	const float* fir = fir_[static_cast<size_t>(path)];
	float* kernel = kernel_[set][static_cast<size_t>(path)];
	const size_t twoB = 2 * block_;
	for (size_t p = first; p < last; ++p) {
		std::memcpy(window2B_, fir + p * block_, block_ * sizeof(float));
		std::fill(window2B_ + block_, window2B_ + twoB, 0.0f);
		pffft_transform(small_, window2B_, kernel + p * twoB, work2B_, PFFFT_FORWARD);
	}
}

void LinearPhase::buildNow(const Target& t) noexcept {
	if (big_ == nullptr) {
		return;
	}
	target_ = t;
	alike_ = pathsAlike();
	expand();
	const size_t twoB = 2 * block_;
	for (int path = 0; path < (alike_ ? 1 : 2); ++path) {
		magnitudes(path, 0, n_ / 2 + 1);
		inverse(path);
		partition(path, live_, 0, parts_);
	}
	if (alike_) {
		std::memcpy(kernel_[live_][1], kernel_[live_][0], parts_ * twoB * sizeof(float));
		std::memcpy(fir_[1], fir_[0], n_ * sizeof(float));
	}
	stage_ = Stage::Idle;
	++builds_;
}

bool LinearPhase::request(const Target& t) noexcept {
	if (busy() || big_ == nullptr) {
		return false;
	}
	target_ = t;
	alike_ = pathsAlike();
	expand();
	stage_ = Stage::Magnitude;
	path_ = 0;
	next_ = 0;
	return true;
}

void LinearPhase::step() noexcept {
	const bool secondPath = path_ == 0 && !alike_;
	switch (stage_) {
	case Stage::Magnitude: {
		const size_t last = std::min(next_ + kBinsPerSlice, n_ / 2 + 1);
		magnitudes(path_, next_, last);
		next_ = last;
		if (next_ == n_ / 2 + 1) {
			path_ = secondPath ? 1 : 0;
			next_ = 0;
			stage_ = secondPath ? Stage::Magnitude : Stage::Inverse;
		}
		break;
	}
	case Stage::Inverse:
		inverse(path_);
		path_ = secondPath ? 1 : 0;
		stage_ = secondPath ? Stage::Inverse : Stage::Partition;
		break;
	case Stage::Partition: {
		const size_t last = std::min(next_ + kPartsPerSlice, parts_);
		partition(path_, 1 - live_, next_, last);
		next_ = last;
		if (next_ == parts_) {
			if (secondPath) {
				path_ = 1;
				next_ = 0;
				break;
			}
			if (alike_) {
				std::memcpy(kernel_[1 - live_][1], kernel_[1 - live_][0], parts_ * 2 * block_ * sizeof(float));
				std::memcpy(fir_[1], fir_[0], n_ * sizeof(float));
			}
			stage_ = Stage::Ready;
			++builds_;
		}
		break;
	}
	case Stage::Idle:
	case Stage::Ready:
		break;
	}
}

void LinearPhase::blockEdge() noexcept {
	const size_t twoB = 2 * block_;
	const float scale = 1.0f / static_cast<float>(twoB);
	const bool swap = stage_ == Stage::Ready;
	head_ = (head_ + 1) % parts_;
	for (size_t c = 0; c < 2; ++c) {
		// THE WINDOW [previous block, this block] INTO THE FREQUENCY-DOMAIN DELAY LINE.
		std::memcpy(window2B_, previous_[c], block_ * sizeof(float));
		std::memcpy(window2B_ + block_, inputs_[c], block_ * sizeof(float));
		std::memcpy(previous_[c], inputs_[c], block_ * sizeof(float));
		pffft_transform(small_, window2B_, fdl_[c] + head_ * twoB, work2B_, PFFFT_FORWARD);
		const auto convolve = [&](size_t set, float* accum) {
			std::fill(accum, accum + twoB, 0.0f);
			for (size_t p = 0; p < parts_; ++p) {
				const size_t idx = (head_ + parts_ - p) % parts_;
				pffft_zconvolve_accumulate(small_, fdl_[c] + idx * twoB, kernel_[set][c] + p * twoB, accum, scale);
			}
			// OVERLAP-SAVE: the second half of the inverse is the valid linear convolution.
			pffft_transform(small_, accum, accum, work2B_, PFFFT_BACKWARD);
		};
		convolve(live_, accum_[c]);
		if (!swap) {
			std::memcpy(outputs_[c], accum_[c] + block_, block_ * sizeof(float));
			continue;
		}
		// THE SWAP: both kernels over the same input spectra, crossfaded across the block.
		convolve(1 - live_, fadeAccum_[c]);
		const float* was = accum_[c] + block_;
		const float* now = fadeAccum_[c] + block_;
		for (size_t i = 0; i < block_; ++i) {
			const float t = static_cast<float>(i + 1) / static_cast<float>(block_);
			outputs_[c][i] = was[i] + t * (now[i] - was[i]);
		}
	}
	if (swap) {
		live_ = 1 - live_;
		stage_ = Stage::Idle;
		++swaps_;
	}
}

} // namespace feathers::fx::eq
