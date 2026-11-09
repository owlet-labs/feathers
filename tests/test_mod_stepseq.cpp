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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/stepseq/stepseq.h"
#include "mod/stepseq/stepseq_types.h"
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

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// THE STEP SEQUENCER (backlog MX), by value. Every reference is RESTATED here from the words — the four draws an
// arrival takes, the bounce of a ping-pong, a ratchet's subdivisions — rather than read back from the engine, so a
// mistake in the engine's order disagrees with the case instead of being copied into it.

using namespace feathers;
namespace sq = feathers::mod::stepseq;

namespace {

constexpr double kRate = 48000.0;

/// A SHAPE ON A TRIG CLOCK, its first step entered — what most cases drive, one Advance at a time.
struct Run {
	sq::Settings s;
	sq::Shape shape;
	explicit Run(sq::Settings settings) : s(settings) {
		shape.prepare(kRate);
		tick();
	}
	void tick(int times = 1) {
		for (int i = 0; i < times; ++i) {
			shape.tick(s, false, false, mod::TransportClock::unknown());
		}
	}
	void advance() {
		shape.fire();
		tick();
	}
};

sq::Settings trigClocked(sq::Direction d, size_t steps) {
	sq::Settings s;
	s.clock = sq::Clock::Trig;
	s.direction = d;
	s.steps = steps;
	return s;
}

/// THE RESTATED DICE: the start draw, then four per arrival — probability, Turing roll, Turing value, pick.
struct Restated {
	sq::Dice dice;
	explicit Restated(int seed, uint32_t ordinal = 0) { dice.seed(sq::playSeed(seed, ordinal)); }
	float start() { return dice.unit(); }
	struct Arrival {
		float plays, turing, value, pick;
	};
	Arrival arrive() {
		Arrival a {};
		a.plays = dice.unit();
		a.turing = dice.unit();
		a.value = dice.unit();
		a.pick = dice.unit();
		return a;
	}
};

std::vector<int> visits(sq::Settings s, int count) {
	Run run(s);
	std::vector<int> out { run.shape.stepForTest() };
	for (int i = 1; i < count; ++i) {
		run.advance();
		out.push_back(run.shape.stepForTest());
	}
	return out;
}

std::vector<float> defaultsRow() {
	std::vector<float> row(sq::kParamCount + sq::kExtraFloats, 0.0f);
	for (size_t i = 0; i < sq::kParamCount; ++i) {
		row[i] = static_cast<float>(sq::kParamDescs[i].def);
	}
	return row;
}

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 8,
};

} // namespace

TEST_CASE("step order per direction: forward, back, ping-pong, random and random walk, against the restated rule") {
	CHECK(visits(trigClocked(sq::Direction::Forward, 4), 9) == std::vector<int> { 0, 1, 2, 3, 0, 1, 2, 3, 0 });
	CHECK(visits(trigClocked(sq::Direction::Backward, 4), 9) == std::vector<int> { 3, 2, 1, 0, 3, 2, 1, 0, 3 });
	// THE ENDS ARE NOT PLAYED TWICE: 0 1 2 3 2 1 0 1 ...
	CHECK(visits(trigClocked(sq::Direction::PingPong, 4), 11)
	      == std::vector<int> { 0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2 });
	CHECK(visits(trigClocked(sq::Direction::PingPong, 1), 3) == std::vector<int> { 0, 0, 0 });

	// RANDOM: any step, from the arrival's fourth draw; the start from the restart's one draw.
	for (int seed : { 0, 7, 999 }) {
		CAPTURE(seed);
		sq::Settings s = trigClocked(sq::Direction::Random, 5);
		s.seed = seed;
		Restated r(seed);
		std::vector<int> want { std::min(static_cast<int>(r.start() * 5.0f), 4) };
		for (int i = 1; i < 64; ++i) {
			want.push_back(std::min(static_cast<int>(r.arrive().pick * 5.0f), 4));
		}
		const std::vector<int> got = visits(s, 64);
		CHECK(got == want);
		// NOT VACUOUS: it visits every step, and not in order.
		for (int k = 0; k < 5; ++k) {
			CHECK(std::count(got.begin(), got.end(), k) > 0);
		}
		CHECK(got != visits(trigClocked(sq::Direction::Forward, 5), 64));
	}

	// RANDOM WALK: one step either way from where it is, wrapping.
	sq::Settings w = trigClocked(sq::Direction::RandomWalk, 6);
	w.seed = 3;
	Restated r(3);
	(void)r.start();
	std::vector<int> want { 0 };
	for (int i = 1; i < 64; ++i) {
		const int from = want.back();
		want.push_back(((r.arrive().pick < 0.5f ? from - 1 : from + 1) + 6) % 6);
	}
	CHECK(visits(w, 64) == want);
}

TEST_CASE("probability decides which arrivals play, from the arrival's first draw; a ratchet hits N times per step") {
	// PROBABILITY 30% ON EVERY STEP: the steps that play are the arrivals whose first draw is below 0.3.
	sq::Settings s = trigClocked(sq::Direction::Forward, 8);
	for (sq::Step& st : s.pattern.step) {
		st.probability = 0.3f;
	}
	s.seed = 11;
	Run run(s);
	Restated r(11);
	(void)r.start();
	int want = r.arrive().plays < 0.3f ? 1 : 0;
	for (int i = 1; i < 2000; ++i) {
		run.advance();
		want += r.arrive().plays < 0.3f ? 1 : 0;
	}
	CHECK(run.shape.hitsForTest() == static_cast<uint32_t>(want));
	// NOT VACUOUS: about 30% of 2000.
	CHECK(want > 500);
	CHECK(want < 700);

	// 0% NEVER PLAYS AND 100% ALWAYS DOES.
	sq::Settings never = trigClocked(sq::Direction::Forward, 4);
	for (sq::Step& st : never.pattern.step) {
		st.probability = 0.0f;
	}
	Run none(never);
	for (int i = 0; i < 100; ++i) {
		none.advance();
	}
	CHECK(none.shape.hitsForTest() == 0u);
	Run all(trigClocked(sq::Direction::Forward, 4));
	for (int i = 0; i < 100; ++i) {
		all.advance();
	}
	CHECK(all.shape.hitsForTest() == 101u);

	// A RATCHET OF 3, ON A TRIG EVERY 30 TICKS: once the step's length is known, three pulses at ticks 0, 10 and 20 of
	// each step, each one tick long on the trigger output, and the gate high for half of each hit.
	sq::Settings rat = trigClocked(sq::Direction::Forward, 4);
	for (sq::Step& st : rat.pattern.step) {
		st.ratchet = 3;
	}
	rat.gateLength = 0.5f;
	sq::Shape shape;
	shape.prepare(kRate);
	std::vector<int> pulses;
	std::vector<int> gates;
	for (int t = 0; t < 30 * 6; ++t) {
		if (t > 0 && t % 30 == 0) {
			shape.fire();
		}
		shape.tick(rat, false, false, mod::TransportClock::unknown());
		if (t >= 60 && t < 90) { // the third step: the interval has been measured
			if (shape.output(sq::kTriggerOutput) == 1.0f) {
				pulses.push_back(t - 60);
			}
			if (shape.output(sq::kGateOutput) == 1.0f) {
				gates.push_back(t - 60);
			}
		}
	}
	CHECK(pulses == std::vector<int> { 0, 10, 20 });
	CHECK(gates == std::vector<int> { 0, 1, 2, 3, 4, 10, 11, 12, 13, 14, 20, 21, 22, 23, 24 });
	// THE TRIGGER IS EXACTLY 0 OR 1, never in between: emitted unsmoothed (backlog MV).
}

TEST_CASE("Turing: Lock 100% repeats the loop exactly, Lock 0% never repeats a step, and a locked step is never rewritten") {
	sq::Settings s = trigClocked(sq::Direction::Forward, 8);
	s.turing = true;
	for (size_t i = 0; i < 8; ++i) {
		s.pattern.step[i].value = static_cast<float>(i) / 10.0f;
	}
	s.pattern.step[5].locked = true;

	const auto passes = [](sq::Settings settings, int count) {
		Run run(settings);
		std::vector<std::vector<float>> out(static_cast<size_t>(count));
		for (int p = 0; p < count; ++p) {
			for (int i = 0; i < 8; ++i) {
				if (p != 0 || i != 0) {
					run.advance();
				}
				out[static_cast<size_t>(p)].push_back(run.shape.targetForTest());
			}
		}
		return out;
	};

	s.turingLock = 1.0f;
	const auto kept = passes(s, 6);
	for (int i = 0; i < 8; ++i) {
		CHECK(kept[0][static_cast<size_t>(i)] == s.pattern.step[static_cast<size_t>(i)].value);
	}
	for (size_t p = 1; p < kept.size(); ++p) {
		CHECK(kept[p] == kept[0]);
	}

	s.turingLock = 0.0f;
	const auto fresh = passes(s, 6);
	for (size_t p = 1; p < fresh.size(); ++p) {
		for (size_t i = 0; i < 8; ++i) {
			CAPTURE(p);
			CAPTURE(i);
			if (i == 5) {
				CHECK(fresh[p][i] == s.pattern.step[5].value); // locked: never rewritten
			}
			else {
				CHECK(fresh[p][i] != fresh[p - 1][i]);
			}
		}
	}

	// AND LOCK 100% FREEZES WHAT THE MACHINE HAS WRITTEN: after passes at 0%, raising the Lock repeats that loop.
	Run run(s);
	for (int i = 0; i < 8 * 3; ++i) {
		run.advance();
	}
	s.turingLock = 1.0f;
	run.s = s;
	std::vector<float> first;
	std::vector<float> second;
	for (int i = 0; i < 8; ++i) {
		run.advance();
		first.push_back(run.shape.targetForTest());
	}
	for (int i = 0; i < 8; ++i) {
		run.advance();
		second.push_back(run.shape.targetForTest());
	}
	CHECK(first == second);
	// WHAT IT REPEATS IS THE REWRITTEN LOOP, not the stored one: entry k is step k + 1 (the 24 advances left it on 0).
	for (size_t k = 0; k < 8; ++k) {
		const size_t step = (k + 1) % 8;
		CAPTURE(step);
		if (step == 5) {
			CHECK(first[k] == s.pattern.step[5].value);
		}
		else {
			CHECK(first[k] != s.pattern.step[step].value);
		}
	}
}

TEST_CASE("Advance and Reset land on the tick their Trig crosses 0.5, re-arm below 0.25, and Reset wins a shared tick") {
	sq::Device dev;
	dev.prepare(kRate);
	std::vector<float> row = defaultsRow();
	row[sq::at(sq::Param::Clock)] = static_cast<float>(sq::Clock::Trig);
	device::ModTickContext ctx {};
	const auto tickWith = [&](float trig, float reset) {
		row[sq::at(sq::Param::Trig)] = trig;
		row[sq::at(sq::Param::Reset)] = reset;
		sq::kModOps.setFields(&dev, row.data(), row.size());
		sq::kModOps.tick(&dev, ctx);
		uint32_t ordinal = 0;
		float phase = 0.0f;
		sq::kModOps.playhead(&dev, &ordinal, &phase);
		return static_cast<int>(ordinal);
	};
	CHECK(tickWith(0.0f, 0.0f) == 0);
	CHECK(tickWith(0.3f, 0.0f) == 0);
	CHECK(tickWith(0.49f, 0.0f) == 0);
	CHECK(tickWith(0.5f, 0.0f) == 1); // the crossing tick
	CHECK(tickWith(0.9f, 0.0f) == 1); // held high: once
	CHECK(tickWith(0.3f, 0.0f) == 1); // not yet below a quarter
	CHECK(tickWith(0.6f, 0.0f) == 1); // so this does not fire
	CHECK(tickWith(0.2f, 0.0f) == 1); // re-armed
	CHECK(tickWith(0.55f, 0.0f) == 2);
	CHECK(tickWith(0.0f, 0.0f) == 2);
	CHECK(tickWith(0.6f, 0.0f) == 3);
	CHECK(tickWith(0.0f, 0.6f) == 0); // RESET, on its crossing tick
	CHECK(tickWith(0.6f, 0.0f) == 1);
	CHECK(tickWith(0.0f, 0.0f) == 1);
	// BOTH RISING ON ONE TICK: the Reset wins and the Advance is absorbed.
	CHECK(tickWith(0.6f, 0.6f) == 0);
	CHECK(tickWith(0.0f, 0.0f) == 0);

	// EACH PAD'S COUNT IN ITS OWN FIELD OF THE WORD: five Advance edges (one absorbed), two Resets, no Randomise.
	const uint32_t word = sq::kModOps.trigFires(&dev);
	CHECK(((word >> sq::kFireAdvanceShift) & sq::kFireAdvanceMask) == 5u);
	CHECK(((word >> sq::kFireResetShift) & sq::kFireResetMask) == 2u);
	for (size_t l = 0; l < sq::kLaneCount; ++l) {
		CHECK(((word >> sq::fireRandomShift(static_cast<sq::Lane>(l))) & sq::kFireRandomMask) == 0u);
	}
}

TEST_CASE("Poly: every note runs its own pattern from its own start, and the value output says each note's step") {
	sq::Device dev;
	dev.prepare(kRate);
	std::vector<float> row = defaultsRow();
	row[sq::at(sq::Param::Poly)] = 1.0f;
	row[sq::at(sq::Param::TimeBase)] = 0.0f;                       // Hz
	row[sq::at(sq::Param::Rate)] = static_cast<float>(std::log2(5.0)); // 5 steps a second: 150 ticks a step
	for (size_t i = 0; i < sq::kMaxSteps; ++i) {
		row[sq::valueIndex(i)] = static_cast<float>(i) / 32.0f;
	}
	sq::kModOps.setFields(&dev, row.data(), row.size());
	sq::kNoteOps.setFields(&dev, row.data(), row.size());
	device::ModTickContext ctx {};
	const auto on = [&](int32_t id, int16_t key) {
		device::NoteEvent e {};
		e.kind = device::NoteEvent::Kind::On;
		e.noteId = id;
		e.key = key;
		device::NoteEvent out[2];
		size_t n = 0;
		sq::kNoteOps.processNotes(&dev, &e, 1, out, 2, &n);
		REQUIRE(n == 1);
	};
	const auto tick = [&](int times) {
		for (int i = 0; i < times; ++i) {
			sq::kModOps.tick(&dev, ctx);
		}
	};
	const auto valueOf = [&](int32_t id, int16_t key) {
		return sq::kModOps.outputForNote(&dev, sq::kValueOutput, id, 0, key);
	};
	on(1, 60);
	tick(301); // A: its first tick starts it, then 300 ticks of clock — two whole steps
	on(2, 64);
	tick(76);  // B: half its first step; A: two and a half
	CHECK(valueOf(1, 60) == 2.0f / 32.0f);
	CHECK(valueOf(2, 64) == 0.0f / 32.0f);
	tick(150);
	CHECK(valueOf(1, 60) == 3.0f / 32.0f);
	CHECK(valueOf(2, 64) == 1.0f / 32.0f);
	// A DESTINATION WITH NO VOICE reads the newest note, B.
	CHECK(sq::kModOps.outputAt(&dev, sq::kValueOutput) == 1.0f / 32.0f);

	// AND MONO IS ONE PATTERN for both: Poly off, every voice reads the same step.
	row[sq::at(sq::Param::Poly)] = 0.0f;
	sq::kModOps.setFields(&dev, row.data(), row.size());
	tick(1);
	CHECK(valueOf(1, 60) == valueOf(2, 64));
}

TEST_CASE("Poly Random: two notes struck together draw different orders, each its ordinal's dice, and a replay of the same notes reproduces both exactly") {
	constexpr int kSteps = 16;
	constexpr int kSeed = 11;
	constexpr int kVisits = 48;
	// ONE RUN OF THE SAME PATCH AND THE SAME NOTES: two keys in one event list, then each note's step once a step.
	const auto play = [&]() {
		sq::Device dev;
		dev.prepare(kRate);
		std::vector<float> row = defaultsRow();
		row[sq::at(sq::Param::Poly)] = 1.0f;
		row[sq::at(sq::Param::Steps)] = static_cast<float>(kSteps);
		row[sq::at(sq::Param::Direction)] = static_cast<float>(sq::Direction::Random);
		row[sq::at(sq::Param::Seed)] = static_cast<float>(kSeed);
		row[sq::at(sq::Param::TimeBase)] = 0.0f;                           // Hz
		row[sq::at(sq::Param::Rate)] = static_cast<float>(std::log2(5.0)); // 150 ticks a step
		for (size_t i = 0; i < sq::kMaxSteps; ++i) {
			row[sq::valueIndex(i)] = static_cast<float>(i) / 32.0f; // the value names the step
		}
		sq::kModOps.setFields(&dev, row.data(), row.size());
		sq::kNoteOps.setFields(&dev, row.data(), row.size());
		device::NoteEvent chord[2] {};
		for (size_t i = 0; i < 2; ++i) {
			chord[i].kind = device::NoteEvent::Kind::On;
			chord[i].noteId = static_cast<int32_t>(i + 1);
			chord[i].key = static_cast<int16_t>(60 + 4 * i);
		}
		device::NoteEvent out[4];
		size_t n = 0;
		sq::kNoteOps.processNotes(&dev, chord, 2, out, 4, &n);
		REQUIRE(n == 2);
		device::ModTickContext ctx {};
		std::array<std::vector<int>, 2> order;
		sq::kModOps.tick(&dev, ctx); // the first tick enters each note's first step
		for (int v = 0; v < kVisits; ++v) {
			for (int t = 0; t < 75; ++t) {
				sq::kModOps.tick(&dev, ctx); // to the middle of the step
			}
			for (size_t i = 0; i < 2; ++i) {
				const float value = sq::kModOps.outputForNote(&dev, sq::kValueOutput, static_cast<int32_t>(i + 1), 0,
				                                              static_cast<int16_t>(60 + 4 * i));
				order[i].push_back(static_cast<int>(std::lround(value * 32.0f)));
			}
			for (int t = 0; t < 75; ++t) {
				sq::kModOps.tick(&dev, ctx);
			}
		}
		return order;
	};
	// THE RESTATED ORDER OF ONE ORDINAL'S DICE: the start draw, then an arrival's fourth draw picks the next step.
	const auto restated = [&](uint32_t ordinal) {
		Restated r(kSeed, ordinal);
		std::vector<int> want { std::min(static_cast<int>(r.start() * static_cast<float>(kSteps)), kSteps - 1) };
		while (want.size() < static_cast<size_t>(kVisits)) {
			want.push_back(std::min(static_cast<int>(r.arrive().pick * static_cast<float>(kSteps)), kSteps - 1));
		}
		return want;
	};
	const std::array<std::vector<int>, 2> first = play();
	// THE FIRST NOTE IS ORDINAL 0 — THE SEED'S OWN DICE, the mono pattern bit for bit; the second is ordinal 1.
	CHECK(first[0] == restated(0));
	CHECK(first[1] == restated(1));
	CHECK(first[0] != first[1]);
	// THE SAME PATCH AND NOTES AGAIN, IN A FRESH DEVICE: both orders exactly.
	const std::array<std::vector<int>, 2> again = play();
	CHECK(again[0] == first[0]);
	CHECK(again[1] == first[1]);
}

TEST_CASE("Poly Random: a note's ordinal is the lowest no other held note carries, so a note struck alone is the seed's own pattern") {
	sq::Device dev;
	dev.prepare(kRate);
	std::vector<float> row = defaultsRow();
	row[sq::at(sq::Param::Poly)] = 1.0f;
	sq::kModOps.setFields(&dev, row.data(), row.size());
	sq::kNoteOps.setFields(&dev, row.data(), row.size());
	const auto send = [&](device::NoteEvent::Kind kind, int32_t id, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = id;
		e.key = key;
		device::NoteEvent out[2];
		size_t n = 0;
		sq::kNoteOps.processNotes(&dev, &e, 1, out, 2, &n);
	};
	const auto ordinalOf = [&](int32_t id, int16_t key) {
		const size_t at = dev.poly.notes.indexOfNote(id, 0, key);
		REQUIRE(at < dev.poly.notes.notes.size());
		return dev.poly.notes.notes[at].shape.ordinal();
	};
	send(device::NoteEvent::Kind::On, 1, 60);
	send(device::NoteEvent::Kind::On, 2, 62);
	send(device::NoteEvent::Kind::On, 3, 64);
	CHECK(ordinalOf(1, 60) == 0u);
	CHECK(ordinalOf(2, 62) == 1u);
	CHECK(ordinalOf(3, 64) == 2u);
	// THE MIDDLE ONE LIFTED: the next note takes its ordinal, the lowest free, and not 3.
	send(device::NoteEvent::Kind::Off, 2, 62);
	send(device::NoteEvent::Kind::On, 4, 65);
	CHECK(ordinalOf(4, 65) == 1u);
	// EVERY KEY LIFTED: a note struck alone is 0 again.
	for (const auto& [id, key] : { std::pair<int32_t, int16_t> { 1, 60 }, { 3, 64 }, { 4, 65 } }) {
		send(device::NoteEvent::Kind::Off, id, key);
	}
	send(device::NoteEvent::Kind::On, 5, 67);
	CHECK(ordinalOf(5, 67) == 0u);
}

TEST_CASE("Randomise: Amount 0 moves nothing, 100% is a fresh pattern, between is a variation; locks survive; one press "
          "draws a new pattern from the same seed") {
	sq::Pattern base;
	for (size_t i = 0; i < sq::kMaxSteps; ++i) {
		base.step[i].value = 0.5f;
	}
	base.step[2].locked = true;
	base.step[9].locked = true;

	CHECK(sq::randomise(base, 16, sq::Lane::Value, 0.0f, 4) == base);

	const sq::Pattern fresh = sq::randomise(base, 16, sq::Lane::Value, 1.0f, 4);
	CHECK(fresh.generation == base.generation + 1);
	// RESTATED: two draws a step, the first the value at 100%.
	sq::Dice dice;
	dice.seed((uint64_t { 4 } << 32) ^ (uint64_t { 0 } << 4) ^ static_cast<uint64_t>(sq::Lane::Value)
	          ^ 0xA5A5'0000'0000'0000ull);
	for (size_t i = 0; i < 16; ++i) {
		CAPTURE(i);
		const float draw = dice.unit();
		(void)dice.unit();
		if (base.step[i].locked) {
			CHECK(fresh.step[i] == base.step[i]); // LOCKS SURVIVE
		}
		else {
			CHECK(fresh.step[i].value == std::clamp(0.5f + 1.0f * (draw - 0.5f), 0.0f, 1.0f));
		}
	}
	// PAST THE PATTERN'S LENGTH NOTHING MOVES.
	for (size_t i = 16; i < sq::kMaxSteps; ++i) {
		CHECK(fresh.step[i] == base.step[i]);
	}
	// A VARIATION STAYS NEAR: at 10% no step moves more than a tenth.
	const sq::Pattern near = sq::randomise(base, 16, sq::Lane::Value, 0.1f, 4);
	float moved = 0.0f;
	for (size_t i = 0; i < 16; ++i) {
		CHECK(std::abs(near.step[i].value - 0.5f) <= 0.05f + 1e-6f);
		moved = std::max(moved, std::abs(near.step[i].value - 0.5f));
	}
	CHECK(moved > 0.0f);
	// A SECOND PRESS, SAME SEED: a different pattern, because the generation moved.
	const sq::Pattern again = sq::randomise(fresh, 16, sq::Lane::Value, 1.0f, 4);
	CHECK(again.generation == fresh.generation + 1);
	int differ = 0;
	for (size_t i = 0; i < 16; ++i) {
		differ += again.step[i].value != fresh.step[i].value ? 1 : 0;
	}
	CHECK(differ == 14); // every unlocked step
	// THE OTHER LANES: gates re-drawn, nothing else touched, and the locked steps kept.
	const sq::Pattern gates = sq::randomise(base, 16, sq::Lane::Gate, 1.0f, 4);
	for (size_t i = 0; i < 16; ++i) {
		CHECK(gates.step[i].value == base.step[i].value);
		CHECK(gates.step[i].probability == base.step[i].probability);
		if (base.step[i].locked) {
			CHECK(gates.step[i] == base.step[i]);
		}
	}
	// AND WHAT IT DRAWS PACKS EXACTLY: the stored pattern is what the act made.
	for (const sq::Lane lane : { sq::Lane::Value, sq::Lane::Gate, sq::Lane::Probability, sq::Lane::Ratchet,
	                             sq::Lane::Slew }) {
		const sq::Pattern p = sq::randomise(base, 32, lane, 0.7f, 9);
		std::vector<float> row(sq::kParamCount + sq::kExtraFloats, 0.0f);
		sq::writePattern(p, row.data());
		CHECK(sq::patternOf(row.data(), row.size()) == p);
	}
}

TEST_CASE("the packed step: zero is the default step, and every field round-trips through the word") {
	CHECK(sq::unpackStep(0.0f, 0.0f) == sq::Step {});
	CHECK(sq::packStep(sq::Step {}) == 0.0f);
	sq::Step s;
	s.value = 0.123f;
	s.gate = sq::Gate::Tie;
	s.probability = 0.37f;
	s.ratchet = 8;
	s.slew = 0.99f;
	s.locked = true;
	CHECK(sq::unpackStep(s.value, sq::packStep(s)) == s);
	// A DRAWN PROBABILITY OR SLEW IS STORED AND PLAYED AT ONE RESOLUTION: rounded to the whole percent when it is set,
	// so what plays before a save is what plays after it.
	sq::Step drawn;
	sq::setLaneValue(&drawn, sq::Lane::Probability, 0.3337f);
	sq::setLaneValue(&drawn, sq::Lane::Slew, 0.6661f);
	CHECK(drawn.probability == 0.33f);
	CHECK(drawn.slew == 0.67f);
	CHECK(sq::unpackStep(drawn.value, sq::packStep(drawn)) == drawn);
	// GARBAGE COMES HOME: a NaN and an out-of-range word read as something valid.
	const sq::Step g = sq::unpackStep(std::nanf(""), 1.0e9f);
	CHECK(g.value == 0.0f);
	CHECK(g.ratchet >= 1);
	CHECK(g.ratchet <= sq::kMaxRatchet);
	CHECK(g.probability >= 0.0f);
	CHECK(g.probability <= 1.0f);
}

TEST_CASE("a randomised pattern saves and loads bit-exact through the rack's patch, plays what was heard, and a reseed "
          "leaves it alone") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(sq::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	REQUIRE(engine.fieldsPresentAt(lane) == sq::kParamCount + sq::kExtraFloats);

	sq::Pattern p;
	p = sq::randomise(p, 16, sq::Lane::Value, 1.0f, 21);
	p = sq::randomise(p, 16, sq::Lane::Gate, 0.6f, 21);
	p = sq::randomise(p, 16, sq::Lane::Probability, 0.5f, 21);
	p = sq::randomise(p, 16, sq::Lane::Ratchet, 0.5f, 21);
	p = sq::randomise(p, 16, sq::Lane::Slew, 0.4f, 21);
	p.step[3].locked = true;
	std::vector<float> row = defaultsRow();
	sq::writePattern(p, row.data());
	for (size_t i = sq::kParamCount; i < row.size(); ++i) {
		engine.setField(lane, i, row[i]);
	}
	engine.setField(lane, sq::at(sq::Param::TimeBase), 0.0f);
	engine.setField(lane, sq::at(sq::Param::Rate), 3.0f); // 8 steps a second
	engine.setField(lane, sq::at(sq::Param::Seed), 21.0f);

	rack::RackPatch patch;
	REQUIRE(rack::patchOf(engine, &patch));
	std::string text;
	REQUIRE(rack::rackToText(patch, &text));
	rack::RackPatch back;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &back, &report));
	rack::RackEngine loaded { kAxes, hostCatalogue() };
	rack::applyRack(back, &loaded, &report);
	loaded.prepare(kRate, 4096);
	REQUIRE(loaded.fieldsPresentAt(lane) == engine.fieldsPresentAt(lane));

	// BIT-EXACT, every float of the row — the controls and the pattern.
	for (size_t i = 0; i < engine.fieldsPresentAt(lane); ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(loaded.fieldAt(lane, i)) == std::bit_cast<uint32_t>(engine.fieldAt(lane, i)));
	}
	std::vector<float> loadedRow(sq::kParamCount + sq::kExtraFloats);
	for (size_t i = 0; i < loadedRow.size(); ++i) {
		loadedRow[i] = loaded.fieldAt(lane, i);
	}
	CHECK(sq::patternOf(loadedRow.data(), loadedRow.size()) == p);

	// AND IT PLAYS WHAT WAS HEARD: the two racks publish the same three outputs, block for block.
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	int hits = 0;
	for (int b = 0; b < 4000; ++b) {
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		loaded.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		for (size_t o = 0; o < 3; ++o) {
			CAPTURE(b);
			CAPTURE(o);
			REQUIRE(loaded.publishedOutputAt(lane, o) == engine.publishedOutputAt(lane, o));
		}
		hits += engine.publishedOutputAt(lane, sq::kTriggerOutput) == 1.0f ? 1 : 0;
	}
	CHECK(hits > 10); // NOT VACUOUS: it played (about 42 steps)

	// A RESEED NEVER MOVES A STORED STEP.
	loaded.setField(lane, sq::at(sq::Param::Seed), 500.0f);
	loaded.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	for (size_t i = sq::kParamCount; i < loadedRow.size(); ++i) {
		CHECK(std::bit_cast<uint32_t>(loaded.fieldAt(lane, i)) == std::bit_cast<uint32_t>(loadedRow[i]));
	}
}

TEST_CASE("one sequencer's Trigger, routed at depth 1 into another's Advance, moves it one step per hit") {
	// THE CONVENTION END TO END (backlog MV): a trigger output is a one-tick pulse to 1, so through a route at depth 1
	// it crosses the fire level on its tick and falls below the re-arm level on the next — every hit is one step.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t a = engine.addDevice(sq::kTraits.tag);
	const size_t b = engine.addDevice(sq::kTraits.tag);
	REQUIRE(a != rack::RackEngine::kNoSlot);
	REQUIRE(b != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	engine.setField(a, sq::at(sq::Param::TimeBase), 0.0f);
	engine.setField(a, sq::at(sq::Param::Rate), 3.0f); // 8 steps a second
	std::vector<float> row = defaultsRow();
	sq::Pattern p;
	p = sq::randomise(p, 16, sq::Lane::Gate, 1.0f, 2); // rests among the hits, so a hit is not every step
	p = sq::randomise(p, 16, sq::Lane::Ratchet, 1.0f, 2);
	sq::writePattern(p, row.data());
	for (size_t i = sq::kParamCount; i < row.size(); ++i) {
		engine.setField(a, i, row[i]);
	}
	engine.setField(b, sq::at(sq::Param::Clock), static_cast<float>(sq::Clock::Trig));
	engine.setRouteAmount(test::outputOf(engine, a, sq::kTriggerOutput), test::controlOf(engine, b, sq::at(sq::Param::Trig)),
	                      1.0f);

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	int hits = 0;
	int moves = 0;
	uint32_t was = mod::Playhead::kIdle;
	for (int blk = 0; blk < 3000; ++blk) {
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		hits += engine.publishedOutputAt(a, sq::kTriggerOutput) == 1.0f ? 1 : 0;
		uint32_t ordinal = mod::Playhead::kIdle;
		float phase = 0.0f;
		REQUIRE(engine.playheadAt(b, ordinal, phase));
		if (was != mod::Playhead::kIdle && ordinal != was) {
			++moves;
		}
		was = ordinal;
	}
	CHECK(hits > 20);
	// B moves on the tick after A's hit is published, so a hit on the very last block has not moved it yet.
	CHECK((moves == hits || moves == hits - 1));
}

//------------------------------------------------------------------------------------------------//
// THE TURING MACHINE'S REWRITES ARE THE PATTERN, AND THE PATCH STORES THEM (backlog MX, approved 2026-10-17)      //
//------------------------------------------------------------------------------------------------//

namespace {

/// A RACK AND ITS BRIDGE, run a block at a time the way a host runs them: drain, render, publish. The bridge's record
/// is what a patch is taken from, and `harvestExtraState` is the main thread catching it up.
struct Bridged {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> bridge;
	DeviceInstanceId id = kNoInstance;
	std::array<float, 64> l {};
	std::array<float, 64> r {};

	Bridged() {
		engine.prepare(kRate, 4096);
		bridge = std::make_unique<rack::RackBridge>(engine);
	}
	/// A sequencer at 8 steps a second, free, Turing on at `lock`, its 16 steps a ramp.
	void seat(float lock) {
		id = bridge->addDevice(sq::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge->setField(id, sq::at(sq::Param::Retrig), 0.0f);
		bridge->setField(id, sq::at(sq::Param::TimeBase), 0.0f);
		bridge->setField(id, sq::at(sq::Param::Rate), 3.0f);
		bridge->setField(id, sq::at(sq::Param::Turing), 1.0f);
		bridge->setField(id, sq::at(sq::Param::TuringLock), lock);
		for (size_t s = 0; s < 16; ++s) {
			bridge->setField(id, sq::valueIndex(s), static_cast<float>(s) / 16.0f);
		}
	}
	void block(int times = 1) {
		for (int i = 0; i < times; ++i) {
			bridge->drain();
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			bridge->publish(engine);
		}
	}
	[[nodiscard]] size_t lane() const { return engine.slotOf(id); }
	[[nodiscard]] float value() const { return engine.publishedOutputAt(lane(), sq::kValueOutput); }
	/// The record's extra state, to the bit.
	[[nodiscard]] std::vector<uint32_t> recordBits() const {
		std::vector<uint32_t> out;
		for (size_t i = sq::kParamCount; i < sq::kParamCount + sq::kExtraFloats; ++i) {
			out.push_back(std::bit_cast<uint32_t>(bridge->fieldAt(id, i)));
		}
		return out;
	}
	[[nodiscard]] sq::Pattern recordPattern() const {
		std::vector<float> row(sq::kParamCount + sq::kExtraFloats);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = bridge->fieldAt(id, i);
		}
		return sq::patternOf(row.data(), row.size());
	}
	/// What the running device would publish now — its whole extra state, rewrites in place.
	[[nodiscard]] std::vector<uint32_t> deviceBits() const {
		std::array<float, device::kMaxExtraStateFloats> extra {};
		size_t n = 0;
		(void)engine.extraStateAt(lane(), extra.data(), extra.size(), &n);
		std::vector<uint32_t> out;
		for (size_t i = 0; i < n; ++i) {
			out.push_back(std::bit_cast<uint32_t>(extra[i]));
		}
		return out;
	}
	[[nodiscard]] std::string save() const {
		std::string text;
		REQUIRE(rack::rackToText(bridge->patch(), &text));
		return text;
	}
	void load(const std::string& text) {
		rack::RackPatch patch;
		REQUIRE(rack::parseRackText(text, &patch));
		(void)bridge->loadPatch(patch);
		id = bridge->deviceAtPosition(0);
		REQUIRE(id != kNoInstance);
	}
	/// Fire the Reset for one block and let it go.
	void reset() {
		bridge->setField(id, sq::at(sq::Param::Reset), 1.0f);
		block();
		bridge->setField(id, sq::at(sq::Param::Reset), 0.0f);
	}
};

/// THE STEP VALUES ALONE, the basis left out: what was heard.
std::array<float, sq::kMaxSteps> valuesOf(const sq::Pattern& p) {
	std::array<float, sq::kMaxSteps> out {};
	for (size_t s = 0; s < sq::kMaxSteps; ++s) {
		out[s] = p.step[s].value;
	}
	return out;
}

} // namespace

TEST_CASE("Turing: a save mid-run stores the rewritten loop, and a load resumes it — after a Reset, the next 2000 blocks "
          "equal the uninterrupted run's") {
	Bridged run;
	run.seat(0.5f);
	run.block(600); // about 75 arrivals, half of them rewritten

	const std::string saved = run.save();
	// THE SAVE CAUGHT THE RECORD UP: it holds what the device plays, rewrites in place.
	CHECK(run.recordBits() == run.deviceBits());
	int rewritten = 0;
	for (size_t s = 0; s < 16; ++s) {
		rewritten += run.recordPattern().step[s].value != static_cast<float>(s) / 16.0f ? 1 : 0;
	}
	CHECK(rewritten > 0); // NOT VACUOUS: the machine did rewrite, and what was saved is not the hand's ramp

	Bridged back;
	back.load(saved);
	back.block();
	CHECK(valuesOf(back.recordPattern()) == valuesOf(run.recordPattern()));

	// FROM HERE THE TWO ARE ONE MUSIC: both sent back to step 1 on the same block, the same dice, the same loop.
	run.reset();
	back.reset();
	for (int b = 0; b < 2000; ++b) {
		CAPTURE(b);
		REQUIRE(back.value() == run.value());
		run.block();
		back.block();
	}
}

TEST_CASE("Turing: at Lock 100% the frozen loop survives a save and a load bit for bit, and goes on repeating") {
	Bridged run;
	run.seat(0.3f);
	run.block(400);
	run.bridge->setField(run.id, sq::at(sq::Param::TuringLock), 1.0f); // freeze what it has become
	run.block(300);
	const std::string saved = run.save();
	const std::vector<uint32_t> bits = run.recordBits();

	Bridged back;
	back.load(saved);
	back.block();
	// EVERY STEP FLOAT TO THE BIT (the basis is the load's own, by design).
	std::vector<uint32_t> got = back.recordBits();
	std::vector<uint32_t> want = bits;
	got.erase(got.begin() + static_cast<std::ptrdiff_t>(sq::kBasisFloat));
	want.erase(want.begin() + static_cast<std::ptrdiff_t>(sq::kBasisFloat));
	CHECK(got == want);
	// AND WHAT SURVIVED IS THE MACHINE'S LOOP, not the hand's ramp: the save caught the record up with it.
	int rewritten = 0;
	for (size_t s = 0; s < 16; ++s) {
		rewritten += back.recordPattern().step[s].value != static_cast<float>(s) / 16.0f ? 1 : 0;
	}
	CHECK(rewritten > 0);

	// AND IT REPEATS: two passes of 16 steps at 8 a second (2 s, 1500 blocks each), the second the first again.
	back.reset();
	std::vector<float> first;
	std::vector<float> second;
	for (int b = 0; b < 1500; ++b) {
		first.push_back(back.value());
		back.block();
	}
	for (int b = 0; b < 1500; ++b) {
		second.push_back(back.value());
		back.block();
	}
	CHECK(first == second);
}

TEST_CASE("Turing: a burst of rewrites far faster than the main thread drains still lands the final register") {
	Bridged run;
	run.seat(0.0f); // every arrival rewrites
	run.block(3000); // about 375 rewrites, with no harvest at all
	run.bridge->harvestExtraState();
	CHECK(run.recordBits() == run.deviceBits());
	CHECK(valuesOf(run.recordPattern()) != valuesOf(sq::Pattern {}));
}

TEST_CASE("Turing: a hand's edit outranks a snapshot taken before it") {
	Bridged run;
	run.seat(0.0f);
	run.block(200);
	run.bridge->harvestExtraState();

	// A HAND DRAWS A STEP — a fresh basis, as the card writes — while a snapshot from before it waits.
	run.block(200);
	run.bridge->setField(run.id, sq::valueIndex(3), 0.123f);
	const float hand = device::freshExtraStateBasis();
	run.bridge->setField(run.id, sq::basisIndex(), hand);
	run.bridge->harvestExtraState();
	CHECK(run.recordPattern().step[3].value == 0.123f);
	CHECK(run.recordPattern().basis == hand);
	// AND THE MACHINE TAKES THE HAND'S STEPS: what it publishes from now on carries the hand's basis, so it lands.
	run.block(100);
	run.bridge->harvestExtraState();
	CHECK(run.recordPattern().basis == hand);
	CHECK(run.recordBits() == run.deviceBits());
}

TEST_CASE("Turing: a load into the same running instance plays what was loaded, even when the file's basis is the one "
          "the machine last took") {
	// THE CASE THE LOAD'S STAMP EXISTS FOR: no hand between the save and the load, so the file's basis equals what the
	// running machine took; the load keeps the instance (the ids match). Frozen at the save, rewriting after it.
	Bridged run;
	run.seat(0.0f);
	run.block(200);
	run.bridge->setField(run.id, sq::at(sq::Param::TuringLock), 1.0f);
	run.block(10);
	const std::string saved = run.save();
	const sq::Pattern atSave = run.recordPattern();
	run.bridge->setField(run.id, sq::at(sq::Param::TuringLock), 0.0f);
	run.block(400);
	run.bridge->harvestExtraState();
	REQUIRE(valuesOf(run.recordPattern()) != valuesOf(atSave)); // NOT VACUOUS: it moved on after the save

	run.load(saved); // Lock 100% again, from the file
	run.block(100);
	run.bridge->harvestExtraState();
	CHECK(valuesOf(run.recordPattern()) == valuesOf(atSave));
	// AND THE MACHINE PLAYS IT: what it would publish now is the loaded loop (it has rewritten nothing since).
	std::vector<uint32_t> device = run.deviceBits();
	if (!device.empty()) {
		std::vector<float> row(sq::kParamCount + sq::kExtraFloats, 0.0f);
		for (size_t i = 0; i < device.size(); ++i) {
			row[sq::kParamCount + i] = std::bit_cast<float>(device[i]);
		}
		CHECK(valuesOf(sq::patternOf(row.data(), row.size())) == valuesOf(atSave));
	}
}

TEST_CASE("Turing in Feathers' modulator strip: the engine's snapshot reaches the editor's record, and the chains block "
          "a save writes holds the machine's loop") {
	// FEATHERS' OWN ROAD, beside the rack's above: the strip slot's snapshot published with the engine's readings
	// (`publishEngineReadings`), harvested on the main thread (`ParamBridge::harvestExtraState`).
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	ParamBridge bridge;
	const size_t slot = bridge.addRackDevice(sq::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	const DeviceInstanceId id = bridge.rackDeviceAt(slot);
	REQUIRE(id != kNoInstance);
	bridge.setDeviceControl(id, sq::at(sq::Param::Retrig), 0.0f);
	bridge.setDeviceControl(id, sq::at(sq::Param::TimeBase), 0.0f);
	bridge.setDeviceControl(id, sq::at(sq::Param::Rate), 3.0f);
	bridge.setDeviceControl(id, sq::at(sq::Param::Turing), 1.0f);
	bridge.setDeviceControl(id, sq::at(sq::Param::TuringLock), 0.0f);
	for (size_t s = 0; s < 16; ++s) {
		bridge.setDeviceState(id, s * sq::kStepFloats, static_cast<float>(s) / 16.0f);
	}
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	publishEditorLanes(bridge, params, synth);

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	for (int b = 0; b < 600; ++b) {
		synth.render(l.data(), r.data(), 64);
		publishEngineReadings(bridge, synth);
	}
	bridge.harvestExtraState();

	std::array<float, device::kMaxExtraStateFloats> extra {};
	size_t count = 0;
	DeviceInstanceId about = kNoInstance;
	REQUIRE(synth.stripExtraState(slot, extra.data(), extra.size(), &count, &about) != 0);
	REQUIRE(about == id);
	REQUIRE(count == sq::kExtraFloats);
	const rack::DeviceState& row = bridge.chainRecord().rowOf(id);
	int rewritten = 0;
	for (size_t k = 0; k < count; ++k) {
		CAPTURE(k);
		CHECK(std::bit_cast<uint32_t>(row.field[sq::kParamCount + k]) == std::bit_cast<uint32_t>(extra[k]));
	}
	for (size_t s = 0; s < 16; ++s) {
		rewritten += row.field[sq::valueIndex(s)] != static_cast<float>(s) / 16.0f ? 1 : 0;
	}
	CHECK(rewritten > 0);

	// AND THE SAVE'S BLOCK CARRIES IT, under the device's key.
	const rack::RackPatch saved = bridge.chainsPatch(false);
	bool found = false;
	for (const rack::RackPatch::Device& d : saved.device) {
		if (d.instanceId != id) {
			continue;
		}
		for (const rack::RackPatch::Value& v : d.extra) {
			found = found || v.key == std::string(sq::kTraits.extraStateKey);
		}
	}
	CHECK(found);
}

//------------------------------------------------------------------------------------------------//
// WHERE IT IS PLAYING PERSISTS WITH THE PATCH (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE")   //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Step Seq persistence: off the transport, a save mid-run and a load into a fresh rack resume on the same step "
          "with the same dice — the next 2000 blocks equal the uninterrupted run's") {
	persist::Rig run;
	run.seat(sq::kTraits.tag);
	run.bridge->setField(run.id, sq::at(sq::Param::Retrig), 0.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::TimeBase), 0.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::Rate), 3.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::Direction), static_cast<float>(sq::Direction::Random));
	run.bridge->setField(run.id, sq::at(sq::Param::Seed), 17.0f);
	for (size_t s = 0; s < 16; ++s) {
		run.bridge->setField(run.id, sq::valueIndex(s), static_cast<float>(s) / 16.0f);
	}
	run.block(3111); // about 32 steps at 8 a second, and mid-step
	const std::string saved = run.save();
	const sq::Shape& at = run.as<sq::Device>().poly.mono;
	const int step = at.stepForTest();
	const int next = at.nextForTest();
	const uint32_t arrivals = at.arrivalsForTest();
	REQUIRE(arrivals > 20u);

	persist::Rig back;
	back.load(saved);
	const sq::Shape& loaded = back.as<sq::Device>().poly.mono;
	CHECK(loaded.stepForTest() == step);
	CHECK(loaded.nextForTest() == next);
	CHECK(loaded.arrivalsForTest() == arrivals);
	for (int b = 0; b < 2000; ++b) {
		CAPTURE(b);
		run.block();
		back.block();
		REQUIRE(back.as<sq::Device>().poly.mono.stepForTest() == run.as<sq::Device>().poly.mono.stepForTest());
		REQUIRE(std::bit_cast<uint32_t>(back.as<sq::Device>().poly.mono.value())
		        == std::bit_cast<uint32_t>(run.as<sq::Device>().poly.mono.value()));
	}
	CHECK(run.as<sq::Device>().poly.mono.arrivalsForTest() > arrivals + 15u); // NOT VACUOUS: it went on stepping

	// THE CONTROL: without the block, a load starts from the top — its first arrival is its first.
	rack::RackPatch patch;
	REQUIRE(rack::parseRackText(saved, &patch));
	std::erase_if(patch.device[0].extra, [](const rack::RackPatch::Value& v) { return v.key == "steps_play"; });
	std::string stripped;
	REQUIRE(rack::rackToText(patch, &stripped));
	persist::Rig bare;
	bare.load(stripped);
	bare.block();
	CHECK(bare.as<sq::Device>().poly.mono.arrivalsForTest() < 3u);
}

// BACKLOG NT — the randomise amount says what it is; the output gets an Amount of its own         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NT: the randomise amount keeps its key under its new label, and the output Amount is a new key at unity") {
	const device::ParamDesc& rand = sq::kParamDescs[sq::at(sq::Param::RandomAmount)];
	CHECK(std::string(rand.key) == "random_amount");
	CHECK(std::string(rand.shortName) == "Rand Amt");
	const device::ParamDesc& amount = sq::kParamDescs[sq::at(sq::Param::Amount)];
	CHECK(std::string(amount.key) == "amount");
	CHECK(amount.def == 1.0);
	CHECK(amount.min == -1.0);
	CHECK(amount.max == 1.0);
	CHECK(amount.modulatable);
	CHECK(sq::Settings {}.amount == 1.0f);
	// NOT THE HOST'S: it would scale the Gate and the Trigger with the value (backlog MX).
	CHECK(sq::kTraits.amountControl < 0);
	// AND A ROW CARRIES IT.
	sq::Settings s;
	s.amount = -0.25f;
	const sq::Row row = sq::rowOf(s);
	CHECK(sq::settingsOf(row.data(), row.size()).amount == -0.25f);
}

TEST_CASE("NT: the output Amount scales the Value output and nothing else — Gate and Trigger are unchanged at every Amount, and 100% is the output to the bit") {
	// A REFERENCE DEVICE AT UNITY BESIDE ONE AT EACH AMOUNT, driven by the same Advances: the value is the reference's
	// times the Amount, the gate and the trigger the reference's own.
	sq::Pattern pattern;
	for (size_t i = 0; i < sq::kMaxSteps; ++i) {
		pattern.step[i].value = static_cast<float>((i * 7) % 16) / 15.0f;
		pattern.step[i].gate = i % 5 == 3 ? sq::Gate::Off : sq::Gate::On;
	}
	for (const bool bipolar : { false, true }) {
		for (const float amount : { 1.0f, 0.5f, 0.0f, -1.0f, -0.3f }) {
			CAPTURE(bipolar);
			CAPTURE(amount);
			sq::Device ref;
			sq::Device dev;
			ref.prepare(kRate);
			dev.prepare(kRate);
			std::vector<float> row = defaultsRow();
			row[sq::at(sq::Param::Clock)] = static_cast<float>(sq::Clock::Trig);
			row[sq::at(sq::Param::Bipolar)] = bipolar ? 1.0f : 0.0f;
			sq::writePattern(pattern, row.data());
			std::vector<float> scaledRow = row;
			scaledRow[sq::at(sq::Param::Amount)] = amount;
			device::ModTickContext ctx {};
			int triggers = 0;
			int gates = 0;
			int values = 0;
			for (int t = 0; t < 400; ++t) {
				const float trig = (t % 10) < 3 ? 1.0f : 0.0f;
				row[sq::at(sq::Param::Trig)] = trig;
				scaledRow[sq::at(sq::Param::Trig)] = trig;
				sq::kModOps.setFields(&ref, row.data(), row.size());
				sq::kModOps.setFields(&dev, scaledRow.data(), scaledRow.size());
				sq::kModOps.tick(&ref, ctx);
				sq::kModOps.tick(&dev, ctx);
				const float v = sq::kModOps.outputAt(&ref, sq::kValueOutput);
				CHECK(sq::kModOps.outputAt(&dev, sq::kValueOutput) == v * amount);
				CHECK(sq::kModOps.outputForNote(&dev, sq::kValueOutput, -1, 0, 60) == v * amount);
				if (amount == 1.0f) {
					CHECK(std::bit_cast<uint32_t>(sq::kModOps.outputAt(&dev, sq::kValueOutput)) == std::bit_cast<uint32_t>(v));
				}
				for (const size_t o : { sq::kGateOutput, sq::kTriggerOutput }) {
					CHECK(sq::kModOps.outputAt(&dev, o) == sq::kModOps.outputAt(&ref, o));
					CHECK(sq::kModOps.outputForNote(&dev, o, -1, 0, 60) == sq::kModOps.outputAt(&ref, o));
				}
				triggers += sq::kModOps.outputAt(&dev, sq::kTriggerOutput) == 1.0f ? 1 : 0;
				gates += sq::kModOps.outputAt(&dev, sq::kGateOutput) == 1.0f ? 1 : 0;
				values += v != 0.0f ? 1 : 0;
			}
			// NOT VACUOUS: pulses fired, gates opened, and the value moved off zero.
			CHECK(triggers > 10);
			CHECK(gates > 10);
			CHECK(values > 100);
		}
	}
}

TEST_CASE("NT: each lane's Randomise Trig randomises exactly its lane as its button would, with the same dice, on the crossing tick, once per edge; every other lane is untouched and locks survive") {
	sq::Pattern pattern;
	for (size_t i = 0; i < sq::kMaxSteps; ++i) {
		pattern.step[i].value = static_cast<float>(i) / 32.0f;
	}
	pattern.step[3].locked = true;
	pattern.step[9].locked = true;
	for (const sq::Lane lane : { sq::Lane::Value, sq::Lane::Gate, sq::Lane::Probability, sq::Lane::Ratchet, sq::Lane::Slew }) {
		CAPTURE(static_cast<int>(lane));
		sq::Device dev;
		dev.prepare(kRate);
		std::vector<float> row = defaultsRow();
		row[sq::at(sq::Param::Clock)] = static_cast<float>(sq::Clock::Trig);
		row[sq::at(sq::Param::RandomAmount)] = 0.8f;
		row[sq::at(sq::Param::Seed)] = 17.0f;
		sq::writePattern(pattern, row.data());
		device::ModTickContext ctx {};
		const auto tickWith = [&](float fire) {
			row[sq::at(sq::randomiseParam(lane))] = fire;
			sq::kModOps.setFields(&dev, row.data(), row.size());
			sq::kModOps.tick(&dev, ctx);
		};
		const auto published = [&]() {
			std::array<float, sq::kExtraFloats> extra {};
			REQUIRE(sq::kModOps.publishExtraState(&dev, extra.data(), extra.size()) != 0);
			std::vector<float> full(sq::kParamCount + sq::kExtraFloats, 0.0f);
			std::copy(extra.begin(), extra.end(), full.begin() + static_cast<std::ptrdiff_t>(sq::kParamCount));
			return sq::patternOf(full.data(), full.size());
		};
		const sq::Pattern start = sq::patternOf(row.data(), row.size());
		tickWith(0.0f);
		tickWith(0.49f);
		CHECK(sq::kModOps.publishExtraState(&dev, nullptr, 0) == 0); // nothing fired, nothing to publish

		// THE CROSSING TICK: exactly the press's act — the same function, the same arguments, the same dice.
		tickWith(0.5f);
		const sq::Pattern first = sq::randomise(start, sq::kDefaultSteps, lane, 0.8f, 17);
		REQUIRE(first != start);
		CHECK(published() == first);
		CHECK(dev.poly.settings.pattern == first); // and it is what plays, now
		// THE RECORD'S OLDER PATTERN HANDED BACK BETWEEN THE TICK AND THE PUBLISH (a drain lands there): the roll is still
		// what is published.
		sq::kModOps.setFields(&dev, row.data(), row.size());
		CHECK(published() == first);
		CHECK(first.step[3] == start.step[3]);     // LOCKS SURVIVE
		CHECK(first.step[9] == start.step[9]);
		// ONLY ITS LANE: every other lane of every step reads as it did.
		for (size_t o = 0; o < sq::kLaneCount; ++o) {
			if (o == static_cast<size_t>(lane)) {
				continue;
			}
			for (size_t s = 0; s < sq::kMaxSteps; ++s) {
				CHECK(sq::laneValue(first.step[s], static_cast<sq::Lane>(o)) == sq::laneValue(start.step[s], static_cast<sq::Lane>(o)));
			}
		}
		// ITS OWN PAD'S COUNT MOVED, and no other lane's.
		const auto countOf = [&](sq::Lane l) {
			return (sq::kModOps.trigFires(&dev) >> sq::fireRandomShift(l)) & sq::kFireRandomMask;
		};
		for (size_t o = 0; o < sq::kLaneCount; ++o) {
			CHECK(countOf(static_cast<sq::Lane>(o)) == (o == static_cast<size_t>(lane) ? 1u : 0u));
		}

		// HELD HIGH IT FIRES ONCE; the record not caught up yet, the device goes on playing its roll.
		tickWith(1.0f);
		tickWith(0.3f);
		CHECK(published() == first);
		CHECK(dev.poly.settings.pattern == first);

		// RE-ARMED AND FIRED AGAIN: a second press on the first one's pattern — a new draw, the generation counted.
		tickWith(0.2f);
		tickWith(1.0f);
		const sq::Pattern second = sq::randomise(first, sq::kDefaultSteps, lane, 0.8f, 17);
		CHECK(second.generation == first.generation + 1);
		CHECK(published() == second);
		CHECK(countOf(lane) == 2u);

		// THE RECORD CATCHES UP (the harvest), and the device then plays the record's own — the same steps.
		sq::writePattern(second, row.data());
		tickWith(0.0f);
		CHECK_FALSE(dev.haveRolled);
		CHECK(dev.poly.settings.pattern == second);
	}
}

TEST_CASE("NT: Randomise on a Trig at Rand Amt zero is no act — nothing published, nothing moved") {
	sq::Device dev;
	dev.prepare(kRate);
	std::vector<float> row = defaultsRow();
	row[sq::at(sq::Param::RandomAmount)] = 0.0f;
	device::ModTickContext ctx {};
	row[sq::at(sq::randomiseParam(sq::Lane::Value))] = 1.0f;
	sq::kModOps.setFields(&dev, row.data(), row.size());
	sq::kModOps.tick(&dev, ctx);
	CHECK(sq::kModOps.publishExtraState(&dev, nullptr, 0) == 0);
	CHECK_FALSE(dev.haveRolled);
}

TEST_CASE("NT: two lanes' Randomise Trigs through the rack — each rolls its own lane, the record catches up, the rolls survive a save and a load, and a hand's edit outranks a roll not yet caught up") {
	Bridged run;
	run.seat(1.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::Turing), 0.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::RandomAmount), 1.0f);
	run.bridge->setField(run.id, sq::at(sq::Param::Seed), 5.0f);
	run.bridge->setField(run.id, sq::wordIndex(2), sq::packStep(sq::Step { .value = 2.0f / 16.0f, .locked = true }));
	run.block(4);
	const sq::Pattern before = run.recordPattern();
	REQUIRE(before.step[2].locked);

	// TWO LANES' TRIGS, ONE AFTER THE OTHER: the Gate page's, then the Value page's — each its own lane.
	for (const sq::Lane lane : { sq::Lane::Gate, sq::Lane::Value }) {
		run.bridge->setField(run.id, sq::at(sq::randomiseParam(lane)), 1.0f);
		run.block();
		run.bridge->setField(run.id, sq::at(sq::randomiseParam(lane)), 0.0f);
		run.block();
	}
	const std::string saved = run.save(); // the save catches the record up
	sq::Pattern expect =
	    sq::randomise(sq::randomise(before, sq::kDefaultSteps, sq::Lane::Gate, 1.0f, 5), sq::kDefaultSteps, sq::Lane::Value, 1.0f, 5);
	expect.basis = before.basis;
	CHECK(run.recordPattern() == expect);
	CHECK(run.recordBits() == run.deviceBits());
	CHECK(run.recordPattern().step[2] == before.step[2]);
	int gatesMoved = 0;
	for (size_t s = 0; s < sq::kDefaultSteps; ++s) {
		gatesMoved += expect.step[s].gate != before.step[s].gate ? 1 : 0;
	}
	CHECK(gatesMoved > 0); // NOT VACUOUS: the Gate page's Trig did move gates
	run.block(2);
	CHECK_FALSE(valuesOf(run.recordPattern()) == valuesOf(before)); // NOT VACUOUS

	Bridged back;
	back.load(saved);
	back.block();
	CHECK(valuesOf(back.recordPattern()) == valuesOf(expect));
	for (size_t s = 0; s < sq::kMaxSteps; ++s) {
		CHECK(back.recordPattern().step[s].gate == expect.step[s].gate); // the Gate lane's roll came back too
	}
	CHECK(back.recordPattern().step[2].locked);

	// A HAND'S EDIT OUTRANKS A ROLL NOT YET CAUGHT UP: fire, then draw a step under a fresh basis before any harvest.
	back.bridge->setField(back.id, sq::at(sq::randomiseParam(sq::Lane::Slew)), 1.0f);
	back.block();
	back.bridge->setField(back.id, sq::at(sq::randomiseParam(sq::Lane::Slew)), 0.0f);
	const sq::Pattern hand = back.recordPattern();
	back.bridge->setField(back.id, sq::valueIndex(5), 0.123f);
	const float basis = device::freshExtraStateBasis();
	back.bridge->setField(back.id, sq::basisIndex(), basis);
	back.bridge->harvestExtraState();
	back.block(2);
	back.bridge->harvestExtraState();
	sq::Pattern drawn = hand;
	drawn.step[5].value = 0.123f;
	drawn.basis = basis;
	CHECK(back.recordPattern() == drawn);
	CHECK(back.recordBits() == back.deviceBits());
}
