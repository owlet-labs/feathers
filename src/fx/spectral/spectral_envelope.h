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

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// **THE SPECTRAL ENVELOPE**: the vocoder's and Formant Shift's one
// estimator. The log-magnitude spectrum, smoothed along a LOG-FREQUENCY axis by a triangle of half-width `10 octaves / Bands`
// (so Bands 8 is the coarse filterbank vocoder's bands, Bands 64 a sixth-of-an-octave envelope that follows formants and not
// harmonics), evaluated on a fixed grid of kEnvelopePoints log-spaced points from 20 Hz to 20 kHz and read back at each bin by
// linear interpolation in log frequency. Averaged in dB, so a harmonic comb's gaps pull the envelope down only as much as a
// listener hears them do. Allocates only in `prepare`.
namespace feathers::fx::spectral {

inline constexpr int kEnvelopePoints = 128;
inline constexpr double kEnvelopeLowHz = 20.0;
inline constexpr double kEnvelopeOctaves = 10.0; ///< 20 Hz to 20 kHz
inline constexpr float kEnvelopeFloorDb = -120.0f;
inline constexpr double kMinBands = 8.0;
inline constexpr double kMaxBands = 128.0; ///< up from 64

/// Grid point g's frequency: 20 Hz · 2^(10 · g / (kEnvelopePoints − 1)).
[[nodiscard]] double envelopeGridHz(int g) noexcept;

/// **FAST log2 AND exp2** (Z1b: the vocoder's per-bin transcendentals were most of its cost at Fine). log2 by the exponent and
/// an odd series in t = (m - 1) / (m + 1) to t^9 over m in [1, 2): error under 1e-5 in log2 (6e-5 dB); exp2 by the integer part
/// and e^f to f^8 over f in [0, ln 2): relative error under 2e-7. Both asserted in test_fx_spectral ("Z1b: ..."). Inputs are
/// finite; fastLog2 takes x > 0.
[[nodiscard]] inline double fastLog2(double x) noexcept {
	int e = 0;
	const double m = 2.0 * std::frexp(x, &e); // [1, 2)
	const double t = (m - 1.0) / (m + 1.0);
	const double t2 = t * t;
	return static_cast<double>(e - 1)
	       + 2.8853900817779268 * t * (1.0 + t2 * (1.0 / 3.0 + t2 * (1.0 / 5.0 + t2 * (1.0 / 7.0 + t2 * (1.0 / 9.0)))));
}
[[nodiscard]] inline double fastExp2(double y) noexcept {
	const double n = std::floor(y);
	const double f = (y - n) * 0.6931471805599453;
	const double p =
	    1.0
	    + f * (1.0 + f * (1.0 / 2.0 + f * (1.0 / 6.0 + f * (1.0 / 24.0 + f * (1.0 / 120.0 + f * (1.0 / 720.0 + f * (1.0 / 5040.0 + f / 40320.0)))))));
	return std::ldexp(p, static_cast<int>(n));
}

class EnvelopeEstimator {
public:
	/// ALLOCATE for up to `maxBins` bins.
	void prepare(int maxBins);
	/// THE ENVELOPE OF `mag` (|X| per bin, `bins` of them, bin i at i · binHz) ON THE GRID, in dB, for `bands`. A grid point no bin
	/// falls within the kernel of takes the nearest bin's level. Bins below 20 Hz (DC among them) are not read.
	/// `energy`: the kernel averages POWER (a filterbank band's energy, which a line spectrum's gaps do not pull down) rather than dB;
	/// the carrier's Flatten measures it so, the sidechain's envelope keeps the dB mean.
	void estimate(const float* mag, int bins, double binHz, double bands, std::array<float, kEnvelopePoints>& gridDb,
	              bool energy = false) noexcept;
	/// The grid's level at `hz`, in dB: log-frequency interpolation, held at the ends.
	[[nodiscard]] static double sampleDb(const std::array<float, kEnvelopePoints>& gridDb, double hz) noexcept;
	/// THE GRID READ BACK AT EACH BIN, as a linear magnitude (`out`, `bins` of them): log-frequency interpolation, held at the ends.
	static void toBins(const std::array<float, kEnvelopePoints>& gridDb, int bins, double binHz, float* out) noexcept;
	/// THE SAME, over this estimator's cached bin positions and with fastExp2 (Z1b): within 2e-7 of `toBins`, relative.
	void toBinsFast(const std::array<float, kEnvelopePoints>& gridDb, int bins, double binHz, float* out) noexcept;

private:
	/// THE BINS' FREQUENCIES, CACHED for a bin spacing (they move only with the resolution and the rate): log2 Hz per bin, each
	/// bin's grid cell and fraction for toBinsFast, and the prefix sums of the count and of log2 Hz the kernel sums need.
	void ensureBins(int bins, double binHz) noexcept;
	double cachedBinHz_ = 0.0;
	int cachedBins_ = 0;
	int first_ = 0;
	std::vector<double> logHz_;
	std::vector<int> binCell_;
	std::vector<double> binFrac_;
	std::vector<double> count_; ///< prefix: bins before i (from first_)
	std::vector<double> sumX_;  ///< prefix: sum of log2 Hz before i
	std::vector<double> sumV_;  ///< prefix: sum of the levels before i (per estimate)
	std::vector<double> sumXV_; ///< prefix: sum of log2 Hz times level before i (per estimate)
	std::vector<float> db_;    ///< each bin's level in dB (scratch)
};

} // namespace feathers::fx::spectral
