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


#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

// A DRONE STRING'S EAR — A HARMONIC RESONATOR (the strings get excited by the harmonics — probably
// exciting the sympathetic strings' own harmonics"). A feedback comb tuned to the string's period, so it rings at f,
// 2f, 3f ... as a string does, and a partial of the input that lands on ANY of them excites it (Pa's second harmonic
// on Sa's third). Replaces the single band-pass at f, which heard only the fundamental.
//
//     y[n] = (1 - g) x[n] + g · L(y)[n - D]        L: a one-pole loop low-pass,  L(s)[n] = (1 - c) s[n] + c L[n - 1]
//
// D is fractional, read by linear interpolation, and shortened by the loop low-pass's phase delay at f so the loop is
// exactly one period long AT THE FUNDAMENTAL (upper resonances fall slightly flat, as a stiff string's do not — a
// damped string's do). The low-pass rolls the upper resonances off as a string's losses do. THE (1 - g) IN FRONT
// KEEPS THE BAND-PASS'S NORMALISATION: 0 dB at the fundamental's peak when c = 0, below it otherwise, and never
// above it at any frequency, because the impulse response is non-negative and sums to at most 1 — so |y| never
// exceeds the largest |x| that went in (the sympathy loop's bound leans on this).
//
// SHARPNESS FROM THE STRING'S φ "Q" (2..32, as the band-pass's was): g is chosen so the comb's half-power bandwidth
// at f matches a band-pass of that Q, in the comb's own high-g form Q = π √g / (1 - g) — √g = (√(π² + 4Q²) - π) / 2Q.
// Q 2 -> g 0.24, Q 8 -> 0.68, Q 32 -> 0.91 (earFeedbackFor).
//
// THE LINE IS NOT OWNED: prepare-time storage is handed in (ResonateDsp::prepare allocates every string's), sized for
// the lowest frequency the ear tunes to, kMinHz; a string below it is heard an octave (or more) up, which keeps its
// even harmonics. No allocation here.

namespace feathers::fx::resonate {

struct HarmonicEar {
	/// Below this the ear tunes to the string's first octave at or above it.
	static constexpr double kMinHz = 20.0;
	/// THE LOOP LOW-PASS'S CORNER, in harmonics of the string: its losses start to tell around the sixth. By ear
	/// (unauditioned).
	static constexpr double kLoopCornerHarmonic = 6.0;
	/// Samples the line needs at `sampleRate`: kMinHz's period and room for the interpolation.
	static int lineLength(double sampleRate) { return static_cast<int>(std::ceil(sampleRate / kMinHz)) + 4; }

	static float feedbackFor(float q) {
		const double qq = std::max(1e-3, static_cast<double>(q));
		const double pi = 3.14159265358979323846;
		const double s = (std::sqrt(pi * pi + 4.0 * qq * qq) - pi) / (2.0 * qq);
		return static_cast<float>(s * s);
	}

	void attach(float* line, int length) {
		line_ = line;
		length_ = length;
		clear();
	}
	/// Tunes to `hz` (folded up by octaves to kMinHz, kept under 0.45 of the rate) at the Q's sharpness.
	void tune(double hz, float q, double sampleRate) {
		double f = std::max(hz, 1e-3);
		while (f < kMinHz) {
			f *= 2.0;
		}
		f = std::min(f, 0.45 * sampleRate);
		const double pi = 3.14159265358979323846;
		g_ = feedbackFor(q);
		c_ = static_cast<float>(std::exp(-2.0 * pi * std::min(kLoopCornerHarmonic * f, 0.45 * sampleRate) / sampleRate));
		// THE LOOP'S LENGTH AT f: one period, less the one-pole's phase delay there.
		const double w = 2.0 * pi * f / sampleRate;
		const double lpDelay = std::atan2(static_cast<double>(c_) * std::sin(w), 1.0 - static_cast<double>(c_) * std::cos(w)) / w;
		delay_ = std::clamp(sampleRate / f - lpDelay, 1.0, static_cast<double>(length_ - 3));
		hz_ = f;
	}
	void clear() {
		if (line_ != nullptr) {
			std::memset(line_, 0, sizeof(float) * static_cast<size_t>(length_));
		}
		pos_ = 0;
		lp_ = 0.0f;
	}
	float process(float x) {
		// READ D back: position pos_ - D, linearly between its two neighbours.
		double at = static_cast<double>(pos_) - delay_;
		if (at < 0.0) {
			at += static_cast<double>(length_);
		}
		const int i0 = static_cast<int>(at);
		const float fr = static_cast<float>(at - static_cast<double>(i0));
		const int i1 = (i0 + 1 == length_) ? 0 : i0 + 1;
		// i0 is the OLDER sample, so the read is (1 - fr) of it plus fr of the next — delay D exactly.
		const float delayed = line_[i0] + (line_[i1] - line_[i0]) * fr;
		lp_ = (1.0f - c_) * delayed + c_ * lp_;
		float y = (1.0f - g_) * x + g_ * lp_;
		if (std::abs(y) < 1e-20f) {
			y = 0.0f; // no denormal tail: a rung-out ear reaches exact zero
		}
		if (std::abs(lp_) < 1e-20f) {
			lp_ = 0.0f;
		}
		line_[pos_] = y;
		pos_ = (pos_ + 1 == length_) ? 0 : pos_ + 1;
		return y;
	}

	float feedback() const { return g_; }
	float loopPole() const { return c_; }
	double delaySamples() const { return delay_; }
	double tunedHz() const { return hz_; }

private:
	float* line_ = nullptr;
	int length_ = 0;
	int pos_ = 0;
	double delay_ = 1.0;
	double hz_ = 0.0;
	float g_ = 0.0f;
	float c_ = 0.0f;
	float lp_ = 0.0f;
};

} // namespace feathers::fx::resonate
