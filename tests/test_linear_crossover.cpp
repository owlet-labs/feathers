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

#include "dsp/linear_crossover.h"
#include "portable_aligned_alloc.h"
#include "test_heap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <functional>
#include <cstddef>
#include <new>
#include <numbers>
#include <vector>

// THE ALLOCATION PROBE, counting only while armed. It sees `operator new`; the pffft buffers come from `malloc` and
// are all taken in `prepare`, which is not armed (read from the code: `LinearFirBank` allocates nowhere else).
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

using namespace feathers::dsp;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = std::numbers::pi;

/// A three-band split: lowpass at `a`, lowpass at `b`. The bands are LP_a, LP_b - LP_a and delayed - LP_b.
struct Split3 {
	LinearFirBank bank;
	std::array<LinearBand, 2> bands;
	explicit Split3(double a, double b, double rate = kRate) : bands { LinearBand { 0.0, a }, LinearBand { 0.0, b } } {
		bank.prepare(rate, 2);
		bank.buildNow(bands.data());
	}
	/// One sample's three bands (left channel).
	std::array<float, 3> run(float x) {
		LinearFirBank::Frame f;
		bank.process(x, 0.5f * x, f);
		return { f.out[0][0], f.out[1][0] - f.out[0][0], f.delayed[0] - f.out[1][0] };
	}
};

/// The impulse response of each band, `taps` samples long.
std::array<std::vector<double>, 3> impulse(Split3& s, size_t taps) {
	std::array<std::vector<double>, 3> ir;
	for (size_t i = 0; i < taps; ++i) {
		const auto b = s.run(i == 0 ? 1.0f : 0.0f);
		for (size_t k = 0; k < 3; ++k) {
			ir[k].push_back(b[k]);
		}
	}
	return ir;
}

/// THE DTFT of an impulse response at `hz`, by direct summation: an independent restatement of the response.
std::complex<double> dtft(const std::vector<double>& h, double hz, double rate = kRate) {
	std::complex<double> sum = 0.0;
	const double w = 2.0 * kPi * hz / rate;
	for (size_t n = 0; n < h.size(); ++n) {
		sum += h[n] * std::polar(1.0, -w * static_cast<double>(n));
	}
	return sum;
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1.0e-12)); }

constexpr size_t kIr = 4096;

} // namespace

TEST_CASE("linear crossover: the bands sum to a pure delay of `latency` - flat to 0.05 dB from 20 Hz to 20 kHz, linear phase") {
	Split3 s(200.0, 2000.0);
	const auto ir = impulse(s, kIr);
	std::vector<double> sum(kIr);
	for (size_t i = 0; i < kIr; ++i) {
		sum[i] = ir[0][i] + ir[1][i] + ir[2][i];
	}
	const uint32_t latency = s.bank.latency();
	CHECK(latency == 1152);
	CHECK(latency == linearCrossoverLatency(kRate));
	for (double hz : { 20.0, 31.0, 55.0, 100.0, 200.0, 440.0, 1000.0, 2000.0, 5000.0, 9000.0, 14000.0, 20000.0 }) {
		CAPTURE(hz);
		const auto h = dtft(sum, hz);
		CHECK(std::fabs(db(std::abs(h))) < 0.05);
		// LINEAR PHASE: H(w) e^{+jw L} has no phase left, whatever w.
		const double w = 2.0 * kPi * hz / kRate;
		const auto residual = h * std::polar(1.0, w * static_cast<double>(latency));
		CHECK(std::fabs(std::arg(residual)) < 0.002);
	}
}

TEST_CASE("linear crossover: the reported latency is the measured impulse delay, at every rate") {
	for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
		CAPTURE(rate);
		Split3 s(200.0, 2000.0, rate);
		const size_t len = static_cast<size_t>(linearCrossoverLatency(rate)) + 64;
		size_t peakAt = 0;
		double peak = 0.0, sumRest = 0.0;
		for (size_t i = 0; i < len; ++i) {
			const auto b = s.run(i == 0 ? 1.0f : 0.0f);
			const double v = static_cast<double>(b[0]) + b[1] + b[2];
			if (std::fabs(v) > peak) {
				peak = std::fabs(v);
				peakAt = i;
			}
		}
		(void)sumRest;
		CHECK(peakAt == linearCrossoverLatency(rate));
		CHECK(peakAt == s.bank.latency());
		CHECK(peak == doctest::Approx(1.0).epsilon(1.0e-6));
	}
}

TEST_CASE("linear crossover: each band's passband, corner and stopband, and no DC outside the lowest band") {
	Split3 s(200.0, 2000.0);
	const auto ir = impulse(s, kIr);
	const auto at = [&](size_t band, double hz) { return db(std::abs(dtft(ir[band], hz))); };
	// PASSBANDS within 0.1 dB, an octave inside the edges.
	CHECK(std::fabs(at(0, 50.0)) < 0.1);
	CHECK(std::fabs(at(1, 632.0)) < 0.1);
	CHECK(std::fabs(at(2, 8000.0)) < 0.1);
	// THE CORNER: -6.02 dB on each side of a symmetric windowed-sinc complementary pair (2 kHz, clear of the transition).
	{
		Split3 t(2000.0, 8000.0);
		const auto tir = impulse(t, kIr);
		CHECK(std::fabs(db(std::abs(dtft(tir[0], 2000.0))) + 6.0206) < 0.1);
	}
	// STOPBANDS an octave away from each edge: at least 75 dB down (measured 82 to 111).
	CHECK(at(0, 400.0) < -75.0);
	CHECK(at(0, 4000.0) < -75.0);
	CHECK(at(1, 100.0) < -75.0);
	CHECK(at(1, 4000.0) < -75.0);
	CHECK(at(2, 1000.0) < -75.0);
	CHECK(at(2, 100.0) < -75.0);
	// No DC leaks above the lowest band: the lowpasses all have unit DC gain, so the bands above it sum to zero there.
	CHECK(at(1, 0.0) < -100.0);
	CHECK(at(2, 0.0) < -100.0);
	CHECK(std::fabs(at(0, 0.0)) < 1e-4);
}

TEST_CASE("linear crossover: a zero input leaves exact zeros after the kernel's tail, and quiet() says so") {
	Split3 s(200.0, 2000.0);
	LinearFirBank::Frame f;
	std::vector<float> noise(3000);
	uint32_t seed = 1;
	for (float& v : noise) {
		seed = seed * 1664525u + 1013904223u;
		v = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
	}
	for (float v : noise) {
		s.bank.process(v, v, f);
	}
	CHECK_FALSE(s.bank.quiet());
	const size_t tail = s.bank.taps() + 5 * s.bank.blockLength();
	bool allZeroAfter = true;
	bool quietEarly = false;
	for (size_t i = 0; i < tail + 2000; ++i) {
		s.bank.process(0.0f, 0.0f, f);
		if (i >= tail) {
			allZeroAfter = allZeroAfter && f.out[0][0] == 0.0f && f.out[1][1] == 0.0f && f.delayed[0] == 0.0f && f.out[1][0] == 0.0f;
			CHECK(s.bank.quiet());
		}
		else if (i < s.bank.latency()) {
			quietEarly = quietEarly || s.bank.quiet();
		}
	}
	CHECK(allZeroAfter);
	CHECK_FALSE(quietEarly);
	// A new input wakes it.
	s.bank.process(1.0f, 1.0f, f);
	for (size_t i = 0; i < s.bank.blockLength(); ++i) {
		s.bank.process(1.0f, 1.0f, f);
	}
	CHECK_FALSE(s.bank.quiet());
}

namespace {

/// The worst |band - reference| of a steady sine `hz` through a bank whose corners are moved by `move(n)` each control
/// step, over `seconds`. `band` picks the band read; the reference is the sine delayed by the latency.
double sweepError(double hz, int bandOf, const std::function<std::array<LinearBand, 2>(size_t)>& move, double seconds,
                  size_t* allocations = nullptr, int* swaps = nullptr) {
	Split3 s(200.0, 2000.0);
	const auto total = static_cast<size_t>(seconds * kRate);
	const size_t latency = s.bank.latency();
	double worst = 0.0;
	if (allocations != nullptr) {
		allocs.allocations = 0;
		allocs.armed = true;
	}
	for (size_t n = 0; n < total; ++n) {
		if (n % 32 == 0) {
			const auto bands = move(n);
			s.bank.setTargets(bands.data());
		}
		const double x = std::sin(2.0 * kPi * hz * static_cast<double>(n) / kRate);
		LinearFirBank::Frame f;
		s.bank.process(static_cast<float>(x), 0.0f, f);
		if (n > 2 * latency) {
			const double band = bandOf == 0 ? f.out[0][0] : bandOf == 1 ? f.out[1][0] - f.out[0][0] : f.delayed[0] - f.out[1][0];
			const double ref = std::sin(2.0 * kPi * hz * static_cast<double>(n - latency) / kRate);
			worst = std::max(worst, std::fabs(band - ref));
		}
	}
	if (allocations != nullptr) {
		allocs.armed = false;
		*allocations = allocs.allocations;
	}
	if (swaps != nullptr) {
		*swaps = s.bank.swapsForTest();
	}
	return worst;
}

} // namespace

TEST_CASE("linear crossover: corners swept through a note's range allocate nothing, rebuild and swap, and a sine inside a band stays clean") {
	// The corners glide 100 -> 800 Hz and 1000 -> 1600 Hz over a second, back, forth; a 3 kHz sine sits in the top band all along.
	const auto glide = [](size_t n) {
		const double t = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / kRate * 1.5);
		return std::array<LinearBand, 2> { LinearBand { 0.0, 100.0 + 700.0 * t }, LinearBand { 0.0, 1000.0 + 600.0 * t } };
	};
	size_t allocations = 99;
	int swaps = 0;
	const double err = sweepError(3000.0, 2, glide, 3.0, &allocations, &swaps);
	CHECK(allocations == 0);
	CHECK(swaps > 20); // not vacuous: kernels really were rebuilt and swapped
	CHECK(err < 5.0e-4);
	// A 60 Hz sine in the low band all along, corners 250 -> 950 Hz (a kernel 90 Hz wide cannot hold 60 Hz under a 100 Hz corner): likewise.
	const auto glideHigh = [&glide](size_t n) {
		auto b = glide(n);
		b[0].hiHz += 150.0;
		b[1].hiHz += 150.0;
		return b;
	};
	const double errLow = sweepError(60.0, 0, glideHigh, 3.0);
	CHECK(errLow < 5.0e-4);
}

TEST_CASE("linear crossover: a corner that jumps crossfades its kernels, it does not step") {
	// 1 kHz sine; the low corner jumps 500 -> 2000 Hz, so band 0 goes from stopband to passband. With the crossfade the band
	// moves at most 1/B a sample on top of the sine's own slope (2 pi 1000/48000 = 0.13); a hard swap would jump by the sine's
	// value at the block edge it lands on, so the sine's phase is stepped through eight values to put that edge anywhere on it.
	double worstOfAll = 0.0;
	for (int phase = 0; phase < 8; ++phase) {
		Split3 s(500.0, 8000.0);
		double worstStep = 0.0, prev = 0.0;
		for (size_t n = 0; n < 24000; ++n) {
			if (n >= 8000) {
				// keep asking: the call is dropped while a build is in flight, and once the corner is there it is a no-op
				const std::array<LinearBand, 2> to { LinearBand { 0.0, 2000.0 }, LinearBand { 0.0, 8000.0 } };
				s.bank.setTargets(to.data());
			}
			LinearFirBank::Frame f;
			const double x = std::sin(2.0 * kPi * (1000.0 * static_cast<double>(n) / kRate + phase / 8.0));
			s.bank.process(static_cast<float>(x), 0.0f, f);
			if (n > 3000) {
				worstStep = std::max(worstStep, std::fabs(static_cast<double>(f.out[0][0]) - prev));
			}
			prev = f.out[0][0];
		}
		CHECK(s.bank.swapsForTest() == 1);
		CHECK(s.bank.liveBandForTest(0).hiHz == 2000.0);
		worstOfAll = std::max(worstOfAll, worstStep);
	}
	CHECK(worstOfAll < 0.2);
	CHECK(worstOfAll > 0.05); // the sine's own slope is in it: the measurement could have shown a step
}
