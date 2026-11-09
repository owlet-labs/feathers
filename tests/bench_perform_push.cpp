/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE PERFORM PUSH BENCHMARK — one `macroPushOf` against one `performPushOf`, timed. NOT A TEST: it
// asserts nothing and ctest never runs it, the same posture as `bench_render.cpp`. It exists because
// `perform.h`'s cost table makes four claims about where the circle made the device cheaper and where it
// made it dearer, and a number in a comment with no instrument behind it is an estimate that has learned
// to look like a measurement (CLAUDE.md: "take the shape from the model; take the number from the
// instrument").
//
// WHY IT IS A QUESTION AT ALL: `performPushOf` calls `macroPushOf` FOUR times — one per waypoint — where
// the pre-circle road called it once per call site. **IT WAS FIVE, the fifth being the Macro/Macro Cfg
// knobs, and those controls were deleted on 2026-09-19** with the circle's centre becoming a zero push;
// any number in `perform.h` or below quoted from before that date is the five-call figure. So the counts
// moved
// in both directions at once: `advanceSlice` went from 3 calls to 1 blend, `grainPlanFor` from 2 PER
// GRAIN to 1 blend per grain, the picture walk from 2 per step over 64 steps to 1 for the whole walk, and
// `publishReadings` from nothing at all on a still device to one blend every block (the push is IN the
// key, so it is built before the key is compared).
//
// **AND WHY THE INPUT SET IS SMALL.** A second draft filled one `Settings` per iteration — 200,000 rows,
// about 22 MB — which streams from memory, and the two loops do not stream EQUALLY: the blend touches the
// corners and the puck as well as the three fields the single call reads, so it paid more memory traffic
// on top of more arithmetic. The bias ran in the safe direction (the blend looked dearer than it is in
// situ, where the device reads one hot `target_` and pays neither) but it was unstated, and 70-odd ns for
// six phi triangles suggested memory was a large share of both numbers. The set is `kRows` = 8192 now —
// what the `i & 8191` mask always implied — wrapped by that mask, so both loops are cache-resident and
// what is left is the arithmetic.
//
// **HOW IT AVOIDS THE TRAP THAT INVALIDATED ITS OWN FIRST DRAFT.** That draft varied only `s.macro`
// between iterations and reported `macroPushOf` at 0.76 ns against `performPushOf` at 191 ns — a 251x
// ratio for 5x the work, which is what a wrong instrument looks like. Every term of `macroPushOf` is
// `macro · f(cfg, gamma) · K`, so with `cfg` and `gamma` loop-invariant the compiler hoisted all six phi
// triangles out of the loop and left a multiply. Here the inputs come from a precomputed array of
// `Settings` that nothing can hoist, BOTH loops read the same array, and all eight terms of the result
// reach a file-scope volatile sink so no term is dead. **AND THE LITERAL 1.0f MACRO IS ON BOTH SIDES**:
// `x * 1.0f` is exactly `x` so the compiler may drop those multiplies and fold the 0.01 gate, but it may
// do so in the A loop and the B loop alike, which is what keeps the ratio the call count. (A LOCAL
// `volatile` pointer is the trap CLAUDE.md
// records — the pointer is dead and the work goes with it; a store of a VALUE to volatile storage must
// happen.)
//
// READING IT: it alternates A and B within each repetition and prints three, because this is a shared
// host whose load swings (CLAUDE.md, benchmarking on a shared host). Take medians, and re-run it rather
// than trusting a number quoted from another day.

#include "fx/sklitter/perform.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace feathers::fx::sklitter;

namespace {

volatile float gSink = 0.0f;

/// ALL EIGHT TERMS, so that no part of either push is dead code.
float sumOf(const MacroPush& p) {
	return p.dryThreshold + p.dReverse + p.dPitch + p.dDelay + p.gatePull + p.subdivMix + p.zoneAPush
	       + p.zoneBPush;
}

} // namespace

int main() {
	constexpr int kIters = 200000;
	constexpr int kRows = 8192; // ~900 KB of Settings: resident, so neither loop is measuring memory
	std::vector<Settings> rows(static_cast<size_t>(kRows));
	for (int i = 0; i < kRows; ++i) {
		Settings& s = rows[static_cast<size_t>(i)];
		// UGLY, NON-DYADIC MULTIPLIERS: a probe value that shares a factor with the quantity under test is
		// a probe that cannot express the answer — the same rule the phase-space fixtures follow.
		const float v = static_cast<float>(i) * (1.0f / 8192.0f);
		s.gamma = v * 1.3f;
		for (size_t c = 0; c < kCorners; ++c) {
			s.cornerCfg[c] = v + 0.1875f * static_cast<float>(c);
		}
		// OFF THE CENTRE, so the blend is the one the device does when the puck has been moved. At the
		// centre the arithmetic is the same — `blendPush` has no fast path — but a reader should not have
		// to know that to trust the number.
		s.perfX = 0.25f;
		s.perfY = -0.5f;
		// **AND OFF BOTH ENDS OF THE DEPTH KNOB, WHICH IS NOT COSMETIC SINCE 2026-09-19.**
		// `performAmountOf` curves the knob with a `std::pow` and SKIPS it at exactly 0 and 1 — so a
		// bench left at the declared default of 1.0 would measure the guard rather than the path the
		// device takes whenever a hand has moved that knob. 0.63 is inside, and ugly enough not to share
		// a factor with anything here.
		s.perfDepth = 0.63f;
	}

	double lastOne = 0.0;
	double lastFive = 0.0;
	for (int rep = 0; rep < 3; ++rep) {
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kIters; ++i) {
			const Settings& s = rows[static_cast<size_t>(i & (kRows - 1))];
			// **THE A SIDE IS ONE WAYPOINT'S BUILD, WHICH IS WHAT THE B SIDE DOES FOUR OF.** It read
			// `s.macro` and `s.macroConfig` until those fields were deleted (2026-09-19); it now reads
			// what `performPushOf` reads — one corner's configuration at macro 1.0 — so the ratio is
			// still exactly the call count and nothing else changed about either side.
			gSink = sumOf(macroPushOf(1.0f, splitCoordinate(s.cornerCfg[0]), s.gamma));
		}
		const auto t1 = std::chrono::steady_clock::now();
		// **THE WEIGHT BUILD IS INSIDE THE TIMED LOOP, WHICH IS WHAT KEEPS THIS COMPARABLE TO THE FIGURES
		// IN `perform.h`.** `performPushOf` used to call `performWeightsOf` itself; it now takes the
		// weights, so the call moved OUT of the function and had to move INTO the loop or the B side
		// would silently get cheaper than the one that was measured.
		for (int i = 0; i < kIters; ++i) {
			const Settings& p = rows[static_cast<size_t>(i & (kRows - 1))];
			gSink = sumOf(performPushOf(p, performWeightsOf(p.perfX, p.perfY), p.perfDepth));
		}
		const auto t2 = std::chrono::steady_clock::now();

		lastOne = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
		lastFive = std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;
		std::printf("rep %d: macroPushOf %6.1f ns | performPushOf %6.1f ns | ratio %.2fx\n", rep, lastOne,
		            lastFive, lastFive / lastOne);
	}

	// THE FOUR CLOCKS THE TABLE IN `perform.h` REPORTS AGAINST. Each denominator is different, which is
	// the whole reason they are listed separately rather than summed.
	const double grainNs = 1024.0 / 48000.0 * 1e9;  // refreshGrainGeometry's literal, at 48 kHz
	const double sliceNs = 6000.0 / 48000.0 * 1e9;  // a sixteenth at 120 BPM
	const double blockNs = 128.0 / 48000.0 * 1e9;   // a 128-frame host block
	std::printf("\nSLICE  (a sixteenth @120bpm/48k = %.0f ns): 3 x macroPushOf %7.1f ns -> 1 x blend %7.1f ns\n",
	            sliceNs, 3 * lastOne, lastFive);
	std::printf("GRAIN  (1024 frames @48k = %.0f ns, x2 voices): 2 x macroPushOf %7.1f ns -> 1 x blend %7.1f ns"
	            "  (%+.1f ns, %.4f%% -> %.4f%% of one grain)\n",
	            grainNs, 2 * lastOne, lastFive, lastFive - 2 * lastOne, 100.0 * 2 * lastOne / grainNs,
	            100.0 * lastFive / grainNs);
	std::printf("WALK   (per REBUILD, 64 steps): 128 x macroPushOf %7.1f ns -> 1 x blend %7.1f ns\n",
	            128 * lastOne, lastFive);
	std::printf("STILL  (per BLOCK, %.0f ns at 128 frames): 0 ns -> 1 x blend %7.1f ns  (%.3f%% of a core)\n",
	            blockNs, lastFive, 100.0 * lastFive / blockNs);
	std::printf("\nsink %g\n", gSink);
	return 0;
}
