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

// THE DRAWER'S OPERATOR BENCHMARK — not a test (backlog LY, the drawer's cost). Where a control block's drawer time goes,
// for one voice at C3 (~179 partials): prepare() alone; prepare() + gains() with each amplitude operator ALONE at
// bench_render's `--drawer 1` setting, then all four together (that setting), with f0 moving every block (vibrato) so
// gains() does the full evaluation every block; the SAME four through the batch as it was at d1742ff4 (restated below),
// so old and new are compared inside one process; each kernel alone, and the product + Tilt's compensation alone; and
// GainCache, the voice's road, on a held note and with f0 moving. Printed in ns per partial per control block. The
// cases are ALTERNATED round by round and the median of the batch means is printed (CLAUDE.md, benchmarking on a shared
// host); the load average at both ends. Batches of 50 blocks, because one clock read costs ~1.4 us here
// (bench_partial_bank.cpp).

#include "dsp/spectral_shaper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using namespace feathers::spectral;
using clk = std::chrono::steady_clock;

volatile float gSink;
constexpr int kMax = 320;
constexpr int kVariants = 64;
constexpr int kBlocksPerBatch = 50;
constexpr int kBatchesPerRound = 4;
constexpr int kRounds = 25;

struct Block {
	double f0;
	int count;
	float hz[kMax];
	float base[kMax];
};

// The bench_render drawer (tests/bench_render.cpp, `--drawer 1`).
ShaperParams representative() {
	ShaperParams p;
	p.filter = {true, FilterMode::LowPass, 2500.0f, 0.6f, 0.4f, 0.5f};
	p.comb = {true, 3.0f, 0.7f, 0.2f};
	p.tilt = {true, -3.0f, 1000.0f};
	p.oddEven = {true, -0.4f};
	return p;
}

// THE BATCH AS IT WAS AT d1742ff4, restated, so the old road and the new run in the SAME process, alternated round by
// round: on this host a figure from another run is a figure from another load. Scalar doubles, libm per partial, every
// group every block, as the voice ran it. Only the operators bench_render's drawer turns on, and LowPass only.
namespace legacy {
constexpr double kDbPerOctaveOfUnitExponent = 6.020599913279624;
constexpr double kResonanceInvTwoWidthSq = 1.0 / (2.0 * kResonanceWidthOctaves * kResonanceWidthOctaves);
double floorHz(double hz) { return hz > kMinHz ? hz : kMinHz; }
double clamp01(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }
double atLeastOne(double v) { return v > 1.0 ? v : 1.0; }
float flushed(double g) { return g >= static_cast<double>(kFlushGain) ? static_cast<float>(g) : 0.0f; }

struct Prepared {
	bool filterOn, combOn, tiltOn, oddEvenOn;
	double log2Cutoff, twoOrder, resonanceAmp, combInvSpacing, combPhase, combDepth, tiltExponent, tiltLog2Pivot, oddGain, evenGain;
};

Prepared prepare(const ShaperParams& p, double f0Hz) {
	Prepared s {};
	s.filterOn = p.filter.on;
	const double track = std::pow(floorHz(f0Hz) / kKeyTrackPivotHz, clamp01(p.filter.keyTrack));
	s.log2Cutoff = std::log2(atLeastOne(atLeastOne(p.filter.cutoffHz) * track));
	s.twoOrder = 2.0 * (1.0 + 15.0 * clamp01(p.filter.slope));
	s.resonanceAmp = kMaxResonanceGain * clamp01(p.filter.resonance);
	s.combOn = p.comb.on;
	s.combInvSpacing = 1.0 / atLeastOne(p.comb.spacing);
	s.combPhase = p.comb.phase;
	s.combDepth = clamp01(p.comb.depth);
	s.tiltOn = p.tilt.on;
	s.tiltExponent = std::clamp(static_cast<double>(p.tilt.dbPerOctave), -kTiltMaxDbPerOctave, kTiltMaxDbPerOctave)
	                 / kDbPerOctaveOfUnitExponent;
	s.tiltLog2Pivot = std::log2(atLeastOne(p.tilt.pivotHz));
	s.oddEvenOn = p.oddEven.on;
	s.oddGain = oddEvenGain(p.oddEven, 1.0);
	s.evenGain = oddEvenGain(p.oddEven, 2.0);
	return s;
}

double gainWithoutTilt(const Prepared& s, double kk, double L) {
	double g = 1.0;
	if (s.filterOn) {
		const double d = L - s.log2Cutoff;
		const double y = std::exp2(std::clamp(s.twoOrder * d, -1000.0, 1000.0));
		double f = 1.0 / std::sqrt(1.0 + y);
		if (s.resonanceAmp > 0.0) {
			f += s.resonanceAmp * std::exp(-(d * d) * kResonanceInvTwoWidthSq);
		}
		g *= std::max(0.0, f);
	}
	if (s.combOn) {
		g *= 1.0 - s.combDepth * 0.5 * (1.0 + std::cos(2.0 * std::numbers::pi * (kk * s.combInvSpacing + s.combPhase)));
	}
	if (s.oddEvenOn) {
		g *= (std::llround(kk) % 2) != 0 ? s.oddGain : s.evenGain;
	}
	return g;
}

void gains(const Prepared& s, const float* k, const float* hz, const float* base, int count, float* out) {
	if (!s.tiltOn) {
		for (int i = 0; i < count; ++i) {
			out[i] = flushed(gainWithoutTilt(s, k[i], std::log2(floorHz(hz[i]))));
		}
		return;
	}
	double powerBefore = 0.0, powerAfter = 0.0;
	for (int i = 0; i < count; ++i) {
		const double L = std::log2(floorHz(hz[i]));
		const double g = gainWithoutTilt(s, k[i], L);
		const double t = std::exp2(s.tiltExponent * std::clamp(L - s.tiltLog2Pivot, -kTiltMaxOctaves, kTiltMaxOctaves));
		const double before = static_cast<double>(base[i]) * g;
		const double after = before * t;
		powerBefore += before * before;
		powerAfter += after * after;
		out[i] = static_cast<float>(g * t);
	}
	double c = std::sqrt(powerBefore / powerAfter);
	if (!(powerAfter > 0.0) || !std::isfinite(c)) {
		c = 1.0;
	}
	for (int i = 0; i < count; ++i) {
		out[i] = flushed(static_cast<double>(out[i]) * c);
	}
}
} // namespace legacy

enum class Kind {
	Legacy,    ///< the d1742ff4 batch (above), f0 moving every block: what every block cost before
	Prepare,   ///< prepare() alone
	Gains,     ///< prepare() + gains(): the uncached batch, f0 moving every block
	Harmonic,  ///< harmonicFactorsAtIndex alone (the kernel, no prepare)
	Filter,    ///< filterFactors alone
	Tilt,      ///< tiltFactors alone
	Product,   ///< factorProduct + compensate over cached factors (what a held note pays per side)
	CacheHeld, ///< GainCache update + apply, f0 and params still: the held note
	CacheGlide ///< GainCache update + apply, f0 moving every block: the filter's and Tilt's factors recompute
};

struct Case {
	const char* name;
	ShaperParams p;
	Kind kind;
	std::vector<double> ns; // per block
	GainCache<kMax>* cache = nullptr;
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

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

} // namespace

int main() {
	// C3 (130.8 Hz, ~179 partials below the fade at 48 kHz) with 5.5 Hz vibrato of 0.6%.
	static Block blocks[kVariants];
	static float k[kMax];
	for (int i = 0; i < kMax; ++i)
		k[i] = static_cast<float>(i + 1);
	for (int v = 0; v < kVariants; ++v) {
		Block& b = blocks[v];
		const double time = v * 32 / 48000.0;
		b.f0 = 130.8 * (1.0 + 0.006 * std::sin(2.0 * std::numbers::pi * 5.5 * time));
		b.count = std::min(kMax, static_cast<int>(0.98 * 24000.0 / b.f0));
		for (int i = 0; i < kMax; ++i) {
			b.hz[i] = static_cast<float>((i + 1) * b.f0);
			b.base[i] = 0.35f / static_cast<float>(i + 1);
		}
	}
	const ShaperParams all = representative();
	static GainCache<kMax> caches[2];
	std::vector<Case> cases;
	cases.push_back({"prepare() alone", all, Kind::Prepare, {}});
	auto alone = [&](const char* name, auto&& keep) {
		ShaperParams p = all;
		p.filter.on = p.comb.on = p.tilt.on = p.oddEven.on = false;
		keep(p);
		cases.push_back({name, p, Kind::Gains, {}});
	};
	alone("Filter LP res 0.6 alone", [&](ShaperParams& p) { p.filter.on = true; });
	alone("Filter LP res 0 alone", [&](ShaperParams& p) {
		p.filter.on = true;
		p.filter.resonance = 0.0f;
	});
	alone("Comb alone", [&](ShaperParams& p) { p.comb.on = true; });
	alone("Tilt alone", [&](ShaperParams& p) { p.tilt.on = true; });
	alone("Odd/Even alone", [&](ShaperParams& p) { p.oddEven.on = true; });
	cases.push_back({"all four (--drawer 1)", all, Kind::Gains, {}});
	cases.push_back({"all four, d1742ff4 batch", all, Kind::Legacy, {}});
	cases.push_back({"  kernel: harmonic group", all, Kind::Harmonic, {}});
	cases.push_back({"  kernel: filter", all, Kind::Filter, {}});
	cases.push_back({"  kernel: tilt", all, Kind::Tilt, {}});
	cases.push_back({"  product + compensation", all, Kind::Product, {}});
	cases.push_back({"GainCache, held note", all, Kind::CacheHeld, {}, &caches[0]});
	cases.push_back({"GainCache, f0 moving", all, Kind::CacheGlide, {}, &caches[1]});

	alignas(16) static float out[kMax];
	alignas(16) static float h[kMax];
	alignas(16) static double f[kMax];
	alignas(16) static float t[kMax];
	int variant = 0;
	const double loadBefore = loadAverage();
	for (int round = 0; round < kRounds; ++round) {
		for (Case& c : cases) {
			const PreparedShaper still = prepare(c.p, blocks[0].f0);
			for (int batch = 0; batch < kBatchesPerRound; ++batch) {
				const auto t0 = clk::now();
				for (int n = 0; n < kBlocksPerBatch; ++n) {
					const Block& b = blocks[variant];
					variant = (variant + 1) % kVariants;
					switch (c.kind) {
					case Kind::Legacy:
						legacy::gains(legacy::prepare(c.p, b.f0), k, b.hz, b.base, b.count, out);
						gSink = out[b.count / 2];
						break;
					case Kind::Prepare: gSink = static_cast<float>(prepare(c.p, b.f0).filter.log2Cutoff); break;
					case Kind::Gains:
						gains(prepare(c.p, b.f0), k, b.hz, b.base, b.count, out);
						gSink = out[b.count / 2];
						break;
					case Kind::Harmonic:
						harmonicFactorsAtIndex(still.harmonic, b.count, h);
						gSink = h[b.count / 2];
						break;
					case Kind::Filter:
						filterFactors(still.filter, b.hz, b.count, f);
						gSink = static_cast<float>(f[b.count / 2]);
						break;
					case Kind::Tilt:
						tiltFactors(still.tilt, b.hz, b.count, t);
						gSink = t[b.count / 2];
						break;
					case Kind::Product: {
						double before = 0.0, after = 0.0;
						factorProduct(h, f, t, nullptr, b.base, blocks[0].count, out, before, after);
						compensate(out, blocks[0].count, before, after);
						gSink = out[3];
						break;
					}
					case Kind::CacheHeld:
						c.cache->update(c.p, blocks[0].f0, blocks[0].hz, blocks[0].count);
						c.cache->apply(blocks[0].base, blocks[0].count, out);
						gSink = out[3];
						break;
					case Kind::CacheGlide:
						c.cache->update(c.p, b.f0, b.hz, b.count);
						c.cache->apply(b.base, b.count, out);
						gSink = out[3];
						break;
					}
				}
				const auto t1 = clk::now();
				c.ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / kBlocksPerBatch);
			}
		}
	}
	const double loadAfter = loadAverage();
	double meanCount = 0.0;
	for (const Block& b : blocks)
		meanCount += b.count;
	meanCount /= kVariants;
	std::printf("prepare() + gains(), one control block of one voice at C3 with vibrato (mean %.1f partials), %d blocks per "
	            "case, alternated in %d rounds\n",
	            meanCount, kRounds * kBatchesPerRound * kBlocksPerBatch, kRounds);
	std::printf("load average (1 min): %.2f before, %.2f after\n", loadBefore, loadAfter);
	for (const Case& c : cases) {
		const double m = median(c.ns);
		std::printf("%-28s %8.0f ns/block = %6.2f ns/partial\n", c.name, m, m / meanCount);
	}
	return 0;
}
