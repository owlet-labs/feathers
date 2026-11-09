/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// SAMPLE & HOLD (backlog MW). Every case asserts a VALUE — what is held, on which tick, how many were taken — read
// through the device's own ops tables, as the rack drives it, or through the rack itself where Poly is the claim.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "device/format_value.h"
#include "device/registry.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/samplehold/samplehold.h"
#include "mod/samplehold/samplehold_types.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack_endpoints.h"
#include "persist_rig.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <string>
#include <cmath>
#include <cstdlib>
#include <set>
#include <vector>

using namespace feathers;
namespace sh = feathers::mod::samplehold;

namespace {

constexpr double kRate = 48000.0;
constexpr int kTicksPerSecond = static_cast<int>(kRate) / mod::kBlockSamples; // 750
constexpr double kTick = static_cast<double>(mod::kBlockSamples) / kRate;

/// ONE DEVICE, DRIVEN THROUGH ITS OPS TABLES as the rack drives it (test_mod_trig's rig).
struct Rig {
	const device::ModOps& mod = sh::kModOps;
	const device::NoteOps& notes = sh::kNoteOps;
	void* mem = nullptr;
	device::ModTickContext ctx {};

	explicit Rig(const sh::Settings& s = sh::defaults()) {
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, kRate);
		apply(s);
	}
	~Rig() {
		mod.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Rig(const Rig&) = delete;
	Rig& operator=(const Rig&) = delete;

	void apply(const sh::Settings& s) {
		const sh::Row row = sh::rowOf(s);
		mod.setFields(mem, row.data(), row.size());
	}
	void tick(int times = 1) {
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	}
	float held() const { return mod.outputAt(mem, static_cast<size_t>(sh::Output::Held)); }
	float pulse() const { return mod.outputAt(mem, static_cast<size_t>(sh::Output::TrigOut)); }
	const sh::Device& device() const { return *static_cast<const sh::Device*>(mem); }
	uint32_t taken() const { return device().mono.taken(); }
	uint32_t clocks() const { return device().mono.clocks(); }
};

/// A CLOCK OF `hz` UNDER Time Base Hz.
sh::Settings atHz(double hz) {
	sh::Settings s = sh::defaults();
	s.timeScale = static_cast<float>(std::log2(hz));
	return s;
}

/// THE HELD VALUES OF `ticks` TICKS, one a tick.
std::vector<float> run(Rig& rig, int ticks) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(ticks));
	for (int i = 0; i < ticks; ++i) {
		rig.tick();
		out.push_back(rig.held());
	}
	return out;
}

} // namespace

TEST_CASE("S&H holds exactly between clocks, and a clock lands every period of Rate") {
	// 7.5 Hz at 48 kHz is a clock every 100 ticks. Between two pulses the output is the SAME float, bit for bit; every
	// pulse is a new value (white noise), and pulses come 100 ticks apart (99..101 for the phase's rounding).
	Rig rig(atHz(7.5));
	std::vector<int> pulses;
	float was = 0.0f;
	for (int i = 0; i < 2000; ++i) {
		rig.tick();
		const float now = rig.held();
		if (rig.pulse() == 1.0f) {
			pulses.push_back(i);
		}
		else if (i > 0) {
			REQUIRE(now == was); // HOLDS: nothing moves the output between clocks
		}
		was = now;
	}
	REQUIRE(pulses.size() >= 20);
	CHECK(pulses.front() == 0); // the first tick takes a first sample: a hold begins holding something
	for (size_t i = 2; i < pulses.size(); ++i) {
		const int gap = pulses[i] - pulses[i - 1];
		CAPTURE(i);
		CHECK(gap >= 99);
		CHECK(gap <= 101);
	}
	// AND THE CLOCKS ADD UP: 2000 ticks at 100 a clock is 19 or 20 clocks after the first sample.
	CHECK(rig.clocks() >= 19);
	CHECK(rig.clocks() <= 20);
}

TEST_CASE("S&H Probability: 0 never resamples, 1 always does, and between them the taken fraction is the probability") {
	SUBCASE("0") {
		sh::Settings s = atHz(10.0);
		s.probability = 0.0f;
		Rig rig(s);
		rig.tick();
		const float first = rig.held();
		REQUIRE(rig.taken() == 1u); // the first sample, which a hold has whatever Probability says
		for (int i = 0; i < 10 * kTicksPerSecond; ++i) {
			rig.tick();
			REQUIRE(rig.held() == first);
			REQUIRE(rig.pulse() == 0.0f);
		}
		CHECK(rig.clocks() >= 99u);
		CHECK(rig.taken() == 1u);
	}
	SUBCASE("1") {
		Rig rig(atHz(10.0));
		rig.tick(10 * kTicksPerSecond);
		REQUIRE(rig.clocks() >= 99u);
		CHECK(rig.taken() == rig.clocks() + 1u);
	}
	SUBCASE("0.3, by count") {
		// 4000 clocks at p = 0.3: the binomial's sd is 29 samples (0.0072 of the count); the bound is four of them.
		sh::Settings s = atHz(10.0);
		s.probability = 0.3f;
		Rig rig(s);
		rig.tick(400 * kTicksPerSecond);
		REQUIRE(rig.clocks() >= 3990u);
		const double fraction = static_cast<double>(rig.taken() - 1u) / static_cast<double>(rig.clocks());
		CAPTURE(fraction);
		CHECK(std::abs(fraction - 0.3) < 0.03);
	}
}

TEST_CASE("S&H Quantize rounds each sample to steps of 1/N of full scale; off leaves it continuous") {
	sh::Settings s = sh::defaults();
	s.source = sh::Source::Input;
	s.clock = sh::Clock::Trig;
	s.input = 0.37f;
	s.quantize = 4;
	{
		Rig rig(s);
		rig.tick();
		CHECK(rig.held() == 0.25f); // round(1.48) / 4
	}
	s.input = -0.62f;
	{
		Rig rig(s);
		rig.tick();
		CHECK(rig.held() == -0.5f); // round(-2.48) / 4
	}
	s.quantize = 0;
	{
		Rig rig(s);
		rig.tick();
		CHECK(rig.held() == -0.62f);
	}
	// AND THE NOISE: every held value of a quantized white staircase is a whole number of twelfths.
	sh::Settings noise = atHz(50.0);
	noise.quantize = 12;
	Rig rig(noise);
	std::set<long> levels;
	for (const float v : run(rig, 20 * kTicksPerSecond)) {
		const double steps = static_cast<double>(v) * 12.0;
		REQUIRE(std::abs(steps - std::round(steps)) < 1e-5);
		levels.insert(std::lround(steps));
	}
	CHECK(levels.size() >= 20u); // of the 25 levels from -12 to 12 — the rounding does not collapse the range
}

TEST_CASE("S&H Slew glides to the new value as a one-pole of exactly its time constant") {
	// Input 1, unipolar, a Trig clock, Slew 100 ms: after k ticks from the take the output is 1 - exp(-k dt / tau).
	sh::Settings s = sh::defaults();
	s.source = sh::Source::Input;
	s.clock = sh::Clock::Trig;
	s.bipolar = false;
	s.input = 0.0f;
	s.slew = static_cast<float>(std::log2(0.1));
	Rig rig(s);
	rig.tick(10);
	REQUIRE(rig.held() == 0.0f);
	s.input = 1.0f;
	s.trig = 1.0f; // fire
	rig.apply(s);
	const double tau = std::exp2(static_cast<double>(s.slew));
	for (int k = 1; k <= 3 * 75; ++k) {
		rig.tick();
		const double expected = 1.0 - std::exp(-static_cast<double>(k) * kTick / tau);
		CAPTURE(k);
		REQUIRE(std::abs(static_cast<double>(rig.held()) - expected) < 1e-5);
	}
}

TEST_CASE("S&H Slew: one time constant in, the output is 1 - 1/e of the way to the new value") {
	sh::Settings s = sh::defaults();
	s.source = sh::Source::Input;
	s.clock = sh::Clock::Trig;
	s.bipolar = false;
	s.slew = static_cast<float>(std::log2(0.2));
	Rig rig(s);
	rig.tick();
	s.input = 1.0f;
	s.trig = 1.0f;
	rig.apply(s);
	rig.tick(150); // 150 ticks at 48 kHz is 200 ms, one tau
	CHECK(std::abs(static_cast<double>(rig.held()) - (1.0 - std::exp(-1.0))) < 1e-5);
	CHECK(rig.device().mono.held() == 1.0f); // the hold itself took the value at once
}

TEST_CASE("S&H Trig clocking: the sample is taken on the tick the Trig reaches 0.5, and not again until below 0.25") {
	sh::Settings s = sh::defaults();
	s.source = sh::Source::Input;
	s.clock = sh::Clock::Trig;
	s.input = 0.2f;
	Rig rig(s);
	rig.tick(); // the first sample: 0.2
	REQUIRE(rig.held() == 0.2f);
	s.input = 0.8f;
	const auto step = [&](float trig) {
		s.trig = trig;
		rig.apply(s);
		rig.tick();
	};
	step(0.3f);
	CHECK(rig.held() == 0.2f);
	CHECK(rig.pulse() == 0.0f);
	step(0.49f);
	CHECK(rig.held() == 0.2f);
	step(0.5f); // THE CROSSING TICK
	CHECK(rig.held() == 0.8f);
	CHECK(rig.pulse() == 1.0f);
	s.input = -0.4f;
	step(0.9f); // still high: no second fire
	CHECK(rig.held() == 0.8f);
	step(0.3f); // between the levels: not re-armed
	step(0.6f);
	CHECK(rig.held() == 0.8f);
	step(0.2f); // re-armed
	step(0.5f);
	CHECK(rig.held() == -0.4f);
}

TEST_CASE("S&H Trig takes a sample under every Clock, and the Rate clock goes on beside it") {
	sh::Settings s = atHz(1.0);
	s.source = sh::Source::Input;
	s.input = 0.1f;
	Rig rig(s);
	rig.tick();
	REQUIRE(rig.held() == 0.1f);
	s.input = 0.6f;
	s.trig = 1.0f;
	rig.apply(s);
	rig.tick();
	CHECK(rig.held() == 0.6f);
	CHECK(rig.taken() == 2u);
	CHECK(rig.clocks() == 1u); // the fire was a clock; Rate's first is still a second away
}

TEST_CASE("S&H Clock Note: a new note takes a sample under mono, and nothing else does") {
	sh::Settings s = sh::defaults();
	s.clock = sh::Clock::Note;
	Rig rig(s);
	rig.tick();
	const float first = rig.held();
	rig.tick(5 * kTicksPerSecond);
	CHECK(rig.held() == first);
	CHECK(rig.taken() == 1u);
	rig.ctx.retrigger = true;
	rig.tick();
	rig.ctx.retrigger = false;
	CHECK(rig.taken() == 2u);
	CHECK(rig.pulse() == 1.0f);
	CHECK(rig.held() != first);
}

TEST_CASE("S&H Random clock: Poisson hits at Rate's mean, from the shared random-events core") {
	// 8 Hz for 100 s: 800 expected, Poisson sd 28; the bound is about four of them.
	sh::Settings s = atHz(8.0);
	s.clock = sh::Clock::Random;
	Rig rig(s);
	rig.tick(100 * kTicksPerSecond);
	CAPTURE(rig.clocks());
	CHECK(rig.clocks() > 690u);
	CHECK(rig.clocks() < 910u);
	// AND IRREGULAR: Rate's clock would put every gap at 93 or 94 ticks; Poisson's spread is wide.
	Rig again(s);
	std::vector<int> gaps;
	int last = -1;
	for (int i = 0; i < 60 * kTicksPerSecond; ++i) {
		again.tick();
		if (again.pulse() == 1.0f) {
			if (last >= 0) {
				gaps.push_back(i - last);
			}
			last = i;
		}
	}
	REQUIRE(gaps.size() > 100u);
	const auto [lo, hi] = std::minmax_element(gaps.begin(), gaps.end());
	CHECK(*lo < 20);
	CHECK(*hi > 300);
}

TEST_CASE("S&H seed: Stable plays the same staircase from the same Seed, another Seed another; Free never restarts, from the Seed too") {
	const auto staircase = [](Rig& rig) { return run(rig, 5 * kTicksPerSecond); };
	sh::Settings s = atHz(20.0);
	s.seed = 5;
	Rig a(s);
	Rig b(s);
	const std::vector<float> first = staircase(a);
	CHECK(staircase(b) == first);
	// A RESET RESTARTS A STABLE STREAM: the same staircase again.
	a.mod.reset(a.mem);
	CHECK(staircase(a) == first);
	// ANOTHER SEED, ANOTHER STAIRCASE.
	s.seed = 6;
	Rig c(s);
	CHECK(staircase(c) != first);
	// FREE: a reset does not restart it — but it is the SEED'S stream, so two instances born alike play alike (backlog
	// NM: a process-wide construction count was its seed, which a reload into a fresh instance could not reproduce).
	s.seedMode = sh::SeedMode::Free;
	s.seed = 5;
	Rig f(s);
	Rig g(s);
	const std::vector<float> free1 = staircase(f);
	CHECK(staircase(g) == free1);
	CHECK(free1 != first); // its own course: not Stable's staircase under the same Seed
	f.mod.reset(f.mem);
	CHECK(staircase(f) != free1);
	// ANOTHER SEED, ANOTHER FREE STREAM — and a Seed moved under a running Free hold takes effect at once.
	s.seed = 6;
	Rig h(s);
	CHECK(staircase(h) != free1);
	s.seed = 5;
	Rig moved(s);
	Rig stayed(s);
	(void)staircase(moved);
	(void)staircase(stayed);
	s.seed = 6;
	moved.apply(s);
	CHECK(staircase(moved) != staircase(stayed));
	// AND THE TRANSPORT STARTING RESTARTS A STABLE ONE: press play, hear the top.
	s.seedMode = sh::SeedMode::Stable;
	s.seed = 5;
	Rig t(s);
	(void)staircase(t);
	t.ctx.clock.playing = true;
	CHECK(staircase(t) == first);
}

TEST_CASE("S&H colours: white jumps anywhere, brown steps at most kBrownStep/2 a clock and still covers the range") {
	const auto steps = [](sh::Source source) {
		sh::Settings s = atHz(50.0);
		s.source = source;
		s.bipolar = false;
		Rig rig(s);
		std::vector<float> held;
		for (int i = 0; i < 200 * kTicksPerSecond; ++i) {
			rig.tick();
			if (rig.pulse() == 1.0f) {
				held.push_back(rig.held());
			}
		}
		return held;
	};
	const auto meanJump = [](const std::vector<float>& v) {
		double sum = 0.0;
		for (size_t i = 1; i < v.size(); ++i) {
			sum += std::abs(static_cast<double>(v[i]) - v[i - 1]);
		}
		return sum / static_cast<double>(v.size() - 1);
	};
	const std::vector<float> white = steps(sh::Source::White);
	const std::vector<float> pink = steps(sh::Source::Pink);
	const std::vector<float> brown = steps(sh::Source::Brown);
	CAPTURE(meanJump(white));
	CAPTURE(meanJump(pink));
	CAPTURE(meanJump(brown));
	CHECK(std::abs(meanJump(white) - 1.0 / 3.0) < 0.02); // E|U - V| for two uniforms
	for (size_t i = 1; i < brown.size(); ++i) {
		REQUIRE(std::abs(static_cast<double>(brown[i]) - brown[i - 1]) <= 0.5 * sh::kBrownStep + 1e-6);
	}
	CHECK(meanJump(brown) < meanJump(pink));
	CHECK(meanJump(pink) < meanJump(white));
	const auto [lo, hi] = std::minmax_element(brown.begin(), brown.end());
	CHECK(*lo < 0.05f);
	CHECK(*hi > 0.95f);
	for (const float v : brown) {
		REQUIRE(v >= 0.0f);
		REQUIRE(v <= 1.0f);
	}
}

TEST_CASE("S&H Trig Out is one tick of exactly 1 per sample taken: unscaled by Amount, unsmoothed by Slew") {
	sh::Settings s = atHz(7.5);
	s.amount = 0.3f;
	s.slew = static_cast<float>(std::log2(0.5));
	Rig rig(s);
	int pulses = 0;
	for (int i = 0; i < 1000; ++i) {
		rig.tick();
		const float p = rig.pulse();
		REQUIRE((p == 0.0f || p == 1.0f));
		if (p == 1.0f) {
			++pulses;
			rig.tick();
			REQUIRE(rig.pulse() == 0.0f); // ONE tick wide
			++i;
		}
	}
	CHECK(pulses == static_cast<int>(rig.taken()));
	// AND AMOUNT SCALES THE HELD OUTPUT, which is where it belongs.
	CHECK(rig.held() == doctest::Approx(0.3 * static_cast<double>(rig.device().mono.value())));
	// A SKIPPED CLOCK EMITS NO PULSE.
	s.probability = 0.0f;
	Rig never(s);
	never.tick();
	REQUIRE(never.pulse() == 1.0f); // the first sample
	for (int i = 0; i < 1000; ++i) {
		never.tick();
		REQUIRE(never.pulse() == 0.0f);
	}
}

TEST_CASE("S&H synced on a playing transport steps on the division, with the song") {
	// 120 bpm, 1/4: a step every beat, 0.5 s, 375 ticks; the song's position is what decides.
	sh::Settings s = sh::defaults();
	s.timeBase = 1 + 12; // "1/4" — Hz, then the divisions in `device::kDivisionBeats` order
	REQUIRE(std::string(sh::kTimeBaseNames[s.timeBase]) == "1/4");
	Rig rig(s);
	rig.ctx.clock.secondsPerBeat = 0.5;
	rig.ctx.clock.playing = true;
	std::vector<double> at;
	double beats = 0.25; // starting a quarter beat in
	for (int i = 0; i < 4 * 375; ++i) {
		rig.ctx.clock.songPositionBeats = beats;
		rig.tick();
		if (rig.pulse() == 1.0f && i > 0) {
			at.push_back(beats);
		}
		beats += kTick / 0.5;
	}
	REQUIRE(at.size() == 4u); // 1500 ticks from beat 0.25 is to beat 4.25: the lines at 1, 2, 3 and 4
	for (size_t i = 0; i < at.size(); ++i) {
		CAPTURE(i);
		CHECK(at[i] >= static_cast<double>(i + 1));
		CHECK(at[i] < static_cast<double>(i + 1) + kTick / 0.5);
	}
}

namespace {

/// THE RACK, POLY: two notes on two voices, a S&H under test and two DAHDSRs used only as per-voice SOURCES (their
/// per-voice cells written by hand, the way `test_mod_trig`'s PolyRig drives them).
struct PolyRig {
	static constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4,
		                                     .outputStride = 4 };
	rack::RackEngine rack { kAxes, hostCatalogue() };
	size_t trigSrc = 0;
	size_t inputSrc = 0;
	size_t dst = 0;

	explicit PolyRig(bool perVoiceTrig) {
		rack.setVoices(2);
		rack.prepare(kRate, 4096);
		trigSrc = rack.addDevice(mod::dahdsr::kTraits.tag);
		inputSrc = rack.addDevice(mod::dahdsr::kTraits.tag);
		dst = rack.addDevice(sh::kTraits.tag);
		REQUIRE(dst != rack::RackEngine::kNoSlot);
		rack.setField(dst, static_cast<size_t>(sh::Param::Poly), 1.0f);
		rack.setField(dst, static_cast<size_t>(sh::Param::Source), static_cast<float>(sh::Source::Input));
		rack.setField(dst, static_cast<size_t>(sh::Param::Clock), static_cast<float>(sh::Clock::Trig));
		rack.setRouteAmount(perVoiceTrig ? test::outputOf(rack, trigSrc, 0) : test::macro(0),
		                    test::controlOf(rack, dst, static_cast<size_t>(sh::Param::Trig)), 1.0f);
		// HALF DEPTH OVER INPUT'S SPAN OF 2: the resolved Input IS the source's value.
		rack.setRouteAmount(test::outputOf(rack, inputSrc, 0),
		                    test::controlOf(rack, dst, static_cast<size_t>(sh::Param::Input)), 0.5f);
		rack.setGate(true);
		device::NoteEvent on[2] {};
		for (size_t i = 0; i < 2; ++i) {
			on[i].kind = device::NoteEvent::Kind::On;
			on[i].noteId = -1;
			on[i].channel = 0;
			on[i].key = static_cast<int16_t>(60 + 2 * i);
			rack.setVoiceNote(i, -1, 0, on[i].key);
		}
		device::NoteEvent out[8];
		REQUIRE(rack.processNotesNow(on, 2, out, 8) == 2);
	}
	void voiceInput(size_t voice, float v) { rack.writeOutputForVoice(test::idAt(rack, inputSrc), 0, voice, v); }
	void voiceTrig(size_t voice, float v) { rack.writeOutputForVoice(test::idAt(rack, trigSrc), 0, voice, v); }
	void tick(int times) {
		for (int i = 0; i < times; ++i) {
			rack.tickModulators(mod::kBlockSamples, backplane::unknownBlock());
		}
	}
	float note(int16_t key) const { return rack.outputForNoteAt(dst, 0, -1, 0, key); }
	float notePulse(int16_t key) const { return rack.outputForNoteAt(dst, 1, -1, 0, key); }
};

} // namespace

TEST_CASE("S&H under Poly holds per voice: each note samples its own voice's Input, on its own voice's Trig") {
	PolyRig rig(true);
	rig.voiceInput(0, 0.2f);
	rig.voiceInput(1, 0.7f);
	rig.tick(3); // each note's first sample is its own voice's Input
	CHECK(rig.note(60) == doctest::Approx(0.2f).epsilon(1e-5));
	CHECK(rig.note(62) == doctest::Approx(0.7f).epsilon(1e-5));

	// VOICE 0's TRIG FIRES: its note takes voice 0's new Input; voice 1's note holds, bit for bit.
	const float before62 = rig.note(62);
	rig.voiceInput(0, -0.5f);
	rig.voiceInput(1, 0.9f);
	rig.voiceTrig(0, 1.0f);
	rig.tick(1);
	CHECK(rig.note(60) == doctest::Approx(-0.5f).epsilon(1e-5));
	CHECK(rig.notePulse(60) == 1.0f);
	CHECK(rig.note(62) == before62);
	CHECK(rig.notePulse(62) == 0.0f);
	rig.tick(1);
	CHECK(rig.notePulse(60) == 0.0f);

	// AND VOICE 1's OWN EDGE, later: its note takes ITS input, and voice 0's (held high, not re-armed) does not.
	rig.voiceInput(0, 0.33f);
	rig.voiceTrig(1, 1.0f);
	rig.tick(1);
	CHECK(rig.note(62) == doctest::Approx(0.9f).epsilon(1e-5));
	CHECK(rig.note(60) == doctest::Approx(-0.5f).epsilon(1e-5));
}

TEST_CASE("S&H under Poly with one Trig for every voice: a fire samples every note, each from its own voice") {
	PolyRig rig(false);
	rig.voiceInput(0, 0.1f);
	rig.voiceInput(1, 0.4f);
	rig.tick(2);
	rig.voiceInput(0, 0.6f);
	rig.voiceInput(1, -0.3f);
	rig.rack.setMacro(0, 1.0f);
	rig.tick(1);
	CHECK(rig.note(60) == doctest::Approx(0.6f).epsilon(1e-5));
	CHECK(rig.note(62) == doctest::Approx(-0.3f).epsilon(1e-5));
}

TEST_CASE("S&H under Poly, noise: two notes struck together hold different staircases, each on its own clock") {
	// Free and Stable both: Stable seeds each note by its ordinal, so the second note is not the first one's copy.
	for (const sh::SeedMode mode : { sh::SeedMode::Stable, sh::SeedMode::Free }) {
		CAPTURE(static_cast<int>(mode));
		PolyRig rig(true);
		rig.rack.setField(rig.dst, static_cast<size_t>(sh::Param::Source), static_cast<float>(sh::Source::White));
		rig.rack.setField(rig.dst, static_cast<size_t>(sh::Param::Clock), static_cast<float>(sh::Clock::Rate));
		rig.rack.setField(rig.dst, static_cast<size_t>(sh::Param::SeedMode), static_cast<float>(mode));
		rig.rack.setField(rig.dst, static_cast<size_t>(sh::Param::Rate), static_cast<float>(std::log2(10.0)));
		int differ = 0;
		for (int i = 0; i < 20; ++i) {
			rig.tick(75);
			differ += rig.note(60) != rig.note(62) ? 1 : 0;
		}
		CHECK(differ >= 19);
	}
}

TEST_CASE("S&H is registered: a modulator with a card, two per-voice outputs, and Trig Out free of the host's Amount") {
	const device::Descriptor* d = nullptr;
	for (const device::Descriptor& desc : kDevices) {
		if (device::sameTag(desc.traits.tag, sh::kTraits.tag)) {
			d = &desc;
		}
	}
	REQUIRE(d != nullptr);
	CHECK(d->category == device::Category::Modulators);
	CHECK(d->mod == &sh::kModOps);
	CHECK(d->notes == &sh::kNoteOps);
	CHECK(d->traits.amountControl == -1); // the host would scale the pulse with it
	REQUIRE(d->outputs.size() == 2u);
	CHECK(d->outputs[0].perVoice);
	CHECK(d->outputs[1].perVoice);
	CHECK_FALSE(d->outputs[1].bipolar);
}

TEST_CASE("S&H Quantize reads \"off\" at the bottom of its travel, where it is off, and a count above it") {
	const device::ParamDesc& q = sh::kParamDescs[static_cast<size_t>(sh::Param::Quantize)];
	CHECK(device::formatValue(q, 0.0) == "off");
	CHECK(device::formatValue(q, 12.0) == "12");
	CHECK(device::editText(q, 0.0) == "off");
}

//------------------------------------------------------------------------------------------------//
// THE HOLD PERSISTS WITH THE PATCH (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS")    //
//------------------------------------------------------------------------------------------------//

namespace {


/// A FREE, PINK HOLD AT 30 Hz with a slew — every part of the block in play: the streams, the rows, the walk, the slew.
sh::Settings freePink() {
	sh::Settings s = atHz(30.0);
	s.source = sh::Source::Pink;
	s.seedMode = sh::SeedMode::Free;
	s.seed = 41;
	s.slew = 0.3f;
	return s;
}

} // namespace

TEST_CASE("S&H persistence: a Free hold saved mid-run and loaded into a fresh rack holds the saved value and takes the "
          "same samples after, bit for bit") {
	persist::Rig run;
	run.seat(sh::kTraits.tag, sh::rowOf(freePink()));
	run.block(2000); // about 40 samples taken at 30 Hz
	const std::string saved = run.save();
	const float heldAtSave = run.as<sh::Device>().mono.held();
	const uint32_t takenAtSave = run.as<sh::Device>().mono.taken();
	REQUIRE(takenAtSave > 20u);

	persist::Rig back;
	back.load(saved);
	back.bridge->drain();
	// THE VALUE ITSELF, ON THE FRESH INSTANCE, BEFORE IT HAS TICKED: what was held, and how many were taken.
	CHECK(std::bit_cast<uint32_t>(back.as<sh::Device>().mono.held()) == std::bit_cast<uint32_t>(heldAtSave));
	CHECK(back.as<sh::Device>().mono.taken() == takenAtSave);
	// AND FROM HERE THE TWO ARE ONE STREAM: every block's output to the bit.
	int moves = 0;
	float was = heldAtSave;
	for (int b = 0; b < 2000; ++b) {
		CAPTURE(b);
		run.block();
		back.block();
		REQUIRE(std::bit_cast<uint32_t>(back.as<sh::Device>().mono.value()) == std::bit_cast<uint32_t>(run.as<sh::Device>().mono.value()));
		moves += run.as<sh::Device>().mono.held() != was ? 1 : 0;
		was = run.as<sh::Device>().mono.held();
	}
	CHECK(moves > 10); // NOT VACUOUS: the hold moved, so the agreement is of two running streams

	// AND THE CONTROL: the same patch with its block taken out loads a hold that has taken nothing — so it is the block,
	// not the settings, that carried the value.
	rack::RackPatch patch;
	REQUIRE(rack::parseRackText(saved, &patch));
	REQUIRE(patch.device.size() == 1u);
	const size_t before = patch.device[0].extra.size();
	std::erase_if(patch.device[0].extra, [](const rack::RackPatch::Value& v) { return v.key == "snh_hold"; });
	REQUIRE(patch.device[0].extra.size() == before - 1);
	std::string stripped;
	REQUIRE(rack::rackToText(patch, &stripped));
	persist::Rig fresh;
	fresh.load(stripped);
	CHECK(fresh.as<sh::Device>().mono.taken() == 0u);
	CHECK(std::bit_cast<uint32_t>(fresh.as<sh::Device>().mono.held()) != std::bit_cast<uint32_t>(heldAtSave));
}

TEST_CASE("S&H: its newest samples, newest first, are every sample's count and raw level, so a frame can draw them all") {
	// RATE AT THE TOP OF ITS TRAVEL, the fastest clock it makes. Bipolar on (the default), so raw = (held + 1) / 2.
	sh::Settings s = sh::defaults();
	s.timeScale = static_cast<float>(sh::kParamDescs[static_cast<size_t>(sh::Param::Rate)].max);
	Rig rig(s);
	std::vector<std::pair<uint32_t, float>> taken;
	for (int k = 0; k < 4 * kTicksPerSecond; ++k) {
		rig.tick();
		if (rig.pulse() == 1.0f) {
			const float held = rig.device().mono.held();
			taken.emplace_back(rig.taken(), 0.5f * (held + 1.0f));
		}
	}
	REQUIRE(taken.size() > device::kRecentEvents);
	std::array<device::RecentEvent, device::kRecentEvents> recent {};
	REQUIRE(rig.mod.recentEvents != nullptr);
	const size_t n = rig.mod.recentEvents(rig.mem, recent.data(), recent.size());
	REQUIRE(n == device::kRecentEvents);
	for (size_t i = 0; i < n; ++i) {
		CAPTURE(i);
		const auto& want = taken[taken.size() - 1 - i];
		CHECK(recent[i].ordinal == want.first);
		CHECK(recent[i].value == want.second);
	}
	// AND THE NEWEST IS WHERE THE PLAYHEAD SAYS THE HOLD STANDS.
	CHECK(recent[0].value == rig.device().mono.playhead().level);
}

TEST_CASE("S&H under Rate in Hz: a Trig restarts the clock, so the next sample is a whole period after the fire") {
	// 1 Hz, 750 ticks a period. A fire at tick 375 (half way): the next Rate clock is at 375 + 750, not at 750.
	sh::Settings s = atHz(1.0);
	Rig rig(s);
	rig.tick(); // the first sample
	std::vector<int> at;
	for (int k = 1; k < 3 * kTicksPerSecond; ++k) {
		s.trig = k == kTicksPerSecond / 2 ? 1.0f : 0.0f;
		rig.apply(s);
		rig.tick();
		if (rig.pulse() == 1.0f) {
			at.push_back(k);
		}
	}
	REQUIRE(at.size() >= 3u);
	CHECK(at[0] == kTicksPerSecond / 2); // the fire's own sample
	CHECK(std::abs(at[1] - (kTicksPerSecond / 2 + kTicksPerSecond)) <= 1);
	CHECK(std::abs(at[2] - (kTicksPerSecond / 2 + 2 * kTicksPerSecond)) <= 1);
}

TEST_CASE("S&H synced on a playing transport: a Trig re-locks the song's step to the fire, and the steps after it fall a "
          "whole division from it") {
	// 120 bpm, 1/4: a step every beat. From beat 0.25, a fire at beat 1.5: the clocks after it are at 2.5 and 3.5,
	// not on the bar's lines at 2 and 3.
	sh::Settings s = sh::defaults();
	s.timeBase = 1 + 12;
	REQUIRE(std::string(sh::kTimeBaseNames[s.timeBase]) == "1/4");
	const auto play = [&](sh::SeedMode seedMode, std::vector<float>* holds) {
		sh::Settings run = s;
		run.seedMode = seedMode;
		run.seed = 4;
		Rig rig(run);
		rig.ctx.clock.secondsPerBeat = 0.5;
		rig.ctx.clock.playing = true;
		std::vector<double> at;
		double beats = 0.25;
		bool fired = false;
		for (int i = 0; i < 4 * 375; ++i) {
			rig.ctx.clock.songPositionBeats = beats;
			const bool fireNow = !fired && beats >= 1.5;
			run.trig = fireNow ? 1.0f : 0.0f;
			rig.apply(run);
			fired = fired || fireNow;
			rig.tick();
			if (rig.pulse() == 1.0f && i > 0) {
				at.push_back(beats);
				if (holds != nullptr) {
					holds->push_back(rig.device().mono.held());
				}
			}
			beats += kTick / 0.5;
		}
		return at;
	};
	for (const sh::SeedMode mode : { sh::SeedMode::Stable, sh::SeedMode::Free }) {
		CAPTURE(static_cast<int>(mode));
		const std::vector<double> at = play(mode, nullptr);
		REQUIRE(at.size() == 4u); // the line at 1, the fire, and two divisions after it: 2.5 and 3.5 (4.25 ends it)
		const double tick = kTick / 0.5;
		CHECK(at[0] >= 1.0);
		CHECK(at[0] < 1.0 + tick);
		CHECK(at[1] >= 1.5);
		CHECK(at[1] < 1.5 + tick);
		for (size_t i = 2; i < at.size(); ++i) {
			CAPTURE(i);
			// A WHOLE DIVISION FROM THE FIRE, to within the tick the crossing is read on.
			CHECK(at[i] - at[1] >= static_cast<double>(i - 1) - tick);
			CHECK(at[i] - at[1] < static_cast<double>(i - 1) + tick);
		}
	}
	// FREE IS STILL DETERMINISTIC: the same song and the same fire hold the same values, and the fire's sample is new.
	std::vector<float> first;
	std::vector<float> again;
	(void)play(sh::SeedMode::Free, &first);
	(void)play(sh::SeedMode::Free, &again);
	CHECK(first == again);
	REQUIRE(first.size() == 4u);
	CHECK(first[1] != first[0]);
}

TEST_CASE("S&H Free synced on a playing transport: a step's sample is the Seed's for that step of the song, however "
          "the song got there") {
	sh::Settings s = sh::defaults();
	s.timeBase = 1 + 12; // 1/4 at 120 bpm: a step every 0.5 s
	s.seedMode = sh::SeedMode::Free;
	s.seed = 9;
	// THE HOLD ITSELF is read below, so the slew (off at its default) does not matter
	const auto playFrom = [&](double fromBeats, double toBeats) {
		Rig rig(s);
		rig.ctx.clock.secondsPerBeat = 0.5;
		rig.ctx.clock.playing = true;
		for (double beats = fromBeats; beats < toBeats; beats += kTick / 0.5) {
			rig.ctx.clock.songPositionBeats = beats;
			rig.tick();
		}
		return rig.device().mono.held();
	};
	// FROM THE TOP TO BEAT 12.5, and STARTED AT BEAT 12.1: on the same song step (12), the same sample.
	const float throughTheSong = playFrom(0.0, 12.5);
	CHECK(playFrom(12.1, 12.5) == throughTheSong);
	// AND ANOTHER STEP IS ANOTHER SAMPLE (not vacuous: the step decides).
	CHECK(playFrom(13.1, 13.5) != throughTheSong);
	// ANOTHER SEED, ANOTHER SAMPLE AT THE SAME STEP.
	s.seed = 10;
	CHECK(playFrom(12.1, 12.5) != throughTheSong);
}

TEST_CASE("S&H persistence in Feathers' modulator strip: the chains block a save writes carries the hold, and a fresh "
          "synth loaded from it holds the same value") {
	const auto seat = [](ParamBridge& bridge) {
		const size_t slot = bridge.addRackDevice(sh::kTraits.tag);
		REQUIRE(slot < kMaxRackSlots);
		const DeviceInstanceId id = bridge.rackDeviceAt(slot);
		const sh::Row row = sh::rowOf(freePink());
		for (size_t i = 0; i < row.size(); ++i) {
			bridge.setDeviceControl(id, i, row[i]);
		}
		return id;
	};
	const auto device = [](ParamBridge& bridge, DeviceInstanceId id) {
		const void* surface = bridge.fxBodies().surfaceOf(id);
		REQUIRE(surface != nullptr);
		return static_cast<const sh::Device*>(surface);
	};
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	ParamBridge bridge;
	const DeviceInstanceId id = seat(bridge);
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	publishEditorLanes(bridge, params, synth);
	for (int b = 0; b < 2000; ++b) {
		synth.render(l.data(), r.data(), 64);
		publishEngineReadings(bridge, synth);
	}
	const rack::RackPatch saved = bridge.chainsPatch(false);
	const float heldAtSave = device(bridge, id)->mono.held();
	REQUIRE(device(bridge, id)->mono.taken() > 20u);

	ParamSet params2;
	Synth synth2 { params2 };
	synth2.activate(kRate, 4096);
	ParamBridge bridge2;
	(void)bridge2.restoreChains(saved);
	(void)synth2.provisionDevices(bridge2.chainState(), bridge2.publishedChains().sequence(), &bridge2.fxBodies());
	publishEditorLanes(bridge2, params2, synth2);
	synth2.render(l.data(), r.data(), 64); // the drain adopts the fresh instance
	DeviceInstanceId back = kNoInstance;
	for (size_t slot = 0; slot < kMaxRackSlots && back == kNoInstance; ++slot) {
		const DeviceInstanceId at = bridge2.rackDeviceAt(slot);
		const device::Descriptor* d = at != kNoInstance ? bridge2.chainRecord().descriptorOf(at) : nullptr;
		back = d != nullptr && device::sameTag(d->traits.tag, sh::kTraits.tag) ? at : kNoInstance;
	}
	REQUIRE(back != kNoInstance);
	// THE SAME HOLD, ONE BLOCK ON IN BOTH: the run renders the block the loaded synth just rendered.
	synth.render(l.data(), r.data(), 64);
	CHECK(device(bridge2, back)->mono.taken() == device(bridge, id)->mono.taken());
	CHECK(std::bit_cast<uint32_t>(device(bridge2, back)->mono.value())
	      == std::bit_cast<uint32_t>(device(bridge, id)->mono.value()));
	CHECK(std::bit_cast<uint32_t>(device(bridge2, back)->mono.held())
	      == std::bit_cast<uint32_t>(device(bridge, id)->mono.held()));
	(void)heldAtSave;
}
