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

#include "dsp/portable_trig.h"

#include <cmath>

namespace feathers::dsp {

namespace {

// pi/2 as a 33-bit head and the tail beyond it (fdlibm's pio2_1 / pio2_1t): k * head is exact for the
// |k| this file is accurate over, so the reduction loses nothing until the tail.
constexpr double kPio2Hi = 1.57079632673412561417e+00;
constexpr double kPio2Lo = 6.07710050650619224932e-11;
constexpr double kTwoOverPi = 6.36619772367581382433e-01;

// sin and cos on [-pi/4, pi/4], Taylor to r^15 and r^16 (the first omitted terms are below 5e-17 there).
double sinKernel(double r) {
	const double r2 = r * r;
	const double p = -1.0 / 6.0
	                 + r2 * (1.0 / 120.0
	                         + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0 + r2 * (-1.0 / 39916800.0 + r2 * (1.0 / 6227020800.0 + r2 * (-1.0 / 1307674368000.0))))));
	return r + r * r2 * p;
}

double cosKernel(double r) {
	const double r2 = r * r;
	const double p = 1.0 / 24.0
	                 + r2 * (-1.0 / 720.0
	                         + r2 * (1.0 / 40320.0 + r2 * (-1.0 / 3628800.0 + r2 * (1.0 / 479001600.0 + r2 * (-1.0 / 87178291200.0 + r2 * (1.0 / 20922789888000.0))))));
	return 1.0 - 0.5 * r2 + r2 * r2 * p;
}

// x = k pi/2 + r, |r| <= pi/4; the quadrant k mod 4.
double reduce(double x, int& quadrant) {
	const double k = std::nearbyint(x * kTwoOverPi); // exact under the default rounding mode, on every host
	quadrant = static_cast<int>(static_cast<long long>(k) & 3);
	return (x - k * kPio2Hi) - k * kPio2Lo;
}

} // namespace

float portableSin(float x) {
	int q = 0;
	const double r = reduce(static_cast<double>(x), q);
	const double v = (q == 0) ? sinKernel(r) : (q == 1) ? cosKernel(r) : (q == 2) ? -sinKernel(r) : -cosKernel(r);
	return static_cast<float>(v);
}

float portableCos(float x) {
	int q = 0;
	const double r = reduce(static_cast<double>(x), q);
	const double v = (q == 0) ? cosKernel(r) : (q == 1) ? -sinKernel(r) : (q == 2) ? -cosKernel(r) : sinKernel(r);
	return static_cast<float>(v);
}

} // namespace feathers::dsp
