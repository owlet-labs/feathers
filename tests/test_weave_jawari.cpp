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


// THE JAWARI BRIDGE (WeaveString::setBridge): a one-sided curved barrier under one end of the string that the
// slow wander strikes and bounces off. Every claim is read as a value through the string's seams — the contact
// law's own output, the energy a contact changed, the node against the surface, the spatial spectrum of the
// scanned shape — never inferred from a render.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/weave_models.h"
#include "dsp/weave_string.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

using owlet::dsp::buildPhiWeaveLaws;
using feathers::WeaveModel;
using feathers::WeaveString;
namespace weave = feathers::weave;

constexpr double kSr = 44100.0;
constexpr double kPhiOffset = 0.3819660112501051;
constexpr double kPi = 3.14159265358979323846;
constexpr WeaveModel kAllModels[] = { WeaveModel::Ring, WeaveModel::Bar, WeaveModel::Tube, WeaveModel::Membrane,
	                                    WeaveModel::Stiffening };
static_assert(std::size(kAllModels) == feathers::kWeaveModelCount);

void setUp(WeaveString& s, WeaveModel m, double phase, float bowScale, float jawari, float curve) {
	s.setModel(m);
	s.setLaws(buildPhiWeaveLaws(phase), buildPhiWeaveLaws(phase + kPhiOffset));
	s.setExcitation({ bowScale });
	s.setBridge({ jawari, curve });
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

// THE SPATIAL SPECTRUM OF THE SCANNED SHAPE'S DEPARTURE FROM REST, for a Loop (the path is the nodes): the
// energy in spatial modes 5..16 and in all of 1..16, from the 32-point DFT of (smoothed nodes - rest pose),
// written out. "Above mode 4" is the measure the brightening is stated in.
struct ModeEnergy {
	double high = 0.0;
	double total = 0.0;
};
ModeEnergy modeEnergy(const WeaveString& w, bool raw = false) {
	constexpr int n = WeaveString::kNodes;
	const float* xs = raw ? w.nodesForTest() : w.smoothedNodes();
	const float* rest = w.restNodes();
	ModeEnergy e;
	for (int k = 1; k <= n / 2; ++k) {
		double re = 0.0;
		double im = 0.0;
		for (int i = 0; i < n; ++i) {
			const double d = static_cast<double>(xs[i]) - rest[i];
			re += d * std::cos(2.0 * kPi * k * i / n);
			im -= d * std::sin(2.0 * kPi * k * i / n);
		}
		const double p = re * re + im * im;
		e.total += p;
		if (k > 4) {
			e.high += p;
		}
	}
	return e;
}

// THE RING'S ENERGY ABOVE MODE 4 ON THE TICKS A CONTACT HAPPENED, against the same ticks of the same string with
// no bridge: two strings in lockstep, one bridged, sharing every bow sample (the bow's noise is deterministic), so
// the unbridged one is the counterfactual. The share is sum(high) / sum(total) over those ticks, on the RAW nodes
// (the ring's own energy).
struct Brightening {
	double ratio = 0.0;
	int contactTicks = 0;
};
Brightening brighteningDuringContact(double phase, float bow, float jawari, float curve, bool raw = true) {
	WeaveString on;
	WeaveString off;
	setUp(on, WeaveModel::Ring, phase, bow, jawari, curve);
	setUp(off, WeaveModel::Ring, phase, bow, 0.0f, curve);
	ModeEnergy a;
	ModeEnergy b;
	Brightening r;
	uint64_t last = 0;
	for (int t = 0; t < ticksFor(10.0); ++t) {
		on.advance(on.samplesUntilTick());
		off.advance(off.samplesUntilTick());
		const uint64_t c = on.bridgeContactsForTest();
		if (t >= ticksFor(2.0) && c != last) {
			const ModeEnergy x = modeEnergy(on, raw);
			const ModeEnergy y = modeEnergy(off, raw);
			a.high += x.high;
			a.total += x.total;
			b.high += y.high;
			b.total += y.total;
			++r.contactTicks;
		}
		last = c;
	}
	r.ratio = (a.total > 0.0 && b.high > 0.0) ? (a.high / a.total) / (b.high / b.total) : 0.0;
	return r;
}

constexpr double kPhases[] = { 0.05, 0.17, 0.29, 0.41, 0.53, 0.65, 0.77, 0.89 };

} // namespace

TEST_CASE("jawari: the contact law puts the node on the surface and throws it back at the restitution") {
	static_assert(WeaveString::kBridgeRestitution > 0.0f && WeaveString::kBridgeRestitution < 1.0f,
	              "a restitution of 1 or more would gain energy at a contact");
	CHECK(WeaveString::kBridgeRestitution < 1.0f);
	// Into the surface: placed on it, the velocity reversed and scaled.
	float x = -1.1f;
	float v = -0.2f;
	CHECK(WeaveString::bridgeContact(x, v, -1.0f));
	CHECK(x == -1.0f);
	CHECK(v == doctest::Approx(0.2f * WeaveString::kBridgeRestitution).epsilon(1e-6));
	CHECK(v > 0.0f);
	// Under it but already leaving (the surface rose under it): placed, the velocity kept.
	x = -1.05f;
	v = 0.03f;
	CHECK(WeaveString::bridgeContact(x, v, -1.0f));
	CHECK(x == -1.0f);
	CHECK(v == 0.03f);
	// Above it: untouched.
	x = -0.9f;
	v = -0.4f;
	CHECK_FALSE(WeaveString::bridgeContact(x, v, -1.0f));
	CHECK(x == -0.9f);
	CHECK(v == -0.4f);
}

TEST_CASE("jawari: a contact never gains energy on any model") {
	for (WeaveModel m : kAllModels) {
		for (double phase : { 0.11, 0.37, 0.62, 0.88 }) {
			for (float jawari : { 0.5f, 1.0f }) {
				for (float curve : { 0.0f, 1.0f }) {
					CAPTURE(weave::modelName(m));
					CAPTURE(phase);
					CAPTURE(jawari);
					CAPTURE(curve);
					WeaveString w;
					setUp(w, m, phase, 2.0f, jawari, curve);
					const int period = ticksFor(3.0);
					runTicks(w, ticksFor(8.0), [&](int t) { w.setCrossfade(triangle01(t, period)); });
					REQUIRE(w.bridgeContactsForTest() > 0u);
					// Energy about the rest pose, over the nodes a sub-step's contacts moved; float rounding of
					// (rest - offset) - rest is the only slack.
					CHECK(w.bridgeMaxEnergyGainForTest() <= 1e-9f);
				}
			}
		}
	}
}

TEST_CASE("jawari: the bridge sits under the string and no node ends a sub-step beneath it") {
	for (WeaveModel m : kAllModels) {
		for (double phase : { 0.11, 0.37, 0.62, 0.88 }) {
			CAPTURE(weave::modelName(m));
			CAPTURE(phase);
			WeaveString w;
			setUp(w, m, phase, 4.0f, 1.0f, 0.5f);
			const int n = w.nodeCount();
			float worst = std::numeric_limits<float>::infinity();
			int bridged = 0;
			for (int i = 0; i < n; ++i) {
				const float s = w.bridgeSurfaceForTest(i);
				if (std::isfinite(s)) {
					++bridged;
					// Under the rest pose, never above it: at Jawari 1 the contact point touches it.
					CHECK(s <= w.restNodes()[i]);
				}
			}
			REQUIRE(bridged >= 5);
			// The crossfade is parked, so the rest pose the surface rides is exact at every tick.
			runTicks(w, ticksFor(6.0), [&](int) {
				for (int i = 0; i < n; ++i) {
					worst = std::min(worst, w.nodesForTest()[i] - w.bridgeSurfaceForTest(i));
				}
			});
			REQUIRE(w.bridgeContactsForTest() > 0u);
			CHECK(worst >= 0.0f);
		}
	}
}

TEST_CASE("jawari: the ring's energy above spatial mode 4 rises during contact against no bridge") {
	std::vector<double> ratios;
	int brighter = 0;
	for (double phase : kPhases) {
		const Brightening b = brighteningDuringContact(phase, 2.0f, 0.8f, 0.5f);
		CAPTURE(phase);
		REQUIRE(b.contactTicks >= 20);
		MESSAGE("phase " << phase << ": share above mode 4 x" << b.ratio << " over " << b.contactTicks << " contact ticks");
		ratios.push_back(b.ratio);
		brighter += b.ratio > 1.0 ? 1 : 0;
	}
	std::sort(ratios.begin(), ratios.end());
	const double median = 0.5 * (ratios[3] + ratios[4]);
	MESSAGE("median x" << median << ", brighter at " << brighter << " of 8");
	CHECK(median >= 2.0);
	// v9, THE LINE: MEASURED brighter at 5 of 8 (x0.78..0.87 at 0.17, 0.53 and 0.77, where the swing lies along the line
	// and it presses more than it kinks); the point bridge was brighter at 7. The median still rises (x2.9).
	CHECK(brighter >= 5);
	// Not vacuous: the same measure on an unbridged pair is exactly 1.
	CHECK(brighteningDuringContact(0.29, 2.0f, 0.0f, 0.5f).contactTicks == 0);
}

TEST_CASE("jawari: contact comes and goes as the wander swings") {
	// Windows of 8 ticks (~23 ms): some touch the bridge and some do not, and the string moves between the two
	// many times a second — the buzz is a pattern in time, not a constant press.
	std::vector<int> changesAll;
	for (double phase : kPhases) {
		CAPTURE(phase);
		WeaveString w;
		setUp(w, WeaveModel::Ring, phase, 2.0f, 0.8f, 0.5f);
		int touching = 0;
		int free = 0;
		int changes = 0;
		int windows = 0;
		bool wasTouching = false;
		uint64_t last = 0;
		runTicks(w, ticksFor(10.0), [&](int t) {
			if ((t + 1) % 8 != 0) {
				return;
			}
			const uint64_t c = w.bridgeContactsForTest();
			const bool now = c != last;
			last = c;
			if (t < ticksFor(2.0)) {
				wasTouching = now;
				return;
			}
			++windows;
			(now ? touching : free) += 1;
			changes += (now != wasTouching) ? 1 : 0;
			wasTouching = now;
		});
		MESSAGE("phase " << phase << ": " << touching << " of " << windows << " windows touch, " << changes << " changes");
		CHECK(touching >= windows / 50);
		CHECK(free >= windows / 20);
		changesAll.push_back(changes);
	}
	// ACROSS THE PHASES: the median phase changes at least 1.5 times a second over 8 s. (v9, the LINE against the string's
	// own swing: MEASURED 6..181 changes a phase, the median ~42; one phase, 0.17, presses and lifts only 6 times.)
	std::sort(changesAll.begin(), changesAll.end());
	CHECK(changesAll[changesAll.size() / 2] >= 12);
}

TEST_CASE("jawari: every model stays bounded at full jawari and four times the bow") {
	for (WeaveModel m : kAllModels) {
		for (float bow : { 4.0f, WeaveString::kMaxBowScale }) {
			for (float curve : { 0.0f, 1.0f }) {
				for (double phase : { 0.11, 0.37, 0.62, 0.88 }) {
					CAPTURE(weave::modelName(m));
					CAPTURE(bow);
					CAPTURE(curve);
					CAPTURE(phase);
					WeaveString w;
					setUp(w, m, phase, bow, 1.0f, curve);
					const int n = w.nodeCount();
					const int period = ticksFor(3.0);
					bool finite = true;
					float worstX = 0.0f;
					float worstScan = 0.0f;
					runTicks(w, ticksFor(20.0), [&](int t) {
						w.setCrossfade(triangle01(t, period));
						for (int i = 0; i < n; ++i) {
							finite = finite && std::isfinite(w.nodesForTest()[i]) && std::isfinite(w.velocitiesForTest()[i]);
							worstX = std::max(worstX, std::abs(w.nodesForTest()[i]));
						}
						for (double p : { 0.01, 0.27, 0.5, 0.77 }) {
							const float s = w.sample(p, 0, 0);
							finite = finite && std::isfinite(s);
							worstScan = std::max(worstScan, std::abs(s));
						}
					});
					REQUIRE(w.bridgeContactsForTest() > 0u);
					CHECK(finite);
					CHECK(worstX <= 2.0f);
					CHECK(worstScan <= 1.0f);
					CHECK(w.bridgeMaxEnergyGainForTest() <= 1e-9f);
				}
			}
		}
	}
}

TEST_CASE("jawari: zero is no bridge and changes no bit") {
	for (WeaveModel m : kAllModels) {
		CAPTURE(weave::modelName(m));
		WeaveString plain;
		plain.setModel(m);
		plain.setLaws(buildPhiWeaveLaws(0.37), buildPhiWeaveLaws(0.37 + kPhiOffset));
		plain.setExcitation({ 2.0f });
		plain.prepare(kSr);
		// Set on, then back to 0 before the string runs: what is left must be the plain string, bit for bit.
		WeaveString zeroed;
		setUp(zeroed, m, 0.37, 2.0f, 1.0f, 0.3f);
		zeroed.setBridge({ 0.0f, 0.9f });
		const int period = ticksFor(3.0);
		bool same = true;
		for (int t = 0; t < ticksFor(5.0); ++t) {
			const float cf = triangle01(t, period);
			plain.setCrossfade(cf);
			zeroed.setCrossfade(cf);
			if (t == ticksFor(1.0)) {
				plain.pluck(0.7f, 0.3f);
				zeroed.pluck(0.7f, 0.3f);
			}
			for (int k = 0; k < plain.tickLength(); ++k) {
				const float a = plain.sample(0.13 + 0.01 * k, 0, k);
				const float b = zeroed.sample(0.13 + 0.01 * k, 0, k);
				same = same && std::memcmp(&a, &b, sizeof a) == 0;
			}
			plain.advance(plain.samplesUntilTick());
			zeroed.advance(zeroed.samplesUntilTick());
		}
		CHECK(same);
		CHECK(zeroed.bridgeContactsForTest() == 0u);
	}
}

TEST_CASE("jawari: the buzz gets through to the scan: the smoothed path's energy above mode 4 rises during contact") {
	// THE SAME MEASURE AS THE RAW CASE, ON THE SMOOTHED NODES — the path the scan reads. Before the smoother was
	// lightened at the bridge (letBuzzThrough) the contact's harmonics mostly stopped at it.
	std::vector<double> ratios;
	for (double phase : kPhases) {
		const Brightening b = brighteningDuringContact(phase, 2.0f, 0.8f, 0.5f, false);
		CAPTURE(phase);
		REQUIRE(b.contactTicks >= 20);
		MESSAGE("phase " << phase << ": smoothed share above mode 4 x" << b.ratio);
		ratios.push_back(b.ratio);
	}
	std::sort(ratios.begin(), ratios.end());
	const double median = 0.5 * (ratios[3] + ratios[4]);
	MESSAGE("smoothed median x" << median);
	// MEASURED 2026-09-27: x1.49 with the smoother as it was (kBuzzThrough 0), x10.2 lightened.
	CHECK(median >= 5.0);
}

TEST_CASE("jawari: letting the buzz through adds no click: the scan's largest step stays near the bridge-free string's") {
	for (double phase : kPhases) {
		CAPTURE(phase);
		float worst[2] = { 0.0f, 0.0f };
		for (int on = 0; on < 2; ++on) {
			WeaveString w;
			setUp(w, WeaveModel::Ring, phase, 2.0f, on ? 1.0f : 0.0f, 0.5f);
			float last = w.sample(0.13, 0, 0);
			double p = 0.13;
			for (int t = 0; t < ticksFor(6.0); ++t) {
				const int m = w.samplesUntilTick();
				for (int k = 0; k < m; ++k) {
					p += 110.0 / kSr;
					const float s = w.sample(p, 0, k);
					if (t > ticksFor(1.0)) {
						worst[on] = std::max(worst[on], std::abs(s - last));
					}
					last = s;
				}
				w.advance(m);
			}
		}
		MESSAGE("phase " << phase << ": largest step " << worst[1] << " at Jawari 1 against " << worst[0] << " without");
		// MEASURED 2026-09-27: at most 1.52x (phase 0.41), every step under 0.015 of full scale.
		CHECK(worst[1] <= 2.0f * worst[0]);
	}
}

namespace {
// THE SHARE OF THE RING'S ENERGY ABOVE SPATIAL MODE 4, raw nodes, every tick from 2 to 10 s, bowed at x2: the median over
// kPhases.
double medianHighShare(float jawari) {
	std::vector<double> shares;
	for (double phase : kPhases) {
		WeaveString w;
		setUp(w, WeaveModel::Ring, phase, 2.0f, jawari, 0.5f);
		ModeEnergy e;
		runTicks(w, ticksFor(10.0), [&](int t) {
			if (t >= ticksFor(2.0)) {
				const ModeEnergy x = modeEnergy(w, true);
				e.high += x.high;
				e.total += x.total;
			}
		});
		shares.push_back(e.total > 0.0 ? e.high / e.total : 0.0);
	}
	std::sort(shares.begin(), shares.end());
	return 0.5 * (shares[3] + shares[4]);
}
} // namespace

TEST_CASE("jawari: the closer the line, the more of the ring's energy is in its short spatial modes") {
	// The premise (2026-09-28): every kink a contact leaves is a short spatial mode. The share of the raw ring's energy
	// above mode 4, median over eight phases, bowed at x2, rises at every step of Jawari.
	const float jawaris[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
	double share[5];
	std::string trace;
	for (size_t k = 0; k < 5; ++k) {
		share[k] = medianHighShare(jawaris[k]);
		trace += " " + std::to_string(jawaris[k]) + ": " + std::to_string(share[k]);
	}
	MESSAGE("median share above mode 4 by Jawari:" << trace);
	for (size_t k = 1; k < 5; ++k) {
		CAPTURE(k);
		CHECK(share[k] > share[k - 1]);
	}
}

TEST_CASE("jawari: a node past the line gives its excess to its neighbours: the sum is kept and the energy never rises") {
	// THE CONTACT LAW BY VALUE, on every model, bowed at x2 against the line at Jawari 1, elastic, for ten seconds with the
	// crossfade swept. Over every sub-step whose contacts passed their excess on in full (applyBridge counts them: no
	// budget cut, no last-pass set), the sum of the displacements before and after is the same (float: 1e-5); over EVERY
	// contact sub-step the energy (energyForTest's measure) never rises. Neighbours are the model's own: two along a Loop
	// or a chain (one at a chain's end), four across the Membrane's grid (fewer at its rim).
	for (WeaveModel m : kAllModels) {
		CAPTURE(weave::modelName(m));
		WeaveString w;
		w.setModel(m);
		w.setLaws(buildPhiWeaveLaws(0.29), buildPhiWeaveLaws(0.29 + kPhiOffset));
		w.setExcitation({ 2.0f });
		w.setBridge({ 1.0f, 0.5f, 1.0f });
		w.prepare(kSr);
		const int period = ticksFor(3.0);
		runTicks(w, ticksFor(10.0), [&](int t) { w.setCrossfade(triangle01(t, period)); });
		MESSAGE(weave::modelName(m) << ": " << w.lineContactStepsForTest() << " contact sub-steps, " << w.lineCleanStepsForTest()
		                            << " passed every excess on in full (budget cuts " << w.lineBudgetBoundForTest()
		                            << "), their worst change of the sum " << w.lineSumDriftForTest() << "; worst energy change "
		                            << w.bridgeMaxEnergyGainForTest());
		REQUIRE(w.lineContactStepsForTest() > 100u);
		CHECK(w.lineCleanStepsForTest() > 0u);
		CHECK(w.lineSumDriftForTest() < 1e-5);
		CHECK(w.bridgeMaxEnergyGainForTest() <= 1e-9f);
	}
}
