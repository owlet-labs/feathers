/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "mod/tracker/tracker.h"
#include "mod/tracker/tracker_types.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace tracker = feathers::mod::tracker;
namespace device = feathers::device;

namespace {

/// THE TRACKER THROUGH ITS OPS — the two tables the host reaches it by, over one storage.
struct Ops {
	void* mem = nullptr;
	explicit Ops(double rate = 48000.0) {
		const device::ModOps& mod = tracker::kModOps;
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, rate);
		tracker::kNoteOps.prepare(mem, rate);
	}
	~Ops() {
		tracker::kModOps.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	/// A ROW OF DECLARED DEFAULTS with the rule and the glide set — glide at zero so the output is the
	/// target and not a slew toward it.
	void rule(tracker::Rule rule, float glide = 0.0f) {
		std::vector<float> row;
		for (const device::ParamDesc& p : tracker::kDescriptor.params) {
			row.push_back(static_cast<float>(p.def));
		}
		row[static_cast<size_t>(tracker::Param::Rule)] = static_cast<float>(rule);
		row[static_cast<size_t>(tracker::Param::Glide)] = glide;
		tracker::kModOps.setFields(mem, row.data(), row.size());
		tracker::kNoteOps.setFields(mem, row.data(), row.size());
	}
	void feed(device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = -1;
		e.channel = 0;
		e.key = key;
		if (kind == device::NoteEvent::Kind::Expression) {
			e.expressionId = 0;
			e.expressionValue = 0.5;
		}
		device::NoteEvent out[4];
		size_t n = 0;
		tracker::kNoteOps.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1); // every event passes through
	}
	float output(int ticks = 4) {
		device::ModTickContext ctx;
		ctx.gate = true;
		for (int i = 0; i < ticks; ++i) {
			tracker::kModOps.tick(mem, ctx);
		}
		return tracker::kModOps.outputAt(mem, 0);
	}
};

} // namespace

TEST_CASE("backlog EH: only an OFF or a CHOKE lifts a key from the tracker's stack — an expression on a held note leaves the pitch where it is") {
	// SHAPE 6. The note handler was an unqualified `else` over every kind that is not On, and the
	// vocabulary carries Expression too: a pressure message reaching the lane stripped its key from the
	// stack. Under Lowest, pressing on the bass note then jumped the pitch to the next note up.
	Ops t;
	t.rule(tracker::Rule::Lowest);
	t.feed(device::NoteEvent::Kind::On, 60);
	t.feed(device::NoteEvent::Kind::On, 64);
	const float lowest = t.output();
	t.feed(device::NoteEvent::Kind::On, 67);
	CHECK(t.output() == lowest); // the rule: a higher note does not move the lowest

	t.feed(device::NoteEvent::Kind::Expression, 60);
	CHECK(t.output() == lowest); // the claim: an expression on the bass note is not its release

	t.feed(device::NoteEvent::Kind::Off, 60);
	const float afterOff = t.output();
	CHECK(afterOff != lowest); // non-vacuity: the OFF does move it, to 64
	t.feed(device::NoteEvent::Kind::Choke, 64);
	CHECK(t.output() != afterOff); // and a choke lifts a key too, to 67
}

TEST_CASE("the tracker's three rules read the stack three ways — by value, root 60, two octaves to full scale") {
	// THE MAPPING RESTATED: (key - root) / (spread x 12), root 60, spread 2 octaves — so 60 reads 0,
	// 72 reads +0.5, 66 reads +0.25. Three keys held in the order 60, 72, 66; each rule names one.
	const auto mapped = [](int key) { return static_cast<float>(key - 60) / 24.0f; };
	for (const tracker::Rule rule : { tracker::Rule::Last, tracker::Rule::Lowest, tracker::Rule::Highest }) {
		CAPTURE(static_cast<int>(rule));
		Ops t;
		t.rule(rule);
		t.feed(device::NoteEvent::Kind::On, 60);
		CHECK(t.output() == doctest::Approx(mapped(60)));
		t.feed(device::NoteEvent::Kind::On, 72);
		t.feed(device::NoteEvent::Kind::On, 66);
		const float out = t.output();
		switch (rule) {
		case tracker::Rule::Last: CHECK(out == doctest::Approx(mapped(66))); break;
		case tracker::Rule::Lowest: CHECK(out == doctest::Approx(mapped(60))); break;
		case tracker::Rule::Highest: CHECK(out == doctest::Approx(mapped(72))); break;
		}
	}
}

TEST_CASE("the glide is the same TIME at every sample rate, and at 48 kHz it is the original per-tick coefficient to the bit") {
	// A tick is a fixed 32 samples, so at 96 kHz it is half as long and the same per-tick coefficient made the glide
	// half the time. The stored number keeps its 48 kHz meaning: `1 - glide^(1/4)` per tick there, and the same time
	// constant elsewhere.
	constexpr float kGlide = 0.95f;
	constexpr int kBlock = 32;
	const float old48k = std::clamp(1.0f - std::pow(kGlide, 0.25f), 0.0f, 1.0f);

	// THE OLD RECURRENCE RESTATED, from the first note at root 60, spread 2 octaves: key 72 maps to +0.5, the output
	// starts at 0. Compared to the bit at 48 kHz, tick for tick.
	{
		Ops t(48000.0);
		t.rule(tracker::Rule::Last, kGlide);
		t.feed(device::NoteEvent::Kind::On, 72);
		float expected = 0.0f;
		for (int tick = 0; tick < 200; ++tick) {
			expected += (0.5f - expected) * old48k;
			CHECK(t.output(1) == expected);
		}
	}

	// THE TIME TO COVER HALF THE DISTANCE, in seconds, at three rates.
	const auto halfTime = [&](double rate) {
		Ops t(rate);
		t.rule(tracker::Rule::Last, kGlide);
		t.feed(device::NoteEvent::Kind::On, 72);
		int ticks = 0;
		while (t.output(1) < 0.25f && ++ticks < 100000) {
		}
		REQUIRE(ticks < 100000);
		return (static_cast<double>(ticks) + 1.0) * kBlock / rate;
	};
	const double t48 = halfTime(48000.0);
	const double t96 = halfTime(96000.0);
	const double t44 = halfTime(44100.0);
	const double t192 = halfTime(192000.0);
	CAPTURE(t48);
	CAPTURE(t96);
	// WITHIN ONE TICK OF THE COARSER RATE: the measurement itself is quantised to a tick.
	CHECK(std::abs(t96 - t48) <= kBlock / 44100.0 + 1e-9);
	CHECK(std::abs(t44 - t48) <= kBlock / 44100.0 + 1e-9);
	CHECK(std::abs(t192 - t48) <= kBlock / 44100.0 + 1e-9);
	// NON-VACUITY: the half time is a real glide (tens of ms), not an instant arrival.
	CHECK(t48 > 0.02);
}
