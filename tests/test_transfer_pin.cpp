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

// **THE ROUTE TRANSFER CURVES, PINNED TO THE BIT** — written BEFORE the φ curve's evaluation was cut out of the
// vocabulary header (the device SDK's stage 2), so the cut is shown to move code and change no number.
//
// It hashes the exact bits of every curve a route can be read through — every kind, every coefficient step, a sweep
// of inputs that crosses both clamps — plus the φ-triangle curve itself and where each φ step peaks. A hash, not a
// table, because the claim is "identical", and identity of 566k floats is one number. PLATFORM-PINNED: `std::pow`
// and `exp2` are libm's, so the bits are this platform's; elsewhere the case still runs and checks finiteness.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/transfer.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace feathers::device;

namespace {

struct Fnv {
	uint64_t h = 1469598103934665603ull;
	bool finite = true;
	void add(float v) {
		finite = finite && std::isfinite(v);
		const uint32_t b = std::bit_cast<uint32_t>(v);
		for (int i = 0; i < 4; ++i) {
			h ^= (b >> (8 * i)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
};

constexpr int kSweep = 1024; // inputs over [-1.25, 1.25]: both clamps, zero, and both signs

float input(int i) { return -1.25f + 2.5f * static_cast<float>(i) / static_cast<float>(kSweep); }

void report(const char* what, const Fnv& f, int count) {
	if (std::getenv("FEATHERS_TRANSFER_PIN_PRINT") != nullptr) {
		std::printf("%s: 0x%016llxull over %d values\n", what, static_cast<unsigned long long>(f.h), count);
	}
}

} // namespace

#if defined(__x86_64__) && defined(__linux__)
#define PIN(expr) CHECK(expr)
#else
#define PIN(expr) (void)0
#endif

TEST_CASE("every route transfer curve, at every coefficient, is bit-identical to its pinned outputs") {
	Fnv f;
	int count = 0;
	for (size_t k = 0; k < kTransferCount; ++k) {
		const auto kind = static_cast<Transfer>(k);
		const auto [lo, hi] = transferStepRange(kind);
		for (int step = lo; step <= hi; ++step) {
			const Transfer t = transferWithStep(kind, step);
			REQUIRE(transferValid(t));
			for (int i = 0; i <= kSweep; ++i) {
				f.add(shapeTransfer(t, input(i)));
				++count;
			}
		}
	}
	report("shapeTransfer", f, count);
	CHECK(f.finite);
	CHECK(count == 1025 * (4 + 2 * 49 + 225));
	PIN(f.h == 0xe9ab4a78b59f15abull);
}

TEST_CASE("the phi-triangle curve and where each phi step peaks are bit-identical to their pinned outputs") {
	Fnv curve;
	Fnv peaks;
	int count = 0;
	for (int step = 0; step <= kPhiSteps; ++step) {
		const float periods = phiPeriods(phiTransferAtStep(step));
		for (int i = 0; i <= kSweep; ++i) {
			curve.add(phiTriangleCurve(input(i), periods));
			++count;
		}
		peaks.add(phiPeakReading(phiTransferAtStep(step)));
	}
	report("phiTriangleCurve", curve, count);
	report("phiPeakReading", peaks, kPhiSteps + 1);
	CHECK(curve.finite);
	CHECK(peaks.finite);
	PIN(curve.h == 0x38c54c8ba36f4defull);
	PIN(peaks.h == 0x312a21f5f2aefa7bull);
}
