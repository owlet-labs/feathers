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

#include "fx/dyn/biquad.h"

#include <array>
#include <cstddef>
#include <cstdint>

// BUS UTILITY'S METER — loudness, peak, correlation, and the over.
//
// **IT IS A TAP AND NOT A MASTER METER, AND SO IN THOSE WORDS.** A meter on the actual master may
// come later; this is intended as a tap, so "master" terminology is loose. So this measures
// WHEREVER THE DEVICE SITS in the chain. It is a
// master meter exactly when Bus Utility is last, and a probe everywhere else — which is the useful
// thing, because a probe you can place is what tells you where a chain got loud. A meter on the
// plugin's real output is a separate instrument and stays on the backlog.
//
// **MEASURED ON THE OUTPUT, NOT THE INPUT.** The device's whole job is setting a level, so the
// number a hand needs is what is LEAVING it. A meter ahead of the gain would move when nothing about
// the mix had.
//
// EVERYTHING HERE IS FIXED-SIZE AND NOTHING ALLOCATES, prepare included. The windows are ring
// buffers of BLOCK sums rather than of samples — 30 floats where three seconds of stereo audio at
// 96 kHz would have been 2.3 MB — and the integrated measurement's gate runs off a histogram of
// 751 bins rather than a growing list of blocks. That is the standard EBU implementation and it is
// also the only shape that fits "no allocation after activate" without an arbitrary cap on how long
// a measurement may run.

namespace feathers::fx::busutil {

/// **WHAT THIS DEVICE PUBLISHES THROUGH `readingAt`, IN INDEX ORDER.**
///
/// `device_host.h` carries readings as ANONYMOUS FLOATS on purpose — *"the host carries numbers it
/// does not understand and the DEVICE'S OWN UI knows what the indices mean"* — so this enum IS the
/// contract, and it is shared by the publisher and the card rather than being two lists that agree
/// by habit. Automod's readings road records what the alternative costs: a first version returned
/// the phase for ANY index, and a second declared reading silently answered the first one's value.
enum class Reading : size_t {
	/// LINEAR sample peak per channel, decaying — not dB. The card converts; a reading is a number
	/// and the unit it is DRAWN in is a drawing decision.
	PeakL,
	PeakR,
	/// LUFS: 400 ms, 3 s, and the gated whole-measurement figure.
	Momentary,
	ShortTerm,
	Integrated,
	/// -1..1. +1 is mono-correlated, 0 is uncorrelated, -1 is one channel against the other.
	Correlation,
	/// **SECONDS SINCE THE MOST RECENT SAMPLE AT OR OVER 0 dBFS**, clamped to `kClipAgeMax`.
	///
	/// **ONE NUMBER, AND BOTH OF THE LED'S TIME CONSTANTS COME OUT OF IT IN THE DRAWING.** The spec is
	/// a prominent 0 dBFS peak LED with an instantaneous core and a decaying
	/// glow. Publishing an AGE rather than a lit/unlit flag is what
	/// makes that possible without the audio thread knowing anything about frames: the card lights
	/// the core while the age is under its own threshold and fades the glow by its own curve, and
	/// **no over can be missed however slowly the UI repaints**, because a single-sample over sets
	/// the age to zero and the age only ever grows from there. A boolean would have to guess how long
	/// to stay true, and a boolean read at 60 fps misses a one-sample flash outright.
	ClipAge,

	/// **THE TRUE SAMPLE PEAK OF THE LAST SHORT WINDOW, LINEAR AND WITH NO BALLISTICS** — an
	/// instantaneous raw amplitude readout.
	///
	/// NOT `PeakL`/`PeakR`, which FALL at 20 dB per second so a bar can be read as it moves. This one
	/// neither falls nor smooths: it is the largest `|sample|` seen, of either channel, since it was
	/// last latched.
	///
	/// **"INSTANTANEOUS" HAS TO MEAN A WINDOW, AND SAYING WHICH ONE IS THE HONEST PART.** A value
	/// published per BLOCK is overwritten several times between repaints, so the number a surface
	/// draws would be whichever block happened to land last — and a transient in any other block
	/// would simply never be seen, while the bar beside it and the LED both caught it. So the peak is
	/// HELD for `kRawPeakHoldSeconds` before it can fall, which is longer than a frame at any rate a
	/// host will run: no smoothing, no decay, and nothing missed between two draws.
	RawPeak,

	kCount,
};

/// WHAT A LOUDNESS READING SAYS WHEN THERE IS NOTHING TO SAY.
///
/// **NOT ZERO, BECAUSE 0 LUFS IS A REAL AND VERY LOUD VALUE.** Silence has no loudness in dB — the
/// true answer is -infinity — so a floor is needed and it has to sit below anything a signal can
/// produce. A meter that answered 0 for silence would draw full scale on an empty chain.
inline constexpr float kSilenceLufs = -200.0f;

/// **HOW LONG THE RAW PEAK IS HELD BEFORE IT MAY FALL.** 50 ms — three frames at 60 fps, so a
/// surface cannot miss a transient between two draws, and short enough that the number still reads as
/// live rather than as a maximum. See `Reading::RawPeak`.
inline constexpr double kRawPeakHoldSeconds = 0.05;

/// THE CEILING ON `Reading::ClipAge`. Ten seconds is far past any glow the card will draw, so the
/// clamp costs nothing visible and keeps the number from growing without bound across a long idle.
inline constexpr float kClipAgeMax = 10.0f;

/// THE ABSOLUTE GATE, BS.1770-4. Blocks quieter than this never enter the integrated measurement.
inline constexpr double kAbsoluteGateLufs = -70.0;

/// THE RELATIVE GATE, in LU below the ungated mean of the blocks that passed the absolute gate.
inline constexpr double kRelativeGateLu = -10.0;

/// ONE BIQUAD, TRANSPOSED DIRECT FORM II — two state words, and the form that behaves best in float
/// at the very low corner stage 2 uses. The dynamics devices' own (`fx/dyn/biquad.h`), since backlog
/// JJ: this file used to carry an identical copy, and two copies of one filter body drift.
using Biquad = dyn::Biquad;

/// **THE K-WEIGHTING PAIR, DESIGNED AT THE HOST'S RATE RATHER THAN COPIED FROM THE STANDARD'S
/// TABLE.** BS.1770-4 publishes its coefficients at 48 kHz only; a build that pasted those numbers
/// would weight the wrong frequencies at 44.1 or 96, which is the same class of error as a filter
/// that ignores its sample rate. Both stages are RBJ designs from the standard's own analogue
/// parameters, and `test_fx_busutil_meter.cpp` pins them by rebuilding at 48 kHz and comparing to the
/// published table — so the design is checked against the SPEC and not against itself.
struct KWeighting {
	Biquad shelf;   ///< stage 1: +4 dB high shelf at 1681.97 Hz
	Biquad highpass; ///< stage 2: RLB highpass at 38.14 Hz

	void design(double sampleRate) noexcept;
	void clear() noexcept {
		shelf.clear();
		highpass.clear();
	}
	[[nodiscard]] double step(double x) noexcept { return highpass.step(shelf.step(x)); }
};

/// THE MEASUREMENT. One per device instance; fed the device's OUTPUT.
class Meter {
public:
	void prepare(double sampleRate) noexcept;
	/// "AS IF JUST SWITCHED ON" — every window, the filters, and the integrated measurement.
	void reset() noexcept;
	/// **THE HAND'S GESTURE, AND ONLY THE INTEGRATED READING MOVES.** Momentary and short-term are
	/// windows over the recent past and have nothing to reset; the integrated figure is a
	/// measurement with a START, and a hand that has changed the mix wants a new one.
	void resetIntegrated() noexcept;

	void process(const float* left, const float* right, int frames) noexcept;

	[[nodiscard]] float reading(Reading r) const noexcept;
	/// TEST-ONLY: the integrated reading recomputed from the histogram (what every `reading` used to do), and how many
	/// times the histogram has been scanned since construction.
	[[nodiscard]] float integratedRecomputedForTest() const noexcept { return computeIntegrated(); }
	[[nodiscard]] size_t integratedScansForTest() const noexcept { return integratedScans_; }

private:
	void closeBlock() noexcept;

	/// 100 ms — the standard's gating-block STEP, and the granularity every window here is built from.
	/// Momentary is four of these, short-term is thirty, and a gating block is the last four, which is
	/// exactly the 400 ms block at 75% overlap BS.1770-4 asks for. One accumulator serves all three.
	static constexpr size_t kBlocksShort = 30; ///< 3 s
	static constexpr size_t kBlocksMomentary = 4; ///< 400 ms
	/// 0.1 LU bins from the absolute gate up to +5 LUFS — the EBU's own resolution.
	///
	/// **IT COSTS THE INTEGRATED FIGURE UP TO 0.05 LU AGAINST THE UNGATED WINDOWS, AND THIS COMMENT
	/// SAID "UNDER 0.01" UNTIL A TEST MEASURED IT.** A block lands in the nearest bin, so half a bin
	/// is the worst case, and a steady tone — every block in one bin — sits at exactly that worst
	/// case: 0.047 LU, reported by `test_fx_busutil_meter.cpp` the first time it compared integrated
	/// against momentary at a tolerance tighter than the quantisation. The number is fine and the
	/// claim was not; a tenth of a LU is inside anybody's reading of a meter, and it is the
	/// resolution the standard's own implementations use.
	static constexpr size_t kHistogramBins = 751;

	double sampleRate_ = 48000.0;
	int blockSamples_ = 4800;
	int blockFill_ = 0;

	KWeighting kwLeft_ {};
	KWeighting kwRight_ {};

	/// Sums for the block being filled.
	double sumSqL_ = 0.0;
	double sumSqR_ = 0.0;
	double sumLR_ = 0.0;
	double sumRawSqL_ = 0.0;
	double sumRawSqR_ = 0.0;

	/// The closed blocks' mean squares, newest at `blockHead_ - 1`.
	std::array<double, kBlocksShort> msL_ {};
	std::array<double, kBlocksShort> msR_ {};
	std::array<double, kBlocksShort> corrNum_ {};
	std::array<double, kBlocksShort> corrDenL_ {};
	std::array<double, kBlocksShort> corrDenR_ {};
	size_t blockHead_ = 0;
	size_t blocksSeen_ = 0;

	/// The integrated measurement's gate.
	std::array<uint32_t, kHistogramBins> histogram_ {};
	double gatedSumEnergy_ = 0.0; ///< blocks above the ABSOLUTE gate
	uint64_t gatedCount_ = 0;
	/// THE INTEGRATED READING, made when a block closes (the only time the histogram moves) and when it is reset.
	float integrated_ = kSilenceLufs;
	size_t integratedScans_ = 0;
	[[nodiscard]] float computeIntegrated() const noexcept;

	float peakL_ = 0.0f;
	float peakR_ = 0.0f;
	/// THE RAW PEAK'S TWO HALVES: what is being accumulated now, and what is being SHOWN. The shown
	/// value cannot fall until the window closes, which is what stops a transient vanishing between
	/// two repaints.
	float rawPeakAccum_ = 0.0f;
	float rawPeakHeld_ = 0.0f;
	int rawPeakSamples_ = 0;
	int rawPeakWindow_ = 2400;
	float peakDecay_ = 0.0f; ///< per-sample multiplier, from a fall time in dB/s

	float clipAge_ = kClipAgeMax;
};

} // namespace feathers::fx::busutil
