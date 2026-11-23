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

// OWLTT'S COMB CROSSOVER (Modes 15, 16): a tree of two combs. Low = between the first comb's teeth, Mid = on them but off the second's, High = on both. At the
// default spacings (x1, x2) that is: between the harmonics, the odd harmonics, the even harmonics. The bands sum to the input exactly.

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

/// Comb with the dynamics at rest (Ratio 0, no clipper): the device is then linear and the bands, unprocessed. The two knobs are the spacings in Hz as
/// `combSpacingHz` reads them: Low is `fc1`, High is five times `fc2`.
Settings combAtRest(double fc1, double fc2, long mode = kCombMode) {
	Settings s = patchDefault();
	s.mode = static_cast<float>(mode);
	s.ratio = 0.0f;
	s.softClip = false;
	s.blend = 1.0f;
	s.keyTrack = false;
	s.lowCrossover = static_cast<float>(fc1);
	s.highCrossover = static_cast<float>(fc2 * 5.0);
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

/// The three bands of `x` through the tree at spacings fc1, fc2 (constant delays), the first channel.
std::array<std::vector<float>, 3> splitOf(const std::vector<float>& x, double fc1, double fc2, int stages = 1) {
	CombSplit comb;
	comb.prepare(kRate);
	const auto d1 = static_cast<float>(CombSplit::delayFor(fc1, kRate));
	const auto d2 = static_cast<float>(CombSplit::delayFor(fc2, kRate));
	std::array<std::vector<float>, 3> y;
	for (auto& band : y) {
		band.resize(x.size());
	}
	for (size_t i = 0; i < x.size(); ++i) {
		const CombSplit::Bands b = comb.process(0, x[i], d1, d2, CombShape { stages });
		y[0][i] = b.between;
		y[1][i] = b.partB;
		y[2][i] = b.partA;
	}
	return y;
}

double powerOf(const std::vector<float>& v, size_t from) {
	double sq = 0.0;
	for (size_t i = from; i < v.size(); ++i) {
		sq += static_cast<double>(v[i]) * v[i];
	}
	return sq / static_cast<double>(v.size() - from);
}

} // namespace

TEST_CASE("Comb: Modes 15 and 16 are the last choices, the fourteen before keep their numbers, and they add no latency") {
	REQUIRE(static_cast<long>(std::size(kModeNames)) == kLastMode);
	CHECK(kCombSharpMode == kLastMode);
	CHECK(kCombMode == kLastMode - 1);
	CHECK(std::string(kModeNames[kCombMode - 1]) == "Comb");
	CHECK(std::string(kModeNames[kCombSharpMode - 1]) == "Comb Sharp");
	CHECK(std::string(kModeNames[kCombMode - 2]) == "LR8 Full (48 dB/oct)");
	CHECK(std::string(kModeNames[9]) == "Twist3");
	CHECK(kParamDescs[static_cast<size_t>(Param::Mode)].max == static_cast<double>(kLastMode));
	for (const long mode : { kCombMode, kCombSharpMode }) {
		const Row row = rowOf(combAtRest(200.0, 400.0, mode));
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), kRate) == 0u);
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), 192000.0) == 0u);
	}
}

TEST_CASE("Comb: the three bands sum to the input exactly, for any pair of spacings, at whole and fractional periods, plain and sharp") {
	// 400 Hz is D = 120 whole samples; most of the others are fractional.
	const std::array<std::pair<double, double>, 8> pairs { { { 20.0, 40.0 }, { 55.0, 110.0 }, { 220.0, 440.0 }, { 400.0, 800.0 }, { 440.0, 1320.0 },
		                                                    { 1000.0, 500.0 }, { 1999.0, 4000.0 }, { 3.0, 300.0 } } };
	for (const int stages : { 1, 2 }) {
		for (const auto& [fc1, fc2] : pairs) {
			CAPTURE(fc1);
			CAPTURE(fc2);
			CAPTURE(stages);
			CombSplit comb;
			comb.prepare(kRate);
			const auto d1 = static_cast<float>(CombSplit::delayFor(fc1, kRate));
			const auto d2 = static_cast<float>(CombSplit::delayFor(fc2, kRate));
			Noise noise { 12345u };
			double worst = 0.0;
			for (int i = 0; i < 20000; ++i) {
				const float x = noise.next();
				const CombSplit::Bands b = comb.process(0, x, d1, d2, CombShape { stages });
				worst = std::max(worst, std::fabs(static_cast<double>(b.between) + b.partB + b.partA - x));
			}
			CHECK(worst < 1e-6);
		}
	}
}

TEST_CASE("Comb: through the device with the dynamics at rest, the output is the input, at several spacing pairs and in both modes") {
	for (const long mode : { kCombMode, kCombSharpMode }) {
		for (const auto& [fc1, fc2] : { std::pair { 55.0, 110.0 }, std::pair { 220.0, 440.0 }, std::pair { 441.0, 1323.0 }, std::pair { 1000.0, 500.0 } }) {
			CAPTURE(fc1);
			CAPTURE(fc2);
			std::vector<float> in(24064), l, r; // a whole number of 256-frame blocks
			Noise noise { 777u };
			for (auto& v : in) {
				v = 0.5f * noise.next();
			}
			l = in;
			r = in;
			Owltt o;
			o.prepare(kRate);
			o.setTarget(combAtRest(fc1, fc2, mode));
			for (size_t at = 0; at < l.size(); at += 256) {
				o.processStereo(&l[at], &r[at], 256);
			}
			double worst = 0.0;
			for (size_t i = 0; i < in.size(); ++i) {
				worst = std::max(worst, std::fabs(static_cast<double>(l[i]) - in[i])); // the left channel, as the other modes' cases read it
			}
			CHECK(o.combDelayForTest(0) == doctest::Approx(kRate / fc1).epsilon(1e-9));
			CHECK(o.combDelayForTest(1) == doctest::Approx(kRate / fc2).epsilon(1e-9));
			CHECK(worst < 1e-5);
		}
	}
}

TEST_CASE("Comb: at the defaults a harmonic tone's odd and even harmonics land in Mid and High and none in Low; noise splits a half, a quarter, a quarter") {
	struct Case { double fc; double rejectDb; };
	// A whole-sample delay (400 Hz, D = 120) is exact to the float; a fractional one is limited by the twelve-tap interpolator.
	for (const Case c : { Case { 400.0, 120.0 }, Case { 441.0, 55.0 }, Case { 220.0, 55.0 } }) {
		CAPTURE(c.fc);
		constexpr int kTop = 16;
		const std::vector<float> x = harmonicTone(c.fc, kTop, 48000);
		const auto y = splitOf(x, c.fc, 2.0 * c.fc);
		double worstOwned = 0.0;
		double worstOther = 1e9;
		for (int k = 1; k <= kTop; ++k) {
			const size_t owner = k % 2 != 0 ? 1 : 2; // odd: Mid; even: High
			for (size_t band = 0; band < 3; ++band) {
				const double gain = harmonicAmplitude(y[band], 4000, c.fc, k) * kTop;
				if (band == owner) {
					worstOwned = std::max(worstOwned, std::fabs(20.0 * std::log10(std::max(gain, 1e-9))));
				}
				else {
					worstOther = std::min(worstOther, -20.0 * std::log10(std::max(gain, 1e-12)));
				}
			}
		}
		MESSAGE("fc " << c.fc << ": owned harmonics within " << worstOwned << " dB of unity; others rejected by at least " << worstOther << " dB");
		CHECK(worstOwned < 1.0);
		CHECK(worstOther > c.rejectDb);
	}
	Noise noise { 4242u };
	std::vector<float> white(96000);
	for (auto& v : white) {
		v = noise.next();
	}
	const auto y = splitOf(white, 441.0, 882.0);
	const double total = powerOf(white, 4000);
	MESSAGE("noise shares " << powerOf(y[0], 4000) / total << " " << powerOf(y[1], 4000) / total << " " << powerOf(y[2], 4000) / total);
	CHECK(powerOf(y[0], 4000) / total == doctest::Approx(0.5).epsilon(0.05));
	CHECK(powerOf(y[1], 4000) / total == doctest::Approx(0.25).epsilon(0.1));
	CHECK(powerOf(y[2], 4000) / total == doctest::Approx(0.25).epsilon(0.1));
}

TEST_CASE("Comb: a second spacing of x3 pulls out every third harmonic, and x1/2 halves the tooth spacing") {
	constexpr int kTop = 18;
	const double f0 = 400.0;
	const std::vector<float> x = harmonicTone(f0, kTop, 48000);
	const auto y = splitOf(x, f0, 3.0 * f0);
	// THE COMB AT SPACING 3 f0 has gain |cos(pi k / 3)| at harmonic k (and the residual |sin|): every third harmonic passes whole to High and none reaches
	// Mid, the others are split 0.5 : 0.866 in amplitude. A two-tap comb cannot do better; Comb Sharp squares it (below).
	for (int k = 1; k <= kTop; ++k) {
		CAPTURE(k);
		CHECK(harmonicAmplitude(y[2], 4000, f0, k) * kTop == doctest::Approx(std::fabs(std::cos(kPi * k / 3.0))).epsilon(1e-3));
		CHECK(harmonicAmplitude(y[1], 4000, f0, k) * kTop == doctest::Approx(std::fabs(std::sin(kPi * k / 3.0))).epsilon(1e-3).scale(1.0));
		if (k % 3 == 0) {
			CHECK(harmonicAmplitude(y[1], 4000, f0, k) * kTop < 1e-4);
		}
	}
	// A FIRST SPACING OF x1/2: the teeth are twice as dense, at the half harmonics too, so the 1.5th (a tone at 600 Hz) is ON the teeth and so is 400 Hz.
	// Measured by the teeth's own positions: a tone at fc * m passes the on-teeth bands (Mid + High) for every whole m, and falls in Low at m + 1/2.
	const double fc = f0 * 0.5;
	for (const int m : { 1, 2, 3, 5 }) {
		for (const double offset : { 0.0, 0.5 }) {
			const double hz = fc * (m + offset);
			std::vector<float> tone(24000);
			for (size_t i = 0; i < tone.size(); ++i) {
				tone[i] = static_cast<float>(std::cos(2.0 * kPi * hz * static_cast<double>(i) / kRate));
			}
			const auto z = splitOf(tone, fc, fc * 2.0);
			std::vector<float> onTeeth(tone.size());
			for (size_t i = 0; i < onTeeth.size(); ++i) {
				onTeeth[i] = z[1][i] + z[2][i];
			}
			const double on = powerOf(onTeeth, 2000);
			const double between = powerOf(z[0], 2000);
			CAPTURE(hz);
			if (offset == 0.0) {
				CHECK(on / (on + between) > 0.999);
			}
			else {
				CHECK(between / (on + between) > 0.999);
			}
		}
	}
}

TEST_CASE("Comb Sharp: the same tree with each comb run twice; its teeth are narrower and its sum is still exact") {
	// A tone a quarter of the way between two teeth: the plain comb passes cos(pi/4) = 0.707 of its amplitude (power 0.5) to the on-teeth bands, the squared one 0.5 (power 0.25).
	const double fc = 441.0;
	std::vector<float> tone(24000);
	for (size_t i = 0; i < tone.size(); ++i) {
		tone[i] = static_cast<float>(std::cos(2.0 * kPi * fc * 3.25 * static_cast<double>(i) / kRate));
	}
	const double total = powerOf(tone, 2000);
	for (const auto& [stages, want] : { std::pair { 1, 0.5 }, std::pair { 2, 0.25 } }) {
		const auto y = splitOf(tone, fc, 2.0 * fc, stages);
		std::vector<float> onTeeth(tone.size());
		for (size_t i = 0; i < onTeeth.size(); ++i) {
			onTeeth[i] = y[1][i] + y[2][i];
		}
		const double on = powerOf(onTeeth, 2000) / total;
		CAPTURE(stages);
		CHECK(on == doctest::Approx(want).epsilon(0.02));
	}
}

TEST_CASE("Comb: compression acts per class, a loud tone of one class compresses its band and not the others") {
	// Spacings 200 / 400 Hz (the knobs' defaults 200 / 2000): 600 Hz is the 3rd harmonic (odd, Mid), 400 Hz the 2nd (even, High), 300 Hz between the teeth (Low).
	const struct { double hz; size_t band; } cases[] = { { 600.0, 1 }, { 400.0, 2 }, { 300.0, 0 } };
	for (const auto& c : cases) {
		CAPTURE(c.hz);
		Settings s = patchDefault();
		s.mode = static_cast<float>(kCombMode);
		s.keyTrack = false;
		s.lowCrossover = 200.0f;
		s.highCrossover = 2000.0f;
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
		for (size_t b = 0; b < 3; ++b) {
			const float gain = o.lastTargetBandGainForTest(b);
			CAPTURE(b);
			if (b == c.band) {
				CHECK(gain < 0.5f);
			}
			else {
				CHECK(gain > 0.95f);
			}
		}
	}
}

TEST_CASE("Comb: Key Track puts the spacings on the played note as ratios, and the bands follow it") {
	feathers::device::NoteContext note;
	note.gate = true;
	Settings s = patchDefault();
	s.mode = static_cast<float>(kCombMode);
	s.keyTrack = true;
	s.lowCrossover = 200.0f;  // x1: the note's own harmonics
	s.highCrossover = 2000.0f; // x2
	s.ratio = 1.0f;
	s.threshold = 0.0f;
	s.softClip = false;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	size_t n = 0;
	const auto run = [&](double hz, int blocks) {
		std::vector<float> l(512), r(512);
		for (int block = 0; block < blocks; ++block) {
			for (size_t i = 0; i < l.size(); ++i, ++n) {
				l[i] = r[i] = 0.8f * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(n) / kRate));
			}
			o.processStereo(l.data(), r.data(), 512);
		}
	};
	// A3 (220 Hz): a loud 440 Hz is its 2nd harmonic, an even one, so High takes it.
	note.pitchSemitones = 57.0f;
	o.setNoteContext(note);
	run(440.0, 60);
	CHECK(o.combSpacingHzForTest(0) == doctest::Approx(220.0).epsilon(1e-6));
	CHECK(o.combSpacingHzForTest(1) == doctest::Approx(440.0).epsilon(1e-6));
	CHECK(o.combDelayForTest(0) == doctest::Approx(kRate / 220.0).epsilon(1e-6));
	CHECK(o.lastTargetBandGainForTest(2) < 0.5f);
	CHECK(o.lastTargetBandGainForTest(0) > 0.95f);
	CHECK(o.lastTargetBandGainForTest(1) > 0.95f);
	// A4 (440 Hz): the same tone is now the fundamental, an odd harmonic, so Mid takes it, and the spacings have glided there.
	note.pitchSemitones = 69.0f;
	o.setNoteContext(note);
	run(440.0, 120);
	CHECK(o.combSpacingHzForTest(0) == doctest::Approx(440.0).epsilon(1e-5));
	CHECK(o.lastTargetBandGainForTest(1) < 0.5f);
	CHECK(o.lastTargetBandGainForTest(0) > 0.95f);
	CHECK(o.lastTargetBandGainForTest(2) > 0.95f);
	// The knobs are ratios: Low 100 is x1/2 (denser teeth), High 3000 is x3.
	s.lowCrossover = 100.0f;
	s.highCrossover = 3000.0f;
	o.setTarget(s);
	run(440.0, 120);
	CHECK(o.combSpacingHzForTest(0) == doctest::Approx(220.0).epsilon(1e-5));
	CHECK(o.combSpacingHzForTest(1) == doctest::Approx(1320.0).epsilon(1e-5));
}

TEST_CASE("Comb: ratios snap to their detents, read as ratios, and name the columns") {
	for (const double d : kCombDetents) {
		CHECK(combSnapRatio(d * 1.02) == d);
		CHECK(combSnapRatio(d / 1.02) == d);
		CHECK(combSnapRatio(d * 1.2) != d); // outside the reach
	}
	CHECK(combRatioText(1.0) == "x1");
	CHECK(combRatioText(0.5) == "x1/2");
	CHECK(combRatioText(1.0 / 8.0) == "x1/8");
	CHECK(combRatioText(1.37) == "x1.4");
	// The knob/ratio maps are each other's inverse, and the defaults are x1 and x2.
	CHECK(combRatioOfKnob(200.0, false) == 1.0);
	CHECK(combRatioOfKnob(2000.0, true) == 2.0);
	CHECK(combKnobOfRatio(combRatioOfKnob(333.0, true), true) == doctest::Approx(333.0));
	CHECK(combSpacingHz(2000.0, true) == 400.0);
	const auto names = [](double a, double b) { return combBandNames(a, b); };
	CHECK(names(200.0, 400.0) == std::array<std::string, 3> { "Between", "Odd", "Even" });
	CHECK(names(200.0, 600.0) == std::array<std::string, 3> { "Between", "Others", "Every 3" });
	CHECK(names(200.0, 300.0) == std::array<std::string, 3> { "Between", "Off x1.5", "On x1.5" });
}

TEST_CASE("Comb: the spacings are clamped to what the lines serve") {
	CHECK(CombSplit::delayFor(1.0e6, kRate) == 6.0);
	CHECK(CombSplit::delayFor(0.001, kRate) == doctest::Approx(kRate / CombSplit::minSpacing(kRate)));
	CHECK(CombSplit::delayFor(0.001, kRate) < CombSplit::kLineSize - 6);
	CHECK(CombSplit::minSpacing(kRate) < 2.5); // the lowest tracked note (20 Hz) at x1/8 is served at 48 kHz
	Settings s = combAtRest(20.0, 40.0);
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	for (int b = 0; b < 4; ++b) {
		o.processStereo(l.data(), r.data(), 512);
	}
	CHECK(o.combSpacingHzForTest(0) == doctest::Approx(20.0).epsilon(1e-9));
	CHECK(o.combSpacingHzForTest(1) == doctest::Approx(40.0).epsilon(1e-9));
	s = combAtRest(2000.0, 4000.0);
	o.setTarget(s);
	for (int b = 0; b < 200; ++b) {
		o.processStereo(l.data(), r.data(), 512);
	}
	CHECK(o.combSpacingHzForTest(1) == doctest::Approx(4000.0).epsilon(1e-6));
	CHECK(o.combDelayForTest(1) >= 6.0);
}

TEST_CASE("Comb: after prepare nothing allocates, through moving spacings, Key Track notes and mode changes") {
	Settings s = combAtRest(150.0, 300.0);
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
			s.mode = 6.0f; // out of Comb and back, into the sharp one
			o.setTarget(s);
		}
		if (block == 50) {
			s.mode = static_cast<float>(kCombSharpMode);
			s.lowCrossover = 600.0f;
			o.setTarget(s);
		}
		o.processStereo(l.data(), r.data(), 512);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0u);
}

TEST_CASE("Comb: a spacing that jumps moves the delay across each block, so the bands do not click") {
	// The bands are scaled unequally (Low -12 dB, High +6 dB) so the output is NOT the input: it is a weighted sum of the three class signals, which move when the
	// delays do. The first spacing jumping from 100 to 400 Hz and back every seven blocks moves its delay fast (a Doppler shift), so the bound is a few times the
	// fixed-spacing step: a delay that STEPPED at a block edge is a click of most of the amplitude (measured 0.93 against 0.17 at first, 0.065 fixed).
	const auto largestStep = [](bool jump) {
		Settings s = combAtRest(100.0, 200.0);
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
	MESSAGE("largest output step: fixed " << still << ", jumping " << jumping);
	CHECK(jumping < 4.0 * still);
}
