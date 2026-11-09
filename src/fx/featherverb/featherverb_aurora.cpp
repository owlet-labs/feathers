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

#include "fx/featherverb/featherverb_aurora.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::featherverb {

namespace {

constexpr double kTau = 6.283185307179586;
constexpr double kReferenceRate = 44100.0;
/// The int32 road's scale: the device's full scale (`kFirmwareFullScale`), so 1.0 here is 1.0 at the send.
constexpr double kScale = 16777216.0;
/// The lines' lengths at Size 0, in milliseconds: mutually prime at every rate that matters, spread so no two
/// laps coincide soon.
constexpr std::array<double, Aurora::kLines> kLineMs { 43.7, 51.1, 62.9, 71.3 };
/// Each line's own slow drift, in hertz — incommensurate, so the lengths never move together.
constexpr std::array<double, Aurora::kLines> kDriftHz { 0.131, 0.173, 0.229, 0.293 };
/// The input diffusers, in milliseconds.
constexpr std::array<double, Aurora::kDiffusers> kDiffuserMs { 4.7, 3.6, 12.7, 9.3 };
/// Olli Niemitalo's 90° allpass pair (squared coefficients of first-order sections on z^-2).
constexpr std::array<float, 4> kHilbertA { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
constexpr std::array<float, 4> kHilbertB { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
/// The longest a line can be asked to be: Size 1 at the longest base, plus the deepest drift.
constexpr double kMaxLineSeconds = 71.3e-3 * 2.65 * 1.05;

/// A one-pole coefficient given at 44.1 kHz, moved to this rate so its corner stays put.
float atRate(double c, double sampleRate) {
	c = std::clamp(c, 0.0, 1.0);
	return static_cast<float>(1.0 - std::pow(1.0 - c, kReferenceRate / sampleRate));
}

/// A TRAPEZOIDAL one-pole's gain for a corner in Hz at this rate: the core's `tptGain` (backlog LB).
float atHz(double hz, double sampleRate) {
	const double g = std::tan(3.14159265358979323846 * std::clamp(hz, 0.0, 0.49 * sampleRate) / sampleRate);
	return static_cast<float>(g / (1.0 + g));
}

/// A one-pole coefficient with its pole at a corner in Hz: the core's `onePoleAt`, for the output lowpass.
float poleAtHz(double hz, double sampleRate) {
	return static_cast<float>(-std::expm1(-2.0 * 3.14159265358979323846 * std::max(hz, 0.0) / sampleRate));
}

/// One step of it: the lowpass of `x`, the state moved on (`Core::tptStep`).
float tptStep(float x, float& state, float G) {
	const float v = (x - state) * G;
	const float lp = v + state;
	state = lp + v;
	return lp;
}

float flush(float v) { return std::fabs(v) < 1.0e-20f ? 0.0f : v; }

} // namespace

void Aurora::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	const auto maxLine = static_cast<size_t>(std::ceil(kMaxLineSeconds * sampleRate_)) + 4;
	for (size_t k = 0; k < lines_.size(); ++k) {
		lines_[k].buf.assign(maxLine, 0.0f);
		lines_[k].driftStep = kDriftHz[k] / sampleRate_;
	}
	for (size_t k = 0; k < diffusers_.size(); ++k) {
		diffusers_[k].length = std::max<size_t>(1, static_cast<size_t>(std::lround(kDiffuserMs[k] * 1.0e-3 * sampleRate_)));
		diffusers_[k].buf.assign(diffusers_[k].length, 0.0f);
	}
	predelayBuf_.assign(static_cast<size_t>(std::ceil(0.05 * sampleRate_)) + 2, 0.0f);
	hilbert_.a = kHilbertA;
	hilbert_.b = kHilbertB;
	reset();
	set(Params {});
}

void Aurora::reset() noexcept {
	for (Line& line : lines_) {
		std::fill(line.buf.begin(), line.buf.end(), 0.0f);
		line.pos = 0;
		line.lp = 0.0f;
		line.drift = 0.0;
	}
	for (Allpass& ap : diffusers_) {
		std::fill(ap.buf.begin(), ap.buf.end(), 0.0f);
		ap.pos = 0;
	}
	std::fill(predelayBuf_.begin(), predelayBuf_.end(), 0.0f);
	predelayPos_ = 0;
	hilbert_.xa = {};
	hilbert_.ya = {};
	hilbert_.xb = {};
	hilbert_.yb = {};
	hilbert_.realDelay = 0.0f;
	phasor_ = 0.0;
	wobble_ = 0.0;
	hpState_ = 0.0f;
	lpL_ = 0.0f;
	lpR_ = 0.0f;
	// THE LINES' DRIFTS START APART, so they are not in step from the first sample.
	for (size_t k = 0; k < lines_.size(); ++k) {
		lines_[k].drift = 0.25 * static_cast<double>(k);
	}
}

void Aurora::set(const Params& p) noexcept {
	const auto unit = [](float v) { return std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f); };
	const std::array<float, 11>& t = p.character;

	const double scale = 0.55 + 2.1 * static_cast<double>(unit(p.size));
	const float decay = unit(p.decay);
	rt60_ = static_cast<float>(1.2 * std::pow(10.0, static_cast<double>(decay * decay)));
	for (size_t k = 0; k < lines_.size(); ++k) {
		Line& line = lines_[k];
		line.length = static_cast<float>(kLineMs[k] * 1.0e-3 * scale * sampleRate_);
		// PER LAP, THE RT60: -60 dB over rt60 seconds, so every line decays at one rate whatever its length.
		line.gain = static_cast<float>(std::pow(10.0, -3.0 * static_cast<double>(line.length) / (rt60_ * sampleRate_)));
	}

	// THE SHIFT: bipolar, squared so the centre of the travel has the fine end. Never quite zero — a quarter hertz
	// is the least that still breaks the loop's modes up, and is heard as smoothness rather than motion.
	const float s = std::clamp(std::isfinite(p.shift) ? p.shift : 0.0f, -1.0f, 1.0f);
	const float mag = 0.25f + 11.75f * s * s;
	shiftHz_ = s < 0.0f ? -mag : mag;
	// THE SHIFT'S OWN LFO (a slow LFO on the shift, its depth and rate both set by Character): Character sets how far it swings — up to 3 Hz either side, enough to carry a quarter-
	// hertz climb through zero into a fall and back, the curtains turning round — and, from a triangle of its own, how
	// fast: one lap in 50 s at the slow end, one in about 3 s at the quick. Both zero at Character 0.
	wobbleHz_ = 3.0f * std::fabs(t[1]);
	wobbleRateHz_ = 0.02f + 0.28f * std::fabs(t[4]);

	diffusionGain_ = 0.25f + 0.45f * unit(p.diffusion + 0.2f * t[2]);
	dampCoeff_ = atRate(1.0 - 0.85 * static_cast<double>(unit(p.damping + 0.2f * t[3])), sampleRate_);
	driftDepth_ = 0.0015f + 0.02f * std::fabs(t[0]);
	widthMix_ = unit(p.width + 0.3f * t[6]);
	hpG_ = atHz(static_cast<double>(p.hpfHz), sampleRate_);
	lpCoeff_ = poleAtHz(static_cast<double>(p.lpfHz), sampleRate_);
	predelayLength_ = std::min(predelayBuf_.size() - 1,
	                           static_cast<size_t>(std::lround(0.05 * static_cast<double>(unit(p.predelay)) * sampleRate_)));
	// LEVEL: MEASURED through the device on sine plucks at Mix 1, Aurora ran at rms 0.046 against Feather's 0.137 and
	// Sky's 0.185 (the defaults); 3.5 seats it between them.
	outGain_ = 3.5f;
}

float Aurora::readLine(const Line& line, float delay) const noexcept {
	// WRAPPED BY A COMPARE, NOT A DIVISION: `at` is in [pos, pos + size), one subtraction brings it in.
	const size_t size = line.buf.size();
	float at = static_cast<float>(line.pos) + static_cast<float>(size) - std::max(1.0f, delay);
	if (at >= static_cast<float>(size)) {
		at -= static_cast<float>(size);
	}
	const auto i0 = static_cast<size_t>(at);
	const float frac = at - static_cast<float>(i0);
	const size_t i1 = i0 + 1 == size ? 0 : i0 + 1;
	const float a = line.buf[i0];
	const float b = line.buf[i1];
	return a + frac * (b - a);
}

void Aurora::run(const int32_t* input, int32_t* outL, int32_t* outR, size_t frames, const int32_t* inject) noexcept {
	if (lines_[0].buf.empty()) {
		return;
	}
	// THE SHIFTER'S OSCILLATOR AS A ROTATING PHASOR, re-anchored from the phase once per call so it cannot drift in
	// magnitude: no trigonometry per sample.
	const double wobble = static_cast<double>(wobbleHz_) * std::sin(kTau * wobble_);
	const double hz = static_cast<double>(shiftHz_) + wobble;
	const double step = hz / sampleRate_;
	double c = std::cos(kTau * phasor_);
	double sn = std::sin(kTau * phasor_);
	const double cr = std::cos(kTau * step);
	const double sr = std::sin(kTau * step);
	phasor_ += step * static_cast<double>(frames);
	phasor_ -= std::floor(phasor_);
	lastShiftHz_ = static_cast<float>(hz);
	wobble_ += static_cast<double>(wobbleRateHz_) * static_cast<double>(frames) / sampleRate_; // the LFO's own lap
	wobble_ -= std::floor(wobble_);

	// THE LINES' DRIFTS, ONCE PER CALL: they move at 0.13 to 0.29 Hz, so a whole host block reads one length.
	// MEASURED: a sine per line per sample made Aurora cost twice Feather.
	std::array<float, kLines> lineDelay {};
	for (size_t k = 0; k < lines_.size(); ++k) {
		Line& line = lines_[k];
		const float mod = 1.0f + driftDepth_ * static_cast<float>(std::sin(kTau * line.drift));
		lineDelay[k] = line.length * mod;
		line.drift += line.driftStep * static_cast<double>(frames);
		line.drift -= std::floor(line.drift);
	}

	const size_t pdSize = predelayBuf_.size();
	for (size_t i = 0; i < frames; ++i) {
		float x = static_cast<float>(static_cast<double>(input[i]) / kScale);
		// THE INPUT HIGHPASS, then the predelay — as the core's.
		const float hp = x - tptStep(x, hpState_, hpG_);
		predelayBuf_[predelayPos_] = hp;
		float u = predelayLength_ == 0 ? hp : predelayBuf_[(predelayPos_ + pdSize - predelayLength_) % pdSize];
		predelayPos_ = (predelayPos_ + 1) % pdSize;
		// THE EXCITER'S INJECTION, past both, as in the core.
		if (inject != nullptr) {
			u += static_cast<float>(static_cast<double>(inject[i]) / kScale);
		}
		// THE DIFFUSERS: four series allpasses smear the attack before the loop takes it.
		for (Allpass& ap : diffusers_) {
			const float v = ap.buf[ap.pos];
			const float w = flush(u + diffusionGain_ * v);
			ap.buf[ap.pos] = w;
			u = v - diffusionGain_ * w;
			ap.pos = ap.pos + 1 == ap.length ? 0 : ap.pos + 1;
		}

		// THE LINES, each read at its own drifting length, damped and scaled for the RT60.
		std::array<float, kLines> d {};
		std::array<float, kLines> f {};
		for (size_t k = 0; k < lines_.size(); ++k) {
			Line& line = lines_[k];
			d[k] = readLine(line, lineDelay[k]);
			line.lp = flush(line.lp + dampCoeff_ * (d[k] - line.lp));
			f[k] = line.lp * line.gain;
		}
		// THE HOUSEHOLDER MIX: lossless, every line into every other.
		const float half = 0.5f * (f[0] + f[1] + f[2] + f[3]);
		for (float& v : f) {
			v -= half;
		}

		// THE SHIFT, on the energy returning to line 0: Niemitalo's pair gives a real and an imaginary copy 90°
		// apart; the phasor turns them, which moves every component by `hz`.
		float re = f[0];
		float im = f[0];
		for (size_t st = 0; st < 4; ++st) {
			const float ya = hilbert_.a[st] * (re + hilbert_.ya[1][st]) - hilbert_.xa[1][st];
			hilbert_.xa[1][st] = hilbert_.xa[0][st];
			hilbert_.xa[0][st] = re;
			hilbert_.ya[1][st] = hilbert_.ya[0][st];
			hilbert_.ya[0][st] = ya;
			re = ya;
			const float yb = hilbert_.b[st] * (im + hilbert_.yb[1][st]) - hilbert_.xb[1][st];
			hilbert_.xb[1][st] = hilbert_.xb[0][st];
			hilbert_.xb[0][st] = im;
			hilbert_.yb[1][st] = hilbert_.yb[0][st];
			hilbert_.yb[0][st] = yb;
			im = yb;
		}
		const float real = hilbert_.realDelay;
		hilbert_.realDelay = re;
		// THE UPPER SIDEBAND: MEASURED, `real·cos − imag·sin` with this pair moved a 1 kHz tail DOWN 79 Hz in 2 s at a
		// rising setting — Niemitalo's branches put the late one first — so the sign is the other way.
		const float shifted = static_cast<float>(real * c + im * sn);
		const double c2 = c * cr - sn * sr;
		sn = c * sr + sn * cr;
		c = c2;
		f[0] = flush(shifted);

		// INTO THE LINES: the diffused input with alternating signs, and the mixed, damped, shifted return.
		for (size_t k = 0; k < lines_.size(); ++k) {
			Line& line = lines_[k];
			const float in = (k & 1u) != 0 ? -u : u;
			line.buf[line.pos] = flush(in + f[k]);
			line.pos = line.pos + 1 == line.buf.size() ? 0 : line.pos + 1;
		}

		// OUT: two pairs of lines, a width, and the output lowpass.
		const float l = 0.5f * (d[0] + d[2]);
		const float r = 0.5f * (d[1] + d[3]);
		const float mid = 0.5f * (l + r);
		const float side = 0.5f * (l - r) * widthMix_;
		lpL_ += lpCoeff_ * ((mid + side) - lpL_);
		lpR_ += lpCoeff_ * ((mid - side) - lpR_);
		const double yl = std::clamp(static_cast<double>(lpL_ * outGain_) * kScale, -2147483648.0, 2147483647.0);
		const double yr = std::clamp(static_cast<double>(lpR_ * outGain_) * kScale, -2147483648.0, 2147483647.0);
		outL[i] = static_cast<int32_t>(yl);
		outR[i] = static_cast<int32_t>(yr);
	}
}

} // namespace feathers::fx::featherverb
