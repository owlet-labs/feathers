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

// THE MULTI-TAP UNDER MOVING CONTROLS (backlog MD) — NOT A TEST (no add_test), bench_phaser's posture.
//
// Two tables. CLICKS: a steady 440 Hz sine through Multi-Tap, wet only, the controls moved once per tick window
// (64 frames, as a modulator moves them), and the worst sample-to-sample step reported against the SAME device's
// worst step while nothing moves; "clicks/s" counts the samples whose step is more than 1.5x that. COST: ns per
// stereo frame, Multi-Tap at eight taps, static, with an LFO on Shape and with an LFO on Taps. The rows ALTERNATE
// inside each repetition (memory: benchmarking on a shared host) — read the medians, and alternate this binary
// with the other build's rather than quoting either alone.

#include "fx/delay/delay.h"
#include "fx/delay/delay_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

namespace dl = feathers::fx::delay;

constexpr double kRate = 48000.0;
constexpr int kTick = 64;
constexpr double kTwoPi = 6.28318530717958647692;

volatile float g_sink = 0.0f;

dl::Settings base(int taps) {
	dl::Settings s = dl::stateOf(dl::rowOf(dl::State {}), static_cast<size_t>(dl::Param::kCount)).settings;
	s.mix = 1.0f;
	s.sync = 1; // 100 ms
	s.timeMultiple = 2.0;
	s.feedback = 0.4f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = dl::Mode::MultiTap;
	s.taps = taps;
	return s;
}

/// WHAT MOVES: the settings for the tick starting at frame `at`.
using Mover = std::function<void(dl::Settings&, int at)>;

struct Clicks {
	double worstRatio = 0.0;
	double clicksPerSecond = 0.0;
	double restDb = 0.0;
};

Clicks clicks(int taps, const Mover& move, int seconds) {
	dl::Delay d;
	d.prepare(kRate);
	d.setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5));
	dl::Settings s = base(taps);
	d.setTarget(s);
	const int warm = 24000;
	const int frames = warm + seconds * static_cast<int>(kRate);
	std::vector<float> l(static_cast<size_t>(frames));
	std::vector<float> r(static_cast<size_t>(frames));
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(i) / kRate));
		r[i] = l[i];
	}
	for (int at = 0; at < frames; at += kTick) {
		if (at >= warm) {
			dl::Settings m = s;
			move(m, at - warm);
			d.setTarget(m);
		}
		d.processStereo(l.data() + at, r.data() + at, std::min(kTick, frames - at));
	}
	float typical = 0.0f;
	for (int i = warm / 2 + 1; i < warm; ++i) {
		typical = std::max(typical, std::fabs(l[static_cast<size_t>(i)] - l[static_cast<size_t>(i) - 1]));
	}
	Clicks c;
	int count = 0;
	for (int i = warm; i < frames; ++i) {
		const float step = std::fabs(l[static_cast<size_t>(i)] - l[static_cast<size_t>(i) - 1]);
		c.worstRatio = std::max(c.worstRatio, static_cast<double>(step / typical));
		count += step > 1.5f * typical ? 1 : 0;
	}
	c.clicksPerSecond = count / static_cast<double>(seconds);
	// **WHAT IS NOT THE TONE**: the output demodulated at 440 Hz and averaged over 12.5 ms keeps what lies within
	// about 40 Hz of the tone — its level and its pitch bends, the comb the taps make, all slow — and the rest is
	// everything else: clicks, and a ripple at a crossfade's own rate. dB of the whole output's energy.
	constexpr int kWindow = 600;
	const int n = frames - warm;
	std::vector<double> cr(static_cast<size_t>(n) + 1, 0.0);
	std::vector<double> ci(static_cast<size_t>(n) + 1, 0.0);
	for (int i = 0; i < n; ++i) {
		const double ph = kTwoPi * 440.0 * static_cast<double>(i + warm) / kRate;
		const double y = l[static_cast<size_t>(i + warm)];
		cr[static_cast<size_t>(i) + 1] = cr[static_cast<size_t>(i)] + y * std::cos(ph);
		ci[static_cast<size_t>(i) + 1] = ci[static_cast<size_t>(i)] - y * std::sin(ph);
	}
	double total = 0.0;
	double rest = 0.0;
	for (int i = kWindow / 2; i < n - kWindow / 2; ++i) {
		const auto lo = static_cast<size_t>(i - kWindow / 2);
		const auto hi = static_cast<size_t>(i + kWindow / 2);
		const double re = (cr[hi] - cr[lo]) / kWindow;
		const double im = (ci[hi] - ci[lo]) / kWindow;
		const double ph = kTwoPi * 440.0 * static_cast<double>(i + warm) / kRate;
		const double y = l[static_cast<size_t>(i + warm)];
		const double tone = 2.0 * (re * std::cos(ph) - im * std::sin(ph));
		total += y * y;
		rest += (y - tone) * (y - tone);
	}
	c.restDb = 10.0 * std::log10(std::max(1.0e-30, rest / total));
	return c;
}

double nsPerFrame(const Mover& move) {
	constexpr int kBlock = 256;
	constexpr int kBlocks = 48000 * 5 / kBlock;
	auto d = std::make_unique<dl::Delay>();
	d->prepare(kRate);
	d->setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5));
	const dl::Settings s = base(dl::kMaxTaps);
	d->setTarget(s);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	auto fill = [&]() {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
	};
	double total = 0.0;
	int at = 0;
	for (int b = -100; b < kBlocks; ++b) {
		fill();
		const auto t0 = std::chrono::steady_clock::now();
		for (int t = 0; t < kBlock; t += kTick) {
			dl::Settings m = s;
			move(m, at);
			d->setTarget(m);
			d->processStereo(l.data() + t, r.data() + t, kTick);
			at += kTick;
		}
		if (b >= 0) {
			total += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
		}
	}
	g_sink = l[0] + r[0];
	return total / (static_cast<double>(kBlocks) * kBlock);
}

double seconds(int at) { return static_cast<double>(at) / kRate; }

} // namespace

int main() {
	const Mover still = [](dl::Settings&, int) {};
	// A HAND: Shape from one end to the other in a second, then back.
	const Mover shapeSweep = [](dl::Settings& s, int at) {
		const double t = std::fmod(seconds(at), 2.0);
		s.tapPattern = dl::patternCoordForShape(static_cast<float>(t < 1.0 ? -1.0 + 2.0 * t : 3.0 - 2.0 * t));
	};
	const Mover shapeLfo = [](dl::Settings& s, int at) {
		s.tapPattern = dl::patternCoordForShape(static_cast<float>(std::sin(kTwoPi * 2.0 * seconds(at))));
	};
	const Mover shapeSlow = [](dl::Settings& s, int at) {
		s.tapPattern = dl::patternCoordForShape(static_cast<float>(0.3 * std::sin(kTwoPi * 0.2 * seconds(at))));
	};
	// ONE STEP A SECOND, 4 <-> 5: a hand clicking Taps.
	const Mover tapsStep = [](dl::Settings& s, int at) { s.taps = (at / 48000) % 2 == 0 ? 5 : 4; };
	const Mover tapsLfo = [](dl::Settings& s, int at) {
		s.taps = static_cast<int>(std::lround(5.0 + 3.0 * std::sin(kTwoPi * 0.5 * seconds(at))));
	};
	const Mover timeLfo = [](dl::Settings& s, int at) {
		s.timeMultiple = 2.0 * std::exp2(0.25 * std::sin(kTwoPi * 2.0 * seconds(at)));
	};

	std::printf("CLICKS (440 Hz sine, wet only, moved every %d frames; ratio = worst step / still worst step)\n", kTick);
	struct Row {
		const char* name;
		int taps;
		const Mover* move;
	};
	const Row rows[] = {
		{ "still, 8 taps", 8, &still },          { "Shape sweep, 4 taps", 4, &shapeSweep },
		{ "Shape sweep, 8 taps", 8, &shapeSweep }, { "Shape LFO 2 Hz, 8 taps", 8, &shapeLfo }, { "Shape LFO 0.2 Hz 30%, 8 taps", 8, &shapeSlow },
		{ "Taps 4<->5 each second", 4, &tapsStep }, { "Taps LFO 2..8, 0.5 Hz", 4, &tapsLfo },
		{ "Time LFO 2 Hz, 8 taps", 8, &timeLfo },
	};
	for (const Row& row : rows) {
		const Clicks c = clicks(row.taps, *row.move, 4);
		std::printf("  %-26s worst %6.2fx   clicks/s %8.1f   not-the-tone %6.1f dB\n", row.name, c.worstRatio, c.clicksPerSecond, c.restDb);
	}

	std::printf("COST (ns per stereo frame, 8 taps; medians of 9 alternating repetitions)\n");
	const Mover spreadStill = [](dl::Settings& s, int) { s.spread = 0.3f; };
	const Mover spreadShapeLfo = [&](dl::Settings& s, int at) {
		shapeLfo(s, at);
		s.spread = 0.3f;
	};
	const Mover* costRows[] = { &still, &shapeLfo, &tapsLfo, &spreadStill, &spreadShapeLfo };
	const char* costNames[] = { "static", "Shape LFO 2 Hz", "Taps LFO 0.5 Hz", "static, Spread 30%", "Shape LFO, Spread 30%" };
	constexpr int kCost = 5;
	std::vector<double> ns[kCost];
	for (int rep = 0; rep < 9; ++rep) {
		for (int k = 0; k < kCost; ++k) {
			const int at = (k + rep) % kCost;
			ns[at].push_back(nsPerFrame(*costRows[at]));
		}
	}
	for (int k = 0; k < kCost; ++k) {
		std::sort(ns[k].begin(), ns[k].end());
		std::printf("  %-22s %7.1f  (min %7.1f, max %7.1f)\n", costNames[k], ns[k][4], ns[k][0], ns[k][8]);
	}
	return 0;
}
