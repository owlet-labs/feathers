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

// THE EQ PLOT'S GEOMETRY AND GRID — pure functions, no window, so the
// grid's tiers are asserted as VALUES by a test rather than inferred from pixels.
//
// x is log frequency over 20 Hz .. 20 kHz; y is dB over ±range. The grid adapts to the pixels it has:
// frequency lines thicken from decades to 1-2-5 to every 1..9 as a decade widens, labels are placed only
// where they fit, octave C lines appear once an octave is wide enough, and the dB step is the finest that
// keeps lines 24 px apart.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace feathers::ui::eq_plot {

inline constexpr double kLowHz = 20.0;
inline constexpr double kHighHz = 20000.0;
inline constexpr double kDecades = 3.0; // log10(20000 / 20)

struct Geometry {
	float x = 0.0f, y = 0.0f, width = 1.0f, height = 1.0f;
	float rangeDb = 24.0f;

	[[nodiscard]] float xForHz(double hz) const noexcept {
		const double t = std::log10(std::max(hz, 1e-3) / kLowHz) / kDecades;
		return x + static_cast<float>(t) * width;
	}
	[[nodiscard]] double hzForX(float px) const noexcept {
		const double t = static_cast<double>((px - x) / std::max(width, 1.0f));
		return kLowHz * std::pow(10.0, t * kDecades);
	}
	[[nodiscard]] float yForDb(double db) const noexcept {
		const double t = (static_cast<double>(rangeDb) - db) / (2.0 * static_cast<double>(rangeDb));
		return y + static_cast<float>(t) * height;
	}
	[[nodiscard]] double dbForY(float py) const noexcept {
		const double t = static_cast<double>((py - y) / std::max(height, 1.0f));
		return static_cast<double>(rangeDb) - t * 2.0 * static_cast<double>(rangeDb);
	}
	[[nodiscard]] float pixelsPerDecade() const noexcept { return width / static_cast<float>(kDecades); }
	[[nodiscard]] float pixelsPerOctave() const noexcept {
		return width / static_cast<float>(kDecades / std::log10(2.0));
	}
};

/// THE FREQUENCY TIERS, by pixels per decade.
inline constexpr float kTwoFiveFromPx = 120.0f;
inline constexpr float kEveryDigitFromPx = 300.0f;
/// Octave C lines (musical grid) once an octave is this wide.
inline constexpr float kNoteLinesFromPxPerOctave = 60.0f;
/// The finest dB step whose lines stay at least this far apart.
inline constexpr float kMinDbSpacingPx = 24.0f;
inline constexpr std::array<float, 5> kDbSteps { 1.0f, 2.0f, 3.0f, 6.0f, 12.0f };

enum class Tier { Decade, TwoFive, Digit };

struct FreqLine {
	double hz;
	Tier tier;
};

/// Every frequency line the plot draws at this width, decades first.
[[nodiscard]] inline std::vector<FreqLine> frequencyLines(const Geometry& g) {
	const float perDecade = g.pixelsPerDecade();
	std::vector<FreqLine> out;
	for (double decade = 10.0; decade <= 10000.0; decade *= 10.0) {
		for (int d = 1; d <= 9; ++d) {
			const double hz = decade * d;
			if (hz < kLowHz || hz > kHighHz) {
				continue;
			}
			const Tier tier = d == 1 ? Tier::Decade : (d == 2 || d == 5) ? Tier::TwoFive : Tier::Digit;
			if (tier == Tier::TwoFive && perDecade < kTwoFiveFromPx) {
				continue;
			}
			if (tier == Tier::Digit && perDecade < kEveryDigitFromPx) {
				continue;
			}
			out.push_back({ hz, tier });
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const FreqLine& a, const FreqLine& b) {
		return static_cast<int>(a.tier) < static_cast<int>(b.tier);
	});
	return out;
}

/// "20", "500", "1k", "2.5k", "10k".
[[nodiscard]] inline std::string hzLabel(double hz) {
	char out[16] = {};
	if (hz >= 1000.0) {
		const double k = hz / 1000.0;
		std::snprintf(out, sizeof(out), std::fabs(k - std::round(k)) < 1e-9 ? "%.0fk" : "%.1fk", k);
	}
	else {
		std::snprintf(out, sizeof(out), "%.0f", hz);
	}
	return out;
}

struct Label {
	double hz;
	std::string text;
	float left;
	float right;
};

/// THE LABELS THAT FIT, placed in tier order (decades win) and refused where they would come within `gap`
/// of one already placed. `charWidth` is the measured average glyph width of the label font.
[[nodiscard]] inline std::vector<Label> frequencyLabels(const Geometry& g, float charWidth, float gap = 8.0f) {
	std::vector<Label> placed;
	for (const FreqLine& line : frequencyLines(g)) {
		Label l { line.hz, hzLabel(line.hz), 0.0f, 0.0f };
		const float w = charWidth * static_cast<float>(l.text.size());
		const float cx = g.xForHz(line.hz);
		l.left = std::clamp(cx - 0.5f * w, g.x, g.x + g.width - w);
		l.right = l.left + w;
		const bool clashes = std::any_of(placed.begin(), placed.end(), [&](const Label& p) {
			return l.left < p.right + gap && p.left < l.right + gap;
		});
		if (!clashes) {
			placed.push_back(l);
		}
	}
	return placed;
}

/// The dB grid step: the finest of 1/2/3/6/12 dB whose lines stay `kMinDbSpacingPx` apart.
[[nodiscard]] inline float dbStep(const Geometry& g) {
	for (const float step : kDbSteps) {
		const float px = g.height * step / (2.0f * g.rangeDb);
		if (px >= kMinDbSpacingPx) {
			return step;
		}
	}
	return kDbSteps.back();
}

[[nodiscard]] inline bool drawsNoteLines(const Geometry& g) {
	return g.pixelsPerOctave() >= kNoteLinesFromPxPerOctave;
}

/// C of each octave in range, in Hz (C at MIDI 12n: 440 * 2^((12n - 69) / 12)).
[[nodiscard]] inline std::vector<double> noteCs() {
	std::vector<double> out;
	for (int midi = 0; midi <= 132; midi += 12) {
		const double hz = 440.0 * std::exp2((midi - 69) / 12.0);
		if (hz >= kLowHz && hz <= kHighHz) {
			out.push_back(hz);
		}
	}
	return out;
}

} // namespace feathers::ui::eq_plot
