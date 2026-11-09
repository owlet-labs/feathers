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

#include "dsp/ks_string.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace feathers::ks {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr float kInvSqrt2 = 0.70710678118654752f;

uint64_t splitmix64(uint64_t x) {
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

} // namespace

double allpassPhaseDelay(double a, double omega) {
	if (omega < 1.0e-9) {
		return (1.0 - a) / (1.0 + a);
	}
	// H = (a + e^−jω) / (1 + a·e^−jω) = e^−jω · conj(D) / D with D = 1 + a·e^−jω, so arg H = −ω − 2·arg D.
	return 1.0 + 2.0 * std::atan2(-a * std::sin(omega), 1.0 + a * std::cos(omega)) / omega;
}

double thiranCoefficient(double d, double omega) {
	if (omega < 1.0e-9) {
		return (1.0 - d) / (1.0 + d);
	}
	return std::sin((1.0 - d) * omega * 0.5) / std::sin((1.0 + d) * omega * 0.5);
}

double loopFirMagnitude(double c, double omega) { return std::abs(1.0 - 2.0 * c + 2.0 * c * std::cos(omega)); }

double loopGainFor(double sampleRate, double f0, double t60Seconds, double c) {
	const double omega = kTwoPi * f0 / sampleRate;
	const double perTrip = (t60Seconds > 0.0 && std::isfinite(t60Seconds)) ? std::pow(10.0, -3.0 / (f0 * t60Seconds))
	                                                                       : kMaxLoopGain;
	const double g = std::min(perTrip, kMaxLoopGain) / std::max(loopFirMagnitude(c, omega), 1.0e-12);
	// |H_F| peaks at 1, at DC, for every c ≤ 0.25: capping g there caps the loop's gain at every frequency.
	return std::min(g, kMaxLoopGain);
}

double dispersionFloor(double period) {
	// A stage's delay is largest at DC for a ≤ 0, (1 − a) / (1 + a); the four together may take kDispersionBudget of
	// the period.
	const double r = kDispersionBudget * period / static_cast<double>(kDispersionStages);
	if (r <= 1.0) {
		return 0.0;
	}
	return (1.0 - r) / (1.0 + r);
}

double loopPhaseDelay(const Tuning& t, double omega) {
	return static_cast<double>(t.n) + allpassPhaseDelay(t.eta, omega) + 1.0
	       + static_cast<double>(kDispersionStages) * allpassPhaseDelay(t.disp, omega);
}

double partialHz(const Tuning& t, double sampleRate, int k) {
	const double target = kTwoPi * static_cast<double>(k);
	const auto phase = [&](double w) { return w * loopPhaseDelay(t, w); };
	double lo = 1.0e-9;
	double hi = std::numbers::pi;
	if (phase(hi) < target) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	for (int i = 0; i < 80; ++i) {
		const double mid = 0.5 * (lo + hi);
		(phase(mid) < target ? lo : hi) = mid;
	}
	return 0.5 * (lo + hi) * sampleRate / kTwoPi;
}

Tuning solveTuning(double sampleRate, double f0, double dispRequested) {
	Tuning t;
	const double period = sampleRate / f0;
	const double omega = kTwoPi * f0 / sampleRate;
	const double floorA = dispersionFloor(period);
	const double asked = std::clamp(dispRequested, -0.95, 0.0);
	t.disp = floorA >= 0.0 ? 0.0 : std::max(asked, floorA);
	const double rest = period - 1.0 - static_cast<double>(kDispersionStages) * allpassPhaseDelay(t.disp, omega);
	t.n = std::max(1, static_cast<int>(std::floor(rest - 0.5)));
	t.d = rest - static_cast<double>(t.n);
	t.eta = thiranCoefficient(t.d, omega);
	return t;
}

PluckNoise::PluckNoise(uint64_t seed) : state(splitmix64(seed) | 1ull) {}

float PluckNoise::next() {
	state ^= state >> 12;
	state ^= state << 25;
	state ^= state >> 27;
	const uint64_t r = state * 2685821657736338717ull;
	return static_cast<float>(static_cast<double>(r >> 40) * (2.0 / 16777216.0) - 1.0);
}

int buildExcitation(const Excitation& e, double period, float* out, float* scratch, int capacity) {
	const int length = std::clamp(static_cast<int>(std::lround(period)), 2, capacity);
	const double noise = std::clamp(static_cast<double>(e.noise), 0.0, 1.0);
	const bool tone = e.tone != nullptr && e.tonePoints > 0 && noise < 1.0;
	// THE TWO PARTS, each at unit RMS before they are mixed, so Noise is a share of the pluck and not of its level.
	double toneSq = 0.0;
	double noiseSq = 0.0;
	if (tone) {
		for (int i = 0; i < length; ++i) {
			const double pos = static_cast<double>(i) * e.tonePoints / static_cast<double>(length);
			const int i0 = static_cast<int>(pos);
			const double fr = pos - i0;
			const float a = e.tone[i0 % e.tonePoints];
			const float b = e.tone[(i0 + 1) % e.tonePoints];
			out[i] = static_cast<float>(a + (b - a) * fr);
			toneSq += static_cast<double>(out[i]) * out[i];
		}
	}
	if (noise > 0.0) {
		PluckNoise rng(e.seed);
		const double h = std::clamp(static_cast<double>(e.hardness), 0.0, 1.0);
		const double alpha = 0.05 + 0.95 * h * h;
		double y = 0.0;
		for (int i = 0; i < length; ++i) {
			y += alpha * (static_cast<double>(rng.next()) - y);
			scratch[i] = static_cast<float>(y);
			noiseSq += y * y;
		}
	}
	const double toneGain = tone && toneSq > 0.0 ? (1.0 - noise) / std::sqrt(toneSq / length) : 0.0;
	const double noiseGain = noise > 0.0 && noiseSq > 0.0 ? noise / std::sqrt(noiseSq / length) : 0.0;
	for (int i = 0; i < length; ++i) {
		const double t = tone ? out[i] * toneGain : 0.0;
		const double w = noise > 0.0 ? scratch[i] * noiseGain : 0.0;
		scratch[i] = static_cast<float>(t + w);
	}
	// THE PICK, circular because the burst is one period of what the loop will repeat: harmonic k is scaled by
	// |1 − e^(−j2πkβ)|, zero where kβ is an integer, and the DC goes with it.
	const double beta = std::clamp(e.pick, 0.02, 0.5);
	// i − lag lies between samples i − lagI − 1 (weight fr) and i − lagI (weight 1 − fr), each wrapped once.
	const double lag = beta * length;
	const int lagI = static_cast<int>(lag);
	const double fr = lag - lagI;
	double sq = 0.0;
	for (int i = 0; i < length; ++i) {
		int near = i - lagI;
		near += near < 0 ? length : 0;
		int far = near - 1;
		far += far < 0 ? length : 0;
		const double back = scratch[near] + (scratch[far] - scratch[near]) * fr;
		out[i] = static_cast<float>(scratch[i] - back);
		sq += static_cast<double>(out[i]) * out[i];
	}
	const double rms = std::sqrt(sq / length);
	const double gain = rms > 0.0 ? static_cast<double>(kExcitationRms) * e.level / rms : 0.0;
	for (int i = 0; i < length; ++i) {
		out[i] = static_cast<float>(out[i] * gain);
	}
	return length;
}

//------------//
//  KsString  //
//------------//

void KsString::prepare(double sampleRate) {
	sr_ = sampleRate;
	const auto need = static_cast<size_t>(std::ceil(sampleRate / kLowestHz)) + kPrimeSamples + kHandoffSamples + 16;
	size_t size = 1;
	while (size < need) {
		size <<= 1;
	}
	line_.assign(size, 0.0f);
	exc_.assign(size, 0.0f);
	mask_ = static_cast<uint32_t>(size - 1);
	reset();
	setFrequency(f0_);
}

void KsString::clearStates() {
	std::fill(line_.begin(), line_.end(), 0.0f);
	a_.x1 = a_.y1 = b_.x1 = b_.y1 = 0.0f;
	h1_ = h2_ = 0.0f;
	dx1_ = {};
	dy1_ = {};
	energy_ = 0.0;
	handoffLeft_ = 0;
}

void KsString::reset() {
	clearStates();
	excRead_ = excLength_ = 0;
	idle_ = true;
	g_ = gTarget_;
	gRampLeft_ = 0;
}

void KsString::setFrequency(double hz) {
	const double top = sr_ / kShortestPeriod;
	const double f = std::isfinite(hz) ? hz : kLowestHz;
	clamped_ = f < kLowestHz || f > top;
	f0_ = std::clamp(f, kLowestHz, top);
	period_ = sr_ / f0_;
	retune();
}

void KsString::setDispersion(double a) {
	dispAsked_ = std::clamp(std::isfinite(a) ? a : 0.0, -0.95, 0.0);
	retune();
}

void KsString::setDamping(double c) {
	c_ = std::clamp(std::isfinite(c) ? c : 0.0, 0.0, 0.25);
	retune();
}

void KsString::setDecay(double t60Seconds, int rampSamples) {
	t60_ = t60Seconds;
	gTarget_ = loopGainFor(sr_, f0_, t60_, c_);
	if (rampSamples > 0 && !idle_) {
		gRampLeft_ = rampSamples;
		gStep_ = (gTarget_ - g_) / rampSamples;
	} else {
		g_ = gTarget_;
		gRampLeft_ = 0;
	}
}

void KsString::setPickups(double left, double right) {
	pickL_ = std::clamp(left, 0.02, 0.5);
	pickR_ = std::clamp(right, 0.02, 0.5);
}

void KsString::retune() {
	const Tuning target = solveTuning(sr_, f0_, dispAsked_);
	dispF_ = static_cast<float>(target.disp);
	const double omega = kTwoPi * f0_ / sr_;
	const double rest = static_cast<double>(target.n) + target.d;
	const auto setPath = [&](ReadPath& p, int n) {
		p.tuning = target;
		p.tuning.n = n;
		p.tuning.d = rest - n;
		p.tuning.eta = thiranCoefficient(p.tuning.d, omega);
		p.eta = static_cast<float>(p.tuning.eta);
	};
	if (idle_) {
		// Nothing rings, so there is nothing to hand off: the path is simply the ideal one.
		setPath(a_, target.n);
		handoffLeft_ = 0;
	} else if (handoffLeft_ > 0) {
		// Mid-handoff the incoming path follows the pitch; the outgoing one follows it only while its own fraction can
		// (a first-order Thiran is stable for any d > 0, accurate in the band), and otherwise keeps the pitch it had
		// — a jump is a crossfade from the old note's path to the new one's.
		const double d = rest - a_.tuning.n;
		if (d >= 0.1 && d < 3.0) {
			setPath(a_, a_.tuning.n);
		}
		setPath(b_, target.n);
	} else {
		const double d = rest - a_.tuning.n;
		if (d >= kFractionLow && d < kFractionHigh) {
			setPath(a_, a_.tuning.n);
		} else {
			setPath(b_, target.n);
			// PRIME the new path: its allpass run over the input it would have seen, so its state is the steady
			// state and the crossfade joins two paths that already agree.
			b_.x1 = b_.y1 = 0.0f;
			for (int j = kPrimeSamples; j >= 1; --j) {
				const float x = line_[(w_ - static_cast<uint32_t>(b_.tuning.n + j)) & mask_];
				const float y = b_.eta * x + b_.x1 - b_.eta * b_.y1;
				b_.x1 = x;
				b_.y1 = y;
			}
			handoffLeft_ = kHandoffSamples;
		}
	}
	const double gNew = loopGainFor(sr_, f0_, t60_, c_);
	if (gRampLeft_ > 0) {
		gStep_ = (gNew - g_) / gRampLeft_;
	} else {
		g_ = gNew;
	}
	gTarget_ = gNew;
}

float KsString::readLine(double delay) const {
	const int i = static_cast<int>(delay);
	const float fr = static_cast<float>(delay - i);
	const float s0 = line_[(w_ - static_cast<uint32_t>(i)) & mask_];
	const float s1 = line_[(w_ - static_cast<uint32_t>(i + 1)) & mask_];
	return s0 + (s1 - s0) * fr;
}

void KsString::pluck(const float* e, int length) {
	if (line_.empty() || length <= 0) {
		return;
	}
	length = std::min(length, static_cast<int>(exc_.size()));
	const int rest = excLength_ - excRead_;
	for (int i = 0; i < rest; ++i) {
		exc_[static_cast<size_t>(i)] = exc_[static_cast<size_t>(excRead_ + i)];
	}
	const int total = std::max(rest, length);
	std::fill(exc_.begin() + rest, exc_.begin() + total, 0.0f);
	for (int i = 0; i < length; ++i) {
		exc_[static_cast<size_t>(i)] += e[i];
	}
	excRead_ = 0;
	excLength_ = total;
	idle_ = false;
}

void KsString::render(float* outL, float* outR, int n) {
	if (idle_ || line_.empty()) {
		std::fill(outL, outL + n, 0.0f);
		if (outR != nullptr) {
			std::fill(outR, outR + n, 0.0f);
		}
		return;
	}
	const float c = static_cast<float>(c_);
	const float mid = 1.0f - 2.0f * c;
	const double energyPole = 1.0 / std::max(period_, 1.0);
	const double floorSq = static_cast<double>(kSilenceFloor) * kSilenceFloor;
	const double delayL = pickL_ * period_;
	const double delayR = pickR_ * period_;
	const bool twoEars = outR != nullptr && delayL != delayR;
	for (int i = 0; i < n; ++i) {
		const float xa = line_[(w_ - static_cast<uint32_t>(a_.tuning.n)) & mask_];
		const float ya = a_.eta * xa + a_.x1 - a_.eta * a_.y1;
		a_.x1 = xa;
		a_.y1 = ya;
		float y = ya;
		if (handoffLeft_ > 0) {
			const float xb = line_[(w_ - static_cast<uint32_t>(b_.tuning.n)) & mask_];
			const float yb = b_.eta * xb + b_.x1 - b_.eta * b_.y1;
			b_.x1 = xb;
			b_.y1 = yb;
			const float t = static_cast<float>(kHandoffSamples - handoffLeft_ + 1) / static_cast<float>(kHandoffSamples);
			y = ya + (yb - ya) * t;
			if (--handoffLeft_ == 0) {
				a_ = b_;
			}
		}
		const float f = c * (y + h2_) + mid * h1_;
		h2_ = h1_;
		h1_ = y;
		float s = f;
		for (int st = 0; st < kDispersionStages; ++st) {
			const float o = dispF_ * s + dx1_[static_cast<size_t>(st)] - dispF_ * dy1_[static_cast<size_t>(st)];
			dx1_[static_cast<size_t>(st)] = s;
			dy1_[static_cast<size_t>(st)] = o;
			s = o;
		}
		if (gRampLeft_ > 0) {
			g_ += gStep_;
			if (--gRampLeft_ == 0) {
				g_ = gTarget_;
			}
		}
		const float e = excRead_ < excLength_ ? exc_[static_cast<size_t>(excRead_++)] : 0.0f;
		const float v = static_cast<float>(g_) * s + e;
		if (!std::isfinite(v)) {
			reset();
			std::fill(outL + i, outL + n, 0.0f);
			if (outR != nullptr) {
				std::fill(outR + i, outR + n, 0.0f);
			}
			return;
		}
		line_[w_] = v;
		const float l = (v - readLine(delayL)) * kInvSqrt2;
		outL[i] = l;
		if (outR != nullptr) {
			outR[i] = twoEars ? (v - readLine(delayR)) * kInvSqrt2 : l;
		}
		w_ = (w_ + 1) & mask_;
		energy_ += (static_cast<double>(v) * v - energy_) * energyPole;
		if (excRead_ >= excLength_ && energy_ < floorSq) {
			// EXACT SILENCE: below −160 dB with nothing left to inject, the string is zeroed rather than left to decay
			// through the denormals, so the output the SLEEP rule reads is exactly 0 from the next sample on.
			reset();
			std::fill(outL + i + 1, outL + n, 0.0f);
			if (outR != nullptr) {
				std::fill(outR + i + 1, outR + n, 0.0f);
			}
			return;
		}
	}
}

} // namespace feathers::ks

namespace feathers::ks {

//----------------//
//  PluckBuilder  //
//----------------//

void PluckBuilder::prepare(double sampleRate) {
	const auto length = static_cast<size_t>(std::ceil(sampleRate / kLowestHz)) + 4;
	out_.assign(length, 0.0f);
	scratch_.assign(length, 0.0f);
	re_.assign(kToneCyclePoints, 0.0f);
	im_.assign(kToneCyclePoints, 0.0f);
	cycle_.assign(kToneCyclePoints, 0.0f);
	cosT_.assign(kToneCyclePoints / 2, 0.0f);
	sinT_.assign(kToneCyclePoints / 2, 0.0f);
	for (int i = 0; i < kToneCyclePoints / 2; ++i) {
		const double a = 2.0 * std::numbers::pi * i / kToneCyclePoints;
		cosT_[static_cast<size_t>(i)] = static_cast<float>(std::cos(a));
		sinT_[static_cast<size_t>(i)] = static_cast<float>(std::sin(a));
	}
}

const float* PluckBuilder::toneCycle(const float* amplitude, int count, double period) {
	toneHarmonics_ = 0;
	if (re_.empty()) {
		return nullptr;
	}
	const int limit = std::min({ count, kToneCyclePoints / 2 - 1, static_cast<int>(std::floor(0.475 * period)) });
	std::fill(re_.begin(), re_.end(), 0.0f);
	std::fill(im_.begin(), im_.end(), 0.0f);
	bool any = false;
	for (int k = 1; k <= limit; ++k) {
		const float a = amplitude[k - 1];
		if (!(a > 0.0f)) {
			continue;
		}
		// x[j] = Re Σ Z_k e^{+j2πkj/M} with Z_k = A_k·e^{j(φ_k − π/2)} is Σ A_k sin(2πkj/M + φ_k).
		const double phi = std::numbers::pi * static_cast<double>(k) * k / kToneReferencePartials - 0.5 * std::numbers::pi;
		re_[static_cast<size_t>(k)] = static_cast<float>(a * std::cos(phi));
		im_[static_cast<size_t>(k)] = static_cast<float>(a * std::sin(phi));
		any = true;
	}
	if (!any) {
		return nullptr;
	}
	toneHarmonics_ = limit;
	// AN IN-PLACE RADIX-2 INVERSE FFT (decimation in time: bit-reversed order, then butterflies on e^{+j2π/len}).
	const int n = kToneCyclePoints;
	for (int i = 1, j = 0; i < n; ++i) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(re_[static_cast<size_t>(i)], re_[static_cast<size_t>(j)]);
			std::swap(im_[static_cast<size_t>(i)], im_[static_cast<size_t>(j)]);
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		const int half = len >> 1;
		const int step = n / len;
		for (int i = 0; i < n; i += len) {
			for (int j = 0; j < half; ++j) {
				const float wr = cosT_[static_cast<size_t>(j * step)];
				const float wi = sinT_[static_cast<size_t>(j * step)];
				const auto u = static_cast<size_t>(i + j);
				const auto v = static_cast<size_t>(i + j + half);
				const float xr = re_[v] * wr - im_[v] * wi;
				const float xi = re_[v] * wi + im_[v] * wr;
				re_[v] = re_[u] - xr;
				im_[v] = im_[u] - xi;
				re_[u] += xr;
				im_[u] += xi;
			}
		}
	}
	std::copy(re_.begin(), re_.end(), cycle_.begin());
	return cycle_.data();
}

void PluckBuilder::pluck(KsString& s, const Excitation& e) {
	if (out_.empty()) {
		return;
	}
	const int n = buildExcitation(e, s.sampleRate() / s.frequencyHz(), out_.data(), scratch_.data(),
	                              static_cast<int>(out_.size()));
	s.pluck(out_.data(), n);
}

} // namespace feathers::ks

namespace feathers::ks {

StringLaws lawsFor(float brightness, float sustain, float pick, float noise, float stretch, const StringControls& c,
                   double f0, bool released) {
	const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(static_cast<double>(v), 0.0, 1.0) : 0.0; };
	StringLaws l;
	const double dark = 1.0 - unit(brightness);
	l.damping = 0.25 * dark * dark;
	l.dispersion = -0.9 * unit(stretch);
	l.pick = 0.04 + 0.36 * unit(pick);
	l.noise = static_cast<float>(unit(noise));
	double held = 0.0;
	if (c.decaySeconds < kDecayRing) {
		held = std::max(1.0e-3, c.decaySeconds) * std::pow(std::max(f0, 1.0) / kDecayReferenceHz, -c.decayTrack)
		       * std::pow(4.0, 2.0 * unit(sustain) - 1.0);
	}
	l.t60 = held;
	if (released && c.releaseSeconds < kReleaseRing) {
		const double r = std::max(1.0e-3, c.releaseSeconds);
		l.t60 = held > 0.0 ? std::min(held, r) : r;
	}
	return l;
}

} // namespace feathers::ks
