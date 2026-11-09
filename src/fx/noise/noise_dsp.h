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

#include "fx/noise/noise_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>

// NOISE'S TWO FILTERS, AS DESIGNS — the colour tilt and the SVF. In a header so the DSP and the card evaluate
// the SAME coefficients: the card's spectrum is the response the device installs, not a drawing of the idea.

namespace feathers::fx::noise {

/// THE COLOUR TILT: a cascade of first-order shelves, one per octave from 20 Hz, each a pole at f and a zero at
/// f x 2^colour. Between the first and last pole the response falls 6.02 x colour dB per octave with a ripple
/// under half a dB (measured, `test_fx_noise.cpp`). At colour +1 each zero lands on the next pole and the cascade
/// collapses to one pole at 20 Hz: exact brown. At -1, one zero at 10 Hz and the top pole: exact violet.
///
/// **THE LOUDNESS IS HELD, NOT THE LEVEL AT ONE FREQUENCY**: `norm` makes the mean power over 20 Hz .. 20 kHz
/// (or the band below Nyquist) equal to white's, so turning Colour moves where the energy is and not how much
/// of it there is. Sub-audio rumble is not counted, and brown is flat below 20 Hz, so it cannot hide any.
struct Tilt {
	static constexpr int kSections = 11;
	static constexpr double kLowestPoleHz = 20.0;
	static constexpr double kBandLowHz = 20.0;
	static constexpr double kBandHighHz = 20000.0;

	std::array<float, kSections> b0 {}, b1 {}, a1 {};
	std::array<double, kSections> b0d {}, b1d {}, a1d {};
	float norm = 1.0f;
	double sampleRate = 48000.0;

	void design(float colour, double fs) {
		sampleRate = std::max(fs, 1.0);
		const double beta = std::clamp(static_cast<double>(colour), -1.0, 1.0);
		const double ceiling = 0.45 * sampleRate;
		for (int k = 0; k < kSections; ++k) {
			const double fp = std::min(kLowestPoleHz * std::exp2(static_cast<double>(k)), ceiling);
			const double fz = std::min(fp * std::exp2(beta), ceiling);
			// PREWARPED BILINEAR: s = (1 - z^-1) / (1 + z^-1), corners at tan(pi f / fs). Unity at DC.
			const double wp = std::tan(std::numbers::pi * fp / sampleRate);
			const double wz = std::tan(std::numbers::pi * fz / sampleRate);
			const double scale = wp / (wz * (wp + 1.0));
			b0d[static_cast<size_t>(k)] = scale * (wz + 1.0);
			b1d[static_cast<size_t>(k)] = scale * (wz - 1.0);
			a1d[static_cast<size_t>(k)] = (wp - 1.0) / (wp + 1.0);
			b0[static_cast<size_t>(k)] = static_cast<float>(b0d[static_cast<size_t>(k)]);
			b1[static_cast<size_t>(k)] = static_cast<float>(b1d[static_cast<size_t>(k)]);
			a1[static_cast<size_t>(k)] = static_cast<float>(a1d[static_cast<size_t>(k)]);
		}
		// THE MEAN POWER OVER THE BAND, integrated in log frequency (|H|^2 f dln f), against white's 1.
		constexpr int kGrid = 96;
		const double lo = kBandLowHz;
		const double hi = std::min(kBandHighHz, ceiling);
		const double step = std::log(hi / lo) / static_cast<double>(kGrid - 1);
		double power = 0.0;
		for (int i = 0; i < kGrid; ++i) {
			const double f = lo * std::exp(step * static_cast<double>(i));
			const double g = shapeGain(f);
			const double w = (i == 0 || i == kGrid - 1) ? 0.5 : 1.0;
			power += w * g * g * f * step;
		}
		power /= (hi - lo);
		norm = static_cast<float>(power > 0.0 ? 1.0 / std::sqrt(power) : 1.0);
	}

	/// |H| OF THE CASCADE BEFORE `norm`, at `hz`.
	[[nodiscard]] double shapeGain(double hz) const {
		const double w = 2.0 * std::numbers::pi * hz / sampleRate;
		const std::complex<double> z1 = std::polar(1.0, -w);
		double g = 1.0;
		for (size_t k = 0; k < static_cast<size_t>(kSections); ++k) {
			g *= std::abs((b0d[k] + b1d[k] * z1) / (1.0 + a1d[k] * z1));
		}
		return g;
	}

	/// |H| AS INSTALLED — the cascade times `norm`.
	[[nodiscard]] double gain(double hz) const { return shapeGain(hz) * static_cast<double>(norm); }
};

/// ONE CHANNEL'S TILT MEMORY — transposed direct form II, one word a section.
struct TiltState {
	std::array<float, Tilt::kSections> s {};

	void clear() { s.fill(0.0f); }

	[[nodiscard]] float step(const Tilt& t, float x) {
		for (size_t k = 0; k < static_cast<size_t>(Tilt::kSections); ++k) {
			const float y = t.b0[k] * x + s[k];
			s[k] = t.b1[k] * x - t.a1[k] * y;
			x = y;
		}
		return x * t.norm;
	}
};

/// THE SVF — Simper's trapezoidal state-variable filter, scalar, one response at a time. Bandpass is the
/// constant-peak form (k x v1), so a ringing bandpass is no louder at its centre than an open one.
///
/// **RESONANCE STOPS SHORT OF SELF-OSCILLATION**: k = 2 - 1.9 x res, so at 100% the peak is +26 dB and the
/// filter still decays. The device adds, and a filter that sang on its own would add sound with Level at 0.
struct Svf {
	double g = 1.0;
	double k = 2.0;
	float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, kf = 2.0f;
	int type = static_cast<int>(FilterType::Lowpass);
	double sampleRate = 48000.0;

	[[nodiscard]] static double kOf(float resonance) {
		return 2.0 - 1.9 * std::clamp(static_cast<double>(resonance), 0.0, 1.0);
	}

	/// THE CORNER IS CLAMPED UNDER NYQUIST (0.45 fs) AND OVER 5 Hz, where tan is tame.
	void design(int filterType, double cornerHz, float resonance, double fs) {
		sampleRate = std::max(fs, 1.0);
		type = filterType;
		const double hz = std::clamp(cornerHz, 5.0, 0.45 * sampleRate);
		g = std::tan(std::numbers::pi * hz / sampleRate);
		k = kOf(resonance);
		const double d1 = 1.0 / (1.0 + g * (g + k));
		a1 = static_cast<float>(d1);
		a2 = static_cast<float>(g * d1);
		a3 = static_cast<float>(g * g * d1);
		kf = static_cast<float>(k);
	}

	[[nodiscard]] double cornerHz() const { return std::atan(g) * sampleRate / std::numbers::pi; }

	/// |H| OF THIS DESIGN AT `hz` — the analog prototype at the prewarped frequency, which the trapezoidal
	/// filter matches exactly.
	[[nodiscard]] double gain(double hz) const {
		const double w = std::tan(std::numbers::pi * std::clamp(hz, 0.0, 0.4999 * sampleRate) / sampleRate) / g;
		const std::complex<double> s(0.0, w);
		const std::complex<double> den = s * s + k * s + 1.0;
		switch (static_cast<FilterType>(type)) {
		case FilterType::Bandpass: return std::abs(k * s / den);
		case FilterType::Highpass: return std::abs(s * s / den);
		case FilterType::Lowpass:
		case FilterType::kCount: break;
		}
		return std::abs(1.0 / den);
	}
};

struct SvfState {
	float ic1 = 0.0f, ic2 = 0.0f;

	void clear() { ic1 = ic2 = 0.0f; }

	/// **A RINGING CLICK ENDS AT EXACT ZERO, NOT ON A DENORMAL.** Between impulses the filter decays geometrically
	/// and, at a low corner, would park in the denormals — costly to compute and never silent (the silence sweep's
	/// lesson, `test_fx_devices.cpp`). Under -400 dB the memory is zero; nothing can hear the difference.
	void flushTiny() {
		constexpr float kRest = 1.0e-20f;
		if (std::fabs(ic1) < kRest && std::fabs(ic2) < kRest) {
			ic1 = ic2 = 0.0f;
		}
	}

	[[nodiscard]] float step(const Svf& f, float v0) {
		const float v3 = v0 - ic2;
		const float v1 = f.a1 * ic1 + f.a2 * v3;
		const float v2 = ic2 + f.a2 * ic1 + f.a3 * v3;
		ic1 = 2.0f * v1 - ic1;
		ic2 = 2.0f * v2 - ic2;
		switch (static_cast<FilterType>(f.type)) {
		case FilterType::Bandpass: return f.kf * v1;
		case FilterType::Highpass: return v0 - f.kf * v1 - v2;
		case FilterType::Lowpass:
		case FilterType::kCount: break;
		}
		return v2;
	}
};

/// WHITE NOISE: xorshift64*, uniform on [-1, 1). Its own stream, so the impulses' draws never shift it.
struct WhiteNoise {
	uint64_t state = 0x9e3779b97f4a7c15ull;

	void seed(uint64_t s) {
		state = s * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
		if (state == 0) {
			state = 0x9e3779b97f4a7c15ull;
		}
	}

	[[nodiscard]] float next() {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		const uint64_t r = state * 0x2545f4914f6cdd1dull;
		return static_cast<float>(static_cast<double>(r >> 11) * (2.0 / 9007199254740992.0) - 1.0);
	}
};

} // namespace feathers::fx::noise
