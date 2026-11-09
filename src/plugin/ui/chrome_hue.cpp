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

#include "plugin/ui/chrome_hue.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace feathers::ui {

namespace {

// Björn Ottosson's OKLab (2020), the published matrices, over linear sRGB.

double toLinear(uint32_t byte) {
	const double c = static_cast<double>(byte) / 255.0;
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double toGamma(double linear) {
	return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

struct Linear {
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
};

Linear labToLinear(double L, double a, double b) {
	const double l = L + 0.3963377774 * a + 0.2158037573 * b;
	const double m = L - 0.1055613458 * a - 0.0638541728 * b;
	const double s = L - 0.0894841775 * a - 1.2914855480 * b;
	const double l3 = l * l * l;
	const double m3 = m * m * m;
	const double s3 = s * s * s;
	return { 4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3,
		     -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3,
		     -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3 };
}

bool inGamut(const Linear& c) {
	// A hair of slack, so a colour that sits ON the boundary (every token at offset 0 does, in some
	// channel) is not shaved by float noise.
	constexpr double e = 1e-7;
	return c.r >= -e && c.r <= 1.0 + e && c.g >= -e && c.g <= 1.0 + e && c.b >= -e && c.b <= 1.0 + e;
}

uint32_t toByte(double linear) {
	const double v = std::clamp(toGamma(std::clamp(linear, 0.0, 1.0)), 0.0, 1.0);
	return static_cast<uint32_t>(std::lround(v * 255.0));
}

std::vector<ChromeHueWatch*>& hueWatches() {
	static std::vector<ChromeHueWatch*> watches;
	return watches;
}

double& offsetSlot() {
	static double offset = 0.0;
	return offset;
}

std::vector<AccentHueWatch*>& accentWatches() {
	static std::vector<AccentHueWatch*> watches;
	return watches;
}

double& accentSlot() {
	static double offset = 0.0;
	return offset;
}

} // namespace

Oklch toOklch(uint32_t argb) noexcept {
	const double r = toLinear((argb >> 16) & 0xffu);
	const double g = toLinear((argb >> 8) & 0xffu);
	const double b = toLinear(argb & 0xffu);
	const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
	const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
	const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
	const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
	const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
	const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
	return { L, std::hypot(A, B), std::atan2(B, A) * 180.0 / std::numbers::pi };
}

uint32_t fromOklch(Oklch colour, uint32_t alpha) noexcept {
	const double radians = colour.h * std::numbers::pi / 180.0;
	const double ca = std::cos(radians);
	const double sa = std::sin(radians);
	Linear c = labToLinear(colour.L, colour.C * ca, colour.C * sa);
	if (!inGamut(c)) {
		// THE GAMUT MAP: bisect on C at this L and h. Zero chroma is always inside (a grey at L in
		// [0,1]), so the bracket is sound; 40 halvings is far below one 8-bit step.
		double lo = 0.0;
		double hi = colour.C;
		for (int i = 0; i < 40; ++i) {
			const double mid = 0.5 * (lo + hi);
			if (inGamut(labToLinear(colour.L, mid * ca, mid * sa))) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		c = labToLinear(colour.L, lo * ca, lo * sa);
	}
	return ((alpha & 0xffu) << 24) | (toByte(c.r) << 16) | (toByte(c.g) << 8) | toByte(c.b);
}

uint32_t rotateHue(uint32_t argb, double degrees) noexcept {
	const double turn = std::remainder(degrees, 360.0);
	if (turn == 0.0 || !std::isfinite(turn)) {
		return argb;
	}
	Oklch c = toOklch(argb);
	c.h += turn;
	return fromOklch(c, argb >> 24);
}

ChromePalette rotatedPalette(const ChromePalette& base, double degrees) noexcept {
	return rotatedPalette(base, degrees, 0.0);
}

ChromePalette rotatedPalette(const ChromePalette& base, double chromeDegrees, double accentDegrees) noexcept {
	ChromePalette out = base;
	// THE GREYS (and the veil, a grey at an alpha): the chrome hue alone.
	for (uint32_t ChromePalette::*member :
	     { &ChromePalette::voidGround, &ChromePalette::surface, &ChromePalette::surfaceRaised, &ChromePalette::line,
	       &ChromePalette::ink, &ChromePalette::muted, &ChromePalette::disabled, &ChromePalette::pointer,
	       &ChromePalette::notifyGround, &ChromePalette::disabledVeil }) {
		out.*member = rotateHue(base.*member, chromeDegrees);
	}
	// THE ROLE FAMILY: one rotation by the SUM, so each role is rounded to bytes once, the family keeps its
	// spacing, and an accent of 0 is the chrome-only turn exactly (chrome + 0.0 == chrome).
	const double roles = chromeDegrees + accentDegrees;
	for (uint32_t ChromePalette::*member : { &ChromePalette::accent, &ChromePalette::value, &ChromePalette::modulation }) {
		out.*member = rotateHue(base.*member, roles);
	}
	// THE INSTANCE TINTS: the chrome hue alone — they are window hues, not roles.
	for (size_t i = 0; i < kInstanceTintCount; ++i) {
		out.instanceTints[i] = rotateHue(base.instanceTints[i], chromeDegrees);
	}
	return out;
}

double clampChromeHue(double degrees) noexcept {
	return std::isfinite(degrees) ? std::clamp(degrees, kChromeHueMin, kChromeHueMax) : 0.0;
}

double chromeHueOffset() noexcept { return offsetSlot(); }

size_t chromeHueWatchCount() noexcept { return hueWatches().size(); }
size_t accentHueWatchCount() noexcept { return accentWatches().size(); }

size_t setChromeHueOffset(double degrees) {
	const double next = clampChromeHue(degrees);
	if (next == offsetSlot()) {
		return 0;
	}
	offsetSlot() = next;
	chrome_detail::gChrome = rotatedPalette(kChromeBase, next, accentSlot());
	// A COPY: nothing a watch does may add or remove one mid-walk (the tooltip detail's rule).
	const std::vector<ChromeHueWatch*> watches = hueWatches();
	for (ChromeHueWatch* w : watches) {
		if (w->changed_) {
			w->changed_(next);
		}
	}
	return watches.size();
}

ChromeHueWatch::ChromeHueWatch(std::function<void(double)> changed) : changed_(std::move(changed)) {
	hueWatches().push_back(this);
}

ChromeHueWatch::~ChromeHueWatch() { std::erase(hueWatches(), this); }

double clampAccentHue(double degrees) noexcept {
	return std::isfinite(degrees) ? std::clamp(degrees, kAccentHueMin, kAccentHueMax) : 0.0;
}

double accentHueOffset() noexcept { return accentSlot(); }

size_t setAccentHueOffset(double degrees) {
	const double next = clampAccentHue(degrees);
	if (next == accentSlot()) {
		return 0;
	}
	accentSlot() = next;
	chrome_detail::gChrome = rotatedPalette(kChromeBase, offsetSlot(), next);
	const std::vector<AccentHueWatch*> watches = accentWatches();
	for (AccentHueWatch* w : watches) {
		if (w->changed_) {
			w->changed_(next);
		}
	}
	return watches.size();
}

AccentHueWatch::AccentHueWatch(std::function<void(double)> changed) : changed_(std::move(changed)) {
	accentWatches().push_back(this);
}

AccentHueWatch::~AccentHueWatch() { std::erase(accentWatches(), this); }

} // namespace feathers::ui
