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

// THE DRUM HIT ALONE (stages S1-S2). Every claim about a frequency, a
// decay or an onset is asserted on the VALUE the hit is using (its seams), restated independently here from the physics
// or the design's formula; a rendered buffer is compared only for identity (determinism) and for exact zeros.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>

#include "dsp/drum_voice.h"

using namespace feathers;
using namespace feathers::drum;

namespace {

std::atomic<long> gAllocs { 0 };
std::atomic<bool> gCounting { false };

void* countedAlloc(std::size_t n) {
	if (gCounting.load()) {
		gAllocs.fetch_add(1);
	}
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

constexpr double kSr = 48000.0;

/// The literature's Bessel zeros j_mn in ascending order (Abramowitz and Stegun, table 9.5), written out here rather than
/// read from the code under test.
constexpr double kJ[16] { 2.404825557695773, 3.831705970207512, 5.135622301840683, 5.520078110286311,
	                      6.380161895923984, 7.015586669815619, 7.588342434503805, 8.417244140399865,
	                      8.653727912911012, 8.771483815959954, 9.761023129981670, 9.936109524217684,
	                      10.17346813506272, 11.06470948850119, 11.08637001924291, 11.61984117214906 };

Laws baseLaws(double hz = 110.0, double t60 = 0.3) {
	Laws l;
	l.hz = hz;
	l.t60 = t60;
	l.sweepOctaves = 0.0;
	l.sweepTau = 0.02;
	l.tone = 0.5f;
	l.noise = 0.3f;
	l.snap = 0.5f;
	l.metal = 0.0f;
	l.count = 3;
	l.spreadSeconds = 0.03;
	return l;
}

struct Rig {
	Hit hit;
	ks::KsString string;
	ks::PluckBuilder builder;
	Rig() {
		string.prepare(kSr);
		builder.prepare(kSr);
		hit.prepare(kSr);
		hit.setString(&string, &builder);
	}
};

std::vector<float> render(Hit& h, size_t n, size_t block = 256) {
	std::vector<float> out(n);
	for (size_t i = 0; i < n; i += block) {
		h.render(out.data() + i, static_cast<int>(std::min(block, n - i)));
	}
	return out;
}

constexpr Kind kAllKinds[] { Kind::Kick, Kind::Membrane, Kind::Snare, Kind::Metal, Kind::Clap, Kind::String };

} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

TEST_CASE("the structure morph: a Membrane's modes at Bessel's ratios at Metal 0, a clamped plate's at 1/3, a free bar's at 2/3, stretched at 1") {
	Rig r;
	Laws l = baseLaws(110.0);
	r.hit.strike(Kind::Membrane, l, 7, 1.0f);
	REQUIRE(r.hit.liveModes() == kMembraneModes);
	for (int k = 0; k < kMembraneModes; ++k) {
		CAPTURE(k);
		CHECK(std::abs(r.hit.currentHz(k) / (110.0 * kJ[k] / kJ[0]) - 1.0) < 1.0e-12);
	}
	// THE CLAMPED PLATE: (λ / λ_01)² from the literature's λ_01 = 3.1962, λ_11 = 4.6109, λ_21 = 5.9057, λ_02 = 6.3064 (Leissa,
	// "Vibration of Plates", NASA SP-160, table 2.1), written out here.
	l.metal = 1.0f / 3.0f;
	r.hit.strike(Kind::Membrane, l, 7, 1.0f);
	const double plate[4] { 1.0, 4.6109 / 3.1962, 5.9057 / 3.1962, 6.3064 / 3.1962 };
	for (int k = 0; k < 4; ++k) {
		CAPTURE(k);
		CHECK(std::abs(r.hit.currentHz(k) / 110.0 - plate[k] * plate[k]) < 2.0e-3 * plate[k] * plate[k]);
	}
	// The bar's first three, (β_n / β_1)² from the roots of cos β cosh β = 1: 1, 2.7565, 5.4039 (Rossing, "The Science of
	// Sound", table 2.2).
	l.metal = 2.0f / 3.0f;
	r.hit.strike(Kind::Membrane, l, 7, 1.0f);
	CHECK(std::abs(r.hit.currentHz(0) - 110.0) < 1.0e-6);
	CHECK(std::abs(r.hit.currentHz(1) / 110.0 - 2.7565) < 1.0e-3);
	CHECK(std::abs(r.hit.currentHz(2) / 110.0 - 5.4039) < 1.0e-3);
	// STRETCHED: the bar's to the power 1.25.
	l.metal = 1.0f;
	r.hit.strike(Kind::Membrane, l, 7, 1.0f);
	CHECK(std::abs(r.hit.currentHz(1) / 110.0 - std::pow(2.7565, 1.25)) < 2.0e-3);
	// Halfway between stations, each ratio is the geometric mean of the two: Bessel and plate at 1/6.
	l.metal = 1.0f / 6.0f;
	r.hit.strike(Kind::Membrane, l, 7, 1.0f);
	CHECK(std::abs(r.hit.currentHz(1) / 110.0 - std::sqrt((kJ[1] / kJ[0]) * plate[1] * plate[1])) < 2.0e-3);
}

TEST_CASE("Material: each mode's T60 is the body's times its ratio to the power -m, m rising with Metal from 0.6 to 2") {
	for (const float metal : { 0.0f, 0.5f, 1.0f }) {
		CAPTURE(metal);
		const double m = 0.6 + 1.4 * static_cast<double>(metal); // the law, written out
		Rig r;
		Laws l = baseLaws(110.0, 0.8);
		l.metal = metal;
		r.hit.strike(Kind::Membrane, l, 7, 1.0f);
		for (const int k : { 1, 3, 6 }) {
			CAPTURE(k);
			const double ratio = r.hit.currentHz(k) / r.hit.currentHz(0);
			const double expect = std::pow(10.0, -3.0 / (0.8 * std::pow(ratio, -m) * kSr));
			CHECK(std::abs(r.hit.modeDecayPerSample(k) - expect) < 1.0e-12);
		}
	}
}

TEST_CASE("each mode falls 60 dB in its own T60, the body's times its ratio to the power -0.6") {
	Rig r;
	const double t60 = 0.4;
	r.hit.strike(Kind::Membrane, baseLaws(110.0, t60), 3, 1.0f);
	for (const int k : { 0, 3, 7 }) {
		CAPTURE(k);
		const double ratio = kJ[k] / kJ[0];
		const double expect = std::pow(10.0, -3.0 / (t60 * std::pow(ratio, -0.6) * kSr));
		CHECK(std::abs(r.hit.modeDecayPerSample(k) - expect) < 1.0e-12);
	}
	// And the state the rotator carries really falls by it: mode 0's |z| after its T60 is a thousandth of where it began.
	const float a0 = r.hit.modeAmplitude(0);
	REQUIRE(a0 > 0.0f);
	const auto n = static_cast<size_t>(std::lround(t60 * kSr));
	render(r.hit, n);
	CHECK(std::abs(r.hit.modeAmplitude(0) / a0 - 1.0e-3) < 2.0e-5);
}

TEST_CASE("a mode at or above 0.45 of the sample rate is silent, never aliased") {
	Rig r;
	r.hit.strike(Kind::Membrane, baseLaws(5000.0), 1, 1.0f);
	int silent = 0;
	for (int k = 0; k < kMembraneModes; ++k) {
		const double hz = 5000.0 * kJ[k] / kJ[0];
		CAPTURE(k);
		if (hz >= 0.45 * kSr) {
			CHECK(r.hit.modeAmplitude(k) == 0.0f);
			CHECK(r.hit.modeDecayPerSample(k) == 0.0);
			++silent;
		}
		else {
			CHECK(r.hit.modeAmplitude(k) > 0.0f);
		}
	}
	CHECK(silent > 0); // the case is not vacuous: 5 kHz puts the top modes past 21.6 kHz
}

TEST_CASE("the Kick's pitch starts Sweep octaves up and has fallen 95 percent of the way by Sweep Time") {
	Rig r;
	Laws l = baseLaws(50.0, 0.5);
	l.sweepOctaves = 2.0;
	l.sweepTau = 0.06 / 3.0;
	r.hit.strike(Kind::Kick, l, 5, 1.0f);
	// THE SEEDED ±2% ON THE DEPTH (doc §13 item 10), read from the hit and bounded here.
	const double tv = r.hit.tensionVariationForTest();
	CHECK(std::abs(tv - 1.0) <= 0.02);
	CHECK(std::abs(r.hit.currentHz() - 50.0 * std::exp2(2.0 * tv)) < 1.0e-9);
	// 0.06 s at 48 kHz is 2880 samples, a whole number of 16-sample control blocks: the block at age 2880 reads it.
	render(r.hit, 2880);
	render(r.hit, 1);
	CHECK(r.hit.age() == 2881);
	CHECK(std::abs(r.hit.currentHz() - 50.0 * std::exp2(2.0 * tv * std::exp(-3.0))) < 1.0e-9);
	// With no Sweep it rests at Hz from the first sample.
	l.sweepOctaves = 0.0;
	r.hit.strike(Kind::Kick, l, 5, 1.0f);
	CHECK(std::abs(r.hit.currentHz() - 50.0) < 1.0e-12);
}

TEST_CASE("Metal's six oscillators sit at the cluster's ratios, spread by Metal") {
	Rig r;
	constexpr double ratios[6] { 1.0, 1.4826, 1.8003, 2.5461, 2.6303, 3.8968 };
	for (const float m : { 0.0f, 0.5f, 1.0f }) {
		Laws l = baseLaws(400.0);
		l.metal = m;
		r.hit.strike(Kind::Metal, l, 9, 1.0f);
		for (int i = 0; i < 6; ++i) {
			CAPTURE(m);
			CAPTURE(i);
			CHECK(std::abs(r.hit.metalHz(i) - 400.0 * std::pow(ratios[i], 0.6 + 0.8 * static_cast<double>(m))) < 1.0e-6);
		}
	}
}

TEST_CASE("the Snare's wires track its pitch (16 times it, Tone an octave either way); the Metal's and the Clap's bands sit where Tone puts them") {
	Rig r;
	for (const float t : { 0.0f, 0.25f, 1.0f }) {
		Laws l = baseLaws(180.0);
		l.tone = t;
		// Strike's per-hit variation moves Tone by up to 3%: read the Tone the hit is using, and restate from that.
		r.hit.strike(Kind::Snare, l, 11, 1.0f);
		const double ts = static_cast<double>(r.hit.laws().tone);
		CAPTURE(t);
		CHECK(std::abs(ts - static_cast<double>(t)) <= 0.03 * static_cast<double>(t) + 1.0e-7);
		// THE WIRES TRACK THE HEAD (Plaits' 16·f0): 16 × 180 Hz, Tone an octave down at 0 and up at 1.
		CHECK(std::abs(r.hit.bandCentreHz() - std::min(16.0 * 180.0 * std::exp2(2.0 * (ts - 0.5)), 0.45 * kSr)) < 1.0e-6);
		l.hz = 400.0;
		r.hit.strike(Kind::Metal, l, 11, 1.0f);
		CHECK(std::abs(r.hit.bandCentreHz() - std::min(2000.0 * std::pow(7.0, static_cast<double>(r.hit.laws().tone)), 0.45 * kSr)) < 1.0e-6);
		l.hz = 1000.0;
		r.hit.strike(Kind::Clap, l, 11, 1.0f);
		CHECK(std::abs(r.hit.bandCentreHz() - 600.0 * std::pow(5.0, static_cast<double>(r.hit.laws().tone))) < 1.0e-6);
	}
	// The Clap's band follows Pitch: an octave up doubles it.
	Laws l = baseLaws(2000.0);
	l.tone = 0.5f;
	r.hit.strike(Kind::Clap, l, 11, 1.0f);
	CHECK(std::abs(r.hit.bandCentreHz() - 2.0 * 600.0 * std::pow(5.0, static_cast<double>(r.hit.laws().tone))) < 1.0e-6);
	// AND THE SNARE'S WIRES follow its pitch too: an octave up doubles them.
	Laws s = baseLaws(150.0);
	s.tone = 0.5f;
	r.hit.strike(Kind::Snare, s, 11, 1.0f);
	const double low = r.hit.bandCentreHz() / std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5));
	s.hz = 300.0;
	r.hit.strike(Kind::Snare, s, 11, 1.0f);
	const double high = r.hit.bandCentreHz() / std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5));
	CHECK(std::abs(high / low - 2.0) < 1.0e-9);
}

TEST_CASE("a Clap's bursts land at onsets drawn from the hit's seed: the first at the strike, the last near Spread") {
	Rig r;
	Laws l = baseLaws(1000.0);
	l.count = 4;
	l.spreadSeconds = 0.03;
	r.hit.strike(Kind::Clap, l, 1234, 1.0f);
	const auto& on = r.hit.clapOnsetsForTest();
	CHECK(on[0] == 0);
	// Restated: onset i is round(i·spread/(n−1)·(1 + 0.3·u_i)·sr), u_i = 0.5·(the i-th draw of PluckNoise(seed ^ salt)).
	ks::PluckNoise rng(1234ull ^ 0xC1A9C1A9B0B5EEDull);
	for (int i = 1; i < 4; ++i) {
		const double u = 0.5 * static_cast<double>(rng.next());
		const auto expect = std::lround(static_cast<double>(i) * 0.03 / 3.0 * (1.0 + 0.3 * u) * kSr);
		CAPTURE(i);
		CHECK(on[static_cast<size_t>(i)] == expect);
		CHECK(on[static_cast<size_t>(i)] > on[static_cast<size_t>(i - 1)]);
	}
	CHECK(on[4] == -1);
	// Another seed, other onsets; the same seed, the same ones (derived, never incidental).
	const auto first = on;
	r.hit.strike(Kind::Clap, l, 1235, 1.0f);
	CHECK(r.hit.clapOnsetsForTest() != first);
	r.hit.strike(Kind::Clap, l, 1234, 1.0f);
	CHECK(r.hit.clapOnsetsForTest() == first);
}

TEST_CASE("every kind reaches exact silence, goes idle, and renders exact zeros after") {
	for (const Kind k : kAllKinds) {
		Rig r;
		Laws l = baseLaws(k == Kind::Kick ? 50.0 : k == Kind::Metal ? 400.0 : k == Kind::Clap ? 1000.0 : 110.0, 0.25);
		l.sweepOctaves = k == Kind::Kick ? 2.0 : 0.0;
		r.hit.strike(k, l, 42, 1.0f);
		CAPTURE(static_cast<int>(k));
		REQUIRE_FALSE(r.hit.idle());
		const std::vector<float> head = render(r.hit, 4800);
		float peak = 0.0f;
		for (const float x : head) {
			peak = std::max(peak, std::abs(x));
		}
		CHECK(peak > 1.0e-3f); // it sounded
		// −160 dB from full is 8/3 T60s; four seconds is ample for every kind at a 0.25 s T60.
		int64_t blocks = 0;
		while (!r.hit.idle() && blocks < 1000) {
			render(r.hit, 256);
			++blocks;
		}
		CHECK(r.hit.idle());
		const std::vector<float> after = render(r.hit, 4096);
		bool allZero = true;
		for (const float x : after) {
			allZero = allZero && x == 0.0f;
		}
		CHECK(allZero);
	}
}

TEST_CASE("the Gate takes every mode's decay down to Release") {
	Rig r;
	r.hit.strike(Kind::Membrane, baseLaws(110.0, 2.0), 3, 1.0f);
	r.hit.release(0.05);
	render(r.hit, 1);
	const double expect = std::pow(10.0, -3.0 / (0.05 * kSr));
	CHECK(std::abs(r.hit.modeDecayPerSample(0) - expect) < 1.0e-12);
	// A mode already shorter than Release keeps its own decay: Release only ever shortens.
	r.hit.strike(Kind::Membrane, baseLaws(110.0, 0.01), 3, 1.0f);
	r.hit.release(0.05);
	render(r.hit, 1);
	CHECK(std::abs(r.hit.modeDecayPerSample(0) - std::pow(10.0, -3.0 / (0.01 * kSr))) < 1.0e-12);
}

TEST_CASE("the same seed renders the same hit to the bit; another seed renders another") {
	for (const Kind k : kAllKinds) {
		CAPTURE(static_cast<int>(k));
		Rig a;
		Rig b;
		Laws l = baseLaws(k == Kind::Clap ? 1000.0 : 220.0, 0.2);
		l.noise = 0.5f;
		a.hit.strike(k, l, 99, 1.0f);
		b.hit.strike(k, l, 99, 1.0f);
		const auto x = render(a.hit, 2048);
		CHECK(x == render(b.hit, 2048));
		b.hit.strike(k, l, 100, 1.0f);
		CHECK(x != render(b.hit, 2048));
	}
}

TEST_CASE("the mapping: Pitch is a fine offset around Tune, Length a multiplier on Decay, velocity moves Tone and Snap") {
	Fields f;
	Controls c;
	c.decaySeconds = 0.5;
	c.pitchRangeSemitones = 3.0;
	c.velocityZone = 2.0 / 3.0; // the Velocity zone's Tension: Tone 0.5, the tension law in full
	f.pitch = 1.0f;
	f.length = 1.0f;
	f.tone = 0.5f;
	f.snap = 0.2f;
	f.sweep = 0.5f;
	const Laws l = lawsFor(Kind::Membrane, f, c, 100.0, 1.0);
	CHECK(std::abs(l.hz - 100.0 * std::pow(2.0, 3.0 / 12.0)) < 1.0e-9);
	CHECK(std::abs(l.t60 - 2.0) < 1.0e-12);
	CHECK(std::abs(static_cast<double>(l.tone) - 0.75) < 1.0e-6);
	CHECK(std::abs(static_cast<double>(l.snap) - 0.45) < 1.0e-6);
	CHECK(std::abs(l.sweepOctaves - 0.5) < 1.0e-12);
	f.pitch = 0.5f;
	f.length = 0.5f;
	const Laws n = lawsFor(Kind::Membrane, f, c, 100.0, 0.5);
	CHECK(n.hz == 100.0);
	CHECK(n.t60 == 0.5);
	// TENSION: the depth goes as velocity², Velocity Tension (default 1) applying the law: the full depth at 1, a quarter at 0.5.
	const Laws kick = lawsFor(Kind::Kick, f, c, 50.0, 1.0);
	CHECK(std::abs(kick.sweepOctaves - 0.5 * 4.0) < 1.0e-12);
	CHECK(std::abs(lawsFor(Kind::Kick, f, c, 50.0, 0.5).sweepOctaves - 0.5 * 4.0 * 0.25) < 1.0e-12);
	c.velocityZone = 1.0 / 3.0; // the Tone zone: no tension, so velocity leaves the depth alone
	CHECK(std::abs(lawsFor(Kind::Kick, f, c, 50.0, 0.5).sweepOctaves - 0.5 * 4.0) < 1.0e-12);
}

TEST_CASE("what each kind reads: Metal no Sweep, Clap neither Sweep nor Metal, String no Noise, Kick no Metal") {
	CHECK_FALSE(kindReads(Kind::Metal).sweep);
	CHECK_FALSE(kindReads(Kind::Clap).sweep);
	CHECK_FALSE(kindReads(Kind::Clap).metal);
	CHECK_FALSE(kindReads(Kind::String).noise);
	CHECK_FALSE(kindReads(Kind::Kick).metal);
	CHECK(kindReads(Kind::Membrane).metal);
	CHECK(kindControls(Kind::Kick).drive);
	CHECK(kindControls(Kind::Membrane).drive); // drive on every kind (2026-11-06)
	CHECK(kindControls(Kind::Clap).count);
	CHECK(kindControls(Kind::Clap).spread);
	CHECK_FALSE(kindControls(Kind::Metal).sweepTime);
}

TEST_CASE("a strike and its render never allocate, on any kind") {
	Rig r;
	std::vector<float> buf(256);
	for (const Kind k : kAllKinds) {
		Laws l = baseLaws(220.0, 0.2);
		gAllocs.store(0);
		gCounting.store(true);
		r.hit.strike(k, l, 5, 1.0f);
		for (int i = 0; i < 20; ++i) {
			r.hit.render(buf.data(), 256);
		}
		r.hit.release(0.05);
		r.hit.render(buf.data(), 256);
		gCounting.store(false);
		CAPTURE(static_cast<int>(k));
		CHECK(gAllocs.load() == 0);
	}
}

TEST_CASE("a String hit with no string is silent and idle, never a crash") {
	Hit h;
	h.prepare(kSr);
	h.strike(Kind::String, baseLaws(), 1, 1.0f);
	CHECK(h.idle());
	const auto out = render(h, 512);
	bool allZero = true;
	for (const float x : out) {
		allZero = allZero && x == 0.0f;
	}
	CHECK(allZero);
}

TEST_CASE("a String hit plays its string at Hz, its decay the hit's T60") {
	Rig r;
	Laws l = baseLaws(220.0, 1.5);
	l.metal = 0.0f;
	r.hit.strike(Kind::String, l, 3, 1.0f);
	CHECK(std::abs(r.string.frequencyHz() - 220.0) < 1.0e-9);
	CHECK(std::abs(r.hit.currentHz() - 220.0) < 1.0e-9);
	render(r.hit, 64);
	CHECK(std::abs(r.string.loopGainTarget() - ks::loopGainFor(kSr, 220.0, 1.5, r.string.damping())) < 1.0e-9);
}

#include "dsp/drum_picture.h"

TEST_CASE("the drum as heard: a Membrane's components are its modes at their ratios, each its strike amplitude times the "
          "root of its own T60 over the body's") {
	Fields f;
	f.pitch = 0.5f;
	f.tone = 0.3f;
	f.metal = 0.0f;
	f.noise = 0.0f;
	partials::PartialSpectrum s;
	heardSpectrum(Kind::Membrane, f, partials::kMaxPartials, s);
	// Restated: a_k = r^−(2.2 − 2·tone) over their sum; heard a_k·√(T60_k / T60) = a_k·r^−0.3; ratio r (Pitch at its middle).
	const double tone = 0.3;
	double sum = 0.0;
	for (int k = 0; k < kMembraneModes; ++k) {
		sum += std::pow(kJ[k] / kJ[0], -(2.2 - 2.0 * tone));
	}
	for (int k = 0; k < kMembraneModes; ++k) {
		const double r = kJ[k] / kJ[0];
		CAPTURE(k);
		CHECK(std::abs(s.ratio[static_cast<size_t>(k)] - r) < 1.0e-5);
		CHECK(std::abs(s.envAtRatio[static_cast<size_t>(k)] - std::pow(r, -(2.2 - 2.0 * tone)) / sum * std::pow(r, -0.3))
		      < 1.0e-6);
	}
	CHECK(s.envAtRatio[kMembraneModes] == 0.0f); // no noise band at Noise 0
	// A component past maxRatio is silent, never folded.
	heardSpectrum(Kind::Membrane, f, partials::kMaxPartials, s, 2.2);
	CHECK(s.ratio[3] == 0.0f); // the fourth mode (2.296) is past 2.2
	CHECK(s.envAtRatio[2] > 0.0f);
}

TEST_CASE("the drum as heard: Metal's components are its six ratios' odd harmonics, and a Kick's its body and harmonics") {
	Fields f;
	f.metal = 0.5f;
	f.noise = 0.2f;
	partials::PartialSpectrum s;
	heardSpectrum(Kind::Metal, f, partials::kMaxPartials, s);
	constexpr double ratios[6] { 1.0, 1.4826, 1.8003, 2.5461, 2.6303, 3.8968 };
	for (int i = 0; i < 6; ++i) {
		CAPTURE(i);
		const double r = std::pow(ratios[i], 0.6 + 0.8 * 0.5);
		CHECK(std::abs(s.ratio[static_cast<size_t>(3 * i)] - r) < 1.0e-5);
		CHECK(std::abs(s.ratio[static_cast<size_t>(3 * i + 1)] - 3.0 * r) < 1.0e-4);
		CHECK(std::abs(s.envAtRatio[static_cast<size_t>(3 * i)] - 0.8 / 6.0) < 1.0e-6);
	}
	Fields k;
	k.tone = 0.6f;
	heardSpectrum(Kind::Kick, k, partials::kMaxPartials, s);
	CHECK(s.ratio[0] == 1.0f);
	CHECK(s.envAtRatio[0] == 1.0f);
	CHECK(std::abs(s.envAtRatio[1] - 0.3) < 1.0e-6);
}

namespace {

Laws snareLaws() {
	Laws l = baseLaws(180.0, 0.6);
	l.tone = 0.4f;
	l.wireT60 = 0.3;
	l.clickT60 = 0.004;
	return l;
}

} // namespace

TEST_CASE("a Snare's three envelopes: the head by Decay, the wires by Wire Decay, the click by Click, each its own") {
	Rig r;
	r.hit.strike(Kind::Snare, snareLaws(), 7, 1.0f);
	const auto perSample = [](double t60) { return static_cast<float>(std::pow(10.0, -3.0 / (t60 * kSr))); };
	CHECK(r.hit.wireDecayPerSampleForTest() == perSample(0.3));
	CHECK(r.hit.clickDecayPerSampleForTest() == perSample(0.004));
	CHECK(std::abs(r.hit.modeDecayPerSample(0) - std::pow(10.0, -3.0 / (0.6 * kSr))) < 1.0e-12);
	// And the mapping: Length scales the wires' T60 as it scales the body's; Click is its own.
	Fields f;
	f.length = 1.0f;
	Controls c;
	c.decaySeconds = 0.2;
	c.wireDecaySeconds = 0.1;
	c.clickDecaySeconds = 0.005;
	const Laws l = lawsFor(Kind::Snare, f, c, 180.0, 0.5);
	CHECK(std::abs(l.t60 - 0.8) < 1.0e-12);
	CHECK(std::abs(l.wireT60 - 0.4) < 1.0e-12);
	CHECK(l.clickT60 == 0.005);
}

TEST_CASE("a Snare's Shape gates its component at Shape times its T60, then it falls in 15 ms") {
	Rig r;
	Laws l = snareLaws();
	l.bodyGate = 0.1;  // Body Shape 1/6 of 0.6 s
	l.wireGate = 0.05; // Wire Shape 1/6 of 0.3 s
	r.hit.strike(Kind::Snare, l, 7, 1.0f);
	render(r.hit, static_cast<size_t>(0.04 * kSr));
	CHECK_FALSE(r.hit.bodyGatedForTest());
	CHECK_FALSE(r.hit.wireGatedForTest());
	CHECK(std::abs(r.hit.modeDecayPerSample(0) - std::pow(10.0, -3.0 / (0.6 * kSr))) < 1.0e-12);
	render(r.hit, static_cast<size_t>(0.02 * kSr));
	CHECK(r.hit.wireGatedForTest());
	CHECK(r.hit.wireDecayPerSampleForTest() == static_cast<float>(std::pow(10.0, -3.0 / (0.015 * kSr))));
	CHECK_FALSE(r.hit.bodyGatedForTest());
	render(r.hit, static_cast<size_t>(0.05 * kSr));
	CHECK(r.hit.bodyGatedForTest());
	CHECK(std::abs(r.hit.modeDecayPerSample(0) - std::pow(10.0, -3.0 / (0.015 * kSr))) < 1.0e-12);
	// No Shape, no gate, however long it rings.
	Rig n;
	n.hit.strike(Kind::Snare, snareLaws(), 7, 1.0f);
	render(n.hit, static_cast<size_t>(0.4 * kSr));
	CHECK_FALSE(n.hit.bodyGatedForTest());
	CHECK_FALSE(n.hit.wireGatedForTest());
}

TEST_CASE("a Snare's wires sweep: their band starts Wire Sweep octaves off its centre and settles over a sixth of their T60") {
	Rig r;
	Laws l = snareLaws();
	l.wireSweepOctaves = 1.0;
	r.hit.strike(Kind::Snare, l, 7, 1.0f);
	const double centre = 16.0 * r.hit.laws().hz * std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5));
	CHECK(std::abs(r.hit.bandCentreHz() - centre * 2.0) < 1.0e-6);
	// 0.05 s is 2400 samples, 150 control blocks: the block at 2400 reads exp(-0.05 / (0.3 / 6)) = e^-1.
	render(r.hit, 2400);
	render(r.hit, 1);
	CHECK(std::abs(r.hit.bandCentreHz() - centre * std::exp2(std::exp(-1.0))) < 1.0e-6);
}

TEST_CASE("a Snare's moving bell glides from Bell Start to Bell End, 95% of the way (in octaves) by Bell Time") {
	Rig r;
	Laws l = snareLaws();
	l.bellStartHz = 3000.0;
	l.bellEndHz = 600.0;
	l.bellTau = 0.03 / 3.0;
	l.bellLevel = 0.5;
	r.hit.strike(Kind::Snare, l, 7, 1.0f);
	CHECK(std::abs(r.hit.bellHzForTest() - 3000.0) < 1.0e-6);
	render(r.hit, static_cast<size_t>(0.03 * kSr)); // 1440 samples, 90 blocks
	render(r.hit, 1);
	CHECK(std::abs(r.hit.bellHzForTest() - 600.0 * std::pow(5.0, std::exp(-3.0))) < 1.0e-6);
	// It ends at exact silence like every hit.
	int blocks = 0;
	while (!r.hit.idle() && blocks < 2000) {
		render(r.hit, 256);
		++blocks;
	}
	CHECK(r.hit.idle());
}

TEST_CASE("a Kick's start phase: 0 starts the sine on its zero crossing, 90 degrees on its crest") {
	Rig r;
	Laws l = baseLaws(50.0, 0.5);
	r.hit.strike(Kind::Kick, l, 3, 1.0f);
	CHECK(std::abs(r.hit.kickPhaseForTest()) < 1.0e-6);
	l.startPhase = std::numbers::pi / 2.0;
	r.hit.strike(Kind::Kick, l, 3, 1.0f);
	CHECK(std::abs(r.hit.kickPhaseForTest() - std::numbers::pi / 2.0) < 1.0e-6);
	// The mapping: degrees in, radians out, clamped to 0..90.
	Controls c;
	c.startPhaseDegrees = 135.0;
	CHECK(std::abs(lawsFor(Kind::Kick, Fields {}, c, 50.0, 0.5).startPhase - std::numbers::pi / 2.0) < 1.0e-12);
}

TEST_CASE("a Kick's Hold keeps its body at full for Hold, then it decays at its own T60") {
	Rig r;
	Laws l = baseLaws(50.0, 0.4);
	l.kickHold = 0.03;
	r.hit.strike(Kind::Kick, l, 3, 1.0f);
	CHECK(r.hit.holdingForTest());
	CHECK(r.hit.bodyDecayPerSampleForTest() == 1.0f);
	render(r.hit, 1424); // 89 blocks: still before 0.03 s (1440 samples)
	CHECK(r.hit.holdingForTest());
	render(r.hit, 32);
	CHECK_FALSE(r.hit.holdingForTest());
	CHECK(r.hit.bodyDecayPerSampleForTest() == static_cast<float>(std::pow(10.0, -3.0 / (0.4 * kSr))));
}

TEST_CASE("a Kick's fishtail dips its level by Dip, deepest at Dip Time, a raised cosine over half of it either side") {
	Rig r;
	Laws l = baseLaws(50.0, 0.5);
	l.kickDip = 0.6;
	l.kickDipTime = 0.016; // 768 samples, 48 control blocks
	r.hit.strike(Kind::Kick, l, 3, 1.0f);
	// Restated: 1 − Dip · ½(1 − cos 2π u), u = (t − T/2) / T.
	const auto want = [&](double t) {
		const double u = (t - 0.008) / 0.016;
		return u <= 0.0 || u >= 1.0 ? 1.0 : 1.0 - 0.6 * 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * u));
	};
	for (const double t : { 0.0, 0.006, 0.012, 0.016, 0.02, 0.03 }) {
		CAPTURE(t);
		CHECK(std::abs(r.hit.dipAtForTest(t) - want(t)) < 1.0e-12);
	}
	CHECK(std::abs(r.hit.dipAtForTest(0.016) - 0.4) < 1.0e-12);
	// And the voice plays it: at the block at 0.016 s the gain is the dip's deepest.
	render(r.hit, 768);
	render(r.hit, 1);
	CHECK(std::abs(r.hit.dipGainForTest() - 0.4f) < 1.0e-3f); // one sample into the block's ramp
}

TEST_CASE("a Kick's click sweep rides on top of its Sweep: Sweep plus Click Sweep octaves up at the strike, gone within its time") {
	Rig r;
	Laws l = baseLaws(50.0, 0.5);
	l.sweepOctaves = 2.0;
	l.sweepTau = 0.04 / 3.0;
	l.clickSweepOctaves = 3.0;
	l.clickSweepTau = 0.001;
	r.hit.strike(Kind::Kick, l, 3, 1.0f);
	const double tv = r.hit.tensionVariationForTest();
	CHECK(std::abs(r.hit.currentHz() - 50.0 * std::exp2(2.0 * tv + 3.0)) < 1.0e-6);
	// At 0.03 s (1440 samples) the click is 30 of its time constants gone (below a tenth of a cent): only Sweep remains.
	render(r.hit, 1440);
	render(r.hit, 1);
	CHECK(std::abs(r.hit.currentHz() - 50.0 * std::exp2(2.0 * tv * std::exp(-0.03 / (0.04 / 3.0)))) < 1.0e-6);
}

TEST_CASE("the drive's curves, restated: 0 in is 0 out, 1 in is 1 out; Tanh odd, Tube leans, Soft Clip flat past its knee") {
	for (const double drive : { 0.25, 0.6, 1.0 }) {
		const double g = 1.0 + 9.0 * drive;
		CAPTURE(drive);
		for (const double x : { -1.0, -0.4, -0.05, 0.0, 0.05, 0.4, 1.0 }) {
			CAPTURE(x);
			// The formulas written out from the design (tanh normalised; tanh leaning by a 0.35 bias, its offset taken out;
			// the cubic 1.5u − 0.5u³ on u clamped to ±1), not read from the code under test.
			const double tanhRef = std::tanh(g * x) / std::tanh(g);
			const double tubeRef = (std::tanh(g * x + 0.35) - std::tanh(0.35)) / (std::tanh(g + 0.35) - std::tanh(0.35));
			const double u = std::clamp(g * x, -1.0, 1.0);
			const double softRef = 1.5 * u - 0.5 * u * u * u;
			CHECK(std::abs(driveTransfer(0, drive, x) - tanhRef) < 1.0e-12);
			CHECK(std::abs(driveTransfer(1, drive, x) - tubeRef) < 1.0e-12);
			CHECK(std::abs(driveTransfer(2, drive, x) - softRef) < 1.0e-12);
		}
		for (int c = 0; c < 3; ++c) {
			CHECK(driveTransfer(c, drive, 0.0) == 0.0);
			CHECK(std::abs(driveTransfer(c, drive, 1.0) - 1.0) < 1.0e-12);
		}
		// Tanh is odd; Tube is not (its even harmonics); Soft Clip is flat past its knee at 1/g.
		CHECK(std::abs(driveTransfer(0, drive, 0.3) + driveTransfer(0, drive, -0.3)) < 1.0e-12);
		CHECK(std::abs(driveTransfer(1, drive, 0.3) + driveTransfer(1, drive, -0.3)) > 1.0e-3);
		CHECK(driveTransfer(2, drive, 1.0 / g) == 1.0);
		CHECK(driveTransfer(2, drive, 0.9) == 1.0);
	}
	CHECK(driveTransfer(1, 0.0, 0.37) == 0.37); // Drive 0 is the identity
}

TEST_CASE("Drive reaches every kind: each hit's output is its undriven output through its curve, sample by sample") {
	for (const Kind k : kAllKinds) {
		for (int curve = 0; curve < 3; ++curve) {
			CAPTURE(static_cast<int>(k));
			CAPTURE(curve);
			CHECK(kindControls(k).drive);
			Laws l = baseLaws(k == Kind::Kick ? 50.0 : k == Kind::Metal ? 400.0 : k == Kind::Clap ? 1000.0 : 110.0, 0.25);
			l.sweepOctaves = k == Kind::Kick ? 2.0 : 0.0;
			Rig clean;
			clean.hit.strike(k, l, 42, 0.8f);
			const std::vector<float> dry = render(clean.hit, 4800);
			l.drive = 0.7;
			l.driveCurve = curve;
			Rig driven;
			driven.hit.strike(k, l, 42, 0.8f);
			CHECK(driven.hit.driveCurveForTest() == curve);
			CHECK(driven.hit.driveGainForTest() == doctest::Approx(1.0 + 9.0 * 0.7).epsilon(1.0e-6));
			const std::vector<float> wet = render(driven.hit, 4800);
			double worst = 0.0;
			double moved = 0.0;
			for (size_t i = 0; i < dry.size(); ++i) {
				const double want = driveTransfer(curve, 0.7, static_cast<double>(dry[i]));
				worst = std::max(worst, std::abs(static_cast<double>(wet[i]) - want));
				moved = std::max(moved, std::abs(want - static_cast<double>(dry[i])));
			}
			CHECK(worst < 2.0e-3); // float against double, fasttanh against tanh
			CHECK(moved > 1.0e-2);  // and the drive did something to restate (not vacuous)
		}
	}
}

TEST_CASE("Drive Tone tilts what goes into the drive: x + tilt·(x − 2·lp) about a 600 Hz pole; the middle is flat") {
	for (const double tone : { 0.0, 0.5, 0.85 }) {
		CAPTURE(tone);
		Laws l = baseLaws(180.0, 0.2);
		l.noise = 0.6f;
		Rig clean;
		clean.hit.strike(Kind::Membrane, l, 9, 0.7f);
		const std::vector<float> dry = render(clean.hit, 4096);
		l.drive = 0.5;
		l.driveTone = tone;
		Rig driven;
		driven.hit.strike(Kind::Membrane, l, 9, 0.7f);
		const double tilt = 2.0 * tone - 1.0;
		CHECK(std::abs(static_cast<double>(driven.hit.driveTiltForTest()) - tilt) < 1.0e-6);
		const std::vector<float> wet = render(driven.hit, 4096);
		const double coef = 1.0 - std::exp(-2.0 * std::numbers::pi * 600.0 / kSr);
		double lp = 0.0;
		double worst = 0.0;
		for (size_t i = 0; i < dry.size(); ++i) {
			const double x = dry[i];
			double pre = x;
			if (tilt != 0.0) {
				lp += coef * (x - lp);
				pre = x + tilt * (x - 2.0 * lp);
			}
			worst = std::max(worst, std::abs(static_cast<double>(wet[i]) - driveTransfer(0, 0.5, pre)));
		}
		CHECK(worst < 2.0e-3);
	}
}

TEST_CASE("with Drive on, a tilt and every curve, every kind still reaches exact zero and goes idle") {
	for (const Kind k : kAllKinds) {
		for (int curve = 0; curve < 3; ++curve) {
			Rig r;
			Laws l = baseLaws(k == Kind::Kick ? 50.0 : k == Kind::Metal ? 400.0 : k == Kind::Clap ? 1000.0 : 110.0, 0.25);
			l.drive = 1.0;
			l.driveCurve = curve;
			l.driveTone = 0.2;
			r.hit.strike(k, l, 42, 1.0f);
			CAPTURE(static_cast<int>(k));
			CAPTURE(curve);
			int blocks = 0;
			while (!r.hit.idle() && blocks < 1000) {
				render(r.hit, 256);
				++blocks;
			}
			CHECK(r.hit.idle());
			const std::vector<float> after = render(r.hit, 1024);
			CHECK(std::all_of(after.begin(), after.end(), [](float x) { return x == 0.0f; }));
		}
	}
}

TEST_CASE("a Clap's hands are the 808's sawtooth: each rises at once and falls in a straight line, landing as the next starts") {
	Rig r;
	Laws l = baseLaws(1000.0, 0.3);
	l.snap = 0.0f; // the 808's straight tooth
	l.noise = 0.0f;
	l.count = 4;
	l.spreadSeconds = 0.03;
	r.hit.strike(Kind::Clap, l, 77, 1.0f);
	const std::array<int, kMaxClapBursts> on = r.hit.clapOnsetsForTest();
	float buf[1];
	int age = 0;
	float level = 0.0f;
	for (int b = 0; b < 3; ++b) { // the teeth before the last (the tail starts at the last)
		const int start = on[static_cast<size_t>(b)];
		const int span = on[static_cast<size_t>(b) + 1] - start;
		CAPTURE(b);
		while (age < start) {
			r.hit.render(buf, 1);
			++age;
		}
		// THE ONSET SAMPLE: the instant rise, to the hit's level (which carries the strike's own variation).
		r.hit.render(buf, 1);
		++age;
		if (b == 0) {
			level = r.hit.clapToothForTest();
		}
		CHECK(r.hit.clapToothForTest() == level);
		// THE FALL: k samples on it stands at level · (1 − k/span), a straight line that reaches 0 at the next onset.
		for (int k = 1; k < span; ++k) {
			r.hit.render(buf, 1);
			++age;
			const float want = level * (1.0f - static_cast<float>(k) / static_cast<float>(span));
			CHECK(std::abs(r.hit.clapToothForTest() - want) < 1.0e-5f * level);
		}
	}
	// THE LAST HAND spans the gap before it, then the tooth has landed: exactly 0 from there (the tail carries the clap on).
	{
		const int start = on[3];
		const int span = on[3] - on[2];
		while (age < start + span) {
			r.hit.render(buf, 1);
			++age;
		}
		CHECK(r.hit.clapToothForTest() > 0.0f); // its last sample, level / span
		r.hit.render(buf, 1);
		CHECK(r.hit.clapToothForTest() == 0.0f);
		r.hit.render(buf, 1); // and it stays landed, never running on below zero
		CHECK(r.hit.clapToothForTest() == 0.0f);
	}
	CHECK(level > 0.5f);
	// Snap bends the fall toward r²: halfway down a tooth stands at a quarter, not a half.
	Rig s;
	l.snap = 1.0f;
	s.hit.strike(Kind::Clap, l, 77, 1.0f);
	const int span = s.hit.clapOnsetsForTest()[1];
	for (int k = 0; k <= span / 2; ++k) {
		s.hit.render(buf, 1);
	}
	const float r2 = 1.0f - static_cast<float>(span / 2) / static_cast<float>(span);
	CHECK(std::abs(s.hit.clapToothForTest() - level * r2 * r2) < 1.0e-4f);
}

TEST_CASE("a Clap's Q and High Pass: its band is as narrow as Q, its high-pass at its corner, off at 20 Hz") {
	for (const double q : { 0.5, 1.6, 6.0 }) {
		Rig r;
		Laws l = baseLaws(1000.0, 0.3);
		l.clapQ = q;
		l.clapHighPassHz = 20.0;
		r.hit.strike(Kind::Clap, l, 3, 1.0f);
		CAPTURE(q);
		CHECK(std::abs(r.hit.clapQForTest() - q) < 1.0e-5 * q);
		CHECK(r.hit.clapHighPassCoefForTest() == 0.0f);
	}
	Rig r;
	Laws l = baseLaws(1000.0, 0.3);
	l.clapHighPassHz = 500.0;
	r.hit.strike(Kind::Clap, l, 3, 1.0f);
	CHECK(std::abs(static_cast<double>(r.hit.clapHighPassCoefForTest()) - (1.0 - std::exp(-2.0 * std::numbers::pi * 500.0 / kSr)))
	      < 1.0e-6);
	CHECK(kindControls(Kind::Clap).clapFilter);
	CHECK_FALSE(kindControls(Kind::Snare).clapFilter);
}

TEST_CASE("a Snare's second bell: Linked at Ratio times the first bell's centre, riding its glide; Free at its own Hz; its ring by Time") {
	// LINKED: at every instant the second bell's centre is Ratio × the first's.
	{
		Rig r;
		Laws l = snareLaws();
		l.bellStartHz = 600.0;
		l.bellEndHz = 200.0;
		l.bellTau = 0.02 / 3.0;
		l.bellLevel = 0.5;
		l.bell2Linked = true;
		l.bell2Ratio = 1.6;
		l.bell2Level = 0.5;
		r.hit.strike(Kind::Snare, l, 7, 1.0f);
		for (int step = 0; step < 6; ++step) {
			CAPTURE(step);
			CHECK(std::abs(r.hit.bell2HzForTest() - 1.6 * r.hit.bellHzForTest()) < 1.0e-9);
			render(r.hit, 160);
		}
		CHECK(std::abs(r.hit.bellHzForTest() - 600.0) > 1.0); // and it did glide, so the riding was tested
	}
	// FREE: at its own Hz throughout, whatever the first bell does.
	{
		Rig r;
		Laws l = snareLaws();
		l.bellLevel = 0.5;
		l.bell2Linked = false;
		l.bell2Hz = 1500.0;
		l.bell2Level = 0.5;
		r.hit.strike(Kind::Snare, l, 7, 1.0f);
		for (int step = 0; step < 4; ++step) {
			CHECK(r.hit.bell2HzForTest() == 1500.0);
			render(r.hit, 200);
		}
	}
	// ITS RING: the gain the n-th sample used is 4 · Level · 10^(−3·(n−1)/(T60·sr)), its own decay, not the hit's.
	{
		Rig r;
		Laws l = snareLaws();
		l.bell2Level = 0.75;
		l.bell2T60 = 0.04;
		r.hit.strike(Kind::Snare, l, 7, 1.0f);
		for (const int n : { 1, 97, 960, 1920 }) {
			Rig s;
			s.hit.strike(Kind::Snare, l, 7, 1.0f);
			render(s.hit, static_cast<size_t>(n), 1);
			const double want = 4.0 * 0.75 * std::pow(10.0, -3.0 * static_cast<double>(n - 1) / (0.04 * kSr));
			CAPTURE(n);
			CHECK(std::abs(static_cast<double>(s.hit.bell2GainForTest()) - want) < 1.0e-4 * want + 1.0e-7);
		}
	}
}

TEST_CASE("a Snare's two bells each hear the hit itself, not each other: both together are the dry hit plus each one's own peak") {
	Laws l = snareLaws();
	l.bellStartHz = 900.0;
	l.bellEndHz = 300.0;
	l.bellLevel = 0.6;
	l.bell2Linked = false;
	l.bell2Hz = 1200.0;
	l.bell2Level = 0.7;
	const auto play = [&](bool one, bool two) {
		Laws m = l;
		m.bellLevel = one ? l.bellLevel : 0.0;
		m.bell2Level = two ? l.bell2Level : 0.0;
		Rig r;
		r.hit.strike(Kind::Snare, m, 21, 1.0f);
		return render(r.hit, 2400);
	};
	const std::vector<float> dry = play(false, false);
	const std::vector<float> a = play(true, false);
	const std::vector<float> b = play(false, true);
	const std::vector<float> both = play(true, true);
	double worst = 0.0;
	double peak2 = 0.0;
	for (size_t i = 0; i < dry.size(); ++i) {
		const double sum = static_cast<double>(a[i]) + static_cast<double>(b[i]) - static_cast<double>(dry[i]);
		worst = std::max(worst, std::abs(static_cast<double>(both[i]) - sum));
		peak2 = std::max(peak2, std::abs(static_cast<double>(b[i]) - static_cast<double>(dry[i])));
	}
	CHECK(worst < 1.0e-5);
	CHECK(peak2 > 1.0e-2); // the second bell added something to superpose (not vacuous)
}

TEST_CASE("Tension: a Membrane's or Snare's glide is depth times its head's ring squared, 10^(-6t/T60), so it glides as long as it rings") {
	for (const Kind k : { Kind::Membrane, Kind::Snare }) {
		CAPTURE(static_cast<int>(k));
		Fields f;
		f.sweep = 0.8f;
		f.length = 0.5f;
		Controls c;
		c.decaySeconds = 0.4;
		c.sweepSeconds = 0.01; // NOT read here: the glide follows the ring
		const Laws l = lawsFor(k, f, c, 120.0, 1.0);
		Controls slow = c;
		slow.sweepSeconds = 0.3;
		CHECK(lawsFor(k, f, slow, 120.0, 1.0) == l);
		CHECK_FALSE(kindControls(k).sweepTime);
		CHECK(kindControls(k).tension);
		Rig r;
		r.hit.strike(k, l, 11, 1.0f);
		const double tv = r.hit.tensionVariationForTest();
		const double depth = 0.8 * kindSweepOctaves(k) * tv;
		CHECK(std::abs(r.hit.currentHz(0) / (120.0 * std::exp2(depth)) - 1.0) < 1.5e-4);
		// 0.1 s on (4800 samples, a whole number of control blocks): the offset is depth · 10^(−6 · 0.1 / 0.4).
		render(r.hit, 4800);
		render(r.hit, 1);
		const double want = 120.0 * std::exp2(depth * std::pow(10.0, -6.0 * 0.1 / 0.4));
		// IN HALF-CENT STEPS: the bend the modes are tuned to is a whole number of 2400ths of an octave.
		const double steps = std::log2(r.hit.currentHz(0) / 120.0) * 2400.0;
		CHECK(std::abs(steps - std::round(steps)) < 1.0e-3);
		// within the glide's half-cent steps (a quarter cent either way: 1.5e-4 of the frequency)
		CHECK(std::abs(r.hit.currentHz(0) / want - 1.0) < 1.5e-4);
		// A drum that rings twice as long glides twice as long: at 0.2 s it stands where the other stood at 0.1 s.
		Controls longer = c;
		longer.decaySeconds = 0.8;
		Rig s;
		s.hit.strike(k, lawsFor(k, f, longer, 120.0, 1.0), 11, 1.0f);
		render(s.hit, 9600);
		render(s.hit, 1);
		CHECK(std::abs(s.hit.currentHz(0) / want - 1.0) < 1.5e-4);
	}
	CHECK(kindControls(Kind::Kick).tension);
	CHECK(kindControls(Kind::Kick).sweepTime);
	CHECK_FALSE(kindControls(Kind::Metal).tension);
}

TEST_CASE("Velocity Noise moves a hit's noise share by depth times (velocity - 0.5), as Velocity moves Tone") {
	Fields f;
	f.noise = 0.4f;
	Controls c;
	c.velocityNoise = 0.6;
	CHECK(std::abs(static_cast<double>(lawsFor(Kind::Snare, f, c, 200.0, 1.0).noise) - (0.4 + 0.6 * 0.5)) < 1.0e-6);
	CHECK(std::abs(static_cast<double>(lawsFor(Kind::Snare, f, c, 200.0, 0.0).noise) - (0.4 - 0.6 * 0.5)) < 1.0e-6);
	c.velocityNoise = -1.0;
	CHECK(lawsFor(Kind::Snare, f, c, 200.0, 1.0).noise == doctest::Approx(0.0).epsilon(1.0e-9)); // clamped at none
	c.velocityNoise = 0.0;
	CHECK(lawsFor(Kind::Snare, f, c, 200.0, 1.0).noise == 0.4f);
	CHECK(kindControls(Kind::Snare).velNoise);
	CHECK_FALSE(kindControls(Kind::String).velNoise); // a String reads no Noise
}

TEST_CASE("the mallet: each mode starts at its strike weight times a raised-cosine pulse's spectrum at its frequency; Snap is its contact time") {
	// The Hann pulse's spectrum, written out here: sinc(x) / (1 − x²), x = f·Tc, 1 at DC and a half at x = 1.
	const auto hann = [](double x) {
		if (std::abs(x - 1.0) < 1.0e-6) {
			return 0.5;
		}
		return std::abs(std::sin(std::numbers::pi * x) / (std::numbers::pi * x) / (1.0 - x * x));
	};
	for (const float snap : { 0.0f, 0.5f, 1.0f }) {
		CAPTURE(snap);
		Laws l = baseLaws(150.0, 0.5);
		l.snap = snap;
		l.tone = 0.6f;
		l.malletSeconds = 0.005 * std::pow(0.06, static_cast<double>(snap));
		Rig r;
		r.hit.strike(Kind::Membrane, l, 4, 1.0f);
		// Mode k over mode 0 (the level and the strike's variation cancel): the strike-position weights times the pulse's.
		const double tone = static_cast<double>(r.hit.laws().tone); // Tone with the strike's own variation
		for (const int k : { 1, 4, 9 }) {
			CAPTURE(k);
			const double ratio = modeRatio(k, 0.0f);
			const double position = std::pow(ratio, -(2.2 - 2.0 * tone));
			const double want = position * hann(150.0 * ratio * l.malletSeconds) / hann(150.0 * l.malletSeconds);
			CHECK(std::abs(static_cast<double>(r.hit.modeAmplitude(k) / r.hit.modeAmplitude(0)) - want) < 1.0e-5);
		}
	}
	// A hard strike (short contact) rings the upper modes; a soft one barely.
	Laws soft = baseLaws(150.0, 0.5);
	soft.malletSeconds = 0.005;
	Laws hard = soft;
	hard.malletSeconds = 0.0003;
	Rig a;
	Rig b;
	a.hit.strike(Kind::Membrane, soft, 4, 1.0f);
	b.hit.strike(Kind::Membrane, hard, 4, 1.0f);
	CHECK(a.hit.modeAmplitude(9) / a.hit.modeAmplitude(0) < 0.2f * (b.hit.modeAmplitude(9) / b.hit.modeAmplitude(0)));
	// Snap sets it, velocity applied: 5 ms at Snap 0, 0.3 ms at Snap 1.
	Fields f;
	f.snap = 0.0f;
	Controls c;
	c.velocityZone = 0.0; // the Level zone: velocity moves no Tone or Snap
	CHECK(std::abs(lawsFor(Kind::Membrane, f, c, 150.0, 0.5).malletSeconds - 0.005) < 1.0e-12);
	f.snap = 1.0f;
	CHECK(std::abs(lawsFor(Kind::Membrane, f, c, 150.0, 0.5).malletSeconds - 0.0003) < 1.0e-12);
}

TEST_CASE("micro-variation: each mode starts at its own seeded phase within 0.1 rad; the first sample stays under a tenth of the level") {
	Laws l = baseLaws(150.0, 0.5);
	l.noise = 0.0f;
	l.snap = 0.0f; // the quietest click, so the first sample is the modes'
	Rig r;
	r.hit.strike(Kind::Membrane, l, 31, 1.0f);
	double spread = 0.0;
	for (int k = 0; k < r.hit.liveModes(); ++k) {
		const double phi = r.hit.modePhaseForTest(k);
		CHECK(std::abs(phi) <= 0.1 + 1.0e-6);
		spread = std::max(spread, std::abs(phi));
	}
	CHECK(spread > 0.01); // they do vary
	Rig s;
	s.hit.strike(Kind::Membrane, l, 31, 1.0f);
	CHECK(s.hit.modePhaseForTest(3) == r.hit.modePhaseForTest(3)); // the same seed, the same phases
	Rig o;
	o.hit.strike(Kind::Membrane, l, 32, 1.0f);
	CHECK(o.hit.modePhaseForTest(3) != r.hit.modePhaseForTest(3)); // another seed, others
	float first = 0.0f;
	Rig q;
	q.hit.strike(Kind::Membrane, l, 31, 1.0f);
	q.hit.render(&first, 1);
	CHECK(std::abs(first) < 0.1f);
	// AND THE TENSION'S ±2%, seeded the same way: a Kick's depth is its Sweep times 1 ± 0.02.
	Laws k = baseLaws(50.0, 0.5);
	k.sweepOctaves = 2.0;
	Rig kr;
	kr.hit.strike(Kind::Kick, k, 31, 1.0f);
	CHECK(std::abs(kr.hit.tensionVariationForTest() - 1.0) <= 0.02);
	CHECK(std::abs(kr.hit.laws().sweepOctaves - 2.0 * kr.hit.tensionVariationForTest()) < 1.0e-12);
	Rig metal;
	metal.hit.strike(Kind::Metal, k, 31, 1.0f);
	CHECK(metal.hit.tensionVariationForTest() == 1.0); // not on a kind without Tension
	// It does vary, seed by seed: three seeds, three depths, none of them exactly the Sweep.
	double seen[3] {};
	for (int i = 0; i < 3; ++i) {
		Rig v;
		v.hit.strike(Kind::Kick, k, 40 + static_cast<uint64_t>(i), 1.0f);
		seen[i] = v.hit.tensionVariationForTest();
		CHECK(seen[i] != 1.0);
	}
	CHECK(seen[0] != seen[1]);
	CHECK(seen[1] != seen[2]);
}

TEST_CASE("a Snare's wire Contour: Exp falls by its T60's factor a sample, Linear by a constant step to zero at the T60, Mod chopped by a 45 Hz falling saw") {
	Laws l = snareLaws();
	l.wireT60 = 0.1;
	float x = 0.0f;
	// EXPONENTIAL: each sample's envelope is the last times 10^(−3 / (T60 · sr)).
	{
		Rig r;
		l.wireContour = 0;
		r.hit.strike(Kind::Snare, l, 3, 1.0f);
		r.hit.render(&x, 1);
		const double e0 = r.hit.wireEnvForTest();
		REQUIRE(e0 > 0.0);
		render(r.hit, 99, 1);
		CHECK(std::abs(r.hit.wireEnvForTest() / e0 - std::pow(10.0, -3.0 * 99.0 / (0.1 * kSr))) < 1.0e-4);
	}
	// LINEAR: a straight fall from where it starts to exactly zero after T60 · sr samples.
	{
		Rig r;
		l.wireContour = 1;
		r.hit.strike(Kind::Snare, l, 3, 1.0f);
		r.hit.render(&x, 1);
		const double e0 = r.hit.wireEnvForTest();
		REQUIRE(e0 > 0.0);
		const double step = e0 / (0.1 * kSr);
		render(r.hit, 2400, 1); // 0.05 s: half way down
		CHECK(std::abs(r.hit.wireEnvForTest() - (e0 - 2400.0 * step)) < 1.0e-4 * e0);
		render(r.hit, 2400 + 16, 1); // past the T60: nothing
		CHECK(r.hit.wireEnvForTest() == 0.0f);
	}
	// MODULATED: the exponential times (1 − 0.8 · phase), the phase rising 45 / sr a sample and wrapping.
	{
		Rig r;
		Rig e;
		l.wireContour = 2;
		r.hit.strike(Kind::Snare, l, 3, 1.0f);
		l.wireContour = 0;
		e.hit.strike(Kind::Snare, l, 3, 1.0f);
		for (const int n : { 1, 300, 800 }) {
			CAPTURE(n);
			Rig a;
			Rig b;
			l.wireContour = 2;
			a.hit.strike(Kind::Snare, l, 3, 1.0f);
			l.wireContour = 0;
			b.hit.strike(Kind::Snare, l, 3, 1.0f);
			render(a.hit, static_cast<size_t>(n), 1);
			render(b.hit, static_cast<size_t>(n), 1);
			double phase = std::fmod(45.0 * static_cast<double>(n - 1) / kSr, 1.0);
			CHECK(std::abs(a.hit.wireEnvForTest() - b.hit.wireEnvForTest() * (1.0 - 0.8 * phase)) < 1.0e-4 * b.hit.wireEnvForTest() + 1.0e-7);
		}
	}
	CHECK(kindControls(Kind::Snare).wireContour);
	CHECK_FALSE(kindControls(Kind::Membrane).wireContour);
}

TEST_CASE("a Snare's Buzz: its wires follow the head's rectified swing past Threshold, (1 - Buzz) + Buzz * clamp((follow - knee) / span)") {
	for (const double thr : { 0.0, 0.3, 0.8 }) {
		CAPTURE(thr);
		Laws l = snareLaws();
		l.buzz = 0.7;
		l.buzzThreshold = thr;
		Rig r;
		r.hit.strike(Kind::Snare, l, 3, 1.0f);
		// The follower restated from the head the hit reports: a one-pole of |head| with a 1 ms time constant.
		const double coef = 1.0 - std::exp(-1.0 / (0.001 * kSr));
		double follow = 0.0;
		float x = 0.0f;
		double worst = 0.0;
		double lo = 1.0;
		for (int i = 0; i < 4800; ++i) {
			r.hit.render(&x, 1);
			follow += coef * (std::abs(static_cast<double>(r.hit.headForTest())) - follow);
			const double knee = 0.5 * thr;
			const double span = std::max(1.0e-3, 0.5 * (1.0 - thr));
			const double g = std::clamp((follow - knee) / span, 0.0, 1.0);
			worst = std::max(worst, std::abs(static_cast<double>(r.hit.buzzForTest()) - ((1.0 - 0.7) + 0.7 * g)));
			lo = std::min(lo, static_cast<double>(r.hit.buzzForTest()));
		}
		CHECK(worst < 1.0e-4);
		CHECK(lo < 0.95); // it did move the wires (not vacuous)
	}
}

TEST_CASE("Width: the left ear is the mono hit to the bit; the right differs by its own decorrelated noise on a Snare's wires or a Clap's band") {
	for (const Kind k : { Kind::Snare, Kind::Clap }) {
		CAPTURE(static_cast<int>(k));
		Laws l = k == Kind::Snare ? snareLaws() : baseLaws(1000.0, 0.3);
		Rig mono;
		mono.hit.strike(k, l, 9, 1.0f);
		const std::vector<float> m = render(mono.hit, 2400);
		l.width = 0.8;
		Rig st;
		st.hit.strike(k, l, 9, 1.0f);
		std::vector<float> left(2400);
		std::vector<float> right(2400);
		double side = 0.0;
		for (size_t i = 0; i < left.size(); ++i) {
			st.hit.render(&left[i], 1, &right[i]);
			CHECK(right[i] - left[i] == doctest::Approx(st.hit.sideForTest()).epsilon(1.0e-5));
			side += static_cast<double>(right[i] - left[i]) * static_cast<double>(right[i] - left[i]);
		}
		CHECK(left == m);   // the left ear is the mono hit, bit for bit
		CHECK(side > 1.0e-4); // and the right does differ
		// At no Width the two ears are one.
		l.width = 0.0;
		Rig none;
		none.hit.strike(k, l, 9, 1.0f);
		bool same = true;
		for (size_t i = 0; i < 512; ++i) {
			float a = 0.0f;
			float b = 0.0f;
			none.hit.render(&a, 1, &b);
			same = same && a == b;
		}
		CHECK(same);
	}
	CHECK(kindControls(Kind::Clap).width);
	CHECK_FALSE(kindControls(Kind::Kick).width);
}

TEST_CASE("Width's ears correlate as cos(Width * pi/2): a Clap's band, all noise, measured over its burst") {
	for (const double w : { 0.0, 0.5, 1.0 }) {
		CAPTURE(w);
		Laws l = baseLaws(1000.0, 0.4);
		l.width = w;
		l.count = 1;
		Rig r;
		r.hit.strike(Kind::Clap, l, 17, 1.0f);
		double ll = 0.0;
		double rr = 0.0;
		double lr = 0.0;
		for (int i = 0; i < 9600; ++i) {
			float a = 0.0f;
			float b = 0.0f;
			r.hit.render(&a, 1, &b);
			ll += static_cast<double>(a) * a;
			rr += static_cast<double>(b) * b;
			lr += static_cast<double>(a) * b;
		}
		const double corr = lr / std::sqrt(ll * rr);
		CHECK(std::abs(corr - std::cos(w * std::numbers::pi / 2.0)) < 0.1);
	}
}

TEST_CASE("a Snare's head takes the Membrane's strike-position Tone: its modes stand to its fundamental as the Membrane's do") {
	for (const float tone : { 0.1f, 0.5f, 0.9f }) {
		CAPTURE(tone);
		Laws l = baseLaws(200.0, 0.4);
		l.tone = tone;
		Rig m;
		Rig s;
		m.hit.strike(Kind::Membrane, l, 8, 1.0f);
		s.hit.strike(Kind::Snare, l, 8, 1.0f);
		for (int k = 1; k < kSnareHeadModes; ++k) {
			CAPTURE(k);
			CHECK(std::abs(s.hit.modeAmplitude(k) / s.hit.modeAmplitude(0) - m.hit.modeAmplitude(k) / m.hit.modeAmplitude(0)) < 1.0e-5f);
		}
	}
}

TEST_CASE("the zone controls (drum proposal §14): each zone's anchor at its stop, a straight line between two, a choice switching at the midpoint, a lap per 1.0") {
	// THE ANCHORS, written out from the proposal's tables (not read back from the code under test).
	struct Wire {
		int contour;
		double wireShape, bodyShape, buzz, threshold;
	};
	const Wire wires[4] { { 0, 0.0, 0.0, 0.0, 0.5 }, { 1, 0.0, 0.0, 0.3, 0.7 }, { 2, 0.0, 0.0, 1.0, 0.3 }, { 1, 0.45, 0.35, 0.4, 0.5 } };
	for (int z = 0; z < 4; ++z) {
		CAPTURE(z);
		const WiresMix m = wiresAt(static_cast<double>(z) / 4.0);
		CHECK(m.contour == wires[z].contour);
		CHECK(m.wireShape == doctest::Approx(wires[z].wireShape));
		CHECK(m.bodyShape == doctest::Approx(wires[z].bodyShape));
		CHECK(m.buzz == doctest::Approx(wires[z].buzz));
		CHECK(m.threshold == doctest::Approx(wires[z].threshold));
		// A LAP LATER, AND A LAP EARLIER, the same zone.
		CHECK(wiresAt(static_cast<double>(z) / 4.0 + 1.0) == m);
		CHECK(wiresAt(static_cast<double>(z) / 4.0 - 2.0) == m);
	}
	// BETWEEN Buzzy and Gated, a quarter across: a quarter of the way on every continuous param; the Contour still Buzzy's.
	const WiresMix q = wiresAt(0.5 + 0.25 / 4.0);
	CHECK(q.buzz == doctest::Approx(1.0 + 0.25 * (0.4 - 1.0)));
	CHECK(q.wireShape == doctest::Approx(0.25 * 0.45));
	CHECK(q.contour == 2);
	CHECK(wiresAt(0.5 + 0.75 / 4.0).contour == 1); // past the midpoint, Gated's
	// THE BELLS: Off, Body, Shell, Ring, Knock.
	const double level1[5] { 0.0, 0.5, 0.5, 0.4, 0.6 };
	const double level2[5] { 0.0, 0.0, 0.35, 0.3, 0.4 };
	const double scale[5] { 1.0, 1.0, 1.0, 3.0, 0.6 };
	const bool linked[5] { true, true, true, true, false };
	for (int z = 0; z < 5; ++z) {
		CAPTURE(z);
		const BellsMix b = bellsAt(static_cast<double>(z) / 5.0);
		CHECK(b.level1 == doctest::Approx(level1[z]));
		CHECK(b.level2 == doctest::Approx(level2[z]));
		CHECK(b.q1Scale == doctest::Approx(scale[z]));
		CHECK(b.linked == linked[z]);
	}
	// THE VELOCITY ZONE: Level (0, 0), Tone (0.5, 0), Tension (0.5, 1); its default (2/3) is today's defaults.
	CHECK(velocityAt(0.0) == VelocityMix { 0.0, 0.0 });
	CHECK(velocityAt(1.0 / 3.0).tone == doctest::Approx(0.5));
	CHECK(velocityAt(1.0 / 3.0).tension == doctest::Approx(0.0));
	CHECK(velocityAt(2.0 / 3.0).tone == doctest::Approx(0.5));
	CHECK(velocityAt(2.0 / 3.0).tension == doctest::Approx(1.0));
	Controls defaults;
	CHECK(velocityAt(defaults.velocityZone).tension == doctest::Approx(1.0));
	// THE YIELDS (lever 1), written out: wire sweep = Sweep × 1 octave; Q from Snap; a linked ratio from Metal.
	CHECK(wireSweepFor(0.4f) == doctest::Approx(0.4));
	CHECK(bellQFor(0.5f, 3.0) == doctest::Approx((2.0 + 5.0) * 3.0));
	CHECK(bell2QFor(0.5f, 1.0) == doctest::Approx(1.0 + 3.0));
	CHECK(bell2RatioFor(0.25f) == doctest::Approx(1.3 + 0.3));
	// AND lawsFor READS THEM: a Snare at Buzzy and Ring.
	Fields f;
	f.snap = 0.5f;
	f.metal = 0.25f;
	f.sweep = 0.6f;
	Controls c;
	c.wires = 0.5;
	c.bells = 3.0 / 5.0;
	const Laws l = lawsFor(Kind::Snare, f, c, 180.0, 0.5); // velocity 0.5: Snap and Tone as the field
	CHECK(l.wireContour == 2);
	CHECK(l.buzz == doctest::Approx(1.0));
	CHECK(l.bellLevel == doctest::Approx(0.4));
	CHECK(l.bellQ == doctest::Approx((2.0 + 10.0 * static_cast<double>(l.snap)) * 3.0));
	CHECK(l.bell2Ratio == doctest::Approx(1.3 + 1.2 * 0.25));
	CHECK(l.wireSweepOctaves == doctest::Approx(0.6));
}
