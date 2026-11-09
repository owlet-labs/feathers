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

#include <array>
#include <cstddef>
#include <cstdint>

struct PFFFT_Setup;

namespace feathers::fx::conv {

//--------------------------------------------------------------------------------------------------//
// THE TELESCOPE: a zero-latency NON-UNIFORMLY partitioned convolver (add the block telescope).  //
//--------------------------------------------------------------------------------------------------//
//
// One uniform partition size is expensive for a long IR: every 128 samples, every partition of the whole
// IR is multiplied in — 2250 of them for 6 s at 48 kHz, which MEASURED 11-14% of a core for the generated
// 2.5 s room alone. The telescope keeps the small blocks where latency needs them and grows them along the IR:
//
//     taps [0, 128)          HEAD: direct form, per sample — the IR's first sample answers the first input
//     taps [128, 2048)       STAGE 0: blocks of 128 (FFT 256), computed at each block's edge (lag 1)
//     taps [2048, 16384)     STAGE 1: blocks of 1024 (FFT 2048), starting TWO of its blocks in (lag 2)
//     taps [16384, end)      STAGE 2: blocks of 8192 (FFT 16384), lag 2
//
// **A STAGE THAT STARTS TWO OF ITS BLOCKS INTO THE IR HAS A WHOLE BLOCK OF SLACK.** Its output block m needs
// the input only up to block m-2, so the work for block m is done DURING block m-1 — spread across it one
// 128-sample step at a time rather than all at the edge, so the cost does not arrive as one spike. Only its
// FFTs happen at its edges. Still zero latency, and still exactly convolution (asserted against the direct
// sum).
struct StageSpec {
	size_t block;  // B: the partition, and the stage's block (FFT 2B)
	size_t offset; // the first tap this stage covers: lag * B
	size_t lag;    // 1: computed at the edge; 2: spread across the block before
};
inline constexpr size_t kHeadTaps = 128;

/// **LITE: THE LATE TAIL AT HALF RATE** (it was picked "Cheaper tail"). Stage 1 takes one more partition, to
/// `kLiteSplit`; from there the IR runs on a half-rate path — the input decimated by a halfband filter, convolved
/// in blocks of `kHalfBlock` half-rate samples (FFT 8192, half stage 2's), and interpolated back by the same
/// filter. The two filters delay by `kHalfbandDelay` each, so the half-rate kernel is built ADVANCED by twice that
/// and the tail lands exactly where it would at full rate. What it costs: the tail above ~a quarter of the rate.
inline constexpr size_t kLiteSplit = 17408;
inline constexpr size_t kHalfbandTaps = 47;
inline constexpr size_t kHalfbandDelay = (kHalfbandTaps - 1) / 2;
inline constexpr size_t kHalfBlock = 4096;
static_assert(kLiteSplit >= 16384 + 2 * kHalfbandDelay, "the half-rate kernel is advanced by both filters' delay");
/// The halfband lowpass (a quarter of the rate), DC gain 1: every other tap is zero but the centre.
[[nodiscard]] const std::array<float, kHalfbandTaps>& halfband() noexcept;
inline constexpr std::array<StageSpec, 3> kStages { {
	{ 128, 128, 1 },
	{ 1024, 2048, 2 },
	{ 8192, 16384, 2 },
} };

/// **AN IMPULSE RESPONSE, PARTITIONED AND TRANSFORMED: WHAT A `Convolver` RUNS ON.** Built whole on the
/// message thread (it allocates, and it runs an FFT per partition), then handed to a running convolver by
/// pointer (`Convolver::setKernel`), so a new IR reaches the audio thread with nothing to compute there.
/// IMMUTABLE once built. It must outlive every convolver running it; its owner frees it on the message thread.
class ConvKernel {
public:
	ConvKernel() = default;
	~ConvKernel();
	ConvKernel(const ConvKernel&) = delete;
	ConvKernel& operator=(const ConvKernel&) = delete;

	/// PARTITION two channels of `frames` samples along the telescope — its late tail at half rate when `lite`.
	/// Message thread.
	void build(const float* left, const float* right, size_t frames, bool lite = false);
	[[nodiscard]] bool lite() const noexcept { return lite_; }

	[[nodiscard]] size_t irFrames() const noexcept { return irFrames_; }
	/// Partitions over every stage (the head not counted).
	[[nodiscard]] size_t partitions() const noexcept;
	[[nodiscard]] size_t stagePartitions(size_t stage) const noexcept { return stagePartitions_[stage]; }
	/// **ITS LOUDEST FREQUENCY'S GAIN** — max |H(f)| of the mid, `(L + R) / 2`, measured at build over a
	/// transform as long as the IR. What a feedback loop around this kernel must stay under to decay.
	[[nodiscard]] float peakGain() const noexcept { return peakGain_; }
	/// ITS AVERAGE GAIN: the RMS of |H(f)| over frequency for the mid — by Parseval, the square root of the mid's
	/// energy. What a feedback loop around this kernel does to a broadband signal, per lap.
	[[nodiscard]] float rmsGain() const noexcept { return rmsGain_; }

private:
	friend class Convolver;
	void release() noexcept;
	size_t irFrames_ = 0;
	float peakGain_ = 0.0f;
	float rmsGain_ = 0.0f;
	bool lite_ = false;
	float* head_[2] = { nullptr, nullptr }; // taps [0, 128), REVERSED
	std::array<size_t, kStages.size()> stagePartitions_ {};
	/// Per stage, per channel: its partitions' spectra, 2B floats each.
	std::array<std::array<float*, 2>, kStages.size()> spectra_ {};
};

/// **THE CONVOLVER: ONE MONO INPUT, A STEREO IR, ZERO LATENCY.** Backlog JD. The IR is a `ConvKernel` it
/// does not own (`setKernel`), except the one `setIr` builds. **ALL ALLOCATION IS IN `prepare`** (the
/// capacity: the longest IR it will ever run) **and `setIr`**; `process` and `setKernel` allocate nothing.
class Convolver {
public:
	/// The base block: the head's length, and the step every stage advances by.
	static constexpr size_t kBlock = 128;

	Convolver() = default;
	~Convolver();
	Convolver(const Convolver&) = delete;
	Convolver& operator=(const Convolver&) = delete;

	/// ALLOCATE for IRs up to `maxIrFrames` long. Message thread. Forgets the kernel.
	void prepare(size_t maxIrFrames);
	[[nodiscard]] size_t capacityFrames() const noexcept { return capacityFrames_; }

	/// THE IR, BUILT HERE into a kernel this convolver owns. Message thread, while `process` is not running.
	void setIr(const float* left, const float* right, size_t frames);

	/// **RUN THIS KERNEL** — one pointer, and every stage's history cleared. AUDIO THREAD SAFE: allocates
	/// nothing. False (the old kernel kept) for a kernel longer than the capacity. Null runs nothing.
	bool setKernel(const ConvKernel* kernel) noexcept;
	[[nodiscard]] const ConvKernel* kernel() const noexcept { return kernel_; }

	/// FORGET THE INPUT HISTORY (the kernel is kept).
	void reset() noexcept;

	/// ADDS the convolution of `in` into `outL`/`outR`. Audio thread; any frame count; allocates nothing.
	void process(const float* in, float* outL, float* outR, size_t frames) noexcept;

	[[nodiscard]] size_t irFrames() const noexcept { return kernel_ != nullptr ? kernel_->irFrames() : 0; }
	[[nodiscard]] size_t partitions() const noexcept { return kernel_ != nullptr ? kernel_->partitions() : 0; }

	/// TEST SEAM: how many STAGE-0 forward FFTs `process` has run since the kernel was set or the convolver
	/// reset — one per 128 frames while it runs — so "Off must SKIP the engine" is a claim about this count.
	[[nodiscard]] uint64_t forwardFftsForTest() const noexcept { return ffts_; }
	/// TEST SEAM: WORK DONE, IN UNITS THAT DO NOT DEPEND ON THE HOST'S LOAD — n·log2(n) per FFT of size n, n per
	/// spectral multiply-accumulate of size n, 1 per head tap. A claim about SPIKES is a claim about this per call.
	[[nodiscard]] uint64_t workForTest() const noexcept { return work_; }

private:
	struct Stage {
		PFFFT_Setup* setup = nullptr;
		PFFFT_Setup* setupHalf = nullptr; // stage 2 under Lite: FFT 2 x kHalfBlock
		size_t capacity = 0;        // partitions the FDL holds
		float* fdl = nullptr;       // capacity spectra of 2B
		size_t fdlHead = 0;         // the newest spectrum's slot
		std::array<float*, 2> accum { nullptr, nullptr }; // 2B each
		std::array<float*, 2> out { nullptr, nullptr };   // B each: the block being played
		std::array<float*, 2> ready { nullptr, nullptr }; // B each, lag 2: the next block, finished early
		float* work = nullptr;      // 2B
		float* window = nullptr;    // 2B
		size_t next = 0;            // lag 2: the next partition to accumulate
	};

	void release() noexcept;
	void endOfBaseBlock() noexcept;
	void stageEdge(size_t s) noexcept;
	void stageStep(size_t s) noexcept;
	/// The last `n` inputs, oldest first, into `dst`.
	void recentInput(float* dst, size_t n) const noexcept;
	void recentHalf(float* dst, size_t n) const noexcept;
	/// The block a stage runs at under this kernel: stage 2 under Lite runs `kHalfBlock` half-rate samples.
	[[nodiscard]] bool halfStage(size_t s) const noexcept { return s == 2 && kernel_ != nullptr && kernel_->lite_; }

	size_t capacityFrames_ = 0;
	const ConvKernel* kernel_ = nullptr;
	ConvKernel owned_;
	std::array<Stage, kStages.size()> stages_ {};
	float* history_ = nullptr; // the head's input history: 2 x 128, a mirrored ring
	float* input_ = nullptr;   // every stage's input: a ring of kInputRing samples
	static constexpr size_t kInputRing = 2 * 8192;
	size_t inputPos_ = 0;      // where the next input sample goes
	/// LITE'S HALF-RATE ROAD: the decimated input (a ring), and each channel's interpolator history.
	float* half_ = nullptr;
	static constexpr size_t kHalfRing = 4 * kHalfBlock;
	size_t halfPos_ = 0;
	std::array<float*, 2> interp_ { nullptr, nullptr }; // the half-rate output's history, 2 x kInterpRing each (mirrored)
	static constexpr size_t kInterpRing = kHalfbandTaps / 2 + 1; // the even taps and the centre's reach
	size_t interpPos_ = 0;
	uint64_t samples_ = 0;     // input samples since reset: its parity is the decimator's phase
	size_t pos_ = 0;           // frames into the current base block
	uint64_t tick_ = 0;        // base blocks completed since reset
	uint64_t ffts_ = 0;
	uint64_t work_ = 0;
};

} // namespace feathers::fx::conv
