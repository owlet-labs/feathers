/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#pragma once

#include "fx/shaper/shaper_curve.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace feathers::fx::eroder {

// THE SHARED SECTION'S PARTS: emphasis, smear, a loop limiter and a slew limiter. Each is stereo, a frame at a time
// (so any host partition gives the same bits), allocates nothing, and is not run at all at the bottom of its setting.
// A filter that has rung down to nothing is set to exact zero once its input is exactly zero, so a tail ends.

namespace shell_detail {

/// Below this, with a zero input, a filter's state is dropped to zero.
inline constexpr double kFloor = 1.0e-9;

/// ONE BIQUAD, transposed direct form II, in double: `y = b0 x + z1;  z1 = b1 x - a1 y + z2;  z2 = b2 x - a2 y`.
struct Biquad {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

	struct State {
		double z1 = 0.0;
		double z2 = 0.0;
	};

	[[nodiscard]] double step(double x, State& s) const noexcept {
		if (x == 0.0 && std::abs(s.z1) < kFloor && std::abs(s.z2) < kFloor) {
			s = State {};
			return 0.0;
		}
		const double y = b0 * x + s.z1;
		s.z1 = b1 * x - a1 * y + s.z2;
		s.z2 = b2 * x - a2 * y;
		return y;
	}

	/// The section from numerator `n` and denominator `d` (each `{c0, c1, c2}`), divided through by `d0`.
	[[nodiscard]] static Biquad from(const double (&n)[3], const double (&d)[3]) noexcept {
		Biquad q;
		q.b0 = n[0] / d[0];
		q.b1 = n[1] / d[0];
		q.b2 = n[2] / d[0];
		q.a1 = d[1] / d[0];
		q.a2 = d[2] / d[0];
		return q;
	}

	/// `H(e^{jw})`.
	[[nodiscard]] std::complex<double> response(double w) const noexcept {
		const std::complex<double> z1 = std::polar(1.0, -w);
		const std::complex<double> z2 = z1 * z1;
		return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
	}
};

} // namespace shell_detail

//--------------------------------------------------------------------------------------------//
// EMPHASIS                                                                                    //
//--------------------------------------------------------------------------------------------//

enum class EmphasisShape { Tilt, Bell };

struct EmphasisSettings {
	EmphasisShape shape = EmphasisShape::Tilt;
	/// Tilt: the highs rise by this and the lows fall by it. Bell: the gain at the centre. Clamped to +-18 dB.
	/// Exactly 0 is neutral: neither `in` nor `out` runs.
	float amountDb = 0.0f;
	/// The tilt's pivot, or the bell's centre.
	float hz = 1000.0f;
	/// The bell's width in octaves (between the half-gain points); unused by the tilt.
	float widthOctaves = 1.0f;
	/// Whether `out` applies the inverse; false leaves it a pass-through.
	bool restore = true;
};

/// **A FILTER BEFORE THE SHAPER AND ITS EXACT INVERSE AFTER IT.** The tilt is `shaper::tiltSection` (a first-order shelf
/// that lifts the highs by `amount` and cuts the lows by it, unity at the pivot); the bell is the peaking biquad
/// `H = (1 + a A, -2 cos w0, 1 - a A) / (1 + a / A, -2 cos w0, 1 - a / A)` with `A = 10^(dB/40)`, whose gain at the
/// centre is exactly `dB`. **The inverse of either is the same section with numerator and denominator exchanged** (the
/// bell's: the gain negated), which is stable because both are minimum phase, and which makes `out(in(x))` the
/// identity wherever nothing non-linear sits between them. `set` keeps the filters' state, so it may be called on every
/// control tick.
class EmphasisPair {
public:
	void prepare(double sampleRate) noexcept {
		rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		coefficientsFor_ = EmphasisSettings {};
		haveCoefficients_ = false;
		reset();
	}

	void reset() noexcept {
		for (int c = 0; c < 2; ++c) {
			tiltIn_[c] = tiltOut_[c] = 0.0f;
			bellIn_[c] = bellOut_[c] = shell_detail::Biquad::State {};
		}
	}

	void set(const EmphasisSettings& settings) noexcept {
		EmphasisSettings s = settings;
		s.amountDb = std::clamp(s.amountDb, -kMaxDb, kMaxDb);
		s.hz = std::clamp(s.hz, 5.0f, static_cast<float>(rate_ * 0.45));
		s.widthOctaves = std::clamp(s.widthOctaves, 0.1f, 6.0f);
		if (haveCoefficients_ && s.shape == coefficientsFor_.shape && s.amountDb == coefficientsFor_.amountDb
		    && s.hz == coefficientsFor_.hz && s.widthOctaves == coefficientsFor_.widthOctaves) {
			if (s.restore != coefficientsFor_.restore) {
				resetOut();
			}
			coefficientsFor_.restore = s.restore;
			return;
		}
		if (!haveCoefficients_ || s.shape != coefficientsFor_.shape || s.restore != coefficientsFor_.restore) {
			reset();
		}
		coefficientsFor_ = s;
		haveCoefficients_ = true;
		if (s.amountDb == 0.0f) {
			return;
		}
		if (s.shape == EmphasisShape::Tilt) {
			tiltFwd_ = shaper::tiltSection(s.amountDb, s.hz, rate_, false);
			tiltInv_ = shaper::tiltSection(s.amountDb, s.hz, rate_, true);
		}
		else {
			bellFwd_ = bell(s.amountDb, s.hz, s.widthOctaves, false);
			bellInv_ = bell(s.amountDb, s.hz, s.widthOctaves, true);
		}
	}

	/// The tuning the filters were last built for (after the clamp), for tests.
	[[nodiscard]] float hzForTest() const noexcept { return coefficientsFor_.hz; }

	[[nodiscard]] bool neutral() const noexcept { return !haveCoefficients_ || coefficientsFor_.amountDb == 0.0f; }

	/// THE FILTER BEFORE THE SHAPER.
	[[nodiscard]] float in(int channel, float x) noexcept {
		if (neutral()) {
			return x;
		}
		const int c = channel & 1;
		if (coefficientsFor_.shape == EmphasisShape::Tilt) {
			return tilt(tiltFwd_, tiltIn_[c], x);
		}
		return static_cast<float>(bellFwd_.step(x, bellIn_[c]));
	}

	/// ITS INVERSE AFTER IT (or `x` itself with `restore` off).
	[[nodiscard]] float out(int channel, float x) noexcept {
		if (neutral() || !coefficientsFor_.restore) {
			return x;
		}
		const int c = channel & 1;
		if (coefficientsFor_.shape == EmphasisShape::Tilt) {
			return tilt(tiltInv_, tiltOut_[c], x);
		}
		return static_cast<float>(bellInv_.step(x, bellOut_[c]));
	}

	/// The bell section for `db` at `hz`, `octaves` wide; `inverse` exchanges numerator and denominator.
	[[nodiscard]] static shell_detail::Biquad bellSection(double db, double hz, double octaves, double rate,
	                                                      bool inverse) noexcept {
		const double a = std::pow(10.0, db / 40.0);
		const double w0 = 2.0 * std::numbers::pi * hz / rate;
		const double alpha = std::sin(w0) * std::sinh(std::numbers::ln2 / 2.0 * octaves * w0 / std::sin(w0));
		const double c = -2.0 * std::cos(w0);
		const double num[3] = {1.0 + alpha * a, c, 1.0 - alpha * a};
		const double den[3] = {1.0 + alpha / a, c, 1.0 - alpha / a};
		return inverse ? shell_detail::Biquad::from(den, num) : shell_detail::Biquad::from(num, den);
	}

private:
	static constexpr float kMaxDb = 18.0f;

	[[nodiscard]] shell_detail::Biquad bell(double db, double hz, double octaves, bool inverse) const noexcept {
		return bellSection(db, hz, octaves, rate_, inverse);
	}

	[[nodiscard]] static float tilt(const shaper::TiltSection& section, float& z, float x) noexcept {
		if (x == 0.0f && std::abs(z) < static_cast<float>(shell_detail::kFloor)) {
			z = 0.0f;
			return 0.0f;
		}
		return section.step(x, z);
	}

	void resetOut() noexcept {
		for (int c = 0; c < 2; ++c) {
			tiltOut_[c] = 0.0f;
			bellOut_[c] = shell_detail::Biquad::State {};
		}
	}

	double rate_ = 48000.0;
	EmphasisSettings coefficientsFor_ {};
	bool haveCoefficients_ = false;
	shaper::TiltSection tiltFwd_ {};
	shaper::TiltSection tiltInv_ {};
	shell_detail::Biquad bellFwd_ {};
	shell_detail::Biquad bellInv_ {};
	float tiltIn_[2] = {};
	float tiltOut_[2] = {};
	shell_detail::Biquad::State bellIn_[2] = {};
	shell_detail::Biquad::State bellOut_[2] = {};
};

//--------------------------------------------------------------------------------------------//
// SMEAR                                                                                       //
//--------------------------------------------------------------------------------------------//

struct SmearSettings {
	/// 0 is not run. Rising, it raises the sections' Q from 0.5 (amount just above 0) to 8 (amount 1), geometrically:
	/// `Q = 0.5 * 16^amount`.
	float amount = 0.0f;
	/// The frequency the sections are centred on.
	float hz = 1000.0f;
};

/// **EIGHT SECOND-ORDER ALLPASS SECTIONS ON ONE FREQUENCY.** The magnitude stays flat; the phase turns about the centre,
/// the faster the higher the Q, so a click is spread into a chirp and a square's corners are thrown out of step. Each
/// section is the allpass biquad `(1 - a, -2 cos w0, 1 + a) / (1 + a, -2 cos w0, 1 - a)`, `a = sin(w0) / (2 Q)`.
class Smear {
public:
	static constexpr int kSections = 8;

	void prepare(double sampleRate) noexcept {
		rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		settings_ = SmearSettings {};
		delay_ = 0.0;
		reset();
	}

	void reset() noexcept {
		for (auto& channel : state_) {
			for (auto& s : channel) {
				s = shell_detail::Biquad::State {};
			}
		}
	}

	void set(const SmearSettings& settings) noexcept {
		SmearSettings s = settings;
		s.amount = std::clamp(s.amount, 0.0f, 1.0f);
		s.hz = std::clamp(s.hz, 5.0f, static_cast<float>(rate_ * 0.45));
		if (s.amount == 0.0f) {
			if (settings_.amount != 0.0f) {
				reset();
			}
			settings_ = s;
			delay_ = 0.0;
			return;
		}
		if (s.amount == settings_.amount && s.hz == settings_.hz) {
			return;
		}
		settings_ = s;
		const double q = 0.5 * std::pow(16.0, static_cast<double>(s.amount));
		const double w0 = 2.0 * std::numbers::pi * s.hz / rate_;
		const double alpha = std::sin(w0) / (2.0 * q);
		const double c = -2.0 * std::cos(w0);
		const double num[3] = {1.0 - alpha, c, 1.0 + alpha};
		const double den[3] = {1.0 + alpha, c, 1.0 - alpha};
		section_ = shell_detail::Biquad::from(num, den);
		// THE GROUP DELAY AT THE CENTRE, from the section's own response: -d(phase)/dw by a central difference.
		const double dw = w0 * 1.0e-4;
		const std::complex<double> ratio = section_.response(w0 + dw) / section_.response(w0 - dw);
		delay_ = -std::arg(ratio) / (2.0 * dw) * kSections;
	}

	[[nodiscard]] bool on() const noexcept { return settings_.amount != 0.0f; }

	/// THE CHAIN'S GROUP DELAY IN SAMPLES AT THE CENTRE, for the settings now held (0 when not run).
	[[nodiscard]] double delayAtCentre() const noexcept { return delay_; }

	[[nodiscard]] float process(int channel, float x) noexcept {
		if (settings_.amount == 0.0f) {
			return x;
		}
		auto& states = state_[channel & 1];
		double v = x;
		for (int i = 0; i < kSections; ++i) {
			v = section_.step(v, states[i]);
		}
		return static_cast<float>(v);
	}

private:
	double rate_ = 48000.0;
	SmearSettings settings_ {};
	double delay_ = 0.0;
	shell_detail::Biquad section_ {};
	shell_detail::Biquad::State state_[2][kSections] = {};
};

//--------------------------------------------------------------------------------------------//
// LOOP LIMITER                                                                                //
//--------------------------------------------------------------------------------------------//

/// **A STEREO-LINKED PEAK LIMITER FOR A FEEDBACK PATH.** The envelope is the larger of the two channels' magnitudes,
/// taken instantly on the way up and let go with a time constant (`env' = max(|x|, env * r)`, `r = exp(-1/(tau fs))`),
/// so the gain `min(1, threshold / env)` is already down on the frame that rises. Both channels get that one gain, and
/// below the threshold it is exactly 1.0f. After a burst the envelope falls to 1/e of its peak in one release time.
class LoopLimiter {
public:
	void prepare(double sampleRate) noexcept {
		rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		setRelease(0.050);
		reset();
	}
	void reset() noexcept {
		env_ = 0.0f;
		gain_ = 1.0f;
	}
	void setThreshold(float threshold) noexcept { threshold_ = std::max(threshold, 1.0e-6f); }
	void setRelease(double seconds) noexcept {
		release_ = static_cast<float>(std::exp(-1.0 / (std::max(seconds, 1.0e-4) * rate_)));
	}

	/// ONE FRAME, in place.
	void process(float& left, float& right) noexcept {
		const float peak = std::max(std::abs(left), std::abs(right));
		env_ = std::max(peak, env_ * release_);
		if (env_ < 1.0e-20f) {
			env_ = 0.0f;
		}
		if (env_ <= threshold_) {
			gain_ = 1.0f;
			left *= 1.0f;
			right *= 1.0f;
			return;
		}
		gain_ = threshold_ / env_;
		left = std::clamp(left * gain_, -threshold_, threshold_);
		right = std::clamp(right * gain_, -threshold_, threshold_);
	}

	/// The gain of the last frame.
	[[nodiscard]] float gain() const noexcept { return gain_; }
	[[nodiscard]] float envelope() const noexcept { return env_; }

private:
	double rate_ = 48000.0;
	float threshold_ = 1.0f;
	float release_ = 0.0f;
	float env_ = 0.0f;
	float gain_ = 1.0f;
};

//--------------------------------------------------------------------------------------------//
// SLEW                                                                                        //
//--------------------------------------------------------------------------------------------//

struct SlewSettings {
	/// False is not run: the output is the input to the bit.
	bool on = false;
	/// The rate, as a frequency: the hard limit is `L = 4 * 0.5 * hz / rate` per sample, so a triangle of 0.5 peak at
	/// `hz` is the fastest thing that passes unchanged at curve 0 (a sine of that peak and frequency is not: its
	/// steepest slope is pi/2 times L). The sound does not depend on the sample rate.
	float hz = 1000.0f;
	/// 0 is a hard slew limiter, 1 a one-pole lowpass with its cutoff at `hz`; continuous between.
	float curve = 0.0f;
	/// -1..1. Positive lets rising steps go `2^balance` times as far and falling steps `2^-balance` times; 0 is symmetric.
	float balance = 0.0f;
};

/// **A PARAMETRIC SLEW LIMITER.** Per frame `e = x - y`, and `y` moves toward `x` by `sign(e) * min(|e|, M)`, where the
/// most it may move is `M = s * ((1 - curve) * L + curve * k * |e|)`: `L` the hard limit, `k = 1 - exp(-2 pi hz / rate)`
/// the one-pole's coefficient, `s` the balance scale for that direction. Curve 0 is `min(|e|, s L)`, a constant maximum
/// step; curve 1 is `k |e|` (and `s k |e|` capped at `|e|`), a one-pole lowpass; between, the cap is a blend of the two
/// and the step is continuous in `curve`. (`L pow(|e|/L, curve)`, the first thought, makes curve 1 the identity, not a
/// lowpass.)
class Slew {
public:
	void prepare(double sampleRate) noexcept {
		rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		set(SlewSettings {});
		reset();
	}
	void reset() noexcept { y_[0] = y_[1] = 0.0f; }

	void set(const SlewSettings& settings) noexcept {
		if (settings.on && !on_) {
			reset();
		}
		on_ = settings.on;
		const double hz = std::clamp(static_cast<double>(settings.hz), 0.01, rate_ * 0.45);
		const float curve = std::clamp(settings.curve, 0.0f, 1.0f);
		const float balance = std::clamp(settings.balance, -1.0f, 1.0f);
		const float limit = static_cast<float>(4.0 * 0.5 * hz / rate_);
		const float k = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * hz / rate_));
		const float up = std::exp2(balance);
		const float down = std::exp2(-balance);
		hardUp_ = (1.0f - curve) * limit * up;
		hardDown_ = (1.0f - curve) * limit * down;
		linearUp_ = curve * k * up;
		linearDown_ = curve * k * down;
		limit_ = limit;
		k_ = k;
	}

	[[nodiscard]] bool on() const noexcept { return on_; }
	/// The hard limit `L` per sample for the settings held.
	[[nodiscard]] float limit() const noexcept { return limit_; }
	[[nodiscard]] float coefficient() const noexcept { return k_; }

	[[nodiscard]] float process(int channel, float x) noexcept {
		if (!on_) {
			return x;
		}
		float& y = y_[channel & 1];
		const float e = x - y;
		if (x == 0.0f && std::abs(y) < static_cast<float>(shell_detail::kFloor)) {
			y = 0.0f;
			return 0.0f;
		}
		const float magnitude = std::abs(e);
		if (e >= 0.0f) {
			y += std::min(magnitude, hardUp_ + linearUp_ * magnitude);
		}
		else {
			y -= std::min(magnitude, hardDown_ + linearDown_ * magnitude);
		}
		return y;
	}

private:
	double rate_ = 48000.0;
	bool on_ = false;
	float hardUp_ = 0.0f, hardDown_ = 0.0f, linearUp_ = 0.0f, linearDown_ = 0.0f;
	float limit_ = 0.0f;
	float k_ = 0.0f;
	float y_[2] = {};
};

} // namespace feathers::fx::eroder
