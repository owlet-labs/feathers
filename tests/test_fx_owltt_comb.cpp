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
#include "doctest.h"

#include "fx/owltt/comb_split.h"
#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "portable_aligned_alloc.h"
#include "test_heap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <new>
#include <numbers>
#include <string>
#include <vector>

// THE ALLOCATION PROBE, counting only while armed (the same as test_fx_owltt_linear.cpp's).
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

using namespace feathers::fx::owltt;

// OWLTT'S COMB CROSSOVER (Mode 15): the three bands are the harmonic classes of f0 — odd, 2 mod 4, 0 mod 4 — and sum to the input exactly.

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = std::numbers::pi;

Settings patchDefault() {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	return stateOf(row);
}

/// Comb at the patch default with the dynamics at rest (Ratio 0, no clipper): the device is then linear and the bands, unprocessed.
Settings combAtRest(double f0) {
	Settings s = patchDefault();
	s.mode = static_cast<float>(kCombMode);
	s.ratio = 0.0f;
	s.softClip = false;
	s.blend = 1.0f;
	s.keyTrack = false;
	s.lowCrossover = static_cast<float>(f0);
	return s;
}

struct Noise {
	uint32_t s;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	}
};

/// The k-th harmonic's amplitude in `y` (from `from`), by a Hann-windowed single-bin DFT at k * f0: 1.0 for a unit cosine.
double harmonicAmplitude(const std::vector<float>& y, size_t from, double f0, int k) {
	std::complex<double> sum = 0.0;
	double window = 0.0;
	const size_t n = y.size() - from;
	for (size_t i = 0; i < n; ++i) {
		const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
		sum += w * static_cast<double>(y[from + i]) * std::polar(1.0, -2.0 * kPi * k * f0 * static_cast<double>(from + i) / kRate);
		window += w;
	}
	return 2.0 * std::abs(sum) / window;
}

/// THE CLASS A HARMONIC BELONGS TO, restated from the stated construction: 0 = odd (Low), 1 = 2 mod 4 (Mid), 2 = 0 mod 4 (High).
int classOf(int k) { return k % 2 != 0 ? 0 : (k % 4 == 2 ? 1 : 2); }

/// A sum of unit cosines at harmonics 1..`top` of `f0`, `n` samples.
std::vector<float> harmonicTone(double f0, int top, size_t n) {
	std::vector<float> x(n, 0.0f);
	for (size_t i = 0; i < n; ++i) {
		double v = 0.0;
		for (int k = 1; k <= top; ++k) {
			v += std::cos(2.0 * kPi * k * f0 * static_cast<double>(i) / kRate + 0.37 * k);
		}
		x[i] = static_cast<float>(v / top);
	}
	return x;
}

} // namespace

TEST_CASE("Comb: Mode 15 is the last choice, the fourteen before keep their numbers, and it adds no latency") {
	REQUIRE(static_cast<long>(std::size(kModeNames)) == kLastMode);
	CHECK(kCombMode == kLastMode);
	CHECK(std::string(kModeNames[kCombMode - 1]) == "Comb (3 classes)");
	CHECK(std::string(kModeNames[kCombMode - 2]) == "LR8 Full (48 dB/oct)"); // the one before it
	CHECK(std::string(kModeNames[9]) == "Twist3");
	CHECK(kParamDescs[static_cast<size_t>(Param::Mode)].max == static_cast<double>(kLastMode));
	Settings s = combAtRest(200.0);
	const Row row = rowOf(s);
	CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), kRate) == 0u);
	CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), 192000.0) == 0u);
}

TEST_CASE("Comb: the three bands sum to the input exactly, at whole- and fractional-sample periods") {
	// 400 Hz is D = 60 whole samples; the others are 54.545, 218.18, 21.82, 4.8 ... (fractional) and the extremes of the range.
	for (const double f0 : { 20.0, 55.0, 220.0, 400.0, 440.0, 1000.0, 1999.0 }) {
		CAPTURE(f0);
		CombSplit comb;
		comb.prepare(kRate);
		const float d1 = static_cast<float>(comb.delayFor(f0, kRate));
		CHECK(std::fabs(d1 - 0.5 * kRate / f0) < 1e-3); // the delay is fs / (2 f0)
		Noise noise { 12345u };
		double worst = 0.0;
		for (int i = 0; i < 20000; ++i) {
			const float x = noise.next();
			const CombSplit::Bands b = comb.process(0, x, d1);
			worst = std::max(worst, std::fabs(static_cast<double>(b.odd) + b.two + b.four - x));
		}
		MESSAGE("f0 " << f0 << ": worst |sum - dry| " << worst);
		CHECK(worst < 1e-6);
	}
}

TEST_CASE("Comb: through the device with the dynamics at rest, the output is the input, at whole and fractional periods") {
	for (const double f0 : { 55.0, 220.0, 400.0, 440.0, 1000.0 }) {
		CAPTURE(f0);
		std::vector<float> in(24064), l, r; // a whole number of 256-frame blocks
		Noise noise { 777u };
		for (auto& v : in) {
			v = 0.5f * noise.next();
		}
		l = in;
		r = in;
		Owltt o;
		o.prepare(kRate);
		o.setTarget(combAtRest(f0));
		for (size_t at = 0; at < l.size(); at += 256) {
			o.processStereo(&l[at], &r[at], 256);
		}
		double worst = 0.0;
		for (size_t i = 0; i < in.size(); ++i) {
			worst = std::max(worst, std::fabs(static_cast<double>(l[i]) - in[i]));
		}
		MESSAGE("f0 " << f0 << ": worst |out - in| " << worst);
		CHECK(o.combDelayForTest() == doctest::Approx(0.5 * kRate / f0).epsilon(1e-9));
		CHECK(worst < 1e-5); // the left channel, as the other modes' cases read it; the bands themselves sum to 1e-6
	}
}

TEST_CASE("Comb: each band holds its harmonic classes and rejects the others") {
	struct Case { double f0; double rejectDb; };
	// A whole-sample delay (400 Hz, D = 60) is exact to the float; a fractional one is limited by the twelve-tap interpolator.
	for (const Case c : { Case { 400.0, 120.0 }, Case { 441.0, 60.0 }, Case { 220.0, 60.0 } }) {
		CAPTURE(c.f0);
		constexpr int kTop = 16; // harmonics 1..16: up to 7 kHz at 441 Hz
		const size_t n = 48000;
		const std::vector<float> x = harmonicTone(c.f0, kTop, n);
		CombSplit comb;
		comb.prepare(kRate);
		const float d1 = static_cast<float>(comb.delayFor(c.f0, kRate));
		std::array<std::vector<float>, 3> y;
		for (auto& band : y) {
			band.resize(n);
		}
		for (size_t i = 0; i < n; ++i) {
			const CombSplit::Bands b = comb.process(0, x[i], d1);
			y[0][i] = b.odd;
			y[1][i] = b.two;
			y[2][i] = b.four;
		}
		double worstOwned = 0.0;
		double worstOther = 1e9;
		for (int k = 1; k <= kTop; ++k) {
			for (int band = 0; band < 3; ++band) {
				// each harmonic enters at amplitude 1/kTop
				const double gain = harmonicAmplitude(y[static_cast<size_t>(band)], 4000, c.f0, k) * kTop;
				if (band == classOf(k)) {
					worstOwned = std::max(worstOwned, std::fabs(20.0 * std::log10(std::max(gain, 1e-9))));
				}
				else {
					worstOther = std::min(worstOther, -20.0 * std::log10(std::max(gain, 1e-12)));
				}
			}
		}
		MESSAGE("f0 " << c.f0 << ": owned harmonics within " << worstOwned << " dB of unity; others rejected by at least " << worstOther << " dB");
		CHECK(worstOwned < 1.0);
		CHECK(worstOther > c.rejectDb);
	}
}

TEST_CASE("Comb: a moving fundamental glides, and the bands still sum to the input throughout") {
	CombSplit comb;
	comb.prepare(kRate);
	Noise noise { 99u };
	double d = comb.delayFor(110.0, kRate);
	const double dEnd = comb.delayFor(400.0, kRate);
	double worst = 0.0;
	float prevOdd = 0.0f;
	double worstStep = 0.0;
	double worstStepInput = 0.0;
	float prevX = 0.0f;
	for (int i = 0; i < 48000; ++i) {
		d += (dEnd - d) * 0.0005;
		const float x = 0.5f * std::sin(static_cast<float>(2.0 * kPi * 1000.0 * i / kRate));
		const CombSplit::Bands b = comb.process(0, x, static_cast<float>(d));
		worst = std::max(worst, std::fabs(static_cast<double>(b.odd) + b.two + b.four - x));
		if (i > 100) {
			worstStep = std::max(worstStep, std::fabs(static_cast<double>(b.odd) - prevOdd));
			worstStepInput = std::max(worstStepInput, std::fabs(static_cast<double>(x) - prevX));
		}
		prevOdd = b.odd;
		prevX = x;
	}
	(void)noise;
	MESSAGE("worst sum error " << worst << "; largest odd-band step " << worstStep << " against the input's " << worstStepInput);
	CHECK(worst < 1e-6);
	CHECK(worstStep < 2.0 * worstStepInput); // no click: a band moves no faster than twice what the input does
}

TEST_CASE("Comb: compression acts per harmonic class, a loud tone of one class compresses its band and not the others") {
	// f0 = 200 Hz (Low Crossover's default, Key Track off): 600 Hz is the 3rd harmonic (odd, Low), 400 Hz the 2nd (2 mod 4, Mid), 800 Hz the 4th (0 mod 4, High).
	const struct { double hz; size_t band; } cases[] = { { 600.0, 0 }, { 400.0, 1 }, { 800.0, 2 } };
	for (const auto& c : cases) {
		CAPTURE(c.hz);
		Settings s = patchDefault();
		s.mode = static_cast<float>(kCombMode);
		s.keyTrack = false;
		s.lowCrossover = 200.0f;
		s.ratio = 1.0f;
		s.threshold = 0.0f;
		s.softClip = false;
		Owltt o;
		o.prepare(kRate);
		o.setTarget(s);
		std::vector<float> l(512), r(512);
		size_t n = 0;
		for (int block = 0; block < 60; ++block) {
			for (size_t i = 0; i < l.size(); ++i, ++n) {
				l[i] = r[i] = 0.8f * static_cast<float>(std::sin(2.0 * kPi * c.hz * static_cast<double>(n) / kRate));
			}
			o.processStereo(l.data(), r.data(), 512);
		}
		std::array<float, 3> gain {};
		for (size_t b = 0; b < 3; ++b) {
			gain[b] = o.lastTargetBandGainForTest(b);
		}
		MESSAGE(c.hz << " Hz: band gains " << gain[0] << " " << gain[1] << " " << gain[2]);
		for (size_t b = 0; b < 3; ++b) {
			if (b == c.band) {
				CHECK(gain[b] < 0.5f);
			}
			else {
				CHECK(gain[b] > 0.95f);
			}
		}
	}
}

TEST_CASE("Comb: Key Track puts f0 on the played note, and the bands follow it") {
	feathers::device::NoteContext note;
	note.gate = true;
	Settings s = patchDefault();
	s.mode = static_cast<float>(kCombMode);
	s.keyTrack = true;
	s.lowCrossover = 200.0f; // the default: f0 is the note itself
	s.ratio = 1.0f;
	s.threshold = 0.0f;
	s.softClip = false;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	const auto run = [&](double hz, int blocks) {
		std::vector<float> l(512), r(512);
		static size_t n = 0;
		for (int block = 0; block < blocks; ++block) {
			for (size_t i = 0; i < l.size(); ++i, ++n) {
				l[i] = r[i] = 0.8f * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(n) / kRate));
			}
			o.processStereo(l.data(), r.data(), 512);
		}
	};
	// A3 (220 Hz): a loud 440 Hz is its 2nd harmonic, so the Mid band takes it.
	note.pitchSemitones = 57.0f;
	o.setNoteContext(note);
	run(440.0, 60);
	CHECK(o.combF0HzForTest() == doctest::Approx(220.0).epsilon(1e-6));
	CHECK(o.combDelayForTest() == doctest::Approx(kRate / (2.0 * 220.0)).epsilon(1e-6));
	CHECK(o.lastTargetBandGainForTest(1) < 0.5f);
	CHECK(o.lastTargetBandGainForTest(0) > 0.95f);
	CHECK(o.lastTargetBandGainForTest(2) > 0.95f);
	// A4 (440 Hz): the same tone is now the fundamental, so the Low band takes it, and f0 has glided there.
	note.pitchSemitones = 69.0f;
	o.setNoteContext(note);
	run(440.0, 120);
	CHECK(o.combF0HzForTest() == doctest::Approx(440.0).epsilon(1e-5));
	CHECK(o.lastTargetBandGainForTest(0) < 0.5f);
	CHECK(o.lastTargetBandGainForTest(1) > 0.95f);
	CHECK(o.lastTargetBandGainForTest(2) > 0.95f);
	// The knob's offset moves it as the other crossovers' does: 400 Hz is +12 semitones from the 200 Hz default, an octave above the note.
	s.lowCrossover = 400.0f;
	o.setTarget(s);
	run(440.0, 120);
	CHECK(o.combF0HzForTest() == doctest::Approx(880.0).epsilon(1e-5));
}

TEST_CASE("Comb: the fundamental is clamped to what the lines serve, and f0 follows the knob when Key Track is off") {
	Settings s = combAtRest(100.0);
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	for (int b = 0; b < 4; ++b) {
		o.processStereo(l.data(), r.data(), 512);
	}
	CHECK(o.combF0HzForTest() == doctest::Approx(100.0).epsilon(1e-9));
	s.lowCrossover = 2000.0f; // the top of the knob: fs / 24 = 2000 Hz at 48 kHz is the line's reach
	o.setTarget(s);
	for (int b = 0; b < 200; ++b) {
		o.processStereo(l.data(), r.data(), 512);
	}
	CHECK(o.combF0HzForTest() == doctest::Approx(2000.0).epsilon(1e-6));
	CHECK(o.combDelayForTest() >= 6.0); // the interpolator's reach: the second comb (half of this) never reads below six samples
	CHECK(o.combDelayForTest() / 2.0 >= 6.0);
}

TEST_CASE("Comb: after prepare nothing allocates, through a moving f0, Key Track notes and a mode change") {
	Settings s = combAtRest(150.0);
	s.keyTrack = true;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	std::vector<float> l(512), r(512);
	Noise noise { 5u };
	feathers::device::NoteContext note;
	note.gate = true;
	allocs.allocations = 0;
	allocs.armed = true;
	for (int block = 0; block < 80; ++block) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = noise.next();
			r[i] = noise.next();
		}
		note.pitchSemitones = 40.0f + static_cast<float>(block % 30);
		o.setNoteContext(note);
		if (block == 40) {
			s.mode = 6.0f; // out of Comb and back
			o.setTarget(s);
		}
		if (block == 50) {
			s.mode = static_cast<float>(kCombMode);
			s.lowCrossover = 600.0f;
			o.setTarget(s);
		}
		o.processStereo(l.data(), r.data(), 512);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0u);
}

TEST_CASE("Comb: a fundamental that jumps moves the delay across each block, so the bands do not click") {
	// The bands are scaled unequally (Low -12 dB, High +6 dB) so the output is NOT the input: it is a weighted sum of the three class signals, which move when the
	// delay does. f0 jumping from 100 to 400 Hz and back every seven blocks moves the delay fast (a Doppler shift), so the bound is a few times the fixed-f0 step: a delay
	// that STEPPED at a block edge is a click of most of the amplitude (measured 0.93 against 0.17 here and 0.065 at a fixed f0).
	const auto largestStep = [](bool jump) {
		Settings s = combAtRest(100.0);
		s.low.inputDb = -12.0f;
		s.high.inputDb = 6.0f;
		Owltt o;
		o.prepare(kRate);
		o.setTarget(s);
		std::vector<float> l(256), r(256);
		size_t n = 0;
		float prev = 0.0f;
		double worst = 0.0;
		for (int block = 0; block < 400; ++block) {
			s.lowCrossover = jump && (block / 7) % 2 == 1 ? 400.0f : 100.0f;
			o.setTarget(s);
			for (size_t i = 0; i < l.size(); ++i, ++n) {
				l[i] = r[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kRate));
			}
			o.processStereo(l.data(), r.data(), 256);
			for (size_t i = 0; i < l.size(); ++i) {
				if (block > 20) {
					worst = std::max(worst, std::fabs(static_cast<double>(l[i]) - prev));
				}
				prev = l[i];
			}
		}
		return worst;
	};
	const double still = largestStep(false);
	const double jumping = largestStep(true);
	MESSAGE("largest output step: fixed f0 " << still << ", jumping f0 " << jumping);
	CHECK(jumping < 4.0 * still);
}
