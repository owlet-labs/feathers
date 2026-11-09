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

#include "fx/spectral/spectral_sidechain.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::spectral {

namespace {

using feathers::spectral::VocodeEnvelope;

/// What a stop's estimator scale adds to read dBFS (Stft::envelopeToFs): the FFT's magnitude scaled by pi / N.
double toFs(int n) { return 20.0 * std::log10(std::numbers::pi / static_cast<double>(std::max(1, n))); }

} // namespace

SidechainEnvelope::~SidechainEnvelope() { release(); }

void SidechainEnvelope::release() noexcept {
	for (PFFFT_Setup*& s : setups_) {
		if (s != nullptr) {
			pffft_destroy_setup(s);
			s = nullptr;
		}
	}
	if (buffer_ != nullptr) {
		pffft_aligned_free(buffer_);
		buffer_ = nullptr;
	}
	if (work_ != nullptr) {
		pffft_aligned_free(work_);
		work_ = nullptr;
	}
}

void SidechainEnvelope::prepare(double sampleRate) {
	release();
	sampleRate_ = sampleRate;
	int nMax = 0;
	for (int r = 0; r < kResolutionCount; ++r) {
		const int n = fftSizeFor(static_cast<Resolution>(r), sampleRate);
		setups_[static_cast<size_t>(r)] = pffft_new_setup(n, PFFFT_REAL);
		nMax = std::max(nMax, n);
	}
	buffer_ = static_cast<float*>(pffft_aligned_malloc(static_cast<size_t>(nMax) * sizeof(float)));
	work_ = static_cast<float*>(pffft_aligned_malloc(static_cast<size_t>(nMax) * sizeof(float)));
	window_.assign(static_cast<size_t>(nMax), 0.0f);
	synthesis_.assign(static_cast<size_t>(nMax), 0.0f);
	envBins_.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	mag_.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	estimator_.prepare(nMax / 2 + 1);
	size_t capacity = 1;
	while (capacity < static_cast<size_t>(nMax)) {
		capacity <<= 1;
	}
	for (Channel& c : ch_) {
		c.history.assign(capacity, 0.0f);
		c.ola.assign(static_cast<size_t>(nMax), 0.0f);
		c.out.assign(static_cast<size_t>(nMax / kOverlap), 0.0f);
	}
	mask_ = capacity - 1;
	n_ = 0;
	const Settings keep = settings_;
	settings_.resolution = static_cast<Resolution>((static_cast<int>(keep.resolution) + 1) % kResolutionCount); // force the stop
	setSettings(keep);
	reset();
}

void SidechainEnvelope::reset() noexcept {
	for (Channel& c : ch_) {
		std::fill(c.history.begin(), c.history.end(), 0.0f);
		c.grid.fill(kEnvelopeFloorDb);
		c.dbfs.fill(VocodeEnvelope::kSilentDb);
		c.flatness = 0.0f;
		std::fill(c.ola.begin(), c.ola.end(), 0.0f);
		std::fill(c.out.begin(), c.out.end(), 0.0f);
	}
	write_ = 0;
	outPos_ = 0;
	ch_[0].rng = 0x2545f491u; // FIXED SEEDS at a reset (the persist rule: never a clock or an address)
	ch_[1].rng = 0x9e3779b9u;
	hopCount_ = 0;
	primed_ = false;
	++serial_;
}

void SidechainEnvelope::setSettings(const Settings& s) noexcept {
	const bool newStop = s.resolution != settings_.resolution || n_ == 0;
	const bool intoStereo = s.stereo && !settings_.stereo;
	settings_ = s;
	if (intoStereo && primed_) {
		// INTO STEREO, both channels start from the envelope as it was (the mono sum's), and part from there.
		ch_[1].grid = ch_[0].grid;
		ch_[1].dbfs = ch_[0].dbfs;
		++serial_;
	}
	if (!newStop || sampleRate_ <= 0.0) {
		return;
	}
	const int was = n_;
	n_ = fftSizeFor(s.resolution, sampleRate_);
	binHz_ = sampleRate_ / static_cast<double>(n_);
	// THE PERIODIC sqrt-Hann the effect analyses with (Stft::setResolution).
	for (int j = 0; j < n_; ++j) {
		window_[static_cast<size_t>(j)] =
		    static_cast<float>(std::sqrt(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / n_))));
		synthesis_[static_cast<size_t>(j)] = static_cast<float>(static_cast<double>(window_[static_cast<size_t>(j)]) / (2.0 * static_cast<double>(n_)));
	}
	hopCount_ = 0;
	outPos_ = 0;
	for (Channel& c : ch_) { // the noise's overlap-add restarts at the new stop
		std::fill(c.ola.begin(), c.ola.end(), 0.0f);
		std::fill(c.out.begin(), c.out.end(), 0.0f);
	}
	// THE ENVELOPE KEPT ACROSS THE STOP, re-expressed on the new one: a noise's per-bin level is 10·log10 of the bin width, and the
	// voices' calibration reads the bin width, so moving the grid by that keeps what they hear where it was. A held (frozen) one too.
	if (was > 0 && primed_) {
		const double move = 10.0 * std::log10(static_cast<double>(was) / static_cast<double>(n_));
		for (Channel& c : ch_) {
			for (size_t g = 0; g < c.grid.size(); ++g) {
				if (c.dbfs[g] > VocodeEnvelope::kSilentDb) {
					c.dbfs[g] = static_cast<float>(static_cast<double>(c.dbfs[g]) + move);
					c.grid[g] = std::max(kEnvelopeFloorDb, static_cast<float>(static_cast<double>(c.dbfs[g]) - toFs(n_)));
				}
			}
		}
		++serial_;
	}
}

void SidechainEnvelope::prime(const float* left, const float* right, double binHz, float flatnessL, float flatnessR) noexcept {
	// Read onto THIS stop's scale (the move setSettings makes), so a block saved at another Resolution sounds where it was saved.
	const double move = binHz > 0.0 && binHz_ > 0.0 ? 10.0 * std::log10(binHz_ / binHz) : 0.0;
	const double fs = toFs(n_);
	for (int k = 0; k < 2; ++k) {
		Channel& c = ch_[static_cast<size_t>(k)];
		c.flatness = std::clamp(k == 1 && right != nullptr ? flatnessR : flatnessL, 0.0f, 1.0f);
		const float* dbfs = k == 1 && right != nullptr ? right : left;
		for (size_t g = 0; g < c.grid.size(); ++g) {
			const double v = static_cast<double>(dbfs[g]);
			const bool silent = !(v > static_cast<double>(VocodeEnvelope::kSilentDb));
			c.dbfs[g] = silent ? VocodeEnvelope::kSilentDb : static_cast<float>(v + move);
			c.grid[g] = silent ? kEnvelopeFloorDb : std::max(kEnvelopeFloorDb, static_cast<float>(v + move - fs));
		}
	}
	primed_ = true;
	++serial_;
}

void SidechainEnvelope::push(const float* left, const float* right, int frames, float* noiseL, float* noiseR) noexcept {
	if (n_ == 0 || frames <= 0) {
		return;
	}
	const int hop = n_ / kOverlap;
	const bool stereo = settings_.stereo;
	const bool noise = settings_.sibilance && noiseL != nullptr;
	for (int i = 0; i < frames; ++i) {
		const float l = left != nullptr ? left[i] : 0.0f;
		const float r = right != nullptr ? right[i] : l;
		// MONO: ½(L + R) (the left alone with no right); STEREO: each channel to its own ring (the right is the left with none).
		ch_[0].history[write_] = stereo ? l : (right != nullptr ? 0.5f * (l + r) : l);
		if (stereo) {
			ch_[1].history[write_] = r;
		}
		// THE NOISE OF THE HOP BEFORE, sample by sample (the Spectral effect's out ring): what the last frame finalised.
		if (noiseL != nullptr) {
			noiseL[i] = noise ? ch_[0].out[static_cast<size_t>(outPos_)] : 0.0f;
			if (noiseR != nullptr) {
				noiseR[i] = noise ? ch_[stereo ? 1 : 0].out[static_cast<size_t>(outPos_)] : 0.0f;
			}
		}
		++outPos_;
		write_ = (write_ + 1) & mask_;
		if (++hopCount_ >= hop) {
			hopCount_ = 0;
			outPos_ = 0;
			if (!holding()) {
				frame(0);
				if (stereo) {
					frame(1);
				}
				primed_ = true;
				++frames_;
			}
			// FROZEN, THE NOISE GOES ON from the held envelope and flatness (as the effect's sibilance does).
			if (settings_.sibilance && primed_) {
				noiseFrame(0);
				if (stereo) {
					noiseFrame(1);
				}
			}
		}
	}
}

void SidechainEnvelope::noiseFrame(int ci) noexcept {
	Channel& ch = ch_[static_cast<size_t>(ci)];
	const int n = n_;
	const int bins = n / 2 + 1;
	const int hop = n / kOverlap;
	// SIBILANCE (the effect's vocodeSpectrum): noise of random phase at the sidechain's level, above 2 kHz, as noisy as it is there.
	estimator_.toBinsFast(ch.grid, bins, binHz_, envBins_.data());
	const float flat = ch.flatness;
	buffer_[0] = 0.0f;
	buffer_[1] = 0.0f;
	for (int i = 1; i < n / 2; ++i) {
		float re = 0.0f;
		float im = 0.0f;
		if (flat > 0.0f && static_cast<double>(i) * binHz_ >= 2000.0) {
			ch.rng ^= ch.rng << 13;
			ch.rng ^= ch.rng >> 17;
			ch.rng ^= ch.rng << 5;
			const double phase = 6.283185307179586 * static_cast<double>(ch.rng) / 4294967296.0;
			const float amp = flat * envBins_[static_cast<size_t>(i)];
			re = amp * static_cast<float>(std::cos(phase));
			im = amp * static_cast<float>(std::sin(phase));
		}
		buffer_[2 * i] = re;
		buffer_[2 * i + 1] = im;
	}
	pffft_transform_ordered(setups_[static_cast<size_t>(settings_.resolution)], buffer_, buffer_, work_, PFFFT_BACKWARD);
	for (int j = 0; j < n; ++j) {
		ch.ola[static_cast<size_t>(j)] += buffer_[j] * synthesis_[static_cast<size_t>(j)];
	}
	// THE HOP'S FINISHED SAMPLES OUT, and the overlap-add moved on by a hop.
	std::copy_n(ch.ola.begin(), hop, ch.out.begin());
	std::copy(ch.ola.begin() + hop, ch.ola.begin() + n, ch.ola.begin());
	std::fill(ch.ola.begin() + (n - hop), ch.ola.begin() + n, 0.0f);
}

void SidechainEnvelope::frame(int ci) noexcept {
	Channel& ch = ch_[static_cast<size_t>(ci)];
	const int n = n_;
	const int bins = n / 2 + 1;
	const size_t start = (write_ - static_cast<size_t>(n)) & mask_;
	bool any = false;
	for (int j = 0; j < n; ++j) {
		const float x = ch.history[(start + static_cast<size_t>(j)) & mask_];
		any = any || x != 0.0f;
		buffer_[j] = x * window_[static_cast<size_t>(j)];
	}
	std::array<float, kEnvelopePoints> now {};
	if (any) {
		pffft_transform_ordered(setups_[static_cast<size_t>(settings_.resolution)], buffer_, buffer_, work_, PFFFT_FORWARD);
		mag_[0] = std::fabs(buffer_[0]);
		mag_[static_cast<size_t>(n / 2)] = std::fabs(buffer_[1]);
		for (int i = 1; i < n / 2; ++i) {
			mag_[static_cast<size_t>(i)] = std::sqrt(buffer_[2 * i] * buffer_[2 * i] + buffer_[2 * i + 1] * buffer_[2 * i + 1]);
		}
		estimator_.estimate(mag_.data(), bins, binHz_, std::clamp(settings_.bands, kMinBands, kMaxBands), now);
		// HOW NOISY ABOVE 2 kHz: the spectral flatness (geometric over arithmetic mean of the power), 0 for a tone, 1 for noise
		// (Stft::sidechainFrame's, which Sibilance reads).
		double logSum = 0.0;
		double sum = 0.0;
		int count = 0;
		for (int i = 1; i < n / 2; ++i) {
			if (static_cast<double>(i) * binHz_ < 2000.0) {
				continue;
			}
			const double p = static_cast<double>(mag_[static_cast<size_t>(i)]) * static_cast<double>(mag_[static_cast<size_t>(i)]);
			logSum += 0.6931471805599453 * fastLog2(p + 1e-30);
			sum += p;
			++count;
		}
		ch.flatness = count > 0 && sum > 0.0 ? static_cast<float>(std::exp(logSum / count) / (sum / count)) : 0.0f;
	}
	else {
		now.fill(kEnvelopeFloorDb);
		ch.flatness = 0.0f;
	}
	// ATTACK AND RELEASE per grid point, in dB, a one-pole each way at the hop's rate (Stft::sidechainFrame).
	const double hopSeconds = static_cast<double>(n / kOverlap) / sampleRate_;
	const double attack = std::exp(-hopSeconds / std::max(settings_.attackMs * 1e-3, 1e-6));
	const double rel = std::exp(-hopSeconds / std::max(settings_.releaseMs * 1e-3, 1e-6));
	const double fs = toFs(n);
	bool changed = !primed_;
	for (size_t g = 0; g < now.size(); ++g) {
		const double target = now[g];
		const double was = primed_ ? static_cast<double>(ch.grid[g]) : target;
		const double coef = target > was ? attack : rel;
		ch.grid[g] = std::max(kEnvelopeFloorDb, static_cast<float>(target + coef * (was - target)));
		// AT THE FLOOR IS SILENCE (the effect's toBins reads it as 0), so a silent sidechain gives a silent rest.
		const float next = ch.grid[g] <= kEnvelopeFloorDb ? VocodeEnvelope::kSilentDb : static_cast<float>(static_cast<double>(ch.grid[g]) + fs);
		changed = changed || next != ch.dbfs[g];
		ch.dbfs[g] = next;
	}
	// THE SERIAL MOVES ONLY WITH THE CONTENTS, so a silent (or settled) sidechain costs its readers nothing (the synth's copy, the
	// phase optimiser's sketch, every voice's gain memory).
	if (changed) {
		++serial_;
	}
}

} // namespace feathers::fx::spectral
