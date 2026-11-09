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

// THE DELAY'S COST PER MODE (the delay-love arc, stage S0) — NOT A TEST (no add_test),
// bench_delay_multitap's posture. The baseline every later stage of the delay-love arc is held against: ns per
// stereo frame for each of the six modes, at Spread 0 and 30%, nothing moving, a 256-frame block. Medians of nine
// repetitions whose order rotates every repetition (memory: benchmarking on a shared host), so a load swing lands on
// every row rather than on one. Read the medians and run the binary for the base and the candidate alternately.

#include "fx/delay/delay.h"
#include "fx/delay/delay_types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace dl = feathers::fx::delay;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

volatile float g_sink = 0.0f;

struct Row {
	const char* name;
	dl::Mode mode;
	float spread;
	/// A FAKE LANE (a gain) in the loop — the S1 road's own cost, without a rack.
	bool lane = false;
};

dl::Settings settingsFor(const Row& row) {
	dl::Settings s = dl::stateOf(dl::rowOf(dl::State {}), static_cast<size_t>(dl::Param::kCount)).settings;
	s.mix = 0.5f;
	s.sync = 1; // 100 ms
	s.timeMultiple = 2.0;
	s.feedback = 0.5f;
	s.mode = row.mode;
	s.spread = row.spread;
	s.taps = dl::kMaxTaps;
	return s;
}

double nsPerFrame(const Row& row) {
	constexpr int kBlocks = 48000 * 3 / kBlock;
	auto d = std::make_unique<dl::Delay>();
	d->prepare(kRate);
	d->setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5));
	d->setTarget(settingsFor(row));
	static float gain = 0.7f;
	if (row.lane) {
		d->setLoopInsert({ &gain, [](void* c, float* l, float* r, int n) {
			                   const float g = *static_cast<float*>(c);
			                   for (int i = 0; i < n; ++i) {
				                   l[i] *= g;
				                   r[i] *= g;
			                   }
		                   },
		                   0 });
	}
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	double total = 0.0;
	for (int b = -50; b < kBlocks; ++b) {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		const auto t0 = std::chrono::steady_clock::now();
		d->processStereo(l.data(), r.data(), kBlock);
		const auto t1 = std::chrono::steady_clock::now();
		if (b >= 0) {
			total += std::chrono::duration<double, std::nano>(t1 - t0).count();
		}
		g_sink = g_sink + l[0];
	}
	return total / (static_cast<double>(kBlocks) * kBlock);
}

} // namespace

int main() {
	const Row rows[] = {
		{ "Clean", dl::Mode::Clean, 0.0f },          { "Clean, Spread 30%", dl::Mode::Clean, 0.3f },
		{ "Granular", dl::Mode::Granular, 0.0f },    { "Granular, Spread 30%", dl::Mode::Granular, 0.3f },
		{ "Xfade", dl::Mode::Xfade, 0.0f },          { "Xfade, Spread 30%", dl::Mode::Xfade, 0.3f },
		{ "Tape", dl::Mode::Tape, 0.0f },            { "Tape, Spread 30%", dl::Mode::Tape, 0.3f },
		{ "Multi-Tap 8", dl::Mode::MultiTap, 0.0f }, { "Multi-Tap 8, Spread 30%", dl::Mode::MultiTap, 0.3f },
		{ "Reverse", dl::Mode::Reverse, 0.0f },      { "Reverse, Spread 30%", dl::Mode::Reverse, 0.3f },
		{ "BBD", dl::Mode::Bbd, 0.0f },              { "BBD, Spread 30%", dl::Mode::Bbd, 0.3f },
		{ "Clean + lane", dl::Mode::Clean, 0.0f, true },  { "Tape + lane", dl::Mode::Tape, 0.0f, true },
		{ "Multi-Tap 8 + lane", dl::Mode::MultiTap, 0.0f, true }, { "Granular + lane", dl::Mode::Granular, 0.0f, true },
	};
	constexpr size_t kRows = std::size(rows);
	std::vector<double> ns[kRows];
	for (size_t rep = 0; rep < 9; ++rep) {
		for (size_t k = 0; k < kRows; ++k) {
			const size_t at = (k + rep) % kRows;
			ns[at].push_back(nsPerFrame(rows[at]));
		}
	}
	std::printf("DELAY COST (ns per stereo frame, block %d, medians of 9 rotating repetitions)\n", kBlock);
	for (size_t k = 0; k < kRows; ++k) {
		std::sort(ns[k].begin(), ns[k].end());
		std::printf("  %-26s %7.1f  (min %7.1f, max %7.1f)\n", rows[k].name, ns[k][4], ns[k][0], ns[k][8]);
	}
	return 0;
}
