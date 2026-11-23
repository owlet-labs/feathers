/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "platform_pin.h"
#include "doctest.h"
#include "test_heap.h"

#include "fx/balance/balance_types.h"
#include "fx/owltt/comb_ratio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

namespace bal = feathers::fx::balance;
namespace ow = feathers::fx::owltt;

// COUNTS THE ALLOCATIONS made while `gCounting` is set: the "no allocation after prepare" case reads it.
namespace {
std::atomic<size_t> gAllocations { 0 };
std::atomic<bool> gCounting { false };
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) {
	if (gCounting.load(std::memory_order_relaxed)) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = feathers::test::heapAlloc(n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void* operator new(std::size_t n, std::align_val_t al) {
	if (gCounting.load(std::memory_order_relaxed)) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = feathers::test::heapAlignedAlloc(static_cast<std::size_t>(al), n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

/// AN OPS INSTANCE, CONSTRUCTED THE WAY A HOST CONSTRUCTS ONE.
class Instance {
public:
	Instance() {
		REQUIRE(bal::kAudioOps.stateBytes <= storage_.size());
		bal::kAudioOps.construct(storage_.data());
		bal::kAudioOps.prepare(storage_.data(), kRate, 4096);
		bal::kAudioOps.setReadingsSink(storage_.data(), feathers::device::ReadingsSink { this, [](void* c, const float* v, size_t n) {
			                                                                       auto* self = static_cast<Instance*>(c);
			                                                                       for (size_t i = 0; i < std::min(n, self->readings_.size()); ++i) {
				                                                                       self->readings_[i] = v[i];
			                                                                       }
		                                                                       } });
	}
	~Instance() { bal::kAudioOps.destruct(storage_.data()); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const bal::Settings& s) {
		const bal::Row row = bal::rowOf(s);
		bal::kAudioOps.setFields(storage_.data(), row.data(), row.size());
	}
	void process(float* l, float* r, int frames) { bal::kAudioOps.processStereo(storage_.data(), l, r, frames); }
	void note(float semitones) {
		feathers::device::NoteContext n;
		n.pitchSemitones = semitones;
		n.gate = true;
		bal::kAudioOps.setNoteContext(storage_.data(), n);
	}
	[[nodiscard]] bool holdsTail() const { return bal::kAudioOps.holdsTail(storage_.data()); }
	[[nodiscard]] float reading(bal::Reading r) const { return readings_[static_cast<size_t>(r)]; }
	[[nodiscard]] float point(size_t channel, size_t pair, size_t side) const { return readings_[bal::pointIndex(channel, pair, side)]; }
	[[nodiscard]] float imbalance(size_t band) const { return bal::imbalanceForTest(storage_.data(), band); }
	[[nodiscard]] bal::SideGains gains(size_t band) const { return bal::gainsForTest(storage_.data(), band); }

private:
	alignas(std::max_align_t) std::array<std::byte, 16384> storage_ {};
	std::array<float, 256> readings_ {};
};

/// Fast settings for a case that settles quickly: short Attack and Release, the strongest correction.
[[nodiscard]] bal::Settings quick(int bands = 2, float strength = 1.0f) {
	bal::Settings s;
	s.bands = bands;
	s.strength = strength;
	s.focus = 1.0f; // per band to the centre: the correction these cases were written for
	s.attack = static_cast<float>(bal::kAttackMinLog2);
	s.release = static_cast<float>(bal::kReleaseMinLog2);
	return s;
}

/// A STEREO BLOCK of sines, each with its own amplitude per side. `phase` carries across blocks.
struct Tones {
	struct Tone {
		double hz;
		double left;
		double right;
	};
	std::vector<Tone> tones;
	double t = 0.0; // samples
	void fill(std::vector<float>& l, std::vector<float>& r) {
		for (size_t i = 0; i < l.size(); ++i) {
			double a = 0.0;
			double b = 0.0;
			for (const Tone& tone : tones) {
				const double s = std::sin(kTwoPi * tone.hz * (t + static_cast<double>(i)) / kRate);
				a += tone.left * s;
				b += tone.right * s;
			}
			l[i] = static_cast<float>(a);
			r[i] = static_cast<float>(b);
		}
		t += static_cast<double>(l.size());
	}
};

/// Runs `seconds` of the tones through `inst`, in 512-sample blocks; returns the LAST `tail` seconds of output.
struct Rendered {
	std::vector<float> l;
	std::vector<float> r;
	std::vector<float> inL;
	std::vector<float> inR;
};
[[nodiscard]] Rendered run(Instance& inst, Tones& tones, double seconds, double tail) {
	Rendered out;
	const auto total = static_cast<size_t>(seconds * kRate);
	const auto keep = static_cast<size_t>(tail * kRate);
	std::vector<float> l(512);
	std::vector<float> r(512);
	for (size_t done = 0; done < total;) {
		const size_t block = std::min<size_t>(512, total - done);
		l.resize(block);
		r.resize(block);
		tones.fill(l, r);
		const std::vector<float> dryL = l;
		const std::vector<float> dryR = r;
		inst.process(l.data(), r.data(), static_cast<int>(l.size()));
		if (done + l.size() + keep > total) {
			out.l.insert(out.l.end(), l.begin(), l.end());
			out.r.insert(out.r.end(), r.begin(), r.end());
			out.inL.insert(out.inL.end(), dryL.begin(), dryL.end());
			out.inR.insert(out.inR.end(), dryR.begin(), dryR.end());
		}
		done += block;
	}
	return out;
}

[[nodiscard]] double rms(const std::vector<float>& v) {
	double s = 0.0;
	for (const float x : v) {
		s += static_cast<double>(x) * static_cast<double>(x);
	}
	return std::sqrt(s / static_cast<double>(std::max<size_t>(v.size(), 1)));
}
[[nodiscard]] double db(double ratio) { return 20.0 * std::log10(ratio); }
/// THE L/R DIFFERENCE of an output, dB (positive = left louder).
[[nodiscard]] double lrDb(const Rendered& r) { return db(rms(r.l) / rms(r.r)); }

/// The amplitude of the sine at `hz` in `v` (a single-bin DFT; `v` is whole cycles or long enough that the leakage is negligible).
[[nodiscard]] double amplitudeAt(const std::vector<float>& v, double hz, double startSample = 0.0) {
	double re = 0.0;
	double im = 0.0;
	for (size_t i = 0; i < v.size(); ++i) {
		const double ph = kTwoPi * hz * (startSample + static_cast<double>(i)) / kRate;
		re += static_cast<double>(v[i]) * std::cos(ph);
		im += static_cast<double>(v[i]) * std::sin(ph);
	}
	return 2.0 * std::hypot(re, im) / static_cast<double>(v.size());
}

//------------------------------------------------------------------------------------------------//
// THE LAWS                                                                                        //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the correction's gains multiply to one and bring the sides to one level at full Strength") {
	for (const float d : { -2.0f, -0.7f, 0.0f, 0.3f, 1.386f, 3.0f }) {
		for (const float s : { 0.0f, 0.25f, 0.5f, 1.0f }) {
			const bal::SideGains g = bal::correctionGains(d, s);
			CAPTURE(d);
			CAPTURE(s);
			CHECK(std::fabs(g.left * g.right - 1.0f) < 2.0e-6f);
			// THE LEVEL DIFFERENCE AFTER, restated: the sides' ratio e^d, times the gains' ratio, is e^{(1 - s) d}.
			const double after = std::log(std::exp(static_cast<double>(d)) * static_cast<double>(g.left) / static_cast<double>(g.right));
			CHECK(after == doctest::Approx((1.0 - static_cast<double>(s)) * static_cast<double>(d)).epsilon(1.0e-5).scale(1.0));
		}
	}
	CHECK(bal::correctionGains(1.0f, 0.0f).left == 1.0f);
	CHECK(bal::correctionGains(1.0f, 0.0f).right == 1.0f);
}

TEST_CASE("the equal-power Pan: centre is exactly untouched, the two powers always add to 2, the ends are one side") {
	CHECK(bal::panGains(0.0f).left == 1.0f);
	CHECK(bal::panGains(0.0f).right == 1.0f);
	for (float p = -1.0f; p <= 1.0f; p += 0.125f) {
		const bal::SideGains g = bal::panGains(p);
		CAPTURE(p);
		CHECK(std::fabs(g.left * g.left + g.right * g.right - 2.0f) < 1.0e-5f);
		// THE LAW RESTATED in doubles: sqrt(2) cos / sin of (1 + p) quarter turns of a half pi.
		const double a = (1.0 + static_cast<double>(p)) * 3.14159265358979323846 / 4.0;
		CHECK(static_cast<double>(g.left) == doctest::Approx(std::sqrt(2.0) * std::cos(a)).epsilon(1.0e-5).scale(1.0));
		CHECK(static_cast<double>(g.right) == doctest::Approx(std::sqrt(2.0) * std::sin(a)).epsilon(1.0e-5).scale(1.0));
	}
	CHECK(bal::panGains(1.0f).left == doctest::Approx(0.0).scale(1.0).epsilon(1.0e-6));
	CHECK(bal::panGains(-1.0f).right == doctest::Approx(0.0).scale(1.0).epsilon(1.0e-6));
}

TEST_CASE("the Pan moves the rendered level by the law, at Strength 0, and 0 leaves the band sum alone") {
	for (const float pan : { -0.6f, 0.0f, 0.4f, 1.0f }) {
		Instance inst;
		bal::Settings s = quick(2, 0.0f);
		s.pan = pan;
		inst.set(s);
		Tones tones { { { 400.0, 0.5, 0.5 } } };
		const Rendered r = run(inst, tones, 0.4, 0.2);
		const double a = (1.0 + static_cast<double>(pan)) * 3.14159265358979323846 / 4.0;
		CAPTURE(pan);
		CHECK(rms(r.l) / rms(r.inL) == doctest::Approx(std::sqrt(2.0) * std::cos(a)).epsilon(2.0e-3).scale(1.0));
		CHECK(rms(r.r) / rms(r.inR) == doctest::Approx(std::sqrt(2.0) * std::sin(a)).epsilon(2.0e-3).scale(1.0));
	}
}

//------------------------------------------------------------------------------------------------//
// THE CORRECTION                                                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a band panned hard to one side is pulled to the centre at 100% and half-way at 50%") {
	// 400 Hz on a 200 Hz comb sits on the second tooth: all of it is in the on-teeth band. Left is 12.04 dB over right.
	const double before = db(1.0 / 0.25);
	for (const float strength : { 1.0f, 0.5f, 0.0f }) {
		Instance inst;
		inst.set(quick(2, strength));
		Tones tones { { { 400.0, 1.0, 0.25 } } };
		const Rendered r = run(inst, tones, 3.0, 0.5);
		CAPTURE(strength);
		// THE EXPECTED DIFFERENCE, from the law: (1 - S) of the original, in dB.
		CHECK(std::fabs(lrDb(r) - before * (1.0 - static_cast<double>(strength))) < 0.5);
	}
	// AND THE OTHER SIDE: right-heavy goes the other way, to the same place.
	Instance inst;
	inst.set(quick(2, 1.0f));
	Tones tones { { { 400.0, 0.25, 1.0 } } };
	const Rendered r = run(inst, tones, 3.0, 0.5);
	CHECK(std::fabs(lrDb(r)) < 0.5);
}

TEST_CASE("the band's overall level does not move: the geometric mean of the two sides is kept") {
	Instance inst;
	inst.set(quick(2, 1.0f));
	Tones tones { { { 400.0, 1.0, 0.25 } } };
	const Rendered r = run(inst, tones, 3.0, 0.5);
	const double before = std::sqrt(rms(r.inL) * rms(r.inR));
	const double after = std::sqrt(rms(r.l) * rms(r.r));
	CHECK(std::fabs(db(after / before)) < 0.3);
}

TEST_CASE("an imbalance in one band does not touch another") {
	// THREE BANDS AT 200 / 400 Hz TEETH: 200 Hz is on the first comb's tooth and off the second's (Odd), 400 Hz is on both (Even).
	Instance inst;
	inst.set(quick(3, 1.0f));
	Tones tones { { { 200.0, 1.0, 0.25 }, { 400.0, 0.5, 0.5 } } };
	const Rendered r = run(inst, tones, 3.0, 0.5);
	// THE ODD BAND'S DETECTOR SEES ln 4; THE EVEN BAND'S SEES NOTHING, and its gains are one.
	CHECK(inst.imbalance(1) == doctest::Approx(std::log(4.0)).epsilon(0.03).scale(1.0));
	CHECK(std::fabs(inst.imbalance(2)) < 0.03f);
	CHECK(std::fabs(inst.gains(2).left - 1.0f) < 0.02f);
	CHECK(std::fabs(inst.gains(2).right - 1.0f) < 0.02f);
	// THE OUTPUT: the 200 Hz tone's sides are level, the 400 Hz tone's amplitude is what went in.
	const double startSample = static_cast<double>(static_cast<size_t>(3.0 * kRate) - r.l.size());
	const double a200l = amplitudeAt(r.l, 200.0, std::fmod(startSample, kRate));
	const double a200r = amplitudeAt(r.r, 200.0, std::fmod(startSample, kRate));
	CHECK(std::fabs(db(a200l / a200r)) < 0.5);
	CHECK(amplitudeAt(r.l, 400.0, std::fmod(startSample, kRate)) == doctest::Approx(0.5).epsilon(0.01).scale(1.0));
	CHECK(amplitudeAt(r.r, 400.0, std::fmod(startSample, kRate)) == doctest::Approx(0.5).epsilon(0.01).scale(1.0));
}

//------------------------------------------------------------------------------------------------//
// ATTACK AND RELEASE                                                                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Release is how fast the measurement follows a side getting quieter, Attack louder: the detector against the exponential") {
	bal::Settings s = quick(2, 1.0f);
	s.attack = static_cast<float>(std::log2(0.1)); // 100 ms
	s.release = static_cast<float>(std::log2(1.0)); // 1 s
	Instance inst;
	inst.set(s);
	// SETTLE BALANCED (0.5 each side), then the right side falls to a quarter: its power to 1/16.
	Tones tones { { { 400.0, 0.5, 0.5 } } };
	(void)run(inst, tones, 6.0, 0.0);
	REQUIRE(std::fabs(inst.imbalance(1)) < 0.01f);
	tones.tones[0].right = 0.125;
	(void)run(inst, tones, 1.0, 0.0);
	// THE DETECTOR IS TWO LAGS IN A ROW: a 30 ms mean square, then the ballistics. Falling from P to P/16: e(t) = a + (1 - a) (tr e^{-t/tr} - tm e^{-t/tm}) / (tr - tm), a = 1/16.
	const double tm = 0.030;
	const double a = 1.0 / 16.0;
	// THE COMB DELAYS THE ON-TEETH BAND BY HALF ITS DELAY ON AVERAGE (a step arrives as two half steps D apart): 200 Hz spacing, D = 5 ms.
	const double lag = 0.5 / 200.0;
	const auto cascade = [&](double t, double tau) { return (tau * std::exp(-t / tau) - tm * std::exp(-t / tm)) / (tau - tm); };
	const double expectedRelease = 0.5 * std::log(1.0 / (a + (1.0 - a) * cascade(1.0 - lag, 1.0)));
	CHECK(static_cast<double>(inst.imbalance(1)) == doctest::Approx(expectedRelease).epsilon(0.01).scale(1.0));
	// THEN THE RIGHT SIDE COMES BACK: it rises at the Attack, 100 ms, through the same 30 ms window: e(t) = 1 - (1 - a) (ta e^{-t/ta} - tm e^{-t/tm}) / (ta - tm).
	(void)run(inst, tones, 12.0, 0.0); // settle the fall completely (e^-12)
	tones.tones[0].right = 0.5;
	(void)run(inst, tones, 0.1, 0.0);
	const double expectedAttack = 0.5 * std::log(1.0 / (1.0 - (1.0 - a) * cascade(0.1 - lag, 0.1)));
	CHECK(static_cast<double>(inst.imbalance(1)) == doctest::Approx(expectedAttack).epsilon(0.01).scale(1.0));
}

//------------------------------------------------------------------------------------------------//
// THE BANDS SUM, THE LOWS, MONO                                                                   //
//------------------------------------------------------------------------------------------------//

namespace {
void noise(std::vector<float>& l, std::vector<float>& r, uint32_t& seed, float left, float right) {
	for (size_t i = 0; i < l.size(); ++i) {
		seed = seed * 1664525u + 1013904223u;
		l[i] = left * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		seed = seed * 1664525u + 1013904223u;
		r[i] = right * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
	}
}
} // namespace

TEST_CASE("at Strength 0 the bands sum back to the input, in two and three bands, plain and sharp") {
	for (const int bands : { 2, 3 }) {
		for (const bool sharp : { false, true }) {
			Instance inst;
			bal::Settings s = quick(bands, 0.0f);
			s.sharp = sharp;
			inst.set(s);
			uint32_t seed = 12345u;
			float worst = 0.0f;
			std::vector<float> l(512);
			std::vector<float> r(512);
			for (int block = 0; block < 200; ++block) {
				noise(l, r, seed, 0.9f, 0.3f);
				const std::vector<float> dl = l;
				const std::vector<float> dr = r;
				inst.process(l.data(), r.data(), 512);
				for (size_t i = 0; i < l.size(); ++i) {
					worst = std::max({ worst, std::fabs(l[i] - dl[i]), std::fabs(r[i] - dr[i]) });
				}
			}
			CAPTURE(bands);
			CAPTURE(sharp);
			// THE BOUND: the bands are residuals, so their sum is the input to a few float roundings of a signal of 0.9.
			CHECK(worst < 5.0e-7f);
		}
	}
}

TEST_CASE("a mono input passes untouched at any Strength: the detectors see no imbalance, so the gains are exactly one") {
	Instance inst;
	inst.set(quick(3, 1.0f));
	uint32_t seed = 99u;
	std::vector<float> l(512);
	std::vector<float> r(512);
	float worst = 0.0f;
	for (int block = 0; block < 100; ++block) {
		noise(l, r, seed, 0.7f, 0.7f);
		r = l;
		const std::vector<float> dry = l;
		inst.process(l.data(), r.data(), 512);
		for (size_t i = 0; i < l.size(); ++i) {
			worst = std::max(worst, std::fabs(l[i] - dry[i]));
			CHECK(l[i] == r[i]);
		}
	}
	CHECK(worst < 5.0e-7f);
	for (size_t b = 0; b < 3; ++b) {
		CHECK(inst.gains(b).left == 1.0f);
		CHECK(inst.gains(b).right == 1.0f);
	}
}

TEST_CASE("HP Wet keeps the lows out of the detectors, and Sub Thru gives them back untouched") {
	// A 60 Hz TONE HARD LEFT UNDER TWO LEVEL TONES, one on a tooth of the 200 Hz comb (2400) and one between two (2500): with the highpass at 500 Hz the detectors
	// hear only the level ones, in both bands. THE CONTROL: with no highpass the same input is a large imbalance in the band that holds the 60 Hz.
	{
		Instance open;
		open.set(quick(2, 1.0f));
		Tones tones { { { 60.0, 1.0, 0.0 }, { 2400.0, 0.4, 0.4 }, { 2500.0, 0.4, 0.4 } } };
		(void)run(open, tones, 3.0, 0.1);
		CHECK(std::max(std::fabs(open.imbalance(0)), std::fabs(open.imbalance(1))) > 0.5f);
	}
	for (const bool sub : { true, false }) {
		Instance inst;
		bal::Settings s = quick(2, 1.0f);
		s.hpWet = static_cast<float>(std::log2(500.0));
		s.subThru = sub;
		inst.set(s);
		Tones tones { { { 60.0, 1.0, 0.0 }, { 2400.0, 0.4, 0.4 }, { 2500.0, 0.4, 0.4 } } };
		const Rendered r = run(inst, tones, 3.0, 1.0);
		CAPTURE(sub);
		for (size_t band = 0; band < 2; ++band) {
			CHECK(std::fabs(inst.imbalance(band)) < 0.02f);
		}
		const double start = std::fmod(static_cast<double>(static_cast<size_t>(3.0 * kRate) - r.l.size()), kRate);
		const double low = amplitudeAt(r.l, 60.0, start);
		if (sub) {
			CHECK(low == doctest::Approx(1.0).epsilon(0.01).scale(1.0));
			CHECK(amplitudeAt(r.r, 60.0, start) < 0.01);
		}
		else {
			// TAKEN OUT: the two-pole 500 Hz highpass is about -37 dB at 60 Hz.
			CHECK(low < 0.05);
		}
		CHECK(amplitudeAt(r.l, 2400.0, start) == doctest::Approx(0.4).epsilon(0.02).scale(1.0));
		CHECK(amplitudeAt(r.l, 2500.0, start) == doctest::Approx(0.4).epsilon(0.02).scale(1.0));
	}
}

TEST_CASE("Key Track places the comb on the played note, and a knob reads as a ratio to it") {
	Instance inst;
	bal::Settings s = quick(2, 1.0f);
	s.keyTrack = true;
	s.spacing = 400.0f; // x2 of the 200 Hz reference
	inst.set(s);
	inst.note(57.0f); // A3, 220 Hz
	std::vector<float> l(4096, 0.0f);
	std::vector<float> r(4096, 0.0f);
	inst.process(l.data(), r.data(), 4096);
	CHECK(inst.reading(bal::Reading::NoteHz) == doctest::Approx(220.0).epsilon(1.0e-4).scale(1.0));
	CHECK(inst.reading(bal::Reading::Spacing1Hz) == doctest::Approx(440.0).epsilon(2.0e-3).scale(1.0));
	// WITHOUT TRACKING the knob is the spacing.
	Instance plain;
	s.keyTrack = false;
	plain.set(s);
	plain.note(57.0f);
	plain.process(l.data(), r.data(), 4096);
	CHECK(plain.reading(bal::Reading::NoteHz) == 0.0f);
	CHECK(plain.reading(bal::Reading::Spacing1Hz) == doctest::Approx(400.0).epsilon(2.0e-3).scale(1.0));
}

//------------------------------------------------------------------------------------------------//
// TAIL, ALLOCATION, STATE                                                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the tail reaches exact zero, and the device holds it until it does") {
	for (const bool sharp : { false, true }) {
		Instance inst;
		bal::Settings s = quick(3, 1.0f);
		s.sharp = sharp;
		s.pan = 0.3f;
		s.spacing = 20.0f; // the longest delay a comb serves
		s.hpWet = static_cast<float>(std::log2(100.0));
		s.subThru = true;
		inst.set(s);
		std::vector<float> l(512, 0.0f);
		std::vector<float> r(512, 0.0f);
		l[3] = 1.0f; // ONE IMPULSE, then silence: the comb's echo comes a whole delay later
		r[3] = 0.4f;
		inst.process(l.data(), r.data(), 512);
		bool sawEcho = false;
		bool silentBlockWhileHolding = false;
		size_t lastNonzero = 0;
		std::vector<bool> holds;
		for (int block = 1; block < 200; ++block) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			inst.process(l.data(), r.data(), 512);
			bool nonzero = false;
			for (size_t i = 0; i < l.size(); ++i) {
				nonzero = nonzero || l[i] != 0.0f || r[i] != 0.0f;
			}
			if (nonzero) {
				sawEcho = true;
				lastNonzero = static_cast<size_t>(block);
			}
			holds.push_back(inst.holdsTail());
		}
		CAPTURE(sharp);
		REQUIRE(sawEcho);
		// EVERY BLOCK BEFORE THE LAST SOUNDING ONE REPORTED A TAIL — including the silent ones between the impulse and its echo.
		for (size_t b = 0; b + 1 < lastNonzero; ++b) {
			silentBlockWhileHolding = silentBlockWhileHolding || holds[b];
			CHECK(holds[b]);
		}
		CHECK(silentBlockWhileHolding);
		// AND IT LETS GO: after the longest delay of zeros, exact silence and no tail.
		CHECK_FALSE(holds.back());
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		inst.process(l.data(), r.data(), 512);
		for (size_t i = 0; i < l.size(); ++i) {
			REQUIRE(l[i] == 0.0f);
			REQUIRE(r[i] == 0.0f);
		}
	}
}

TEST_CASE("nothing allocates after prepare") {
	Instance inst;
	bal::Settings s = quick(3, 0.7f);
	s.keyTrack = true;
	s.sharp = true;
	s.hpWet = static_cast<float>(std::log2(200.0));
	s.subThru = true;
	s.pan = -0.2f;
	inst.set(s);
	inst.note(60.0f);
	uint32_t seed = 7u;
	std::vector<float> l(512);
	std::vector<float> r(512);
	noise(l, r, seed, 0.5f, 0.2f);
	inst.process(l.data(), r.data(), 512);
	gAllocations = 0;
	gCounting = true;
	for (int block = 0; block < 50; ++block) {
		noise(l, r, seed, 0.5f, 0.2f);
		s.strength = 0.1f + 0.01f * static_cast<float>(block);
		s.spacing = 100.0f + 3.0f * static_cast<float>(block);
		s.bands = block % 7 == 0 ? 2 : 3;
		inst.set(s);
		inst.note(48.0f + static_cast<float>(block % 12));
		inst.process(l.data(), r.data(), 512);
	}
	gCounting = false;
	CHECK(gAllocations.load() == 0u);
}

TEST_CASE("a saved row loads into a fresh instance that renders the same") {
	bal::Settings s = quick(3, 0.8f);
	s.spacing = 310.0f;
	s.spacing2 = 4400.0f;
	s.keyTrack = true;
	s.sharp = true;
	s.pan = 0.25f;
	s.attack = -2.5f;
	s.release = 1.0f;
	s.hpWet = 9.0f;
	s.subThru = true;
	// THE ROW ROUND TRIP, field for field.
	CHECK(bal::stateOf(bal::rowOf(s), bal::rowOf(s).size()) == s);
	// A SECOND INSTANCE LOADED FROM THE SAVED ROW RENDERS THE FIRST'S OUTPUT, BIT FOR BIT (the detectors are not persisted: both start empty).
	Instance a;
	Instance b;
	a.set(s);
	const bal::Row saved = bal::rowOf(s);
	b.set(bal::stateOf(saved, saved.size()));
	uint32_t seed = 5u;
	std::vector<float> l(512);
	std::vector<float> r(512);
	for (int block = 0; block < 20; ++block) {
		noise(l, r, seed, 0.6f, 0.2f);
		std::vector<float> l2 = l;
		std::vector<float> r2 = r;
		a.process(l.data(), r.data(), 512);
		b.process(l2.data(), r2.data(), 512);
		CHECK(l == l2);
		CHECK(r == r2);
	}
	// A FIELD THE ROW LACKS READS AS ITS DECLARED DEFAULT, never zero.
	bal::Row empty {};
	CHECK(bal::stateOf(empty, 0) == bal::Settings {});
}


//------------------------------------------------------------------------------------------------//
// FOCUS, TOLERANCE                                                                                //
//------------------------------------------------------------------------------------------------//

namespace {
/// A TONE'S L/R LEVEL IN dB (positive = left louder) in a rendered tail, and the whole's.
struct Probe {
	double hz;
	double left;
	double right;
};
[[nodiscard]] double toneDb(const Rendered& r, const Probe& p, double startSample) {
	return db(amplitudeAt(r.l, p.hz, startSample) / std::max(amplitudeAt(r.r, p.hz, startSample), 1.0e-9));
}
[[nodiscard]] double inputDb(const Probe& p) { return db(p.left / p.right); }
/// THE WHOLE'S L/R POWER RATIO IN dB for a set of tones (each a sine, so its power is half its amplitude squared).
[[nodiscard]] double wholeDb(const std::vector<Probe>& tones) {
	double l = 0.0;
	double r = 0.0;
	for (const Probe& t : tones) {
		l += t.left * t.left;
		r += t.right * t.right;
	}
	return 10.0 * std::log10(l / r);
}
[[nodiscard]] Rendered renderTones(const bal::Settings& s, const std::vector<Probe>& tones, double& startSample) {
	Instance inst;
	inst.set(s);
	Tones t;
	for (const Probe& p : tones) {
		t.tones.push_back({ p.hz, p.left, p.right });
	}
	const Rendered r = run(inst, t, 3.0, 0.5);
	startSample = std::fmod(static_cast<double>(static_cast<size_t>(3.0 * kRate) - r.l.size()), kRate);
	return r;
}
} // namespace

TEST_CASE("the correction law: the whole's part, Focus of each band's own, outside the Tolerance") {
	// d, dTotal in nepers. Focus 1, no tolerance: the band's own d. Focus 0: the whole's. Between: linear in Focus.
	CHECK(bal::correctedImbalance(0.8f, 0.2f, 1.0f, 0.0f) == doctest::Approx(0.8).scale(1.0));
	CHECK(bal::correctedImbalance(0.8f, 0.2f, 0.0f, 0.0f) == doctest::Approx(0.2).scale(1.0));
	CHECK(bal::correctedImbalance(0.8f, 0.2f, 0.5f, 0.0f) == doctest::Approx(0.5).scale(1.0));
	// A DEAD ZONE ON THE DEVIATION: within it only the whole's part; past it, the excess with its sign.
	CHECK(bal::correctedImbalance(0.4f, 0.2f, 1.0f, 0.3f) == doctest::Approx(0.2).scale(1.0));
	CHECK(bal::correctedImbalance(0.9f, 0.2f, 1.0f, 0.3f) == doctest::Approx(0.2 + 0.4).scale(1.0));
	CHECK(bal::correctedImbalance(-0.3f, 0.2f, 1.0f, 0.3f) == doctest::Approx(0.2 - 0.2).scale(1.0));
	// THE GAINS: Strength scales the whole correction, and a mono band (d = dTotal = 0) is exactly untouched.
	const bal::SideGains full = bal::bandCorrection(0.8f, 0.2f, 1.0f, 1.0f, 0.0f);
	const bal::SideGains half = bal::bandCorrection(0.8f, 0.2f, 0.5f, 1.0f, 0.0f);
	CHECK(std::log(full.left / full.right) == doctest::Approx(-0.8).scale(1.0).epsilon(1e-5));
	CHECK(std::log(half.left / half.right) == doctest::Approx(-0.4).scale(1.0).epsilon(1e-5));
	CHECK(bal::bandCorrection(0.0f, 0.0f, 1.0f, 0.7f, 2.0f).left == 1.0f);
}

TEST_CASE("Focus 0 balances the whole and keeps the bands' relative image, Focus 100% brings every band to the centre") {
	// THREE BANDS AT 200 / 400 HZ TEETH. The whole leans left (the odd and even bands do), but the Between band (300 Hz) leans right.
	const std::vector<Probe> tones { { 200.0, 1.0, 0.5 }, { 400.0, 1.0, 0.5 }, { 300.0, 0.2, 0.4 } };
	const double total = wholeDb(tones);
	REQUIRE(total > 4.0);
	double start = 0.0;
	for (const float strength : { 1.0f, 0.5f }) {
		bal::Settings s = quick(3, strength);
		s.focus = 0.0f;
		const Rendered r = renderTones(s, tones, start);
		CAPTURE(strength);
		// THE OVERALL L/R ENERGY IS BALANCED (at Strength 1; half-way at 0.5), AND EVERY BAND MOVED BY THE SAME dB: its deviation from the rest is kept.
		CHECK(std::fabs(lrDb(r) - total * (1.0 - static_cast<double>(strength))) < 0.35);
		for (const Probe& p : tones) {
			CHECK(toneDb(r, p, start) == doctest::Approx(inputDb(p) - total * static_cast<double>(strength)).epsilon(0.0).scale(1.0).epsilon(0.02));
		}
	}
	// THE BETWEEN BAND STILL LEANS RIGHT OF THE OTHERS at Strength 1: it was 6 dB right, it is.
	bal::Settings s = quick(3, 1.0f);
	s.focus = 0.0f;
	const Rendered r = renderTones(s, tones, start);
	CHECK(toneDb(r, tones[2], start) < toneDb(r, tones[0], start) - 8.0);
	// FOCUS 100%: every band at the centre.
	s.focus = 1.0f;
	const Rendered centred = renderTones(s, tones, start);
	for (const Probe& p : tones) {
		CHECK(std::fabs(toneDb(centred, p, start)) < 0.5);
	}
}

TEST_CASE("Tolerance is a dead zone on a band's deviation from the whole: inside it nothing more, outside it only the excess") {
	// TWO BANDS (300 Hz between the teeth of 200, 400 on them): the 400 leans 1 dB left, the 300 leans 6 dB right. The whole leans about 1.2 dB right.
	const std::vector<Probe> tones { { 400.0, 0.8, 0.8 / std::pow(10.0, 1.0 / 20.0) }, { 300.0, 0.4, 0.8 } };
	const double total = wholeDb(tones);
	const double devOn = inputDb(tones[0]) - total;  // about +2.2: inside 3 dB
	const double devBetween = inputDb(tones[1]) - total; // about -4.8: outside
	REQUIRE(std::fabs(devOn) < 3.0);
	REQUIRE(std::fabs(devBetween) > 3.0);
	double start = 0.0;
	for (const float strength : { 1.0f, 0.5f }) {
		bal::Settings s = quick(2, strength);
		s.focus = 1.0f;
		s.toleranceDb = 3.0f;
		const Rendered r = renderTones(s, tones, start);
		const double sx = static_cast<double>(strength);
		CAPTURE(strength);
		// THE LAW RESTATED: the corrected imbalance is the whole's plus the excess past the tolerance (with its sign); Strength scales that.
		const auto expected = [&](const Probe& p) {
			const double dev = inputDb(p) - total;
			const double excess = std::max(0.0, std::fabs(dev) - 3.0);
			return inputDb(p) - sx * (total + (dev < 0.0 ? -excess : excess));
		};
		for (const Probe& p : tones) {
			CHECK(toneDb(r, p, start) == doctest::Approx(expected(p)).epsilon(0.0).scale(1.0).epsilon(0.02));
		}
		if (strength == 1.0f) {
			// INSIDE: it keeps its deviation from the whole; OUTSIDE: pulled to exactly the tolerance off the (now centred) whole.
			CHECK(toneDb(r, tones[0], start) == doctest::Approx(devOn).scale(1.0).epsilon(0.03));
			CHECK(toneDb(r, tones[1], start) == doctest::Approx(-3.0).scale(1.0).epsilon(0.03));
		}
	}
}

TEST_CASE("the bands' names: harmonic classes only for a tracked note at x1, structural everywhere else") {
	// THE TABLE: (tracking) x (r1 in x1, x1.5, x2) x (r2 in x2, x3, x2.5). Harmonic names need tracking AND r1 = x1 AND r2 = x2 or a whole n >= 3.
	for (const bool tracking : { false, true }) {
		for (const double r1 : { 1.0, 1.5, 2.0 }) {
			for (const double r2 : { 2.0, 3.0, 2.5 }) {
				std::array<std::string, 3> expected { "Gaps", "Teeth A", "Teeth B" };
				if (tracking && r1 == 1.0 && r2 == 2.0) {
					expected = { "Between", "Odd", "Even" };
				}
				else if (tracking && r1 == 1.0 && r2 == 3.0) {
					expected = { "Between", "Others", "Every 3" };
				}
				CAPTURE(tracking);
				CAPTURE(r1);
				CAPTURE(r2);
				CHECK(ow::combBandNames(r1, r2, tracking) == expected);
			}
		}
	}
	// TWO BANDS: x1 is Between / Harmonics, a whole n Rest / Every n, tracked; Gaps / Teeth otherwise.
	CHECK(ow::combBandNames2(1.0, true) == std::array<std::string, 2> { "Between", "Harmonics" });
	CHECK(ow::combBandNames2(3.0, true) == std::array<std::string, 2> { "Rest", "Every 3" });
	CHECK(ow::combBandNames2(1.5, true) == std::array<std::string, 2> { "Gaps", "Teeth" });
	CHECK(ow::combBandNames2(1.0, false) == std::array<std::string, 2> { "Gaps", "Teeth" });
	// THE DEVICE'S OWN READING: Key Track on without a note heard is a fixed comb, and says so.
	bal::Settings s;
	s.bands = 3;
	s.keyTrack = true;
	CHECK(bal::bandNames(s, true)[1] == "Odd");
	CHECK(bal::bandNames(s, false)[1] == "Teeth A");
	s.keyTrack = false;
	CHECK(bal::bandNames(s, true)[1] == "Teeth A");
	s.bands = 2;
	CHECK(bal::bandNames(s, true)[1] == "Teeth");
	s.keyTrack = true;
	CHECK(bal::bandNames(s, true)[1] == "Harmonics");
	// THE CAPTION SAYS WHAT THE TEETH ARE.
	CHECK(ow::combTeethCaption(200.0, false, 1.0, 2.0, true) == "Teeth every 200 Hz; B takes every 2nd tooth");
}


//------------------------------------------------------------------------------------------------//
// ORDER: THE LOWS AND THE PAN                                                                     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Pan places the whole, lows included, and a low-only imbalance is not corrected") {
	for (const float pan : { -1.0f, 0.5f, 0.0f }) {
		bal::Settings s = quick(2, 1.0f);
		s.hpWet = static_cast<float>(std::log2(500.0));
		s.subThru = true;
		s.pan = pan;
		double start = 0.0;
		// A 60 Hz TONE 6 dB LEFT AND NOTHING ELSE: it passes the highpass only as the Sub Thru's lows, so the detectors never act on it.
		const Rendered r = renderTones(s, { { 60.0, 1.0, 0.5 } }, start);
		const bal::SideGains g = bal::panGains(pan);
		CAPTURE(pan);
		CHECK(amplitudeAt(r.l, 60.0, start) == doctest::Approx(1.0 * static_cast<double>(g.left)).epsilon(0.02).scale(1.0));
		CHECK(amplitudeAt(r.r, 60.0, start) == doctest::Approx(0.5 * static_cast<double>(g.right)).epsilon(0.02).scale(1.0));
	}
}

//------------------------------------------------------------------------------------------------//
// WIDEN                                                                                           //
//------------------------------------------------------------------------------------------------//

namespace {
[[nodiscard]] double correlation(const std::vector<float>& a, const std::vector<float>& b) {
	double ab = 0.0;
	double aa = 0.0;
	double bb = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		ab += static_cast<double>(a[i]) * static_cast<double>(b[i]);
		aa += static_cast<double>(a[i]) * static_cast<double>(a[i]);
		bb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
	}
	return ab / std::sqrt(aa * bb);
}
/// A CENTRED SAW at `hz`, or NOISE: filled into both channels.
void centred(std::vector<float>& l, std::vector<float>& r, bool saw, double hz, double& phase, uint32_t& seed) {
	for (size_t i = 0; i < l.size(); ++i) {
		float v;
		if (saw) {
			phase += hz / kRate;
			phase -= std::floor(phase);
			v = static_cast<float>(0.5 * (2.0 * phase - 1.0));
		}
		else {
			seed = seed * 1664525u + 1013904223u;
			v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
		l[i] = v;
		r[i] = v;
	}
}
struct WidenRun {
	std::vector<float> l, r;
};
[[nodiscard]] WidenRun renderCentred(const bal::Settings& s, bool saw, double seconds, double tail) {
	Instance inst;
	inst.set(s);
	double phase = 0.0;
	uint32_t seed = 4242u;
	WidenRun out;
	std::vector<float> l(512);
	std::vector<float> r(512);
	const auto blocks = static_cast<size_t>(seconds * kRate / 512.0);
	for (size_t b = 0; b < blocks; ++b) {
		centred(l, r, saw, 110.0, phase, seed);
		inst.process(l.data(), r.data(), 512);
		if (static_cast<double>(b * 512) >= (seconds - tail) * kRate) {
			out.l.insert(out.l.end(), l.begin(), l.end());
			out.r.insert(out.r.end(), r.begin(), r.end());
		}
	}
	return out;
}
[[nodiscard]] bal::Settings widenSettings(float widen, int bands = 3) {
	bal::Settings s = quick(bands, 0.0f);
	s.spacing = 110.0f; // the saw's own harmonics: odd ones in Odd, even ones in Even
	s.widen = widen;
	return s;
}
} // namespace

TEST_CASE("Widen is mono-safe: left plus right is what it was, for any input and any amount") {
	for (const int bands : { 2, 3 }) {
		for (const float widen : { 0.3f, 1.0f }) {
			Instance wide;
			Instance flat;
			bal::Settings s = quick(bands, 0.6f);
			s.widen = widen;
			wide.set(s);
			s.widen = 0.0f;
			flat.set(s);
			uint32_t seed = 31u;
			std::vector<float> l(512), r(512);
			float worst = 0.0f;
			for (int block = 0; block < 100; ++block) {
				noise(l, r, seed, 0.9f, 0.3f); // an unequal stereo input, so it is not a property of a centred one
				std::vector<float> l2 = l, r2 = r;
				wide.process(l.data(), r.data(), 512);
				flat.process(l2.data(), r2.data(), 512);
				for (size_t i = 0; i < l.size(); ++i) {
					worst = std::max(worst, std::fabs((l[i] + r[i]) - (l2[i] + r2[i])));
				}
			}
			CAPTURE(bands);
			CAPTURE(widen);
			CHECK(worst < 2.0e-6f);
		}
	}
}

TEST_CASE("Widen stays level from side to side: a centred saw and noise within 1 dB at 100%") {
	for (const bool saw : { true, false }) {
		const WidenRun r = renderCentred(widenSettings(1.0f), saw, 4.0, 1.0);
		CAPTURE(saw);
		const double lr = db(rms(r.l) / rms(r.r));
		CHECK(std::fabs(lr) < 1.0);
		// NON-VACUOUS: the sides are not the same signal any more.
		CHECK(correlation(r.l, r.r) < 0.9);
	}
}

TEST_CASE("Widen makes the sides less alike as it rises, and 0 changes nothing") {
	for (const bool saw : { true, false }) {
		double previous = 2.0;
		for (const float widen : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			const WidenRun r = renderCentred(widenSettings(widen), saw, 3.0, 1.0);
			const double c = correlation(r.l, r.r);
			CAPTURE(saw);
			CAPTURE(widen);
			CHECK(c < previous);
			previous = c;
			if (widen == 0.0f) {
				CHECK(c > 0.9999);
			}
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE GONIOMETER'S DATA AND THE CORRELATION                                                       //
//------------------------------------------------------------------------------------------------//

namespace {
/// THE PRINCIPAL AXIS OF A CLOUD OF (L, R) PAIRS, degrees from the L axis: 0 is all left, 45 is mono (the plot's vertical), 90 is all right.
struct Cloud {
	double ll = 0.0, rr = 0.0, lr = 0.0;
	size_t n = 0;
	void add(float l, float r) {
		ll += static_cast<double>(l) * static_cast<double>(l);
		rr += static_cast<double>(r) * static_cast<double>(r);
		lr += static_cast<double>(l) * static_cast<double>(r);
		++n;
	}
	[[nodiscard]] double angleDeg() const { return 0.5 * std::atan2(2.0 * lr, ll - rr) * 180.0 / 3.14159265358979323846; }
};
/// Renders `tones` for `settleSeconds`, then 100 blocks of 512 samples, gathering channel `channel`'s newest pairs from the readings.
[[nodiscard]] Cloud gather(const bal::Settings& s, const std::vector<Probe>& tones, size_t channel) {
	Instance inst;
	inst.set(s);
	Tones t;
	for (const Probe& p : tones) {
		t.tones.push_back({ p.hz, p.left, p.right });
	}
	(void)run(inst, t, 3.0, 0.0);
	Cloud c;
	const size_t blockFrames = 512;
	std::vector<float> l(blockFrames), r(blockFrames);
	for (int block = 0; block < 100; ++block) {
		t.fill(l, r);
		inst.process(l.data(), r.data(), static_cast<int>(blockFrames));
		for (size_t k = 0; k < bal::kRingPairs; ++k) {
			c.add(inst.point(channel, k, 0), inst.point(channel, k, 1));
		}
	}
	return c;
}
} // namespace

TEST_CASE("the goniometer's data: a band leaning left is a cloud leaning left, straight at Strength 100%, and the ghost is the raw input") {
	// 400 Hz on a 200 Hz comb is all in the on-teeth band (channel 1 of two); left is four times right: the axis is atan(1/4) = 14.04 degrees from L.
	const std::vector<Probe> tones { { 400.0, 1.0, 0.25 } };
	const double lean = std::atan(0.25) * 180.0 / 3.14159265358979323846;
	for (const float strength : { 0.0f, 1.0f }) {
		const Cloud band = gather(quick(2, strength), tones, 1);
		const Cloud ghost = gather(quick(2, strength), tones, bal::kInputChannel);
		CAPTURE(strength);
		REQUIRE(band.n > 1000u);
		CHECK(band.angleDeg() == doctest::Approx(strength == 0.0f ? lean : 45.0).epsilon(0.0).scale(1.0).epsilon(0.04));
		// THE GHOST IS THE INPUT, BEFORE ANY CORRECTION: it leans the same however much the band was straightened.
		CHECK(ghost.angleDeg() == doctest::Approx(lean).scale(1.0).epsilon(0.02));
	}
	// A BAND WITH NOTHING IN IT IS A POINT AT THE ORIGIN.
	const Cloud empty = gather(quick(3, 1.0f), tones, 0);
	CHECK(empty.ll + empty.rr < 1.0e-6);
}


TEST_CASE("the goniometer's points are consecutive samples: a stereo sine pair at a fixed phase offset lies on its ellipse") {
	// L = a sin(wt), R = b sin(wt + phi): every point satisfies (L/a)^2 + (R/b)^2 - 2 (L/a)(R/b) cos(phi) = sin^2(phi). 400 Hz is on a tooth of the 200 Hz comb,
	// so the band passes it whole (the comb's delay is two whole cycles) and the band's points, like the input's, are the ellipse.
	const double a = 0.8;
	const double b = 0.5;
	const double phi = 1.0;
	Instance inst;
	inst.set(quick(2, 0.0f));
	std::vector<float> l(512), r(512);
	double worst[2] = { 0.0, 0.0 };
	size_t count = 0;
	double t = 0.0;
	for (int block = 0; block < 200; ++block) {
		for (size_t i = 0; i < l.size(); ++i, t += 1.0) {
			l[i] = static_cast<float>(a * std::sin(kTwoPi * 400.0 * t / kRate));
			r[i] = static_cast<float>(b * std::sin(kTwoPi * 400.0 * t / kRate + phi));
		}
		inst.process(l.data(), r.data(), 512);
		if (block < 20) {
			continue;
		}
		// THE POINTS ARE CONSECUTIVE: neighbours advance the phase by exactly 2 pi 400 / 48000, so the input's cloud walks the ellipse in order.
		size_t channel = 0;
		for (const size_t ch : { bal::kInputChannel, static_cast<size_t>(1) }) {
			for (size_t k = 0; k < bal::kRingPairs; ++k) {
				if (k > 0) {
					const double dx = static_cast<double>(inst.point(ch, k, 0) - inst.point(ch, k - 1, 0));
					const double dy = static_cast<double>(inst.point(ch, k, 1) - inst.point(ch, k - 1, 1));
					// ONE SAMPLE'S STEP ALONG THE ELLIPSE: the phase moves 2 pi 400 / 48000 = 0.052 rad, at most 0.052 of the larger semi-axis.
					CHECK(std::hypot(dx, dy) < 0.06);
				}
				const double x = static_cast<double>(inst.point(ch, k, 0)) / a;
				const double y = static_cast<double>(inst.point(ch, k, 1)) / b;
				const double residual = x * x + y * y - 2.0 * x * y * std::cos(phi) - std::sin(phi) * std::sin(phi);
				worst[channel] = std::max(worst[channel], std::fabs(residual));
				++count;
			}
			++channel;
		}
		// THE INPUT'S NEWEST PAIR IS THE LAST SAMPLE OF THE BLOCK, to the bit.
		CHECK(inst.point(bal::kInputChannel, bal::kRingPairs - 1, 0) == l[511] * 0.0f + static_cast<float>(a * std::sin(kTwoPi * 400.0 * (t - 1.0) / kRate)));
	}
	REQUIRE(count > 1000u);
	CHECK(worst[0] < 2.0e-3);
	CHECK(worst[1] < 5.0e-3);
}
TEST_CASE("the correlation is 1 for mono, about 0 for decorrelated noise, and falls as Widen rises") {
	const auto corr = [](const bal::Settings& s, bool centred, bal::Reading which) {
		Instance inst;
		inst.set(s);
		uint32_t seed = 77u;
		double phase = 0.0;
		std::vector<float> l(512), r(512);
		for (int block = 0; block < 400; ++block) {
			if (centred) {
				::centred(l, r, false, 110.0, phase, seed);
			}
			else {
				noise(l, r, seed, 0.5f, 0.5f);
			}
			inst.process(l.data(), r.data(), 512);
		}
		return inst.reading(which);
	};
	bal::Settings s = quick(3, 0.0f);
	CHECK(corr(s, true, bal::Reading::CorrAll) > 0.999f);
	CHECK(corr(s, true, bal::Reading::Corr1) > 0.999f);
	CHECK(std::fabs(corr(s, false, bal::Reading::CorrAll)) < 0.15f);
	float previous = 2.0f;
	for (const float widen : { 0.0f, 0.5f, 1.0f }) {
		s.widen = widen;
		const float c = corr(s, true, bal::Reading::CorrAll);
		CAPTURE(widen);
		CHECK(c < previous);
		previous = c;
	}
}
TEST_CASE("the comb ratios helper names the two bands by the ratio, and OWLTT's names are the ones it always had") {
	CHECK(ow::combRatioText(0.5) == "x1/2");
	CHECK(ow::combSnapRatio(1.02) == 1.0);
	CHECK(ow::keyHzOf(69.0) == 440.0);
}

} // namespace
