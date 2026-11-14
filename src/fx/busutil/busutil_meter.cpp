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

#include "fx/busutil/busutil_meter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::busutil {

namespace {

/// **THE STANDARD'S ANALOGUE PARAMETERS, NOT ITS 48 kHz COEFFICIENTS.** BS.1770-4 tabulates the
/// filters as numbers at one rate; these are the values those numbers were derived FROM, so
/// designing at the host's rate reproduces the table at 48 kHz and is correct everywhere else.
constexpr double kShelfHz = 1681.974450955533;
constexpr double kShelfGainDb = 3.999843853973347;
constexpr double kShelfQ = 0.7071752369554196;
constexpr double kHighpassHz = 38.13547087602444;
constexpr double kHighpassQ = 0.5003270373238773;

/// THE OFFSET IN THE LOUDNESS EQUATION, BS.1770-4: `L = -0.691 + 10 log10(sum G_i * z_i)`.
constexpr double kLoudnessOffset = -0.691;

/// A STEREO PAIR WEIGHS 1.0 AND 1.0. The channel weights G_i are only above unity for surround
/// channels, which this device does not have and is not pretending to.
constexpr double kChannelWeight = 1.0;

/// HOW FAST THE PEAK READING FALLS — 20 dB per second, the broadcast convention. A peak that fell
/// instantly could not be read off a moving picture, and one that held forever would be a maximum
/// rather than a meter.
constexpr double kPeakFallDbPerSecond = 20.0;

[[nodiscard]] double loudnessOf(double meanSquareSum) noexcept {
	if (!(meanSquareSum > 0.0)) {
		return static_cast<double>(kSilenceLufs);
	}
	return kLoudnessOffset + 10.0 * std::log10(meanSquareSum);
}

/// THE INVERSE, for turning a bin's loudness back into the energy the gate has to average. Averaging
/// LOUDNESSES would be averaging logarithms, which is a different and wrong number.
[[nodiscard]] double energyOf(double lufs) noexcept {
	return std::pow(10.0, (lufs - kLoudnessOffset) / 10.0);
}

} // namespace

void KWeighting::design(double sampleRate) noexcept {
	const double fs = std::max(sampleRate, 8000.0);
	const double pi = std::numbers::pi_v<double>;

	// **STAGE 1 — THE BILINEAR SHELF THE STANDARD'S TABLE WAS GENERATED FROM, NOT THE RBJ COOKBOOK
	// ONE.** Both are "a high shelf at 1681.97 Hz, Q 0.707, +4 dB" and they are DIFFERENT FILTERS:
	// the RBJ form gave a1 = -1.65378 where Table 1 says -1.69066, which is a real difference in
	// where the shelf turns, not a rounding. The first draft here used RBJ for both stages on the
	// strength of the parameters matching, and the coefficient case is what caught it — which is
	// exactly the job that case exists for, since every loudness assertion in the file would have
	// gone on passing against the wrong filter measured correctly.
	//
	// `Vb`'s exponent is not a tidy number and is not supposed to be: it is part of the published
	// derivation, chosen so the shelf's midpoint lands where the standard wants it.
	{
		const double K = std::tan(pi * kShelfHz / fs);
		const double Vh = std::pow(10.0, kShelfGainDb / 20.0);
		const double Vb = std::pow(Vh, 0.4996667741545416);
		const double a0 = 1.0 + K / kShelfQ + K * K;

		shelf.b0 = (Vh + Vb * K / kShelfQ + K * K) / a0;
		shelf.b1 = 2.0 * (K * K - Vh) / a0;
		shelf.b2 = (Vh - Vb * K / kShelfQ + K * K) / a0;
		shelf.a1 = 2.0 * (K * K - 1.0) / a0;
		shelf.a2 = (1.0 - K / kShelfQ + K * K) / a0;
	}

	// STAGE 2 — THE RLB HIGHPASS, from the same bilinear derivation.
	{
		const double K = std::tan(pi * kHighpassHz / fs);
		const double a0 = 1.0 + K / kHighpassQ + K * K;
		// **THE NUMERATOR IS {1, -2, 1} AND NOT RBJ's {(1+cos)/2, -(1+cos), (1+cos)/2}, BECAUSE THE
		// STANDARD'S IS.** The poles are RBJ's — that is the rate-dependent half and it reproduces
		// Table 2's `a` exactly — but BS.1770-4 tabulates the RLB stage with a numerator of exactly
		// 1, -2, 1, which is a slightly different passband normalisation from RBJ's unity-at-Nyquist.
		// MEASURED: RBJ's scaling is 0.99503 of the standard's at 48 kHz, a flat -0.043 dB on every
		// loudness reading. Small, real, and wrong — a meter is a number people compare against other
		// people's numbers, so "close to the standard" is not a thing it may be.
		highpass.b0 = 1.0;
		highpass.b1 = -2.0;
		highpass.b2 = 1.0;
		highpass.a1 = 2.0 * (K * K - 1.0) / a0;
		highpass.a2 = (1.0 - K / kHighpassQ + K * K) / a0;
	}

	clear();
}

void Meter::prepare(double sampleRate) noexcept {
	sampleRate_ = std::max(sampleRate, 8000.0);
	// 100 ms, AT LEAST ONE SAMPLE. The floor is not defensive dressing: a block of zero samples would
	// close forever and divide by nothing.
	blockSamples_ = std::max(1, static_cast<int>(std::lround(sampleRate_ * 0.1)));
	kwLeft_.design(sampleRate_);
	kwRight_.design(sampleRate_);
	// A FALL TIME IN dB PER SECOND, CONVERTED TO A PER-SAMPLE FACTOR — so the meter falls at the same
	// visible speed whatever the host's rate, which is the same rule the device's own slew follows.
	peakDecay_ = static_cast<float>(std::pow(10.0, -kPeakFallDbPerSecond / (20.0 * sampleRate_)));
	// A TIME, IN SAMPLES — the same rule the ramp and the loudness blocks follow, so the hold is the
	// same duration whatever rate the host runs at.
	rawPeakWindow_ = std::max(1, static_cast<int>(std::lround(sampleRate_ * kRawPeakHoldSeconds)));
	reset();
}

void Meter::reset() noexcept {
	kwLeft_.clear();
	kwRight_.clear();
	blockFill_ = 0;
	sumSqL_ = sumSqR_ = sumLR_ = sumRawSqL_ = sumRawSqR_ = 0.0;
	msL_.fill(0.0);
	msR_.fill(0.0);
	corrNum_.fill(0.0);
	corrDenL_.fill(0.0);
	corrDenR_.fill(0.0);
	blockHead_ = 0;
	blocksSeen_ = 0;
	peakL_ = peakR_ = 0.0f;
	rawPeakAccum_ = rawPeakHeld_ = 0.0f;
	rawPeakSamples_ = 0;
	clipAge_ = kClipAgeMax;
	resetIntegrated();
}

void Meter::resetIntegrated() noexcept {
	histogram_.fill(0u);
	gatedSumEnergy_ = 0.0;
	gatedCount_ = 0;
	++integratedScans_;
	integrated_ = computeIntegrated();
}

void Meter::closeBlock() noexcept {
	const double n = static_cast<double>(blockSamples_);
	msL_[blockHead_] = sumSqL_ / n;
	msR_[blockHead_] = sumSqR_ / n;
	corrNum_[blockHead_] = sumLR_;
	corrDenL_[blockHead_] = sumRawSqL_;
	corrDenR_[blockHead_] = sumRawSqR_;
	blockHead_ = (blockHead_ + 1) % kBlocksShort;
	++blocksSeen_;
	sumSqL_ = sumSqR_ = sumLR_ = sumRawSqL_ = sumRawSqR_ = 0.0;
	blockFill_ = 0;

	// **THE GATING BLOCK IS THE LAST FOUR, WHICH IS THE 400 ms WINDOW AT 75% OVERLAP THE STANDARD
	// ASKS FOR — the overlap is not an extra mechanism, it is what stepping a four-block window by
	// one block already is.** It only exists once four blocks have been seen; before that there is no
	// 400 ms of audio to measure and inventing one would put a quiet partial block into the gate.
	if (blocksSeen_ < kBlocksMomentary) {
		return;
	}
	double sum = 0.0;
	for (size_t i = 0; i < kBlocksMomentary; ++i) {
		const size_t at = (blockHead_ + kBlocksShort - 1 - i) % kBlocksShort;
		sum += kChannelWeight * (msL_[at] + msR_[at]);
	}
	const double blockLoudness = loudnessOf(sum / static_cast<double>(kBlocksMomentary));
	if (blockLoudness <= kAbsoluteGateLufs) {
		return;
	}
	gatedSumEnergy_ += energyOf(blockLoudness);
	++gatedCount_;
	const long bin = std::lround((blockLoudness - kAbsoluteGateLufs) * 10.0);
	histogram_[static_cast<size_t>(std::clamp<long>(bin, 0, static_cast<long>(kHistogramBins) - 1))]++;
	// THE GATE CHANGES ONLY HERE, once per 100 ms block, so the reading is made here and read from the cache.
	++integratedScans_;
	integrated_ = computeIntegrated();
}

void Meter::process(const float* left, const float* right, int frames) noexcept {
	for (int i = 0; i < frames; ++i) {
		const double l = static_cast<double>(left[i]);
		const double r = static_cast<double>(right[i]);

		// **THE OVER IS DETECTED ON THE SAMPLE, WITH NO BALLISTICS AT ALL.** Any averaging here would
		// be able to miss a single-sample over, which is precisely the over worth knowing about.
		// `>=` and not `>`: 0 dBFS IS the ceiling, not the first value past it.
		if (std::fabs(l) >= 1.0 || std::fabs(r) >= 1.0) {
			clipAge_ = 0.0f;
		}

		peakL_ = std::max(peakL_ * peakDecay_, static_cast<float>(std::fabs(l)));
		peakR_ = std::max(peakR_ * peakDecay_, static_cast<float>(std::fabs(r)));

		// **THE RAW PEAK: LARGEST OF EITHER CHANNEL, NO DECAY, LATCHED ON A FIXED WINDOW.** The held
		// value cannot fall mid-window, so a transient survives long enough for any frame rate to
		// draw it; when the window closes the accumulator becomes the new held value and starts
		// again from nothing. That is what makes it fall at all — nothing here multiplies it down.
		rawPeakAccum_ = std::max(rawPeakAccum_,
		                         static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
		if (++rawPeakSamples_ >= rawPeakWindow_) {
			rawPeakHeld_ = rawPeakAccum_;
			rawPeakAccum_ = 0.0f;
			rawPeakSamples_ = 0;
		}

		const double kl = kwLeft_.step(l);
		const double kr = kwRight_.step(r);
		sumSqL_ += kl * kl;
		sumSqR_ += kr * kr;

		// CORRELATION IS MEASURED ON THE RAW SIGNAL, not on the K-weighted one. It is a question about
		// the stereo image, and weighting it for perceived loudness would answer a question nobody
		// asked — the phase relationship of a bass note is not less real for being low.
		sumLR_ += l * r;
		sumRawSqL_ += l * l;
		sumRawSqR_ += r * r;

		if (++blockFill_ >= blockSamples_) {
			closeBlock();
		}
	}

	// THE AGE ADVANCES BY THE BLOCK, WHICH IS AS FINE AS ANY UI CAN READ IT. Advancing per sample
	// would cost a float add per sample for a number nothing can see move at that resolution.
	if (frames > 0) {
		clipAge_ = std::min(kClipAgeMax,
		                    clipAge_ + static_cast<float>(static_cast<double>(frames) / sampleRate_));
	}
}

float Meter::computeIntegrated() const noexcept {
	if (gatedCount_ == 0) {
		return kSilenceLufs;
	}
	// THE RELATIVE GATE IS COMPUTED FROM THE BLOCKS THAT PASSED THE ABSOLUTE ONE, then applied to
	// the same population — two passes over one histogram, which is why the histogram exists.
	const double ungated = loudnessOf(gatedSumEnergy_ / static_cast<double>(gatedCount_));
	const double threshold = ungated + kRelativeGateLu;
	double sum = 0.0;
	uint64_t count = 0;
	for (size_t bin = 0; bin < kHistogramBins; ++bin) {
		if (histogram_[bin] == 0u) {
			continue;
		}
		const double binLoudness = kAbsoluteGateLufs + static_cast<double>(bin) * 0.1;
		if (binLoudness <= threshold) {
			continue;
		}
		sum += energyOf(binLoudness) * static_cast<double>(histogram_[bin]);
		count += histogram_[bin];
	}
	if (count == 0) {
		return kSilenceLufs;
	}
	return static_cast<float>(loudnessOf(sum / static_cast<double>(count)));
}

float Meter::reading(Reading r) const noexcept {
	const auto windowed = [this](size_t blocks) -> double {
		const size_t have = std::min(blocksSeen_, kBlocksShort);
		if (have == 0) {
			return static_cast<double>(kSilenceLufs);
		}
		// **AS MANY BLOCKS AS EXIST, NOT AS MANY AS ASKED FOR.** A 3 s window over 0.4 s of audio
		// that treated the missing blocks as silence would read 8 dB low and settle upward over the
		// first three seconds — a meter that is wrong for exactly as long as anybody is watching it
		// start.
		const size_t take = std::min(blocks, have);
		double sum = 0.0;
		for (size_t i = 0; i < take; ++i) {
			const size_t at = (blockHead_ + kBlocksShort - 1 - i) % kBlocksShort;
			sum += kChannelWeight * (msL_[at] + msR_[at]);
		}
		return loudnessOf(sum / static_cast<double>(take));
	};

	switch (r) {
	case Reading::PeakL:
		return peakL_;
	case Reading::PeakR:
		return peakR_;
	case Reading::Momentary:
		return static_cast<float>(windowed(kBlocksMomentary));
	case Reading::ShortTerm:
		return static_cast<float>(windowed(kBlocksShort));
	case Reading::Integrated:
		return integrated_;
	case Reading::Correlation: {
		const size_t have = std::min(blocksSeen_, kBlocksShort);
		if (have == 0) {
			return 0.0f;
		}
		// **A SHORT WINDOW, THREE BLOCKS.** Correlation over three seconds is a number that barely
		// moves and so says nothing about the sound you are hearing; 300 ms tracks a phase problem
		// arriving without flickering on every note.
		const size_t take = std::min<size_t>(3, have);
		double num = 0.0;
		double denL = 0.0;
		double denR = 0.0;
		for (size_t i = 0; i < take; ++i) {
			const size_t at = (blockHead_ + kBlocksShort - 1 - i) % kBlocksShort;
			num += corrNum_[at];
			denL += corrDenL_[at];
			denR += corrDenR_[at];
		}
		const double den = std::sqrt(denL * denR);
		// SILENCE IS NOT CORRELATED AND IT IS NOT ANTI-CORRELATED — it has no phase relationship at
		// all, and 0 is the only honest answer. Dividing by a zero denominator would give a NaN that
		// draws as whatever the card's arithmetic does with one.
		if (!(den > 0.0)) {
			return 0.0f;
		}
		return static_cast<float>(std::clamp(num / den, -1.0, 1.0));
	}
	case Reading::ClipAge:
		return clipAge_;
	case Reading::RawPeak:
		// **THE LARGER OF HELD AND ACCUMULATING**, so a peak that has just arrived is visible in the
		// same block rather than waiting for the window to close. Without the max, a transient at the
		// start of a window would be invisible for up to 50 ms — which is exactly the miss this
		// reading exists to avoid, arriving from the other side.
		return std::max(rawPeakHeld_, rawPeakAccum_);
	case Reading::kCount:
		break;
	}
	return 0.0f;
}

} // namespace feathers::fx::busutil
