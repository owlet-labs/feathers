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
 */

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

// A FRACTIONAL DELAY LINE READ THROUGH A WINDOWED SINC — the comb's delay, in the Filter device and in
// the automodulator. Written for the 2026-09-24 licence migration from the textbook method, not from
// the GPL line it replaces: Julius O. Smith III, "Digital Audio Resampling Home Page" (bandlimited
// interpolation, ccrma.stanford.edu/~jos/resample) and "Physical Audio Signal Processing", §
// "Bandlimited Interpolation of Time-Limited Signals", for the windowed-sinc interpolator and its
// table of phases; the Blackman window is Blackman & Tukey (1958) as tabulated in Harris, "On the Use
// of Windows for Harmonic Analysis with the DFT" (Proc. IEEE, 1978).
//
// THE KERNEL. A read at a fractional delay d = i + f (i an integer, 0 <= f < 1) is a twelve-tap FIR
// over the samples written i − 5 .. i + 6 writes ago, and the tap on the sample `lag` writes ago is
//
//     h(t) = B · sinc(B · t) · w(t / 6),      t = lag − d,      sinc(x) = sin(πx) / (πx)
//     w(u) = 0.42 + 0.5·cos(πu) + 0.08·cos(2πu)   for |u| <= 1   (Blackman), 0 outside
//
// — an ideal lowpass of bandwidth B (cycles per sample × 2, i.e. its edge is at B/2 · sr), delayed by
// d and truncated to ±6 samples by the window.
//
// B = 0.455, AND THAT IS A PARITY DECISION RATHER THAN A DESIGN ONE. It was MEASURED from the line this
// replaces, as a black box — an impulse written in and read back at a set of fractional delays — and
// the old kernel's taps are this formula at B = 0.455 to five digits, which is a lowpass at the
// interpolator: −0.41 dB at 0.1·sr, −3.9 dB at 0.2·sr, −15 dB at 0.3·sr. Inside a comb's feedback loop
// that darkens every pass, and the comb's character has been voiced with it, so it is kept. A wider B
// (0.9, say) would be a brighter comb — a sound change for the ear, not for a licence migration.
// The response is NOT normalised to unity at DC (it is 0.9995), for the same reason: the loop gain
// at a given resonance is what it was.
//
// THE TABLE holds the kernel at kPhases + 1 evenly spaced fractions and the reader interpolates
// LINEARLY BETWEEN ROWS, so the taps move continuously with the delay rather than in 1/kPhases steps.
// `SincTable::kernel` is the one function that turns a fraction into taps, and both `read` and any
// closed-form evaluator of the line call it — so a drawn response and the sound cannot disagree about
// the taps.
//
// THE CONTRACT A CALLER DEPENDS ON: `read(d)` returns the signal as it was d writes ago, where the most
// recent write is ONE write ago, so a caller that reads before it writes gets a delay of d. The window
// reaches six samples to the NEWER side of d, so d must be at least kMinDelay (6) or the read would
// take samples that have not been written; and at most Size − kHalfTaps.

namespace feathers::dsp {

class SincTable {
public:
	static constexpr int kTaps = 12;
	static constexpr int kHalfTaps = kTaps / 2;
	static constexpr int kPhases = 256;
	static constexpr double kBandwidth = 0.455;

	/// THE TABLE AT `bandwidth` — the default is the comb's voiced 0.455 (see the file header), and every caller
	/// that names none builds exactly the table it always did. The flanger asks for 1.0: see `tapAt`.
	explicit SincTable(double bandwidth = kBandwidth) : bandwidth_(bandwidth) {
		for (int row = 0; row <= kPhases; ++row) {
			const double f = static_cast<double>(row) / static_cast<double>(kPhases);
			for (int j = 0; j < kTaps; ++j) {
				// Tap j sits on lag (i + 6 − j), so t = lag − (i + f) = 6 − j − f.
				const double t = static_cast<double>(kHalfTaps - j) - f;
				coeff_[static_cast<size_t>(row)][static_cast<size_t>(j)] = static_cast<float>(tapAt(t, bandwidth_));
			}
		}
		for (int row = 0; row < kPhases; ++row) {
			for (int j = 0; j < kTaps; ++j) {
				const auto r = static_cast<size_t>(row);
				const auto c = static_cast<size_t>(j);
				delta_[r][c] = coeff_[r + 1][c] - coeff_[r][c];
			}
		}
		delta_[static_cast<size_t>(kPhases)] = {};
	}

	/// THE KERNEL, in closed form, at any t — what the default table's rows are samples of.
	static double tapAt(double t) { return tapAt(t, kBandwidth); }

	/// THE KERNEL AT BANDWIDTH `b`. **At b = 1 the frac = 0 row is an EXACT delta** — every other integer t is a
	/// zero of sinc, and those are returned as 0.0 rather than as sin(πk)'s ~1e-16 residue, while t = 0 is
	/// 1 · 1 · w(0) = 1 — so an integer read is the sample itself, bit for bit. The price at FRACTIONAL
	/// delays: with the passband reaching Nyquist there is no transition band, so the twelve-tap window's
	/// truncation shows as ripple and images near the top of the band, and a half-sample read has its usual
	/// zero at Nyquist. For a swept flanger that is fine (the comb's own notches dwarf it); for a voiced comb
	/// in a feedback loop it is why the default stays 0.455. Only x = 0 can be an integer at b = 0.455 over
	/// this window (0.455·t reaches ±1 only off the table's dyadic grid), so the default table is unchanged.
	static double tapAt(double t, double b) {
		const double u = t / static_cast<double>(kHalfTaps);
		if (!(std::abs(u) < 1.0)) {
			return 0.0;
		}
		const double x = b * t;
		if (x != 0.0 && x == std::round(x)) {
			return 0.0; // a zero of sinc, exactly
		}
		const double sinc = (x == 0.0) ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
		const double window = 0.42 + 0.5 * std::cos(std::numbers::pi * u) + 0.08 * std::cos(2.0 * std::numbers::pi * u);
		return b * sinc * window;
	}

	[[nodiscard]] double bandwidth() const noexcept { return bandwidth_; }

	/// The twelve taps for a fraction in [0, 1): out[j] weights the sample (i + 6 − j) writes ago.
	void kernel(float frac, float* out) const {
		const float position = frac * static_cast<float>(kPhases);
		int row = static_cast<int>(position);
		row = row < 0 ? 0 : (row > kPhases - 1 ? kPhases - 1 : row);
		const float w = position - static_cast<float>(row);
		const auto& c = coeff_[static_cast<size_t>(row)];
		const auto& d = delta_[static_cast<size_t>(row)];
		for (int j = 0; j < kTaps; ++j) {
			out[j] = c[static_cast<size_t>(j)] + w * d[static_cast<size_t>(j)];
		}
	}

private:
	double bandwidth_ = kBandwidth;
	std::array<std::array<float, kTaps>, kPhases + 1> coeff_ {};
	std::array<std::array<float, kTaps>, kPhases + 1> delta_ {};
};

/// A delay line of `Size` samples (a power of two) read through a `SincTable`. Holds the table by
/// reference, so the table must outlive it — the users hold one namespace-scope table for everybody.
///
/// NO ALLOCATION: the buffer is a fixed array inside the object. It is `Size + kTaps` long because each
/// of the first kTaps slots is MIRRORED past the end on write, so a twelve-tap read is always one
/// contiguous run and never wraps.
template <int Size>
class SincDelayLine {
	static_assert(Size > 0 && (Size & (Size - 1)) == 0, "the line length must be a power of two");
	static_assert(Size > SincTable::kTaps, "the line must be longer than the interpolator");

public:
	static constexpr int kTaps = SincTable::kTaps;
	/// The shortest delay a read may ask for — see the file header.
	static constexpr int kMinDelay = SincTable::kHalfTaps;

	explicit SincDelayLine(const SincTable& table) : table_(&table) {}

	void clear() {
		buffer_.fill(0.0f);
		writePos_ = 0;
		quietRun_ = Size;
	}

	void write(float x) {
		buffer_[static_cast<size_t>(writePos_)] = x;
		if (writePos_ < kTaps) {
			buffer_[static_cast<size_t>(writePos_ + Size)] = x;
		}
		writePos_ = (writePos_ + 1) & (Size - 1);
		quietRun_ = (x == 0.0f) ? (quietRun_ < Size ? quietRun_ + 1 : Size) : 0;
	}

	/// **WHETHER ANY NON-ZERO SAMPLE IS STILL IN THE LINE** (backlog MC) — false once the last `Size` writes were all
	/// exactly zero, which is every cell, mirror included. A read of a line that holds something can still be exactly
	/// zero (twelve zero cells around the head), which is why a host asking "is this device done" needs this and
	/// not the output.
	[[nodiscard]] bool holdsSound() const noexcept { return quietRun_ < Size; }

	/// The signal `delay` writes ago, sinc-interpolated. kMinDelay <= delay <= Size − kMinDelay.
	float read(float delay) const {
		const int whole = static_cast<int>(delay);
		const float frac = delay - static_cast<float>(whole);
		float taps[kTaps];
		table_->kernel(frac, taps);
		// Tap 0 is the OLDEST sample in the window, (whole + 6) writes ago; the most recent write is at
		// writePos_ − 1, so a sample `lag` writes ago is at writePos_ − lag.
		const int start = (writePos_ - whole - kMinDelay) & (Size - 1);
		const float* x = buffer_.data() + start;
		float sum = 0.0f;
		for (int j = 0; j < kTaps; ++j) {
			sum += x[j] * taps[j];
		}
		return sum;
	}

private:
	const SincTable* table_;
	std::array<float, static_cast<size_t>(Size + kTaps)> buffer_ {};
	int writePos_ = 0;
	/// Consecutive exactly-zero writes, saturating at `Size`; a fresh line is all zero.
	int quietRun_ = Size;
};

} // namespace feathers::dsp
