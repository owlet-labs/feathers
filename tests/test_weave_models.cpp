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

// THE WEAVE PHYSICS MODELS (src/dsp/weave_models.h): Bar, Tube, Membrane and Stiffening beside the Ring
// they generalise. The stencils, the paths and the equilibria are RESTATED here from their definitions —
// the neighbour sums written out — and compared against the string's own state through its test seams,
// so a slip in the model code shows as a value. That the Ring itself is unchanged is
// test_weave_ring_pin's job.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/weave_models.h"
#include "dsp/weave_string.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
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
using feathers::WeaveModel;
using feathers::WeaveString;
namespace weave = feathers::weave;

constexpr double kSr = 44100.0;
constexpr double kPhiOffset = 0.3819660112501051;
// Every model, from the count: a sixth model joins every loop below without an edit here.
constexpr std::array<WeaveModel, feathers::kWeaveModelCount> makeAll() {
	std::array<WeaveModel, feathers::kWeaveModelCount> all {};
	for (int i = 0; i < feathers::kWeaveModelCount; ++i) {
		all[static_cast<size_t>(i)] = static_cast<WeaveModel>(i);
	}
	return all;
}
constexpr std::array<WeaveModel, feathers::kWeaveModelCount> kAll = makeAll();
constexpr int kGrid = 8;

void setUp(WeaveString& s, WeaveModel m, double phaseA, double phaseB, float bowScale) {
	s.setModel(m);
	s.setLaws(buildPhiWeaveLaws(phaseA), buildPhiWeaveLaws(phaseB));
	WeaveString::Excitation e;
	e.bowScale = bowScale;
	s.setExcitation(e);
	s.prepare(kSr);
}

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

float triangle01(int t, int period) {
	const float tri = static_cast<float>(t % period) / static_cast<float>(period);
	return tri < 0.5f ? 2.0f * tri : 2.0f - 2.0f * tri;
}

// THE STENCILS, RESTATED: the elastic force each model's definition says node i feels, from x and the
// per-node gain, written out neighbour by neighbour (weave_models.h names the models).
double restatedForce(WeaveModel m, const float* x, const float* g, int i) {
	auto at = [&](int j) -> double { return static_cast<double>(x[j]); };
	switch (m) {
	case WeaveModel::Ring:
	case WeaveModel::Stiffening: {
		const double xm = at((i + 31) % 32);
		const double xp = at((i + 1) % 32);
		double f = g[i] * (xm + xp - 2.0 * at(i));
		if (m == WeaveModel::Stiffening) {
			const double dp = xp - at(i);
			const double dm = at(i) - xm;
			f += feathers::kBetaOverCoupling * g[i] * (dp * dp * dp - dm * dm * dm);
		}
		return f;
	}
	case WeaveModel::Tube: {
		const double xm = (i > 0) ? at(i - 1) : 0.0; // held at both ends
		const double xp = (i < 31) ? at(i + 1) : 0.0;
		return g[i] * (xm + xp - 2.0 * at(i));
	}
	case WeaveModel::Bar: {
		// Moment at an interior node j: g_j (x[j-1] - 2x[j] + x[j+1]); none at the free ends.
		auto moment = [&](int j) -> double {
			if (j < 1 || j > 30) {
				return 0.0;
			}
			return g[j] * (at(j - 1) - 2.0 * at(j) + at(j + 1));
		};
		return -(moment(i - 1) - 2.0 * moment(i) + moment(i + 1));
	}
	case WeaveModel::Membrane: {
		const int col = i % kGrid;
		const int row = i / kGrid;
		double sum = 0.0; // held at the rim: a missing neighbour is zero
		sum += (col > 0) ? at(i - 1) : 0.0;
		sum += (col < kGrid - 1) ? at(i + 1) : 0.0;
		sum += (row > 0) ? at(i - kGrid) : 0.0;
		sum += (row < kGrid - 1) ? at(i + kGrid) : 0.0;
		return g[i] * (sum - 4.0 * at(i));
	}
	}
	return 0.0;
}

// Gershgorin bound on the per-tick stiffness operator diag(k) + K, K restated from the stencil by
// probing it with unit vectors (K's column j is minus the force of the unit displacement e_j).
double gershgorin(WeaveModel m, const float* k, const float* g, int n) {
	std::vector<double> rows(static_cast<size_t>(n), 0.0);
	float e[feathers::kWeaveMaxNodes] = {};
	for (int j = 0; j < n; ++j) {
		e[j] = 1.0f;
		for (int i = 0; i < n; ++i) {
			const double kij = -restatedForce(m, e, g, i) + ((i == j) ? static_cast<double>(k[i]) : 0.0);
			rows[static_cast<size_t>(i)] += std::abs(kij);
		}
		e[j] = 0.0f;
	}
	return *std::max_element(rows.begin(), rows.end());
}

} // namespace

TEST_CASE("weave models: every stencil's stability bound holds across 4096 phases") {
	// Each stencil's gain is scaled so its Gershgorin bound is the Ring's own k + 4c (weave_models.h), and
	// the tested bound is the firmware's 4 (this integrator's true edge is 16 - 4d). Stiffening's LINEAR
	// part is the Ring's and is checked here; its cubic depends on the stretch, so it is checked against the
	// true edge at the stretches a string reaches, in the case below. MEASURED 2026-09-25, worst over the
	// sweep: 1.816 (Ring, Tube, Stiffening's linear part), 1.712 (Bar, its home spring at 0.1), 1.734
	// (Membrane).
	for (WeaveModel m : kAll) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		double worst = 0.0;
		for (int s = 0; s < 4096; s += 1) {
			WeaveString w;
			setUp(w, m, s / 4096.0, s / 4096.0, 1.0f);
			const int n = w.nodeCount();
			// Stiffening probed as its linear part, the Ring's stencil (a unit probe would feed the cubic).
			const WeaveModel linear = (m == WeaveModel::Stiffening) ? WeaveModel::Ring : m;
			worst = std::max(worst, gershgorin(linear, w.nodeStiffnessForTest(), w.nodeGainForTest(), n));
		}
		MESSAGE(std::string(weave::modelName(m)) << ": worst Gershgorin bound " << worst);
		CHECK(worst < 4.0);
	}
}

TEST_CASE("weave models: Stiffening stays inside this integrator's true edge at the stretches it reaches") {
	// The cubic linearises, at node i, to a Gershgorin row of k + 4c + 6 beta (dp^2 + dm^2) (dp, dm its
	// neighbour differences) — the linear row plus 3 beta dp^2 + 3 beta dm^2 twice over, diagonal and
	// off-diagonal. Driven hard (bow x4, crossfade swept every 3 s, a minute at every 64th phase), that row
	// at every node and tick is measured against the half-dt integrator's TRUE edge 16 - 4d at that node
	// (weave_models.h derives it), and must stay 3 inside it. MEASURED 2026-09-26 at beta = 8c: worst
	// margin 6.26; largest neighbour difference
	// 0.98. At c/4 the margin was 14.1; at 16c it went to -1.9, past the edge, which is why 16c was not
	// the choice. The linear models are held to the conservative 4 in the case above: their stiffness
	// does not grow with the stretch, so they have no need of the true edge's room.
	const int ticks = ticksFor(60.0);
	const int period = ticksFor(3.0);
	double worstMargin = 1e9;
	double largestStretch = 0.0;
	for (int k = 0; k < 64; ++k) {
		CAPTURE(k);
		WeaveString w;
		setUp(w, WeaveModel::Stiffening, k / 64.0, k / 64.0 + kPhiOffset, 4.0f);
		runTicks(w, ticks, [&](int t) {
			w.setCrossfade(triangle01(t, period));
			const float* x = w.nodesForTest();
			for (int i = 0; i < 32; ++i) {
				const double dp = x[(i + 1) % 32] - x[i];
				const double dm = x[i] - x[(i + 31) % 32];
				const double c = w.nodeGainForTest()[i];
				const double beta = feathers::kBetaOverCoupling * c;
				const double row = w.nodeStiffnessForTest()[i] + 4.0 * c + 6.0 * beta * (dp * dp + dm * dm);
				const double edge = 16.0 - 4.0 * w.nodeDampingForTest()[i];
				worstMargin = std::min(worstMargin, edge - row);
				largestStretch = std::max(largestStretch, std::abs(dp));
			}
		});
	}
	MESSAGE("Stiffening at bow x4: worst margin to 16 - 4d " << worstMargin << ", largest neighbour difference "
	                                                          << largestStretch);
	CHECK(worstMargin >= 3.0);
}

TEST_CASE("weave models: the stencil gains are the laws' coupling scaled into the shared bound") {
	const owlet::dsp::PhiWeaveParams p = buildPhiWeaveLaws(0.37);
	for (WeaveModel m : { WeaveModel::Ring, WeaveModel::Bar, WeaveModel::Tube, WeaveModel::Stiffening }) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		WeaveString w;
		setUp(w, m, 0.37, 0.37, 1.0f);
		const float scale = (m == WeaveModel::Bar) ? feathers::kBarBendingGain : 1.0f;
		const float homeScale = (m == WeaveModel::Bar) ? feathers::kBarHomeSpring : 1.0f;
		const float bowScale = (m == WeaveModel::Bar) ? feathers::kBarBowGain : 1.0f;
		for (int i = 0; i < 32; ++i) {
			CHECK(w.nodeGainForTest()[i] == p.coupling[i] * scale);
			CHECK(w.nodeStiffnessForTest()[i] == p.stiffness[i] * homeScale);
			CHECK(w.nodeBowBalanceForTest()[i] == p.bowBalance[i] * bowScale);
		}
	}
	// The Membrane reads the laws by radius: its four centre nodes share one radius and so one law, and
	// its gain is half the coupling read there.
	WeaveString w;
	setUp(w, WeaveModel::Membrane, 0.37, 0.37, 1.0f);
	const float* g = w.nodeGainForTest();
	const int c00 = weave::membraneNode(3, 3);
	CHECK(g[c00] == g[weave::membraneNode(4, 3)]);
	CHECK(g[c00] == g[weave::membraneNode(3, 4)]);
	CHECK(g[c00] == g[weave::membraneNode(4, 4)]);
	CHECK(g[weave::membraneNode(0, 0)] == g[weave::membraneNode(7, 7)]); // the corners are law 31
	CHECK(g[weave::membraneNode(0, 0)] == feathers::kMembraneGridGain * p.coupling[31]);
}

TEST_CASE("weave models: each model comes to rest in its own rest pose") {
	// Seated by reset() at the model's rest pose, the nodes must satisfy k(x - home) = F(x) with F the
	// RESTATED stencil — and a plucked, unbowed string left 30 s must come back to the same pose.
	for (WeaveModel m : kAll) {
		for (double phase : { 0.1, 0.45, 0.8 }) {
			const std::string model = weave::modelName(m);
			CAPTURE(model);
			CAPTURE(phase);
			WeaveString w;
			setUp(w, m, phase, phase, 0.0f);
			const int n = w.nodeCount();
			const float* x = w.nodesForTest();
			std::vector<float> rest(x, x + n);
			double residual = 0.0;
			double fromHome = 0.0;
			for (int i = 0; i < n; ++i) {
				const double r = w.nodeStiffnessForTest()[i] * (static_cast<double>(x[i]) - w.nodeHomeForTest()[i])
				                 - restatedForce(m, x, w.nodeGainForTest(), i);
				residual = std::max(residual, std::abs(r));
				fromHome = std::max(fromHome, std::abs(static_cast<double>(x[i]) - w.nodeHomeForTest()[i]));
			}
			CHECK(residual < 1e-6);
			CHECK(fromHome > 1e-3); // the pose is not simply home, or this checks nothing

			runTicks(w, ticksFor(30.0), [](int) {});
			double back = 0.0;
			for (int i = 0; i < n; ++i) {
				back = std::max(back, static_cast<double>(std::abs(w.nodesForTest()[i] - rest[static_cast<size_t>(i)])));
			}
			CHECK(back < 1e-3);
		}
	}
}

TEST_CASE("weave models: Stiffening's rest pose stays finite from an extreme state") {
	// Laws no preset makes: the stiffness at its clamp, a coupling past its range, and a home shape of up
	// to float's largest value, at one node and alternating (the cubic's worst case, every neighbour
	// difference twice the amplitude). Newton starts far from its answer and does not converge in its
	// steps, but every pose must come back finite, and a finite pose from finite laws never needs the
	// linear fallback — the iterate is bounded by the linear pose here, far inside double's range. MEASURED
	// 2026-09-26: the pose's peak is 2.3e37 at home FLT_MAX on one node, 5.8e35 alternating.
	static weave::RestPoseScratch scratch;
	for (float amp : { 1.0f, 1e10f, 1e30f, std::numeric_limits<float>::max() }) {
		for (bool alternating : { false, true }) {
			CAPTURE(amp);
			CAPTURE(alternating);
			weave::NodeLaws laws {};
			for (int i = 0; i < 32; ++i) {
				laws.k[i] = 0.016f;
				laws.gain[i] = 0.45f;
				laws.home[i] = alternating ? ((i & 1) ? amp : -amp) : ((i == 5) ? amp : 0.0f);
			}
			float pose[feathers::kWeaveMaxNodes];
			weave::restPose(WeaveModel::Stiffening, laws, scratch, pose);
			bool finite = true;
			for (int i = 0; i < 32; ++i) {
				finite = finite && std::isfinite(pose[i]);
			}
			CHECK(finite);
		}
	}
}

TEST_CASE("weave models: each model's scan path is read from its own geometry") {
	// A bowed, plucked string mid-flight, its path restated from its smoothed nodes: out and back along
	// the chain (0 -> 31 -> 0) for Tube and Bar, 32 bilinear samples on a circle of radius 0.6 for the
	// Membrane, the nodes themselves for Ring and Stiffening.
	for (WeaveModel m : kAll) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		WeaveString w;
		setUp(w, m, 0.3, 0.3 + kPhiOffset, 1.5f);
		runTicks(w, 200, [](int) {});
		if (weave::pathIsNodes(m)) {
			continue; // their path IS the smoothed nodes, the same array: restated in the case below
		}
		const float* xs = w.smoothedModelNodesForTest();
		const float* path = w.smoothedNodes();
		for (int j = 0; j < 32; ++j) {
			CAPTURE(j);
			double expect = 0.0;
			if (m == WeaveModel::Tube || m == WeaveModel::Bar) {
				const double t = j / 32.0;
				const double u = 31.0 * ((t < 0.5) ? 2.0 * t : 2.0 - 2.0 * t);
				const int i = std::min(static_cast<int>(u), 30);
				expect = xs[i] + (xs[i + 1] - xs[i]) * (u - i);
			}
			else {
				const double a = 2.0 * 3.14159265358979 * j / 32.0;
				const double cx = (0.6 * std::cos(a) + 1.0) * 4.0 - 0.5; // node centres at (index + 0.5)/4 - 1
				const double cy = (0.6 * std::sin(a) + 1.0) * 4.0 - 0.5;
				const int ix = static_cast<int>(cx);
				const int iy = static_cast<int>(cy);
				const double fx = cx - ix;
				const double fy = cy - iy;
				auto n = [&](int c, int r) -> double { return xs[r * kGrid + c]; };
				const double top = n(ix, iy) + (n(ix + 1, iy) - n(ix, iy)) * fx;
				const double bottom = n(ix, iy + 1) + (n(ix + 1, iy + 1) - n(ix, iy + 1)) * fx;
				expect = top + (bottom - top) * fy;
			}
			CHECK(std::abs(path[j] - expect) < 1e-5);
		}
	}
}

TEST_CASE("weave models: Ring and Stiffening scan the two-pole smoothing of their raw nodes") {
	// Their path is their smoothed nodes, the very array smoothedModelNodesForTest returns — so comparing
	// the two checks nothing. Restated here instead from the RAW nodes: one tick of an unbowed string with
	// its crossfade parked (so no bow force and no morph-bow noise) is two sub-steps of
	//     v += 0.25 (F(x) - k (x - home)) - 0.5 d v  (railed at +-0.25),  x += v  (railed at +-2),
	//     xs1 += a (x - xs1),  xs += a (xs1 - xs),
	// F the restated stencil, d and a the laws' damping and shimmer; the path after the tick must be xs.
	for (WeaveModel m : { WeaveModel::Ring, WeaveModel::Stiffening }) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		const owlet::dsp::PhiWeaveParams p = buildPhiWeaveLaws(0.3);
		WeaveString w;
		setUp(w, m, 0.3, 0.3, 0.0f);
		w.pluck(0.9f, 0.3f);
		runTicks(w, 20, [](int) {});
		double x[32], v[32], xs1[32], xs[32];
		for (int i = 0; i < 32; ++i) {
			x[i] = w.nodesForTest()[i];
			v[i] = w.velocitiesForTest()[i];
			xs1[i] = w.smoothedFirstPoleForTest()[i];
			xs[i] = w.smoothedModelNodesForTest()[i];
		}
		const double a = p.shimmerAlpha;
		for (int sub = 0; sub < 2; ++sub) {
			float xf[feathers::kWeaveMaxNodes] = {};
			for (int i = 0; i < 32; ++i) {
				xf[i] = static_cast<float>(x[i]);
			}
			for (int i = 0; i < 32; ++i) {
				const double f = restatedForce(m, xf, w.nodeGainForTest(), i);
				v[i] += 0.25 * (f - p.stiffness[i] * (x[i] - p.home[i])) - 0.5 * p.damping[i] * v[i];
				v[i] = std::clamp(v[i], -0.25, 0.25);
			}
			for (int i = 0; i < 32; ++i) {
				x[i] = std::clamp(x[i] + v[i], -2.0, 2.0);
				xs1[i] += a * (x[i] - xs1[i]);
				xs[i] += a * (xs1[i] - xs[i]);
			}
		}
		const float before = w.smoothedNodes()[5];
		w.advance(w.samplesUntilTick());
		double worst = 0.0;
		double moved = 0.0;
		for (int j = 0; j < 32; ++j) {
			worst = std::max(worst, std::abs(w.smoothedNodes()[j] - xs[j]));
			moved = std::max(moved, std::abs(xs[j] - w.nodesForTest()[j]));
		}
		MESSAGE(model << ": worst |path - restated smoothing| " << worst << "; the smoothing lags the raw nodes by up to "
		              << moved);
		CHECK(worst < 1e-5);
		CHECK(moved > 1e-3);                               // the path is not simply the raw nodes, or this checks nothing
		CHECK(std::abs(w.smoothedNodes()[5] - before) > 1e-4); // and it moved over the tick
	}
}

TEST_CASE("weave models: every model's pluck adds energy and it decays") {
	// MEASURED 2026-09-25 at phase 0.3, pluck 0.8 at 0.3 (energy one tick after the pluck / 3 s later): Ring
	// 0.235 / 1.9e-5, Bar 0.813 / 3.8e-5, Tube 0.700 / 2.6e-5, Membrane 0.0115 / 8.1e-7 (its bump lands
	// on few of its 64 nodes, so the margin is 0.005, under half of that), Stiffening 0.215 / 2.1e-5 at beta = 8c
	// (0.233 / 1.7e-5 at c/4).
	for (WeaveModel m : kAll) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		WeaveString w;
		setUp(w, m, 0.3, 0.3, 0.0f);
		w.pluck(0.0f);
		runTicks(w, ticksFor(1.0), [](int) {});
		const float e0 = w.energyForTest();
		w.pluck(0.8f, 0.3f);
		w.advance(w.samplesUntilTick());
		const float e1 = w.energyForTest();
		float peak = e1;
		runTicks(w, ticksFor(0.2), [&](int) { peak = std::max(peak, w.energyForTest()); });
		runTicks(w, ticksFor(2.8), [](int) {});
		const float e3 = w.energyForTest();
		MESSAGE(std::string(weave::modelName(m)) << ": E0 " << e0 << ", one tick after the pluck " << e1 << ", peak " << peak
		                            << ", 3 s later " << e3);
		CHECK(e1 > e0 + 0.005f);
		CHECK(e3 < 0.1f * peak);
	}
}

TEST_CASE("weave models: Stiffening's springs stiffen with stretch by the cubic's own amount") {
	// A sine shape x_i = A sin(theta i), theta = 2 pi m / 32, on a uniform ring of gain c. The linear
	// spring's force on it is -4 c s^2 x (s = sin(theta / 2)); the cubic's component along the shape is,
	// by cos^3 = (3 cos + cos 3)/4, a further -12 beta A^3 s^4 sin(theta i). So the stiffness the shape
	// feels, the force projected on it per unit amplitude, is the linear one times
	//     1 + 3 (beta / c) A^2 s^2,   beta / c = 1/4,
	// and the Ring's stays at 1 whatever the amplitude. Restated here and measured through
	// weave::elasticForce, the function the physics calls.
	constexpr int kMode = 3;
	constexpr float kGain = 0.2f;
	const double theta = 2.0 * 3.14159265358979 * kMode / 32.0;
	const double s = std::sin(theta / 2.0);
	float gain[feathers::kWeaveMaxNodes];
	std::fill(gain, gain + 32, kGain);
	auto stiffness = [&](WeaveModel m, double amp) {
		float x[feathers::kWeaveMaxNodes] = {};
		float force[feathers::kWeaveMaxNodes] = {};
		double norm = 0.0;
		for (int i = 0; i < 32; ++i) {
			x[i] = static_cast<float>(amp * std::sin(theta * i));
			norm += std::sin(theta * i) * std::sin(theta * i);
		}
		weave::elasticForce(m, x, gain, force);
		double proj = 0.0;
		for (int i = 0; i < 32; ++i) {
			proj -= force[i] * std::sin(theta * i);
		}
		return proj / (amp * norm);
	};
	const double linear = 4.0 * kGain * s * s;
	for (double amp : { 0.05, 0.5, 1.0, 1.5 }) {
		CAPTURE(amp);
		const double expect = linear * (1.0 + 3.0 * feathers::kBetaOverCoupling * amp * amp * s * s);
		CHECK(std::abs(stiffness(WeaveModel::Stiffening, amp) / expect - 1.0) < 1e-4);
		CHECK(std::abs(stiffness(WeaveModel::Ring, amp) / linear - 1.0) < 1e-4);
	}
	const double rise = stiffness(WeaveModel::Stiffening, 1.5) / stiffness(WeaveModel::Stiffening, 0.05);
	MESSAGE("mode 3: stiffness at A = 1.5 over A = 0.05: " << rise);
	CHECK(rise > 1.1); // the effect is there to be heard, not a rounding
}

TEST_CASE("weave models: Stiffening's cubic is heard as a raised mode 3 at a laws' pluck") {
	// the decision (2026-09-26) was for a beta that is HEARD, so the amount is pinned as a value. At
	// every 64th phase, an unbowed Ring and Stiffening under the same laws, each plucked by the laws' own
	// pluck: the raw displacement from rest projected on mode 3 about the pluck node,
	// sum_i (x_i - rest_i) cos(2 pi 3 (i - i0) / 32), its frequency over the first second from its zero
	// crossings (linearly interpolated), and Stiffening's over the Ring's. MEASURED 2026-09-26: median
	// +6.04% at beta = 8c (range -7.0..+32%: a projection on one mode of a string whose laws are not
	// uniform, so single phases scatter and the median is what is pinned); +0.22% at the first draft's c/4
	// (the known-bad).
	auto modeThreeFrequency = [](WeaveModel m, const owlet::dsp::PhiWeaveParams& p) {
		WeaveString w;
		w.setModel(m);
		w.setLaws(p, p);
		WeaveString::Excitation e;
		e.bowScale = 0.0f;
		w.setExcitation(e);
		w.prepare(kSr); // the laws' pluck is pending; it lands at the first tick
		std::vector<float> rest(w.nodesForTest(), w.nodesForTest() + 32);
		const double i0 = p.pluckPos * 32.0;
		std::vector<double> y;
		runTicks(w, ticksFor(1.0), [&](int) {
			double s = 0.0;
			for (int i = 0; i < 32; ++i) {
				s += (w.nodesForTest()[i] - rest[static_cast<size_t>(i)]) * std::cos(2.0 * 3.14159265358979 * 3.0 * (i - i0) / 32.0);
			}
			y.push_back(s);
		});
		std::vector<double> crossings;
		for (size_t t = 1; t < y.size(); ++t) {
			if ((y[t - 1] < 0.0) != (y[t] < 0.0)) {
				crossings.push_back(static_cast<double>(t - 1) + y[t - 1] / (y[t - 1] - y[t]));
			}
		}
		REQUIRE(crossings.size() >= 5);
		return 0.5 * static_cast<double>(crossings.size() - 1) / (crossings.back() - crossings.front());
	};
	std::vector<double> shift;
	for (int k = 0; k < 64; ++k) {
		CAPTURE(k);
		const owlet::dsp::PhiWeaveParams p = buildPhiWeaveLaws(k / 64.0);
		shift.push_back(modeThreeFrequency(WeaveModel::Stiffening, p) / modeThreeFrequency(WeaveModel::Ring, p) - 1.0);
	}
	std::sort(shift.begin(), shift.end());
	const double median = shift[shift.size() / 2];
	MESSAGE("mode 3 at a laws' pluck, Stiffening over Ring: median " << 100.0 * median << "%, range " << 100.0 * shift.front()
	                                                                 << ".." << 100.0 * shift.back() << "%");
	CHECK(median >= 0.04);
}

TEST_CASE("weave models: a pluck on the Bar spreads along it and is not held in place by its home spring") {
	// One node in the middle of an unbowed bar displaced by 0.5; the spread is the RMS distance, in
	// nodes, of the departure from rest about that node, weighted by its square. The home spring is
	// weak (kBarHomeSpring) so that the bending modes carry the pluck away. MEASURED 2026-09-25 at 40
	// ticks, phases 0.63 / 0.8 / 0.95 (low coupling, where a strong spring dominates): 3.05 / 2.64 / 2.55
	// nodes with the spring at x0.1; 1.97 / 1.68 / 1.78 at the laws' full stiffness (the known-bad). At
	// tick 1 it is 0.02 or less: the instrument sees a pluck that is still local.
	for (double phase : { 0.63, 0.8, 0.95 }) {
		CAPTURE(phase);
		WeaveString w;
		setUp(w, WeaveModel::Bar, phase, phase, 0.0f);
		w.pluck(0.0f);
		runTicks(w, 3, [](int) {});
		std::vector<float> rest(w.nodesForTest(), w.nodesForTest() + 32);
		constexpr int kAt = 15;
		auto spread = [&]() {
			double num = 0.0;
			double den = 0.0;
			for (int i = 0; i < 32; ++i) {
				const double d = w.nodesForTest()[i] - rest[static_cast<size_t>(i)];
				num += (i - kAt) * (i - kAt) * d * d;
				den += d * d;
			}
			return std::sqrt(num / den);
		};
		w.displaceForTest(kAt, 0.5f);
		w.advance(w.samplesUntilTick());
		const double first = spread();
		runTicks(w, 39, [](int) {});
		const double at40 = spread();
		MESSAGE("phase " << phase << ": spread at tick 1 " << first << ", at tick 40 " << at40 << " nodes");
		CHECK(first < 0.1);
		CHECK(at40 > 2.3);
	}
}

TEST_CASE("weave models: a Membrane displaced at its centre reaches the scan circle later") {
	// The scan circle (radius 0.6) never reads the four centre nodes. Displaced there and left unbowed,
	// the sheet must carry the disturbance OUT to the circle: read the RAW nodes along the circle (the
	// path restated from nodesForTest, so the smoother's lag cannot fake a delay) and the departure from
	// rest is small at the first tick and peaks later. The CONTROL displaces the nodes the circle reads
	// at angle 0 by the same amount: there the departure is large at once — which is what shows the
	// first case's delay is travel, not the instrument.
	auto rawCircle = [](const WeaveString& w, float* out) {
		const float* x = w.nodesForTest();
		for (int j = 0; j < 32; ++j) {
			const double a = 2.0 * 3.14159265358979 * j / 32.0;
			const double cx = (0.6 * std::cos(a) + 1.0) * 4.0 - 0.5;
			const double cy = (0.6 * std::sin(a) + 1.0) * 4.0 - 0.5;
			const int ix = static_cast<int>(cx);
			const int iy = static_cast<int>(cy);
			const double fx = cx - ix;
			const double fy = cy - iy;
			auto n = [&](int c, int r) -> double { return x[r * kGrid + c]; };
			const double top = n(ix, iy) + (n(ix + 1, iy) - n(ix, iy)) * fx;
			const double bottom = n(ix, iy + 1) + (n(ix + 1, iy + 1) - n(ix, iy + 1)) * fx;
			out[j] = static_cast<float>(top + (bottom - top) * fy);
		}
	};
	struct Result {
		float firstTick;
		float peak;
		long peakTick;
	};
	auto run = [&](const int (*nodes)[2], int count) {
		WeaveString w;
		setUp(w, WeaveModel::Membrane, 0.3, 0.3, 0.0f);
		w.pluck(0.0f);
		runTicks(w, 3, [](int) {});
		float rest[32];
		rawCircle(w, rest);
		for (int i = 0; i < count; ++i) {
			w.displaceForTest(weave::membraneNode(nodes[i][0], nodes[i][1]), 1.0f);
		}
		std::vector<float> dep;
		runTicks(w, ticksFor(1.0), [&](int) {
			float now[32];
			rawCircle(w, now);
			float d = 0.f;
			for (int j = 0; j < 32; ++j) {
				d = std::max(d, std::abs(now[j] - rest[j]));
			}
			dep.push_back(d);
		});
		const auto at = std::max_element(dep.begin(), dep.end()) - dep.begin();
		return Result { dep[0], dep[static_cast<size_t>(at)], static_cast<long>(at) + 1 };
	};
	const int centre[4][2] = { { 3, 3 }, { 4, 3 }, { 3, 4 }, { 4, 4 } };
	const int onCircle[4][2] = { { 5, 3 }, { 6, 3 }, { 5, 4 }, { 6, 4 } }; // the taps at angle 0
	const Result c = run(centre, 4);
	const Result ctl = run(onCircle, 4);
	MESSAGE("centre: tick 1 " << c.firstTick << ", peak " << c.peak << " at tick " << c.peakTick
	                          << "; control on the circle: tick 1 " << ctl.firstTick << ", peak " << ctl.peak
	                          << " at tick " << ctl.peakTick);
	REQUIRE(c.peak > 1e-3f);
	CHECK(c.peakTick >= 2);
	CHECK(c.firstTick < 0.2f * c.peak);
	CHECK(ctl.firstTick > 0.5f * ctl.peak); // the instrument can see an immediate departure
}

TEST_CASE("weave models: a model switch lands at the next tick") {
	WeaveString w;
	setUp(w, WeaveModel::Ring, 0.3, 0.3 + kPhiOffset, 1.0f);
	runTicks(w, 100, [](int) {});
	w.advance(w.samplesUntilTick() - 1); // the last sample of a window
	w.setModel(WeaveModel::Membrane);
	CHECK(w.model() == WeaveModel::Ring); // not until the tick
	w.advance(1);
	CHECK(w.model() == WeaveModel::Membrane);
	CHECK(w.nodeCount() == 64);
}

TEST_CASE("weave models: a model switch fades over the first half window and opens at the new model's level") {
	// Every ordered pair of models at phases 0.1 / 0.37 / 0.6 / 0.85 (laws A = phase, B = phase + 0.382,
	// bow x1), switched on a window's last sample.
	// THE FADE: the first half of the window after the switch fades FROM the old model's last tables to
	// the new model's first, so its first sample sits beside the old model's last one and no sample of
	// the window steps far from the one before it (read at path phase 0.37).
	// THE LEVEL: the loudness control is seated on the new model at the switch, as reset() seats it.
	// Level = RMS of the cur table the switch's tick builds. The REFERENCE is independent of the switch:
	// a string reset straight into the new model under the same laws. "Settled" = the median level over
	// 3..4 s after the switch.
	// MEASURED 2026-09-26: worst |first - last| 4.9e-4, worst step 0.016; the switch opens at the fresh
	// string's level to 2e-5 and at 0.999..1.028x the settled level. (The 1.43..1.52 this case once quoted
	// for the Bar was measured before kBarBowGain, with the full bow on the loosened bar.) Before the AGC
	// was seated (2026-09-25; the known-bad: no seatAgc()
	// at the switch) the level opened at 0.35..2.50x the settled level, and up to 2.5x the fresh string's
	// (Ring -> Membrane at 0.37: 0.057 against 0.123).
	double lo = 1e9;
	double hi = 0.0;
	double worstFresh = 0.0;
	float worstSeam = 0.f;
	float worstStep = 0.f;
	auto rms = [](const WeaveString& w) {
		double s = 0.0;
		for (int i = 0; i < 32; ++i) {
			s += static_cast<double>(w.tableForTest(0, i)) * w.tableForTest(0, i);
		}
		return std::sqrt(s / 32.0);
	};
	for (double phase : { 0.1, 0.37, 0.6, 0.85 }) {
		for (WeaveModel to : kAll) {
			WeaveString fresh;
			setUp(fresh, to, phase, phase + kPhiOffset, 1.0f);
			const double freshLevel = rms(fresh);
			for (WeaveModel from : kAll) {
				if (from == to) {
					continue;
				}
				const std::string pair = std::string(weave::modelName(from)) + " -> " + weave::modelName(to);
				CAPTURE(pair);
				CAPTURE(phase);
				WeaveString w;
				setUp(w, from, phase, phase + kPhiOffset, 1.0f);
				runTicks(w, 1000, [](int) {});
				w.advance(w.samplesUntilTick() - 1); // the last sample of a window
				const float before = w.sample(0.37, 0, 0);
				w.setModel(to);
				w.advance(1);
				REQUIRE(w.model() == to);
				const double just = rms(w);
				const float after = w.sample(0.37, 0, 0);
				float step = 0.f;
				float prev = after;
				for (int k = 1; k < w.samplesUntilTick(); ++k) {
					const float v = w.sample(0.37, 0, k);
					step = std::max(step, std::abs(v - prev));
					prev = v;
				}
				w.advance(w.samplesUntilTick());
				std::vector<double> settled;
				runTicks(w, ticksFor(4.0), [&](int t) {
					if (t >= ticksFor(3.0)) {
						settled.push_back(rms(w));
					}
				});
				std::sort(settled.begin(), settled.end());
				const double ratio = just / settled[settled.size() / 2];
				lo = std::min(lo, ratio);
				hi = std::max(hi, ratio);
				worstFresh = std::max(worstFresh, std::abs(just / freshLevel - 1.0));
				worstSeam = std::max(worstSeam, std::abs(after - before));
				worstStep = std::max(worstStep, step);
				CHECK(std::abs(after - before) < 0.005f);
				CHECK(step < 0.03f);
				CHECK(std::abs(just / freshLevel - 1.0) < 1e-3);
				CHECK(ratio > 0.97);
				CHECK(ratio < 1.1);
			}
		}
	}
	MESSAGE("across the switch: worst |first - last| " << worstSeam << ", worst sample-to-sample step in the window "
	                                                   << worstStep << "; just after / settled " << lo << " .. " << hi
	                                                   << "; worst |just / fresh - 1| " << worstFresh);
}

TEST_CASE("weave models: output is identical at any host block size for every model") {
	for (WeaveModel m : kAll) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		auto render = [&](int block) {
			WeaveString w;
			setUp(w, m, 0.23, 0.23 + kPhiOffset, 1.0f);
			std::vector<float> out;
			double phase = 0.0;
			const int total = static_cast<int>(kSr);
			int done = 0;
			while (done < total) {
				int left = std::min(block, total - done);
				while (left > 0) {
					const int n = std::min(left, w.samplesUntilTick());
					w.setCrossfade(0.5f + 0.5f * std::sin(static_cast<float>(done / w.tickLength()) * 0.01f));
					for (int k = 0; k < n; ++k) {
						out.push_back(w.sample(phase + w.travelPhaseAt(k), 0, k));
						phase += 110.0 / kSr;
					}
					w.advance(n);
					done += n;
					left -= n;
				}
			}
			return out;
		};
		CHECK(render(1) == render(441));
	}
}

TEST_CASE("weave models: the per-chunk Fade reads exactly what the offset form reads") {
	for (WeaveModel m : kAll) {
		const std::string model = weave::modelName(m);
		CAPTURE(model);
		WeaveString w;
		setUp(w, m, 0.4, 0.4 + kPhiOffset, 1.0f);
		runTicks(w, 20, [](int) {});
		w.advance(17);
		for (int k = 0; k < w.samplesUntilTick(); ++k) {
			const WeaveString::Fade f = w.fadeAt(k);
			CHECK(f.travel == w.travelPhaseAt(k));
			for (int mip = 0; mip < 3; ++mip) {
				CHECK(w.sample(f, 0.61, mip) == w.sample(0.61, mip, k));
			}
		}
	}
}

TEST_CASE("weave models: no allocation in any model's tick or in a model switch") {
	WeaveString w;
	setUp(w, WeaveModel::Ring, 0.3, 0.3 + kPhiOffset, 1.0f);
	float sink = 0.f;
	uint64_t allocs = 0;
	{
		AllocWindow window;
		for (int t = 0; t < 500; ++t) {
			if (t % 100 == 0) {
				w.setModel(kAll[static_cast<size_t>((t / 100) % feathers::kWeaveModelCount)]);
				w.pluck();
			}
			w.setCrossfade(static_cast<float>(t % 50) / 50.f);
			const int n = w.samplesUntilTick();
			for (int k = 0; k < n; ++k) {
				sink += w.sample(w.fadeAt(k), 0.3, t % 3);
			}
			w.advance(n);
		}
		allocs = window.allocs();
	}
	CHECK(std::isfinite(sink));
	CHECK(allocs == 0);
}

namespace {

// THE STRESS CASES, sized to run in under about a minute each on a loaded host (2026-09-26: the ten-minute
// versions took up to 550 s at load 40). Each model's run is split into four ctest entries by PHASE
// QUARTER — quarter q takes phases k = q, q + 4, ..., so every quarter spans the whole circle and all 64
// phases are still covered — and ctest -j runs them side by side.
//
// One run: the crossfade swept 0 -> 1 -> 0 every 3 s (the strongest morph-bow a smoothed knob gives),
// the laws' bow times `bow`, for `sweeps` whole sweeps. What it asserts, every one live:
//   - finite: every node and the scan, every tick;
//   - THE DISPLACEMENT RAIL HOLDS: |x| <= 2 at every node and tick. It cannot fail while the clamp is in;
//     it is here to fail when the clamp is taken out, which nothing else here sees (the rail-hit count
//     only counts clamps that happened);
//   - no growth: the mean kinetic energy (sum v^2) over the LAST three sweeps against sweeps 3..6 (9..18 s,
//     past the start-up), worst phase: a healthy string's energy wanders between two such windows, and a
//     diverging one climbs until the rails hold it;
//   - the scan stays under 0.8, well inside the 1.0 ceiling (a loudness-control fault would cross it);
//   - how often each rail had to catch, as a fraction of node-sub-steps.
struct StressResult {
	double worstGrowth = 0.0;
	float worstScan = 0.f;
	float worstX = 0.f;
	bool finite = true;
	double xFraction = 0.0;
	double vFraction = 0.0;
};

StressResult stress(WeaveModel m, float bow, int sweeps, int quarter) {
	const int period = ticksFor(3.0);
	const int ticks = sweeps * period;
	StressResult r;
	uint64_t xHits = 0;
	uint64_t vHits = 0;
	uint64_t steps = 0;
	for (int k = quarter; k < 64; k += 4) {
		CAPTURE(k);
		WeaveString w;
		setUp(w, m, k / 64.0, k / 64.0 + kPhiOffset, bow);
		const int n = w.nodeCount();
		double first = 0.0;
		double last = 0.0;
		runTicks(w, ticks, [&](int t) {
			w.setCrossfade(triangle01(t, period));
			double kinetic = 0.0;
			for (int i = 0; i < n; ++i) {
				const float x = w.nodesForTest()[i];
				r.finite = r.finite && std::isfinite(x);
				r.worstX = std::max(r.worstX, std::abs(x));
				kinetic += static_cast<double>(w.velocitiesForTest()[i]) * w.velocitiesForTest()[i];
			}
			if (t >= 3 * period && t < 6 * period) {
				first += kinetic; // summed: both windows are three sweeps long, so the ratio is of means
			}
			if (t >= ticks - 3 * period) {
				last += kinetic;
			}
			for (double p : { 0.01, 0.27, 0.5, 0.77 }) {
				const float v = w.sample(p, 0, 0);
				r.finite = r.finite && std::isfinite(v);
				r.worstScan = std::max(r.worstScan, std::abs(v));
			}
		});
		REQUIRE(first > 0.0);
		r.worstGrowth = std::max(r.worstGrowth, last / first);
		xHits += w.displacementRailHitsForTest();
		vHits += w.velocityRailHitsForTest();
		steps += static_cast<uint64_t>(ticks) * 2u * static_cast<uint64_t>(n);
	}
	r.xFraction = static_cast<double>(xHits) / static_cast<double>(steps);
	r.vFraction = static_cast<double>(vHits) / static_cast<double>(steps);
	return r;
}

// BOUNDED: bow x2 for 60 sweeps (three simulated minutes). MEASURED 2026-09-26, worst quarter per model
// (growth; displacement rail; velocity rail; scan): Bar 1.37, 3.4e-3, 0, 0.61; Tube 1.91, 9.6e-4, 0,
// 0.47; Membrane 1.25, 1.6e-5, 0, 0.58; Stiffening 1.40, 2.3e-4, 7.9e-8, 0.61.
void boundedThreeMinutes(WeaveModel m, int quarter, double growthBound, double xRailBound, double vRailBound) {
	const StressResult r = stress(m, 2.0f, 60, quarter);
	MESSAGE(std::string(weave::modelName(m)) << " bounded, quarter " << quarter + 1 << ": growth " << r.worstGrowth
	                                         << ", displacement rail " << r.xFraction << ", velocity rail " << r.vFraction
	                                         << ", worst scan " << r.worstScan << ", worst |x| " << r.worstX);
	CHECK(r.finite);
	CHECK(r.worstX <= 2.0f);
	CHECK(r.worstScan < 0.8f);
	CHECK(r.worstGrowth < growthBound);
	CHECK(r.xFraction < xRailBound);
	CHECK(r.vFraction < vRailBound);
}

// FOUR TIMES THE BOW: loud audio with Drive up can push the WEAVE effect's ring harder than bow x2, so
// each model is driven at x4 for 20 sweeps (a minute). MEASURED 2026-09-26, worst quarter per model
// (growth; displacement rail; velocity rail; scan): Ring 2.27, 1.8e-3, 2.1e-5, 0.61; Bar 2.70, 1.1e-2,
// 1.7e-4, 0.64; Tube 2.18, 4.5e-3, 9.0e-5, 0.57; Membrane 1.36, 2.6e-4, 5.8e-6, 0.60; Stiffening 1.90,
// 1.3e-3, 6.9e-6, 0.60. The Ring's own 2.27 says how far a healthy string's energy wanders between the
// two windows at its worst phase.
void fourTimesTheBow(WeaveModel m, int quarter, double growthBound, double xRailBound, double vRailBound) {
	const StressResult r = stress(m, 4.0f, 20, quarter);
	MESSAGE(std::string(weave::modelName(m)) << " at bow x4, quarter " << quarter + 1 << ": growth " << r.worstGrowth
	                                         << ", displacement rail " << r.xFraction << ", velocity rail " << r.vFraction
	                                         << ", worst scan " << r.worstScan << ", worst |x| " << r.worstX);
	CHECK(r.finite);
	CHECK(r.worstX <= 2.0f);
	CHECK(r.worstScan < 0.8f);
	CHECK(r.worstGrowth < growthBound);
	CHECK(r.xFraction < xRailBound);
	CHECK(r.vFraction < vRailBound);
}

} // namespace

// Four ctest entries per model (tests/CMakeLists.txt names them); the case names carry no comma.
#define WEAVE_BOUNDED(M, Q)                                                                                  \
	TEST_CASE("weave models: " #M " stays bounded for three simulated minutes over phase quarter " #Q " of 4") { \
		boundedThreeMinutes(WeaveModel::M, Q - 1, kBoundedGrowth, kBoundedXRail, kBoundedVRail);            \
	}
#define WEAVE_X4(M, Q)                                                                                                   \
	TEST_CASE("weave models: " #M " at four times the bow stays finite with its rails as a net over phase quarter " #Q " of 4") { \
		fourTimesTheBow(WeaveModel::M, Q - 1, kX4Growth, kX4XRail, kX4VRail);                                           \
	}

namespace {
// The bounds. Every rail fraction is now PER QUARTER (16 phases), where the ten-minute cases bounded the
// 64-phase aggregate, and one quarter can carry most of a model's hits: Bar x4's velocity rail is
// 4.2e-5 over all 64 phases and 1.67e-4 in quarter 4 alone. So:
// - bounded growth 2.0: KEPT (worst measured 1.91, Tube quarter 3);
// - bounded displacement rail 1e-2: NEW, about three times the worst (3.4e-3, Bar quarter 3);
// - bounded velocity rail 1e-5: KEPT (worst 7.9e-8, Stiffening);
// - x4 growth 4.0: KEPT (worst 2.70, Bar quarter 4 — the Ring's own worst is 2.27);
// - x4 displacement rail 1.2e-2: KEPT (worst 1.12e-2, Bar quarter 3 — tight, and deterministic);
// - x4 velocity rail 3.5e-4: WAS 1e-4 OVER ALL 64 PHASES, which Bar's quarter 4 exceeds by itself at
//   1.67e-4 while the aggregate is still 4.2e-5; about twice that quarter, under the 4e-4 the old
//   aggregate bound would allow a single quarter.
constexpr double kBoundedGrowth = 2.0;
constexpr double kBoundedXRail = 1e-2;
constexpr double kBoundedVRail = 1e-5;
constexpr double kX4Growth = 4.0;
constexpr double kX4XRail = 1.2e-2;
constexpr double kX4VRail = 3.5e-4;
} // namespace

WEAVE_BOUNDED(Bar, 1)
WEAVE_BOUNDED(Bar, 2)
WEAVE_BOUNDED(Bar, 3)
WEAVE_BOUNDED(Bar, 4)
WEAVE_BOUNDED(Tube, 1)
WEAVE_BOUNDED(Tube, 2)
WEAVE_BOUNDED(Tube, 3)
WEAVE_BOUNDED(Tube, 4)
WEAVE_BOUNDED(Membrane, 1)
WEAVE_BOUNDED(Membrane, 2)
WEAVE_BOUNDED(Membrane, 3)
WEAVE_BOUNDED(Membrane, 4)
WEAVE_BOUNDED(Stiffening, 1)
WEAVE_BOUNDED(Stiffening, 2)
WEAVE_BOUNDED(Stiffening, 3)
WEAVE_BOUNDED(Stiffening, 4)

WEAVE_X4(Ring, 1)
WEAVE_X4(Ring, 2)
WEAVE_X4(Ring, 3)
WEAVE_X4(Ring, 4)
WEAVE_X4(Bar, 1)
WEAVE_X4(Bar, 2)
WEAVE_X4(Bar, 3)
WEAVE_X4(Bar, 4)
WEAVE_X4(Tube, 1)
WEAVE_X4(Tube, 2)
WEAVE_X4(Tube, 3)
WEAVE_X4(Tube, 4)
WEAVE_X4(Membrane, 1)
WEAVE_X4(Membrane, 2)
WEAVE_X4(Membrane, 3)
WEAVE_X4(Membrane, 4)
WEAVE_X4(Stiffening, 1)
WEAVE_X4(Stiffening, 2)
WEAVE_X4(Stiffening, 3)
WEAVE_X4(Stiffening, 4)
