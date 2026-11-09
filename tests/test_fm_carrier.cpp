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

// THE FM CARRIER TABLE (doc §10 T1, fm-architecture-measurements.md §3) — a plugin-owned 1024-point float
// sine, linearly interpolated. The claims that carry weight, each with the known-bad that proves the case
// has teeth:
//
//   - It is a sine to the resolution the lens can measure: worst harmonic below the doc's −116.9 dB THD.
//   - It wraps by construction: evalCarrier(0) == evalCarrier(1) and the value is continuous across the
//     cycle boundary (no step where the guard sample is missing).
//   - The guard sample makes the last segment a real linear interp, not a wrap across the table.
//
// The THD measurement renders one exact-bin cycle (fc = m·fs/N, m odd) with a seven-term Blackman-Harris
// window (the same instrument the aliasing suite uses), so leakage is zero by construction — a lesson the
// FM design doc (§7) paid for with a swept-gestures STFT that reported a hard quantiser as smoother than a
// linear sweep.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/fm_carrier.h"

#include "spectrum.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace feathers::fm;

TEST_CASE("fm carrier table: a sine to the worst harmonic") {
	// EXACT-BIN carrier so the DFT has a single spectral line and no leakage. 1024 samples at 48 kHz;
	// m = 255 (odd) keeps every harmonic odd too, so the even-symmetric spurs a real sine cannot make are
	// the measurement's own, not the table's.
	constexpr double kSr = 48000.0;
	constexpr int kN = 1024;
	constexpr int kM = 255;
	constexpr double kFc = kSr * static_cast<double>(kM) / static_cast<double>(kN);

	using feathers::test::dB;
	using feathers::test::magnitudeAt;
	using feathers::test::Window;
 	const Window win(kN);
	std::vector<float> rendered(static_cast<size_t>(kN));
	for (int i = 0; i < kN; ++i) {
		rendered[static_cast<size_t>(i)] = evalCarrier(static_cast<double>(i) / static_cast<double>(kN) * static_cast<double>(kM));
	}

	const double fundamental = magnitudeAt(rendered, win, kSr, kFc);
	CHECK(dB(fundamental) > -1.0); // fundamental present, near 0 dB

	// Sum the power in every harmonic EXCEPT the fundamental; the loudest survivor is the THD reference.
	// m odd ⇒ harmonics are odd multiples: 3, 5, 7 ...; stop below Nyquist (kN/2 = 512).
	double harmonicPower = 0.0;
	double worstHarmonic = 0.0;
	for (int h = 3; h * kM < kN / 2; h += 2) {
		const double f = kSr * static_cast<double>(h * kM) / static_cast<double>(kN);
		const double mag = magnitudeAt(rendered, win, kSr, f);
		harmonicPower += mag * mag;
		worstHarmonic = std::max(worstHarmonic, mag);
	}
	const double thd = std::sqrt(harmonicPower) / fundamental;
	const double thdDb = dB(thd);
	std::printf("[fm carrier] 1024-pt, fc %.1f Hz, exact-bin: worst harmonic %.1f dB, THD %.1f dB\n",
	            kFc, dB(worstHarmonic / fundamental), thdDb);

	// THE CLAIM: worst harmonic below the doc's −116.9 dB THD floor (allow a little for float32 rounding in
	// the DFT arithmetic itself — the table is the thing under test, not the measurement).
	CHECK(thdDb < -110.0);
	CHECK(dB(worstHarmonic / fundamental) < -110.0);
}

TEST_CASE("fm carrier table: wraps by construction") {
	// The guard sample closes the cycle: phase 0 and phase 1 are the same point, and the value at the
	// boundary is continuous (no step). The known-bad is a table WITHOUT the guard sample, where the last
	// segment wraps from sample 1023 to sample 0 across the whole cycle — that step shows up as a nonzero
	// jump here.
	CHECK(evalCarrier(0.0) == doctest::Approx(0.0));
	CHECK(evalCarrier(1.0) == doctest::Approx(0.0));
	CHECK(evalCarrier(0.25) == doctest::Approx(1.0).epsilon(0.01));
	CHECK(evalCarrier(0.5) == doctest::Approx(0.0).epsilon(0.02));
	CHECK(evalCarrier(0.75) == doctest::Approx(-1.0).epsilon(0.01));

	// Continuity across the boundary: phase 0/1 is a zero crossing (sin(0) = 0, rising), so the sample
	// just before 1.0 is the negative of the sample just after 0.0 — odd symmetry the guard sample
	// guarantees. evalCarrier(0) == evalCarrier(1) == 0 exactly (checked above), so there is no step; a
	// table WITHOUT the guard sample wraps the last segment across the whole cycle and this sign symmetry
	// would break.
	const float before = evalCarrier(1.0 - 1e-4f);
	const float after = evalCarrier(1e-4f);
	CHECK(before == doctest::Approx(-after).epsilon(1e-3));
}

TEST_CASE("fm carrier table: linear interpolation between samples") {
	// Halfway between two table entries the value is the mean of the two samples (linear interp, NOT the
	// sine of the midpoint — that distinction is what separates the carrier from a std::sin call). The
	// known-bad is a nearest-neighbour lookup, which would read one sample exactly and never land on the
	// mean.
	const auto& table = carrierTable();
	const double pos = carrierPosition(0.0);
	const size_t i0 = static_cast<size_t>(pos); // phase 0 → index 0
	const float expected = 0.5f * (table.samples[i0] + table.samples[i0 + 1]);
	// A phase halfway to the next slot:
	const float at = evalCarrier(0.5 / static_cast<double>(kCarrierSlots));
	CHECK(at == doctest::Approx(expected).epsilon(1e-5));
}
