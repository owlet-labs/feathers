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

//------------------------------------------------------------------------------------------------//
// RESONANCE: Res Home plus Vibe's per-split phase; the bands are residuals, so they sum at any g.  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Comb resonance: the three bands sum to the input at g = 0, 0.5 and 0.95 and while g sweeps (to the float, 1e-6)") {
	for (const int stages : { 1, 2 }) {
		for (const float g : { 0.0f, 0.5f, kCombMaxResonance }) {
			CAPTURE(stages);
			CAPTURE(g);
			CombSplit comb;
			comb.prepare(kRate);
			const auto d1 = static_cast<float>(CombSplit::delayFor(441.0, kRate));
			const auto d2 = static_cast<float>(CombSplit::delayFor(882.0, kRate));
			Noise noise { 31u };
			double worst = 0.0;
			for (int i = 0; i < 30000; ++i) {
				const float x = 0.5f * noise.next();
				const CombSplit::Bands b = comb.process(0, x, d1, d2, CombShape { stages }, g, g);
				worst = std::max(worst, std::fabs(static_cast<double>(b.between) + b.partB + b.partA - x));
			}
			CHECK(worst < 1e-6);
		}
		// A SWEEP: g runs 0 .. 0.95 .. 0 across the run, a different one for each split, with the delays moving too.
		CombSplit comb;
		comb.prepare(kRate);
		Noise noise { 32u };
		double worst = 0.0;
		for (int i = 0; i < 60000; ++i) {
			const double t = static_cast<double>(i) / 60000.0;
			const float g1 = kCombMaxResonance * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * t * 3.0));
			const float g2 = kCombMaxResonance * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * t * 5.0));
			const auto d1 = static_cast<float>(CombSplit::delayFor(200.0 + 150.0 * std::sin(2.0 * kPi * t * 2.0), kRate));
			const auto d2 = static_cast<float>(CombSplit::delayFor(400.0 + 200.0 * std::sin(2.0 * kPi * t * 3.0), kRate));
			const float x = 0.5f * noise.next();
			const CombSplit::Bands b = comb.process(0, x, d1, d2, CombShape { stages }, g1, g2);
			worst = std::max(worst, std::fabs(static_cast<double>(b.between) + b.partB + b.partA - x));
		}
		CHECK(worst < 1e-6);
	}
}

TEST_CASE("Comb resonance: the teeth's gain off a tooth follows the feedback comb's law, so the Q rises with g") {
	// A tone a tenth of the way between two teeth (theta = 0.2 pi of the delay's phase). The on-teeth band is E = FB(FF x): its gain there is
	// |cos(theta / 2)| (the feedforward comb) times (1 - g) / |1 - g e^{-j theta}| (the feedback comb), restated here from the two transfer functions.
	const double fc = 441.0;
	const double theta = 0.2 * kPi;
	const double hz = fc * (3.0 + theta / (2.0 * kPi));
	double previous = 2.0;
	for (const float g : { 0.0f, 0.5f, 0.9f }) {
		CAPTURE(g);
		CombSplit comb;
		comb.prepare(kRate);
		const auto d1 = static_cast<float>(CombSplit::delayFor(fc, kRate));
		const auto d2 = static_cast<float>(CombSplit::delayFor(2.0 * fc, kRate));
		std::vector<float> on(48000);
		for (size_t i = 0; i < on.size(); ++i) {
			const auto x = static_cast<float>(std::cos(2.0 * kPi * hz * static_cast<double>(i) / kRate));
			const CombSplit::Bands b = comb.process(0, x, d1, d2, CombShape { 1 }, g, 0.0f);
			on[i] = b.partB + b.partA;
		}
		const double measured = std::sqrt(2.0 * powerOf(on, 20000));
		const std::complex<double> fb = (1.0 - g) / (1.0 - static_cast<double>(g) * std::polar(1.0, -theta));
		const double want = std::fabs(std::cos(theta / 2.0)) * std::abs(fb);
		MESSAGE("g " << g << ": gain a tenth off a tooth " << measured << ", restated " << want);
		CHECK(measured == doctest::Approx(want).epsilon(0.01));
		CHECK(measured < previous); // narrower teeth as g rises
		previous = measured;
	}
}

TEST_CASE("Comb resonance: Res Home sets the goal as a share of the clamp, Vibe's zone phase moves each split around it, and the clamp holds") {
	Settings s = combAtRest(200.0, 400.0);
	Owltt o;
	o.prepare(kRate);
	const auto run = [&](int blocks) {
		std::vector<float> l(512, 0.0f), r(512, 0.0f);
		for (int b = 0; b < blocks; ++b) {
			o.processStereo(l.data(), r.data(), 512);
		}
	};
	// AT THE DEFAULTS there is no resonance: Res Home 0 and Vibe at 0.
	s.vibe = 0.0f;
	s.combResHome = 0.0f;
	o.setTarget(s);
	run(4);
	CHECK(o.combResonanceGoalForTest(0) == 0.0f);
	CHECK(o.combResonanceGoalForTest(1) == 0.0f);
	// RES HOME moves both splits to its share of the clamp (Vibe still at 0), and the gain arrives there (smoothed).
	for (const float home : { 0.25f, 0.5f, 1.0f }) {
		CAPTURE(home);
		s.combResHome = home;
		o.setTarget(s);
		run(60);
		CHECK(o.combResonanceGoalForTest(0) == doctest::Approx(home * kCombMaxResonance).epsilon(1e-6));
		CHECK(o.combResonanceGoalForTest(1) == doctest::Approx(home * kCombMaxResonance).epsilon(1e-6));
		CHECK(o.combResonanceForTest(0) == doctest::Approx(home * kCombMaxResonance).epsilon(1e-5));
	}
	// VIBE IN SPREAD (mode 1): the two splits' phases are 0.2 and 0.6 of the position in the zone, added to Home, so they differ by the pattern.
	s.vibeMode = 1;
	s.vibe = 0.5f;
	s.combResHome = 0.25f;
	o.setTarget(s);
	run(60);
	const double home = 0.25 * kCombMaxResonance;
	const double pos = 0.5; // the position in the zone is the amount
	MESSAGE("goals " << o.combResonanceGoalForTest(0) << " " << o.combResonanceGoalForTest(1) << ", restated " << home + 0.2 * pos << " " << home + 0.6 * pos);
	CHECK(o.combResonanceGoalForTest(0) == doctest::Approx(home + kCombVibeResonanceDepth * 0.2 * pos).epsilon(1e-4));
	CHECK(o.combResonanceGoalForTest(1) == doctest::Approx(home + kCombVibeResonanceDepth * 0.6 * pos).epsilon(1e-4));
	CHECK(o.combResonanceGoalForTest(1) > o.combResonanceGoalForTest(0));
	// THE CLAMP: Home at the top and Vibe pushing past it stays at the named constant, both as goal and as gain.
	s.combResHome = 1.0f;
	s.vibe = 1.0f;
	s.vibeMode = 2; // Pairs: 0.5 of the position on both
	o.setTarget(s);
	run(60);
	for (size_t k = 0; k < 2; ++k) {
		CHECK(o.combResonanceGoalForTest(k) <= kCombMaxResonance);
		CHECK(o.combResonanceForTest(k) <= kCombMaxResonance);
	}
	CHECK(o.combResonanceForTest(1) == doctest::Approx(kCombMaxResonance));
}

TEST_CASE("Comb resonance: the device sums to the input at several Home and Vibe settings and through a sweep of both, and stays bounded at the clamp") {
	const struct { float home; uint32_t vibeMode; float vibe; } settings[] = { { 0.0f, 8, 0.0f }, { 0.5f, 8, 0.0f }, { 1.0f, 8, 0.0f }, { 0.3f, 1, 0.8f }, { 1.0f, 2, 1.0f }, { 0.6f, 7, 0.9f } };
	for (const long mode : { kCombMode, kCombSharpMode }) {
		for (const auto& c : settings) {
			CAPTURE(mode);
			CAPTURE(c.home);
			CAPTURE(c.vibeMode);
			Settings s = combAtRest(220.0, 660.0, mode);
			s.combResHome = c.home;
			s.vibeMode = c.vibeMode;
			s.vibe = c.vibe;
			std::vector<float> in(48128), l, r;
			Noise noise { 91u };
			for (auto& v : in) {
				v = 0.5f * noise.next();
			}
			l = in;
			r = in;
			Owltt o;
			o.prepare(kRate);
			o.setTarget(s);
			double worst = 0.0;
			double peak = 0.0;
			for (size_t at = 0; at < l.size(); at += 256) {
				if (at % 4096 == 0) { // a sweep of Res Home and Vibe as the run goes
					s.combResHome = static_cast<float>(0.5 - 0.5 * std::cos(static_cast<double>(at) / 9000.0));
					s.vibe = static_cast<float>(0.5 - 0.5 * std::sin(static_cast<double>(at) / 7000.0));
					o.setTarget(s);
				}
				o.processStereo(&l[at], &r[at], 256);
			}
			for (size_t i = 0; i < in.size(); ++i) {
				worst = std::max(worst, std::fabs(static_cast<double>(l[i]) - in[i]));
				peak = std::max(peak, std::fabs(static_cast<double>(l[i])));
			}
			CHECK(worst < 1e-5);
			CHECK(peak < 1.0); // sums to the input, which is bounded by 0.5: no growth anywhere
		}
	}
}

TEST_CASE("Comb resonance: at the clamp the teeth ring after the input stops and the output still reaches exact zero") {
	for (const long mode : { kCombMode, kCombSharpMode }) {
		CAPTURE(mode);
		Settings s = patchDefault();
		s.mode = static_cast<float>(mode);
		s.keyTrack = false;
		s.combResHome = 1.0f;
		s.ratio = 1.0f;
		s.threshold = 0.3f;
		Owltt o;
		o.prepare(kRate);
		o.setTarget(s);
		Noise noise { 7u };
		std::vector<float> l(512), r(512);
		for (int block = 0; block < 100; ++block) {
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = 0.5f * noise.next();
				r[i] = 0.5f * noise.next();
			}
			o.processStereo(l.data(), r.data(), 512);
		}
		int lastNonZero = -1;
		const int silentBlocks = 60 * 94; // 60 s
		for (int block = 0; block < silentBlocks; ++block) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			o.processStereo(l.data(), r.data(), 512);
			for (size_t i = 0; i < l.size(); ++i) {
				if (l[i] != 0.0f || r[i] != 0.0f) {
					lastNonZero = block;
					break;
				}
			}
		}
		MESSAGE("mode " << mode << ": last non-zero block " << lastNonZero << " of " << silentBlocks << " (" << lastNonZero * 512.0 / kRate << " s)");
		CHECK(lastNonZero >= 5); // not vacuous: the teeth rang
		CHECK(lastNonZero < silentBlocks / 2);
	}
}

TEST_CASE("Comb resonance: the loop is stable at the clamp at fractional delays (an impulse dies away), and the clamp is what keeps it so") {
	// THE SUM CANNOT SEE A LOOP BLOW UP (the complement cancels it), so the bands themselves are watched: an impulse through the resonant comb at a
	// fractional delay, then silence. At g = kCombMaxResonance the ringing falls by 0.95 a period; a loop gain of 1.02 would grow.
	for (const double fc : { 441.0, 200.0, 1234.5, 3000.0 }) {
		for (const int stages : { 1, 2 }) {
			CAPTURE(fc);
			CAPTURE(stages);
			const auto tail = [&](float g) {
				CombSplit comb;
				comb.prepare(kRate);
				const auto d1 = static_cast<float>(CombSplit::delayFor(fc, kRate));
				const auto d2 = static_cast<float>(CombSplit::delayFor(2.0 * fc, kRate));
				double early = 0.0;
				double late = 0.0;
				for (int i = 0; i < 480000; ++i) {
					const CombSplit::Bands b = comb.process(0, i == 0 ? 1.0f : 0.0f, d1, d2, CombShape { stages }, g, g);
					double m = std::max({ std::fabs(static_cast<double>(b.partA)), std::fabs(static_cast<double>(b.partB)), std::fabs(static_cast<double>(b.between)) });
					if (!std::isfinite(b.partA) || !std::isfinite(b.partB) || !std::isfinite(b.between)) {
						m = 1.0e30; // overflowed: a max() would drop a NaN
					}
					if (i < 48000) {
						early = std::max(early, m);
					}
					if (i >= 432000) {
						late = std::max(late, m);
					}
				}
				return std::pair { early, late };
			};
			const auto [early, late] = tail(kCombMaxResonance);
			MESSAGE("at the clamp: first second " << early << ", last second " << late);
			CHECK(early > 0.01); // not vacuous
			CHECK(late < early * 1.0e-3);
			CHECK(tail(1.02f).second > 10.0 * early); // an unclamped loop grows: this is what the clamp prevents
		}
	}
}

//------------------------------------------------------------------------------------------------//
// HP WET AND SUB THRU: upstream of the split and the detectors, in every mode.                     //
//------------------------------------------------------------------------------------------------//

namespace {

/// The log2 hertz HP Wet's control stores for `hz`.
float hpLog2(double hz) { return static_cast<float>(std::log2(hz)); }

/// A TWO-POLE BUTTERWORTH HIGHPASS, RESTATED from the bilinear transform (the RBJ biquad), the independent check on the shared trapezoidal filter.
std::vector<float> butterworthHighpass(const std::vector<float>& x, double hz) {
	const double w0 = 2.0 * kPi * hz / kRate;
	const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
	const double cw = std::cos(w0);
	const double a0 = 1.0 + alpha;
	const double b0 = (1.0 + cw) / 2.0 / a0;
	const double b1 = -(1.0 + cw) / a0;
	const double b2 = b0;
	const double a1 = -2.0 * cw / a0;
	const double a2 = (1.0 - alpha) / a0;
	std::vector<float> y(x.size());
	double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		const double v = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x[i];
		y2 = y1;
		y1 = v;
		y[i] = static_cast<float>(v);
	}
	return y;
}

std::vector<float> throughDevice(const Settings& s, const std::vector<float>& in) {
	std::vector<float> l = in, r = in;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	for (size_t at = 0; at < l.size(); at += 256) {
		o.processStereo(&l[at], &r[at], 256);
	}
	return l;
}

} // namespace

TEST_CASE("HP Wet: with Sub Thru and the dynamics at rest the output is the input in time, in Comb, in Linear and with a lookahead; without it, the high-passed input") {
	Noise noise { 2024u };
	std::vector<float> in(48128);
	for (auto& v : in) {
		v = 0.5f * noise.next();
	}
	for (const long mode : { kCombMode, kCombSharpMode, 11L }) {
		for (const uint32_t lookahead : { 0u, 5u }) {
			CAPTURE(mode);
			CAPTURE(lookahead);
			Settings s = combAtRest(220.0, 440.0, mode);
			s.hpWet = hpLog2(800.0);
			s.subThru = true;
			s.lookahead = lookahead;
			const Row row = rowOf(s);
			const size_t late = kAudioOps.latencySamples(nullptr, row.data(), row.size(), kRate);
			const std::vector<float> out = throughDevice(s, in);
			double worst = 0.0;
			for (size_t i = late; i < in.size(); ++i) {
				worst = std::max(worst, std::fabs(static_cast<double>(out[i]) - in[i - late]));
			}
			MESSAGE("mode " << mode << " lookahead " << lookahead << ": delay " << late << ", worst |out - delayed in| " << worst);
			CHECK(worst < 2e-5);
			CHECK(late == (mode == 11 ? 1152u : 0u) + (lookahead == 5u ? 240u : 0u));
		}
	}
	// SUB THRU OFF: what the bands hear is all there is, the input high-passed at HP Wet (restated by a bilinear biquad).
	Settings s = combAtRest(220.0, 440.0);
	s.hpWet = hpLog2(800.0);
	s.subThru = false;
	const std::vector<float> out = throughDevice(s, in);
	const std::vector<float> want = butterworthHighpass(in, std::exp2(static_cast<double>(hpLog2(800.0))));
	double worst = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		worst = std::max(worst, std::fabs(static_cast<double>(out[i]) - want[i]));
	}
	MESSAGE("Sub Thru off: worst |out - restated highpass| " << worst);
	CHECK(worst < 1e-3);
	// NOT VACUOUS: the highpass really took something (its low end is half or more of this noise's energy at 800 Hz of 24 kHz? no — a few per cent).
	double diff = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		diff = std::max(diff, std::fabs(static_cast<double>(out[i]) - in[i]));
	}
	CHECK(diff > 0.05);
}

TEST_CASE("HP Wet: the low end does not drive the detectors, and Sub Thru gives it back uncompressed") {
	// A loud 60 Hz and a quiet 3 kHz through LR2 Full at the default corners (200 Hz, 2 kHz): the sub sits in the Low band and compresses it, until HP Wet takes it out.
	const auto run = [](bool hp, bool subThru, std::array<float, 3>& gains, std::vector<float>& out) {
		Settings s = patchDefault();
		s.mode = 6.0f;
		s.keyTrack = false;
		s.ratio = 1.0f;
		s.threshold = 0.2f;
		s.softClip = false;
		s.blend = 1.0f;
		s.hpWet = hp ? hpLog2(400.0) : static_cast<float>(feathers::fx::kWetHighpassLog2Min);
		s.subThru = subThru;
		Owltt o;
		o.prepare(kRate);
		o.setTarget(s);
		out.clear();
		std::vector<float> l(512), r(512);
		size_t n = 0;
		for (int block = 0; block < 120; ++block) {
			for (size_t i = 0; i < l.size(); ++i, ++n) {
				const double t = static_cast<double>(n) / kRate;
				l[i] = r[i] = static_cast<float>(0.8 * std::sin(2.0 * kPi * 60.0 * t) + 0.02 * std::sin(2.0 * kPi * 3000.0 * t));
			}
			o.processStereo(l.data(), r.data(), 512);
			out.insert(out.end(), l.begin(), l.end());
		}
		for (size_t b = 0; b < 3; ++b) {
			gains[b] = o.lastTargetBandGainForTest(b);
		}
	};
	std::array<float, 3> off {}, on {}, thru {};
	std::vector<float> outOff, outOn, outThru;
	run(false, false, off, outOff);
	run(true, false, on, outOn);
	run(true, true, thru, outThru);
	MESSAGE("Low band gain: no filter " << off[0] << ", HP Wet " << on[0] << ", with Sub Thru " << thru[0]);
	CHECK(off[0] < 0.5f);  // the sub drives the Low band's compressor
	// WITH HP WET the Low band hears only what the 2-pole highpass leaves of the 60 Hz (-33 dB at 400 Hz: 0.8 x 0.022), so it is nearly out of compression.
	CHECK(on[0] > 0.5f);
	CHECK(on[0] > 4.0f * off[0]);
	CHECK(thru[0] == on[0]); // Sub Thru changes nothing the detectors hear: the lows are given back after them
	// THE 60 Hz IN THE OUTPUT: whole through Sub Thru (the lows, uncompressed), gone without it.
	const auto sub = [](const std::vector<float>& y) {
		const std::vector<float> tail(y.begin() + 30000, y.end());
		std::complex<double> sum = 0.0;
		for (size_t i = 0; i < tail.size(); ++i) {
			sum += static_cast<double>(tail[i]) * std::polar(1.0, -2.0 * kPi * 60.0 * static_cast<double>(30000 + i) / kRate);
		}
		return 2.0 * std::abs(sum) / static_cast<double>(tail.size());
	};
	MESSAGE("60 Hz amplitude: no filter " << sub(outOff) << ", HP Wet " << sub(outOn) << ", with Sub Thru " << sub(outThru));
	CHECK(sub(outThru) > 0.75);  // 0.8 in, less the 400 Hz highpass's own few per cent
	CHECK(sub(outOn) < 0.1);
	CHECK(sub(outOff) < sub(outThru)); // the compressed sub is quieter than the uncompressed one
}

TEST_CASE("HP Wet: Key Track puts the cutoff the semitones above the played note, as the Eroder's does; off, the hertz rules") {
	feathers::device::NoteContext note;
	note.gate = true;
	Settings s = combAtRest(200.0, 400.0);
	s.keyTrack = true;
	s.hpWetSemis = 12.0f; // an octave above the note
	s.hpWet = hpLog2(512.0);
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	note.pitchSemitones = 57.0f; // A3, 220 Hz
	o.setNoteContext(note);
	o.processStereo(l.data(), r.data(), 512);
	CHECK(o.hpOnForTest());
	CHECK(o.hpHzForTest() == doctest::Approx(440.0).epsilon(1e-6));
	note.pitchSemitones = 69.0f; // A4
	o.setNoteContext(note);
	o.processStereo(l.data(), r.data(), 512);
	CHECK(o.hpHzForTest() == doctest::Approx(880.0).epsilon(1e-6));
	s.hpWetSemis = 0.0f; // the note itself
	o.setTarget(s);
	o.processStereo(l.data(), r.data(), 512);
	CHECK(o.hpHzForTest() == doctest::Approx(440.0).epsilon(1e-6));
	s.hpWetSemis = static_cast<float>(kHpWetSemisMin); // the bottom: no filter
	o.setTarget(s);
	o.processStereo(l.data(), r.data(), 512);
	CHECK_FALSE(o.hpOnForTest());
	s.keyTrack = false; // the hertz knob, whatever the semitones say
	s.hpWetSemis = 12.0f;
	o.setTarget(s);
	o.processStereo(l.data(), r.data(), 512);
	CHECK(o.hpOnForTest());
	CHECK(o.hpHzForTest() == doctest::Approx(512.0).epsilon(1e-6));
	s.hpWet = static_cast<float>(feathers::fx::kWetHighpassLog2Min);
	o.setTarget(s);
	o.processStereo(l.data(), r.data(), 512);
	CHECK_FALSE(o.hpOnForTest());
}

TEST_CASE("HP Wet: with it at the bottom every mode is what it was (a row from before it loads with it off), and nothing allocates with it on") {
	const Row full = rowOf(patchDefault());
	CHECK(kParamDescs[static_cast<size_t>(Param::HpWet)].def == feathers::fx::kWetHighpassLog2Min);
	CHECK(kParamDescs[static_cast<size_t>(Param::SubThru)].def == 0.0);
	const Settings fresh = stateOf(full); // a patch from before reads the new fields at these defaults (the host fills omitted fields from them)
	CHECK(fresh.hpWet == static_cast<float>(feathers::fx::kWetHighpassLog2Min));
	CHECK_FALSE(fresh.subThru);
	// NO ALLOCATION with the filter, Sub Thru, the Linear latency and a lookahead all running, through mode changes and a moving corner.
	Settings s = combAtRest(150.0, 300.0, 11);
	s.hpWet = hpLog2(300.0);
	s.subThru = true;
	s.lookahead = 5;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	std::vector<float> l(512), r(512);
	Noise noise { 55u };
	allocs.allocations = 0;
	allocs.armed = true;
	for (int block = 0; block < 60; ++block) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = noise.next();
			r[i] = noise.next();
		}
		s.hpWet = hpLog2(100.0 + 20.0 * block);
		s.mode = block < 20 ? 11.0f : (block < 40 ? static_cast<float>(kCombMode) : 6.0f);
		o.setTarget(s);
		o.processStereo(l.data(), r.data(), 512);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0u);
}
