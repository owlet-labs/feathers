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
#include <numbers>

namespace feathers::fx::dyn {

/// A TRANSPOSED DIRECT-FORM-II BIQUAD, in double.
///
/// **THE ONE COPY.** Bus Utility's meter (`busutil_meter.h`'s K-weighting) and its output highpass run
/// on this struct too since backlog JJ; it had its own identical copy, and two copies of one filter
/// body drift. It lives here rather than in Bus Utility because including `busutil_meter.h` would pull
/// a LUFS meter into every dynamics device.
struct Biquad {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
	double z1 = 0.0, z2 = 0.0;

	[[nodiscard]] double step(double x) noexcept {
		const double y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
	void clear() noexcept { z1 = z2 = 0.0; }

	/// RBJ HIGHPASS AT Q = 1/sqrt(2), which IS Butterworth: 12 dB/oct, -3 dB at `hz` (Bus Utility's
	/// `refreshFilter` design). **THE STATE IS KEPT**, so moving the corner mid-stream does not
	/// silence the filter for a block.
	void designHighpass(double hz, double sampleRate) noexcept {
		constexpr double kButterworthQ = 0.70710678118654752;
		const double w0 = 2.0 * std::numbers::pi_v<double> * hz / sampleRate;
		const double cosw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * kButterworthQ);
		const double a0 = 1.0 + alpha;
		b0 = ((1.0 + cosw) * 0.5) / a0;
		b1 = -(1.0 + cosw) / a0;
		b2 = ((1.0 + cosw) * 0.5) / a0;
		a1 = (-2.0 * cosw) / a0;
		a2 = (1.0 - alpha) / a0;
	}
};

} // namespace feathers::fx::dyn
