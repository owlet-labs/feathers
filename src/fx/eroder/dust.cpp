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

#include "fx/eroder/dust.h"

#include "fx/ring_lerp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::eroder {

namespace {

/// Below this the feedback tap is flushed, so a float comb reaches exact zero instead of circling in the denormals.
constexpr float kFeedbackFloor = 1.0e-9f;

/// ONE CHANNEL OUT OF THE RING BY A FOUR-POINT HERMITE. A head that never rests would, read linearly, lowpass the
/// signal by an amount that follows its fractional position — a hiss of its own on top of the one asked for.
/// `absolutePos` must sit at least two frames behind the newest write.
void readHermite(const sklitter::Ring& ring, double absolutePos, float* left, float* right) noexcept {
	const size_t len = ring.length();
	if (len < 4) {
		*left = 0.0f;
		*right = 0.0f;
		return;
	}
	const double whole = std::floor(absolutePos);
	const auto t = static_cast<float>(absolutePos - whole);
	const size_t i1 = ringIndex(whole, len);
	const size_t i0 = (i1 + len - 1u) % len;
	const size_t i2 = (i1 + 1u) % len;
	const size_t i3 = (i1 + 2u) % len;
	float l[4];
	float r[4];
	ring.readWrapped(i0, &l[0], &r[0]);
	ring.readWrapped(i1, &l[1], &r[1]);
	ring.readWrapped(i2, &l[2], &r[2]);
	ring.readWrapped(i3, &l[3], &r[3]);
	const auto at = [t](const float* y) {
		const float c1 = 0.5f * (y[2] - y[0]);
		const float c2 = y[0] - 2.5f * y[1] + 2.0f * y[2] - 0.5f * y[3];
		const float c3 = 0.5f * (y[3] - y[0]) + 1.5f * (y[1] - y[2]);
		return ((c3 * t + c2) * t + c1) * t + y[1];
	};
	*left = at(l);
	*right = at(r);
}

/// ONE POLE TOWARD A TARGET, LANDING ON IT: a glide toward zero would otherwise spend for ever in the denormals.
float glide(float value, float target, float coefficient) noexcept {
	const float gap = target - value;
	return std::abs(gap) < 1.0e-9f ? target : value + gap * coefficient;
}

/// **HOW MUCH OF A WHITE INPUT'S POWER THE BAND PASSES — EXACTLY, FOR THE FILTER AS IT RUNS.** The band is two
/// identical trapezoidal bandpasses in series; written as one four-state linear system `z' = A z + B x`,
/// `y = C z + D x`, its state covariance under unit white noise solves `P = A P A^T + B B^T`, and the output's
/// variance is `C P C^T + D^2`. That is sixteen linear equations, solved here by elimination. A closed form for the
/// analogue prototype would be right at low frequencies and decibels out near the top of the range, where the
/// trapezoidal warping narrows the band.
double bandPower(double g, double k) noexcept {
	const double a1 = 1.0 / (1.0 + g * (g + k));
	const double a2 = g * a1;
	const double a3 = g * a2;
	// ONE SECTION: the two integrator states, the input, and the bandpass output scaled to unit peak gain.
	const double sa[2][2] = { { 2.0 * a1 - 1.0, -2.0 * a2 }, { 2.0 * a2, 1.0 - 2.0 * a3 } };
	const double sb[2] = { 2.0 * a2, 2.0 * a3 };
	const double sc[2] = { k * a1, -k * a2 };
	const double sd = k * a2;

	double a[4][4] = {};
	double b[4] = {};
	double c[4] = {};
	for (int i = 0; i < 2; ++i) {
		for (int j = 0; j < 2; ++j) {
			a[i][j] = sa[i][j];
			a[i + 2][j + 2] = sa[i][j];
			a[i + 2][j] = sb[i] * sc[j];
		}
		b[i] = sb[i];
		b[i + 2] = sb[i] * sd;
		c[i] = sd * sc[i];
		c[i + 2] = sc[i];
	}
	const double d = sd * sd;

	// (I - A (x) A) vec(P) = vec(B B^T), augmented with its right-hand side.
	double m[16][17] = {};
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			const int row = i * 4 + j;
			for (int p = 0; p < 4; ++p) {
				for (int q = 0; q < 4; ++q) {
					m[row][p * 4 + q] = (row == p * 4 + q ? 1.0 : 0.0) - a[i][p] * a[j][q];
				}
			}
			m[row][16] = b[i] * b[j];
		}
	}
	for (int col = 0; col < 16; ++col) {
		int pivot = col;
		for (int row = col + 1; row < 16; ++row) {
			if (std::abs(m[row][col]) > std::abs(m[pivot][col])) {
				pivot = row;
			}
		}
		if (std::abs(m[pivot][col]) < 1.0e-300) {
			return 1.0;
		}
		if (pivot != col) {
			for (int x = col; x < 17; ++x) {
				std::swap(m[pivot][x], m[col][x]);
			}
		}
		for (int row = col + 1; row < 16; ++row) {
			const double f = m[row][col] / m[col][col];
			for (int x = col; x < 17; ++x) {
				m[row][x] -= f * m[col][x];
			}
		}
	}
	double p[16] = {};
	for (int row = 15; row >= 0; --row) {
		double sum = m[row][16];
		for (int x = row + 1; x < 16; ++x) {
			sum -= m[row][x] * p[x];
		}
		p[row] = sum / m[row][row];
	}
	double power = d * d;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			power += c[i] * c[j] * p[i * 4 + j];
		}
	}
	return power > 0.0 && std::isfinite(power) ? power : 1.0;
}

} // namespace

DustDerived deriveDust(const Settings& s, double pitchSemitones, double sampleRate) noexcept {
	const double rate = sampleRate > 0.0 ? sampleRate : kReferenceRate;
	DustDerived d;

	// THE BOTTOM OF A LogSeconds RANGE IS "NO TIME AT ALL".
	d.amountSeconds = static_cast<double>(s.amount) <= kDustAmountLog2Min
	                      ? 0.0
	                      : std::min(std::exp2(static_cast<double>(s.amount)), kDustAmountMaxSeconds);

	const double pitchRatio = s.track ? std::exp2((pitchSemitones - kReferenceNote) / 12.0) : 1.0;
	d.freqHz = std::clamp(std::exp2(static_cast<double>(s.freq)) * pitchRatio, kDustFreqFloorHz,
	                      std::min(kDustFreqCeilingHz, kDustFreqNyquistFraction * rate));

	// WIDTH IS THE BAND'S Q, even in octaves of Q: narrow at 0, wide at 1.
	const double width = std::clamp(static_cast<double>(s.width), 0.0, 1.0);
	d.q = kDustQNarrow * std::pow(kDustQWide / kDustQNarrow, width);

	d.g = std::tan(std::numbers::pi * d.freqHz / rate);
	d.k = 1.0 / d.q;
	d.a1 = 1.0 / (1.0 + d.g * (d.g + d.k));
	d.a2 = d.g * d.a1;
	d.a3 = d.g * d.a2;

	// SO THE SHAKE IS AS DEEP AT ANY FREQ, WIDTH AND RATE.
	d.noiseGain = kDustNoiseRms / std::sqrt(kDustWhiteVariance * bandPower(d.g, d.k));

	d.sineStep = 2.0 * std::numbers::pi * d.freqHz / rate;

	d.stereo = std::clamp(s.stereo, 0.0f, 1.0f);
	// THE RIGHT NOISE IS (1 - stereo) OF THE LEFT'S PLUS stereo OF ITS OWN, brought back to the left's power.
	const double own = static_cast<double>(d.stereo);
	d.stereoNorm = static_cast<float>(1.0 / std::sqrt((1.0 - own) * (1.0 - own) + own * own));
	d.sinePhaseOffset = own * std::numbers::pi * 0.5;

	d.source = std::clamp(s.source, 0.0f, 1.0f);
	d.grit = std::clamp(s.grit, 0.0f, 1.0f);

	d.feedback = std::clamp(static_cast<double>(s.resonance), -1.0, 1.0) * kDustFeedbackCeiling;
	const double mix = std::clamp(static_cast<double>(s.mix), 0.0, 1.0);
	// A COMB'S BROADBAND POWER GAIN IS 1 / (1 - fb^2): taking it back out leaves the peaks louder and the whole no
	// quieter, which is what turning a resonance up should sound like.
	d.wetGain = mix * std::sqrt(1.0 - d.feedback * d.feedback);
	d.dryGain = 1.0 - mix;

	const double drive = std::clamp(static_cast<double>(s.drive), 0.0, 1.0);
	d.driveGain = std::exp2(drive * kDustDriveOctaves);
	d.driveNorm = 1.0 / std::sqrt(d.driveGain);
	return d;
}

void Dust::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE ONE ALLOCATION: twice the largest Amount, the margin, and a few frames for the interpolation's neighbours.
	line_.prepare(sampleRate_, 2.0 * kDustAmountMaxSeconds + (kDustMarginFrames + 8.0) / sampleRate_);
	smoothing_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDustSmoothingSeconds * sampleRate_)));
	reset();
}

void Dust::reset() noexcept {
	line_.reset();
	writeCounter_ = 0;
	untilTick_ = 0;
	placed_ = false;
	amountFrames_ = 0.0f;
	feedback_ = 0.0f;
	wetGain_ = 0.0f;
	dryGain_ = 1.0f;
	for (Band& band : bands_) {
		band = Band {};
	}
	sinePhase_ = 0.0;
	held_ = 0.0f;
	prevMid_ = 0.0f;
	feedbackL_ = feedbackR_ = 0.0f;
	rng_ = kRngSeed;
	lastDelayFrames_[0] = lastDelayFrames_[1] = 0.0;
	lastMod_[0] = lastMod_[1] = 0.0f;
	lastNoise_[0] = lastNoise_[1] = 0.0f;
	holds_ = 0;
}

float Dust::nextNoise() noexcept {
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	// THE TOP 24 BITS, signed — exact in a float, and in [-1, 1).
	return static_cast<float>(static_cast<int32_t>(rng_) >> 8) * (1.0f / 8388608.0f);
}

void Dust::process(const Settings& settings, float* left, float* right, int frames) noexcept {
	if (line_.length() < 8) {
		return;
	}
	int done = 0;
	while (done < frames) {
		if (untilTick_ <= 0) {
			tick(settings);
			untilTick_ = kControlFrames;
		}
		const int take = std::min(frames - done, untilTick_);
		renderChunk(left + done, right + done, take);
		untilTick_ -= take;
		done += take;
	}
}

void Dust::tick(const Settings& settings) noexcept {
	derived_ = deriveDust(settings, pitchSemitones_, sampleRate_);
	if (!placed_) {
		amountFrames_ = static_cast<float>(derived_.amountSeconds * sampleRate_);
		feedback_ = static_cast<float>(derived_.feedback);
		wetGain_ = static_cast<float>(derived_.wetGain);
		dryGain_ = static_cast<float>(derived_.dryGain);
		placed_ = true;
	}
}

void Dust::renderChunk(float* left, float* right, int frames) noexcept {
	const DustDerived& d = derived_;

	const auto amountTarget = static_cast<float>(d.amountSeconds * sampleRate_);
	const auto feedbackTarget = static_cast<float>(d.feedback);
	const auto wetTarget = static_cast<float>(d.wetGain);
	const auto dryTarget = static_cast<float>(d.dryGain);

	const auto a1 = static_cast<float>(d.a1);
	const auto a2 = static_cast<float>(d.a2);
	const auto a3 = static_cast<float>(d.a3);
	const auto k = static_cast<float>(d.k);
	const auto bandGain = static_cast<float>(d.noiseGain);
	// ONE BANDPASS, trapezoidal, of unit peak gain.
	const auto band = [a1, a2, a3, k](Band& state, float x) {
		const float v3 = x - state.ic2;
		const float v1 = a1 * state.ic1 + a2 * v3;
		const float v2 = state.ic2 + a2 * state.ic1 + a3 * v3;
		state.ic1 = 2.0f * v1 - state.ic1;
		state.ic2 = 2.0f * v2 - state.ic2;
		return v1 * k;
	};
	const float ownNoise = d.stereo * d.stereoNorm;
	const float sharedNoise = (1.0f - d.stereo) * d.stereoNorm;
	const float source = d.source;
	const float grit = d.grit;

	const bool driven = d.driveGain > 1.0;
	const auto driveGain = static_cast<float>(d.driveGain);
	const auto driveNorm = static_cast<float>(d.driveNorm);
	const auto margin = static_cast<float>(kDustMarginFrames);

	for (int i = 0; i < frames; ++i) {
		// A NON-FINITE INPUT SAMPLE IS TREATED AS 0: the feedback would keep one for ever.
		const float inL = std::isfinite(left[i]) ? left[i] : 0.0f;
		const float inR = std::isfinite(right[i]) ? right[i] : 0.0f;

		amountFrames_ = glide(amountFrames_, amountTarget, smoothing_);
		feedback_ = glide(feedback_, feedbackTarget, smoothing_);
		wetGain_ = glide(wetGain_, wetTarget, smoothing_);
		dryGain_ = glide(dryGain_, dryTarget, smoothing_);

		line_.write(std::clamp(inL + feedbackL_ * feedback_, -kDustCeiling, kDustCeiling),
		            std::clamp(inR + feedbackR_ * feedback_, -kDustCeiling, kDustCeiling));
		++writeCounter_;
		const auto newest = static_cast<double>(writeCounter_ - 1u);

		// THE HOLD RE-DRAWS WHEN THE INPUT'S MID CHANGES SIGN, zero counting as positive.
		const float mid = inL + inR;
		if ((mid < 0.0f) != (prevMid_ < 0.0f)) {
			held_ = nextNoise();
			++holds_;
		}
		prevMid_ = mid;

		// TWO DRAWS A FRAME, ALWAYS, so the stream does not depend on any control.
		const float whiteL = nextNoise();
		const float whiteOwn = nextNoise();
		const float whiteR = whiteL * sharedNoise + whiteOwn * ownNoise;

		// THE BAND: TWO BANDPASSES IN SERIES. One alone falls only 6 dB an octave either side, and the head's SPEED
		// is what is heard — a displacement falling at 6 dB an octave is a speed that does not fall at all, so every
		// Freq would hiss alike.
		const float noiseL = band(bands_[1], band(bands_[0], whiteL)) * bandGain;
		const float noiseR = band(bands_[3], band(bands_[2], whiteR)) * bandGain;
		lastNoise_[0] = noiseL;
		lastNoise_[1] = noiseR;

		const auto sineL = static_cast<float>(std::sin(sinePhase_));
		const auto sineR = static_cast<float>(std::sin(sinePhase_ + d.sinePhaseOffset));
		sinePhase_ += d.sineStep;
		if (sinePhase_ >= 2.0 * std::numbers::pi) {
			sinePhase_ -= 2.0 * std::numbers::pi;
		}

		// TWO PRODUCTS A BLEND, so each end of Source and of Grit is exactly the thing it names.
		float modL = sineL * (1.0f - source) + noiseL * source;
		float modR = sineR * (1.0f - source) + noiseR * source;
		modL = std::clamp(modL * (1.0f - grit) + held_ * grit, -1.0f, 1.0f);
		modR = std::clamp(modR * (1.0f - grit) + held_ * grit, -1.0f, 1.0f);
		lastMod_[0] = modL;
		lastMod_[1] = modR;

		// THE READ HEADS: from the margin to the margin plus twice the Amount, never earlier.
		const float delayL = margin + amountFrames_ * (1.0f + modL);
		const float delayR = margin + amountFrames_ * (1.0f + modR);
		lastDelayFrames_[0] = static_cast<double>(delayL);
		lastDelayFrames_[1] = static_cast<double>(delayR);

		float wetL = 0.0f;
		float wetR = 0.0f;
		float unused = 0.0f;
		readHermite(line_, newest - static_cast<double>(delayL), &wetL, &unused);
		readHermite(line_, newest - static_cast<double>(delayR), &unused, &wetR);

		// THE TAP GOES BACK IN ON THE NEXT FRAME'S WRITE, so the loop is one frame longer than the head's delay.
		feedbackL_ = std::abs(wetL) < kFeedbackFloor ? 0.0f : wetL;
		feedbackR_ = std::abs(wetR) < kFeedbackFloor ? 0.0f : wetR;

		if (driven) {
			wetL = std::tanh(wetL * driveGain) * driveNorm;
			wetR = std::tanh(wetR * driveGain) * driveNorm;
		}

		// RIGHT THEN LEFT: the mono road runs this over one buffer and the survivor must be the left.
		right[i] = inR * dryGain_ + wetR * wetGain_;
		left[i] = inL * dryGain_ + wetL * wetGain_;
	}
}

} // namespace feathers::fx::eroder
