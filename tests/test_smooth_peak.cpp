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

// THE SMOOTH SHAPER'S LEVEL LAW: the knee is 0 dBFS and the shaped curve is peak-normalised, so what the shaper shapes
// peaks at exactly 0 dBFS when driven to the knee, with nothing after it. Every case asserts a value (the factor
// through a test seam, an independent restatement of the peak, the render's peak against both).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "fx/smooth/smooth.h"
#include "fx/smooth/smooth_types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

//--------------------------------------------------------------------------------------------------//
// THE ALLOCATION COUNTER — a replacement operator new for THIS BINARY ONLY, counting inside a window. //
//--------------------------------------------------------------------------------------------------//
namespace {
std::atomic<bool> g_countAllocs { false };
std::atomic<uint64_t> g_allocs { 0 };
void noteAlloc() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	noteAlloc();
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

namespace smooth = feathers::fx::smooth;

namespace {

constexpr double kRate = 768000.0; // a sine of 1 kHz is 768 samples a cycle: the render's peak is the curve's, not a sample's luck
constexpr double kHz = 1000.0;
constexpr float kToleranceDb = 0.1f;
/// The render against its restatement: the highpass is written out in double, the device runs it in q31.
constexpr double kRenderToleranceDb = 0.2;

smooth::Settings settingsAt(int mode, float position, float driveFader) {
	smooth::Settings s;
	s.harmonicMode = mode;
	s.harmonic = position;
	s.drive = driveFader;
	return s;
}

double dbOf(double v) {
	return 20.0 * std::log10(std::max(v, 1.0e-12));
}

/// THE INPUT: a sine of `amplitude` (or a saw, or noise), `n` frames, interleaved as one mono signal.
enum class Shape { Sine, Saw, Pink };
std::vector<float> signalOf(Shape shape, double amplitude, int n) {
	std::vector<float> x(static_cast<size_t>(n));
	uint32_t seed = 12345u;
	double b0 = 0.0, b1 = 0.0, b2 = 0.0;
	for (int i = 0; i < n; ++i) {
		const double ph = std::fmod(kHz * static_cast<double>(i) / kRate, 1.0);
		double v = 0.0;
		switch (shape) {
		case Shape::Sine: v = std::sin(2.0 * M_PI * ph); break;
		case Shape::Saw: v = 2.0 * ph - 1.0; break;
		case Shape::Pink: {
			seed = seed * 1664525u + 1013904223u;
			const double white = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
			b0 = 0.99765 * b0 + white * 0.0990460;
			b1 = 0.96300 * b1 + white * 0.2965164;
			b2 = 0.57000 * b2 + white * 1.0526913;
			v = (b0 + b1 + b2 + white * 0.1848) * 0.2;
			break;
		}
		}
		x[static_cast<size_t>(i)] = static_cast<float>(amplitude * v);
	}
	return x;
}

struct Measured {
	double peak = 0.0;
	double rms = 0.0;
	smooth::SmoothShaper::CoefficientSet coefficients {};
	float norm = 1.0f;
};

/// Render through a fresh shaper (stereo, both channels the same) and measure the second half: past the first blocks.
Measured render(const smooth::Settings& s, const std::vector<float>& in, smooth::SmoothShaper* keep = nullptr) {
	smooth::SmoothShaper local;
	smooth::SmoothShaper& sh = keep != nullptr ? *keep : local;
	sh.prepare(kRate);
	sh.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	sh.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	Measured m;
	const size_t from = l.size() / 2;
	double sum = 0.0;
	for (size_t i = from; i < l.size(); ++i) {
		m.peak = std::max(m.peak, static_cast<double>(std::fabs(l[i])));
		sum += static_cast<double>(l[i]) * static_cast<double>(l[i]);
	}
	m.rms = std::sqrt(sum / static_cast<double>(l.size() - from));
	m.coefficients = sh.coefficientsForTest();
	m.norm = sh.peakNormForTest(0);
	return m;
}

} // namespace

namespace {

/// WHAT THE RENDER'S PEAK MUST BE, RESTATED: the transfer at the input's own samples, through the wet highpass written
/// out in double (a one-pole lowpass subtracted, its step the device's `alpha / 2^31`), the largest |sample| of the
/// second half. The highpass is what moves a lopsided curve's peak by that curve's DC.
double expectedRenderPeak(const smooth::Settings& s, const std::vector<float>& in) {
	const smooth::Picture p = smooth::pictureOf(s);
	smooth::SmoothShaper probe;
	probe.prepare(kRate);
	const double step = static_cast<double>(probe.hpfAlphaForTest()) / 2147483648.0; // twice(mulhi(d, alpha)) = d * alpha / 2^31
	double lp = 0.0;
	double peak = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		const double x = static_cast<double>(smooth::transferAt(p, in[i]));
		lp += (x - lp) * step;
		if (i >= in.size() / 2) {
			peak = std::max(peak, std::fabs(x - lp));
		}
	}
	return peak;
}

} // namespace

TEST_CASE("smooth peak (a'): a 0 dBFS sine peaks at 0 dBFS after the wet highpass, at unity Drive and at +12 dB, in every mode and position") {
	const std::vector<float> in = signalOf(Shape::Sine, 1.0, 76800);
	double worst = 0.0;
	double worstExpected = 0.0;
	for (float fader : { 0.5f, 1.0f }) { // unity, and +12 dB: driven into the hold
		for (int mode = 0; mode < 8; ++mode) {
			for (int k = 0; k < 9; ++k) {
				const float pos = 0.12f * static_cast<float>(k);
				const smooth::Settings s = settingsAt(mode, pos, fader);
				const Measured m = render(s, in);
				INFO("fader " << fader << " mode " << mode << " position " << pos << ": render peak " << m.peak << " ("
				              << dbOf(m.peak) << " dB), norm " << m.norm);
				CHECK(std::fabs(dbOf(m.peak)) < kRenderToleranceDb);
				worst = std::max(worst, std::fabs(dbOf(m.peak)));
				// AND THE RESTATEMENT, independent of the factor's own scan: the held curve at the input's own samples through the
				// highpass written out in double.
				const double expected = expectedRenderPeak(s, in);
				CHECK(std::fabs(dbOf(expected)) < kRenderToleranceDb);
				CHECK(std::fabs(dbOf(m.peak) - dbOf(expected)) < kRenderToleranceDb);
				worstExpected = std::max(worstExpected, std::fabs(dbOf(expected)));
			}
		}
	}
	MESSAGE("worst render deviation from 0 dBFS " << worst << " dB; worst restated " << worstExpected << " dB (144 cells)");
}

TEST_CASE("smooth peak (dc): the output AFTER the wet highpass never goes over 0 dBFS, lopsided curves included") {
	const std::vector<float> in = signalOf(Shape::Sine, 1.0, 76800);
	double highest = -200.0;
	int lopsided = 0;
	for (float fader : { 0.5f, 1.0f }) {
		for (int mode = 0; mode < 8; ++mode) {
			for (int k = 0; k < 9; ++k) {
				const smooth::Settings s = settingsAt(mode, 0.12f * static_cast<float>(k), fader);
				const Measured m = render(s, in);
				INFO("fader " << fader << " mode " << mode << " k " << k << ": " << dbOf(m.peak) << " dB");
				CHECK(dbOf(m.peak) <= static_cast<double>(kToleranceDb));
				highest = std::max(highest, dbOf(m.peak));
				// NON-VACUITY: count the curves that carry a DC the highpass has to take off (the ones where it matters).
				smooth::Picture p = smooth::pictureOf(s);
				double sum = 0.0;
				for (int i = 0; i < 512; ++i) {
					sum += static_cast<double>(smooth::transferAt(p, static_cast<float>(std::sin(2.0 * M_PI * (i + 0.5) / 512.0))));
				}
				if (std::fabs(sum / 512.0) > 0.02) {
					++lopsided;
				}
			}
		}
	}
	CHECK(lopsided >= 10);
	MESSAGE("highest peak after the highpass " << highest << " dB; " << lopsided << " of 144 cells are lopsided (DC > 2%)");
}

TEST_CASE("smooth peak (b): the hold is where the curve peaks - below it the input passes, past it the output holds") {
	for (int mode : { 0, 1, 7 }) {
		for (float pos : { 0.12f, 0.36f, 0.6f }) {
			const smooth::Settings s = settingsAt(mode, pos, 0.5f);
			const smooth::Picture p = smooth::pictureOf(s);
			INFO("mode " << mode << " position " << pos << ": hold " << p.weights.holdPos << " / " << p.weights.holdNeg);
			REQUIRE(p.weights.holdPos > 0.01f);
			REQUIRE(p.weights.holdPos <= 1.0f);
			const auto zoneInputAt = [&](double level) {
				smooth::SmoothShaper sh;
				sh.prepare(kRate);
				sh.setTarget(s);
				int32_t seen = 0;
				for (int i = 0; i < 4096; ++i) {
					float l = static_cast<float>(level * std::sin(2.0 * M_PI * i / 768.0)), r = l;
					sh.processStereo(&l, &r, 1);
					seen = std::max(seen, sh.probeForTest(0).zoneInput);
				}
				return seen;
			};
			const double ceiling = static_cast<double>(static_cast<int32_t>(p.weights.holdPos * 16777216.0f)) * 128.0;
			// A SINE A QUARTER UNDER THE HOLD never reaches it; one a quarter over it sits at it. (Zone 7's pass-through is
			// the driven input itself, scaled <<7; the Chebyshevs' likewise.)
			const int32_t under = zoneInputAt(0.75 * static_cast<double>(p.weights.holdPos));
			const int32_t over = zoneInputAt(1.25 * static_cast<double>(p.weights.holdPos));
			CHECK(static_cast<double>(under) < 0.8 * ceiling);
			CHECK(static_cast<double>(under) > 0.5 * ceiling); // non-vacuity: the signal got there
			if (p.weights.holdPos < 0.8f) {
				CHECK(static_cast<double>(over) == doctest::Approx(ceiling).epsilon(1e-4));
			}
		}
	}
}

TEST_CASE("smooth peak (c): driven past the hold the curve stays at its peak - by the curve's own shape, with no clipper in the path") {
	for (int mode : { 0, 1, 7 }) {
		for (int k = 0; k < 9; ++k) {
			const smooth::Settings s = settingsAt(mode, 0.12f * static_cast<float>(k), 1.0f); // +12 dB
			const smooth::Picture p = smooth::pictureOf(s);
			INFO("mode " << mode << " k " << k << ": hold " << p.weights.holdPos << " / " << p.weights.holdNeg);
			// PAST THE HOLD the output is the value at the hold, on each side, for every input up to full scale and beyond.
			const float atHoldPos = smooth::transferAt(p, p.weights.holdPos);
			const float atHoldNeg = smooth::transferAt(p, -p.weights.holdNeg);
			for (float x : { 1.0f, 1.5f, 4.0f }) {
				CHECK(smooth::transferAt(p, x) == atHoldPos);
				CHECK(smooth::transferAt(p, -x) == atHoldNeg);
			}
			// AND THE HELD LEVEL IS WITHIN kHoldWithinDb OF THE PEAK: nothing in between is larger than that (a dense scan).
			double peak = 0.0;
			for (int i = -4096; i <= 4096; ++i) {
				peak = std::max(peak, static_cast<double>(std::fabs(smooth::transferAt(p, static_cast<float>(i) / 4096.0f))));
			}
			CHECK(peak <= static_cast<double>(std::max(std::fabs(atHoldPos), std::fabs(atHoldNeg))) * std::pow(10.0, smooth::kHoldWithinDb / 20.0) * 1.0005);
		}
	}
}

TEST_CASE("smooth peak (c2): Evens and Rect cannot take the curve over its peak") {
	for (int mode : { 0, 3, 7 }) {
		for (int t = 0; t < 14; ++t) { // across Twist's Evens zone (both ways), into Rect, and the meta zone with all of them
			const float twist = t < 10 ? 0.125f + 0.0125f * static_cast<float>(t) : (t == 10 ? 0.3f : (t == 11 ? 0.42f : (t == 12 ? 0.7f : 0.9f)));
			smooth::Settings s = settingsAt(mode, 0.5f, 1.0f);
			s.sweepOn = true;
			s.sweep = twist;
			const smooth::Picture p = smooth::pictureOf(s);
			smooth::Picture plain = p; // the same blend and level law, with the modifiers off
			plain.twist = {};
			double peak = 0.0;
			double base = 0.0;
			for (int i = -4096; i <= 4096; ++i) {
				peak = std::max(peak, static_cast<double>(std::fabs(smooth::transferAt(p, static_cast<float>(i) / 4096.0f))));
				base = std::max(base, static_cast<double>(std::fabs(smooth::transferAt(plain, static_cast<float>(i) / 4096.0f))));
			}
			INFO("mode " << mode << " twist " << twist << ": " << dbOf(peak) << " dB against the plain curve's " << dbOf(base));
			CHECK(peak <= std::max(base, 1.0) * 1.001); // the dry blend is capped at 0 dBFS: never past the unmodified peak or 1.0
		}
	}
}

TEST_CASE("smooth peak (d): an old patch loads and follows the new rule") {
	// A patch from before the law changed is the same row of numbers: the law is the device's, so the stored values load
	// as they always did and sound by the new rule (schema 2 says so in the ledger).
	smooth::State old;
	old.settings.harmonicMode = 4;
	old.settings.harmonic = 0.3f;
	const smooth::Row row = smooth::rowOf(old);
	const smooth::State loaded = smooth::stateOf(row, row.size() - 2); // an older, shorter row
	const std::vector<float> in = signalOf(Shape::Sine, 1.0, 76800);
	const Measured m = render(loaded.settings, in);
	CHECK(std::fabs(dbOf(m.peak)) < kRenderToleranceDb);
	CHECK(smooth::kTraits.schema == 2);
}

TEST_CASE("smooth peak (loop): the Twist feedback tap is scaled by 1 / the peak factor") {
	// THE FACTOR LIFTS EVERY CURVE TO 0 dBFS, which would raise the feedback loop's gain by the same amount; the tap gives it
	// back, so the loop keeps the calibration the curves had before they were normalised. Restated here from Twist's own
	// feedback amount, the zone's scale and the drive's, and compared with the amount the block runs.
	for (int mode : { 0, 2, 3, 5, 6 }) {
		for (float drive : { 0.5f, 0.75f }) {
			smooth::Settings s = settingsAt(mode, 0.6f, drive);
			s.sweepOn = false;
			s.pillOn[static_cast<size_t>(smooth::Pill::Fdbk)] = true; // Twist's feedback pill, up
			s.pillAmount[static_cast<size_t>(smooth::Pill::Fdbk)] = 0.9f;
			smooth::SmoothShaper sh;
			sh.prepare(kRate);
			sh.setTarget(s);
			std::vector<float> l(256, 0.1f), r(256, 0.1f);
			sh.processStereo(l.data(), r.data(), 256);
			const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
			const smooth::TwistParams twist = smooth::twistOf(fc);
			REQUIRE(twist.feedbackAmount > 0.01f);
			const double driveNorm = static_cast<double>(fc.drive) / 2147483648.0;
			const double meanNorm = 0.5 * (static_cast<double>(sh.levelTargetForTest(0).norm) + static_cast<double>(sh.levelTargetForTest(1).norm));
			const double expected = static_cast<double>(twist.feedbackAmount) * 0.25 * (1.0 - 0.1 * driveNorm) / meanNorm * 2147483647.0;
			INFO("mode " << mode << " drive " << drive << ": norm " << meanNorm);
			CHECK(static_cast<double>(sh.feedbackAmountForTest()) == doctest::Approx(expected).epsilon(2e-3));
			CHECK(meanNorm > 1.5); // non-vacuity: the scaling is not 1
		}
	}
}

TEST_CASE("smooth peak (e): no allocation over blocks run while sweeping position") {
	smooth::SmoothShaper sh;
	sh.prepare(kRate);
	smooth::Settings s = settingsAt(3, 0.0f, 0.5f);
	sh.setTarget(s);
	std::vector<float> l = signalOf(Shape::Sine, 1.0, 256);
	std::vector<float> r = l;
	sh.processStereo(l.data(), r.data(), 256); // placed
	const std::vector<float> src = signalOf(Shape::Sine, 1.0, 256);
	g_allocs.store(0);
	g_countAllocs.store(true);
	for (int mode = 0; mode < 8; ++mode) {
		s.harmonicMode = mode;
		for (int b = 0; b < 200; ++b) {
			s.harmonic = 0.005f * static_cast<float>(b); // the position moves every block: the peak is found every block
			sh.setTarget(s);
			std::copy(src.begin(), src.end(), l.begin());
			std::copy(src.begin(), src.end(), r.begin());
			sh.processStereo(l.data(), r.data(), 256);
		}
	}
	g_countAllocs.store(false);

	CHECK(g_allocs.load() == 0u);
}

TEST_CASE("smooth peak (g): the level table") {
	struct Row { const char* name; Shape shape; double amp; };
	const Row rows[] = { { "sine 0 dBFS", Shape::Sine, 1.0 },
	                     { "sine -12 dBFS", Shape::Sine, 0.2512 },
	                     { "saw -6 dBFS", Shape::Saw, 0.5012 },
	                     { "pink -12 dBFS", Shape::Pink, 0.2512 } };
	for (float fader : { 0.5f, 1.0f }) {
		for (const Row& row : rows) {
			const std::vector<float> in = signalOf(row.shape, row.amp, 19200);
			std::vector<double> db;
			for (int mode = 0; mode < 8; ++mode) {
				for (int k = 0; k < 9; ++k) {
					const Measured m = render(settingsAt(mode, 0.12f * static_cast<float>(k), fader), in);
					db.push_back(dbOf(m.rms));
				}
			}
			std::sort(db.begin(), db.end());
			MESSAGE("LEVEL drive " << (fader == 0.5f ? "unity" : "+12dB") << " " << row.name << ": rms dB min " << db.front()
			                       << " median " << db[db.size() / 2] << " max " << db.back());
		}
	}
}

TEST_CASE("smooth peak (n): the factor's and the holds' range per mode") {
	for (int mode = 0; mode < 8; ++mode) {
		std::vector<float> holds;
		float lo = 1e9f, hi = 0.0f;
		for (int k = 0; k < 100; ++k) {
			const smooth::Picture p = smooth::pictureOf(settingsAt(mode, 0.01f * static_cast<float>(k), 0.5f));
			lo = std::min(lo, p.weights.norm);
			hi = std::max(hi, p.weights.norm);
			holds.push_back(std::min(p.weights.holdPos, p.weights.holdNeg));
		}
		std::sort(holds.begin(), holds.end());
		CHECK(lo >= 1.2f); // every curve peaks below 0 dBFS un-normalised, so the factor boosts it
		CHECK(hi < 20.0f); // and none is a near-silent blend amplified without bound
		MESSAGE("NORM mode " << mode << " min " << lo << " (" << dbOf(lo) << " dB) max " << hi << " (" << dbOf(hi) << " dB); hold min "
		                     << holds.front() << " median " << holds[holds.size() / 2] << " max " << holds.back());
	}
}

TEST_CASE("smooth peak (late): how many of the 72 cells pass a -6 dBFS sine unclipped") {
	int passes = 0;
	int saturating = 0;
	for (int mode = 0; mode < 8; ++mode) {
		for (int k = 0; k < 9; ++k) {
			const smooth::Picture p = smooth::pictureOf(settingsAt(mode, 0.12f * static_cast<float>(k), 0.5f));
			const bool saturates = mode == 0 || mode == 1 || mode == 7;
			saturating += saturates ? 1 : 0;
			// A -6.02 dBFS sine (amplitude 0.5012) is unclipped when it stays below both holds (FM zones never clip).
			if (!saturates || (p.weights.holdPos > 0.5012f && p.weights.holdNeg > 0.5012f)) {
				++passes;
			}
		}
	}
	MESSAGE("LATE -6 dBFS passes unclipped in " << passes << " of 72 cells (" << saturating << " saturating cells, " << passes - (72 - saturating) << " of them pass)");
	CHECK(passes > 0);
}

TEST_CASE("smooth peak (outermost): the hold is the outermost local maximum within kHoldWithinDb of the side's global maximum") {
	int later = 0; // cells where the outermost differs from the global maximum's place: the non-vacuity of the rule
	for (int mode : { 0, 1 }) {
		for (int k = 0; k < 9; ++k) {
			const smooth::Settings s = settingsAt(mode, 0.12f * static_cast<float>(k), 0.5f);
			smooth::Picture p = smooth::pictureOf(s);
			const float holdPos = p.weights.holdPos;
			p.weights.holdPos = p.weights.holdNeg = 1.0f; // the whole curve, to scan it independently
			constexpr int kN = 8192;
			std::vector<double> v(kN + 1);
			double global = 0.0;
			int atGlobal = 0;
			for (int i = 0; i <= kN; ++i) {
				v[static_cast<size_t>(i)] = std::fabs(static_cast<double>(smooth::transferAt(p, static_cast<float>(i) / kN)));
				if (v[static_cast<size_t>(i)] > global) {
					global = v[static_cast<size_t>(i)];
					atGlobal = i;
				}
			}
			const double floorValue = global * std::pow(10.0, -smooth::kHoldWithinDb / 20.0);
			int outermost = 0;
			for (int i = 1; i < kN; ++i) {
				const double x = v[static_cast<size_t>(i)];
				if (x >= v[static_cast<size_t>(i - 1)] && x >= v[static_cast<size_t>(i + 1)] && x >= floorValue) {
					outermost = i;
				}
			}
			if (v[kN] >= v[kN - 1] && v[kN] >= floorValue) {
				outermost = kN;
			}
			INFO("mode " << mode << " k " << k << ": hold " << holdPos << ", outermost " << static_cast<double>(outermost) / kN << ", global at "
			             << static_cast<double>(atGlobal) / kN);
			CHECK(static_cast<double>(holdPos) == doctest::Approx(static_cast<double>(outermost) / kN).epsilon(0.01).scale(1.0));
			CHECK(std::fabs(static_cast<double>(holdPos) - static_cast<double>(outermost) / kN) < 0.004);
			// THE HELD LEVEL stays within kHoldWithinDb of the global maximum.
			CHECK(dbOf(v[static_cast<size_t>(std::lround(static_cast<double>(holdPos) * kN))]) >= dbOf(global) - smooth::kHoldWithinDb - 0.05);
			later += (outermost - atGlobal > kN / 20) ? 1 : 0;
		}
	}
	CHECK(later >= 3);
	MESSAGE("cells (of 18) where the hold sits later than the global maximum: " << later);
}
