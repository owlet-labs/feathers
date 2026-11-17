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

// A LINEAR-PHASE CROSSOVER: one engine for OWLTT's three bands and the Rack's Bands mode.
//
// THE SPLIT. Lowpass prototypes LP_k are symmetric windowed-sinc FIRs, all centred on one tap and each normalised to
// a DC gain of exactly 1. The bands are LP_0, LP_1 - LP_0, ..., and the top band is the input delayed by the kernel's
// centre minus LP_last. They TELESCOPE: the bands sum to the input delayed by `latency`, a pure delay, whatever the
// kernels hold and wherever the corners sit. The sum is exact by construction (the same convolved LP is subtracted
// from one band and added to the next), not by the kernels agreeing with each other.
//
// THE CONVOLUTION is the EQ's linear-phase road at a fraction of the length: uniform partitioned overlap-save on
// pffft, blocks of B = N/16, one frequency-domain delay line per input channel, one kernel per output. The EQ's
// `LinearPhase` serves two fixed paths from a magnitude curve at N = 16384 (0.19 s); this one serves up to
// `kMaxOutputs` outputs of one stereo input from corners, at N = 2048 (24 ms at 48 kHz), so it is its own class over
// the same partition / zconvolve / overlap-save recipe rather than a parameterisation of that one.
//
// LATENCY is `N/2 + B` samples and does not depend on the corners: a moving corner never moves it, so it can be
// reported to a host once and never restart one. `linearCrossoverLatency` is the one statement of it.
//
// MOVING CORNERS. `setTargets` is called at control rate and is cheap when nothing moved. When a corner has moved by
// more than `kRebuildTolerance` (relative) the changed kernels are rebuilt, ONE KERNEL A CONTROL-GRID STEP (every 32
// samples, counted inside `process`), into the spare of two kernel sets, on the audio thread, allocation-free. When
// the build is complete the next block edge convolves the block with BOTH sets and crossfades linearly across it.
// Two linear-phase kernels with one centre crossfade to a linear-phase result, and the telescoping sum is untouched
// (the delayed input does not move), so the swap cannot comb or break the sum. A corner that moves faster than a
// rebuild takes is followed in steps: the target taken is the one at the time the build STARTS, and a request made
// while a build is in flight is dropped (the caller asks again at its next control step). Every buffer is allocated
// in `prepare`.
//
// RESOLUTION. A windowed-sinc lowpass cannot be steeper than its window allows: the Kaiser window (`kKaiserBeta`)
// puts the transition between -6 dB and the stopband about `transitionHz` wide (about 100 Hz at 48 kHz), and a
// corner below about twice that is a gentle roll rather than a wall. The stopband is `kStopbandDb` below the
// passband, measured in the tests.
//
// THE TAIL. A zero input leaves the FIR output exactly zero once the delay line has shifted its last non-zero block
// out (`quiet()`): partitions of zero spectra accumulate exactly 0.0f, so a rack or device that waits for `quiet()`
// can sleep on an exactly silent block.

#include <array>
#include <cstddef>
#include <cstdint>

struct PFFFT_Setup;

namespace feathers::dsp {

/// THE KERNEL LENGTH (taps) at a rate: 2048 up to 50 kHz, 4096 to 100 kHz, 8192 above, so the resolution in Hz and the
/// latency in milliseconds hold across rates.
[[nodiscard]] constexpr size_t linearCrossoverTaps(double sampleRate) noexcept {
	return sampleRate > 100000.0 ? 8192 : sampleRate > 50000.0 ? 4096 : 2048;
}
/// THE CONVOLUTION BLOCK, N/16.
[[nodiscard]] constexpr size_t linearCrossoverBlock(double sampleRate) noexcept {
	return linearCrossoverTaps(sampleRate) / 16;
}
/// THE LATENCY IN SAMPLES: the kernel's centre plus the block's wait. 1152 at 44.1 and 48 kHz (24 ms).
[[nodiscard]] constexpr uint32_t linearCrossoverLatency(double sampleRate) noexcept {
	return static_cast<uint32_t>(linearCrossoverTaps(sampleRate) / 2 + linearCrossoverBlock(sampleRate));
}

/// THE KAISER WINDOW'S SHAPE, and what it buys: a stopband about 70 dB down, a transition about 3 bins' width.
inline constexpr double kLinearKaiserBeta = 7.0;
/// THE HALF BANDS' WINDOW is much lighter: their spectrum is smooth, so truncation leaves nothing to suppress, and a heavy window
/// would round the crossing of two halves (a 0.22 dB dip at beta 7, 0.04 dB at 2).
inline constexpr double kLinearHalfKaiserBeta = 2.0;
/// A CORNER REBUILDS WHEN IT MOVES BY MORE THAN THIS, RELATIVELY (1.7 cents): below it a kernel is not worth a build.
inline constexpr double kLinearRebuildTolerance = 0.001;
/// "TO NYQUIST": a band's `hiHz` at or above this is the open top (the delta tap minus the lowpass below).
inline constexpr double kLinearOpenTopHz = 1.0e9;
/// How many steepnesses the kernels are windowed for.
inline constexpr size_t kLinearSteepSteps = 3;
/// THE HALF BANDS' RAISED-COSINE TRANSITION, in kernel bins (8 bins is 188 Hz at 48 kHz): wide enough that the kernel's window barely smooths it. The window still rounds the crossing, so
/// two halves in series sum to within 0.1 dB of flat (about -0.05 dB at a corner, measured), not exactly flat as the full bands do.
inline constexpr double kLinearHalfTransitionBins = 8.0;

/// WHAT ONE OUTPUT'S KERNEL IS: the band between two corners, `loHz` at or below 0 meaning "from DC" and `hiHz` at or
/// above `kLinearOpenTopHz` meaning "to Nyquist". {0, f} is a lowpass, {f, open} a highpass, {a, b} a bandpass.
struct LinearBand {
	double loHz = 0.0;
	double hiHz = kLinearOpenTopHz;
	/// THE SQUARE ROOT OF THE BAND: a zero-phase kernel whose magnitude is the square root of this band's, so two of them in
	/// series (one before a chain, one after it) make the band, and the squares of every half of a split sum to the band sum.
	/// Built in the frequency domain from the band's own kernel, windowed again, so the pair is the band to a fraction of a dB.
	bool half = false;
	/// THE STEEPNESS: 0 is the full kernel, 1 and 2 are a half and a quarter of its reach (zero-padded, same centre, same delay), so the
	/// transition is two and four times wider (-0.1 dB to -60 dB in about 94, 186 and 368 Hz at 48 kHz) and the latency is the same for all three.
	uint8_t steep = 0;
	friend bool operator==(const LinearBand&, const LinearBand&) = default;
};

/// ONE STEREO INPUT, UP TO `kMaxOutputs` LINEAR-PHASE FILTERED OUTPUTS, each `latency()` samples late.
class LinearFirBank {
public:
	static constexpr size_t kMaxOutputs = 8;
	static constexpr size_t kGridFrames = 32; // the control grid a build is sliced on

	/// One sample's result: every output's two channels and the input delayed by exactly `latency()`.
	struct Frame {
		std::array<std::array<float, 2>, kMaxOutputs> out {};
		std::array<float, 2> delayed {};
	};

	LinearFirBank() = default;
	LinearFirBank(const LinearFirBank&) = delete;
	LinearFirBank& operator=(const LinearFirBank&) = delete;
	~LinearFirBank() { release(); }

	/// Allocate everything for `outputs` outputs at `sampleRate`. Not for the audio thread; the only allocation.
	void prepare(double sampleRate, size_t outputs);
	/// Forget the audio (delay lines, the block in progress, the delayed input), keep the kernels and the targets.
	void clearAudio() noexcept;
	/// Build every output's kernel from `bands` NOW, whole, cancelling any build in flight. For a start or a reset.
	void buildNow(const LinearBand* bands) noexcept;
	/// The control-rate call: begin rebuilding the outputs whose band moved beyond the tolerance. Dropped while a
	/// build is in flight or waiting to swap in. `bands` has `outputs()` entries.
	void setTargets(const LinearBand* bands) noexcept;
	/// ONE SAMPLE IN; the outputs and the delayed input, each `latency()` late. Runs the control grid itself.
	void process(float left, float right, Frame& frame) noexcept {
		inputs_[0][pos_] = left;
		inputs_[1][pos_] = right;
		for (size_t o = 0; o < outputs_; ++o) {
			frame.out[o][0] = out_[o][0][pos_];
			frame.out[o][1] = out_[o][1][pos_];
		}
		frame.delayed[0] = delay_[0][delayPos_];
		frame.delayed[1] = delay_[1][delayPos_];
		delay_[0][delayPos_] = left;
		delay_[1][delayPos_] = right;
		if (++delayPos_ == delayLen_) {
			delayPos_ = 0;
		}
		if (++grid_ == kGridFrames) {
			grid_ = 0;
			step();
		}
		if (++pos_ == block_) {
			pos_ = 0;
			blockEdge();
		}
	}

	/// True once the input has been exactly zero for longer than the longest thing the bank can still emit, so every
	/// output and the delayed input are exactly zero from here until a non-zero input.
	[[nodiscard]] bool quiet() const noexcept { return zeroBlocks_ > parts_ + 2; }

	[[nodiscard]] size_t outputs() const noexcept { return outputs_; }
	[[nodiscard]] size_t taps() const noexcept { return n_; }
	[[nodiscard]] size_t blockLength() const noexcept { return block_; }
	[[nodiscard]] uint32_t latency() const noexcept { return static_cast<uint32_t>(delayLen_); }
	[[nodiscard]] bool busy() const noexcept { return stage_ != Stage::Idle; }
	[[nodiscard]] int buildsForTest() const noexcept { return builds_; }
	[[nodiscard]] int swapsForTest() const noexcept { return swaps_; }
	/// The corner each output's LIVE kernel was designed at (what the audio is being filtered with right now).
	[[nodiscard]] LinearBand liveBandForTest(size_t o) const noexcept { return live_band_[o]; }

private:
	enum class Stage { Idle, Building, Ready };

	void release() noexcept;
	void step() noexcept;
	void blockEdge() noexcept;
	/// Output `o`'s taps from `band`, into `fir_`, then partitioned into kernel set `set`.
	void design(size_t o, const LinearBand& band, size_t set) noexcept;
	/// The raised-cosine half-band kernel of `band`, into `fir_` (see `LinearBand::half`).
	void designHalf(const LinearBand& band) noexcept;
	/// The half-kernel of a lowpass at `hz` (centre first), unit DC gain, into `half`.
	void lowpassHalf(double hz, double* half, size_t steep) const noexcept;

	double sampleRate_ = 48000.0;
	size_t outputs_ = 0, n_ = 0, block_ = 1, parts_ = 0, delayLen_ = 1;
	PFFFT_Setup* small_ = nullptr; // 2B
	PFFFT_Setup* big_ = nullptr;   // N: the half-band design's transforms
	// Kernels: [set][output] of `parts_` spectra of 2B, pffft's internal order.
	std::array<std::array<float*, kMaxOutputs>, 2> kernel_ {};
	size_t live_ = 0;
	std::array<LinearBand, kMaxOutputs> live_band_ {}, target_ {};
	std::array<bool, kMaxOutputs> changed_ {};
	// Audio: the input block in progress and the last, the delay lines, the accumulators, the outputs.
	std::array<float*, 2> inputs_ {}, previous_ {}, fdl_ {}, accum_ {}, fadeAccum_ {}, delay_ {};
	std::array<std::array<float*, 2>, kMaxOutputs> out_ {};
	float* window2B_ = nullptr;
	float* work2B_ = nullptr;
	double* half_ = nullptr;   // N/2 + 1: scratch for one lowpass's half kernel
	double* halfLo_ = nullptr; // and the second
	double* kaiser_ = nullptr; // N/2 + 1: the window, centre first
	float* fir_ = nullptr;     // N: the taps being partitioned
	float* spec_ = nullptr;    // N: the half-band design's spectrum
	float* specWork_ = nullptr;
	float* windowN_ = nullptr; // N: the half bands' (lighter) Kaiser window over all N taps, centre N/2
	size_t pos_ = 0, head_ = 0, delayPos_ = 0, grid_ = 0;
	Stage stage_ = Stage::Idle;
	size_t next_ = 0;
	uint32_t zeroBlocks_ = 0;
	int builds_ = 0, swaps_ = 0;
};

} // namespace feathers::dsp
