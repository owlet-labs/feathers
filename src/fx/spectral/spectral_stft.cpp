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

#include "fx/spectral/spectral_stft.h"

#include "character/phi_triangle.hpp"
#include "dsp/partial_spectrum.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace feathers::fx::spectral {

bool isRealFftSize(int n) noexcept {
	if (n < kMinFftSize || n % kMinFftSize != 0) {
		return false;
	}
	int q = n / kMinFftSize;
	for (const int f : { 2, 3, 5 }) {
		while (q % f == 0) {
			q /= f;
		}
	}
	return q == 1;
}

uint32_t latencyFor(Resolution r, double sampleRate) noexcept {
	const int i = std::clamp(static_cast<int>(r), 0, kResolutionCount - 1);
	const double samples = std::round(kResolutionMs[i] * std::max(sampleRate, 1.0) / 1000.0);
	return static_cast<uint32_t>(std::max(samples, static_cast<double>(kMinFftSize)));
}

int fftSizeFor(Resolution r, double sampleRate) noexcept {
	int n = static_cast<int>(latencyFor(r, sampleRate));
	n -= n % kMinFftSize;
	while (n > kMinFftSize && !isRealFftSize(n)) {
		n -= kMinFftSize;
	}
	return std::max(n, kMinFftSize);
}

void Stft::Aligned::allocate(size_t count) {
	release();
	p = static_cast<float*>(pffft_aligned_malloc(std::max<size_t>(count, 1) * sizeof(float)));
	std::fill(p, p + std::max<size_t>(count, 1), 0.0f);
}

void Stft::Aligned::release() noexcept {
	if (p != nullptr) {
		pffft_aligned_free(p);
		p = nullptr;
	}
}

Stft::~Stft() { release(); }

void Stft::release() noexcept {
	for (auto*& s : setups_) {
		if (s != nullptr) {
			pffft_destroy_setup(s);
			s = nullptr;
		}
	}
	frame_.release();
	sideSpectrum_.release();
	drySpectrum_.release();
	spectrum_.release();
	work_.release();
}

void Stft::prepare(double sampleRate) {
	release();
	sampleRate_ = sampleRate;
	int nMax = 0;
	uint32_t lMax = 0;
	for (int r = 0; r < kResolutionCount; ++r) {
		const int n = fftSizeFor(static_cast<Resolution>(r), sampleRate);
		setups_[static_cast<size_t>(r)] = pffft_new_setup(n, PFFFT_REAL);
		nMax = std::max(nMax, n);
		lMax = std::max(lMax, latencyFor(static_cast<Resolution>(r), sampleRate));
	}
	frame_.allocate(static_cast<size_t>(nMax));
	sideSpectrum_.allocate(static_cast<size_t>(nMax));
	drySpectrum_.allocate(static_cast<size_t>(nMax));
	mag_.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	envX_.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	for (Side& s : side_) {
		s.env.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	}
	estimator_.prepare(nMax / 2 + 1);
	fshiftRatio_.assign(static_cast<size_t>(nMax / 2 + 1), 1.0f);
	partialsGain_.assign(static_cast<size_t>(nMax / 2 + 1), 1.0f);
	peaks_.assign(static_cast<size_t>(nMax / 2 + 1), 0);
	bandFirst_.assign(static_cast<size_t>(nMax / 2 + 1), 0);
	bandW_.assign(static_cast<size_t>(nMax / 2 + 1) * kBandReach, 0.0f);
	bandU_.assign(static_cast<size_t>(nMax / 2 + 1), 0.0f);
	bandKey_ = {};
	spectrum_.allocate(static_cast<size_t>(nMax));
	work_.allocate(static_cast<size_t>(nMax));
	analysisWindow_.assign(static_cast<size_t>(nMax), 0.0f);
	synthesisWindow_.assign(static_cast<size_t>(nMax), 0.0f);
	gains_.assign(static_cast<size_t>(nMax / 2 + 1), 1.0f);
	size_t capacity = 1;
	while (capacity < static_cast<size_t>(lMax) + 1) {
		capacity <<= 1;
	}
	mask_ = capacity - 1;
	for (Side& s : side_) {
		s.history.assign(capacity, 0.0f);
	}
	carrierHistory_.assign(capacity, 0.0f);
	for (auto& c : ch_) {
		c.history.assign(capacity, 0.0f);
		c.ola.assign(static_cast<size_t>(nMax), 0.0f);
		c.out.assign(static_cast<size_t>(nMax / kOverlap), 0.0f);
	}
	const Resolution keep = resolution_;
	n_ = 0; // force the windows to be built
	resolution_ = keep;
	setResolution(keep);
	reset();
}

void Stft::setResolution(Resolution r) noexcept {
	if (r == resolution_ && n_ != 0) {
		return;
	}
	resolution_ = r;
	if (sampleRate_ <= 0.0) {
		return;
	}
	latency_ = latencyFor(r, sampleRate_);
	n_ = fftSizeFor(r, sampleRate_);
	pad_ = static_cast<int>(latency_) - n_;
	// PERIODIC sqrt-Hann: hann[j] = 0.5 (1 − cos 2πj/N) sums to exactly 2 at a hop of N/4, so the synthesis side carries
	// 1/2 for the overlap and 1/N for pffft's unscaled inverse.
	const double scale = 1.0 / (2.0 * static_cast<double>(n_));
	for (int j = 0; j < n_; ++j) {
		const double hann = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / n_));
		const double s = std::sqrt(hann);
		analysisWindow_[static_cast<size_t>(j)] = static_cast<float>(s);
		synthesisWindow_[static_cast<size_t>(j)] = static_cast<float>(s * scale);
	}
	std::fill(gains_.begin(), gains_.end(), 1.0f);
	reset();
}

void Stft::reset() noexcept {
	for (Side& s : side_) {
		std::fill(s.history.begin(), s.history.end(), 0.0f);
		s.primed = false;
		s.gridS.fill(kEnvelopeFloorDb);
		s.flatness = 0.0f;
		s.copy = false;
		s.maxDb = kEnvelopeFloorDb;
	}
	std::fill(carrierHistory_.begin(), carrierHistory_.end(), 0.0f);
	carrierPhase_ = 0.0;
	carrierRng_ = 0x9e3779b9u;
	pink_.fill(0.0f);
	for (auto& c : ch_) {
		std::fill(c.history.begin(), c.history.end(), 0.0f);
		std::fill(c.ola.begin(), c.ola.end(), 0.0f);
		std::fill(c.out.begin(), c.out.end(), 0.0f);
	}
	write_ = 0;
	hopCount_ = 0;
	outPos_ = 0;
	running_ = false;
	framesSinceStart_ = 0;
	w_ = 0.0f;
	wSteps_ = 0;
	frames_ = 0;
	analysed_ = 0;
	vocodeMix_ = 0.0f;
	noiseRng_ = 0x2545f491u;
	mix_ = mixTarget_;
	mixLeft_ = 0;
	mixFresh_ = true;
	bands_.fill(kBandFloorDb);
}

void Stft::setMix(float mix) noexcept {
	const float target = std::clamp(mix, 0.0f, 1.0f);
	if (mixFresh_) {
		mix_ = mixTarget_ = target;
		mixLeft_ = 0;
		return;
	}
	if (target != mixTarget_) {
		mixTarget_ = target;
		mixStep_ = (target - mix_) / static_cast<float>(kMixRampSamples);
		mixLeft_ = kMixRampSamples;
	}
}

bool Stft::loadFrame(int channel, bool carrier) noexcept {
	// THE FRAME ENDS `pad_` SAMPLES BEHIND THE NEWEST INPUT (at write_ − 1), so a sample leaves pad + N = L samples after
	// it entered.
	const size_t start = (write_ - static_cast<size_t>(pad_) - static_cast<size_t>(n_)) & mask_;
	// AN INTERNAL CARRIER STANDS IN FOR THE INPUT in every channel: the vocoder shapes it, and Mix's dry side keeps the input.
	const std::vector<float>& ring = carrier ? carrierHistory_ : ch_[static_cast<size_t>(channel)].history;
	bool any = false;
	for (int j = 0; j < n_; ++j) {
		const float x = ring[(start + static_cast<size_t>(j)) & mask_];
		any = any || x != 0.0f;
		frame_.p[j] = x * analysisWindow_[static_cast<size_t>(j)];
	}
	return any;
}

void Stft::measureBands() noexcept {
	// A sine of amplitude A at a bin centre reads |X| = A · Σw / 2 = A · N / π under the sqrt-Hann (Σw = 2N/π).
	const double toFs = std::numbers::pi / static_cast<double>(n_);
	const double binHz = sampleRate_ / static_cast<double>(n_);
	const double span = std::log2(kBandHighHz / kBandLowHz);
	bands_.fill(kBandFloorDb);
	for (int i = 1; i < n_ / 2; ++i) {
		const double hz = i * binHz;
		if (hz < kBandLowHz || hz >= kBandHighHz) {
			continue;
		}
		const auto b = static_cast<size_t>(std::log2(hz / kBandLowHz) / span * kSpectrumBands);
		const double mag = std::sqrt(spectrum_.p[2 * i] * spectrum_.p[2 * i] + spectrum_.p[2 * i + 1] * spectrum_.p[2 * i + 1]) * toFs;
		const auto db = static_cast<float>(6.020599913279624 * fastLog2(std::max(mag, 1e-6))); // 20 log10, fast (Z1b)
		bands_[std::min(b, bands_.size() - 1)] = std::max(bands_[std::min(b, bands_.size() - 1)], db);
	}
	++analysed_;
}

void Stft::computeFrame(int channels) noexcept {
	PFFFT_Setup* setup = setups_[static_cast<size_t>(resolution_)];
	const int n = n_;
	const int h = n / kOverlap;
	// THE VOCODER (design §10.3): the sidechain's envelope once a frame, while it acts or fades out.
	const bool vocoding = vocodeBusy();
	if (vocoding) {
		(void)sidechainFrame(0);
		if (vocode_.stereo == 1) {
			(void)sidechainFrame(1);
		}
	}
	// AN INTERNAL CARRIER (design §10.7, M5) stands in for the input while Vocode is on. While the vocoder crossfades in or out,
	// the fade's other side is the INPUT, not the raw carrier: its spectrum is kept and blended with the vocoded carrier's.
	const bool carrier = internalCarrier();
	const bool blend = carrier && vocodeMix_ < 1.0f;
	for (int c = 0; c < channels; ++c) {
		Channel& ch = ch_[static_cast<size_t>(c)];
		bool dryAny = false;
		if (blend) {
			dryAny = loadFrame(c, false);
			if (dryAny) {
				pffft_transform_ordered(setup, frame_.p, drySpectrum_.p, work_.p, PFFFT_FORWARD);
			}
		}
		const bool carrierAny = loadFrame(c, carrier);
		const bool any = carrierAny || dryAny;
		// A FRAME OF ZEROS IS WRITTEN AS ZEROS, without a transform: the tail ends at exact zero by construction.
		if (!any && c == 0 && analyse_) {
			bands_.fill(kBandFloorDb);
		}
		if (any) {
			if (carrierAny) {
				pffft_transform_ordered(setup, frame_.p, spectrum_.p, work_.p, PFFFT_FORWARD);
			}
			else {
				std::fill_n(spectrum_.p, n, 0.0f);
			}
			if (c == 0 && analyse_) {
				measureBands(); // what is shaped (the input, or the internal carrier), before the gains, while a card listens
			}
			// PARTIALS FIRST (design §11): it simplifies what Formant Shift and Vocode then shape. The first channel chooses.
			if (partials_.on) {
				partialsSpectrum(c == 0);
			}
			if (vocoding) {
				vocodeSpectrum(blend ? 1.0f : vocodeMix_, channels == 2 ? c : -1);
				if (blend) {
					const float m = vocodeMix_;
					for (int j = 0; j < n; ++j) {
						spectrum_.p[j] = m * spectrum_.p[j] + (dryAny ? (1.0f - m) * drySpectrum_.p[j] : 0.0f);
					}
				}
			}
			else if (fshiftOn_) {
				shiftSpectrum();
			}
			// pffft's ordered real layout: [DC, Nyquist, re1, im1, re2, im2, ...].
			spectrum_.p[0] *= gains_[0];
			spectrum_.p[1] *= gains_[static_cast<size_t>(n / 2)];
			for (int i = 1; i < n / 2; ++i) {
				const float g = gains_[static_cast<size_t>(i)];
				spectrum_.p[2 * i] *= g;
				spectrum_.p[2 * i + 1] *= g;
			}
			pffft_transform_ordered(setup, spectrum_.p, frame_.p, work_.p, PFFFT_BACKWARD);
			for (int j = 0; j < n; ++j) {
				ch.ola[static_cast<size_t>(j)] += frame_.p[j] * synthesisWindow_[static_cast<size_t>(j)];
			}
		}
		std::copy_n(ch.ola.begin(), h, ch.out.begin());
		std::memmove(ch.ola.data(), ch.ola.data() + h, static_cast<size_t>(n - h) * sizeof(float));
		std::fill_n(ch.ola.begin() + (n - h), h, 0.0f);
	}
	++frames_;
	++framesSinceStart_;
	// THE VOCODER'S CROSSFADE, a step a frame: one window from off to on, or back.
	const float step = 1.0f / static_cast<float>(kOverlap);
	vocodeMix_ = vocodeActive() ? std::min(1.0f, vocodeMix_ + step) : std::max(0.0f, vocodeMix_ - step);
}

void Stft::process(float* left, float* right, int frames) noexcept {
	if (left == nullptr || frames <= 0 || n_ == 0) {
		return;
	}
	const int channels = right != nullptr ? 2 : 1;
	float* io[2] = { left, right };
	const int h = n_ / kOverlap;
	mixFresh_ = false;
	for (int i = 0; i < frames; ++i) {
		if (!identity_ && !running_) {
			// STARTING: the overlap-add holds nothing yet, so the output stays dry until N/H frames have overlapped.
			running_ = true;
			framesSinceStart_ = 0;
			for (auto& c : ch_) {
				std::fill(c.ola.begin(), c.ola.end(), 0.0f);
				std::fill(c.out.begin(), c.out.end(), 0.0f);
			}
		}
		const float target = !identity_ && framesSinceStart_ >= kOverlap ? 1.0f : 0.0f;
		if (mixLeft_ > 0) {
			mix_ = --mixLeft_ == 0 ? mixTarget_ : mix_ + mixStep_;
		}
		const float m = mix_ * w_;
		for (int c = 0; c < channels; ++c) {
			Channel& ch = ch_[static_cast<size_t>(c)];
			ch.history[write_] = io[c][i];
			const float dry = ch.history[(write_ - latency_) & mask_];
			if (w_ == 0.0f) {
				io[c][i] = dry;
			}
			else {
				// dry·(1 − m) + wet·m, NOT dry + (wet − dry)·m: at m = 1 the first is the wet to the bit.
				io[c][i] = dry * (1.0f - m) + ch.out[static_cast<size_t>(outPos_)] * m;
			}
		}
		// THE SIDECHAIN, mono, at the same index as the input: a frame reads both from one instant.
		if (vocode_.stereo == 1) {
			// STEREO: each channel to its own ring.
			side_[0].history[write_] = sideL_ != nullptr ? sideL_[i] : 0.0f;
			side_[1].history[write_] = sideR_ != nullptr ? sideR_[i] : (sideL_ != nullptr ? sideL_[i] : 0.0f);
		}
		else {
			side_[0].history[write_] = sideL_ != nullptr && sideR_ != nullptr ? 0.5f * (sideL_[i] + sideR_[i]) : 0.0f;
		}
		// AND THE INTERNAL CARRIER, at the same index again, so the frame it is read into is the same instant.
		carrierHistory_[write_] = internalCarrier() ? nextCarrier() : 0.0f;
		// COUNTED IN WHOLE SAMPLES, not summed in floats: a float sum of H steps of 1/H can fall short of 1 and take a step more.
		wSteps_ = target > 0.0f ? std::min(wSteps_ + 1, h) : std::max(wSteps_ - 1, 0);
		w_ = static_cast<float>(wSteps_) / static_cast<float>(h);
		++outPos_;
		write_ = (write_ + 1) & mask_;
		if (++hopCount_ == h) {
			hopCount_ = 0;
			outPos_ = 0;
			if (running_) {
				if (identity_ && w_ == 0.0f) {
					running_ = false;
				}
				else {
					computeFrame(channels);
				}
			}
			if (!running_ && analyse_) {
				// THE CARD'S VIEW OF THE INPUT while nothing is being shaped: one forward transform, the left channel.
				if (loadFrame(0, false)) {
					pffft_transform_ordered(setups_[static_cast<size_t>(resolution_)], frame_.p, spectrum_.p, work_.p, PFFFT_FORWARD);
					measureBands();
				}
				else {
					bands_.fill(kBandFloorDb);
				}
			}
		}
	}
	// THE HOST'S SIDECHAIN BUFFERS END WITH THIS CALL: nothing may read them after it.
	sideL_ = nullptr;
	sideR_ = nullptr;
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

void Stft::primeFrozen(int k, const float* dbfs, float flatness) noexcept {
	Side& sd = side_[static_cast<size_t>(k)];
	const double toFs = envelopeToFs();
	for (size_t g = 0; g < sd.gridS.size(); ++g) {
		sd.gridS[g] = std::max(kEnvelopeFloorDb, static_cast<float>(static_cast<double>(dbfs[g]) - toFs));
	}
	sd.flatness = flatness;
	sd.primed = true;
}

bool Stft::sidechainFrame(int k) noexcept {
	Side& sd = side_[static_cast<size_t>(k)];
	PFFFT_Setup* setup = setups_[static_cast<size_t>(resolution_)];
	const int n = n_;
	const int bins = n / 2 + 1;
	const double binHz = sampleRate_ / static_cast<double>(n);
	bool any = false;
	// FROZEN (design §10.9): the envelope stays as it was; only what is imposed from it is rebuilt below.
	const bool frozen = vocode_.freeze && sd.primed;
	if (!frozen) {
		// THE SIDECHAIN'S FRAME, from the same instant as the input's (the same `pad_` behind its newest sample).
		const size_t start = (write_ - static_cast<size_t>(pad_) - static_cast<size_t>(n)) & mask_;
		for (int j = 0; j < n; ++j) {
			const float x = sd.history[(start + static_cast<size_t>(j)) & mask_];
			any = any || x != 0.0f;
			sideSpectrum_.p[j] = x * analysisWindow_[static_cast<size_t>(j)];
		}
		std::array<float, kEnvelopePoints> now {};
		if (any) {
			pffft_transform_ordered(setup, sideSpectrum_.p, sideSpectrum_.p, work_.p, PFFFT_FORWARD);
			mag_[0] = std::fabs(sideSpectrum_.p[0]);
			mag_[static_cast<size_t>(n / 2)] = std::fabs(sideSpectrum_.p[1]);
			for (int i = 1; i < n / 2; ++i) {
				mag_[static_cast<size_t>(i)] = std::sqrt(sideSpectrum_.p[2 * i] * sideSpectrum_.p[2 * i] + sideSpectrum_.p[2 * i + 1] * sideSpectrum_.p[2 * i + 1]);
			}
			estimator_.estimate(mag_.data(), bins, binHz, vocode_.bands, now);
			// HOW NOISY ABOVE 2 kHz: the spectral flatness (geometric over arithmetic mean of the power), 0 for a tone, 1 for noise.
			double logSum = 0.0;
			double sum = 0.0;
			int count = 0;
			for (int i = 1; i < n / 2; ++i) {
				if (static_cast<double>(i) * binHz < 2000.0) {
					continue;
				}
				const double p = static_cast<double>(mag_[static_cast<size_t>(i)]) * static_cast<double>(mag_[static_cast<size_t>(i)]);
				logSum += 0.6931471805599453 * fastLog2(p + 1e-30); // ln, fast (Z1b)
				sum += p;
				++count;
			}
			sd.flatness = count > 0 && sum > 0.0 ? static_cast<float>(std::exp(logSum / count) / (sum / count)) : 0.0f;
		}
		else {
			now.fill(kEnvelopeFloorDb);
			sd.flatness = 0.0f;
		}
		// ATTACK AND RELEASE across frames, per grid point, in dB: a one-pole each way at the hop's rate.
		const double hopSeconds = static_cast<double>(n / kOverlap) / sampleRate_;
		const double attack = std::exp(-hopSeconds / std::max(vocode_.attackMs * 1e-3, 1e-6));
		const double release = std::exp(-hopSeconds / std::max(vocode_.releaseMs * 1e-3, 1e-6));
		for (size_t g = 0; g < now.size(); ++g) {
			const double target = now[g];
			const double was = sd.primed ? static_cast<double>(sd.gridS[g]) : target;
			const double coef = target > was ? attack : release;
			sd.gridS[g] = std::max(kEnvelopeFloorDb, static_cast<float>(target + coef * (was - target)));
		}
		sd.primed = true;
	}
	// WHAT IS IMPOSED (design §10.9): the envelope, held outside Low..High and closed below the Gate. A copy only while either acts,
	// so the defaults impose sd.gridS itself, bit for bit as before.
	// (WITHIN A HUNDREDTH OF A PERCENT OF THE ENDS is the ends: the stored control is log2 Hz, and exp2 of log2(20) is not 20.)
	const bool ranged = vocode_.lowHz > kEnvelopeLowHz * 1.0001 || vocode_.highHz < 20000.0 * 0.9999; // the control's top, 20 kHz
	const bool gated = vocode_.gateDb > -96.0 + 1e-6; // at its bottom, off (and the card says "Off")
	const std::array<float, kEnvelopePoints>& imposed = ranged || gated ? sd.gridE : sd.gridS;
	if (ranged || gated) {
		const double low = EnvelopeEstimator::sampleDb(sd.gridS, vocode_.lowHz);
		const double high = EnvelopeEstimator::sampleDb(sd.gridS, std::max(vocode_.highHz, vocode_.lowHz));
		// THE GATE IN dBFS, the scale the card's bands are drawn on (the FFT's magnitude scaled by pi / N).
		const double toFs = 20.0 * std::log10(3.141592653589793 / static_cast<double>(std::max(1, n)));
		for (int g = 0; g < kEnvelopePoints; ++g) {
			const double hz = envelopeGridHz(g);
			double db = hz < vocode_.lowHz ? low : hz > vocode_.highHz ? high : static_cast<double>(sd.gridS[static_cast<size_t>(g)]);
			if (gated && db + toFs < vocode_.gateDb) {
				db = kEnvelopeFloorDb;
			}
			sd.gridE[static_cast<size_t>(g)] = static_cast<float>(db);
		}
	}
	sd.copy = ranged || gated;
	sd.maxDb = *std::max_element(imposed.begin(), imposed.end());
	estimator_.toBinsFast(imposed, bins, binHz, sd.env.data());
	// FORMANT SHIFT, VOCODING: the sidechain's envelope read at hz / ratio, so it moves before it is imposed. AND THE FROZEN
	// ENVELOPE'S TRANSPOSITION (Z10) on the same re-read: the two ratios multiply (their semitones add). Outside the grid the edge
	// values hold (`sampleDb`), as they do for Formant Shift.
	const double transpose = frozen && freezeShift_ != 0.0 ? std::exp2(freezeShift_ / 12.0) : 1.0;
	if (fshiftOn_ || transpose != 1.0) {
		for (int i = 0; i < bins; ++i) {
			const double hz = static_cast<double>(i) * binHz;
			const double r = std::max((fshiftOn_ ? static_cast<double>(fshiftRatio_[static_cast<size_t>(i)]) : 1.0) * transpose, 1e-6);
			const double db = EnvelopeEstimator::sampleDb(imposed, hz / r);
			sd.env[static_cast<size_t>(i)] = db <= static_cast<double>(kEnvelopeFloorDb) ? 0.0f : static_cast<float>(fastExp2(db * 0.16609640474436813));
		}
	}
	// BANDWIDTH BELOW 1 (design §10.9): the envelope imposed through discrete bands; at 1 the smooth envelope, untouched.
	if (banded()) {
		bandEnvelope(sd, bins, binHz);
	}
	return any;
}

void Stft::vocodeSpectrum(float mix, int c) noexcept {
	// STEREO (design §10.9): each channel by its own sidechain channel; otherwise both by the one.
	const Side& sd = side_[vocode_.stereo == 1 && c == 1 ? 1 : 0];
	const int n = n_;
	const int bins = n / 2 + 1;
	const double binHz = sampleRate_ / static_cast<double>(n);
	// THE CARRIER'S OWN ENVELOPE, this frame (no smoothing across frames: it flattens what is there now).
	mag_[0] = std::fabs(spectrum_.p[0]);
	mag_[static_cast<size_t>(n / 2)] = std::fabs(spectrum_.p[1]);
	for (int i = 1; i < n / 2; ++i) {
		mag_[static_cast<size_t>(i)] = std::sqrt(spectrum_.p[2 * i] * spectrum_.p[2 * i] + spectrum_.p[2 * i + 1] * spectrum_.p[2 * i + 1]);
	}
	// BY ENERGY PER BAND, as a filterbank measures its carrier: a dB mean counted the gaps between a harmonic
	// carrier's partials, more of them per kernel the higher it looked, and left the output tilted (a Square 6.2 dB bright by 12.8 kHz).
	estimator_.estimate(mag_.data(), bins, binHz, vocode_.bands, gridX_, true);
	estimator_.toBinsFast(gridX_, bins, binHz, envX_.data());
	// HARMONIC: the carrier's loudest member per group, for the next frame's layout (the first channel's carrier).
	if ((vocode_.zone == 4 || vocode_.zone == 5 || vocode_.zone == 6) && c <= 0) {
		pickMembers();
	}
	// ON THE BANDED ROAD (Bandwidth below 1) THE CARRIER IS MEASURED PER BAND, on the weights the sidechain's envelope is imposed
	// with (§10.7.1); the smooth road keeps the energy envelope above, bit for bit.
	if (banded()) {
		rebuildBands(bins, binHz);
		bandCarrier(bins);
	}

	const float flatten = std::clamp(vocode_.flatten, 0.0f, 1.0f);
	// THE GAIN PER BIN: E_x^(−Flatten) · E_s, crossfaded with 1 by the mix. DEPTH (design §10.9) scales that gain in dB up to 1
	// (0 leaves the carrier as it is) and past 1 deepens the envelope below its peak (only its loudest parts come through); at
	// exactly 1 it is the expression as it always was, bit for bit.
	const double depth = std::clamp(vocode_.depth, 0.0, 2.0);
	// SPREAD (design §10.9): the bands alternate between the channels, band b (centred at u = b) to the left when even. The left
	// is weighted sqrt(2)·abs(cos(pi u / 2)), the right sqrt(2)·abs(sin(pi u / 2)), so the two together keep the power.
	// BY BAND ORDER IN THE LAYOUT (decision 6, §10.7.1): band b centred at u = b on the layout's band coordinate (whatever the Zone),
	// to the left when even — sqrt(2)·abs(cos(pi u / 2)) left and sqrt(2)·abs(sin(pi u / 2)) right, so the two keep the power.
	const bool spread = vocode_.stereo == 2 && c >= 0;
	if (spread) {
		rebuildBands(bins, binHz);
	}
	const auto spreadAt = [&](size_t i) {
		const double u = static_cast<double>(bandU_[i]);
		const double w = c == 0 ? std::cos(1.5707963267948966 * u) : std::sin(1.5707963267948966 * u);
		return static_cast<float>(1.4142135623730951 * std::fabs(w));
	};
	const auto gainAt = [&](size_t i) {
		const float ex = envX_[i];
		float voc = 0.0f;
		if (depth == 1.0) {
			// E_x^(-Flatten): a division at the default Flatten 1, the fast pair otherwise (Z1b).
			voc = ex > 0.0f ? (flatten == 1.0f ? sd.env[i] / ex : static_cast<float>(fastExp2(-static_cast<double>(flatten) * fastLog2(ex))) * sd.env[i]) : 0.0f;
		}
		else if (depth == 0.0) {
			voc = 1.0f;
		}
		else if (ex > 0.0f && sd.env[i] > 0.0f) {
			const double es = 6.020599913279624 * fastLog2(static_cast<double>(sd.env[i]));
			double db = -static_cast<double>(flatten) * 6.020599913279624 * fastLog2(static_cast<double>(ex)) + es;
			db = depth < 1.0 ? db * depth : db + (depth - 1.0) * (es - static_cast<double>(sd.maxDb));
			voc = static_cast<float>(fastExp2(db * 0.16609640474436813));
		}
		if (spread) {
			voc *= spreadAt(i);
		}
		return 1.0f + mix * (voc - 1.0f);
	};
	spectrum_.p[0] *= gainAt(0);
	spectrum_.p[1] *= gainAt(static_cast<size_t>(n / 2));
	const float noise = mix * std::clamp(vocode_.sibilance, 0.0f, 1.0f) * sd.flatness;
	for (int i = 1; i < n / 2; ++i) {
		const auto u = static_cast<size_t>(i);
		const float g = gainAt(u);
		spectrum_.p[2 * i] *= g;
		spectrum_.p[2 * i + 1] *= g;
		// SIBILANCE: noise of random phase at the sidechain's level, above 2 kHz, as noisy as the sidechain is there.
		if (noise > 0.0f && static_cast<double>(i) * binHz >= 2000.0) {
			noiseRng_ ^= noiseRng_ << 13;
			noiseRng_ ^= noiseRng_ >> 17;
			noiseRng_ ^= noiseRng_ << 5;
			const double phase = 6.283185307179586 * static_cast<double>(noiseRng_) / 4294967296.0;
			const float amp = noise * sd.env[u] * (spread ? spreadAt(u) : 1.0f);
			spectrum_.p[2 * i] += amp * static_cast<float>(std::cos(phase));
			spectrum_.p[2 * i + 1] += amp * static_cast<float>(std::sin(phase));
		}
	}
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

void Stft::shiftSpectrum() noexcept {
	const int n = n_;
	const int bins = n / 2 + 1;
	const double binHz = sampleRate_ / static_cast<double>(n);
	// THE INPUT'S OWN ENVELOPE, this frame (the vocoder's estimator, at its Bands).
	mag_[0] = std::fabs(spectrum_.p[0]);
	mag_[static_cast<size_t>(n / 2)] = std::fabs(spectrum_.p[1]);
	for (int i = 1; i < n / 2; ++i) {
		mag_[static_cast<size_t>(i)] = std::sqrt(spectrum_.p[2 * i] * spectrum_.p[2 * i] + spectrum_.p[2 * i + 1] * spectrum_.p[2 * i + 1]);
	}
	estimator_.estimate(mag_.data(), bins, binHz, vocode_.bands, gridX_);
	// THE ENVELOPE MOVES, THE PARTIALS STAY: each bin keeps its phase and frequency, scaled from E(hz) to E(hz / r).
	const auto gainAt = [&](int i) {
		const double hz = static_cast<double>(i) * binHz;
		const double r = std::max(static_cast<double>(fshiftRatio_[static_cast<size_t>(i)]), 1e-6);
		const double here = EnvelopeEstimator::sampleDb(gridX_, hz);
		const double there = EnvelopeEstimator::sampleDb(gridX_, hz / r);
		return there <= static_cast<double>(kEnvelopeFloorDb) ? 0.0f : static_cast<float>(fastExp2((there - here) * 0.16609640474436813));
	};
	spectrum_.p[0] *= gainAt(0);
	spectrum_.p[1] *= gainAt(n / 2);
	for (int i = 1; i < n / 2; ++i) {
		const float g = gainAt(i);
		spectrum_.p[2 * i] *= g;
		spectrum_.p[2 * i + 1] *= g;
	}
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

namespace {
/// polyBLEP's correction for a step at phase 0, `t` the phase and `dt` the phase increment (Välimäki's two-sample residual).
float polyBlep(double t, double dt) noexcept {
	if (t < dt) {
		t /= dt;
		return static_cast<float>(t + t - t * t - 1.0);
	}
	if (t > 1.0 - dt) {
		t = (t - 1.0) / dt;
		return static_cast<float>(t * t + t + t + 1.0);
	}
	return 0.0f;
}
} // namespace

float Stft::nextCarrier() noexcept {
	constexpr float kLevel = 0.5f; // -6 dBFS: Flatten takes the level out, and at Flatten 0 this is the carrier's colour
	if (vocode_.carrier == 1) {
		// A BAND-LIMITED SQUARE at the note (polyBLEP at both edges), held below Nyquist — never moved by Offset (Z9).
		const double dt = std::clamp(carrierHz_ / sampleRate_, 0.0, 0.5);
		float v = carrierPhase_ < 0.5 ? 1.0f : -1.0f;
		v += polyBlep(carrierPhase_, dt);
		v -= polyBlep(std::fmod(carrierPhase_ + 0.5, 1.0), dt);
		carrierPhase_ += dt;
		if (carrierPhase_ >= 1.0) {
			carrierPhase_ -= 1.0;
		}
		return kLevel * v;
	}
	// THE NOISES: one xorshift, uniform in [-1, 1); deterministic from reset, so the partition sweep holds.
	carrierRng_ ^= carrierRng_ << 13;
	carrierRng_ ^= carrierRng_ >> 17;
	carrierRng_ ^= carrierRng_ << 5;
	const float white = static_cast<float>(carrierRng_) * (2.0f / 4294967296.0f) - 1.0f;
	if (vocode_.carrier == 2) {
		return kLevel * white;
	}
	// PINK BY KELLET'S FIXED FILTER: six one-poles and a direct term, -3 dB/oct within ±0.05 dB from 9 Hz to Nyquist at 44.1k.
	auto& b = pink_;
	b[0] = 0.99886f * b[0] + white * 0.0555179f;
	b[1] = 0.99332f * b[1] + white * 0.0750759f;
	b[2] = 0.96900f * b[2] + white * 0.1538520f;
	b[3] = 0.86650f * b[3] + white * 0.3104856f;
	b[4] = 0.55000f * b[4] + white * 0.5329522f;
	b[5] = -0.7616f * b[5] - white * 0.0168980f;
	const float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
	b[6] = white * 0.115926f;
	return kLevel * 0.11f * pink;
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

void Stft::partialsSpectrum(bool choose) noexcept {
	const int n = n_;
	const int bins = n / 2 + 1;
	const double count = std::clamp(partials_.count, kPartialsMin, kPartialsMax);
	const int whole = static_cast<int>(std::ceil(count - 1e-9)); // the partials touched; the last weighted by its fraction
	const float lastWeight = static_cast<float>(count - static_cast<double>(whole - 1));
	const float depth = std::clamp(partials_.depth, 0.0f, 1.0f);
	const float rest = 1.0f - depth;
	if (choose) {
		std::fill_n(partialsGain_.begin(), bins, rest);
		// EACH KEPT PARTIAL'S MAIN LOBE (sqrt-Hann: two bins either side) is raised to its weight.
		const auto keep = [&](int centre, float weight) {
			const float g = rest + weight * depth;
			for (int i = std::max(0, centre - kPartialsLobe); i <= std::min(bins - 1, centre + kPartialsLobe); ++i) {
				partialsGain_[static_cast<size_t>(i)] = std::max(partialsGain_[static_cast<size_t>(i)], g);
			}
		};
		if (partials_.mode == 2) {
			// CENTERS: the harmonics of f0 at the centre set (1 and a few of 2..7), each kept by its weight from the shared generator.
			const double binHz = sampleRate_ / static_cast<double>(n);
			for (int k = 1; k <= 7; ++k) {
				const auto centre = static_cast<int>(std::lround(static_cast<double>(k) * f0Hz_ / binHz));
				if (centre <= 0 || centre >= bins - 1) {
					break;
				}
				const auto w = static_cast<float>(partials::sparseKeep(k, partials_.centersSet));
				if (w > 0.0f) {
					keep(centre, w);
				}
			}
		}
		else if (partials_.mode == 1) {
			// HARMONICS 1..N OF f0, at the nearest bin; the ones past Nyquist are simply not there.
			const double binHz = sampleRate_ / static_cast<double>(n);
			for (int k = 1; k <= whole; ++k) {
				const double hz = static_cast<double>(k) * f0Hz_;
				const auto centre = static_cast<int>(std::lround(hz / binHz));
				if (centre <= 0 || centre >= bins - 1) {
					break;
				}
				keep(centre, k == whole ? lastWeight : 1.0f);
			}
		}
		else {
			// THE N STRONGEST LOCAL MAXIMA of |X|², strongest first; the Nth weighted by the fraction.
			const auto power = [&](int i) {
				return spectrum_.p[2 * i] * spectrum_.p[2 * i] + spectrum_.p[2 * i + 1] * spectrum_.p[2 * i + 1];
			};
			int found = 0;
			float before = 0.0f;
			float here = power(1);
			for (int i = 1; i < n / 2 - 1; ++i) {
				const float after = power(i + 1);
				if (here > 0.0f && here > before && here >= after) {
					peaks_[static_cast<size_t>(found++)] = i;
				}
				before = here;
				here = after;
			}
			const int kept = std::min(whole, found);
			const auto stronger = [&](int a, int b) { return power(a) > power(b); };
			// THE TOP SET AND ITS WEAKEST, NOT A SORT (M6c): nth_element puts the kept-th strongest at kept - 1 and the stronger ones
			// before it in no order, which is all the mask needs: `keep` takes a maximum per bin, so the order it is applied in cannot
			// matter, and only the weakest kept peak carries the fraction. Linear, where partial_sort was N log N in the kept count.
			if (kept > 0) {
				std::nth_element(peaks_.begin(), peaks_.begin() + (kept - 1), peaks_.begin() + found, stronger);
			}
			for (int r = 0; r < kept; ++r) {
				keep(peaks_[static_cast<size_t>(r)], r == whole - 1 ? lastWeight : 1.0f);
			}
		}
	}
	spectrum_.p[0] *= partialsGain_[0];
	spectrum_.p[1] *= partialsGain_[static_cast<size_t>(n / 2)];
	for (int i = 1; i < n / 2; ++i) {
		const float g = partialsGain_[static_cast<size_t>(i)];
		spectrum_.p[2 * i] *= g;
		spectrum_.p[2 * i + 1] *= g;
	}
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

void Stft::rebuildBands(int bins, double binHz) noexcept {
	// THE LAYOUT (design §10.7.2): `bands` of them (rounded; the count is always Bands' — decision 5), their centres and widths laid by
	// the shared layout (dsp/vocode_bands.h: the Zone, Bandwidth, the Widths zone). Rebuilt only when what it depends on moves, so a
	// still layout costs nothing per frame.
	const int count = static_cast<int>(std::lround(std::clamp(vocode_.bands, kMinBands, kMaxBands)));
	const double low = std::clamp(vocode_.lowHz, kEnvelopeLowHz, 20000.0);
	const double high = std::max(std::clamp(vocode_.highHz, kEnvelopeLowHz, 20000.0), low * 1.01);
	const double bw = std::clamp(vocode_.bandwidth, 0.0, 1.0);
	// 0 Even, 1 Speech, 2 Low, 3 High, 4 Harmonic, 5 Inharmonic — each an amount 0..1 from Even (two entries) —
	// and 6 Free, whose own UNBOUNDED coordinate (Z7) is decoded into a mix of the named axes (freeAxes).
	const int zone = std::clamp(vocode_.zone, 0, 6);
	const double amount = zoneDrive();
	const feathers::spectral::BandSpec spec { count, low, high, bw, zone, amount, vocode_.widthZone, widthDrive(), f0Hz_ };
	bool inharmonic = false;
	const bool harmonic = feathers::spectral::layoutHarmonic(spec, &inharmonic);
	if (harmonic) {
		regroup(count, low, high, inharmonic);
	}
	const BandKey key { count, bw, low, high, binHz, bins, zone, amount, harmonic ? grouping_.f0() : 0.0, harmonic ? pickVersion_ : 0,
	                    vocode_.widthZone, widthDrive(), vocode_.widthZone == 3 || vocode_.widthZone == 4 || vocode_.widthZone == 5 ? f0Hz_ : 0.0 };
	if (key == bandKey_) {
		return;
	}
	bandKey_ = key;
	bandCount_ = count;
	feathers::spectral::layoutBands(spec, harmonic ? &grouping_ : nullptr, bandCentre_.data(), bandSigma_.data());
	for (int b = 0; b < count; ++b) {
		bandCentreBin_[static_cast<size_t>(b)] =
		    std::clamp(static_cast<int>(std::lround(std::exp2(bandCentre_[static_cast<size_t>(b)]) / binHz)), 0, bins - 1);
	}
	// PARTITION OF UNITY (DECIDED, §10.7.1): at every bin the weights of the bands within reach (the nearest and four either side),
	// each Gaussian divided by their sum at that bin, so wherever bands crowd or widen their shares shrink together and level and
	// tilt never move. Outside the outermost centres a bin is weighed as if at that centre: the edge, held, whatever the widths.
	int nearest = 0;
	for (int i = 0; i < bins; ++i) {
		const double hz = std::max(static_cast<double>(i) * binHz, kEnvelopeLowHz);
		const double x = std::clamp(std::log2(hz), bandCentre_[0], bandCentre_[static_cast<size_t>(count - 1)]);
		while (nearest + 1 < count
		       && std::fabs(bandCentre_[static_cast<size_t>(nearest + 1)] - x) <= std::fabs(bandCentre_[static_cast<size_t>(nearest)] - x)) {
			++nearest;
		}
		// THE BAND COORDINATE, for Spread: linear between the centres either side.
		{
			const int j = x >= bandCentre_[static_cast<size_t>(nearest)] ? nearest : std::max(nearest - 1, 0);
			const double gap = j + 1 < count ? bandCentre_[static_cast<size_t>(j + 1)] - bandCentre_[static_cast<size_t>(j)] : 0.0;
			bandU_[static_cast<size_t>(i)] = static_cast<float>(j + (gap > 0.0 ? (x - bandCentre_[static_cast<size_t>(j)]) / gap : 0.0));
		}
		const int reach = (kBandReach - 1) / 2;
		const int first = std::max(0, nearest - reach);
		const int last = std::min(count - 1, nearest + reach);
		float* w = &bandW_[static_cast<size_t>(i) * kBandReach];
		std::fill_n(w, kBandReach, 0.0f);
		std::array<double, kBandReach> g {};
		double sum = 0.0;
		for (int b = first; b <= last; ++b) {
			const double d = (x - bandCentre_[static_cast<size_t>(b)]) / bandSigma_[static_cast<size_t>(b)];
			g[static_cast<size_t>(b - first)] = std::exp(-0.5 * d * d);
			sum += g[static_cast<size_t>(b - first)];
		}
		if (sum < 1e-30) {
			w[nearest - first] = 1.0f;
		}
		else {
			for (int b = first; b <= last; ++b) {
				w[b - first] = static_cast<float>(g[static_cast<size_t>(b - first)] / sum);
			}
		}
		bandFirst_[static_cast<size_t>(i)] = first;
	}
}

void Stft::regroup(int count, double low, double high, bool inharmonic) noexcept {
	if (grouping_.regroup(count, low, high, inharmonic, f0Hz_)) {
		++groupings_;
		++pickVersion_;
	}
}

void Stft::pickMembers() noexcept {
	// EACH GROUP CENTRED ON ITS LOUDEST MEMBER (the quietest gap, inharmonic) in the carrier's spectrum this frame (mag_): the
	// next frame's layout follows. Only within groups; a pick that moves bumps the version, so the layout rebuilds then and only then.
	if (!grouping_.grouped() || grouping_.members() <= 0) {
		return;
	}
	const double binHz = sampleRate_ / static_cast<double>(n_);
	const int bins = n_ / 2 + 1;
	const bool inharmonic = grouping_.inharmonic();
	const auto level = [&](int m) {
		const int bin = std::clamp(static_cast<int>(std::lround(memberHz(m) / binHz)), 0, bins - 1);
		double p = static_cast<double>(mag_[static_cast<size_t>(bin)]);
		if (!inharmonic) { // a harmonic's peak may sit a bin either side of its nominal one
			p = std::max({ p, static_cast<double>(mag_[static_cast<size_t>(std::max(bin - 1, 0))]),
			               static_cast<double>(mag_[static_cast<size_t>(std::min(bin + 1, bins - 1))]) });
		}
		return p;
	};
	bool moved = false;
	for (int b = 0; b < grouping_.bands(); ++b) {
		int best = grouping_.pick(b);
		double bestLevel = level(best);
		for (int m = grouping_.first(b); m <= grouping_.last(b); ++m) {
			const double l = level(m);
			if (inharmonic ? l < bestLevel : l > bestLevel) {
				best = m;
				bestLevel = l;
			}
		}
		moved = grouping_.setPick(b, best) || moved;
	}
	if (moved) {
		++pickVersion_;
	}
}
void Stft::bandEnvelope(Side& sd, int bins, double binHz) noexcept {
	// THE ENVELOPE THROUGH THE BANDS: each band's level is the envelope at its centre (after Formant Shift, which has already moved
	// it), and each bin takes the weighted sum of its bands' levels. With the weights a partition of unity, a flat sidechain
	// comes out flat at its own level, edges included, at any count, Range or Bandwidth.
	rebuildBands(bins, binHz);
	std::array<float, static_cast<size_t>(kMaxBands) + 1> level {};
	for (int b = 0; b < bandCount_; ++b) {
		level[static_cast<size_t>(b)] = sd.env[static_cast<size_t>(bandCentreBin_[static_cast<size_t>(b)])];
	}
	for (int i = 0; i < bins; ++i) {
		const float* w = &bandW_[static_cast<size_t>(i) * kBandReach];
		const int first = bandFirst_[static_cast<size_t>(i)];
		double sum = 0.0;
		for (int k = 0; k < kBandReach && first + k < bandCount_; ++k) {
			sum += static_cast<double>(w[k]) * static_cast<double>(level[static_cast<size_t>(first + k)]);
		}
		sd.env[static_cast<size_t>(i)] = static_cast<float>(sum);
	}
}

void Stft::bandCarrier(int bins) noexcept {
	// THE CARRIER'S ENERGY PER BAND, ON THE SAME WEIGHTS (§10.7.1: how partition of unity composes with energy Flatten): P_b the
	// W_b-weighted mean of |X|^2, and each bin's flattening amplitude 1 / sum_b W_b(f) P_b^(-1/2), so a carrier whose energy is
	// flat per band is left flat, and each band's output energy is the sidechain's band energy, band by band.
	std::array<double, static_cast<size_t>(kMaxBands) + 1> power {};
	std::array<double, static_cast<size_t>(kMaxBands) + 1> weight {};
	for (int i = 0; i < bins; ++i) {
		const float* w = &bandW_[static_cast<size_t>(i) * kBandReach];
		const int first = bandFirst_[static_cast<size_t>(i)];
		const double p = static_cast<double>(mag_[static_cast<size_t>(i)]) * static_cast<double>(mag_[static_cast<size_t>(i)]);
		for (int k = 0; k < kBandReach && first + k < bandCount_; ++k) {
			power[static_cast<size_t>(first + k)] += static_cast<double>(w[k]) * p;
			weight[static_cast<size_t>(first + k)] += static_cast<double>(w[k]);
		}
	}
	std::array<double, static_cast<size_t>(kMaxBands) + 1> inv {};
	for (int b = 0; b < bandCount_; ++b) {
		const auto u = static_cast<size_t>(b);
		inv[u] = 1.0 / std::sqrt(std::max(weight[u] > 0.0 ? power[u] / weight[u] : 0.0, 1e-12));
	}
	for (int i = 0; i < bins; ++i) {
		const float* w = &bandW_[static_cast<size_t>(i) * kBandReach];
		const int first = bandFirst_[static_cast<size_t>(i)];
		double s = 0.0;
		for (int k = 0; k < kBandReach && first + k < bandCount_; ++k) {
			s += static_cast<double>(w[k]) * inv[static_cast<size_t>(first + k)];
		}
		envX_[static_cast<size_t>(i)] = s > 0.0 ? static_cast<float>(1.0 / s) : 0.0f;
	}
}

float Stft::bandWeightSumForTest(int bin) const noexcept {
	if (bandW_.empty() || bin < 0 || static_cast<size_t>(bin) >= bandFirst_.size()) {
		return 0.0f;
	}
	double sum = 0.0;
	for (int k = 0; k < kBandReach; ++k) {
		sum += static_cast<double>(bandW_[static_cast<size_t>(bin) * kBandReach + static_cast<size_t>(k)]);
	}
	return static_cast<float>(sum);
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

std::array<double, 4> Stft::freeAxesForTest(double x) noexcept {
	const feathers::spectral::FreeAxes a = feathers::spectral::freeAxes(x);
	return { a.speech, a.low, a.high, a.harmonic };
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

std::array<double, 2> Stft::widthAxesForTest(double x) noexcept {
	const feathers::spectral::WidthAxes w = feathers::spectral::widthAxes(x);
	return { w.tilt, w.harmonic };
}

} // namespace feathers::fx::spectral
