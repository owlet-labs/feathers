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

// THE ROTATOR BANK BENCHMARK — not a test (plan Task 8 quality round). One PartialBank, one 32-sample control block:
// setTargets (fade, floor, live count, one sincos and normalisation per partial, ramps, renormalisation) and then
// render, for 320 / 128 / 32 live partials, mono and stereo. Every block moves the pitch (vibrato) and the targets,
// so every block does the full work a voice does. Printed in ns per partial per sample, INCLUDING setTargets, beside
// the render-only figure (the bench_additive_voice.cpp kernel priced render at 0.31). The cases are ALTERNATED
// round by round and the median of the batch means is printed, because this host is shared and load swings within a
// run (CLAUDE.md); the load average is printed at both ends. Blocks are timed in batches of 50, because ONE CLOCK
// READ COSTS ~1.4 us on this host (printed below): per-block reads made the first draft of this bench a clock bench.

#include "dsp/partial_bank.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using feathers::partials::kControlBlock;
using feathers::partials::kMaxPartials;
using feathers::partials::PartialBank;
using clk = std::chrono::steady_clock;

volatile float gSink; // keeps the renders observable
constexpr int kVariants = 64; // precomputed control blocks, cycled
constexpr int kBlocksPerBatch = 50; // timed together: a clock read here costs ~1.4 us, measured below
constexpr int kBatchesPerRound = 4;
constexpr int kRounds = 25;

struct Tables {
	float inc[kVariants][kMaxPartials];
	float l[kVariants][kMaxPartials];
	float r[kVariants][kMaxPartials];
};

void fill(Tables& t) {
	for (int v = 0; v < kVariants; ++v) {
		// ~61 Hz at 48 kHz with 5.5 Hz vibrato (0.6%), so all 320 harmonics sit below the fade.
		const double time = v * kControlBlock / 48000.0;
		const double w0 = 0.008 * (1.0 + 0.006 * std::sin(2.0 * std::numbers::pi * 5.5 * time));
		for (int k = 0; k < kMaxPartials; ++k) {
			t.inc[v][k] = static_cast<float>((k + 1) * w0);
			const double base = 1.0 / (k + 1) * (k % 2 ? 0.6 : 1.0);
			t.l[v][k] = static_cast<float>(base * (1.0 + 0.2 * std::sin(0.1 * v + k)));
			t.r[v][k] = static_cast<float>(base * (1.0 + 0.2 * std::cos(0.13 * v + 0.7 * k)));
		}
	}
}

struct Case {
	const char* name;
	int partials;
	bool stereo;
	PartialBank* bank;
	std::vector<double> total, render; // ns per block
	int variant = 0;
};

double loadAverage() {
	double a = -1.0;
	if (FILE* f = std::fopen("/proc/loadavg", "r")) {
		if (std::fscanf(f, "%lf", &a) != 1)
			a = -1.0;
		std::fclose(f);
	}
	return a;
}

double median(std::vector<double>& v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

} // namespace

int main() {
	static Tables tables;
	fill(tables);
	static PartialBank banks[6];
	std::vector<Case> cases { { "320 partials, mono", 320, false, &banks[0], {}, {} },
		                      { "320 partials, stereo", 320, true, &banks[1], {}, {} },
		                      { "128 partials, mono", 128, false, &banks[2], {}, {} },
		                      { "128 partials, stereo", 128, true, &banks[3], {}, {} },
		                      { "32 partials, mono", 32, false, &banks[4], {}, {} },
		                      { "32 partials, stereo", 32, true, &banks[5], {}, {} } };
	for (Case& c : cases) {
		c.bank->reset(c.partials);
		c.bank->snapNextTargets();
	}
	alignas(16) static float outL[kControlBlock], outR[kControlBlock];
	const double loadBefore = loadAverage();
	std::vector<double> clockCost;
	for (int round = 0; round < kRounds; ++round) {
		for (Case& c : cases) {
			for (int batch = 0; batch < kBatchesPerRound; ++batch) {
				// setTargets + render, a batch of blocks between two clock reads.
				const auto t0 = clk::now();
				for (int b = 0; b < kBlocksPerBatch; ++b) {
					const int v = c.variant;
					c.variant = (c.variant + 1) % kVariants;
					c.bank->setTargets(tables.inc[v], tables.l[v], tables.r[v], c.partials, kControlBlock, c.stereo);
					c.bank->render(outL, outR, kControlBlock);
				}
				const auto t1 = clk::now();
				c.total.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / kBlocksPerBatch);
				// render alone: one untimed setTargets whose ramp spans the whole batch, so every timed render runs
				// the RAMPING kernel, as a voice's does.
				const int v = c.variant;
				c.variant = (c.variant + 1) % kVariants;
				c.bank->setTargets(tables.inc[v], tables.l[v], tables.r[v], c.partials, kControlBlock * kBlocksPerBatch,
				                   c.stereo);
				const auto t2 = clk::now();
				for (int b = 0; b < kBlocksPerBatch; ++b)
					c.bank->render(outL, outR, kControlBlock);
				const auto t3 = clk::now();
				c.render.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count() / kBlocksPerBatch);
			}
		}
		for (int i = 0; i < 64; ++i) {
			const auto a = clk::now();
			const auto z = clk::now();
			clockCost.push_back(std::chrono::duration<double, std::nano>(z - a).count());
		}
	}
	const double loadAfter = loadAverage();
	gSink = outL[3] + outR[7];
	std::printf("PartialBank, one 32-sample control block per voice, %d blocks per case (batches of 50, median of batch means), alternated in %d rounds\n",
	            kRounds * kBatchesPerRound * kBlocksPerBatch, kRounds);
	std::printf("load average (1 min): %.2f before, %.2f after; one clock read ~%.0f ns\n", loadBefore, loadAfter,
	            median(clockCost));
	for (Case& c : cases) {
		const double tot = median(c.total), ren = median(c.render);
		const double perPS = 1.0 / (c.partials * kControlBlock);
		std::printf("%-22s setTargets+render %7.0f ns/block = %.3f ns/partial/sample | render only %7.0f ns = %.3f | "
		            "setTargets share %4.1f%%\n",
		            c.name, tot, tot * perPS, ren, ren * perPS, 100.0 * (tot - ren) / tot);
	}
	return 0;
}
