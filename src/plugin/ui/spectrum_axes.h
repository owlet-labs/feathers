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

#include <algorithm>
#include <cmath>

// THE SPECTRUM AXES, shared by the additive drawer's plot (`SpectrumPlot`) and the Spectral device's card
// (`SpectralDevicePlot`), so a frequency and a level sit at the same place on both: Hz on a log scale from kMinHz to
// kMaxHz, dB linear from kTopDb down to kBottomDb, inside a plot rectangle the caller names.
namespace feathers::ui::spectrum_axes {

inline constexpr double kMinHz = 20.0;
inline constexpr double kMaxHz = 20000.0;
inline constexpr double kTopDb = 12.0;
inline constexpr double kBottomDb = -48.0;

[[nodiscard]] inline float hzToX(double hz, float left, float right) {
	const double t = std::log(std::max(hz, 1e-6) / kMinHz) / std::log(kMaxHz / kMinHz);
	return left + static_cast<float>(t) * (right - left);
}

[[nodiscard]] inline double xToHz(float x, float left, float right) {
	const double span = std::max(1e-6f, right - left);
	return kMinHz * std::pow(kMaxHz / kMinHz, static_cast<double>(x - left) / span);
}

[[nodiscard]] inline float dbToY(double db, float top, float bottom) {
	const double t = (kTopDb - db) / (kTopDb - kBottomDb);
	return top + static_cast<float>(t) * (bottom - top);
}

[[nodiscard]] inline double yToDb(float y, float top, float bottom) {
	const double span = std::max(1e-6f, bottom - top);
	return kTopDb - static_cast<double>(y - top) / span * (kTopDb - kBottomDb);
}

} // namespace feathers::ui::spectrum_axes
