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

#include <array>
#include <cstddef>
#include <cstdint>

// **THE VOCODER'S BAND LAYOUT, ONCE, FOR BOTH VOCODERS** (Z1-Z7): where `count` bands sit between a Range's Low and High (the Spacing zone: Even, Speech, Low, High,
// Harmonic, Inharmonic, Free) and how wide each is (Bandwidth, and the Widths zone). Moved here unchanged from the Spectral
// effect's Stft, which calls it for its bins, so the engine's voices, which call it for their partials, lay the same bands.
// Everything is in log2 Hz. No allocation.
namespace feathers::spectral {

/// The most bands either vocoder lays (the Spectral effect's kMaxBands) and room for one more.
inline constexpr int kMaxVocodeBands = 129;

/// **FREE** (Z4): the coordinate's amounts of the named axes, each a phi triangle at its own rate (kFreeRates).
struct FreeAxes {
	double speech = 0.0;
	double low = 0.0;
	double high = 0.0;
	double harmonic = 0.0; ///< bipolar: + the harmonics, - the gaps
};
/// The axes' rates: 0.5 x phi^1..4, so the slowest peaks at 0.62 of the coordinate and no two move in step.
inline constexpr std::array<double, 4> kFreeRates { 0.80901699437494742, 1.3090169943749474, 2.1180339887498949, 3.4270509831248424 };
[[nodiscard]] FreeAxes freeAxes(double x) noexcept;

/// THE WIDTHS ZONE'S FREE AXES (Z5): a tilt and a harmonic bias, both bipolar.
struct WidthAxes {
	double tilt = 0.0;
	double harmonic = 0.0;
};
inline constexpr std::array<double, 2> kWidthRates { 1.0290855136357462, 1.6650873704278463 }; ///< 0.5 phi^1.5, 0.5 phi^2.5
[[nodiscard]] WidthAxes widthAxes(double x) noexcept;

/// WHAT A LAYOUT IS LAID FROM. `zone` 0 Even, 1 Speech, 2 Low, 3 High, 4 Harmonic, 5 Inharmonic, 6 Free; `zoneDrive` what it reads
/// (a named zone's fenced amount, Free's unbounded coordinate). `widthZone` 0 Even, 1 Narrow Low, 2 Narrow High, 3 Harmonic,
/// 4 Inharmonic, 5 Free, `widthDrive` likewise. `f0` the note the harmonic modes read.
struct BandSpec {
	int count = 24;
	double low = 20.0;
	double high = 20000.0;
	double bandwidth = 1.0;
	int zone = 0;
	double zoneDrive = 0.0;
	int widthZone = 0;
	double widthDrive = 0.0;
	double f0 = 261.6255653005986;
};

/// Whether `spec` lays its centres on the harmonics or the gaps (Harmonic, Inharmonic, or Free with a harmonic axis), and which.
[[nodiscard]] bool layoutHarmonic(const BandSpec& spec, bool* inharmonic) noexcept;

/// **HARMONIC** (Z3): the carrier's harmonics of f0 (or the gaps between them, inharmonic) in the Range are the MEMBERS; each band
/// covers a GROUP of neighbouring members (one each at the bottom, wider groups above), centred on its PICK (the loudest member, the
/// quietest gap, as the caller last saw the carrier: `pick`). Re-laid only when the count, the Range, the side or f0 (past a cent)
/// moves.
class BandGrouping {
public:
	/// Lays the groups; answers whether anything was re-laid.
	bool regroup(int count, double low, double high, bool inharmonic, double f0) noexcept;
	/// Member m's frequency, Hz.
	[[nodiscard]] double memberHz(int m) const noexcept {
		return (static_cast<double>(m + firstMember_) + (inharmonic_ ? 1.5 : 1.0)) * f0_;
	}
	/// Whether there are more members than bands (groups and picks), or every member is a band and the rest fill the other kind's places.
	[[nodiscard]] bool grouped() const noexcept { return grouped_; }
	[[nodiscard]] bool inharmonic() const noexcept { return inharmonic_; }
	[[nodiscard]] int members() const noexcept { return members_; }
	[[nodiscard]] int bands() const noexcept { return bands_; }
	[[nodiscard]] int firstMember() const noexcept { return firstMember_; }
	[[nodiscard]] double f0() const noexcept { return f0_; }
	[[nodiscard]] int first(int b) const noexcept { return first_[static_cast<size_t>(b)]; }
	[[nodiscard]] int last(int b) const noexcept { return last_[static_cast<size_t>(b)]; }
	[[nodiscard]] int pick(int b) const noexcept { return pick_[static_cast<size_t>(b)]; }
	/// Band b's centre on this grouping, log2 Hz (sorted, strictly rising).
	[[nodiscard]] double target(int b) const noexcept { return target_[static_cast<size_t>(b)]; }
	/// Moves band b's pick to member m (grouped only); answers whether it moved.
	bool setPick(int b, int m) noexcept;

private:
	std::array<int, kMaxVocodeBands> first_ {};
	std::array<int, kMaxVocodeBands> last_ {};
	std::array<int, kMaxVocodeBands> pick_ {};
	std::array<double, kMaxVocodeBands> target_ {};
	int members_ = 0;
	int firstMember_ = 0;
	int bands_ = 0;
	double f0_ = 0.0;
	double low_ = 0.0;
	double high_ = 0.0;
	bool inharmonic_ = false;
	bool grouped_ = false;
};

/// Band b's width factor under the Widths zone (Z5), a power of two around 1, for a band centred at `centreLog2`.
[[nodiscard]] double widthFactor(const BandSpec& spec, double centreLog2, int b, int count) noexcept;

/// **THE LAYOUT**: `spec.count` (clamped to 1..kMaxVocodeBands - 1) centres and widths, log2 Hz, into `centre` and `sigma`; `grouping`
/// is read in the harmonic modes (null: Even's centres there). Answers the count.
int layoutBands(const BandSpec& spec, const BandGrouping* grouping, double* centre, double* sigma) noexcept;

} // namespace feathers::spectral
