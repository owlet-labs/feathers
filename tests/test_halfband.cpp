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

// The voice's 4x decimator, measured as a filter: its coefficients against the design formula they
// claim to come from, and its response as VALUES — gain at named frequencies, DC, group delay.
//
// Every response number here is measured by driving the real Halfband4x with a sine at the 4x rate
// and fitting the output at the frequency the tone lands on after decimation, which is the only
// thing a decimator's user ever hears. The bounds are what the replaced (vendored, GPL) decimator
// measured under the same instrument on 2026-09-24, so a green here says "at least as good as what
// shipped before the licence migration" — see halfband.h for the table.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/halfband.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

using feathers::Halfband4x;

constexpr double kPi = 3.14159265358979323846;
constexpr double kOutRate = 48000.0;
constexpr double kInRate = 4.0 * kOutRate;

// ---- the design formula, restated independently of the header's literals ----------------------
//
// Valenzuela & Constantinides (1983), in the closed form de Soras' HIIR computes. Kept in the test
// rather than the source so the literals in halfband.h are checked against a derivation rather than
// being the output of one — the two would otherwise agree by construction.

void transitionParams(double tbw, double& k, double& q) {
	k = std::tan((1.0 - 2.0 * tbw) * kPi / 4.0);
	k *= k;
	const double kk = std::pow(1.0 - k * k, 0.25);
	const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
	const double e4 = e * e * e * e;
	q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
}

double designCoef(int index, int n, double tbw) {
	double k = 0.0;
	double q = 0.0;
	transitionParams(tbw, k, q);
	const int order = 2 * n + 1;
	const int c = index + 1;
	double num = 0.0;
	for (int m = 0, sign = 1;; m++, sign = -sign) {
		const double t = std::pow(q, m * (m + 1)) * std::sin((2 * m + 1) * c * kPi / order) * sign;
		num += t;
		if (std::fabs(t) <= 1e-100) {
			break;
		}
	}
	double den = 0.0;
	for (int m = 1, sign = -1;; m++, sign = -sign) {
		const double t = std::pow(q, m * m) * std::cos(2 * m * c * kPi / order) * sign;
		den += t;
		if (std::fabs(t) <= 1e-100) {
			break;
		}
	}
	const double w = num * std::pow(q, 0.25) / (den + 0.5);
	const double w2 = w * w;
	const double x = std::sqrt((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
	return (1.0 - x) / (1.0 + x);
}

// ---- the instrument ------------------------------------------------------------------------------

/// Complex response of the whole 4x decimator to a unit sine at `hz` (an input-rate frequency), read
/// at the output frequency the tone folds to. Phase is relative to the input sampled at the output
/// instants (input index 4m for output m), so its slope is the group delay in output samples.
std::complex<double> toneResponse(double hz, int warm = 4096, int len = 16384) {
	Halfband4x d;
	constexpr int kBlock = Halfband4x::kMaxBlock;
	float in[4 * kBlock];
	float out[kBlock];
	std::vector<double> y;
	y.reserve(static_cast<size_t>(len));
	long idx = 0;
	for (int b = 0; b < warm + len; b += kBlock) {
		for (float& v : in) {
			v = static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(idx++) / kInRate));
		}
		d.decimate(in, out, kBlock);
		for (int i = 0; i < kBlock; i++) {
			if (b + i >= warm && static_cast<int>(y.size()) < len) {
				y.push_back(out[i]);
			}
		}
	}
	double fa = std::fmod(hz, kOutRate);
	const bool mirrored = fa > kOutRate / 2.0;
	if (mirrored) {
		fa = kOutRate - fa;
	}
	std::complex<double> acc = 0.0;
	double wsum = 0.0;
	for (int m = 0; m < len; m++) {
		const double t = static_cast<double>(warm + m);
		const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (m + 0.5) / len); // Hann
		acc += w * y[static_cast<size_t>(m)] * std::polar(1.0, -2.0 * kPi * fa * t / kOutRate);
		wsum += w;
	}
	acc *= 2.0 / wsum;
	if (mirrored) {
		acc = std::conj(acc);
	}
	return acc / std::polar(1.0, -kPi / 2.0); // the input was a sine: cos shifted by -pi/2
}

double gainDb(double hz) {
	return 20.0 * std::log10(std::abs(toneResponse(hz)) + 1e-30);
}

double groupDelaySamples(double hz) {
	constexpr double df = 5.0;
	double dp = std::arg(toneResponse(hz + df)) - std::arg(toneResponse(hz - df));
	while (dp > kPi) {
		dp -= 2.0 * kPi;
	}
	while (dp < -kPi) {
		dp += 2.0 * kPi;
	}
	return -dp / (2.0 * kPi * 2.0 * df / kOutRate);
}

} // namespace

TEST_CASE("the coefficients are the Valenzuela-Constantinides design they say they are") {
	// Each literal re-derived from the formula at the order and transition width the header names.
	// macOS's libm computes the design ~5.5 ulps from glibc's (measured on CI 2026-09-27: 1.22e-15 on a
	// stage-2 coefficient), so a tight bound is not portable. 1e-14 (~45 ulps) still fails any edit at the
	// 13th significant digit or above while tolerating libm differences across platforms.
	for (int i = 0; i < feathers::kHalfbandStage1Coefs; i++) {
		CAPTURE(i);
		CHECK(std::fabs(feathers::kHalfbandStage1[i] -
		                designCoef(i, feathers::kHalfbandStage1Coefs, feathers::kHalfbandStage1Tbw)) <
		      1e-14);
	}
	for (int i = 0; i < feathers::kHalfbandStage2Coefs; i++) {
		CAPTURE(i);
		CHECK(std::fabs(feathers::kHalfbandStage2[i] -
		                designCoef(i, feathers::kHalfbandStage2Coefs, feathers::kHalfbandStage2Tbw)) <
		      1e-14);
	}
	// Ascending and inside (0, 1): the branch dealing and the stability of every section rely on it.
	for (int i = 0; i < feathers::kHalfbandStage2Coefs; i++) {
		CAPTURE(i);
		CHECK(feathers::kHalfbandStage2[i] > 0.0);
		CHECK(feathers::kHalfbandStage2[i] < 1.0);
		if (i > 0) {
			CHECK(feathers::kHalfbandStage2[i] > feathers::kHalfbandStage2[i - 1]);
		}
	}
}

TEST_CASE("DC passes at unity") {
	Halfband4x d;
	float in[4 * Halfband4x::kMaxBlock];
	float out[Halfband4x::kMaxBlock];
	std::fill(std::begin(in), std::end(in), 1.0f);
	for (int b = 0; b < 64; b++) {
		d.decimate(in, out, Halfband4x::kMaxBlock);
	}
	// Absolute bounds, not Approx: Approx adds a scale term that would swallow a 0.1 dB error here.
	for (const float v : out) {
		CHECK(std::fabs(v - 1.0f) < 1e-5f);
	}
}

TEST_CASE("the passband is flat to 20 kHz and beyond, to a thousandth of a dB") {
	// The design ripple is ~1e-10 dB; 0.001 dB is the instrument's float floor with margin. The
	// replaced decimator measured the same way stayed inside +/-0.0002 dB.
	for (const double hz : {20.0, 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 20000.0, 22000.0,
	                        23000.0}) {
		CAPTURE(hz);
		CHECK(std::fabs(gainDb(hz)) < 0.001);
	}
}

TEST_CASE("the stopband: every image that would fold into the band is 104 dB down or more") {
	// Input frequencies above 24.5 kHz land back below 24 kHz after decimation. The replaced
	// decimator's worst anywhere from 24.5 to 96 kHz was -103.9 dB (at 24.85 kHz); this one's
	// measured worst is -107.9, at the 24.5 kHz edge itself. Named points first: the edge, the
	// replaced filter's weak spots, the stage-1 stopband edge near 71 kHz, and the top.
	for (const double hz : {24500.0, 25000.0, 28000.0, 29250.0, 32000.0, 44000.0, 45000.0, 72000.0,
	                        78500.0, 90000.0, 95000.0}) {
		CAPTURE(hz);
		CHECK(gainDb(hz) < -104.0);
	}
	// And a sweep, because a list of named frequencies is exactly how this repo once measured the
	// wrong thing (test_aliasing's header). 250 Hz steps resolve stage 2's ripple lobes, which are
	// several kHz wide.
	double worst = -1e9;
	double worstHz = 0.0;
	for (double hz = 24500.0; hz < 96000.0; hz += 250.0) {
		const double g = gainDb(hz);
		if (g > worst) {
			worst = g;
			worstHz = hz;
		}
	}
	CAPTURE(worstHz);
	CHECK(worst < -104.0);
}

TEST_CASE("the transition band is where the design puts it") {
	// Half gain at the halfband point is the defining property of the structure (power
	// complementarity): exactly -3.01 dB at a quarter of stage 2's input rate, i.e. 24 kHz. Measured
	// a hair either side because 24 kHz itself lands on the output Nyquist, where a sine and its
	// image coincide.
	CHECK(gainDb(23500.0) > -0.001);
	CHECK(gainDb(24000.0 - 150.0) > -1.0);
	CHECK(gainDb(24000.0 + 150.0) < -6.0);
}

TEST_CASE("latency: group delay at the host rate") {
	// Replaced decimator: 2.23 samples at DC, 7.77 at 20 kHz. This one: 2.44 and 8.81 — a fifth of a
	// sample (4.4 us) more at DC, one sample (21 us) more at 20 kHz, the price of the extra stage-2
	// coefficients that buy its stopband margin. Pinned to a tenth so a stage lost or doubled (each
	// moves it by a whole sample or more) is caught, while a re-measurement elsewhere is not.
	const double dc = groupDelaySamples(100.0);
	CHECK(dc > 2.39);
	CHECK(dc < 2.49);
	const double top = groupDelaySamples(20000.0);
	CHECK(top > 8.71);
	CHECK(top < 8.91);
}

TEST_CASE("the right ear adopts the left's decimator by memcpy and continues it exactly") {
	static_assert(std::is_trivially_copyable_v<Halfband4x>);
	Halfband4x a;
	float in[4 * Halfband4x::kMaxBlock];
	float outA[Halfband4x::kMaxBlock];
	float outB[Halfband4x::kMaxBlock];
	for (int i = 0; i < 4 * Halfband4x::kMaxBlock; i++) {
		in[i] = static_cast<float>(std::sin(0.37 * i));
	}
	a.decimate(in, outA, Halfband4x::kMaxBlock);
	Halfband4x b;
	std::memcpy(static_cast<void*>(&b), static_cast<const void*>(&a), sizeof(b));
	a.decimate(in, outA, Halfband4x::kMaxBlock);
	b.decimate(in, outB, Halfband4x::kMaxBlock);
	for (int i = 0; i < Halfband4x::kMaxBlock; i++) {
		CAPTURE(i);
		CHECK(outA[i] == outB[i]);
	}
	// And the state was not trivially empty — a reset decimator gives a different second block.
	Halfband4x fresh;
	fresh.decimate(in, outB, Halfband4x::kMaxBlock);
	CHECK(std::memcmp(outA, outB, sizeof(outA)) != 0);
}

TEST_CASE("block size does not change the output: 4 at a time equals 32 at a time") {
	constexpr int kTotal = 4 * Halfband4x::kMaxBlock;
	std::vector<float> in(4 * kTotal);
	for (size_t i = 0; i < in.size(); i++) {
		in[i] = static_cast<float>(std::sin(0.013 * static_cast<double>(i * i % 997)));
	}
	Halfband4x big;
	Halfband4x small;
	std::vector<float> outBig(kTotal);
	std::vector<float> outSmall(kTotal);
	for (int o = 0; o < kTotal; o += Halfband4x::kMaxBlock) {
		big.decimate(in.data() + 4 * o, outBig.data() + o, Halfband4x::kMaxBlock);
	}
	for (int o = 0; o < kTotal; o += Halfband4x::kGroup) {
		small.decimate(in.data() + 4 * o, outSmall.data() + o, Halfband4x::kGroup);
	}
	CHECK(outBig == outSmall);
}

// ---- backlog JL: one section loop for both directions, bit for bit ---------------------------------
//
// THE TWO CLASSES AS THEY WERE BEFORE THE SHARED HELPER, kept here verbatim as the REFERENCE. Until
// 2026-09-25 each class walked the allpass sections with its own copy of the loop; the refactor put that
// loop in one place. A refactor of float arithmetic is only a refactor if the bits survive it, so the
// check is exact equality of every output word on a signal that visits the awkward values (full scale,
// sign flips, subnormals, zeros), not a tolerance.
namespace {

template <int N>
class ReferenceDecimator {
public:
	explicit ReferenceDecimator(const double (&coefs)[N]) {
		for (int i = 0; i < N; i++) {
			a_[i] = static_cast<float>(coefs[i]);
			x1_[i] = 0.0f;
			y1_[i] = 0.0f;
		}
	}
	float process(float earlier, float later) {
		float b0 = later;
		float b1 = earlier;
		for (int i = 0; i < N; i += 2) {
			const float y0 = (b0 - y1_[i]) * a_[i] + x1_[i];
			x1_[i] = b0;
			y1_[i] = y0;
			b0 = y0;
			const float y1 = (b1 - y1_[i + 1]) * a_[i + 1] + x1_[i + 1];
			x1_[i + 1] = b1;
			y1_[i + 1] = y1;
			b1 = y1;
		}
		return 0.5f * (b0 + b1);
	}

private:
	float a_[N];
	float x1_[N];
	float y1_[N];
};

template <int N>
class ReferenceInterpolator {
public:
	explicit ReferenceInterpolator(const double (&coefs)[N]) {
		for (int i = 0; i < N; i++) {
			a_[i] = static_cast<float>(coefs[i]);
			x1_[i] = 0.0f;
			y1_[i] = 0.0f;
		}
	}
	void process(float in, float& earlier, float& later) {
		float b0 = in;
		float b1 = in;
		for (int i = 0; i < N; i += 2) {
			const float y0 = (b0 - y1_[i]) * a_[i] + x1_[i];
			x1_[i] = b0;
			y1_[i] = y0;
			b0 = y0;
			const float y1 = (b1 - y1_[i + 1]) * a_[i + 1] + x1_[i + 1];
			x1_[i + 1] = b1;
			y1_[i + 1] = y1;
			b1 = y1;
		}
		earlier = b0;
		later = b1;
	}

private:
	float a_[N];
	float x1_[N];
	float y1_[N];
};

/// A FIXED, NASTY SIGNAL: an LCG's noise at full scale, a sine, runs of zeros, subnormals, and a
/// polarity-flipping square — deterministic, so the reference is recomputed rather than stored.
std::vector<float> nastySignal(size_t n) {
	std::vector<float> s(n);
	uint32_t lcg = 0x2545F491u;
	for (size_t i = 0; i < n; i++) {
		lcg = lcg * 1664525u + 1013904223u;
		const float noise = static_cast<float>(static_cast<int32_t>(lcg)) / 2147483648.0f;
		switch ((i / 257) % 5) {
		case 0: s[i] = noise; break;
		case 1: s[i] = static_cast<float>(std::sin(0.071 * static_cast<double>(i))); break;
		case 2: s[i] = 0.0f; break;
		case 3: s[i] = noise * 1e-39f; break; // subnormal
		default: s[i] = (i & 1) ? 1.0f : -1.0f; break;
		}
	}
	return s;
}

uint32_t bitsOf(float f) {
	uint32_t u = 0;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

template <int N>
void checkDecimatorBits(const double (&coefs)[N]) {
	const std::vector<float> in = nastySignal(4096);
	feathers::HalfbandDecimator2x<N> now(coefs);
	ReferenceDecimator<N> ref(coefs);
	size_t mismatches = 0;
	size_t nonzero = 0;
	for (size_t i = 0; i + 1 < in.size(); i += 2) {
		const float got = now.process(in[i], in[i + 1]);
		const float want = ref.process(in[i], in[i + 1]);
		mismatches += bitsOf(got) != bitsOf(want);
		nonzero += want != 0.0f;
		if (i == in.size() / 2) {
			// reset() mid-stream: both start again from silence.
			now.reset();
			ref = ReferenceDecimator<N>(coefs);
		}
	}
	CHECK(mismatches == 0);
	CHECK(nonzero > 1000); // not vacuous: the comparison saw real output
}

template <int N>
void checkInterpolatorBits(const double (&coefs)[N]) {
	const std::vector<float> in = nastySignal(4096);
	feathers::HalfbandInterpolator2x<N> now(coefs);
	ReferenceInterpolator<N> ref(coefs);
	size_t mismatches = 0;
	size_t nonzero = 0;
	for (size_t i = 0; i < in.size(); i++) {
		float ga = 0.0f, gb = 0.0f, wa = 0.0f, wb = 0.0f;
		now.process(in[i], ga, gb);
		ref.process(in[i], wa, wb);
		mismatches += (bitsOf(ga) != bitsOf(wa)) + (bitsOf(gb) != bitsOf(wb));
		nonzero += (wa != 0.0f) + (wb != 0.0f);
		if (i == in.size() / 2) {
			now.reset();
			ref = ReferenceInterpolator<N>(coefs);
		}
	}
	CHECK(mismatches == 0);
	CHECK(nonzero > 2000);
}

} // namespace

TEST_CASE("backlog JL: the decimator is bit-identical to its pre-refactor section loop, both stages") {
	checkDecimatorBits(feathers::kHalfbandStage1);
	checkDecimatorBits(feathers::kHalfbandStage2);
}

TEST_CASE("backlog JL: the interpolator is bit-identical to its pre-refactor section loop, both stages") {
	checkInterpolatorBits(feathers::kHalfbandStage1);
	checkInterpolatorBits(feathers::kHalfbandStage2);
}
