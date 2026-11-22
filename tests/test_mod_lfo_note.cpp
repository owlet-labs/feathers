/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// THE LFO'S NOTE TIME BASE: the wave runs at the played note's frequency times Rate (a ratio), at the control tick's
// rate, clamped to a quarter of it. This binary replaces global operator new to count allocations on the tick road.

#include "test_heap.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"
#include "mod/per_note.h"

#include "device/registry.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <memory>
#include <string>
#include <vector>

namespace {
std::atomic<long> gNews { 0 };

[[gnu::noinline]] void* countedAlloc(std::size_t n) {
	gNews.fetch_add(1, std::memory_order_relaxed);
	if (void* p = feathers::test::heapAlloc(n == 0 ? 1 : n)) {
		return p;
	}
	throw std::bad_alloc();
}
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

using namespace feathers;
using namespace feathers::mod::lfo;

namespace {

constexpr double kSampleRate = 48000.0;

/// A KEY'S FREQUENCY, restated from the platform's contract (MIDI: A4 = key 69 = 440 Hz), not from our own helper.
double keyHz(int key) {
	return 440.0 * std::pow(2.0, (key - 69) / 12.0);
}

Settings noteSine(double ratio) {
	Settings s;
	s.wave = Waveform::Sine;
	s.bipolar = true;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = kNoteSyncBase;
	s.timeScale = static_cast<float>(std::log2(ratio));
	s.phase = 0.1f; // off the zero crossings: a quarter-rate wave samples 0, 1, 0, -1 from phase 0
	return s;
}

/// THE FREQUENCY OF THE OUTPUT ITSELF — rising zero crossings of the published value per second, over `seconds`.
double measuredHz(Shape& shape, const Settings& s, double seconds) {
	const double tickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;
	const int ticks = static_cast<int>(seconds / tickSeconds);
	int crossings = 0;
	float prev = 0.0f;
	for (int k = 0; k < ticks; ++k) {
		shape.tick(s, false, false, feathers::mod::TransportClock {});
		const float v = shape.value();
		if (k > 0 && prev < 0.0f && v >= 0.0f) {
			++crossings;
		}
		prev = v;
	}
	return crossings / (static_cast<double>(ticks) * tickSeconds);
}

} // namespace

TEST_CASE("lfo Note: the wave runs at the played note's frequency times Rate, measured from the output") {
	REQUIRE(std::string(kTimeBaseNames[kNoteSyncBase]) == "Note");
	constexpr int kA1 = 33; // 55 Hz
	for (const double ratio : { 1.0, 2.0, 3.0 }) {
		const Settings s = noteSine(ratio);
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(static_cast<float>(kA1));
		const double hz = measuredHz(shape, s, 8.0);
		CHECK(hz == doctest::Approx(keyHz(kA1) * ratio).epsilon(0.01));
		// AND THE QUANTITY, exactly: the frequency the tick ran the wave at.
		CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(kA1) * ratio).epsilon(1e-6));
	}
	SUBCASE("a different note is a different frequency") {
		const Settings s = noteSine(1.0);
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(45.0f); // A2, 110 Hz
		CHECK(measuredHz(shape, s, 8.0) == doctest::Approx(110.0).epsilon(0.01));
		CHECK(shape.effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
	}
}

TEST_CASE("lfo Note: before any note the wave sits on C3, and it keeps the note it last had") {
	const Settings s = noteSine(1.0);
	Shape shape;
	shape.prepare(kSampleRate);
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(130.8128).epsilon(1e-4)); // MIDI 48
	shape.setNotePitch(52.0f); // E3
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
	shape.reset(); // a release or a reset does not forget the note
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
}

TEST_CASE("lfo Note: a ratio past the clamp sits AT a quarter of the tick rate, and rateMul counts in it") {
	const double maxHz = kSampleRate / (4.0 * feathers::mod::kBlockSamples); // 187.5, restated
	const Settings s = noteSine(8.0);
	Shape shape;
	shape.prepare(kSampleRate);
	shape.setNotePitch(33.0f); // 55 Hz x 8 = 440 Hz asked
	CHECK(measuredHz(shape, s, 4.0) == doctest::Approx(maxHz).epsilon(0.01));
	CHECK(shape.effectiveHzForTest() == doctest::Approx(maxHz).epsilon(1e-6));
	// A rateMul of 2 on a note that asks for 110 Hz asks 220 Hz: clamped to the same ceiling, not 2 x it.
	const Settings low = noteSine(1.0);
	shape.setNotePitch(45.0f);
	shape.tick(low, false, false, feathers::mod::TransportClock {}, 2.0f);
	CHECK(shape.effectiveHzForTest() == doctest::Approx(maxHz).epsilon(1e-6));
	// Below the ceiling nothing is clamped.
	shape.setNotePitch(33.0f);
	shape.tick(low, false, false, feathers::mod::TransportClock {}, 2.0f);
	CHECK(shape.effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
	// The ceiling follows the sample rate (a quarter of the tick rate).
	Shape fast;
	fast.prepare(96000.0);
	fast.setNotePitch(33.0f);
	fast.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(fast.effectiveHzForTest() == doctest::Approx(375.0).epsilon(1e-6));
}

TEST_CASE("lfo Note: the other bases are untouched by the note") {
	Settings hz = noteSine(1.0);
	hz.timeBase = 0; // Hz: Rate is a frequency, 1X = 1 Hz
	Shape shape;
	shape.prepare(kSampleRate);
	shape.setNotePitch(80.0f);
	shape.tick(hz, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(1.0).epsilon(1e-6));
	CHECK_FALSE(hz.synced());
	Settings div = hz;
	div.timeBase = 13; // 1/4
	CHECK(div.synced());
	CHECK_FALSE(div.noteTracked());
}

TEST_CASE("lfo Note under Poly: every voice's wave runs at its own note; the shared wave at the host's last") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	Poly poly;
	poly.prepare(kSampleRate);
	poly.settings = noteSine(1.0);
	poly.settings.mode = feathers::mod::Mode::LfoRetrig;
	poly.settings.poly = true;
	REQUIRE(poly.poly());
	const device::NoteEvent in[2] = {
		{ device::NoteEvent::Kind::On, 0, 1, 0, 0, 33, 1.0 },
		{ device::NoteEvent::Kind::On, 0, 2, 0, 0, 45, 1.0 },
	};
	device::NoteEvent out[4] {};
	size_t made = 0;
	poly.processNotes(in, 2, out, 4, &made, false, [](Shape& shape, const feathers::mod::PerNoteTable<Shape>::Note*) {
		shape.reset();
	});
	REQUIRE(made == 2);
	device::ModTickContext ctx {};
	ctx.gate = true;
	ctx.notePitch = 52.0f; // the host's last note: E3, 164.81 Hz
	poly.tick(ctx);
	CHECK(poly.shapeForNote(1, 0, 33).effectiveHzForTest() == doctest::Approx(55.0).epsilon(1e-6));
	CHECK(poly.shapeForNote(2, 0, 45).effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
}

TEST_CASE("lfo Note: the shared wave follows ModTickContext::notePitch when Poly is off") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	Poly poly;
	poly.prepare(kSampleRate);
	poly.settings = noteSine(1.0);
	device::ModTickContext ctx {};
	CHECK(ctx.notePitch == 48.0f); // the contract's default is C3
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(48)).epsilon(1e-6));
	ctx.notePitch = 69.0f;
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(187.5).epsilon(1e-6));
	ctx.notePitch = 33.0f;
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(33)).epsilon(1e-6));
}

TEST_CASE("lfo Note: the tick road allocates nothing") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	auto poly = std::make_unique<Poly>();
	poly->prepare(kSampleRate);
	poly->settings = noteSine(1.0);
	poly->settings.mode = feathers::mod::Mode::LfoRetrig;
	poly->settings.poly = true;
	const device::NoteEvent in[1] = { { device::NoteEvent::Kind::On, 0, 1, 0, 0, 40, 1.0 } };
	device::NoteEvent out[2] {};
	size_t made = 0;
	device::ModTickContext ctx {};
	ctx.gate = true;
	const long before = gNews.load();
	poly->processNotes(in, 1, out, 2, &made, false, [](Shape& shape, const feathers::mod::PerNoteTable<Shape>::Note*) {
		shape.reset();
	});
	for (int k = 0; k < 2000; ++k) {
		ctx.notePitch = static_cast<float>(40 + k % 20);
		poly->tick(ctx);
	}
	CHECK(gNews.load() == before);
}

TEST_CASE("lfo Note: the base and the ratio survive save and load") {
	Settings s = noteSine(3.0);
	const Row row = rowOf(s);
	const Settings back = settingsOf(row, row.size());
	CHECK(back.noteTracked());
	CHECK(back.timeBase == kNoteSyncBase);
	CHECK(back.timeScale == doctest::Approx(std::log2(3.0f)));
	// The base is stored by its label: the Choice's own name table says "Note" at the stored index.
	const auto& desc = kParamDescs[static_cast<size_t>(Param::TimeBase)];
	CHECK(std::string(desc.names[static_cast<size_t>(row[static_cast<size_t>(Param::TimeBase)])]) == "Note");
}
