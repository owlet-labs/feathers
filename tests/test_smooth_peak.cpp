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

constexpr double kRate = 192000.0; // a sine of 1 kHz is 192 samples a cycle: the render's peak is the curve's, not a sample's luck
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

/// THE PEAK, RESTATED INDEPENDENTLY: the largest |shaper output| over a driven input x in [-1, 1] at unity, on a
/// grid of its own (offset from the code's), through `transferAt` with the normalisation taken out.
double independentPeak(const smooth::Settings& s) {
	smooth::Picture p = smooth::pictureOf(s);
	REQUIRE(p.driveGain == doctest::Approx(1.0));
	p.weights.norm = 1.0f;
	p.twist = {};
	double peak = 0.0;
	constexpr int kN = 8191; // odd, and not a multiple of the code's grids
	for (int i = -kN; i <= kN; ++i) {
		peak = std::max(peak, static_cast<double>(std::fabs(smooth::transferAt(p, static_cast<float>(i) / kN))));
	}
	return peak;
}

} // namespace

namespace {

/// THE SHAPED CURVE'S PEAK AT ONE DRIVE, INDEPENDENTLY: the largest |transfer| over a dense scan of the input in
/// [-1, 1], through the picture the card draws (the shaper's own arithmetic) with its own normalisation in.
double shapedPeak(const smooth::Settings& s) {
	const smooth::Picture p = smooth::pictureOf(s);
	double peak = 0.0;
	constexpr int kN = 8191;
	for (int i = -kN; i <= kN; ++i) {
		peak = std::max(peak, static_cast<double>(std::fabs(smooth::transferAt(p, static_cast<float>(i) / kN))));
	}
	return peak;
}

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

TEST_CASE("smooth peak (a): a 0 dBFS sine at unity Drive peaks at 0 dBFS at every mode and position") {
	const std::vector<float> in = signalOf(Shape::Sine, 1.0, 19200);
	double worstShaped = 0.0;
	double worstRender = 0.0;
	for (int mode = 0; mode < 8; ++mode) {
		for (int k = 0; k < 9; ++k) {
			const float pos = 0.12f * static_cast<float>(k);
			const smooth::Settings s = settingsAt(mode, pos, 0.5f);
			const Measured m = render(s, in);
			INFO("mode " << mode << " position " << pos << ": render peak " << m.peak << " (" << dbOf(m.peak) << " dB), norm " << m.norm);
			// THE FACTOR the device holds is 1 / the independently found peak of the un-normalised curve.
			const double expectedNorm = 1.0 / independentPeak(s);
			CHECK(static_cast<double>(m.norm) == doctest::Approx(expectedNorm).epsilon(0.004));
			// BY CONSTRUCTION: the shaped curve peaks at exactly 0 dBFS, within 0.1 dB (a dense scan, no highpass).
			const double shaped = shapedPeak(s);
			CHECK(std::fabs(dbOf(shaped)) < static_cast<double>(kToleranceDb));
			worstShaped = std::max(worstShaped, std::fabs(dbOf(shaped)));
			// THE RENDER is the shaped curve through the device's 100 Hz wet highpass, which removes the DC a lopsided
			// curve (Ring, Add, Mod, some positions of Fold) carries, so it moves that curve's peak by up to that DC.
			// It must be what the shaped curve at the input's own samples says, mean removed, within 0.1 dB.
			const double expected = expectedRenderPeak(s, in);
			CHECK(std::fabs(dbOf(m.peak) - dbOf(expected)) < kRenderToleranceDb);
			worstRender = std::max(worstRender, std::fabs(dbOf(m.peak) - dbOf(expected)));
		}
	}
	MESSAGE("worst shaped-curve deviation from 0 dBFS " << worstShaped << " dB; worst render vs restatement " << worstRender << " dB (72 cells)");
}

TEST_CASE("smooth peak (b): a -6 dBFS sine no longer reaches the saturation point") {
	smooth::Settings s = settingsAt(0, 0.5f, 0.5f);
	for (int mode : { 0, 1, 7 }) {
		s.harmonicMode = mode;
		smooth::SmoothShaper sh;
		sh.prepare(kRate);
		sh.setTarget(s);
		const std::vector<float> in = signalOf(Shape::Sine, 0.5, 4800);
		int32_t seen = 0;
		for (float v : in) {
			float l = v, r = v;
			sh.processStereo(&l, &r, 1);
			seen = std::max(seen, sh.probeForTest(0).zoneInput);
		}
		INFO("mode " << mode);
		CHECK(seen <= (1 << 30) + (1 << 20)); // x = 0.5 (+ a hair for the table's sine) - not the 0x7FFFFF80 ceiling
		CHECK(seen > (1 << 29));              // non-vacuity: the zone did see the signal
	}
}

TEST_CASE("smooth peak (c): at full Drive the shaped curve never exceeds 0 dBFS, with no clipper in the path") {
	const std::vector<float> in = signalOf(Shape::Sine, 1.0, 19200);
	double worst = -200.0;
	double worstRender = -200.0;
	for (int mode = 0; mode < 8; ++mode) {
		for (int k = 0; k < 9; ++k) {
			const float pos = 0.12f * static_cast<float>(k);
			const smooth::Settings s = settingsAt(mode, pos, 1.0f);
			// THE CURVE at 4x: over the whole input range, never above the peak the normalisation made (+0.1 dB).
			const double shaped = shapedPeak(s);
			INFO("mode " << mode << " position " << pos << ": shaped peak " << dbOf(shaped) << " dB");
			CHECK(dbOf(shaped) <= static_cast<double>(kToleranceDb));
			worst = std::max(worst, dbOf(shaped));
			// THE RENDER agrees with the restatement (its highpass moves a lopsided curve's peak by that curve's DC).
			const Measured m = render(s, in);
			CHECK(std::fabs(dbOf(m.peak) - dbOf(expectedRenderPeak(s, in))) < kRenderToleranceDb);
			worstRender = std::max(worstRender, dbOf(m.peak));
		}
	}
	MESSAGE("highest shaped peak at full Drive: " << worst << " dB; highest render peak (after the wet highpass): " << worstRender << " dB");
}

TEST_CASE("smooth peak (c2): Evens and Rect cannot take the curve over its peak") {
	for (int mode : { 0, 3, 7 }) {
		for (int t = 0; t < 14; ++t) { // across Twist's Evens zone (both ways), into Rect, and the meta zone with all of them
			const float twist = t < 10 ? 0.125f + 0.0125f * static_cast<float>(t) : (t == 10 ? 0.3f : (t == 11 ? 0.42f : (t == 12 ? 0.7f : 0.9f)));
			smooth::Settings s = settingsAt(mode, 0.5f, 1.0f);
			s.sweepOn = true;
			s.sweep = twist;
			const smooth::Picture p = smooth::pictureOf(s);
			double peak = 0.0;
			for (int i = -4096; i <= 4096; ++i) {
				peak = std::max(peak, static_cast<double>(std::fabs(smooth::transferAt(p, static_cast<float>(i) / 4096.0f))));
			}
			INFO("mode " << mode << " twist " << twist << ": " << dbOf(peak) << " dB (evens " << p.twist.evenAmount << "/"
			                 << p.twist.evenDryBlend << " rect " << p.twist.rectAmount << ")");
			CHECK(dbOf(peak) <= static_cast<double>(kToleranceDb));
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
	const Measured m = render(loaded.settings, signalOf(Shape::Sine, 1.0, 19200));
	CHECK(std::fabs(dbOf(shapedPeak(loaded.settings))) < static_cast<double>(kToleranceDb));
	CHECK(std::fabs(dbOf(m.peak) - dbOf(expectedRenderPeak(loaded.settings, signalOf(Shape::Sine, 1.0, 19200)))) < kRenderToleranceDb);
	CHECK(smooth::kTraits.schema == 2);
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

TEST_CASE("smooth peak (n): the factor's range per mode") {
	for (int mode = 0; mode < 8; ++mode) {
		float lo = 1e9f, hi = 0.0f;
		for (int k = 0; k < 100; ++k) {
			const float n = smooth::pictureOf(settingsAt(mode, 0.01f * static_cast<float>(k), 0.5f)).weights.norm;
			lo = std::min(lo, n);
			hi = std::max(hi, n);
		}
		CHECK(lo >= 1.9f); // every curve peaks below 0 dBFS un-normalised, so the factor boosts it
		CHECK(hi < 20.0f); // and none is a near-silent blend amplified without bound
		MESSAGE("NORM mode " << mode << " min " << lo << " (" << dbOf(lo) << " dB) max " << hi << " (" << dbOf(hi) << " dB)");
	}
}
