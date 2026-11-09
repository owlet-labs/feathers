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

// THE EQ'S LINEAR PHASE (step 7).
//
// ONE FIR PER PATH, MID AND SIDE. The input is always taken to mid and side, and each is convolved with its
// path's kernel: the Stereo bands and the Mid bands for mid, the Stereo bands and the Side bands for side
// (the same paths the curve draws). With no Mid or Side band the two kernels are the same and this is the
// stereo EQ. Converting ALWAYS, rather than only when an M/S band acts as minimum phase does, means the
// domain never changes under a running convolution; on a mono signal the side is exactly zero, so a Mid band
// there is still bit-identical to the same band on Stereo.
//
// THE KERNEL is the path's DIGITAL magnitude — the same one minimum phase realises and the plot draws (R5),
// so the Phase switch changes phase only — sampled at N points, zero phase, centred at N/2 and Blackman
// windowed (a periodic window, symmetric about N/2).
//
// THE CONVOLUTION is uniform partitioned overlap-save in blocks of B, one frequency-domain delay line per
// path. Output is B late for the blocking and N/2 for the kernel's centre: N/2 + B, `linearLatency`.
//
// A REBUILD is sliced across the 32-sample control grid (one slice a step: magnitude bins in chunks, then
// the inverse transform and window, then the partition transforms), into the spare of two kernel sets, on
// the audio thread and allocation-free, so it lands at the same sample however fast a render runs. When it
// is complete the next block edge crossfades from the old kernel to the new over one block. Two
// linear-phase kernels with one centre crossfade to a linear-phase result: the swap cannot comb (§3.5).
//
// Every buffer is allocated in `prepare`.

#include "fx/eq/eq_design.h"
#include "fx/eq/eq_types.h"

#include <array>
#include <cstddef>

struct PFFFT_Setup;

namespace feathers::fx::eq {

class LinearPhase {
public:
	/// WHAT A KERNEL IS BUILT FROM: each band's design and the tilt, and which path each band is on.
	struct Target {
		std::array<BandDesign, kBandCount> bands {};
		std::array<Channel, kBandCount> channel {};
		TiltDesign tilt {};
	};

	LinearPhase() = default;
	LinearPhase(const LinearPhase&) = delete;
	LinearPhase& operator=(const LinearPhase&) = delete;
	~LinearPhase() { release(); }

	void prepare(double sampleRate);
	/// Forget the audio (the delay lines and the block in progress), keep the kernel.
	void clearAudio() noexcept;
	/// Build `t` into the live kernel NOW, whole, and cancel any build in flight. For a start: a switch into
	/// Linear, or a reset, where there is no kernel to crossfade from.
	void buildNow(const Target& t) noexcept;
	/// Start a sliced build of `t`. False (and nothing starts) while one is in flight or waiting to swap in.
	bool request(const Target& t) noexcept;
	[[nodiscard]] bool busy() const noexcept { return stage_ != Stage::Idle; }
	/// ONE CONTROL-GRID STEP of a build in flight.
	void step() noexcept;

	/// ONE SAMPLE, mid and side in and out, N/2 + B late.
	void process(float mid, float side, float& midOut, float& sideOut) noexcept {
		inputs_[0][pos_] = mid;
		inputs_[1][pos_] = side;
		midOut = outputs_[0][pos_];
		sideOut = outputs_[1][pos_];
		if (++pos_ == block_) {
			pos_ = 0;
			blockEdge();
		}
	}

	[[nodiscard]] size_t firLength() const noexcept { return n_; }
	[[nodiscard]] size_t blockLength() const noexcept { return block_; }
	[[nodiscard]] int buildsForTest() const noexcept { return builds_; }
	[[nodiscard]] int swapsForTest() const noexcept { return swaps_; }


private:
	enum class Stage { Idle, Magnitude, Inverse, Partition, Ready };

	/// ONE SECTION'S |H|² AS A FUNCTION OF cos w AND cos 2w ALONE — (nA + nB cos w + nC cos 2w) over (dA + dB cos w +
	/// dC cos 2w) — the same magnitude as `magnitudeSquared`, expanded once per build instead of four trig calls a bin.
	struct Section2 {
		double nA, nB, nC, dA, dB, dC;
	};
	static constexpr size_t kMaxPathSections = kBandCount * kMaxSectionsPerBand + kTiltSections;
	/// Flatten `target_` into the two paths' section lists and gains.
	void expand() noexcept;

	void release() noexcept;
	void blockEdge() noexcept;
	/// Bins [first, last) of path `path`'s magnitude, from `target_`.
	void magnitudes(int path, size_t first, size_t last) noexcept;
	/// The inverse transform, centring and window of path `path`, into `fir_[path]`.
	void inverse(int path) noexcept;
	/// Partitions [first, last) of path `path` into kernel set `set`.
	void partition(int path, size_t set, size_t first, size_t last) noexcept;
	[[nodiscard]] bool pathsAlike() const noexcept;

	double sampleRate_ = 48000.0;
	size_t n_ = 0, block_ = 1, parts_ = 0;
	PFFFT_Setup* big_ = nullptr;   // N, the kernel's inverse
	PFFFT_Setup* small_ = nullptr; // 2B, the partitions and the blocks
	// Kernels: [set][path] of `parts_` spectra of 2B, in pffft's internal order.
	std::array<std::array<float*, 2>, 2> kernel_ {};
	size_t live_ = 0;
	// Audio: the input block in progress, the last one, the delay lines, the accumulators, the outputs.
	std::array<float*, 2> inputs_ {}, previous_ {}, fdl_ {}, accum_ {}, fadeAccum_ {}, outputs_ {};
	float* window2B_ = nullptr;
	float* work2B_ = nullptr;
	size_t pos_ = 0, head_ = 0;
	// The build: its target, its stage, where it has got to, and its scratch.
	Target target_ {};
	Stage stage_ = Stage::Idle;
	int path_ = 0;
	size_t next_ = 0;
	bool alike_ = false;
	std::array<std::array<Section2, kMaxPathSections>, 2> sections_ {};
	std::array<size_t, 2> sectionCount_ {};
	std::array<double, 2> pathGain_ {};
	std::array<float*, 2> magnitude_ {}; // N/2 + 1 each
	std::array<float*, 2> fir_ {};       // N each: the last built taps per path
	float* spectrumN_ = nullptr;
	float* workN_ = nullptr;
	float* windowN_ = nullptr;
	int builds_ = 0, swaps_ = 0;
};

} // namespace feathers::fx::eq
