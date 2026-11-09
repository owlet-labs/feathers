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

// THE PHASER/FLANGER BENCHMARK — what one device costs per sample, stereo, at 48 kHz in 256-sample blocks, with
// the LFO running and the loop hot (Feedback 0.7, Drive 0.3, Damping 4 kHz: every branch of the loop taken).
// NOT A TEST: it asserts nothing and ctest never runs it (bench_render's posture).
//
// The rows are the plan's Task 5 question: the barberpole runs three cascades (Phaser) or three reads of one
// line (Flanger), so what does Up cost against Normal? Phaser at 12 stages, Flanger with through-zero on. The
// rows ALTERNATE inside each repetition and the repetitions rotate the order, because this is a shared host
// whose load swings (memory: benchmarking on a shared host) — take the medians, and re-run rather than quote a
// number from another day. "ns/sample" is per STEREO sample frame.

#include "fx/phaser/phaser.h"
#include "fx/phaser/phaser_response.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <vector>

namespace {

namespace ph = feathers::fx::phaser;

volatile float g_sink = 0.0f;

double nsPerSample(ph::Mode mode, ph::SweepShape sweep, bool throughZero, int stages, bool travelled,
                   bool curveHome = false) {
	constexpr int kBlock = 256;
	constexpr int kBlocks = 48000 * 10 / kBlock; // 10 s
	ph::Settings s;
	s.mode = mode;
	s.sweep = sweep;
	s.throughZero = throughZero;
	s.stages = stages;
	s.feedback = 0.7f;
	s.drive = 0.3f;
	s.dampingLog2Hz = static_cast<float>(std::log2(4000.0));
	s.lfoOn = true;
	s.lfoDepth = 0.8f;
	s.rateLog2Hz = 0.0f; // 1 Hz
	if (travelled) {
		// THE EXPLORE LAYER AWAY FROM HOME on every one of its thirty-four phases (folded travel, as the pad stores it):
		// every stage its own corner, every offset and the bent LFO live — its most expensive state.
		for (size_t i = 0; i < s.travel.size(); ++i) {
			const double v = 0.137 * static_cast<double>(i + 1) + 0.29;
			s.travel[i] = static_cast<float>(v - std::floor(v));
		}
		// ROUND 2'S SWEEP CURVE LEFT HOME on request (`curveHome`): the A/B for what its per-sample pow calls cost.
		if (curveHome) {
			s.travel[ph::phase_space::kCurvePhase] = 0.0f;
		}
	}
	auto d = std::make_unique<ph::Phaser>();
	d->setTarget(s);
	d->prepare(48000.0);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	auto fill = [&]() {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			seed = seed * 1664525u + 1013904223u;
			r[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
	};
	for (int b = 0; b < 200; ++b) { // warm: the loop full, the glides landed
		fill();
		d->processStereo(l.data(), r.data(), kBlock);
	}
	double total = 0.0;
	for (int b = 0; b < kBlocks; ++b) {
		fill();
		const auto t0 = std::chrono::steady_clock::now();
		d->processStereo(l.data(), r.data(), kBlock);
		total += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
	}
	g_sink = l[0] + r[0];
	return total / (static_cast<double>(kBlocks) * kBlock);
}

/// THE PLOT'S COST ON THE UI THREAD (Task 8): one channel's model built, then `magnitudeDb` at 512 log-spaced
/// frequencies, 20 Hz to 20 kHz — what the card will pay every frame. Microseconds for the whole sweep, the median of
/// `kSweeps` sweeps (the model built once per sweep, as a frame would).
double usPerPlotSweep(ph::Mode mode, bool travelled) {
	constexpr int kPoints = 512;
	constexpr int kSweeps = 400;
	ph::Settings s;
	s.mode = mode;
	s.sweep = ph::SweepShape::Up;
	s.throughZero = mode == ph::Mode::Flanger;
	s.stages = 12;
	s.feedback = 0.7f;
	s.drive = 0.3f;
	s.dampingLog2Hz = static_cast<float>(std::log2(4000.0));
	s.lfoDepth = 0.8f;
	if (travelled) {
		for (size_t i = 0; i < s.travel.size(); ++i) {
			const double v = 0.137 * static_cast<double>(i + 1) + 0.29;
			s.travel[i] = static_cast<float>(v - std::floor(v));
		}
	}
	ph::ResponseInputs in;
	in.lfoPhase = 0.37;
	in.barberPhase = 0.37;
	std::vector<double> hz(kPoints);
	for (int i = 0; i < kPoints; ++i) {
		hz[static_cast<size_t>(i)] = 20.0 * std::pow(1000.0, static_cast<double>(i) / (kPoints - 1));
	}
	std::vector<double> us(kSweeps);
	for (int k = 0; k < kSweeps; ++k) {
		in.lfoPhase = 0.37 + 1.0e-4 * k; // a moving LFO: the model is rebuilt from new readings every frame
		const auto t0 = std::chrono::steady_clock::now();
		const ph::ResponseModel m = ph::responseModelOf(s, in, 48000.0);
		float acc = 0.0f;
		for (const double f : hz) {
			acc += ph::magnitudeDb(m, f);
		}
		us[static_cast<size_t>(k)] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		g_sink = acc;
	}
	std::sort(us.begin(), us.end());
	return us[us.size() / 2];
}

} // namespace

int main() {
	struct Row {
		const char* name;
		ph::Mode mode;
		ph::SweepShape sweep;
		bool throughZero;
		int stages;
		bool travelled = false;
		bool curveHome = false;
	};
	// EACH ROW AT HOME AND WITH TRAVEL (the explore layer's cost), alternating like every other pair.
	const Row rows[] = {
		{ "Phaser 12, Normal", ph::Mode::Phaser, ph::SweepShape::Normal, false, 12 },
		{ "Phaser 12, Normal +T", ph::Mode::Phaser, ph::SweepShape::Normal, false, 12, true },
		{ "Phaser 12, Up", ph::Mode::Phaser, ph::SweepShape::Up, false, 12 },
		{ "Phaser 12, Up +T", ph::Mode::Phaser, ph::SweepShape::Up, false, 12, true },
		{ "Flanger TZ, Normal", ph::Mode::Flanger, ph::SweepShape::Normal, true, 12 },
		{ "Flanger TZ, Normal +T", ph::Mode::Flanger, ph::SweepShape::Normal, true, 12, true },
		{ "Flanger TZ, Up", ph::Mode::Flanger, ph::SweepShape::Up, true, 12 },
		{ "Flanger TZ, Up +T", ph::Mode::Flanger, ph::SweepShape::Up, true, 12, true },
		{ "Flanger, Normal", ph::Mode::Flanger, ph::SweepShape::Normal, false, 12 },
		{ "Flanger, Normal +T", ph::Mode::Flanger, ph::SweepShape::Normal, false, 12, true },
		{ "Flanger, Up", ph::Mode::Flanger, ph::SweepShape::Up, false, 12 },
		{ "Flanger, Up +T", ph::Mode::Flanger, ph::SweepShape::Up, false, 12, true },
		{ "Flanger, Up +T -curve", ph::Mode::Flanger, ph::SweepShape::Up, false, 12, true, true },
		{ "Flanger TZ, Up +T -curve", ph::Mode::Flanger, ph::SweepShape::Up, true, 12, true, true },
	};
	constexpr int kRows = static_cast<int>(std::size(rows));
	constexpr int kReps = 5;
	double ns[kReps][kRows] = {};
	for (int rep = 0; rep < kReps; ++rep) {
		for (int j = 0; j < kRows; ++j) {
			const int k = (j + rep) % kRows;
			ns[rep][k] = nsPerSample(rows[k].mode, rows[k].sweep, rows[k].throughZero, rows[k].stages, rows[k].travelled,
			                         rows[k].curveHome);
		}
	}
	const double budgetNs = 1.0e9 / 48000.0;
	std::printf("stereo, 48 kHz, 256-sample blocks, LFO 1 Hz Depth 0.8, Feedback 0.7, Drive 0.3, Damping 4 kHz:\n");
	for (int k = 0; k < kRows; ++k) {
		double v[kReps];
		for (int rep = 0; rep < kReps; ++rep) {
			v[rep] = ns[rep][k];
		}
		std::sort(v, v + kReps);
		std::printf("%-22s ns/sample:", rows[k].name);
		for (int rep = 0; rep < kReps; ++rep) {
			std::printf(" %7.1f", ns[rep][k]);
		}
		std::printf("   median %7.1f   %.2f%% of a core\n", v[kReps / 2], 100.0 * v[kReps / 2] / budgetNs);
	}
	std::printf("\nTHE PLOT (UI thread): one channel's model + magnitudeDb at 512 frequencies, Up, 12 stages, median of 400:\n");
	std::printf("%-22s %8.1f us\n", "Phaser 12, Up", usPerPlotSweep(ph::Mode::Phaser, false));
	std::printf("%-22s %8.1f us\n", "Phaser 12, Up +T", usPerPlotSweep(ph::Mode::Phaser, true));
	std::printf("%-22s %8.1f us\n", "Flanger TZ, Up", usPerPlotSweep(ph::Mode::Flanger, false));
	return 0;
}
