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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "dsp/random_events.h"
#include "fx/noise/noise_dsp.h"
#include "fx/noise/noise_types.h"
#include "mod/rtrig/rtrig.h"
#include "persist_rig.h"
#include "rack/rack_state.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// RANDOM TRIGGER (backlog NA (a)) — by value: the event counts each distribution gives, the hits against a NOISE
// instance on the same settings and Seed, Size and Decay at known hits, the Trig pulse's width, each note's own stream
// under Poly, and the card's drawn distributions against the generator's own histogram.

using namespace feathers;
namespace rt = mod::rtrig;
namespace nz = fx::noise;
using P = rt::Param;

namespace {

constexpr double kRate = 48000.0;
constexpr double kTickSeconds = static_cast<double>(mod::kBlockSamples) / kRate;

/// ONE RANDOM TRIGGER, built the way a host builds a modulator, driven through `setFields` and a real row.
struct Trig {
	const device::ModOps& ops = *rt::kDescriptor.mod;
	const device::NoteOps& notes = *rt::kDescriptor.notes;
	void* mem = nullptr;
	rt::Row row = rt::rowOf(rt::Settings {});

	Trig() {
		mem = feathers::test::heapAlignedAlloc(64, ((ops.stateBytes + 63) / 64) * 64);
		ops.construct(mem);
		ops.prepare(mem, kRate);
		apply();
	}
	~Trig() {
		ops.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Trig(const Trig&) = delete;
	Trig& operator=(const Trig&) = delete;

	Trig& set(P p, float v) {
		row[static_cast<size_t>(p)] = v;
		return *this;
	}
	void apply() { ops.setFields(mem, row.data(), row.size()); }
	void tick(bool retrigger = false, bool gate = false) {
		device::ModTickContext ctx;
		ctx.gate = gate;
		ctx.retrigger = retrigger;
		ops.tick(mem, ctx);
	}
	void note(device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e;
		e.kind = kind;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
	}
	[[nodiscard]] float out(rt::Output o) const { return ops.outputAt(mem, static_cast<size_t>(o)); }
	[[nodiscard]] float outFor(rt::Output o, int16_t key) const {
		return ops.outputForNote(mem, static_cast<size_t>(o), -1, 0, key);
	}
	[[nodiscard]] const rt::Shape& shape() const { return rt::sharedShapeForTest(mem); }
};

/// ONE NOISE, on the bus, as a host builds it.
struct Noise {
	const device::AudioOps& ops = *nz::kDescriptor.audio;
	void* mem = nullptr;
	nz::Row row = nz::rowOf(nz::Settings {});

	Noise() {
		mem = feathers::test::heapAlignedAlloc(64, ((ops.stateBytes + 63) / 64) * 64);
		ops.construct(mem);
		ops.prepare(mem, kRate, 4096);
	}
	~Noise() {
		ops.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Noise(const Noise&) = delete;
	Noise& operator=(const Noise&) = delete;

	Noise& set(nz::Param p, float v) {
		row[static_cast<size_t>(p)] = v;
		return *this;
	}
	void apply() { ops.setFields(mem, row.data(), row.size()); }
};

/// THE HITS OF `seconds` OF A STREAM: (sample index, value).
struct Hit {
	size_t at;
	float value;
};
std::vector<Hit> hitsOf(Trig& t, double seconds) {
	std::vector<Hit> hits;
	const auto ticks = static_cast<size_t>(seconds / kTickSeconds);
	for (size_t k = 0; k < ticks; ++k) {
		t.tick();
		const rt::Shape& s = t.shape();
		REQUIRE(s.hitsLastTick() <= 1); // the seeds and rates below are chosen so; a second hit would be invisible here
		if (s.hitsLastTick() == 1) {
			hits.push_back({ k * mod::kBlockSamples + static_cast<size_t>(s.lastHitOffset()), s.size() });
		}
	}
	return hits;
}

/// COUNT EVERY HIT, however many land in one tick (the shape's own count).
uint32_t countOf(Trig& t, double seconds) {
	const auto ticks = static_cast<size_t>(seconds / kTickSeconds);
	for (size_t k = 0; k < ticks; ++k) {
		t.tick();
	}
	return t.shape().hits();
}

} // namespace

TEST_CASE("Random Trigger: each timing gives the rate asked for, and each size distribution its law, by count") {
	constexpr double kSeconds = 120.0;
	constexpr double kHz = 8.0;
	const double expected = kHz * kSeconds;
	struct Case {
		dsp::EventTiming timing;
		double tolerance; // a fraction of the expected count
	};
	// REGULAR at jitter 0.25 is a grid: within one hit. Poisson's sd is sqrt(960) = 31, 3.2%; Clustered's bursts widen
	// it, so its bound is looser.
	for (const Case c : { Case { dsp::EventTiming::Regular, 1.5 / expected }, Case { dsp::EventTiming::Poisson, 0.12 },
	                      Case { dsp::EventTiming::Clustered, 0.25 } }) {
		CAPTURE(static_cast<int>(c.timing));
		Trig t;
		t.set(P::Density, static_cast<float>(std::log2(kHz))).set(P::Timing, static_cast<float>(c.timing)).apply();
		const double n = countOf(t, kSeconds);
		CHECK(std::fabs(n - expected) <= c.tolerance * expected);
	}

	// THE SIZES, over 2000 hits each: Fixed all 1; Uniform mean 1/2; Exponential capped at 1, mean (1 - e^-3)/3 and
	// e^-3 of them at the cap. Random polarity sends half of them down.
	struct SizeCase {
		dsp::HitSize size;
		double mean;
		double atOne;
	};
	for (const SizeCase c : { SizeCase { dsp::HitSize::Fixed, 1.0, 1.0 }, SizeCase { dsp::HitSize::Uniform, 0.5, 0.0 },
	                          SizeCase { dsp::HitSize::Exponential, (1.0 - std::exp(-3.0)) / 3.0, std::exp(-3.0) } }) {
		CAPTURE(static_cast<int>(c.size));
		Trig t;
		t.set(P::Density, static_cast<float>(std::log2(20.0))).set(P::HitSize, static_cast<float>(c.size))
		    .set(P::Polarity, 1.0f).set(P::Timing, 0.0f).set(P::Jitter, 0.0f).apply();
		const std::vector<Hit> hits = hitsOf(t, 100.0);
		REQUIRE(hits.size() >= 1990);
		double sum = 0.0;
		size_t atOne = 0;
		size_t negative = 0;
		for (const Hit& h : hits) {
			sum += std::fabs(h.value);
			atOne += std::fabs(h.value) == 1.0f ? 1u : 0u;
			negative += h.value < 0.0f ? 1u : 0u;
		}
		const double n = static_cast<double>(hits.size());
		CHECK(std::fabs(sum / n - c.mean) < 0.02);
		CHECK(std::fabs(static_cast<double>(atOne) / n - c.atOne) < 0.015);
		CHECK(std::fabs(static_cast<double>(negative) / n - 0.5) < 0.04);
	}
}

TEST_CASE("Random Trigger: the same settings and Seed play NOISE's Impulses, hit for hit and sample for sample") {
	// THE SEED NOISE OPENS ITS LEFT EAR ON IS THE ONE THIS OPENS ON.
	for (const uint64_t seed : { 0ull, 7ull, 999ull }) {
		CHECK(rt::eventStreamSeed(seed) == nz::eventStreamSeed(seed));
	}
	// AND THE SAME CONTROLS MAKE THE SAME CORE SETTINGS, every timing, size, polarity and a synced rate.
	for (int timing = 0; timing < 3; ++timing) {
		for (int size = 0; size < 3; ++size) {
			for (const int sync : { 0, 5 }) {
				nz::Settings n;
				rt::Settings r;
				n.timing = r.timing = timing;
				n.hitSize = r.hitSize = size;
				n.sync = r.sync = sync;
				n.randomPolarity = r.randomPolarity = size == 1;
				n.density = r.density = 3.3f;
				n.jitter = r.jitter = 0.4f;
				n.cluster = r.cluster = 0.7f;
				const dsp::RandomEventSettings a = nz::eventSettingsOf(n, 0.4);
				const dsp::RandomEventSettings b = rt::eventSettingsOf(r, 0.4);
				CHECK(a.rateHz == b.rateHz);
				CHECK(a.timing == b.timing);
				CHECK(a.jitter == b.jitter);
				CHECK(a.cluster == b.cluster);
				CHECK(a.size == b.size);
				CHECK(a.randomPolarity == b.randomPolarity);
			}
		}
	}

	// **AND AGAINST A NOISE INSTANCE.** Its Impulses at unity (Level 0.5) through its filter wide open: the clicks are
	// rebuilt from this device's hits — the sample each landed on and its value — and run through the same filter,
	// designed the same way. Every output sample must be EQUAL, so a hit one sample off, or one size off, is red.
	struct Config {
		dsp::EventTiming timing;
		dsp::HitSize size;
		float jitter;
		float hz;
		int seed;
	};
	for (const Config c : { Config { dsp::EventTiming::Regular, dsp::HitSize::Uniform, 0.5f, 6.0f, 11 },
	                        Config { dsp::EventTiming::Poisson, dsp::HitSize::Exponential, 0.0f, 3.0f, 42 } }) {
		CAPTURE(c.seed);
		Trig t;
		t.set(P::Density, std::log2(c.hz)).set(P::Timing, static_cast<float>(c.timing))
		    .set(P::HitSize, static_cast<float>(c.size)).set(P::Jitter, c.jitter).set(P::Polarity, 1.0f)
		    .set(P::Seed, static_cast<float>(c.seed)).apply();
		Noise noise;
		noise.set(nz::Param::Source, 1.0f).set(nz::Param::Level, 0.5f).set(nz::Param::Density, std::log2(c.hz))
		    .set(nz::Param::Timing, static_cast<float>(c.timing)).set(nz::Param::HitSize, static_cast<float>(c.size))
		    .set(nz::Param::Jitter, c.jitter).set(nz::Param::Polarity, 1.0f).set(nz::Param::Seed, static_cast<float>(c.seed))
		    .apply();

		constexpr double kSeconds = 20.0;
		const std::vector<Hit> hits = hitsOf(t, kSeconds);
		REQUIRE(hits.size() > static_cast<size_t>(0.5 * c.hz * kSeconds));
		const size_t frames = static_cast<size_t>(kSeconds / kTickSeconds) * mod::kBlockSamples;
		std::vector<float> rendered(frames, 0.0f);
		for (size_t at = 0; at < frames; at += mod::kBlockSamples) {
			noise.ops.processMono(noise.mem, rendered.data() + at, mod::kBlockSamples);
		}
		std::vector<float> clicks(frames, 0.0f);
		for (const Hit& h : hits) {
			clicks[h.at] = h.value;
		}
		nz::Svf svf;
		// THE CORNER AS NOISE COMPUTES IT: from the control's float, through `exp2` in double.
		svf.design(static_cast<int>(nz::FilterType::Lowpass), std::exp2(static_cast<double>(static_cast<float>(nz::kCutoffMaxLog2))), 0.0f, kRate);
		nz::SvfState state;
		size_t mismatches = 0;
		size_t firstMismatch = frames;
		for (size_t i = 0; i < frames; ++i) {
			const float y = state.step(svf, clicks[i]);
			state.flushTiny();
			if (y != rendered[i]) {
				++mismatches;
				firstMismatch = std::min(firstMismatch, i);
			}
		}
		CAPTURE(firstMismatch);
		CHECK(mismatches == 0);
	}
}

TEST_CASE("Random Trigger: Trig is a one-tick pulse to 1 on exactly the ticks a hit lands in") {
	Trig t;
	t.set(P::Density, std::log2(30.0f)).apply();
	size_t pulses = 0;
	size_t ticksWithHits = 0;
	bool previous = false;
	size_t widePulses = 0;
	for (int k = 0; k < 40000; ++k) {
		t.tick();
		const float trig = t.out(rt::Output::Trig);
		const bool hit = t.shape().hitsLastTick() > 0;
		CHECK((trig == 1.0f || trig == 0.0f));
		CHECK((trig == 1.0f) == hit);
		ticksWithHits += hit ? 1u : 0u;
		pulses += (trig == 1.0f && !previous) ? 1u : 0u;
		// A PULSE THAT IS STILL HIGH THE TICK AFTER is two hits in neighbouring ticks, never one hit held.
		widePulses += (previous && trig == 1.0f && t.shape().hitsLastTick() == 0) ? 1u : 0u;
		previous = trig == 1.0f;
	}
	CHECK(ticksWithHits > 1000);
	CHECK(pulses > 0);
	CHECK(widePulses == 0);
}

TEST_CASE("Random Trigger: Size holds the newest hit and Decay falls from it 60 dB in Decay seconds") {
	constexpr float kDecaySeconds = 0.5f;
	Trig t;
	// A GRID AT 1 Hz, so the gap after each hit (750 ticks) outlasts the decay being measured (375).
	t.set(P::Density, 0.0f).set(P::Timing, 0.0f).set(P::Jitter, 0.0f).set(P::HitSize, 1.0f).set(P::Polarity, 1.0f)
	    .set(P::Decay, std::log2(kDecaySeconds))
	    .apply();
	// THE FIRST HIT.
	int guard = 0;
	do {
		t.tick();
	} while (t.shape().hitsLastTick() == 0 && ++guard < 100000);
	REQUIRE(t.shape().hitsLastTick() == 1);
	const float v = t.shape().size();
	REQUIRE(v != 0.0f);
	CHECK(t.out(rt::Output::Size) == v);
	CHECK(t.out(rt::Output::Decay) == v);
	// EVERY TICK UNTIL THE NEXT HIT: Size held, Decay = v x 10^(-3 n tick / decay) — restated, not read off the device.
	double expected = static_cast<double>(v);
	const double perTick = std::pow(10.0, -3.0 * kTickSeconds / static_cast<double>(kDecaySeconds));
	int n = 0;
	for (; n < 20000; ++n) {
		t.tick();
		if (t.shape().hitsLastTick() > 0) {
			break;
		}
		expected = static_cast<double>(static_cast<float>(expected * perTick));
		if (std::fabs(expected) < mod::kSmoothingRest) {
			expected = 0.0;
		}
		CHECK(t.out(rt::Output::Size) == v);
		CHECK(t.out(rt::Output::Decay) == doctest::Approx(expected).epsilon(1e-5));
		if (n == static_cast<int>(std::lround(kDecaySeconds / kTickSeconds)) - 1) {
			// AT DECAY SECONDS, 1/1000 OF THE HIT.
			CHECK(std::fabs(std::fabs(t.out(rt::Output::Decay)) / (std::fabs(v) * 1.0e-3) - 1.0) < 0.01);
		}
	}
	REQUIRE(n > static_cast<int>(kDecaySeconds / kTickSeconds) + 10);
	// AT THE BOTTOM OF ITS TRAVEL THE DECAY IS ONE TICK LONG.
	t.set(P::Decay, mod::kTimeLog2SecondsMin).apply();
	do {
		t.tick();
	} while (t.shape().hitsLastTick() == 0);
	CHECK(t.out(rt::Output::Decay) == t.shape().size());
	t.tick();
	if (t.shape().hitsLastTick() == 0) {
		CHECK(t.out(rt::Output::Decay) == 0.0f);
	}
}

namespace {

/// ONE NOISE'S MOD ROAD (backlog NA (b)): the instance a host builds, ticked as the rack ticks it.
struct NoiseRoad : Noise {
	const device::ModOps& mod = *nz::kDescriptor.mod;
	void tick() {
		device::ModTickContext ctx;
		mod.tick(mem, ctx);
	}
	[[nodiscard]] float out(nz::Out o) const { return mod.outputAt(mem, static_cast<size_t>(o)); }
};

} // namespace

TEST_CASE("NOISE: Impulse Decay is Random Trigger's Decay, tick for tick, on the same settings and Seed") {
	// THE SAME `HitDecay` AT THE SAME FACTOR, so on one Seed the two devices publish one envelope: every tick's Trig,
	// Size and Decay EQUAL, under a signed, uneven size law and a Decay off its default.
	const float decay = std::log2(0.3f);
	Trig t;
	t.set(P::Density, std::log2(9.0f)).set(P::HitSize, 1.0f).set(P::Polarity, 1.0f).set(P::Seed, 23.0f)
	    .set(P::Decay, decay).apply();
	NoiseRoad noise;
	noise.set(nz::Param::Source, 1.0f).set(nz::Param::Density, std::log2(9.0f)).set(nz::Param::HitSize, 1.0f)
	    .set(nz::Param::Polarity, 1.0f).set(nz::Param::Seed, 23.0f).set(nz::Param::Decay, decay).apply();
	size_t hits = 0;
	size_t decaying = 0;
	size_t mismatches = 0;
	for (int k = 0; k < 15000; ++k) {
		t.tick();
		noise.tick();
		const float d = noise.out(nz::Out::Decay);
		mismatches += noise.out(nz::Out::Trig) != t.out(rt::Output::Trig) ? 1u : 0u;
		mismatches += noise.out(nz::Out::Size) != t.out(rt::Output::Size) ? 1u : 0u;
		mismatches += d != t.out(rt::Output::Decay) ? 1u : 0u;
		hits += noise.out(nz::Out::Trig) == 1.0f ? 1u : 0u;
		decaying += d != 0.0f && d != noise.out(nz::Out::Size) ? 1u : 0u;
	}
	CHECK(mismatches == 0);
	CHECK(hits > 100);
	CHECK(decaying > 1000); // not vacuous: it was falling between hits, not standing at the size
}

TEST_CASE("NOISE: Impulse Decay stands at each hit's value and falls 60 dB in Decay seconds") {
	constexpr float kDecaySeconds = 0.4f;
	NoiseRoad noise;
	// A GRID AT 1 Hz, so the gap after each hit (750 ticks) outlasts the decay being measured (300).
	noise.set(nz::Param::Source, 1.0f).set(nz::Param::Density, 0.0f).set(nz::Param::Timing, 0.0f)
	    .set(nz::Param::Jitter, 0.0f).set(nz::Param::HitSize, 1.0f).set(nz::Param::Polarity, 1.0f)
	    .set(nz::Param::Decay, std::log2(kDecaySeconds)).apply();
	int guard = 0;
	do {
		noise.tick();
	} while (noise.out(nz::Out::Trig) == 0.0f && ++guard < 100000);
	REQUIRE(noise.out(nz::Out::Trig) == 1.0f);
	const float v = noise.out(nz::Out::Size);
	REQUIRE(v != 0.0f);
	CHECK(noise.out(nz::Out::Decay) == v); // the hit's own tick: its full value
	// EVERY TICK UNTIL THE NEXT HIT: v x 10^(-3 n tick / decay) — restated, not read off either device.
	double expected = static_cast<double>(v);
	const double perTick = std::pow(10.0, -3.0 * kTickSeconds / static_cast<double>(kDecaySeconds));
	int n = 0;
	for (; n < 20000; ++n) {
		noise.tick();
		if (noise.out(nz::Out::Trig) != 0.0f) {
			break;
		}
		expected = static_cast<double>(static_cast<float>(expected * perTick));
		if (std::fabs(expected) < mod::kSmoothingRest) {
			expected = 0.0;
		}
		CHECK(noise.out(nz::Out::Size) == v);
		CHECK(noise.out(nz::Out::Decay) == doctest::Approx(expected).epsilon(1e-5));
		if (n == static_cast<int>(std::lround(kDecaySeconds / kTickSeconds)) - 1) {
			// AT DECAY SECONDS, 1/1000 OF THE HIT.
			CHECK(std::fabs(std::fabs(noise.out(nz::Out::Decay)) / (std::fabs(v) * 1.0e-3) - 1.0) < 0.01);
		}
	}
	REQUIRE(n > static_cast<int>(kDecaySeconds / kTickSeconds) + 10);
	// THE NEXT HIT STARTS IT AGAIN FROM ITS OWN VALUE.
	CHECK(noise.out(nz::Out::Decay) == noise.out(nz::Out::Size));
	// AND UNDER NOISE (no impulses) IT RESTS.
	noise.set(nz::Param::Source, 0.0f).apply();
	noise.tick();
	CHECK(noise.out(nz::Out::Decay) == 0.0f);
}

TEST_CASE("Random Trigger: under Poly each note runs its own stream from its On; under global Stable restarts at a note") {
	constexpr int16_t kA = 60;
	constexpr int16_t kB = 64;
	Trig t;
	t.set(P::Density, std::log2(40.0f)).set(P::HitSize, 1.0f).set(P::Poly, 1.0f).apply();
	// NOTE A AT TICK 0, NOTE B 37 TICKS LATER: under Stable B's pattern is A's, 37 ticks behind.
	constexpr int kLag = 37;
	constexpr int kTicks = 3000;
	std::vector<float> a(kTicks), b(kTicks);
	// A HAS PLAYED BEFORE, so its stream is 300 ticks in: its next On must start it from the top, not carry on.
	t.note(device::NoteEvent::Kind::On, kA);
	for (int k = 0; k < 300; ++k) {
		t.tick(false, true);
	}
	t.note(device::NoteEvent::Kind::Off, kA);
	t.tick(false, false);
	t.note(device::NoteEvent::Kind::On, kA);
	for (int k = 0; k < kTicks + kLag; ++k) {
		if (k == kLag) {
			t.note(device::NoteEvent::Kind::On, kB);
		}
		t.tick(false, true);
		if (k < kTicks) {
			a[static_cast<size_t>(k)] = t.outFor(rt::Output::Size, kA);
		}
		if (k >= kLag) {
			b[static_cast<size_t>(k - kLag)] = t.outFor(rt::Output::Size, kB);
		}
	}
	size_t differ = 0;
	size_t nonzero = 0;
	for (int k = 0; k < kTicks; ++k) {
		differ += a[static_cast<size_t>(k)] != b[static_cast<size_t>(k)] ? 1u : 0u;
		nonzero += a[static_cast<size_t>(k)] != 0.0f ? 1u : 0u;
	}
	CHECK(nonzero > kTicks / 2);
	CHECK(differ == 0);
	// AND AT ONE INSTANT THE TWO NOTES ARE IN DIFFERENT PLACES, so the voices are independent.
	{
		size_t sameNow = 0;
		for (int k = 0; k < 400; ++k) {
			t.tick(false, true);
			sameNow += t.outFor(rt::Output::Trig, kA) == t.outFor(rt::Output::Trig, kB)
			                   && t.outFor(rt::Output::Size, kA) == t.outFor(rt::Output::Size, kB)
			               ? 1u
			               : 0u;
		}
		CHECK(sameNow < 400);
	}

	// FREE: two notes struck together still differ.
	{
		Trig f;
		f.set(P::Density, std::log2(40.0f)).set(P::HitSize, 1.0f).set(P::Poly, 1.0f).set(P::SeedMode, 1.0f).apply();
		f.note(device::NoteEvent::Kind::On, kA);
		f.note(device::NoteEvent::Kind::On, kB);
		size_t same = 0;
		for (int k = 0; k < 2000; ++k) {
			f.tick(false, true);
			same += f.outFor(rt::Output::Size, kA) == f.outFor(rt::Output::Size, kB) ? 1u : 0u;
		}
		CHECK(same < 1000);
	}

	// GLOBAL, STABLE: a note restarts the one stream, so what follows a note is the pattern from its top.
	{
		Trig g;
		g.set(P::Density, std::log2(40.0f)).set(P::HitSize, 1.0f).apply();
		std::vector<float> first(500), second(500);
		g.tick(true, true);
		first[0] = g.out(rt::Output::Size);
		for (size_t k = 1; k < first.size(); ++k) {
			g.tick(false, true);
			first[k] = g.out(rt::Output::Size);
		}
		g.tick(true, true);
		second[0] = g.out(rt::Output::Size);
		for (size_t k = 1; k < second.size(); ++k) {
			g.tick(false, true);
			second[k] = g.out(rt::Output::Size);
		}
		// THE SIZE HELD FROM BEFORE THE RESTART stands until the first hit after it, so compare from that hit on.
		size_t start = 0;
		while (start < first.size() && first[start] == 0.0f) {
			++start;
		}
		REQUIRE(start < 100);
		size_t mismatch = 0;
		for (size_t k = start; k < first.size(); ++k) {
			mismatch += first[k] != second[k] ? 1u : 0u;
		}
		CHECK(mismatch == 0);
	}
}

TEST_CASE("Random Trigger: the gap distribution the card draws is the generator's own, by histogram") {
	struct Case {
		dsp::EventTiming timing;
		float jitter;
		float cluster;
	};
	for (const Case c : { Case { dsp::EventTiming::Poisson, 0.0f, 0.0f }, Case { dsp::EventTiming::Regular, 0.6f, 0.0f },
	                      Case { dsp::EventTiming::Clustered, 0.0f, 0.5f } }) {
		CAPTURE(static_cast<int>(c.timing));
		// THE GENERATOR ALONE, one sample at a time, 40,000 gaps at a mean of 480 samples.
		dsp::RandomEvents ev;
		ev.setSampleRate(kRate);
		ev.seed(5);
		dsp::RandomEventSettings s;
		s.rateHz = 100.0;
		s.timing = c.timing;
		s.jitter = c.jitter;
		s.cluster = c.cluster;
		constexpr int kBins = 12;
		constexpr double kSpan = 3.0;
		std::vector<double> histogram(kBins, 0.0);
		size_t gaps = 0;
		long last = -1;
		for (long i = 0; gaps < 40000; ++i) {
			if (ev.next(s) != 0.0f) {
				if (last >= 0) {
					const double x = static_cast<double>(i - last) / 480.0;
					if (x < kSpan) {
						histogram[static_cast<size_t>(x / kSpan * kBins)] += 1.0;
					}
					++gaps;
				}
				last = i;
			}
		}
		// EACH BIN'S SHARE AGAINST THE DRAWN DENSITY INTEGRATED OVER IT.
		for (int b = 0; b < kBins; ++b) {
			const double lo = kSpan * b / kBins;
			const double hi = kSpan * (b + 1) / kBins;
			double mass = 0.0;
			constexpr int kSteps = 400;
			for (int k = 0; k < kSteps; ++k) {
				mass += rt::gapDensity(c.timing, c.jitter, c.cluster, lo + (hi - lo) * (k + 0.5) / kSteps) * (hi - lo) / kSteps;
			}
			CAPTURE(b);
			CHECK(std::fabs(histogram[static_cast<size_t>(b)] / static_cast<double>(gaps) - mass) < 0.012);
		}
	}
	// AND THE SIZES' MASS: Exponential's cap holds e^-3 of the hits; its density integrates with it to 1.
	double total = rt::sizeMassAtOne(dsp::HitSize::Exponential);
	for (int k = 0; k < 10000; ++k) {
		total += rt::sizeDensity(dsp::HitSize::Exponential, (k + 0.5) / 10000.0) / 10000.0;
	}
	CHECK(total == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("Random Trigger: a Trig fires one hit on its rising edge, sized by the law, and moves none of the stream's own") {
	Trig fired;
	Trig twin;
	for (Trig* t : { &fired, &twin }) {
		t->set(P::Density, std::log2(3.0f)).set(P::HitSize, 1.0f).set(P::Polarity, 1.0f).apply();
	}
	const auto fires = [](const Trig& t) { return t.ops.trigFires(t.mem); };
	// 300 TICKS IN, the Trig rises: one hit lands in that tick, and the count of fires is 1.
	for (int k = 0; k < 300; ++k) {
		fired.tick();
		twin.tick();
	}
	const uint32_t before = fired.shape().hits();
	REQUIRE(before == twin.shape().hits());
	fired.set(P::Trig, 1.0f).apply();
	fired.tick();
	twin.tick();
	CHECK(fires(fired) == 1u);
	CHECK(fired.out(rt::Output::Trig) == 1.0f);
	CHECK(fired.shape().hits() == twin.shape().hits() + 1u);
	const float v = fired.shape().size();
	if (twin.shape().hitsLastTick() == 0) {
		// UNIFORM ON (0, 1], EITHER SIGN — and the Decay output starts from it.
		CHECK(std::fabs(v) > 0.0f);
		CHECK(std::fabs(v) <= 1.0f);
		CHECK(fired.out(rt::Output::Decay) == v);
	}
	// HELD HIGH IT DOES NOT FIRE AGAIN; DOWN AND UP AGAIN IT DOES.
	for (int k = 0; k < 20; ++k) {
		fired.tick();
		twin.tick();
	}
	CHECK(fires(fired) == 1u);
	fired.set(P::Trig, 0.0f).apply();
	fired.tick();
	twin.tick();
	fired.set(P::Trig, 1.0f).apply();
	fired.tick();
	twin.tick();
	CHECK(fires(fired) == 2u);
	// AND THE STREAM'S OWN HITS ARE WHERE THEY WERE: after 2000 more ticks the two differ by the two fires exactly.
	for (int k = 0; k < 2000; ++k) {
		fired.tick();
		twin.tick();
	}
	CHECK(fired.shape().hits() == twin.shape().hits() + 2u);
	CHECK(fired.out(rt::Output::Size) == twin.out(rt::Output::Size));
}

TEST_CASE("Random Trigger: its newest hits, newest first, are every hit's count and size, however many a tick holds") {
	Trig t;
	// 150 HITS A SECOND, five ticks to a hit on average: several frames' worth, and now and then two in one tick.
	t.set(P::Density, std::log2(150.0f)).set(P::HitSize, 1.0f).set(P::Polarity, 1.0f).apply();
	// EVERY HIT BY ITS COUNT, read where a tick holds exactly one (its size is then the tick's Size).
	std::vector<std::pair<uint32_t, float>> single;
	for (int k = 0; k < 400; ++k) {
		t.tick();
		if (t.shape().hitsLastTick() == 1) {
			single.emplace_back(t.shape().hits(), t.shape().size());
		}
	}
	const uint32_t hits = t.shape().hits();
	REQUIRE(hits > device::kRecentEvents);
	std::array<device::RecentEvent, device::kRecentEvents> recent {};
	REQUIRE(t.ops.recentEvents != nullptr);
	const size_t n = t.ops.recentEvents(t.mem, recent.data(), recent.size());
	REQUIRE(n == device::kRecentEvents);
	size_t matched = 0;
	for (size_t i = 0; i < n; ++i) {
		CAPTURE(i);
		CHECK(recent[i].ordinal == hits - static_cast<uint32_t>(i)); // newest first, none skipped
		for (const auto& [ordinal, size] : single) {
			if (ordinal == recent[i].ordinal) {
				CHECK(recent[i].value == size);
				++matched;
			}
		}
	}
	CHECK(matched >= 8u); // not vacuous: most of the sixteen were checked against their tick's own Size
	CHECK(recent[0].value == t.shape().size());
}

TEST_CASE("NOISE: a Trig fires one click on its rising edge — Random Trigger's fired size on the same Seed, in both ears "
          "alike — and moves none of the generator's own clicks") {
	const device::ModOps& mod = *nz::kDescriptor.mod;
	Noise fired;
	Noise twin;
	Trig rtrig;
	for (Noise* n : { &fired, &twin }) {
		n->set(nz::Param::Source, 1.0f).set(nz::Param::Density, std::log2(3.0f)).set(nz::Param::HitSize, 1.0f);
		n->set(nz::Param::Polarity, 1.0f).set(nz::Param::Seed, 7.0f).apply();
	}
	rtrig.set(P::Density, std::log2(3.0f)).set(P::HitSize, 1.0f).set(P::Polarity, 1.0f).set(P::Seed, 7.0f).apply();
	std::array<float, mod::kBlockSamples> fl {}, fr {}, tl {}, tr {};
	const auto window = [&](bool fire) {
		fired.set(nz::Param::Trig, fire ? 1.0f : 0.0f).apply();
		rtrig.set(P::Trig, fire ? 1.0f : 0.0f).apply();
		device::ModTickContext ctx;
		mod.tick(fired.mem, ctx);
		mod.tick(twin.mem, ctx);
		rtrig.tick();
		fl.fill(0.0f);
		fr.fill(0.0f);
		tl.fill(0.0f);
		tr.fill(0.0f);
		fired.ops.processStereo(fired.mem, fl.data(), fr.data(), mod::kBlockSamples);
		twin.ops.processStereo(twin.mem, tl.data(), tr.data(), mod::kBlockSamples);
	};
	for (int k = 0; k < 300; ++k) {
		window(false);
		CHECK(fl == tl); // nothing fired, nothing differs
	}
	REQUIRE(mod.trigFires(fired.mem) == 0u);
	// THE TRIG RISES: one fire, published on this tick as Random Trigger publishes its own fired hit.
	window(true);
	CHECK(mod.trigFires(fired.mem) == 1u);
	CHECK(mod.outputAt(fired.mem, static_cast<size_t>(nz::Out::Trig)) == 1.0f);
	const float v = mod.outputAt(fired.mem, static_cast<size_t>(nz::Out::Size));
	REQUIRE(mod.outputAt(twin.mem, static_cast<size_t>(nz::Out::Trig)) == 0.0f); // no click of its own in this window
	{
		CHECK(v == rtrig.out(rt::Output::Size)); // the same stream for fires, from the same Seed
		CHECK(std::fabs(v) > 0.0f);
		CHECK(std::fabs(v) <= 1.0f);
	}
	// THE CLICK SOUNDS FROM THE WINDOW'S FIRST SAMPLE, IN BOTH EARS ALIKE (the filter is linear, so the difference from the
	// twin is the fire's own response in each ear).
	CHECK(fl[0] != tl[0]);
	for (size_t i = 0; i < fl.size(); ++i) {
		CAPTURE(i);
		CHECK(std::fabs((fl[i] - tl[i]) - (fr[i] - tr[i])) <= 1e-6f);
	}
	// HELD HIGH IT DOES NOT FIRE AGAIN.
	for (int k = 0; k < 10; ++k) {
		window(true);
	}
	CHECK(mod.trigFires(fired.mem) == 1u);
	// AND THE GENERATOR'S OWN CLICKS ARE WHERE THEY WERE: the published Impulse Trig agrees with the twin's, tick by tick.
	int ownHits = 0;
	for (int k = 0; k < 2000; ++k) {
		window(false);
		const float a = mod.outputAt(fired.mem, static_cast<size_t>(nz::Out::Trig));
		CHECK(a == mod.outputAt(twin.mem, static_cast<size_t>(nz::Out::Trig)));
		ownHits += a == 1.0f ? 1 : 0;
	}
	CHECK(ownHits >= 2);
}

TEST_CASE("NOISE: the Trig is read only under Impulses — under Noise a rising Trig fires nothing") {
	const device::ModOps& mod = *nz::kDescriptor.mod;
	Noise n;
	n.set(nz::Param::Source, 0.0f).apply();
	n.set(nz::Param::Trig, 1.0f).apply();
	device::ModTickContext ctx;
	mod.tick(n.mem, ctx);
	CHECK(mod.trigFires(n.mem) == 0u);
	CHECK(mod.outputAt(n.mem, static_cast<size_t>(nz::Out::Trig)) == 0.0f);
	// AND THE CONTROL SAYS SO: gated on Source, live only under Impulses.
	const device::ParamDesc& d = nz::kParamDescs[static_cast<size_t>(nz::Param::Trig)];
	CHECK(std::string(d.key) == "noise_trig_in");
	CHECK(d.gateControl == nz::kSourceControl);
	CHECK_FALSE(device::relevantFor(d, 0.0));
	CHECK(device::relevantFor(d, 1.0));
}

//------------------------------------------------------------------------------------------------//
// THE HELD HIT PERSISTS WITH THE PATCH (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS")//
//------------------------------------------------------------------------------------------------//

namespace {

/// THE PATCH WITH ONE BLOCK TAKEN OUT — the control: what a load holds without it.
std::string without(const std::string& saved, const char* key) {
	rack::RackPatch patch;
	REQUIRE(rack::parseRackText(saved, &patch));
	REQUIRE(patch.device.size() == 1u);
	const size_t before = patch.device[0].extra.size();
	std::erase_if(patch.device[0].extra, [key](const rack::RackPatch::Value& v) { return v.key == key; });
	REQUIRE(patch.device[0].extra.size() == before - 1);
	std::string text;
	REQUIRE(rack::rackToText(patch, &text));
	return text;
}

/// A BLOCK AS THE RUNNING INSTANCE LAST PUBLISHED IT, to the bit.
std::vector<uint32_t> blockBits(const device::Descriptor& d, const void* storage) {
	std::vector<float> floats(d.state->blocks[0].floats);
	REQUIRE(d.state->read(storage, 0, floats.data(), floats.size()) != 0);
	std::vector<uint32_t> out;
	for (const float f : floats) {
		out.push_back(std::bit_cast<uint32_t>(f));
	}
	return out;
}

} // namespace

TEST_CASE("Random Trigger persistence: the held Size and Decay and the stream's place survive a save and a load into a "
          "fresh rack, and the fresh one hits where the saved one would have") {
	rt::Settings s {};
	s.seedMode = static_cast<int>(rt::SeedMode::Free);
	s.hitSize = static_cast<int>(dsp::HitSize::Uniform);
	s.randomPolarity = true;
	s.decay = 1.0f; // two seconds, so the decay is mid-fall at the save
	const rt::Row row = rt::rowOf(s);
	persist::Rig run;
	run.seat(rt::kTraits.tag, row);
	run.block(3000); // four seconds: about sixteen hits
	const std::string saved = run.save();
	const rt::Shape& was = rt::sharedShapeForTest(run.storage());
	const float size = was.size();
	const float decay = was.decayValue();
	const uint32_t hits = was.hits();
	REQUIRE(hits > 5u);
	REQUIRE(size != 0.0f);

	persist::Rig back;
	back.load(saved);
	// THE VALUES THEMSELVES, ON THE FRESH INSTANCE BEFORE IT TICKS.
	const rt::Shape& loaded = rt::sharedShapeForTest(back.storage());
	CHECK(std::bit_cast<uint32_t>(loaded.size()) == std::bit_cast<uint32_t>(size));
	CHECK(std::bit_cast<uint32_t>(loaded.decayValue()) == std::bit_cast<uint32_t>(decay));
	CHECK(loaded.hits() == hits);
	// AND FROM HERE ONE STREAM: every block's Size and Decay, and the hit count, to the bit.
	for (int b = 0; b < 3000; ++b) {
		CAPTURE(b);
		run.block();
		back.block();
		const rt::Shape& a = rt::sharedShapeForTest(run.storage());
		const rt::Shape& c = rt::sharedShapeForTest(back.storage());
		REQUIRE(c.hits() == a.hits());
		REQUIRE(std::bit_cast<uint32_t>(c.size()) == std::bit_cast<uint32_t>(a.size()));
		REQUIRE(std::bit_cast<uint32_t>(c.decayValue()) == std::bit_cast<uint32_t>(a.decayValue()));
	}
	CHECK(rt::sharedShapeForTest(run.storage()).hits() > hits + 5u); // NOT VACUOUS: it went on hitting

	// THE CONTROL: without the block, a load holds nothing until its first hit.
	persist::Rig bare;
	bare.load(without(saved, "rtrig_stream"));
	CHECK(rt::sharedShapeForTest(bare.storage()).size() == 0.0f);
	CHECK(rt::sharedShapeForTest(bare.storage()).hits() == 0u);
}

TEST_CASE("NOISE persistence: the held Impulse Size and Decay survive a save and a load into a fresh rack, value by value") {
	nz::Settings s {};
	s.source = static_cast<int>(nz::Source::Impulses);
	s.hitSize = static_cast<int>(dsp::HitSize::Uniform);
	s.randomPolarity = true;
	s.decay = 1.0f;
	s.poly = false;
	const nz::Row row = nz::rowOf(s);
	persist::Rig run;
	run.seat(nz::kTraits.tag, row);
	run.block(3000);
	const std::string saved = run.save();
	const std::vector<uint32_t> atSave = blockBits(nz::kDescriptor, run.storage());
	const size_t lane = run.engine.slotOf(run.id);
	const float size = run.engine.publishedOutputAt(lane, static_cast<size_t>(nz::Out::Size));
	REQUIRE(size != 0.0f);

	persist::Rig back;
	back.load(saved);
	// THE BLOCK THE FRESH INSTANCE HOLDS BEFORE IT TICKS IS THE ONE SAVED — Size and Decay to the bit.
	CHECK(blockBits(nz::kDescriptor, back.storage()) == atSave);
	std::array<float, nz::kHeldWords> words {};
	REQUIRE(nz::kStateOps.read(back.storage(), 0, words.data(), words.size()) != 0);
	device::BlockReader held(words.data(), words.size());
	CHECK(std::bit_cast<uint32_t>(held.f32()) == std::bit_cast<uint32_t>(size));

	// THE CONTROL: without the block, Impulse Size opens at 0.
	persist::Rig bare;
	bare.load(without(saved, "noise_held"));
	std::array<float, nz::kHeldWords> none {};
	CHECK(nz::kStateOps.read(bare.storage(), 0, none.data(), none.size()) == 0); // nothing published yet: never ticked
	bare.block();
	REQUIRE(nz::kStateOps.read(bare.storage(), 0, none.data(), none.size()) != 0);
	device::BlockReader fresh(none.data(), none.size());
	CHECK(std::bit_cast<uint32_t>(fresh.f32()) != std::bit_cast<uint32_t>(size));
}
