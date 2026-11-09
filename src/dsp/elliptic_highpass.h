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
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

// THE KEYTRACKED BRICKWALL HIGH-PASS — backlog NL: removes exactly the fundamental and everything below
// it, ideally preserving the second harmonic to within 1 dB.
//
// ONE IMPLEMENTATION, TWO HOMES: HARM's High-pass mode and the Filter device's "Brickwall HP" type both run
// this, so the two cannot drift into two different brickwalls.
//
// THE SPEC IS A ONE-OCTAVE TRANSITION: the frequency to remove (f0, the fundamental) must be deep in the
// stopband and 2·f0 (the second harmonic) must be in the passband. An ELLIPTIC high-pass, because it is
// the steepest transition per pole there is, so the octave can be met with margin either side.
//
// THE DEFAULT, CHOSEN AND STATED (the brief's): ORDER 8, 0.1 dB PASSBAND RIPPLE, 60 dB STOPBAND. Order 8
// at those two numbers needs a transition ratio of only 1.18 (`selectivity`), so the octave holds it with
// room to spare, and the room is split EVENLY in log frequency: the stopband edge sits `margin` ABOVE f0
// and the passband edge `margin` BELOW 2·f0, where margin = sqrt(2·selectivity) = 1.30, about four and a
// half semitones. So a note bent or detuned by less than that still has its fundamental at least 60 dB
// down and its octave within 0.1 dB. (Order 6 would meet the octave too, with almost no margin left.)
//
// THE STRUCTURE: four second-order sections, each an Andy Simper trapezoidal SVF with a notch-style output
// mix, out = v0 - k·v1 + m2·v2, which puts a zero pair on the jw axis below the poles — exactly an elliptic
// high-pass section. In double, which an SVF does not need for stability but which keeps an 8 Hz corner
// at 192 kHz (g ~ 1.6e-4) exact to far below the stopband.
//
// WHY AN SVF AND NOT A BIQUAD: THE CORNER MOVES WITH THE NOTE, and fast. A direct-form biquad whose
// coefficients jump can blow up — its state is the past OUTPUT in a basis the new coefficients do not
// share. The trapezoidal SVF's state is two integrator charges whose meaning does not depend on g, so a
// coefficient change between two samples is a change of integrator rate rather than a reinterpretation of
// memory. MEASURED rather than trusted: tests/test_elliptic_highpass.cpp jumps the corner across six octaves
// every 16 samples under full-scale noise.
//
// THE BILINEAR WARP ONLY HELPS. The whole analog design is prewarped at the PASSBAND edge, so that edge
// lands exactly; every frequency below it maps to an analog frequency proportionally LOWER (tan is convex),
// so the stopband edge lands deeper in the stopband than designed, never shallower.
//
// DESIGN MATHEMATICS: S. J. Orfanidis, "Lecture Notes on Elliptic Filter Design" (Rutgers, 2006) — the
// degree equation, Landen's transformation and the pole/zero placement, transcribed here from the published
// equations, in double, at static initialisation (no allocation; fixed arrays).

namespace feathers::dsp {

struct EllipticHighpassDesign {
	static constexpr int kOrder = 8;
	static constexpr int kSections = kOrder / 2;
	static constexpr double kPassbandRippleDb = 0.1;
	static constexpr double kStopbandDb = 60.0;
	/// THE HIGHEST CORNER THE BILINEAR PREWARP IS ASKED FOR, as a fraction of the sample rate. tan(pi·x)
	/// runs to infinity at 0.5; above this the passband edge stops rising and the filter keeps removing
	/// everything below it, which is the right answer for an f0 whose octave is past Nyquist anyway.
	static constexpr double kMaxPassbandFraction = 0.49;

	/// One section of the HIGH-PASS normalised to a passband edge of 1 rad/s:
	/// H(s) = (s² + zero²) / (s² + damping·pole·s + pole²).
	struct Section {
		double pole = 1.0;
		double damping = 1.0; ///< 1/Q
		double zero = 0.0;
		/// The SVF's low-pass weight in the output mix: (zero/pole)² - 1.
		double lowMix = -1.0;
	};

	/// Stopband edge over passband edge (< 1). 0.847 at the defaults.
	double selectivity = 0.0;
	/// How far the stopband edge sits above f0, and the passband edge below 2·f0: sqrt(2·selectivity).
	double margin = 1.0;
	/// The overall gain, 10^(-Rp/20): an even-order elliptic sits at the bottom of its ripple at infinity.
	double gain = 1.0;
	/// Ordered by ascending Q, so the gentlest section sees the input first.
	std::array<Section, kSections> sections {};

	EllipticHighpassDesign() { design(); }

	/// The passband edge for a tracked f0, in hertz.
	[[nodiscard]] double passbandEdgeHz(double f0Hz) const { return f0Hz * 2.0 / margin; }
	/// The stopband edge for a tracked f0, in hertz: at and below it the response is at least 60 dB down.
	[[nodiscard]] double stopbandEdgeHz(double f0Hz) const { return f0Hz * margin; }

	/// THE ANALOG PROTOTYPE'S MAGNITUDE at `ratio` = f / f0 — note-independent, which is what lets HARM draw
	/// it on its ratio axis. Linear, 0..1.
	[[nodiscard]] double magnitudeAtRatio(double ratio) const {
		if (!(ratio > 0.0)) {
			return 0.0;
		}
		const double w = ratio * margin * 0.5; // in units of the passband edge
		double h = gain;
		for (const Section& s : sections) {
			const std::complex<double> num(s.zero * s.zero - w * w, 0.0);
			const std::complex<double> den(s.pole * s.pole - w * w, s.damping * s.pole * w);
			h *= std::abs(num / den);
		}
		return h;
	}

	/// THE DIGITAL RESPONSE THE SAMPLE LOOP RUNS, at `freqHz`, for a filter tracking `f0Hz` — section for
	/// section the SVF's own transfer function, H = m0 + m1·g(z²-1)/den + m2·g²(z+1)²/den, the identity
	/// fx/filter's stageResponse draws every type with. Linear magnitude.
	[[nodiscard]] double digitalMagnitude(double f0Hz, double sampleRate, double freqHz) const {
		const double t = prewarp(f0Hz, sampleRate);
		const std::complex<double> z =
		    std::exp(std::complex<double>(0.0, 2.0 * std::numbers::pi * freqHz / sampleRate));
		const auto zp1 = 1.0 + z;
		const auto zm1 = z - 1.0;
		const auto zz1 = z * z - 1.0;
		std::complex<double> h { gain, 0.0 };
		for (const Section& s : sections) {
			const double g = s.pole * t;
			const double k = s.damping;
			const auto den = zm1 * zm1 + g * g * zp1 * zp1 + g * k * zz1;
			const auto num = den - k * g * zz1 + s.lowMix * g * g * zp1 * zp1;
			h *= num / den;
		}
		return std::abs(h);
	}

	/// tan(pi · passband edge / sample rate), clamped below Nyquist — the one number a corner change moves.
	[[nodiscard]] double prewarp(double f0Hz, double sampleRate) const {
		const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
		const double fraction = std::clamp(passbandEdgeHz(std::max(f0Hz, 0.0)) / sr, 1.0e-7, kMaxPassbandFraction);
		return std::tan(std::numbers::pi * fraction);
	}

private:
	static constexpr int kLandenSteps = 12;

	/// Landen's descending moduli of (k, k'), passing k' explicitly: at k' near 1 (k1' below is
	/// 1 - 1.2e-8) recomputing it from k loses every digit that matters.
	static int landen(double k, double kp, std::array<double, kLandenSteps>& v) {
		int n = 0;
		while (k > 1.0e-15 && n < kLandenSteps) {
			const double next = k / (1.0 + kp);
			k = next * next;
			kp = std::sqrt((1.0 - k) * (1.0 + k));
			v[static_cast<size_t>(n++)] = k;
		}
		return n;
	}

	/// sn(u·K, k) and cd(u·K, k) by ascending Landen (Orfanidis' sne/cde).
	template <typename T>
	static T ascend(T w, double k, double kp) {
		std::array<double, kLandenSteps> v {};
		const int n = landen(k, kp, v);
		for (int i = n - 1; i >= 0; --i) {
			const double vi = v[static_cast<size_t>(i)];
			w = (1.0 + vi) * w / (1.0 + vi * w * w);
		}
		return w;
	}

	void design() {
		const double ep = std::sqrt(std::pow(10.0, kPassbandRippleDb / 10.0) - 1.0);
		const double es = std::sqrt(std::pow(10.0, kStopbandDb / 10.0) - 1.0);
		const double k1 = ep / es;
		const double k1p = std::sqrt((1.0 - k1) * (1.0 + k1));
		constexpr int kL = kOrder / 2;
		const auto u = [](int i) { return (2.0 * i - 1.0) / static_cast<double>(kOrder); };

		// THE DEGREE EQUATION: the selectivity this order buys at these two attenuations.
		double prod = 1.0;
		for (int i = 1; i <= kL; ++i) {
			prod *= ascend(std::sin(u(i) * std::numbers::pi * 0.5), k1p, k1);
		}
		const double kp = std::pow(k1p, kOrder) * std::pow(prod, 4.0);
		const double k = std::sqrt((1.0 - kp) * (1.0 + kp));
		selectivity = k;
		margin = std::sqrt(2.0 * k);
		gain = 1.0 / std::sqrt(1.0 + ep * ep);

		// v0 = asne(j/ep, k1) / (jN), real: the descending Landen iteration on the imaginary axis.
		std::array<double, kLandenSteps> v1 {};
		const int n1 = landen(k1, k1p, v1);
		double y = 1.0 / ep;
		for (int i = 0; i < n1; ++i) {
			const double prev = i == 0 ? k1 : v1[static_cast<size_t>(i - 1)];
			y = y / (1.0 + std::sqrt(1.0 + y * y * prev * prev)) * 2.0 / (1.0 + v1[static_cast<size_t>(i)]);
		}
		const double v0 = 2.0 / (std::numbers::pi * kOrder) * std::asinh(y);

		for (int i = 1; i <= kL; ++i) {
			// The LOW-PASS prototype's zero (j / (k·zeta)) and pole (j · cd((u - j·v0)K, k)), passband edge 1.
			const double zeta = ascend(std::cos(u(i) * std::numbers::pi * 0.5), k, kp);
			const double lpZero = 1.0 / (k * zeta);
			const double a = u(i) * std::numbers::pi * 0.5;
			const double b = v0 * std::numbers::pi * 0.5;
			const std::complex<double> w0(std::cos(a) * std::cosh(b), std::sin(a) * std::sinh(b));
			const std::complex<double> lpPole = std::complex<double>(0.0, 1.0) * ascend(w0, k, kp);
			// LOW-PASS TO HIGH-PASS, s -> 1/s, which maps a pole or zero of radius r to radius 1/r and keeps
			// the passband edge at 1.
			const double radius = std::abs(lpPole);
			Section s;
			s.pole = 1.0 / radius;
			s.damping = -2.0 * lpPole.real() / radius;
			s.zero = 1.0 / lpZero;
			s.lowMix = (s.zero / s.pole) * (s.zero / s.pole) - 1.0;
			sections[static_cast<size_t>(i - 1)] = s;
		}
		std::sort(sections.begin(), sections.end(),
		          [](const Section& x, const Section& y2) { return x.damping > y2.damping; });
	}
};

/// THE ONE DESIGN, built once during static initialisation — before any audio or message thread exists,
/// for the reason fx/filter's kSincTable gives: a lazily built static is built inside whichever call asks
/// first, which may be a render.
inline const EllipticHighpassDesign kEllipticHighpass {};

/// ONE CHANNEL OF THE BRICKWALL. Fixed size, no allocation; the owner sets the corner at its own control
/// rate (both owners use a 16-sample grid) and runs `process` per sample.
class EllipticHighpass {
public:
	/// Track `f0Hz`: the frequency to remove, with everything below it. The passband begins
	/// `kEllipticHighpass.passbandEdgeHz(f0Hz)` up — inside the octave.
	void setCorner(double f0Hz, double sampleRate) {
		f0_ = f0Hz;
		const double t = kEllipticHighpass.prewarp(f0Hz, sampleRate);
		for (size_t i = 0; i < kSections; ++i) {
			const auto& s = kEllipticHighpass.sections[i];
			const double g = s.pole * t;
			Coeffs& c = coeffs_[i];
			c.a1 = 1.0 / (1.0 + g * (g + s.damping));
			c.a2 = g * c.a1;
			c.a3 = g * c.a2;
			c.k = s.damping;
			c.m2 = s.lowMix;
		}
		// A DECAYED STATE GOES TO EXACT ZERO rather than crawling into the denormals (fx/filter's kRestLevel, for
		// the same arithmetic) — checked here, at the owner's control rate, rather than per sample.
		bool tiny = true;
		for (size_t i = 0; i < kSections && tiny; ++i) {
			tiny = std::abs(ic1_[i]) < kRestState && std::abs(ic2_[i]) < kRestState;
		}
		if (tiny) {
			reset();
		}
	}

	void reset() {
		ic1_.fill(0.0);
		ic2_.fill(0.0);
	}

	float process(float input) {
		double x = static_cast<double>(input);
		for (size_t i = 0; i < kSections; ++i) {
			const Coeffs& c = coeffs_[i];
			const double v3 = x - ic2_[i];
			const double v1 = c.a1 * ic1_[i] + c.a2 * v3;
			const double v2 = ic2_[i] + c.a2 * ic1_[i] + c.a3 * v3;
			ic1_[i] = 2.0 * v1 - ic1_[i];
			ic2_[i] = 2.0 * v2 - ic2_[i];
			x = x - c.k * v1 + c.m2 * v2;
		}
		return static_cast<float>(x * kEllipticHighpass.gain);
	}

	/// The f0 the last `setCorner` asked for — a value seam for tests (CLAUDE.md: a corner is not recoverable
	/// from a buffer).
	[[nodiscard]] double cornerHz() const { return f0_; }

private:
	static constexpr size_t kSections = EllipticHighpassDesign::kSections;
	/// BELOW THIS EVERY INTEGRATOR IS CLEARED: 400 dB under full scale, so the error it makes is nothing a float
	/// output can carry, and a tail rings down to exact zero in about two seconds at a 50 Hz corner rather than
	/// three — measured by the "settles" case in tests/test_dsp_primitives.cpp.
	static constexpr double kRestState = 1.0e-20;
	struct Coeffs {
		double a1 = 1.0;
		double a2 = 0.0;
		double a3 = 0.0;
		double k = 1.0;
		double m2 = -1.0;
	};
	std::array<Coeffs, kSections> coeffs_ {};
	std::array<double, kSections> ic1_ {};
	std::array<double, kSections> ic2_ {};
	double f0_ = 0.0;
};

} // namespace feathers::dsp
