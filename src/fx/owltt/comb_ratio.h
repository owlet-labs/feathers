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
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

// THE COMB'S RATIOS AND THE KEY TRACK'S NOTE, shared by every device that splits by comb (OWLTT's Comb modes, Stereo Balance): how a
// spacing knob reads as a ratio to the played note, the detents a ratio snaps to, the names of the bands those spacings make, and the
// note's frequency. MOVED, not copied, out of `owltt_types.h`, which includes this file: OWLTT's behaviour is unchanged to the bit.

namespace feathers::fx::owltt {

/// the knob / 5 in Hz (its default 2000 reads as 400 Hz, twice Low's 200).
inline constexpr double kCombLowUnityKnob = 200.0;  ///< Low Crossover's value for x1
inline constexpr double kCombHighUnityKnob = 1000.0; ///< High Crossover's value for x1
/// The spacing a knob means, Hz: the knob itself for Low, a fifth of it for High (the default pair 200 / 2000 is then 200 / 400).
[[nodiscard]] inline double combSpacingHz(double knob, bool high) noexcept { return high ? knob * (kCombLowUnityKnob / kCombHighUnityKnob) : knob; }
/// The ratio to the note a knob means while tracking.
[[nodiscard]] inline double combRatioOfKnob(double knob, bool high) noexcept { return knob / (high ? kCombHighUnityKnob : kCombLowUnityKnob); }
[[nodiscard]] inline double combKnobOfRatio(double ratio, bool high) noexcept { return ratio * (high ? kCombHighUnityKnob : kCombLowUnityKnob); }
/// THE DETENTS a ratio snaps to, as the Harm device's harmonic box has them: simple ratios from x1/8 to x8.
inline constexpr double kCombDetents[] = { 1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0 };
inline constexpr double kCombDetentReach = 0.04; ///< a ratio within 4% of a detent (in log) is on it
/// The ratio, snapped to a detent when within reach.
[[nodiscard]] inline double combSnapRatio(double ratio) noexcept {
	for (const double d : kCombDetents) {
		if (ratio > 0.0 && std::fabs(std::log(ratio / d)) < kCombDetentReach) {
			return d;
		}
	}
	return ratio;
}
/// A ratio as it reads: "x1", "x2", "x1/2" on a detent, else two significant figures ("x1.4").
[[nodiscard]] inline std::string combRatioText(double ratio) {
	static constexpr const char* kNames[] = { "x1/8", "x1/4", "x1/3", "x1/2", "x1", "x2", "x3", "x4", "x8" };
	for (size_t i = 0; i < std::size(kCombDetents); ++i) {
		if (std::fabs(std::log(std::max(ratio, 1e-9) / kCombDetents[i])) < 1.0e-3) {
			return kNames[i];
		}
	}
	char out[24] = {};
	std::snprintf(out, sizeof(out), "x%.2g", ratio);
	return out;
}
/// THE COLUMNS' NAMES FOLLOW THE RATIOS: the second spacing over the first says what the on-teeth band is cut into. At 2 the halves are the odd and
/// the even teeth of the first comb; at a whole n the High band is every n-th and Mid the rest; otherwise the two are named for the second comb.
[[nodiscard]] inline std::array<std::string, 3> combBandNames(double spacing1, double spacing2) {
	const double r = spacing2 / std::max(spacing1, 1e-9);
	if (std::fabs(r - 2.0) < 0.02) {
		return { "Between", "Odd", "Even" };
	}
	if (r > 2.5 && std::fabs(r - std::round(r)) < 0.02) {
		return { "Between", "Others", "Every " + std::to_string(static_cast<int>(std::lround(r))) };
	}
	return { "Between", "Off " + combRatioText(r), "On " + combRatioText(r) };
}

/// **THE TWO BANDS OF ONE COMB'S NAMES** (Stereo Balance's two-band mode; OWLTT's two-split `combBandNames` is the three-band reading): `ratio` is the tooth spacing over the
/// played note (x1 = the note's own harmonics). Index 0 is what falls between the teeth, index 1 what sits on them. At x1 the teeth are the harmonics; at a whole n the on-teeth
/// band is every n-th harmonic (x2: the even ones) and the other holds the rest; otherwise the two are named for the spacing.
[[nodiscard]] inline std::array<std::string, 2> combBandNames2(double ratio) {
	if (std::fabs(ratio - 1.0) < 0.02) {
		return { "Between", "Harmonics" };
	}
	if (std::fabs(ratio - 2.0) < 0.02) {
		return { "Rest", "Even" };
	}
	if (ratio > 2.5 && std::fabs(ratio - std::round(ratio)) < 0.02) {
		return { "Rest", "Every " + std::to_string(static_cast<int>(std::lround(ratio))) };
	}
	return { "Off " + combRatioText(ratio), "On " + combRatioText(ratio) };
}

/// THE NOTE'S OWN FREQUENCY, A4 = 69, the pitch clamped to [−60, 200] semitones — `fx::phaser::keyHzOf`, restated.
[[nodiscard]] inline double keyHzOf(double pitchSemitones) noexcept {
	const double p = std::isfinite(pitchSemitones) ? std::clamp(pitchSemitones, -60.0, 200.0) : 69.0;
	return 440.0 * std::exp2((p - 69.0) / 12.0);
}

/// **THE CROSSOVERS' ONE REFERENCE**: Low Crossover's default, 200 Hz, as the row stores it. WHERE THE PHASER'S CONTRACT
/// DID NOT FIT: its one knob is measured from its own default; two corners each measured from their own default would
/// both land on the note. From ONE reference the default split transposes with the note — Low on it, High 39.9
/// semitones (its default's 2 kHz) above.
inline constexpr double kCrossoverKeyRefHz = 200.0;

/// A CROSSOVER KNOB (Hz) AS AN OFFSET FROM THE NOTE, semitones.
[[nodiscard]] inline double crossoverKeyOffsetSemitones(double knobHz) noexcept {
	return 12.0 * std::log2(std::max(knobHz, 1e-6) / kCrossoverKeyRefHz);
}

/// **THE CORNER THE CROSSOVER RUNS AT**, Hz: the knob, or — tracking (Key Track on and a note heard) — the note moved
/// by the knob's offset, clamped to the audible 20 Hz .. 20 kHz.
[[nodiscard]] inline double trackedCrossoverHz(double knobHz, bool tracking, double noteHz) noexcept {
	if (!tracking) {
		return knobHz;
	}
	return std::clamp(noteHz * (knobHz / kCrossoverKeyRefHz), 20.0, 20000.0);
}

/// The glide and its landing — the Phaser's (`kKeySmoothSeconds`, `kGlideSnap`).
inline constexpr double kKeySmoothSeconds = 0.005;
inline constexpr double kGlideSnap = 1.0e-7;

} // namespace feathers::fx::owltt
