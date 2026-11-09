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

// **A-PH: THE ADDITIVE ENGINE'S PHASES, OPTIMISED.**
//
// With the partials' amplitudes fixed the energy is fixed (Parseval): phases move only the PEAK. A low-crest phase profile
// buys headroom — the same sound peaks lower, so it can be turned up as far before a ceiling. The engine's note-on law
// (weighted Schroeder, PartialBank::reset(power, count)) is the closed form; this is the iterative reducer that beats it
// (measured 5.04 → 2.64 dB of crest on the default patch), and the plumbing that gets its result to a note-on without
// the audio thread ever waiting, allocating or locking:
//
//   - `PhaseOptimiser` (the BUILD side, a worker thread the synth owns): one period of Σ A_k sin(kθ + φ_k), clipped at
//     0.9 of its peak, the phases taken back from the clipped period's spectrum, the amplitudes kept — Van der Ouderaa's
//     L∞ iteration, from the closed form, so its worst case is today's. FFT, preallocated at construction.
//   - `PhaseRequestSlot` (audio → worker) and `PhaseProfileStore` (worker → audio): single-writer seqlocks over fixed
//     arrays. The request slot holds ONE request — the latest wins, so a modulated sound that re-bakes every block makes
//     the worker compute only the newest spectrum, never a backlog. Every request and result carries the table's BUILD
//     SERIAL (the ABA rule: an address is not an identity, CLAUDE.md).
//   - `profileDistance`: at note-on the voice takes the latest profile only when it was optimised for (nearly) the
//     spectrum the note is about to play — the serial says which job a profile came from; CLOSENESS says whether to use
//     it, because under modulation the serial moves every block and no profile would ever match it exactly.
//
// THE PROFILE IS USED ONLY WHERE PHASES ARE ALREADY RESET (a note-on from silence or a retrigger: the bank's priming).
// A legato note, a glide, a held note whose spectrum moves, a stolen voice fading out: their phases are never touched.

#include "dsp/partial_spectrum.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers::partials {

/// WHAT THE ENGINE DOES WITH ITS PHASES (Phases: Natural / Headroom / Sub priority).
enum class PhaseMode : uint8_t { Natural, Headroom, SubPriority };

/// The engine-wide phasing settings, carried on the Character and the prepared table.
struct Phasing {
	PhaseMode mode{PhaseMode::Natural};
	/// SUB PRIORITY: the sub partial's (partial 0's) LEVEL against the rest's root-sum-square, dB, set after every drawer
	/// operator and Damp, none of which touch it (A-PH3, design §8.10). 0 dB: as much energy as all the other partials together.
	float subDb{-6.0f};
	/// SUB PRIORITY: the per-voice peak ceiling, dBFS. A voice whose optimised peak would pass it is scaled down WHOLE (sub
	/// and rest together, so the balance survives); never up. The mix's final ceiling is a Limit device's job (by design).
	float ceilingDb{-6.0f};
	friend bool operator==(const Phasing&, const Phasing&) = default;
};

/// THE SUB'S LEVEL against the rest's RSS, linear (Sub priority only; meaningless otherwise).
[[nodiscard]] inline float subLevelOf(const Phasing& p) noexcept;

/// ONE OPTIMISED PHASE PROFILE: the phases for partials 0..count-1 (partial i is treated as harmonic i + 1), the unit-RSS
/// amplitudes they were optimised for, and the period's peak per unit RSS (so a voice can predict its own peak).
struct PhaseProfile {
	uint64_t serial{0}; ///< the build serial of the table it was optimised for; 0 = none
	int count{0};
	std::array<float, kMaxPartials> amp{};   ///< unit root-sum-square
	std::array<float, kMaxPartials> phase{}; ///< radians, the bank's convention: partial i renders amp·sin(phase + n·w)
	float peakPerRss{0.0f};                  ///< max over one period of |Σ amp_k sin(...)|, for amplitudes of unit RSS
	float crestDb{0.0f};                     ///< 20·log10(peak / RMS), as measured on the optimised period
};

/// THE CLOSED FORM, the bank's own note-on law (PartialBank::reset(power, count)), as phases: φ_k = −2π·(k·S0 − S1).
void schroederPhases(const float* amp, int count, float* phase) noexcept;

/// One period of Σ amp_k·sin(k·θ + phase_k) sampled at `points`: its peak |x| (direct synthesis; a MEASURE for tests and
/// the design's tables, not for the audio thread).
[[nodiscard]] double periodPeak(const float* amp, const float* phase, int count, int points);

/// THE ITERATIVE REDUCER. Allocates its FFT scratch at construction (the largest size it will ever use); `optimise`
/// allocates nothing. Not for the audio thread: hundreds of FFTs a call.
class PhaseOptimiser {
public:
	PhaseOptimiser();
	/// Optimise for `amp` (any scale; count ≤ kMaxPartials) over `rounds`; `out.serial` is left for the caller.
	void optimise(const float* amp, int count, PhaseProfile& out, int rounds = 300);

private:
	/// The smallest power of two holding 8 points per period per harmonic, at least 512 (the peak is read on it).
	[[nodiscard]] static int sizeFor(int count) noexcept;
	void fft(double* re, double* im, int n, bool inverse) noexcept;
	std::vector<double> re_, im_;
	std::vector<double> amp_;
	std::vector<double> phase_, best_;
};

/// HOW FAR APART TWO SPECTRA ARE, as the voice judges a profile: ‖a/|a| − p.amp‖ over max(count, p.count) partials (both
/// unit RSS), in [0, 2]. O(count), no allocation: safe at a note-on on the audio thread.
[[nodiscard]] float profileDistance(const PhaseProfile& p, const float* amp, int count) noexcept;

/// THE CLOSENESS BOUND: a profile optimised for a spectrum within this of the note's is used; otherwise the closed form.
inline constexpr float kProfileTolerance = 0.2f;

/// **AUDIO → WORKER: THE LATEST REQUEST.** One writer (the audio thread), one reader (the worker). A seqlock over a
/// fixed array: the writer makes the sequence odd, stores, makes it even; a reader that saw it odd or moving reads
/// nothing this time and tries again on its next poll. No allocation, no lock, and a request the worker has not taken yet
/// is simply overwritten by a newer one (latest wins: the throttle).
class PhaseRequestSlot {
public:
	struct Request {
		uint64_t serial{0};
		int count{0};
		std::array<float, kMaxPartials> amp{};
	};
	/// THE AUDIO THREAD: post the spectrum of build `serial`.
	void post(uint64_t serial, const float* amp, int count) noexcept;
	/// THE WORKER: the newest request it has not taken yet, if any (and a consistent one).
	bool take(Request& out) noexcept;
	/// How many requests were posted, and how many the worker took: the difference is the throttle at work (requests a
	/// newer one overwrote before the worker reached them).
	[[nodiscard]] uint64_t postedForTest() const noexcept { return posted_.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t takenForTest() const noexcept { return takenCount_.load(std::memory_order_relaxed); }

private:
	std::atomic<uint32_t> sequence_{0};
	std::atomic<uint64_t> serial_{0};
	std::atomic<int> count_{0};
	std::array<std::atomic<float>, kMaxPartials> amp_{};
	uint64_t taken_{0}; ///< the worker's own: the last serial it took
	std::atomic<uint64_t> posted_{0};
	std::atomic<uint64_t> takenCount_{0};
};

/// **WORKER → AUDIO: THE LATEST PROFILE.** One writer (the worker), any readers (voices at note-on). The same seqlock;
/// a read that loses the race uses the closed form for that note, never a torn profile.
class PhaseProfileStore {
public:
	void publish(const PhaseProfile& p) noexcept;
	/// A consistent copy of the latest profile, or false (none yet, or the writer was mid-publish).
	bool read(PhaseProfile& out) const noexcept;
	/// TEST ONLY: the serial of the latest profile (0 = none).
	[[nodiscard]] uint64_t serialForTest() const noexcept { return serial_.load(std::memory_order_acquire); }

private:
	std::atomic<uint32_t> sequence_{0};
	std::atomic<uint64_t> serial_{0};
	std::atomic<int> count_{0};
	std::atomic<float> peakPerRss_{0.0f};
	std::atomic<float> crestDb_{0.0f};
	std::array<std::atomic<float>, kMaxPartials> amp_{};
	std::array<std::atomic<float>, kMaxPartials> phase_{};
};

inline float subLevelOf(const Phasing& p) noexcept {
	return static_cast<float>(std::pow(10.0, static_cast<double>(p.subDb) / 20.0));
}

} // namespace feathers::partials
