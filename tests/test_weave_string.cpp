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

// The weave RING (src/dsp/weave_string): its own tick clock, the physics' bounds, the excitation
// seam, and the scan. Energy, ticks and table values are read through the test seams — a claim
// about a value is asserted as the value, not inferred from a render.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/portable_trig.h"
#include "dsp/weave_string.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

//--------------------------------------------------------------------------------------------------//
// THE ALLOCATION COUNTER — the pattern of test_rack_engine.cpp: a replacement operator new for THIS  //
// TEST BINARY ONLY, counting while a case holds the window open.                                    //
//--------------------------------------------------------------------------------------------------//
namespace {
std::atomic<bool> g_countAllocs { false };
std::atomic<uint64_t> g_allocs { 0 };
struct AllocWindow {
	AllocWindow() {
		g_allocs.store(0);
		g_countAllocs.store(true);
	}
	~AllocWindow() { g_countAllocs.store(false); }
	uint64_t allocs() const { return g_allocs.load(); }
};
void noteAlloc() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	noteAlloc();
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return operator new(n); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t al) {
	noteAlloc();
	void* p = nullptr;
	const std::size_t a = static_cast<std::size_t>(al);
	if (feathers::test::heapPosixMemalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n == 0 ? 1 : n) != 0) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t al) { return operator new(n, al); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

namespace {

using owlet::dsp::buildPhiWeaveLaws;
using feathers::WeaveString;

constexpr double kSr = 44100.0;
// The B side of every morphing case: a fixed golden-section offset from A.
constexpr double kPhiOffset = 0.3819660112501051;

void setUp(WeaveString& s, double sr, double phaseA, double phaseB, float bowScale) {
	s.setLaws(buildPhiWeaveLaws(phaseA), buildPhiWeaveLaws(phaseB));
	WeaveString::Excitation e;
	e.bowScale = bowScale;
	s.setExcitation(e);
	s.prepare(sr);
}

// Advance whole windows, calling `perTick` after each tick.
template <typename F>
void runTicks(WeaveString& s, int ticks, F&& perTick) {
	for (int t = 0; t < ticks; ++t) {
		s.advance(s.samplesUntilTick());
		perTick(t);
	}
}

int ticksFor(double seconds) {
	return static_cast<int>(seconds * WeaveString::kTickHz);
}

// The caller contract: cut the host block at the tick, read each sample of the chunk at its offset,
// then advance. The crossfade follows the WINDOW index (a caller's parameters are sampled at the
// tick), and a pluck is requested at the chunk holding absolute sample kPluckAt.
std::vector<float> renderInBlocks(int block, int total) {
	constexpr int kPluckAt = 30011;
	WeaveString s;
	setUp(s, kSr, 0.23, 0.23 + kPhiOffset, 1.0f);
	std::vector<float> out;
	out.reserve(static_cast<size_t>(total));
	double phase = 0.0;
	const double inc = 110.0 / kSr;
	int done = 0;
	bool plucked = false;
	while (done < total) {
		int hostLeft = std::min(block, total - done);
		while (hostLeft > 0) {
			const int m = std::min(hostLeft, s.samplesUntilTick());
			const int window = done / s.tickLength();
			s.setCrossfade(0.5f + 0.5f * std::sin(static_cast<float>(window) * 0.01f));
			if (!plucked && done + m > kPluckAt) {
				s.pluck();
				plucked = true;
			}
			for (int k = 0; k < m; ++k) {
				out.push_back(s.sample(phase + s.travelPhaseAt(k), 0, k));
				phase += inc;
				phase -= std::floor(phase);
			}
			s.advance(m);
			done += m;
			hostLeft -= m;
		}
	}
	return out;
}

} // namespace

TEST_CASE("weave string: output is identical at any host block size") {
	const int total = static_cast<int>(2.0 * kSr);
	const std::vector<float> ref = renderInBlocks(1, total);
	float peak = 0.f;
	for (float v : ref) {
		peak = std::max(peak, std::abs(v));
	}
	REQUIRE(peak > 0.1f); // the comparison is not between two silences
	for (int block : { 7, 64, 441, 4096 }) {
		CAPTURE(block);
		const std::vector<float> got = renderInBlocks(block, total);
		REQUIRE(got.size() == ref.size());
		int firstDiff = -1;
		for (size_t i = 0; i < ref.size(); ++i) {
			if (got[i] != ref[i]) {
				firstDiff = static_cast<int>(i);
				break;
			}
		}
		CHECK(firstDiff == -1);
	}
}

TEST_CASE("weave string: the tick rate is the firmware's at any sample rate") {
	for (double sr : { 44100.0, 48000.0, 96000.0 }) {
		CAPTURE(sr);
		WeaveString s;
		setUp(s, sr, 0.3, 0.3, 1.0f);
		const int total = static_cast<int>(sr);
		// Odd-sized pieces, so the count cannot depend on the caller's alignment with the tick.
		for (int done = 0; done < total;) {
			const int m = std::min(37, total - done);
			s.advance(m);
			done += m;
		}
		CHECK(std::abs(static_cast<double>(s.ticksForTest()) - WeaveString::kTickHz) <= 1.0);
		// And exactly: one tick per whole window reached.
		CHECK(s.ticksForTest() == static_cast<uint64_t>(total / s.tickLength()));
	}
}

namespace {

// Crossfade swept 0 -> 1 -> 0 every 3 s (the strongest morph-bow a smoothed knob gives), the laws' own
// bow doubled, for 60 sweeps (three simulated minutes; the ten-minute version took up to a minute on a
// loaded host, 2026-09-26), split in two ctest entries by PHASE HALF — half h takes phases k = h, h + 2,
// ..., so both span the whole circle and all 64 phases are covered. The assertions:
//   - |x| <= 2 and |scan| <= 1 cannot fail while the clamps are in; they are here to fail when the
//     displacement clamp is taken out (the ring then reaches |x| 3.46 and stays finite, so nothing else
//     would see it);
//   - no growth: the MEAN kinetic energy (sum v^2) over the last three sweeps against sweeps 3..6
//     (9..18 s, past the start-up), at every phase;
//   - the velocity rail is never engaged here;
//   - the scan stays under 0.8, well inside the 1.0 ceiling (a loudness-control fault would cross it).
// MEASURED 2026-09-26: worst growth 1.43 (half 1) and 1.34 (half 2) against the bound 1.5, kept from the
// ten-minute case (whose peak-over-a-minute ratio read 1.13); worst |x| 2.0 (the rail reached); worst
// scan 0.59; velocity rail never engaged.
void boundedThreeMinutes(int half) {
	const int period = ticksFor(3.0);
	const int ticks = 60 * period;
	float worstX = 0.f;
	float worstScan = 0.f;
	double worstGrowth = 0.0;
	for (int k = half; k < 64; k += 2) {
		CAPTURE(k);
		const double phase = static_cast<double>(k) / 64.0;
		WeaveString s;
		setUp(s, kSr, phase, phase + kPhiOffset, 2.0f);
		bool finite = true;
		float scanPeak = 0.f;
		double early = 0.0;
		double late = 0.0;
		runTicks(s, ticks, [&](int t) {
			const int inPeriod = t % period;
			const float tri = static_cast<float>(inPeriod) / static_cast<float>(period);
			s.setCrossfade(tri < 0.5f ? 2.0f * tri : 2.0f - 2.0f * tri);
			const float* x = s.nodesForTest();
			const float* v = s.velocitiesForTest();
			double kinetic = 0.0;
			for (int i = 0; i < WeaveString::kNodes; ++i) {
				if (!std::isfinite(x[i]) || std::abs(x[i]) > 2.0f) {
					finite = false;
				}
				worstX = std::max(worstX, std::abs(x[i]));
				kinetic += static_cast<double>(v[i]) * v[i];
			}
			if (t >= 3 * period && t < 6 * period) {
				early += kinetic; // summed: both windows are three sweeps long, so the ratio is of means
			}
			if (t >= ticks - 3 * period) {
				late += kinetic;
			}
			for (int off : { 0, s.tickLength() / 2, s.tickLength() - 1 }) {
				for (double p : { 0.01, 0.27, 0.5, 0.77 }) {
					const float val = s.sample(p, 0, off);
					if (!std::isfinite(val)) {
						finite = false;
					}
					scanPeak = std::max(scanPeak, std::abs(val));
				}
			}
		});
		REQUIRE(finite);
		CHECK(scanPeak < 0.8f);
		CHECK(s.velocityRailHitsForTest() == 0);
		REQUIRE(early > 0.0);
		CHECK(late < 1.5 * early);
		worstGrowth = std::max(worstGrowth, late / early);
		worstScan = std::max(worstScan, scanPeak);
	}
	MESSAGE("half " << half + 1 << ": worst |x| = " << worstX << ", worst |scan| = " << worstScan
	                << ", worst last / early mean kinetic energy = " << worstGrowth);
}

} // namespace

// Two ctest entries (tests/CMakeLists.txt names them); the names carry no comma.
TEST_CASE("weave string: bounded for three simulated minutes over phase half 1 of 2") {
	boundedThreeMinutes(0);
}
TEST_CASE("weave string: bounded for three simulated minutes over phase half 2 of 2") {
	boundedThreeMinutes(1);
}

TEST_CASE("weave string: at default excitation the displacement rail almost never engages") {
	// The laws' own bow (scale 1), the crossfade swept slowly (0 -> 1 -> 0 over 20 s, a hand on a knob)
	// and a pluck every 2 s, at every 64th phase for a minute. The rail is a safety net here, not part
	// of the sound: MEASURED 2026-09-25, the displacement rail engaged on 707 of 84.7 M node-sub-steps
	// (8.4e-6), the velocity rail on 18 (2.1e-7). The bounds are fault detectors — a ring running
	// hot enough to lean on its rail would cross it by orders of magnitude.
	const int ticks = ticksFor(60.0);
	const int period = ticksFor(20.0);
	const int pluckEvery = ticksFor(2.0);
	uint64_t hits = 0;
	uint64_t vHits = 0;
	uint64_t nodeSteps = 0;
	for (int k = 0; k < 64; ++k) {
		WeaveString s;
		setUp(s, kSr, static_cast<double>(k) / 64.0, static_cast<double>(k) / 64.0 + kPhiOffset, 1.0f);
		runTicks(s, ticks, [&](int t) {
			if (t % pluckEvery == 0) {
				s.pluck();
			}
			const float tri = static_cast<float>(t % period) / static_cast<float>(period);
			s.setCrossfade(tri < 0.5f ? 2.0f * tri : 2.0f - 2.0f * tri);
		});
		hits += s.displacementRailHitsForTest();
		vHits += s.velocityRailHitsForTest();
		nodeSteps += static_cast<uint64_t>(ticks) * 2u * WeaveString::kNodes;
	}
	const double fraction = static_cast<double>(hits) / static_cast<double>(nodeSteps);
	MESSAGE("displacement rail: " << hits << " of " << nodeSteps << " node-sub-steps (" << fraction
	                              << "); velocity rail: " << vHits);
	CHECK(fraction < 1e-4);
	CHECK(static_cast<double>(vHits) / static_cast<double>(nodeSteps) < 1e-5);
}

TEST_CASE("weave string: one advance() spanning several ticks equals the same span in pieces") {
	WeaveString whole;
	WeaveString pieces;
	for (WeaveString* s : { &whole, &pieces }) {
		setUp(*s, kSr, 0.61, 0.61 + kPhiOffset, 1.5f);
		s->setCrossfade(0.3f);
		s->advance(50); // start mid-window, so the span begins and ends off a boundary
	}
	whole.advance(1000);
	for (int n = 0; n < 1000; ++n) {
		pieces.advance(1);
	}
	REQUIRE(whole.ticksForTest() == pieces.ticksForTest());
	CHECK(whole.ticksForTest() == static_cast<uint64_t>((50 + 1000) / whole.tickLength()));
	CHECK(whole.samplesUntilTick() == pieces.samplesUntilTick());
	int differ = 0;
	for (int i = 0; i < WeaveString::kNodes; ++i) {
		differ += (whole.nodesForTest()[i] != pieces.nodesForTest()[i]) ? 1 : 0;
		differ += (whole.smoothedNodes()[i] != pieces.smoothedNodes()[i]) ? 1 : 0;
	}
	CHECK(differ == 0);
	for (int k : { 0, 5, whole.samplesUntilTick() - 1 }) {
		CHECK(whole.sample(0.37, 0, k) == pieces.sample(0.37, 0, k));
		CHECK(whole.travelPhaseAt(k) == pieces.travelPhaseAt(k));
	}
}

TEST_CASE("weave string: a pluck adds energy and it decays") {
	WeaveString s;
	setUp(s, kSr, 0.3, 0.3, 0.0f);
	runTicks(s, ticksFor(5.0), [](int) {});
	const float e0 = s.energyForTest();
	s.pluck();
	s.advance(s.samplesUntilTick());
	const float e1 = s.energyForTest();
	MESSAGE("E0 = " << e0 << ", after one tick = " << e1);
	// MEASURED 2026-09-25 at phase 0.3: E0 7.1e-9, 0.0756 one tick after the pluck, peak 0.0775,
	// 4.0e-6 three seconds later. The margin is about half the measured jump.
	CHECK(e1 > e0 + 0.04f);
	float peak = e1;
	runTicks(s, ticksFor(0.2), [&](int) { peak = std::max(peak, s.energyForTest()); });
	runTicks(s, ticksFor(2.8), [](int) {});
	MESSAGE("post-pluck peak = " << peak << ", 3 s later = " << s.energyForTest());
	CHECK(s.energyForTest() < 0.1f * peak);
}

TEST_CASE("weave string: the rest pose is the ring's equilibrium, not its home shape") {
	for (double phase : { 0.0, 0.3, 0.618 }) {
		CAPTURE(phase);
		const owlet::dsp::PhiWeaveParams p = buildPhiWeaveLaws(phase);
		float rest[WeaveString::kNodes];
		WeaveString::restPose(p, rest);
		// The equation, restated: no net spring force at any node.
		float residual = 0.f;
		float fromHome = 0.f;
		for (int i = 0; i < WeaveString::kNodes; ++i) {
			const float xm = rest[(i + 31) % 32];
			const float xp = rest[(i + 1) % 32];
			residual = std::max(residual, std::abs(p.coupling[i] * (xm + xp - 2.0f * rest[i])
			                                       - p.stiffness[i] * (rest[i] - p.home[i])));
			fromHome = std::max(fromHome, std::abs(rest[i] - p.home[i]));
		}
		CHECK(residual < 1e-6f);
		MESSAGE("largest rest-pose departure from home: " << fromHome);
		// MEASURED 2026-09-25: 0.250 at phase 0, 0.464 at 0.3, 0.050 at 0.618.
		CHECK(fromHome > 0.02f); // it is not home, or this case is vacuous

		// And the physics agrees: plucked, unbowed and left 20 s, the ring comes back to it.
		WeaveString s;
		setUp(s, kSr, phase, phase, 0.0f);
		runTicks(s, ticksFor(20.0), [](int) {});
		float off = 0.f;
		for (int i = 0; i < WeaveString::kNodes; ++i) {
			off = std::max(off, std::abs(s.nodesForTest()[i] - rest[i]));
		}
		CHECK(off < 1e-3f);
	}
}

TEST_CASE("weave string: a parked crossfade with no bow leaves the string at rest") {
	WeaveString s;
	setUp(s, kSr, 0.3, 0.3 + kPhiOffset, 0.0f);
	s.setCrossfade(0.4f);
	s.reset();
	s.pluck(0.0f); // replaces reset's pending pluck with an empty one
	float worst = 0.f;
	runTicks(s, ticksFor(5.0), [&](int) { worst = std::max(worst, s.energyForTest()); });
	CHECK(worst < 1e-9f);
}

TEST_CASE("weave string: crossfade motion bows the string") {
	// The SAME laws on both sides, so the crossfade moves no law and no home shape: whatever energy
	// appears is the morph-bow's alone.
	auto sweepPeak = [](double seconds) {
		WeaveString s;
		setUp(s, kSr, 0.3, 0.3, 0.0f);
		s.pluck(0.0f);
		float parked = 0.f;
		runTicks(s, ticksFor(2.0), [&](int) { parked = std::max(parked, s.energyForTest()); });
		REQUIRE(parked < 1e-9f);
		const int n = ticksFor(seconds);
		float peak = 0.f;
		runTicks(s, n, [&](int t) {
			s.setCrossfade(static_cast<float>(t + 1) / static_cast<float>(n));
			peak = std::max(peak, s.energyForTest());
		});
		return peak;
	};
	const float slow = sweepPeak(2.0);
	const float fast = sweepPeak(0.25);
	MESSAGE("morph-bow energy peak: 2 s sweep = " << slow << ", 0.25 s sweep = " << fast);
	// MEASURED 2026-09-25 at phase 0.3: peak 2.9e-6 over a 2 s sweep, 5.0e-5 over 0.25 s, against
	// under 1e-9 parked. The morph-bow is a gentle agitation by design (noise x speed x gain x 0.02).
	CHECK(slow > 1e-6f);
	CHECK(fast > slow);
}

TEST_CASE("weave string: the scan reads the smoothed ring, not the raw one") {
	WeaveString s;
	setUp(s, kSr, 0.3, 0.3, 0.0f);
	s.pluck(0.0f);
	runTicks(s, 10, [](int) {});
	s.pluck(1.0f, 0.4f);
	runTicks(s, 3, [](int) {});

	// Three ticks after a pluck the raw ring has jumped and the smoother is still catching up, so the
	// raw and smoothed rings are measurably different — without that this case could not tell them apart.
	const float* x = s.nodesForTest();
	const float* xs = s.smoothedNodes();
	float apart = 0.f;
	for (int i = 0; i < WeaveString::kNodes; ++i) {
		apart = std::max(apart, std::abs(x[i] - xs[i]));
	}
	REQUIRE(apart > 0.05f);

	// On the window's last sample the fade has reached cur, whose entries are the smoothed ring times
	// the AGC scale, clamped to full scale; mip 1 is its circular [1 2 1]/4.
	const int last = s.samplesUntilTick() - 1;
	const float scale = s.scaleForTest();
	auto node0 = [&](int i) {
		i = (i + WeaveString::kNodes) % WeaveString::kNodes;
		return std::clamp(xs[i] * scale, -1.0f, 1.0f);
	};
	for (int i = 0; i < WeaveString::kNodes; ++i) {
		CAPTURE(i);
		const double phase = static_cast<double>(i) / WeaveString::kNodes;
		CHECK(std::abs(s.sample(phase, 0, last) - node0(i)) < 1e-6f);
		const float mip1 = 0.25f * (node0(i - 1) + 2.0f * node0(i) + node0(i + 1));
		CHECK(std::abs(s.sample(phase, 1, last) - mip1) < 1e-6f);
	}
	// Half-way between two nodes the scan is their mean (linear, as the firmware's final scan).
	CHECK(std::abs(s.sample(4.5 / WeaveString::kNodes, 0, last) - 0.5f * (node0(4) + node0(5))) < 1e-6f);
}

TEST_CASE("weave string: node motion is C1 at every fade join") {
	// A lively, bowed and plucked ring read at one fixed scan position: the signal is one point's
	// motion across the windows. Each fade segment is smoothstep-shaped, so the slope at every join
	// (the tick boundary, and the half-window) is near zero — at most 2/len of the steepest slope
	// anywhere. A linear fade would put the full segment slope at the join.
	WeaveString s;
	setUp(s, kSr, 0.61, 0.61, 2.0f);
	const int L = s.tickLength();
	const int half = L >> 1;
	std::vector<float> sig;
	std::vector<int> posInWindow;
	s.pluck(1.0f, 0.2f);
	for (int t = 0; t < ticksFor(2.0); ++t) {
		for (int k = 0; k < L; ++k) {
			sig.push_back(s.sample(5.3 / WeaveString::kNodes, 0, k));
			posInWindow.push_back(k);
		}
		s.advance(L);
	}
	float maxSlope = 0.f;
	float maxJoinSlope = 0.f;
	for (size_t n = 1; n < sig.size(); ++n) {
		const float d = std::abs(sig[n] - sig[n - 1]);
		maxSlope = std::max(maxSlope, d);
		if (posInWindow[n] == 0 || posInWindow[n] == half) {
			maxJoinSlope = std::max(maxJoinSlope, d);
		}
	}
	// MEASURED 2026-09-25 at 44.1 kHz (64-sample segments): the steepest join is 0.031 of the steepest
	// slope, the 2/len smoothstep predicts.
	MESSAGE("steepest slope " << maxSlope << ", steepest at a join " << maxJoinSlope);
	REQUIRE(maxSlope > 1e-4f);
	CHECK(maxJoinSlope < 0.1f * maxSlope);
}

TEST_CASE("weave string: the AGC lands every 64th phase inside a measured loudness window") {
	// Loudness as the RMS of the cur mip-0 table (the scanned cycle's nodes), averaged over seconds 5-10
	// with the laws' own bow and a parked crossfade. MEASURED 2026-09-25: 0.174 (phase 37/64) to 0.328
	// (phase 0) — a 5.5 dB spread — and the table never passed 0.59 of full scale. The window is that
	// measurement with a little room, not a model's prediction; the AGC's own targets (RMS 0.55 of the
	// 0.5 reference, peak at the reference) put the ceiling of it near 0.275 x outGain.
	float lo = 1e9f;
	float hi = 0.f;
	for (int k = 0; k < 64; ++k) {
		CAPTURE(k);
		const double phase = static_cast<double>(k) / 64.0;
		WeaveString s;
		setUp(s, kSr, phase, phase, 1.0f);
		double sum = 0.0;
		int n = 0;
		runTicks(s, ticksFor(10.0), [&](int t) {
			if (t < ticksFor(5.0)) {
				return;
			}
			float ss = 0.f;
			for (int i = 0; i < WeaveString::kNodes; ++i) {
				ss += s.tableForTest(0, i) * s.tableForTest(0, i);
			}
			sum += std::sqrt(ss / WeaveString::kNodes);
			++n;
		});
		const float rms = static_cast<float>(sum / n);
		lo = std::min(lo, rms);
		hi = std::max(hi, rms);
		CHECK(rms > 0.15f);
		CHECK(rms < 0.36f);
	}
	MESSAGE("table RMS across phases: " << lo << " .. " << hi << " (" << 20.f * std::log10(hi / lo) << " dB)");
	CHECK(20.f * std::log10(hi / lo) < 6.5f);
}

TEST_CASE("weave string: non-finite excitation and crossfade leave the string finite") {
	// A host drives bowScale from audio. inf x 0 is NaN at every node outside the bow window, std::clamp
	// passes NaN through, and a NaN node stays NaN until reset() — so the setters must refuse it.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	WeaveString s;
	setUp(s, kSr, 0.3, 0.3 + kPhiOffset, 1.0f);
	const float bad[] = { nan, inf, -inf, 1e30f, -1e30f };
	bool finite = true;
	for (float v : bad) {
		WeaveString::Excitation e;
		e.bowScale = v;
		s.setExcitation(e);
		s.setCrossfade(v);
		s.pluck(v, v);
		runTicks(s, 50, [&](int) {
			for (int i = 0; i < s.nodeCount(); ++i) {
				finite = finite && std::isfinite(s.nodesForTest()[i]);
			}
			for (int k : { 0, s.tickLength() - 1 }) {
				finite = finite && std::isfinite(s.sample(0.3, 0, k)) && std::isfinite(s.travelPhaseAt(k));
			}
		});
		s.pluck(0.5f, v);
		s.pluck(v, 0.25f);
		runTicks(s, 50, [&](int) {
			for (int i = 0; i < s.nodeCount(); ++i) {
				finite = finite && std::isfinite(s.nodesForTest()[i]);
			}
		});
	}
	CHECK(finite);
	// A non-finite scan phase reads phase 0 rather than garbage.
	CHECK(s.sample(static_cast<double>(nan), 0, 0) == s.sample(0.0, 0, 0));
	CHECK(s.sample(static_cast<double>(inf), 0, 0) == s.sample(0.0, 0, 0));
}

TEST_CASE("weave string: travel runs continuously through a wrap at a negative rate") {
	// Phase 0's travel triangle is negative (travelRate = t^3 * 0.004 with t = -0.77), so starting at 0
	// the rotation wraps to just under 1 at the first tick. Across every window the reader's travel must
	// (a) end exactly where the sub-steps left it and (b) never step more than |rate| / len per sample —
	// including across the wrap and the window boundary. A wrap MUST be seen, or the case is vacuous.
	WeaveString s;
	setUp(s, kSr, 0.0, 0.0, 1.0f);
	const float rate = s.travelRateForTest();
	CAPTURE(rate);
	REQUIRE(rate < -1e-4f);
	const int len = s.tickLength();
	const double bound = std::abs(rate) / len * (1.0 + 1e-3) + 1e-7;
	auto wrapDiff = [](double d) { return d - std::round(d); };
	double worstStep = 0.0;
	bool sawWrap = false;
	float prev = s.travelPhaseAt(0);
	for (int t = 0; t < 400; ++t) {
		for (int k = 0; k < len; ++k) {
			const float now = s.travelPhaseAt(k);
			if (std::abs(static_cast<double>(now) - prev) > 0.5) {
				sawWrap = true;
			}
			worstStep = std::max(worstStep, std::abs(wrapDiff(static_cast<double>(now) - prev)));
			prev = now;
		}
		// The window's last sample is where the two sub-steps of the tick that began it left the
		// rotation (the post-tick travel phase).
		CHECK(std::abs(wrapDiff(static_cast<double>(prev) - s.travelPhaseForTest())) < 1e-6);
		s.advance(len);
	}
	MESSAGE("travel rate " << rate << " per tick; worst per-sample step " << worstStep << " (bound " << bound << ")");
	CHECK(sawWrap);
	CHECK(worstStep <= bound);
}

TEST_CASE("weave string: the fade joins land exactly where the segments meet") {
	// At a window's half-way join the prev -> mid segment has reached t = smoothstep(1) = 1, and its
	// first sample sits at t = smoothstep(1 / half). Read at node positions (no spatial blend), the scan
	// must equal those two restatements exactly.
	WeaveString s;
	setUp(s, kSr, 0.61, 0.61 + kPhiOffset, 1.5f);
	runTicks(s, 37, [&](int t) { s.setCrossfade(0.01f * static_cast<float>(t % 50)); });
	const int half = s.tickLength() / 2;
	const float t0 = (1.0f / static_cast<float>(half)) * (1.0f / static_cast<float>(half))
	                 * (3.0f - 2.0f * (1.0f / static_cast<float>(half)));
	float worstFromMid = 0.f;
	for (int mip = 0; mip < 3; ++mip) {
		for (int i = 0; i < WeaveString::kNodes; ++i) {
			CAPTURE(mip);
			CAPTURE(i);
			const double phase = static_cast<double>(i) / WeaveString::kNodes;
			const float prev = s.tableSetForTest(0, mip, i);
			const float mid = s.tableSetForTest(1, mip, i);
			CHECK(s.sample(phase, mip, half - 1) == prev + (mid - prev) * 1.0f);
			CHECK(s.sample(phase, mip, 0) == prev + (mid - prev) * t0);
			worstFromMid = std::max(worstFromMid, std::abs(s.sample(phase, mip, half - 1) - mid));
		}
	}
	// prev + (mid - prev) is mid itself up to one rounding: say how close.
	MESSAGE("half-way join against mid itself: worst difference " << worstFromMid);
	CHECK(worstFromMid < 1e-7f);
}

TEST_CASE("weave string: a tick of one or two samples still ticks and fades correctly") {
	for (int want : { 1, 2 }) {
		CAPTURE(want);
		WeaveString s;
		setUp(s, WeaveString::kTickHz * want, 0.3, 0.3, 1.0f);
		REQUIRE(s.tickLength() == want);
		// Length 1: the window is all second segment, t = smoothstep(1/1) = 1. Length 2: one sample in
		// each segment, both at t = 1.
		for (int k = 0; k < want; ++k) {
			const WeaveString::Fade f = s.fadeAt(k);
			CHECK(f.segment == ((want == 1) ? 1 : k));
			CHECK(f.t == 1.0f);
		}
		const int total = 1000 * want + 1;
		bool finite = true;
		for (int n = 0; n < total; ++n) {
			finite = finite && std::isfinite(s.sample(0.3, 0, 0));
			s.advance(1);
		}
		CHECK(finite);
		CHECK(s.ticksForTest() == static_cast<uint64_t>(total / want));
	}
}

TEST_CASE("weave string: mip thresholds are 700 and 1800 Hz") {
	CHECK(WeaveString::mipForFrequency(110.0) == 0);
	CHECK(WeaveString::mipForFrequency(699.9) == 0);
	CHECK(WeaveString::mipForFrequency(700.1) == 1);
	CHECK(WeaveString::mipForFrequency(1799.9) == 1);
	CHECK(WeaveString::mipForFrequency(1800.1) == 2);
	CHECK(WeaveString::mipForFrequency(8000.0) == 2);
}

TEST_CASE("weave string: no allocation after prepare") {
	WeaveString s;
	setUp(s, kSr, 0.3, 0.3 + kPhiOffset, 1.0f);
	const owlet::dsp::PhiWeaveParams a = buildPhiWeaveLaws(0.7);
	const owlet::dsp::PhiWeaveParams b = buildPhiWeaveLaws(0.1);
	float sink = 0.f;
	uint64_t allocs = 0;
	{
		AllocWindow w;
		for (int t = 0; t < 400; ++t) {
			if (t == 100) {
				s.setLaws(a, b);
			}
			if (t % 50 == 0) {
				s.pluck();
				WeaveString::Excitation e;
				e.bowScale = 0.5f + 0.25f * static_cast<float>(t % 7);
				s.setExcitation(e);
			}
			s.setCrossfade(static_cast<float>(t % 100) / 100.f);
			const int m = s.samplesUntilTick();
			for (int k = 0; k < m; ++k) {
				sink += s.sample(0.3 + s.travelPhaseAt(k), t % 3, k);
			}
			s.advance(m);
		}
		allocs = w.allocs();
	}
	CHECK(std::isfinite(sink));
	CHECK(allocs == 0);
}

TEST_CASE("weave string: the windows' sine and cosine are the correctly rounded float, from no C library") {
	// The ring's bow, pluck and line windows integrate their trig, so a C library's cosf rounding one value the other way
	// rendered a different Resonate on macOS (dsp/portable_trig.h). portableSin/Cos are pure double arithmetic; here they
	// are held to the correctly rounded float over [-2 pi, 2 pi] (20001 points, and every float of a window's raised-cosine
	// argument near 0, pi/2 and pi), against long double where it is wider than double. Where long double IS double
	// (arm64 macOS) the reference itself can round a hair-close case the other way, so there a 1-ulp gap is allowed.
	// Known-bad: glibc's cosf in portableCos's place, 270 values 1 ulp off (a polynomial one term short is NOT one: r^15/15! is
	// 2e-14, far under a float's ulp).
	constexpr double kPi = 3.14159265358979323846;
	const bool exactRef = LDBL_MANT_DIG > 53;
	const auto ulps = [](float a, float b) {
		return a == b ? 0 : (std::nextafter(a, b) == b ? 1 : 2); // 2: two or more
	};
	int worst = 0;
	int off = 0;
	const auto check = [&](float x) {
		const float c = static_cast<float>(std::cos(static_cast<long double>(x)));
		const float s = static_cast<float>(std::sin(static_cast<long double>(x)));
		const int dc = ulps(feathers::dsp::portableCos(x), c);
		const int ds = ulps(feathers::dsp::portableSin(x), s);
		worst = std::max({ worst, dc, ds });
		off += (dc > 0) + (ds > 0);
	};
	for (int i = 0; i <= 20000; ++i) {
		check(static_cast<float>(kPi * (-2.0 + 4.0 * i / 20000.0)));
	}
	for (const float centre : { 0.0f, 1.5707964f, 3.1415927f }) {
		float x = centre;
		for (int i = 0; i < 4096; ++i) {
			x = std::nextafter(x, -10.0f);
		}
		for (int i = 0; i < 8192; ++i, x = std::nextafter(x, 10.0f)) {
			check(x);
		}
	}
	MESSAGE("portable sine and cosine: " << off << " values off the reference, the worst by " << worst << " ulp (long double " << LDBL_MANT_DIG << " bits)");
	CHECK(worst <= (exactRef ? 0 : 1));
}
