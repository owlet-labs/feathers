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
/// **THE BANDS' NAMES, EXACT OR STRUCTURAL.** A comb's bands are harmonic classes ONLY when its teeth sit on the harmonics of the input's pitch, and only a tracked note
/// says what that is. So `harmonic` (Key Track on and a note heard) with the first spacing at x1 of it names them as harmonics: the second spacing at x2 makes
/// Between / Odd / Even, at a whole n >= 3 Between / Others / Every n. EVERYTHING ELSE (a fixed spacing in Hz, a ratio that is not x1, a second spacing that is
/// neither) is named for what it structurally is, which is always true: Gaps (between the first comb's teeth), Teeth A (on them, off the second comb's) and Teeth B (on both).
/// `r1` and `r2` are the two spacings as ratios to the note (`combRatioOfKnob`); untracked they are unused.
[[nodiscard]] inline std::array<std::string, 3> combBandNames(double r1, double r2, bool harmonic) {
	if (harmonic && std::fabs(r1 - 1.0) < 0.02) {
		const double r = r2 / r1;
		if (std::fabs(r - 2.0) < 0.02) {
			return { "Between", "Odd", "Even" };
		}
		if (r > 2.5 && std::fabs(r - std::round(r)) < 0.02) {
			return { "Between", "Others", "Every " + std::to_string(static_cast<int>(std::lround(r))) };
		}
	}
	return { "Gaps", "Teeth A", "Teeth B" };
}

/// **THE TWO BANDS OF ONE COMB'S NAMES**, by the same rule: harmonic only for a tracked note, with the spacing at x1 (Between / Harmonics) or a whole n >= 2
/// (Rest / Every n); otherwise Gaps / Teeth.
[[nodiscard]] inline std::array<std::string, 2> combBandNames2(double r1, bool harmonic) {
	if (harmonic && std::fabs(r1 - 1.0) < 0.02) {
		return { "Between", "Harmonics" };
	}
	if (harmonic && r1 > 1.5 && std::fabs(r1 - std::round(r1)) < 0.02) {
		return { "Rest", "Every " + std::to_string(static_cast<int>(std::lround(r1))) };
	}
	return { "Gaps", "Teeth" };
}

/// **WHAT THE TEETH ARE, in words for a caption or a tooltip**: the first spacing (Hz, or the ratio to the note while tracked) and, with a second comb, which teeth B takes.
[[nodiscard]] inline std::string combTeethCaption(double spacing1Hz, bool tracked, double r1, double spacing2Over1, bool threeBands) {
	char out[96];
	if (tracked) {
		std::snprintf(out, sizeof out, "Teeth every %s of the note (%.0f Hz)", combRatioText(r1).c_str(), spacing1Hz);
	}
	else {
		std::snprintf(out, sizeof out, "Teeth every %.0f Hz", spacing1Hz);
	}
	std::string text = out;
	if (threeBands) {
		if (std::fabs(spacing2Over1 - std::round(spacing2Over1)) < 0.02 && spacing2Over1 > 1.5) {
			text += "; B takes every " + std::to_string(static_cast<int>(std::lround(spacing2Over1))) + (std::lround(spacing2Over1) == 2 ? "nd" : std::lround(spacing2Over1) == 3 ? "rd" : "th") + " tooth";
		}
		else {
			text += "; B takes the teeth of a comb " + combRatioText(spacing2Over1) + " as wide";
		}
	}
	return text;
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
