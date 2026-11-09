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

// THE VOWEL SET, ONE TABLE FOR TWO READERS: PhiVox's zone anchors (slice_build.cpp, buildVoxSliced: the vowel a V5 voice
// sings in vox country) and the spectral drawer's Formant operator (additive design §3.2: "reuse PhiVox's formant data as
// the vowel set rather than inventing one"). Moved here from slice_build.cpp unchanged, so a retune flows to both;
// test_vox_voice and test_flagship hold PhiVox's output to the bit across the move.
//
// WHAT PHIVOX HAS IS F1 AND F2 ONLY. Its bandwidths are not data: VOSIM's emerge from each zone's pulse count and decay,
// and its levels from 0.62 and 0.62 × a φ balance. So the Formant operator's widths and levels are its own constants
// (spectral_shaper.h, kVowelBandwidth*, kVowelF*Level), tuned by ear.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace feathers {

inline constexpr std::size_t kVoxAnchorCount = 8;
/// Vowel formant anchors per zone (F1, F2 in Hz). In PhiVox they are gravity wells only: the φ banks wander ±0.6..0.8
/// octaves around them.
///                                                          Breath   Hum     Round   Open    Bright  Nasal   Growl   Rasp
inline constexpr std::array<float, kVoxAnchorCount> kVoxAnchorF1 = {300.0f, 350.0f, 450.0f, 750.0f, 550.0f, 900.0f, 250.0f, 700.0f};
inline constexpr std::array<float, kVoxAnchorCount> kVoxAnchorF2 = {2200.0f, 800.0f, 900.0f, 1150.0f, 1900.0f, 1600.0f, 600.0f, 3400.0f};
inline constexpr std::array<const char* const, kVoxAnchorCount> kVoxAnchorNames{"Breath", "Hum",   "Round", "Open",
                                                                                "Bright", "Nasal", "Growl", "Rasp"};

struct VowelFormants {
	double f1Hz{0.0};
	double f2Hz{0.0};
};

/// THE FORMANT OPERATOR'S WALK: position 0..7 runs through the anchors in order, interpolated in LOG-Hz between
/// neighbours (a formant glides by ratio, as vox_bake's blend does), so an integer position is that anchor exactly.
/// Clamped to [0, 7]; a NaN reads as 0. NOT PhiVox's own walk, which interpolates in Hz on the raw zone
/// (slice_build.cpp) — that one is the firmware's and stays as it is.
inline VowelFormants vowelFormants(double position) {
	constexpr double kLast = static_cast<double>(kVoxAnchorCount - 1);
	const double p = position >= 0.0 ? std::min(position, kLast) : 0.0;
	const auto i = std::min(static_cast<std::size_t>(p), kVoxAnchorCount - 2);
	const double t = p - static_cast<double>(i);
	const auto at = [&](const std::array<float, kVoxAnchorCount>& a) {
		const double lo = static_cast<double>(a[i]);
		const double hi = static_cast<double>(a[i + 1]);
		return t == 0.0 ? lo : t == 1.0 ? hi : lo * std::exp2(t * std::log2(hi / lo));
	};
	return {at(kVoxAnchorF1), at(kVoxAnchorF2)};
}

} // namespace feathers
