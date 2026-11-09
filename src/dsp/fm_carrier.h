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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

// THE FM CARRIER — a plugin-owned 1024-point float sine table (doc §10 T1, fm-architecture-measurements.md
// §3). NOT the firmware's 256-point table (kFwSineWaveDiff is {value,diff} Q31 pairs) and NOT the smooth
// source (whose harmonic content is coupled to an audition-tunable field). A dedicated table: 9.4x
// cheaper than the smooth-source path, 79 dB cleaner, THD −116.9 dB. Linearly interpolated, wrap by
// construction (the table is one full cycle and the lookup wraps the phase), ~40 lines.

namespace feathers::fm {

/// TABLE SIZE — 1024 points. A power of two so the phase-to-index multiply is the measurement's 4.65 ns
/// eval, and 1024 resolves the sine to THD −116.9 dB (the 512-point table is −116.9 dB too at half the
/// memory; 1024 is the measured sweet spot the doc records).
inline constexpr size_t kCarrierSlots = 1024;
/// ONE MORE THAN the top index — the guard sample. The table is one full cycle plus a duplicate of sample
/// 0 at the end, so the linear interp at the wrap reads two adjacent real samples instead of wrapping
/// across the cycle boundary (which would be a step). kVoxSlots is kFlagshipSlots deliberately, so the
/// carrier and the flagship grid share a resolution and there is no resampling seam between them.
inline constexpr size_t kCarrierTable = kCarrierSlots + 1;

/// THE TABLE — a plain aggregate (NOT constexpr-constructible: std::sin isn't constexpr under libc++, so a
/// constexpr ctor is a macOS build breaker). Filled once by carrierTable(); carrierPosition() and
/// evalCarrier() read it.
struct CarrierTable {
	std::array<float, kCarrierTable> samples{};
};

/// THE TABLE itself, built once and shared by every voice. A function-local static rather than a
/// constexpr literal: std::sin is constexpr under libstdc++ (Linux) but NOT under libc++ (macOS Clang),
/// so a constexpr ctor that calls sin is a portability bug that builds at home and fails in CI. The Meyer
/// singleton sidesteps both — constructed on first use, thread-safe since C++11, one table per process.
/// Full cycle plus the guard sample; index with carrierPosition() and read with evalCarrier().
inline const CarrierTable& carrierTable() {
	static const CarrierTable table([] {
		CarrierTable t;
		for (size_t i = 0; i < kCarrierTable; ++i) {
			const double phase =
			    static_cast<double>(i) / static_cast<double>(kCarrierSlots) * 2.0 * std::numbers::pi;
			t.samples[i] = static_cast<float>(std::sin(phase));
		}
		return t;
	}());
	return table;
}

/// Phase (cycles, any range — wraps) to a fractional table position. Wrap by construction: one full cycle
/// maps to the table and the guard sample closes the last segment.
inline double carrierPosition(double phaseCycles) {
	const double p = phaseCycles - std::floor(phaseCycles);
	return p * static_cast<double>(kCarrierSlots);
}

/// Look up the carrier at `phaseCycles`, linearly interpolated. The seam-carrier idiom: wrap the phase,
/// scale to table space, read two adjacent samples (the guard sample rules out the wrap-across-the-end
/// case), lerp by the fraction. Cheap enough to run per sample downstream of the flagship grid read.
inline float evalCarrier(double phaseCycles) {
	const double pos = carrierPosition(phaseCycles);
	auto index = static_cast<size_t>(pos);
	index = std::min(index, static_cast<size_t>(kCarrierSlots - 1));
	const float frac = static_cast<float>(pos - static_cast<double>(index));
	const auto& samples = carrierTable().samples;
	const float a = samples[index];
	const float b = samples[index + 1];
	return a + (b - a) * frac;
}

} // namespace feathers::fm
