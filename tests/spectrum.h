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
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

// Just enough spectrum analysis for the two test files that need it: a window with more dynamic
// range than the thing being measured, a magnitude at one exact frequency, and a full spectrum.
//
// The full spectrum is the important one and it was not here originally. The first version of the
// alias suite correlated only against a list of computed fold destinations, and the list was wrong —
// it enumerated folds against the 48 kHz output rate, which for the oversampled path is the
// decimator's stopband rather than where that path actually aliases. It therefore measured decimator
// rejection and reported it as an alias floor. Sweeping every bin cannot make that mistake, because
// it does not predict anything: whatever is not a harmonic is an alias, wherever it landed.

namespace feathers::test {

/// Seven-term Blackman-Harris. The usual four-term one bottoms out near -92 dB of sidelobe, which is
/// the same order as the alias floor being measured; this one is nearer -180 dB, so nothing showing
/// up in a bin can be the fundamental leaking sideways into it.
inline std::vector<double> blackmanHarris7(int n) {
	static constexpr double a[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
	                                0.01081174209837, 0.00077658482522, 0.00001388721735};
	std::vector<double> w(static_cast<size_t>(n));
	for (int i = 0; i < n; i++) {
		double acc = 0.0;
		for (int k = 0; k < 7; k++) {
			const double sign = (k % 2 == 0) ? 1.0 : -1.0;
			acc += sign * a[k] * std::cos(2.0 * std::numbers::pi * static_cast<double>(k * i) / static_cast<double>(n));
		}
		w[static_cast<size_t>(i)] = acc;
	}
	return w;
}

/// A window and the sum that normalises magnitudes taken through it, computed once and reused.
struct Window {
	explicit Window(int n) : w(blackmanHarris7(n)) {
		for (double v : w) {
			sum += v;
		}
	}
	std::vector<double> w;
	double sum{0.0};
};

/// Magnitude of `x` at an arbitrary, not necessarily bin-centred, frequency. Scaled so a pure
/// sinusoid of amplitude A reads A.
///
/// The phase is reduced modulo one cycle before it becomes an angle. Without that, `freqHz * i / fs`
/// reaches ~24000 cycles by the end of a one-second buffer, and the low bits of the angle — exactly
/// the ones carrying the -100 dB detail — are gone.
inline double magnitudeAt(const std::vector<float>& x, const Window& win, double sampleRate, double freqHz) {
	double re = 0.0;
	double im = 0.0;
	const double cyclesPerSample = freqHz / sampleRate;
	for (size_t i = 0; i < x.size(); i++) {
		const double angle = 2.0 * std::numbers::pi * std::fmod(cyclesPerSample * static_cast<double>(i), 1.0);
		const double v = win.w[i] * static_cast<double>(x[i]);
		re += v * std::cos(angle);
		im -= v * std::sin(angle);
	}
	return 2.0 * std::sqrt(re * re + im * im) / win.sum;
}

/// Amplitude spectrum of `x` through `win`: entry k is the amplitude a sinusoid sitting exactly on
/// bin k would read. Returns N/2 + 1 entries, and `x.size()` must be a power of two.
inline std::vector<double> amplitudeSpectrum(const std::vector<float>& x, const Window& win) {
	const size_t n = x.size();
	std::vector<std::complex<double>> a(n);
	for (size_t i = 0; i < n; i++) {
		a[i] = std::complex<double>(win.w[i] * static_cast<double>(x[i]), 0.0);
	}

	// Iterative radix-2 Cooley-Tukey: bit-reversal permutation, then log2(n) butterfly passes.
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; (j & bit) != 0; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(a[i], a[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double theta = -2.0 * std::numbers::pi / static_cast<double>(len);
		for (size_t i = 0; i < n; i += len) {
			for (size_t k = 0; k < len / 2; k++) {
				const std::complex<double> w(std::cos(theta * static_cast<double>(k)),
				                             std::sin(theta * static_cast<double>(k)));
				const std::complex<double> u = a[i + k];
				const std::complex<double> v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
			}
		}
	}

	std::vector<double> out(n / 2 + 1);
	for (size_t k = 0; k < out.size(); k++) {
		out[k] = 2.0 * std::abs(a[k]) / win.sum;
	}
	return out;
}

inline double dB(double ratio) {
	return 20.0 * std::log10(std::max(ratio, 1e-30));
}

inline double noteToHz(double midiNote) {
	return 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
}

} // namespace feathers::test
