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

// THE WEAVE MODEL BENCHMARK — the cost of one physics tick (two sub-steps, the smoother, the path, the
// loudness control and the tables) for each WeaveModel, against the Ring. NOT A TEST: it asserts nothing
// and ctest never runs it, the posture of bench_render.cpp and bench_perform_push.cpp.
//
// What it times is advance(tickLength()) on a bowed string, twice: with the crossfade moving every tick (so the
// effective laws are rebuilt, the worst case) and parked — the physics alone, no scan: a reader's per-sample scan
// cost is the same for every model, because every model hands the scan the same 32-point tables.
//
// READING IT: the models run in rotating order within each of five repetitions, because this is a
// shared host whose load swings; take the medians and the RATIO to Ring, and re-run rather than trusting
// a number quoted from another day. A tick happens ~344.5 times a second per string, so ns/tick x 344.5
// is the string's CPU cost per second of audio. It times PROCESS CPU time (std::clock), not wall time:
// under the load this host runs at, a wall-clock run was preempted for most of its length (5.4 s of CPU in
// 19 s) and its ranges spanned 5x, where CPU time repeats to a few per cent.
//
// MEASURED 2026-09-25 23:55, load average 61-64 on 16 cores (a shared host, other sessions building; no
// quieter moment came), three runs of five repetitions each; median ns/tick (range of the three medians),
// x Ring:
//   laws rebuilt every tick   Ring 1627 (1572..1640) 1.00, Bar 2450 (2423..2474) 1.52, Tube 2430
//                             (2384..2442) 1.49, Membrane 6602 (6570..6711) 4.04, Stiffening 1808 (1801..1821) 1.11
//   laws parked               Ring 1296 (1265..1330) 1.00, Bar 2083 (2073..2093) 1.60, Tube 2057 (2046..2061)
//                             1.59, Membrane 4687 (4662..4779) 3.60, Stiffening 1469 (1423..1507) 1.13
// MEASURED 2026-09-26 ~09:00, load 1.4-1.7 (quiet), three runs alternated with the build before the lazy
// rest pose and with 2a8d4381's string built beside them; median ns/tick, before -> after (2a8d4381):
//   laws rebuilt every tick, nothing asked (the synth)
//                             Ring 2035 -> 526 (520), Bar 2263 -> 751 (752), Tube 2126 -> 741 (741),
//                             Membrane 9660 -> 2024 (2027), Stiffening 7163 -> 570 (567)
//   laws rebuilt every tick, motionEnergy() asked every tick (Resonate's gate), after only:
//                             Ring 598, Bar 821, Tube 805, Membrane 2318, Stiffening 799 — the rest pose
//                             re-solved at most every kRestPoseMaxAgeTicks (32) ticks, plus the sum.
// The rebuild used to solve the rest pose every time the laws moved; now it is solved on ask.
// A tick that carries a model switch also pays the rest-pose solve (not timed here): measured beside it
// at ~6 us into a 32-node model, ~18 us into Stiffening (Newton) and ~30 us into the Membrane (64 x 64).

#include "character/phi_weave.hpp"
#include "dsp/weave_string.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

volatile float g_sink = 0.0f;

double nsPerTick(feathers::WeaveModel model, int ticks, bool moving, bool asksEnergy) {
	feathers::WeaveString s;
	s.setModel(model);
	s.setLaws(owlet::dsp::buildPhiWeaveLaws(0.3), owlet::dsp::buildPhiWeaveLaws(0.68));
	s.prepare(48000.0);
	const std::clock_t t0 = std::clock();
	for (int t = 0; t < ticks; ++t) {
		s.setCrossfade(moving ? static_cast<float>(t % 200) / 200.0f : 0.25f);
		s.advance(s.tickLength());
		if (asksEnergy) {
			g_sink = s.motionEnergy(); // what Resonate's gate reads once a tick
		}
	}
	const std::clock_t t1 = std::clock();
	g_sink = s.sample(0.3, 0, 0);
	return static_cast<double>(t1 - t0) * (1e9 / CLOCKS_PER_SEC) / ticks;
}

void table(bool moving, bool asksEnergy) {
	constexpr int kTicks = 40000;
	constexpr int kReps = 5;
	std::printf("%s%s\n", moving ? "crossfade moving every tick (laws rebuilt every tick)"
	                            : "crossfade parked (laws cached)",
	            asksEnergy ? ", motionEnergy() read every tick (Resonate's gate):" : ":");
	double results[kReps][feathers::kWeaveModelCount] = {};
	for (int rep = 0; rep < kReps; ++rep) {
		for (int j = 0; j < feathers::kWeaveModelCount; ++j) {
			const int m = (j + rep) % feathers::kWeaveModelCount;
			results[rep][m] = nsPerTick(static_cast<feathers::WeaveModel>(m), kTicks, moving, asksEnergy);
		}
	}
	double ringMedian = 0.0;
	for (int m = 0; m < feathers::kWeaveModelCount; ++m) {
		double v[kReps];
		for (int rep = 0; rep < kReps; ++rep) {
			v[rep] = results[rep][m];
		}
		std::sort(v, v + kReps);
		const double median = v[kReps / 2];
		if (m == 0) {
			ringMedian = median;
		}
		std::printf("%-10s  ns/tick: median %6.0f  range %6.0f..%-6.0f  x Ring %.2f   us per second of audio %.0f\n",
		            feathers::weave::modelName(static_cast<feathers::WeaveModel>(m)), median, v[0], v[kReps - 1],
		            median / ringMedian, median * feathers::WeaveString::kTickHz / 1000.0);
	}
}

} // namespace

int main() {
	table(true, false);
	table(true, true);
	table(false, false);
	return 0;
}
