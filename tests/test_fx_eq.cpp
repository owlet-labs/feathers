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

// THE EQ DEVICE, minimum phase. The design library has its own
// suite (test_eq_design); this one is about the device: the codec, identity, what a band measures as at
// a probe frequency against the ANALOG prototype restated here, block-size invariance, smoothing that
// lands, Amount's exact inverse through two devices in series, the Nyquist fade, off, and the rest.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "fx/eq/eq_types.h"
#include "plugin/ui/eq_plot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>


//------------------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — `test_latency.cpp`'s, counting only while armed. It sees `operator new`; the  //
// pffft buffers come from `malloc` and are all taken in `prepare`, which is not armed.                //
//------------------------------------------------------------------------------------------------//

namespace {

struct Allocs {
	bool armed = false;
	size_t allocations = 0;
};
Allocs allocs;

void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
namespace eq = feathers::fx::eq;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// AN OPS INSTANCE, CONSTRUCTED THE WAY A HOST CONSTRUCTS ONE — into storage the test owns.
class Instance {
public:
	explicit Instance(double rate = kRate) : storage_(std::make_unique<Storage>()) {
		REQUIRE(eq::kAudioOps.stateBytes <= sizeof(Storage));
		REQUIRE(eq::kAudioOps.stateAlign <= alignof(std::max_align_t));
		eq::kAudioOps.construct(mem());
		eq::kAudioOps.prepare(mem(), rate, 4096);
	}
	~Instance() { eq::kAudioOps.destruct(mem()); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const eq::Settings& s) {
		const eq::Row row = eq::rowOf(s);
		eq::kAudioOps.setFields(mem(), row.data(), row.size());
	}
	void process(float* l, float* r, int frames) { eq::kAudioOps.processStereo(mem(), l, r, frames); }
	void reset() { eq::kAudioOps.reset(mem()); }
	/// Hand the device a sounding note, the way the lane does (Key Track's road).
	void note(float pitchSemitones) {
		feathers::device::NoteContext n;
		n.pitchSemitones = pitchSemitones;
		n.gate = true;
		eq::kAudioOps.setNoteContext(mem(), n);
	}
	/// Capture the device's readings through the host's sink road.
	void listen() {
		eq::kAudioOps.setReadingsSink(mem(), feathers::device::ReadingsSink {
		                                         this, [](void* context, const float* values, size_t count) {
			                                         auto* self = static_cast<Instance*>(context);
			                                         self->readingCount = count;
			                                         self->lastReading = count > 0 ? values[0] : 0.0f;
			                                         self->allReadings.assign(values, values + count);
		                                         } });
	}
	size_t readingCount = 0;
	float lastReading = -1.0f;
	std::vector<float> allReadings;
	/// Write extra-state fields behind the params, the way the host stores them.
	void setWithExtra(const eq::Settings& s, const std::array<float, eq::kExtraState>& extra) {
		std::vector<float> row(eq::kParamCount + eq::kExtraState);
		const eq::Row params = eq::rowOf(s);
		std::copy(params.begin(), params.end(), row.begin());
		std::copy(extra.begin(), extra.end(), row.begin() + static_cast<long>(eq::kParamCount));
		eq::kAudioOps.setFields(mem(), row.data(), row.size());
	}
	/// A whole row, params then extra state, already laid out (no allocation here).
	void setRow(const std::vector<float>& row) { eq::kAudioOps.setFields(mem(), row.data(), row.size()); }
	[[nodiscard]] const void* storage() const { return storage_->bytes.data(); }

	/// Process `l` and `r` in place in blocks of `block`.
	void run(std::vector<float>& l, std::vector<float>& r, int block) {
		const int n = static_cast<int>(l.size());
		for (int at = 0; at < n; at += block) {
			process(l.data() + at, r.data() + at, std::min(block, n - at));
		}
	}

private:
	struct Storage {
		alignas(std::max_align_t) std::array<std::byte, 1 << 16> bytes {};
	};
	void* mem() { return storage_->bytes.data(); }
	std::unique_ptr<Storage> storage_;
};

/// A flat EQ: every band's gain at 0 dB and every cut off — the default layout.
eq::Settings flat() {
	return eq::Settings {};
}

eq::Settings withBell(int band, double hz, float gainDb, double q) {
	eq::Settings s = flat();
	auto& b = s.bands[static_cast<size_t>(band)];
	b.type = eq::BandType::Bell;
	b.on = true;
	b.freqLog2 = static_cast<float>(std::log2(hz));
	b.gainDb = gainDb;
	b.qLog2 = static_cast<float>(std::log2(q));
	return s;
}

std::vector<float> noise(int n, uint32_t seed) {
	std::vector<float> v(static_cast<size_t>(n));
	uint32_t x = seed;
	for (float& s : v) {
		x = x * 1664525u + 1013904223u;
		s = (static_cast<float>(x >> 8) / 16777216.0f - 0.5f) * 0.8f;
	}
	return v;
}

/// THE STEADY-STATE GAIN, in dB, of a sine at `hz` through a device with settings `s`.
double measuredDb(const eq::Settings& s, double hz) {
	Instance dev;
	dev.set(s);
	const int n = 48000, settle = 24000;
	std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] =
		    static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / kRate));
	}
	const std::vector<float> in = l;
	dev.run(l, r, 256);
	double eo = 0.0, ei = 0.0;
	for (int i = settle; i < n; ++i) {
		eo += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)];
		ei += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
	}
	return 10.0 * std::log10(eo / ei);
}

/// RBJ's analog bell, restated (A = 10^(dB/40)); not eq::analogPrototype.
double analogBellDb(double gainDb, double q, double fOverF0) {
	const double a = std::pow(10.0, gainDb / 40.0);
	const std::complex<double> s(0.0, fOverF0);
	return 20.0 * std::log10(std::abs((s * s + (a / q) * s + 1.0) / (s * s + s / (a * q) + 1.0)));
}

} // namespace

TEST_CASE("EQ: a row round-trips and a short row takes the defaults") {
	eq::Settings s;
	s.enabled = false;
	s.offsetOctaves = -1.25f;
	s.amount = -0.5f;
	s.tiltDbPerOctave = 2.5f;
	s.tiltPivotLog2 = 11.0f;
	s.outputDb = -3.5f;
	for (int b = 0; b < eq::kBandCount; ++b) {
		auto& p = s.bands[static_cast<size_t>(b)];
		p.type = static_cast<eq::BandType>(b % static_cast<int>(eq::BandType::kCount));
		p.on = b % 2 == 0;
		p.freqLog2 = 5.0f + static_cast<float>(b);
		p.gainDb = -12.0f + 3.0f * static_cast<float>(b);
		p.qLog2 = -1.0f + 0.5f * static_cast<float>(b);
		p.slopeIndex = b % eq::kSlopeCount;
	}
	const eq::Row row = eq::rowOf(s);
	CHECK(eq::stateOf(row, row.size()) == s);

	const eq::Settings partial = eq::stateOf(row, 2);
	eq::Settings expected;
	expected.enabled = false;
	expected.offsetOctaves = -1.25f;
	CHECK(partial == expected);
	// The descriptor's defaults ARE the default Settings.
	eq::Row defaults {};
	for (size_t i = 0; i < defaults.size(); ++i) {
		defaults[i] = static_cast<float>(eq::kParamDescs[i].def);
	}
	CHECK(eq::stateOf(defaults, defaults.size()) == eq::Settings {});
}

TEST_CASE("EQ: the default device is bit-exact identity") {
	Instance dev;
	dev.set(flat());
	std::vector<float> l = noise(20000, 1u), r = noise(20000, 2u);
	const std::vector<float> inL = l, inR = r;
	dev.run(l, r, 97);
	CHECK(l == inL);
	CHECK(r == inR);
	for (int b = 0; b < eq::kBandCount; ++b) {
		CHECK(eq::bandBypassedForTest(dev.storage(), b));
	}
}

TEST_CASE("EQ: a band measures as the analog prototype at probe frequencies") {
	// Band 4 as a +9 dB, Q 2 bell at 1 kHz. At f0 the matched design meets the analog magnitude exactly;
	// an octave either side it is within the design's measured well-below-Nyquist bound (0.168 dB).
	const eq::Settings s = withBell(3, 1000.0, 9.0f, 2.0);
	for (double hz : { 500.0, 1000.0, 2000.0 }) {
		CAPTURE(hz);
		const double want = analogBellDb(9.0, 2.0, hz / 1000.0);
		CHECK(std::fabs(measuredDb(s, hz) - want) < (hz == 1000.0 ? 0.02 : 0.2));
	}
	// AND THE GLOBALS THROUGH THE DEVICE, each by value:
	// Offset +1 octave puts a 500 Hz bell at 1 kHz.
	eq::Settings off = withBell(3, 500.0, 9.0f, 2.0);
	off.offsetOctaves = 1.0f;
	CHECK(std::fabs(measuredDb(off, 1000.0) - 9.0) < 0.02);
	// Amount 50% halves the gain in dB.
	eq::Settings half = s;
	half.amount = 0.5f;
	CHECK(std::fabs(measuredDb(half, 1000.0) - 4.5) < 0.02);
	// Output +6 dB is +6 dB everywhere.
	eq::Settings loud = flat();
	loud.outputDb = 6.0f;
	CHECK(std::fabs(measuredDb(loud, 300.0) - 6.0) < 0.01);
	// Tilt +3 dB/oct around 1 kHz: +6 dB two octaves up, -6 two down, within the tilt's line error.
	eq::Settings tilt = flat();
	tilt.tiltDbPerOctave = 3.0f;
	CHECK(std::fabs(measuredDb(tilt, 1000.0)) < 0.05);
	CHECK(std::fabs(measuredDb(tilt, 4000.0) - 6.0) < 0.7);
	CHECK(std::fabs(measuredDb(tilt, 250.0) + 6.0) < 0.7);
}

TEST_CASE("EQ: the host's block size does not change a single sample") {
	eq::Settings a = withBell(2, 300.0, 7.0f, 1.5);
	a.bands[0].on = true; // the 24 dB low cut at 30 Hz
	a.tiltDbPerOctave = -1.5f;
	eq::Settings b = a;
	b.bands[2].gainDb = -9.0f; // a change mid-stream, smoothed on the grid
	b.offsetOctaves = 0.75f;

	auto render = [&](int block) {
		Instance dev;
		std::vector<float> l = noise(8192, 7u), r = noise(8192, 8u);
		dev.set(a);
		std::vector<float> l1(l.begin(), l.begin() + 4096), r1(r.begin(), r.begin() + 4096);
		std::vector<float> l2(l.begin() + 4096, l.end()), r2(r.begin() + 4096, r.end());
		dev.run(l1, r1, block);
		dev.set(b);
		dev.run(l2, r2, block);
		l1.insert(l1.end(), l2.begin(), l2.end());
		r1.insert(r1.end(), r2.begin(), r2.end());
		return std::make_pair(l1, r1);
	};
	const auto ref = render(512);
	for (int block : { 1, 7, 32, 33, 4096 }) {
		CAPTURE(block);
		const auto got = render(block);
		CHECK(got.first == ref.first);
		CHECK(got.second == ref.second);
	}
}

TEST_CASE("EQ: a moved gain glides on the grid and lands exactly, then the band bypasses") {
	Instance dev;
	dev.set(withBell(4, 2500.0, 12.0f, 1.0));
	std::vector<float> l(4800, 0.1f), r(4800, 0.1f);
	dev.run(l, r, 256);
	CHECK(eq::effectiveForTest(dev.storage(), 4).gainDb == 12.0);

	dev.set(flat()); // the gain goes to 0 dB
	std::vector<float> one(32, 0.1f), two(32, 0.1f);
	dev.process(one.data(), two.data(), 32);
	const double mid = eq::effectiveForTest(dev.storage(), 4).gainDb;
	CHECK(mid > 0.0);
	CHECK(mid < 12.0);
	CHECK_FALSE(eq::bandBypassedForTest(dev.storage(), 4));

	std::vector<float> l2(9600, 0.1f), r2(9600, 0.1f);
	dev.run(l2, r2, 256);
	CHECK(eq::effectiveForTest(dev.storage(), 4).gainDb == 0.0);
	CHECK(eq::bandBypassedForTest(dev.storage(), 4));
}

TEST_CASE("EQ: Amount -100% after +100% gives back the input through two devices in series") {
	eq::Settings s = flat();
	s.bands[1].gainDb = 8.0f;   // low shelf
	s.bands[2].gainDb = -11.0f; // bells
	s.bands[4].gainDb = 6.0f;
	s.bands[4].qLog2 = 2.0f;
	s.bands[6].gainDb = -4.0f; // high shelf
	s.tiltDbPerOctave = 2.0f;
	eq::Settings inverse = s;
	inverse.amount = -1.0f;

	Instance a, b;
	a.set(s);
	b.set(inverse);
	std::vector<float> l = noise(48000, 3u), r = noise(48000, 4u);
	const std::vector<float> in = l;
	a.run(l, r, 128);
	double moved = 0.0;
	for (size_t i = 24000; i < l.size(); ++i) {
		moved = std::max(moved, static_cast<double>(std::fabs(l[i] - in[i])));
	}
	b.run(l, r, 128);
	double worst = 0.0;
	for (size_t i = 24000; i < l.size(); ++i) {
		worst = std::max(worst, static_cast<double>(std::fabs(l[i] - in[i])));
	}
	CHECK(moved > 0.05);  // the first device really does change the signal
	CHECK(worst < 1e-4); // and the second undoes it, to float rounding through ~17 sections twice
}

TEST_CASE("EQ: a bell pushed over Nyquist by Offset is bypassed and the output is the input") {
	eq::Settings s = withBell(5, 6000.0, 12.0f, 1.0);
	s.offsetOctaves = 3.0f; // 48 kHz, far past Nyquist
	Instance dev;
	dev.set(s);
	std::vector<float> l = noise(4096, 5u), r = noise(4096, 6u);
	const std::vector<float> in = l;
	dev.run(l, r, 64);
	CHECK(eq::effectiveForTest(dev.storage(), 5).gainDb == 0.0);
	CHECK(eq::bandBypassedForTest(dev.storage(), 5));
	CHECK(l == in);
}

TEST_CASE("EQ: switched off it is exactly the input once the fade is over") {
	eq::Settings s = withBell(3, 1000.0, 12.0f, 1.0);
	s.enabled = false;
	Instance dev;
	dev.set(s);
	std::vector<float> l = noise(4096, 9u), r = noise(4096, 10u);
	const std::vector<float> in = l;
	dev.run(l, r, 128);
	CHECK(l == in);
}

TEST_CASE("EQ: a ringing low bell reaches exact silence and nothing audible is cut short") {
	// THE WORST RING AT AMOUNT 100%: +24 dB, Q 18 at 20 Hz. Its pole Q is A*Q = 72, so tau = 1.15 s and a
	// 180 dB fall takes ~24 s (derived, doc §2.7). At Amount 200% it is +48 dB, pole Q 285 and ~94 s —
	// the filter's own physics, which the rest cannot and must not shorten above -180 dBFS.
	const eq::Settings s = withBell(3, 20.0, 24.0f, 18.0);
	Instance dev;
	dev.set(s);
	const int block = 512;
	std::vector<float> l(block), r(block);
	int t = 0;
	for (int k = 0; k < 94; ++k) { // a second of 20 Hz
		for (int i = 0; i < block; ++i, ++t) {
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] =
			    static_cast<float>(0.25 * std::sin(2.0 * kPi * 20.0 * t / kRate));
		}
		dev.process(l.data(), r.data(), block);
	}
	float peak = 0.0f;
	int lastNonZero = -1;
	float lastLoud = 0.0f; // the largest sample in the last block that was not exact zero
	const int blocks = 60 * static_cast<int>(kRate) / block; // up to a minute of silence
	for (int k = 0; k < blocks; ++k) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		dev.process(l.data(), r.data(), block);
		float m = 0.0f;
		for (int i = 0; i < block; ++i) {
			m = std::max({ m, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]) });
		}
		peak = std::max(peak, m);
		if (m != 0.0f) {
			lastNonZero = k;
			lastLoud = m;
		}
	}
	MESSAGE("last non-zero block " << lastNonZero << " (" << lastNonZero * block / kRate << " s)");
	CHECK(peak > 1.0f);                // it really did ring, loudly, into the silence
	CHECK(lastNonZero < blocks - 100); // and it stopped, exactly
	CHECK(lastNonZero * block / kRate > 15.0); // not early: the ring is ~24 s to -180 dB (derived)
	CHECK(lastLoud < 1.0e-8f);         // the last thing it said was under -160 dBFS
}

TEST_CASE("EQ: a band changing type crossfades rather than stepping") {
	// A LOW SHELF +12 dB AT 1 kHz under a 100 Hz sine lifts it ~4x; switched to a HIGH SHELF it passes it
	// at ~1x. Without the fade the new filter starts empty and the output steps by ~3x the input at once.
	eq::Settings s = withBell(2, 1000.0, 12.0f, 0.71);
	s.bands[2].type = eq::BandType::LowShelf;
	Instance dev;
	dev.set(s);
	const int n = 48000;
	std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 100.0 * i / kRate));
	}
	std::vector<float> a(l.begin(), l.begin() + n / 2), ar(r.begin(), r.begin() + n / 2);
	std::vector<float> b(l.begin() + n / 2, l.end()), br(r.begin() + n / 2, r.end());
	dev.run(a, ar, 256);
	float natural = 0.0f;
	for (size_t i = a.size() - 2000; i < a.size(); ++i) {
		natural = std::max(natural, std::fabs(a[i] - a[i - 1]));
	}
	s.bands[2].type = eq::BandType::HighShelf;
	dev.set(s);
	dev.run(b, br, 256);
	float across = std::fabs(b[0] - a.back());
	for (size_t i = 1; i < 2000; ++i) {
		across = std::max(across, std::fabs(b[i] - b[i - 1]));
	}
	MESSAGE("largest per-sample step: before " << natural << ", across the change " << across);
	CHECK(natural > 0.005f);           // the probe really is moving
	CHECK(across < natural * 1.5f);    // and the change adds no step of its own
}

TEST_CASE("EQ: the output meter reads the post-trim peak") {
	eq::Settings s = flat();
	s.outputDb = 6.0f;
	Instance dev;
	dev.listen();
	dev.set(s);
	std::vector<float> l(9600), r(9600);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = r[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate));
	}
	dev.run(l, r, 256);
	REQUIRE(dev.readingCount == static_cast<size_t>(eq::Reading::kCount));
	// +6 dB is x1.9953: a 0.25 sine peaks at 0.4988.
	CHECK(std::fabs(dev.lastReading - 0.25f * std::pow(10.0f, 6.0f / 20.0f)) < 0.005f);
}

TEST_CASE("EQ plot: the grid thickens with the pixels it has") {
	namespace ep = feathers::ui::eq_plot;
	const auto count = [](const ep::Geometry& g, ep::Tier t) {
		int n = 0;
		for (const ep::FreqLine& l : ep::frequencyLines(g)) {
			n += l.tier == t ? 1 : 0;
		}
		return n;
	};
	// THE CARD (about 200 px wide, 67 px a decade): decades only — 100, 1k, 10k.
	const ep::Geometry card { 0.0f, 0.0f, 200.0f, 116.0f, 24.0f };
	CHECK(count(card, ep::Tier::Decade) == 3);
	CHECK(count(card, ep::Tier::TwoFive) == 0);
	CHECK(count(card, ep::Tier::Digit) == 0);
	CHECK(ep::dbStep(card) == 12.0f); // 2.4 px a dB: only 12 dB keeps 24 px
	CHECK_FALSE(ep::drawsNoteLines(card));
	// 400 px (133 a decade): 2 and 5 join — 20, 50, 200, 500, 2k, 5k, 20k.
	const ep::Geometry mid { 0.0f, 0.0f, 400.0f, 300.0f, 24.0f };
	CHECK(count(mid, ep::Tier::TwoFive) == 7);
	CHECK(count(mid, ep::Tier::Digit) == 0);
	// 1000 px (333 a decade): every digit, and octave C lines (100 px an octave).
	const ep::Geometry big { 0.0f, 0.0f, 1000.0f, 500.0f, 24.0f };
	CHECK(count(big, ep::Tier::Digit) > 0);
	CHECK(ep::drawsNoteLines(big));
	CHECK(ep::dbStep(big) == 3.0f); // 10.4 px a dB: 3 dB is the finest at 24 px
	// AND THE ZOOM: ±6 dB over the same 500 px is 41.7 px a dB, so every dB gets a line.
	const ep::Geometry zoomed { 0.0f, 0.0f, 1000.0f, 500.0f, 6.0f };
	CHECK(ep::dbStep(zoomed) == 1.0f);
}

TEST_CASE("EQ plot: labels are placed only where they fit, decades first") {
	namespace ep = feathers::ui::eq_plot;
	const ep::Geometry card { 0.0f, 0.0f, 200.0f, 116.0f, 24.0f };
	const auto labels = ep::frequencyLabels(card, 5.0f);
	REQUIRE(labels.size() == 3);
	CHECK(labels[0].text == "100");
	CHECK(labels[1].text == "1k");
	CHECK(labels[2].text == "10k");
	// No two labels overlap, at any width.
	for (const float w : { 200.0f, 400.0f, 700.0f, 1500.0f }) {
		const auto ls = ep::frequencyLabels({ 0.0f, 0.0f, w, 300.0f, 24.0f }, 5.0f);
		for (size_t i = 0; i < ls.size(); ++i) {
			for (size_t j = i + 1; j < ls.size(); ++j) {
				CAPTURE(w);
				CHECK((ls[i].right <= ls[j].left || ls[j].right <= ls[i].left));
			}
		}
	}
	// THE GEOMETRY IS ITS OWN INVERSE, which is what a drag relies on.
	const ep::Geometry g { 10.0f, 5.0f, 640.0f, 300.0f, 12.0f };
	CHECK(g.hzForX(g.xForHz(1234.5)) == doctest::Approx(1234.5).epsilon(1e-6));
	CHECK(g.dbForY(g.yForDb(-7.25)) == doctest::Approx(-7.25).epsilon(1e-6));
}

namespace {

/// A second of a sine at `hz` through `dev` in 256-sample blocks.
void runSine(Instance& dev, double hz, double amplitude, int samples = 48000) {
	std::vector<float> l(static_cast<size_t>(samples)), r(static_cast<size_t>(samples));
	for (int i = 0; i < samples; ++i) {
		l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] =
		    static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / kRate));
	}
	dev.run(l, r, 256);
}

size_t binOf(double hz) {
	size_t best = 0;
	for (size_t k = 0; k < eq::kAnalyserBins; ++k) {
		if (std::fabs(std::log2(eq::analyserBinHz(k) / hz)) < std::fabs(std::log2(eq::analyserBinHz(best) / hz))) {
			best = k;
		}
	}
	return best;
}

} // namespace

TEST_CASE("EQ analyser: a sine lands in its bin, and POST minus PRE is the EQ's gain there") {
	// A +9 dB, Q 2 bell at 1 kHz, probed with a 1 kHz sine: the analyser must put the energy in the log
	// bin holding 1 kHz, and the difference between its two taps there must be the bell's +9 dB — a value
	// the analyser's own normalisation cannot fake, since it is the same on both taps.
	Instance dev;
	dev.listen();
	dev.set(withBell(3, 1000.0, 9.0f, 2.0));
	runSine(dev, 1000.0, 0.25);
	const std::vector<float>& v = dev.allReadings;
	REQUIRE(v.size() == eq::kReadingCount);
	CHECK(v[static_cast<size_t>(eq::Reading::FrameA)] == v[static_cast<size_t>(eq::Reading::FrameB)]);
	CHECK(v[static_cast<size_t>(eq::Reading::FrameA)] > 0.0f);
	const size_t k = binOf(1000.0);
	size_t loudest = 0;
	for (size_t i = 0; i < eq::kAnalyserBins; ++i) {
		if (v[static_cast<size_t>(eq::Reading::PostFirst) + i] > v[static_cast<size_t>(eq::Reading::PostFirst) + loudest]) {
			loudest = i;
		}
	}
	CHECK(loudest == k);
	const float pre = v[static_cast<size_t>(eq::Reading::PreFirst) + k];
	const float post = v[static_cast<size_t>(eq::Reading::PostFirst) + k];
	MESSAGE("1 kHz bin " << k << ": pre " << pre << " dB, post " << post << " dB");
	CHECK(std::fabs((post - pre) - 9.0f) < 0.3f);
	// FAR FROM THE BELL, the taps agree: 100 Hz is 3.3 octaves away and the bell does nothing there.
	Instance far;
	far.listen();
	far.set(withBell(3, 1000.0, 9.0f, 2.0));
	runSine(far, 100.0, 0.25);
	const size_t k100 = binOf(100.0);
	CHECK(std::fabs(far.allReadings[static_cast<size_t>(eq::Reading::PostFirst) + k100]
	                - far.allReadings[static_cast<size_t>(eq::Reading::PreFirst) + k100])
	      < 0.2f);
}

TEST_CASE("EQ analyser: a tap switched off costs no transforms") {
	// 8192 at 48 kHz, a hop of 2048: a second holds 23 hops. Four transforms a hop with both taps on, two
	// with one, none with neither.
	const auto transforms = [](bool pre, bool post) {
		Instance dev;
		std::array<float, eq::kExtraState> extra {};
		extra[static_cast<size_t>(eq::Extra::Pre)] = pre ? 0.0f : 1.0f;
		extra[static_cast<size_t>(eq::Extra::Post)] = post ? 0.0f : 1.0f;
		dev.setWithExtra(flat(), extra);
		runSine(dev, 440.0, 0.25);
		return eq::analyserTransformsForTest(dev.storage());
	};
	const int both = transforms(true, true);
	CHECK(both >= 4 * 22);
	CHECK(transforms(true, false) == both / 2);
	CHECK(transforms(false, true) == both / 2);
	CHECK(transforms(false, false) == 0);
}

// ---------------------------------------------------------------------------------------------------
// MID/SIDE (step 6, doc §7)
// ---------------------------------------------------------------------------------------------------

namespace {

/// Settings plus the extra-state block with each band's channel, as the host stores them.
std::array<float, eq::kExtraState> channels(std::initializer_list<std::pair<int, eq::Channel>> bands,
                                            eq::Channel view = eq::Channel::Stereo) {
	std::array<float, eq::kExtraState> extra {};
	for (const auto& [band, channel] : bands) {
		extra[static_cast<size_t>(eq::channelField(band))] = static_cast<float>(channel);
	}
	extra[static_cast<size_t>(eq::Extra::AnalyserChannel)] = static_cast<float>(view);
	return extra;
}

struct Stereo {
	std::vector<float> l, r;
};

/// `in` through a fresh device with `s` and `extra`, in blocks of 256.
Stereo through(const eq::Settings& s, const std::array<float, eq::kExtraState>& extra, Stereo in,
               bool* midSide = nullptr) {
	Instance dev;
	dev.setWithExtra(s, extra);
	dev.run(in.l, in.r, 256);
	if (midSide != nullptr) {
		*midSide = eq::midSideActiveForTest(dev.storage());
	}
	return in;
}

bool bitEqual(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

} // namespace

TEST_CASE("EQ M/S: a Mid band on a mono signal is exactly the same band on Stereo") {
	// Mono: L = R = x, so mid = x and side = 0 — the Mid band sees what the Stereo band sees on each side,
	// and (x + x) / 2, y + 0 and y - 0 are all exact in floats.
	const eq::Settings s = withBell(2, 700.0, 7.5f, 1.4);
	const std::vector<float> x = noise(24000, 11);
	bool ms = false;
	const Stereo stereo = through(s, channels({}), { x, x });
	const Stereo mid = through(s, channels({ { 2, eq::Channel::Mid } }), { x, x }, &ms);
	CHECK(ms); // the conversion really ran
	CHECK(bitEqual(mid.l, stereo.l));
	CHECK(bitEqual(mid.r, stereo.r));
	CHECK_FALSE(bitEqual(stereo.l, x)); // and the band really does something
}

TEST_CASE("EQ M/S: a Side band on a mono signal is exactly identity") {
	const eq::Settings s = withBell(4, 3000.0, -9.0f, 0.9);
	const std::vector<float> x = noise(24000, 12);
	bool ms = false;
	const Stereo side = through(s, channels({ { 4, eq::Channel::Side } }), { x, x }, &ms);
	CHECK(ms);
	CHECK(bitEqual(side.l, x));
	CHECK(bitEqual(side.r, x));
	// NOT VACUOUS: the same band on Stereo moves this signal.
	CHECK_FALSE(bitEqual(through(s, channels({}), { x, x }).l, x));
}

TEST_CASE("EQ M/S: with no Mid or Side band acting, left and right never take the round trip") {
	// (L + R)/2 + (L - R)/2 is not L in floats, so a patch with no M/S band must skip the conversion
	// entirely. A Mid band at 0 dB (bypassed) and a Side band switched off are not acting.
	eq::Settings s = withBell(1, 250.0, 4.0f, 1.0);
	s.bands[5].type = eq::BandType::Bell;
	s.bands[5].on = true;
	s.bands[5].gainDb = 0.0f;
	s.bands[6].on = false;
	s.bands[6].type = eq::BandType::Bell;
	s.bands[6].gainDb = 6.0f;
	// THE RIGHT ~60 dB BELOW THE LEFT, so (L + R)/2 + (L - R)/2 rounds away its low bits: the round trip shows.
	std::vector<float> quiet = noise(24000, 22);
	for (float& v : quiet) {
		v *= 1.37e-3f;
	}
	const Stereo in { noise(24000, 21), quiet };
	bool ms = true;
	const Stereo plain = through(s, channels({}), in);
	const Stereo idle = through(s, channels({ { 5, eq::Channel::Mid }, { 6, eq::Channel::Side } }), in, &ms);
	CHECK_FALSE(ms);
	CHECK(bitEqual(idle.l, plain.l));
	CHECK(bitEqual(idle.r, plain.r));
	// And the moment one acts, the conversion runs.
	s.bands[5].gainDb = 3.0f;
	bool acting = false;
	(void)through(s, channels({ { 5, eq::Channel::Mid } }), in, &acting);
	CHECK(acting);
}

TEST_CASE("EQ M/S: on a pure side signal a Side band is the Stereo band, and a Mid band does nothing") {
	// L = x, R = -x: mid = 0, side = x exactly. A linear band is odd, so the Stereo band gives y and -y.
	const eq::Settings s = withBell(3, 1500.0, 6.0f, 2.0);
	const std::vector<float> x = noise(24000, 31);
	std::vector<float> minus(x.size());
	std::transform(x.begin(), x.end(), minus.begin(), [](float v) { return -v; });
	const Stereo stereo = through(s, channels({}), { x, minus });
	const Stereo side = through(s, channels({ { 3, eq::Channel::Side } }), { x, minus });
	CHECK(bitEqual(side.l, stereo.l));
	CHECK(bitEqual(side.r, stereo.r));
	const Stereo mid = through(s, channels({ { 3, eq::Channel::Mid } }), { x, minus });
	CHECK(bitEqual(mid.l, x));
	CHECK(bitEqual(mid.r, minus));
	CHECK_FALSE(bitEqual(stereo.l, x));
}

TEST_CASE("EQ M/S: a band changing channel fades out and back in rather than stepping") {
	// +12 dB at 100 Hz under a 100 Hz sine on the LEFT only: on Stereo the left is lifted ~4x; moved to Mid
	// the left becomes 4 x/2 + x/2 = 2.5x. Moving at once would restart the band from empty state — a step.
	const eq::Settings s = withBell(2, 100.0, 12.0f, 0.71);
	Instance dev;
	dev.setWithExtra(s, channels({}));
	const int n = 48000;
	std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n), 0.0f);
	for (int i = 0; i < n; ++i) {
		l[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 100.0 * i / kRate));
	}
	std::vector<float> a(l.begin(), l.begin() + n / 2), ar(r.begin(), r.begin() + n / 2);
	std::vector<float> b(l.begin() + n / 2, l.end()), br(r.begin() + n / 2, r.end());
	dev.run(a, ar, 256);
	float natural = 0.0f;
	for (size_t i = a.size() - 2000; i < a.size(); ++i) {
		natural = std::max(natural, std::fabs(a[i] - a[i - 1]));
	}
	dev.setWithExtra(s, channels({ { 2, eq::Channel::Mid } }));
	// Still on Stereo 100 samples in (fading out); on Mid after 20 ms.
	std::vector<float> first(b.begin(), b.begin() + 100), firstR(br.begin(), br.begin() + 100);
	dev.run(first, firstR, 100);
	CHECK(eq::bandChannelForTest(dev.storage(), 2) == eq::Channel::Stereo);
	std::vector<float> rest(b.begin() + 100, b.end()), restR(br.begin() + 100, br.end());
	dev.run(rest, restR, 256);
	CHECK(eq::bandChannelForTest(dev.storage(), 2) == eq::Channel::Mid);
	std::vector<float> after = first;
	after.insert(after.end(), rest.begin(), rest.end());
	float across = std::fabs(after[0] - a.back());
	for (size_t i = 1; i < 2000; ++i) {
		across = std::max(across, std::fabs(after[i] - after[i - 1]));
	}
	MESSAGE("largest per-sample step: before " << natural << ", across the move " << across);
	CHECK(natural > 0.002f);
	CHECK(across < natural * 1.5f);
	// AND IT LANDED: the left settles at 2.5x the input's amplitude, not 4x.
	float peak = 0.0f;
	for (size_t i = after.size() - 4800; i < after.size(); ++i) {
		peak = std::max(peak, std::fabs(after[i]));
	}
	CHECK(peak / 0.1f == doctest::Approx(0.5 * std::pow(10.0, 12.0 / 20.0) + 0.5).epsilon(0.02));
}

TEST_CASE("EQ analyser: the view — Mid of a mono signal is Stereo and its Side is empty; antiphase the reverse") {
	// From the SAME two transforms through the cross spectrum, which only works if L and R are transformed
	// over the same samples: a mono signal's side is then exactly zero power, not merely small.
	const auto frame = [](eq::Channel view, bool antiphase) {
		Instance dev;
		dev.listen();
		dev.setWithExtra(flat(), channels({}, view));
		const int n = 48000;
		std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * i / kRate));
			r[static_cast<size_t>(i)] = antiphase ? -l[static_cast<size_t>(i)] : l[static_cast<size_t>(i)];
		}
		dev.run(l, r, 256);
		REQUIRE(dev.allReadings.size() == eq::kReadingCount);
		CHECK(dev.allReadings[static_cast<size_t>(eq::Reading::View)] == static_cast<float>(view));
		return dev.allReadings;
	};
	const size_t k = binOf(1000.0);
	const auto post = [k](const std::vector<float>& v) { return v[static_cast<size_t>(eq::Reading::PostFirst) + k]; };
	const auto pre = [k](const std::vector<float>& v) { return v[static_cast<size_t>(eq::Reading::PreFirst) + k]; };
	const std::vector<float> stereo = frame(eq::Channel::Stereo, false);
	const std::vector<float> mid = frame(eq::Channel::Mid, false);
	const std::vector<float> side = frame(eq::Channel::Side, false);
	MESSAGE("mono 1 kHz: stereo " << post(stereo) << ", mid " << post(mid) << ", side " << post(side) << " dB");
	CHECK(post(stereo) > -30.0f); // mean density over a log bin wider than the sine's (A15)
	CHECK(std::fabs(post(mid) - post(stereo)) < 1e-3f);
	CHECK(std::fabs(pre(mid) - pre(stereo)) < 1e-3f);
	CHECK(post(side) == eq::kAnalyserFloorDb);
	CHECK(pre(side) == eq::kAnalyserFloorDb);
	const std::vector<float> antiMid = frame(eq::Channel::Mid, true);
	const std::vector<float> antiSide = frame(eq::Channel::Side, true);
	CHECK(post(antiMid) == eq::kAnalyserFloorDb);
	CHECK(std::fabs(post(antiSide) - post(stereo)) < 1e-3f);
}

// ---------------------------------------------------------------------------------------------------
// LINEAR PHASE (and the latency contract it reports under)
// ---------------------------------------------------------------------------------------------------

namespace {

eq::Settings linearOf(eq::Settings s) {
	s.linearPhase = true;
	return s;
}

/// `in` (mono, both channels) through a fresh device at `rate`, in blocks of `block`; the left channel out.
std::vector<float> linearThrough(const eq::Settings& s, std::vector<float> in, double rate = kRate, int block = 256) {
	Instance dev(rate);
	dev.set(s);
	std::vector<float> r = in;
	dev.run(in, r, block);
	return in;
}

size_t peakIndex(const std::vector<float>& v) {
	size_t best = 0;
	for (size_t i = 1; i < v.size(); ++i) {
		if (std::fabs(v[i]) > std::fabs(v[best])) {
			best = i;
		}
	}
	return best;
}

std::vector<float> impulse(size_t n, size_t at = 0) {
	std::vector<float> v(n, 0.0f);
	v[at] = 1.0f;
	return v;
}

} // namespace

TEST_CASE("EQ linear: the latency is a pure function of the stored Phase and the rate, and Phase is not modulatable") {
	const auto latencyOf = [](const eq::Settings& s, double rate, size_t present = eq::kParamCount) {
		const eq::Row row = eq::rowOf(s);
		return eq::kAudioOps.latencySamples(nullptr, row.data(), present, rate);
	};
	REQUIRE(eq::kAudioOps.latencySamples != nullptr);
	CHECK(latencyOf(flat(), 48000.0) == 0u);
	CHECK(latencyOf(linearOf(flat()), 48000.0) == 9216u);  // 16384 / 2 + 1024
	CHECK(latencyOf(linearOf(flat()), 44100.0) == 9216u);
	CHECK(latencyOf(linearOf(flat()), 96000.0) == 18432u); // 32768 / 2 + 2048
	CHECK(latencyOf(linearOf(flat()), 192000.0) == 36864u);
	// UNDER JM's ONE SECOND at every rate (L4).
	for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 }) {
		CAPTURE(rate);
		CHECK(static_cast<double>(latencyOf(linearOf(flat()), rate)) < rate);
	}
	// A row that stops short of Phase is Minimum.
	CHECK(latencyOf(linearOf(flat()), 48000.0, eq::paramIndex(eq::Param::Phase)) == 0u);
	// NOT MODULATABLE (L1): the host sums the STORED row.
	CHECK_FALSE(eq::kParamDescs[eq::paramIndex(eq::Param::Phase)].modulatable);
}

TEST_CASE("EQ linear: an impulse comes out exactly the declared latency late, at 48 and 96 kHz") {
	for (const double rate : { 48000.0, 96000.0 }) {
		CAPTURE(rate);
		const size_t latency = eq::linearLatency(rate);
		// FLAT: the kernel is a single tap, so the impulse comes out whole, and exactly that late.
		const std::vector<float> flatOut = linearThrough(linearOf(flat()), impulse(latency * 3), rate);
		CHECK(peakIndex(flatOut) == latency);
		CHECK(flatOut[latency] == doctest::Approx(1.0).epsilon(1e-5));
		// A +9 dB BELL: its kernel's centre is its peak, at the same place.
		const std::vector<float> bellOut = linearThrough(linearOf(withBell(3, 300.0, 9.0f, 2.0)), impulse(latency * 3), rate);
		CHECK(peakIndex(bellOut) == latency);
		// The impulse arrives nowhere near before its pre-ringing: nothing at all in the first block.
		for (size_t i = 0; i < eq::linearBlockLength(rate); ++i) {
			CHECK(bellOut[i] == 0.0f);
		}
	}
}

TEST_CASE("EQ linear: the impulse response is symmetric about its peak") {
	eq::Settings s = withBell(2, 300.0, 9.0f, 2.0);
	s.bands[5].gainDb = -6.0f;
	s.tiltDbPerOctave = 1.5f;
	const size_t latency = eq::linearLatency(kRate);
	const std::vector<float> out = linearThrough(linearOf(s), impulse(latency * 3));
	const size_t half = eq::linearFirLength(kRate) / 2;
	float worst = 0.0f;
	for (size_t k = 1; k < half; ++k) {
		worst = std::max(worst, std::fabs(out[latency - k] - out[latency + k]));
	}
	MESSAGE("peak " << out[latency] << ", worst asymmetry " << worst);
	CHECK(worst < 1e-6f * std::fabs(out[latency]));
	CHECK(std::fabs(out[latency + 1]) > 1e-4f); // not vacuous: the kernel has a body
}

TEST_CASE("EQ linear: the magnitude is minimum phase's, to within the doc's measured bounds") {
	// R5: the FIR is built from the SAME digital magnitude, so the switch changes phase only. Measured both ways
	// by the same sine probe; the bounds are §3.3's N = 16384 column, with a little room for the probe.
	struct Probe {
		eq::Settings s;
		double hz;
		double bound;
	};
	eq::Settings shelf = flat();
	shelf.bands[1].type = eq::BandType::LowShelf;
	shelf.bands[1].freqLog2 = static_cast<float>(std::log2(80.0));
	shelf.bands[1].gainDb = 12.0f;
	const Probe probes[] = {
		{ withBell(3, 1000.0, 12.0f, 4.0), 1000.0, 0.05 },
		{ withBell(3, 100.0, 12.0f, 4.0), 100.0, 0.6 },
		{ withBell(3, 40.0, 12.0f, 1.0), 40.0, 0.3 },
		{ shelf, 60.0, 0.05 },
		{ withBell(3, 1000.0, 12.0f, 4.0), 707.0, 0.05 },
	};
	for (const Probe& p : probes) {
		CAPTURE(p.hz);
		const double minimum = measuredDb(p.s, p.hz);
		const double linear = measuredDb(linearOf(p.s), p.hz);
		MESSAGE(p.hz << " Hz: minimum " << minimum << " dB, linear " << linear << " dB");
		CHECK(std::fabs(linear - minimum) < p.bound);
		CHECK(std::fabs(minimum) > 1.0); // not vacuous: the probe sits where the EQ acts
	}
}

TEST_CASE("EQ linear: silence is exact within N + 2B of the input stopping") {
	const size_t span = eq::linearFirLength(kRate) + 2 * eq::linearBlockLength(kRate);
	std::vector<float> in = noise(20000, 41);
	in.resize(20000 + span + 5000, 0.0f);
	const std::vector<float> out = linearThrough(linearOf(withBell(4, 2000.0, 6.0f, 1.0)), in);
	size_t lastSound = 0;
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i] != 0.0f) {
			lastSound = i;
		}
	}
	MESSAGE("input stops at 20000; the last sample that sounds is " << lastSound);
	CHECK(lastSound > 20000);
	CHECK(lastSound < 20000 + span);
}

TEST_CASE("EQ linear: switched off it is the input delayed by exactly the latency") {
	eq::Settings s = linearOf(withBell(3, 800.0, 9.0f, 1.0));
	s.enabled = false;
	const size_t latency = eq::linearLatency(kRate);
	const std::vector<float> in = noise(40000, 42);
	Instance dev;
	dev.set(s);
	// Settle the enable ramp from its start, then compare from a fresh run of the same device.
	std::vector<float> l = in, r = in;
	dev.run(l, r, 256);
	size_t mismatches = 0;
	for (size_t i = 20000; i < in.size(); ++i) {
		mismatches += l[i] == in[i - latency] ? 0 : 1;
	}
	CHECK(mismatches == 0);
}

TEST_CASE("EQ linear: a Mid band on a mono signal is bit-identical to the same band on Stereo") {
	const eq::Settings s = linearOf(withBell(2, 700.0, 7.5f, 1.4));
	const std::vector<float> x = noise(30000, 43);
	const Stereo stereo = through(s, channels({}), { x, x });
	const Stereo mid = through(s, channels({ { 2, eq::Channel::Mid } }), { x, x });
	CHECK(bitEqual(mid.l, stereo.l));
	CHECK(bitEqual(mid.r, stereo.r));
	// And a Side band there is the input, delayed.
	const Stereo side = through(s, channels({ { 2, eq::Channel::Side } }), { x, x });
	const size_t latency = eq::linearLatency(kRate);
	float worst = 0.0f;
	for (size_t i = latency; i < x.size(); ++i) {
		worst = std::max(worst, std::fabs(side.l[i] - x[i - latency]));
	}
	CHECK(worst < 1e-5f);
}

TEST_CASE("EQ linear: the host's block size does not change a single sample") {
	const eq::Settings s = linearOf(withBell(4, 1200.0, -8.0f, 3.0));
	const std::vector<float> in = noise(30000, 44);
	const std::vector<float> a = linearThrough(s, in, kRate, 64);
	const std::vector<float> b = linearThrough(s, in, kRate, 997);
	CHECK(bitEqual(a, b));
}

TEST_CASE("EQ linear: a gain that moves rebuilds the kernel and crossfades to it, with no step, and lands") {
	// A 100 Hz sine under a 100 Hz bell taken from 0 to +12 dB while it plays. Each rebuild swaps kernels
	// across one block; without the crossfade the swap is a jump of several times the sine's own motion.
	eq::Settings s = linearOf(withBell(3, 100.0, 0.0f, 1.0));
	Instance dev;
	dev.set(s);
	const int n = 96000;
	std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 100.0 * i / kRate));
	}
	const std::vector<float> in = l;
	std::vector<float> a(l.begin(), l.begin() + n / 2), ar(a);
	std::vector<float> b(l.begin() + n / 2, l.end()), br(b);
	dev.run(a, ar, 256);
	const int swapsBefore = eq::linearSwapsForTest(dev.storage());
	s.bands[3].gainDb = 12.0f;
	dev.set(s);
	dev.run(b, br, 256);
	CHECK(eq::linearSwapsForTest(dev.storage()) > swapsBefore);
	// THE LARGEST PER-SAMPLE STEP across the change, against the loudest the sine itself makes at +12 dB.
	const float natural = static_cast<float>(0.1 * std::pow(10.0, 12.0 / 20.0) * 2.0 * kPi * 100.0 / kRate);
	float across = std::fabs(b[0] - a.back());
	for (size_t i = 1; i < 20000; ++i) {
		across = std::max(across, std::fabs(b[i] - b[i - 1]));
	}
	MESSAGE("largest step " << across << " against the sine's own " << natural);
	CHECK(across < natural * 1.2f);
	// AND IT LANDED at +12 dB (the delay changes nothing about a steady sine's level).
	double eo = 0.0, ei = 0.0;
	for (size_t i = b.size() - 24000; i < b.size(); ++i) {
		eo += static_cast<double>(b[i]) * b[i];
		ei += static_cast<double>(in[i + static_cast<size_t>(n / 2)]) * in[i + static_cast<size_t>(n / 2)];
	}
	CHECK(10.0 * std::log10(eo / ei) == doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE("EQ linear: nothing allocates on the audio thread while bands, Offset and Tilt move, or across a Phase switch") {
	Instance dev;
	eq::Settings s = linearOf(withBell(3, 500.0, 6.0f, 1.0));
	std::vector<float> l = noise(256, 51), r = noise(256, 52);
	dev.set(s);
	dev.process(l.data(), r.data(), 256);
	std::vector<float> row(eq::kParamCount + eq::kExtraState);
	allocs.allocations = 0;
	allocs.armed = true;
	std::array<float, eq::kExtraState> extra {};
	for (int block = 0; block < 400; ++block) {
		s.bands[3].gainDb = static_cast<float>(6.0 * std::sin(block * 0.05));
		s.offsetOctaves = static_cast<float>(0.5 * std::sin(block * 0.03));
		s.tiltDbPerOctave = static_cast<float>(block % 50) * 0.02f;
		extra[static_cast<size_t>(eq::channelField(5))] = static_cast<float>((block / 40) % 3);
		s.linearPhase = block < 300 || block >= 350; // out to Minimum and back
		const eq::Row params = eq::rowOf(s);
		std::copy(params.begin(), params.end(), row.begin());
		std::copy(extra.begin(), extra.end(), row.begin() + static_cast<long>(eq::kParamCount));
		dev.setRow(row);
		dev.process(l.data(), r.data(), 256);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK(eq::linearSwapsForTest(dev.storage()) > 3); // not vacuous: kernels really were rebuilt and swapped
}

//------------------------------------------------------------------------------------------------//
// KEY TRACK (backlog MQ): every band's Hz times f0 / C3, from the note the lane hands the device.     //
//------------------------------------------------------------------------------------------------//

namespace {

/// Every band on, as a bell or a cut, all at or under 5 kHz so an octave up stays clear of the design limit.
eq::Settings trackedLayout(bool keyTrack) {
	eq::Settings s = flat();
	s.keyTrack = keyTrack;
	for (int b = 0; b < eq::kBandCount; ++b) {
		auto& p = s.bands[static_cast<size_t>(b)];
		p.on = true;
		p.type = b == 0 ? eq::BandType::LowCut : eq::BandType::Bell;
		p.freqLog2 = static_cast<float>(std::log2(60.0 * std::exp2(0.7 * b)));
		p.gainDb = b == 0 ? 0.0f : 3.0f;
	}
	return s;
}

void runFor(Instance& dev, int samples) {
	std::vector<float> l = noise(samples, 7), r = noise(samples, 8);
	dev.run(l, r, 256);
}

} // namespace

TEST_CASE("EQ Key Track: an octave up doubles every band's Hz with tracking on, and moves nothing with it off") {
	for (const bool on : { true, false }) {
		Instance dev;
		dev.set(trackedLayout(on));
		dev.note(72.0f); // C4: an octave above C3
		runFor(dev, 9600);
		for (int b = 0; b < eq::kBandCount; ++b) {
			const double stored = std::exp2(static_cast<double>(trackedLayout(on).bands[static_cast<size_t>(b)].freqLog2));
			CAPTURE(b);
			CAPTURE(on);
			CHECK(eq::effectiveForTest(dev.storage(), b).freqHz == (on ? 2.0 * stored : stored));
		}
	}
}

TEST_CASE("EQ Key Track: a fifth up scales by f0 / C3, and the next note moves it again") {
	Instance dev;
	dev.set(trackedLayout(true));
	const double stored = std::exp2(static_cast<double>(trackedLayout(true).bands[3].freqLog2));
	dev.note(67.0f);
	runFor(dev, 9600);
	// f0 / C3 for G3 = 2^(7/12), restated from the note rather than from the device's octaves.
	const double g3 = 440.0 * std::exp2((67.0 - 69.0) / 12.0), c3 = 440.0 * std::exp2((60.0 - 69.0) / 12.0);
	CHECK(eq::effectiveForTest(dev.storage(), 3).freqHz == doctest::Approx(stored * g3 / c3).epsilon(1e-9));
	dev.note(48.0f); // C2: an octave below
	runFor(dev, 9600);
	CHECK(eq::effectiveForTest(dev.storage(), 3).freqHz == doctest::Approx(stored * 0.5).epsilon(1e-12));
}

TEST_CASE("EQ Key Track: with no note heard it stays at C3, and publishes the octaves it applies") {
	Instance dev;
	dev.listen();
	dev.set(trackedLayout(true));
	runFor(dev, 4800);
	const double stored = std::exp2(static_cast<double>(trackedLayout(true).bands[5].freqLog2));
	CHECK(eq::effectiveForTest(dev.storage(), 5).freqHz == stored);
	REQUIRE(dev.allReadings.size() == eq::kReadingCount);
	CHECK(dev.allReadings[static_cast<size_t>(eq::Reading::KeyOctaves)] == 0.0f);
	dev.note(84.0f); // two octaves up
	runFor(dev, 4800);
	CHECK(dev.allReadings[static_cast<size_t>(eq::Reading::KeyOctaves)] == 2.0f);
	// OFF PUBLISHES ZERO, whatever the note: the plot draws the device's own number.
	dev.set(trackedLayout(false));
	runFor(dev, 4800);
	CHECK(dev.allReadings[static_cast<size_t>(eq::Reading::KeyOctaves)] == 0.0f);
}

TEST_CASE("EQ Key Track: off is today's EQ to the bit, whatever note is played") {
	const eq::Settings s = trackedLayout(false);
	Instance a, b;
	a.set(s);
	b.set(s);
	std::vector<float> la = noise(48000, 11), ra = noise(48000, 12);
	std::vector<float> lb = la, rb = ra;
	for (int at = 0; at < 48000; at += 256) {
		b.note(static_cast<float>(36 + (at / 256) % 48)); // a note every block on one, none on the other
		const int n = std::min(256, 48000 - at);
		a.process(la.data() + at, ra.data() + at, n);
		b.process(lb.data() + at, rb.data() + at, n);
	}
	CHECK(bitEqual(la, lb));
	CHECK(bitEqual(ra, rb));
	// NOT VACUOUS: the same notes with Key Track ON change the output.
	Instance c;
	eq::Settings tracked = s;
	tracked.keyTrack = true;
	c.set(tracked);
	std::vector<float> lc = noise(48000, 11), rc = noise(48000, 12);
	for (int at = 0; at < 48000; at += 256) {
		c.note(static_cast<float>(36 + (at / 256) % 48));
		c.process(lc.data() + at, rc.data() + at, std::min(256, 48000 - at));
	}
	CHECK_FALSE(bitEqual(la, lc));
}

TEST_CASE("EQ Key Track: a stored switch, off by default and not modulatable, under its own key") {
	const auto& d = eq::kParamDescs[eq::paramIndex(eq::Param::KeyTrack)];
	CHECK(std::string(d.key) == "key_track");
	CHECK(d.def == 0.0);
	CHECK(d.stepped);
	CHECK_FALSE(d.modulatable);
	CHECK_FALSE(eq::Settings {}.keyTrack);
	// IT ROUND-TRIPS, and a row that predates it reads Off.
	eq::Settings s = flat();
	s.keyTrack = true;
	CHECK(eq::stateOf(eq::rowOf(s), eq::kParamCount).keyTrack);
	CHECK_FALSE(eq::stateOf(eq::rowOf(s), eq::paramIndex(eq::Param::KeyTrack)).keyTrack);
}

// PLATFORM-PINNED: the hashes were measured on Linux x86-64; libm and float contraction differ on Apple, so the bits do too (the same
// reason vox-voice and flagship carry the platform-pinned label). Case-level guard: the rest of the suite still runs on macOS.
#if defined(__x86_64__) && defined(__linux__)
TEST_CASE("Rack stage 2 / §7a: lifting the analyser out of eq.cpp left the EQ's readings BIT-IDENTICAL — every published frame, three views, both taps, by hash of the bits") {
	// THE PROOF THE LIFT CHANGED NOTHING: a stereo noise (L and R different) through a +6 dB bell, every reading the EQ
	// publishes over two seconds, in each of the three views, hashed bit by bit (FNV-1a over each float's bits). The
	// constants were MEASURED on the code BEFORE the lift (eq.cpp's own `Analyser`, at 3afd66f8) and are what the lifted
	// `dsp::Analyser` must reproduce exactly.
	const auto hashOf = [](int view) {
		Instance dev;
		uint64_t hash = 1469598103934665603ull;
		size_t frames = 0;
		dev.listen();
		std::array<float, eq::kExtraState> extra {};
		extra[static_cast<size_t>(eq::Extra::Version)] = 1.0f;
		extra[static_cast<size_t>(eq::Extra::AnalyserChannel)] = static_cast<float>(view);
		dev.setWithExtra(withBell(3, 1000.0, 6.0f, 2.0), extra);
		std::vector<float> l = noise(96000, 7u);
		std::vector<float> r = noise(96000, 11u);
		float lastFrame = 0.0f;
		for (int at = 0; at < 96000; at += 256) {
			dev.process(l.data() + at, r.data() + at, 256);
			if (dev.allReadings.empty()) {
				continue;
			}
			const float frame = dev.allReadings[static_cast<size_t>(eq::Reading::FrameA)];
			if (frame == lastFrame) {
				continue;
			}
			lastFrame = frame;
			++frames;
			for (size_t i = static_cast<size_t>(eq::Reading::FrameA); i <= static_cast<size_t>(eq::Reading::FrameB); ++i) {
				uint32_t bits = 0;
				std::memcpy(&bits, &dev.allReadings[i], sizeof(bits));
				for (int b = 0; b < 4; ++b) {
					hash ^= (bits >> (8 * b)) & 0xFFu;
					hash *= 1099511628211ull;
				}
			}
		}
		return std::pair<uint64_t, size_t> { hash, frames };
	};
	const uint64_t kBefore[3] = { 10460662479627648722ull, 12220929874821164315ull, 13100679701499237962ull };
	for (int view = 0; view < 3; ++view) {
		const auto [hash, frames] = hashOf(view);
		CAPTURE(view);
		CHECK(frames == 46);
		CHECK(hash == kBefore[view]);
	}
}
#endif
