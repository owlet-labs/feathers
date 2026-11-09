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

#include <cmath>
#include <cstddef>

// THE SPECTRUM ANALYSER'S LOG BINS — the EQ's, lifted so the Rack's spectrum strip
// (rack stage 2) reads the same bins. No FFT here: this is what a view needs to place a bin, and the UI includes
// it without pulling pffft in. The analyser itself is `dsp/analyser.h`.

namespace feathers::dsp {

/// 96 BINS FROM 20 Hz TO 20 kHz, ~9.6 an octave, each a power DENSITY (the mean over the FFT bins it covers, or the
/// interpolated power at its centre where it is narrower than one), so pink noise reads -3 dB/oct as on any per-bin
/// analyser.
inline constexpr size_t kAnalyserBins = 96;
inline constexpr double kAnalyserLowHz = 20.0;
inline constexpr double kAnalyserHighHz = 20000.0;
/// What a bin with no energy at all publishes, in dB.
inline constexpr float kAnalyserFloorDb = -160.0f;

[[nodiscard]] inline double analyserBinHz(size_t k) noexcept {
	return kAnalyserLowHz
	       * std::pow(kAnalyserHighHz / kAnalyserLowHz, (static_cast<double>(k) + 0.5) / static_cast<double>(kAnalyserBins));
}

/// THE VIEW a finished frame is computed in: the mean of the channels' power, or the power of their mid or side.
enum class AnalyserView : int { Stereo = 0, Mid = 1, Side = 2 };

} // namespace feathers::dsp
