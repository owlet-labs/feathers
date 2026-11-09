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

// THE TERRAIN'S SPECTRUM FOR ONE SOUND (additive engine design §2): the envelope, the
// fine structure and the off-grid ratios, asserted by value against restatements written in this file.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/partial_spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

using namespace feathers::partials;

namespace {
std::array<float, kMaxPartials> zeros() {
	std::array<float, kMaxPartials> a {};
	return a;
}
double frac(double x) {
	return x - std::floor(x);
}
/// A bipolar detail pattern in [-1, 1] that is neither constant nor periodic in k.
std::array<float, kMaxPartials> detailPattern() {
	std::array<float, kMaxPartials> a {};
	for (int i = 0; i < kMaxPartials; ++i) {
		a[i] = static_cast<float>(std::sin(0.7 * i + 0.3));
	}
	return a;
}
/// The table's grid, restated: point j sits at log2 k = min + j·(max − min)/(points − 1).
double gridLog2(int j) {
	return kEnvelopeLog2Min + j * (kEnvelopeLog2Max - kEnvelopeLog2Min) / (kEnvelopePoints - 1);
}
/// Every field mid-range and on: the sound the value cases build.
PartialFields busyFields() {
	PartialFields f {};
	f.rolloff = 0.4f;
	f.formant1Centre = 0.3f;
	f.formant1Width = 0.2f;
	f.formant1Gain = 0.6f;
	f.formant2Centre = 0.7f;
	f.formant2Width = 0.1f;
	f.formant2Gain = 0.4f;
	f.oddEven = 0.3f;
	f.combSpacing = 0.35f;
	f.combDepth = 0.4f;
	f.detail = 0.5f;
	f.offGrid = 0.7f;
	return f;
}
} // namespace

TEST_CASE("envelope: rolloff is k^-p with p re-spread (PAL): 1 + c·sign(u)·|u|^1.6, u = 2·field − 1, c 1.2 below the saw and 1.5 above") {
	PartialFields f {};
	f.rolloff = 0.5f; // u = 0: p = 1, a saw's rolloff
	CHECK(std::abs(envelopeAt(f, 1.0) - 1.0) < 1e-12);
	CHECK(std::abs(envelopeAt(f, 4.0) - 0.25) < 1e-12);
	f.rolloff = 1.0f; // p = 2.5, the smooth spectrum's
	CHECK(std::abs(envelopeAt(f, 2.0) - std::pow(2.0, -2.5)) < 1e-12);
	f.rolloff = 0.0f; // p = −0.2: a rising tilt
	CHECK(std::abs(envelopeAt(f, 8.0) - std::pow(8.0, 0.2)) < 1e-12);
	// THE RESTATEMENT AT EVERY FIELD, and the curve monotonic and continuous.
	double last = -10.0;
	for (int i = 0; i <= 1000; ++i) {
		const float r = static_cast<float>(i) / 1000.0f;
		const double u = 2.0 * static_cast<double>(r) - 1.0;
		const double want = u < 0.0 ? 1.0 - 1.2 * std::pow(-u, 1.6) : 1.0 + 1.5 * std::pow(u, 1.6);
		CHECK(rolloffExponent(r) == doctest::Approx(want).epsilon(1e-12));
		CHECK(rolloffExponent(r) > last);
		CHECK(rolloffExponent(r) - last < (i == 0 ? 100.0 : 0.02)); // no jump
		last = rolloffExponent(r);
	}
	// THE SPREAD: the middle half of the field now covers p 0.65 .. 1.5 (it was 1.0 .. 2.0), nearer the saw.
	CHECK(rolloffExponent(0.25f) == doctest::Approx(1.0 - 1.2 * std::pow(0.5, 1.6)));
	CHECK(rolloffExponent(0.75f) == doctest::Approx(1.0 + 1.5 * std::pow(0.5, 1.6)));
}

TEST_CASE("envelope: a formant is a gaussian in octaves at its centre, adding 4·gain") {
	PartialFields f {};
	f.rolloff = 0.5f;        // p = 1 (PAL's re-spread)
	f.formant1Centre = 0.4f; // log2 c = 1 + 5·0.4 = 3, so c ≈ 8
	f.formant1Width = 0.0f;  // w = 0.25 octaves
	f.formant1Gain = 0.5f;   // G = 2
	// The centre from the STORED float: 0.4f is 0.4000000059..., so log2 c is 3 + 3e-8, and one width up
	// from a literal 8 is off by 4.7e-8 (measured), far outside the bound.
	const double c = std::exp2(1.0 + 5.0 * static_cast<double>(f.formant1Centre));
	CHECK(std::abs(c - 8.0) < 1e-6);
	CHECK(std::abs(envelopeAt(f, c) - std::pow(c, -1.0) * 3.0) < 1e-12); // 1 + G at the centre
	const double kUp = c * std::exp2(0.25); // one width up: e^-0.5
	CHECK(std::abs(envelopeAt(f, kUp) - std::pow(kUp, -1.0) * (1.0 + 2.0 * std::exp(-0.5))) < 1e-12);
}

TEST_CASE("envelope: formant 2 with a non-zero width, alone") {
	PartialFields f {};
	f.rolloff = 0.5f;        // p = 1 (PAL's re-spread)
	f.formant2Centre = 0.5f; // log2 c = 1 + 5·0.5 = 3.5, exact
	f.formant2Width = 0.5f;  // w = 0.25 + 1.5·0.5 = 1 octave, exact
	f.formant2Gain = 0.25f;  // G = 1
	CHECK(std::abs(envelopeAt(f, std::exp2(3.5)) - std::exp2(-3.5) * 2.0) < 1e-12);
	const double g1 = 1.0 + std::exp(-0.5); // one width either side
	CHECK(std::abs(envelopeAt(f, std::exp2(4.5)) - std::exp2(-4.5) * g1) < 1e-12);
	CHECK(std::abs(envelopeAt(f, std::exp2(2.5)) - std::exp2(-2.5) * g1) < 1e-12);
	const double g2 = 1.0 + std::exp(-2.0); // two widths up
	CHECK(std::abs(envelopeAt(f, std::exp2(5.5)) - std::exp2(-5.5) * g2) < 1e-12);
}

TEST_CASE("fine structure: odd/even is one-sided, comb has teeth, detail ramps in with k") {
	PartialFields f {};
	CHECK(fineAt(f, 7, 0.0f) == 1.0); // all fields at rest: nothing
	f.oddEven = 1.0f;
	CHECK(fineAt(f, 2, 0.0f) == 0.0);
	CHECK(fineAt(f, 3, 0.0f) == 1.0);
	f = {};
	f.combSpacing = 0.1f; // spacing 2 + 10·0.1 = 3
	f.combDepth = 1.0f;
	CHECK(std::abs(fineAt(f, 3, 0.0f)) < 1e-12);
	f = {};
	f.detail = 1.0f;
	CHECK(std::abs(fineAt(f, 1, 1.0f) - 1.2) < 1e-12);  // w(1) = 0.2
	CHECK(std::abs(fineAt(f, 8, -0.5f) - 0.5) < 1e-12); // w(8) = 1
	CHECK(fineAt(f, 20, -1.0f) == 0.0);                 // clamped at zero, never negative
}

TEST_CASE("fine structure: the comb passes a tooth's midpoint at full gain and halves between") {
	PartialFields f {};
	f.combDepth = 1.0f;
	f.combSpacing = 0.2f; // spacing 2 + 10·0.2 = 4
	CHECK(std::abs(fineAt(f, 2, 0.0f) - 1.0) < 1e-12); // midpoint: 1 − ½(1 + cos π)
	CHECK(std::abs(fineAt(f, 4, 0.0f)) < 1e-12);       // the tooth
	CHECK(std::abs(fineAt(f, 1, 0.0f) - 0.5) < 1e-7);  // a quarter: 1 − ½(1 + cos ½π)
	f.combSpacing = 0.4f;                               // spacing 6
	f.combDepth = 0.5f;
	CHECK(std::abs(fineAt(f, 3, 0.0f) - 1.0) < 1e-12);
	CHECK(std::abs(fineAt(f, 6, 0.0f) - 0.5) < 1e-12); // half depth: the tooth keeps half
}

TEST_CASE("off-grid: field 0 is exactly harmonic everywhere; partials below the start never move") {
	CHECK(offGridStart(0.0f) == 320.0);
	CHECK(std::abs(offGridStart(1.0f) - 6.0) < 1e-9);
	for (int k = 1; k <= kMaxPartials; ++k) {
		CHECK(offGridRatio(k, offGridStart(0.0f)) == static_cast<double>(k));
	}
	const double s = offGridStart(0.6f);
	for (int k = 1; k <= static_cast<int>(s); ++k) {
		CHECK(offGridRatio(k, s) == static_cast<double>(k));
	}
}

TEST_CASE("off-grid: the start sweeps 320 to 6 on a LOG scale") {
	const double top = std::log2(320.0);
	const double bottom = std::log2(6.0);
	for (const float g : {0.25f, 0.5f, 0.75f}) {
		const double want = std::exp2(top - static_cast<double>(g) * (top - bottom));
		CHECK(std::abs(offGridStart(g) - want) < 1e-9);
	}
	CHECK(std::abs(offGridStart(0.5f) - std::sqrt(320.0 * 6.0)) < 1e-9); // the geometric mean
}

TEST_CASE("off-grid: past the start, k + depth·ramp·(frac(kφ) − ½)") {
	const double s = offGridStart(1.0f); // 6, so the ramp is max(4, 3) = 4 harmonics long
	const int k = 40;                    // well past the ramp: it reads 1
	const double want = k + 0.5 * (frac(k * std::numbers::phi) - 0.5);
	CHECK(std::abs(offGridRatio(k, s) - want) < 1e-12);
	// INSIDE the ramp, against smoothstep restated: x = (k − s)/4, r = x²(3 − 2x).
	for (const int kk : {7, 8, 9}) {
		const double x = (kk - s) / 4.0;
		REQUIRE(x > 0.0);
		REQUIRE(x < 1.0);
		const double r = x * x * (3.0 - 2.0 * x);
		const double w = kk + 0.5 * r * (frac(kk * std::numbers::phi) - 0.5);
		CHECK(std::abs(offGridRatio(kk, s) - w) < 1e-12);
	}
}

TEST_CASE("off-grid: continuous in the field, swept finely across integer starts") {
	// A LIPSCHITZ BOUND, derived. ratio = k + D·r(x)·(frac(kφ) − ½) with x = (k − s)/R, R = max(4, s/2).
	// |r'| ≤ 1.5 (smoothstep), |frac − ½| ≤ ½. Inside the ramp (0 < x ≤ 1): with R = 4, |dx/ds| = 1/4; with
	// R = s/2 (s ≥ 8), x = 2k/s − 2 so |dx/ds| = 2k/s² ≤ 3/s ≤ 3/8 because k ≤ 1.5s. So |dratio/ds| ≤
	// D·1.5·(3/8)·½. And s = 320·(6/320)^g gives |ds/dg| = s·ln(320/6) ≤ s(lo)·ln(320/6) on [lo, hi].
	// At the start itself the early return and the ramp's zero agree, so nothing jumps there either.
	const float lo = 0.55f, hi = 0.60f;
	const double sLo = offGridStart(lo), sHi = offGridStart(hi);
	REQUIRE(std::floor(sLo) - std::floor(sHi) >= 3.0); // the sweep crosses several integer starts
	const double lipschitz = kOffGridDepth * 1.5 * (3.0 / 8.0) * 0.5 * sLo * std::log(320.0 / 6.0);
	const int steps = 25000;
	double worst = 0.0; // the largest |Δratio| / bound seen
	float g0 = lo;
	std::array<double, kMaxPartials> prev {};
	double s0 = offGridStart(g0);
	for (int kk = 1; kk <= kMaxPartials; ++kk) {
		prev[kk - 1] = offGridRatio(kk, s0);
	}
	for (int n = 1; n <= steps; ++n) {
		const float g1 = static_cast<float>(lo + (static_cast<double>(hi) - lo) * n / steps);
		const double s1 = offGridStart(g1);
		const double bound = lipschitz * (static_cast<double>(g1) - static_cast<double>(g0)) + 1e-12;
		for (int kk = 1; kk <= kMaxPartials; ++kk) {
			const double r = offGridRatio(kk, s1);
			worst = std::max(worst, std::abs(r - prev[kk - 1]) / bound);
			prev[kk - 1] = r;
		}
		g0 = g1;
	}
	CHECK(worst <= 1.0);
	CHECK(worst > 0.0); // the sweep moved something: not vacuous
}

TEST_CASE("buildSound: normalised to kTargetRss over the envelope × fine at each ratio") {
	PartialFields f {};
	f.rolloff = 0.3f;
	f.oddEven = 0.5f;
	PartialSpectrum s {};
	buildSound(f, zeros(), s);
	double power = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double a = static_cast<double>(s.envAtRatio[i]) * static_cast<double>(s.fine[i]);
		power += a * a;
	}
	CHECK(std::abs(std::sqrt(power) - kTargetRss) < 1e-5);
}

TEST_CASE("buildSound: envAtRatio is the table read at each partial's ratio") {
	PartialFields f {};
	f.rolloff = 0.6f;
	f.offGrid = 0.8f;
	PartialSpectrum s {};
	buildSound(f, zeros(), s);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(s.envAtRatio[i] == envelopeRead(s, static_cast<double>(s.ratio[i])));
	}
	// And with every field on and a detail pattern.
	buildSound(busyFields(), detailPattern(), s);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(s.envAtRatio[i] == envelopeRead(s, static_cast<double>(s.ratio[i])));
	}
}

TEST_CASE("buildSound: fine is fineAt at each harmonic with ITS OWN detail triangle") {
	const PartialFields f = busyFields();
	const auto detail = detailPattern();
	PartialSpectrum s {};
	buildSound(f, detail, s);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(s.fine[i] == static_cast<float>(fineAt(f, i + 1, detail[i])));
	}
}

TEST_CASE("buildSound: the table is the envelope on its log2 grid, times a scale restated from the RSS") {
	const PartialFields f = busyFields();
	const auto detail = detailPattern();
	PartialSpectrum s {};
	buildSound(f, detail, s);
	// The un-normalised table in double, read by linear interpolation in log2 k at each ratio.
	std::array<double, kEnvelopePoints> table {};
	for (int j = 0; j < kEnvelopePoints; ++j) {
		table[j] = envelopeAt(f, std::exp2(gridLog2(j)));
	}
	const double step = (kEnvelopeLog2Max - kEnvelopeLog2Min) / (kEnvelopePoints - 1);
	const double start = offGridStart(f.offGrid);
	double power = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double ratio = static_cast<double>(static_cast<float>(offGridRatio(i + 1, start)));
		CHECK(static_cast<double>(s.ratio[i]) == ratio);
		const double pos = (std::log2(ratio) - kEnvelopeLog2Min) / step;
		const int j = static_cast<int>(std::floor(pos));
		const double t = pos - j;
		const double e = table[j] + (table[j + 1] - table[j]) * t;
		const double a = e * fineAt(f, i + 1, detail[i]);
		power += a * a;
	}
	const double scale = kTargetRss / std::sqrt(power);
	for (const int j : {0, 1, 37, 100, 128, 200, 254, 255}) {
		const double want = scale * table[j];
		CHECK(std::abs(static_cast<double>(s.envelope[j]) - want) <= 2e-6 * want);
	}
}

TEST_CASE("envelopeRead: linear in log2 k between grid points, clamped at both ends, safe for any k") {
	PartialSpectrum s {};
	buildSound(busyFields(), detailPattern(), s);
	REQUIRE(s.envelope[0] != s.envelope[kEnvelopePoints - 1]);
	const double step = (kEnvelopeLog2Max - kEnvelopeLog2Min) / (kEnvelopePoints - 1);
	for (const double k : {0.7, 1.0, 5.3, 77.7, 333.3, 1000.0}) {
		const double pos = (std::log2(k) - kEnvelopeLog2Min) / step;
		const int j = static_cast<int>(std::floor(pos));
		const double t = pos - j;
		const double want = s.envelope[j] + (static_cast<double>(s.envelope[j + 1]) - s.envelope[j]) * t;
		CHECK(std::abs(static_cast<double>(envelopeRead(s, k)) - want) <= 1e-6 * std::abs(want));
	}
	const float bottom = s.envelope[0];
	const float top = s.envelope[kEnvelopePoints - 1];
	CHECK(envelopeRead(s, 0.5) == bottom);
	CHECK(envelopeRead(s, 0.25) == bottom);
	CHECK(envelopeRead(s, 2048.0) == top);
	// The values Task 9 can hand it: none may reach the int cast.
	CHECK(envelopeRead(s, std::numeric_limits<double>::quiet_NaN()) == bottom);
	CHECK(envelopeRead(s, std::numeric_limits<double>::infinity()) == top);
	CHECK(envelopeRead(s, 0.0) == bottom);
	CHECK(envelopeRead(s, -3.0) == bottom);
	CHECK(envelopeRead(s, 1e300) == top);
	CHECK(envelopeRead(s, std::numeric_limits<double>::max()) == top);
}

TEST_CASE("buildSound: out-of-range and NaN fields give a finite spectrum equal to the clamped one") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	PartialFields wild {};
	wild.rolloff = 2.0f;
	wild.formant1Centre = -3.0f;
	wild.formant1Width = -0.5f; // w < 0: 0/0 at the centre if it got through
	wild.formant1Gain = 3.0f;
	wild.formant2Centre = 7.0f;
	wild.formant2Width = nan;
	wild.formant2Gain = 0.5f;
	wild.oddEven = 1.5f;  // negative even gain if it got through
	wild.combSpacing = -1.0f;
	wild.combDepth = 2.0f;
	wild.detail = 3.0f;
	wild.offGrid = nan;   // passes `<= 0` if it got through
	wild.smoothness = -2.0f;
	PartialFields tame {};
	tame.rolloff = 1.0f;
	tame.formant1Centre = 0.0f;
	tame.formant1Width = 0.0f;
	tame.formant1Gain = 1.0f;
	tame.formant2Centre = 1.0f;
	tame.formant2Width = 0.0f;
	tame.formant2Gain = 0.5f;
	tame.oddEven = 1.0f;
	tame.combSpacing = 0.0f;
	tame.combDepth = 1.0f;
	tame.detail = 1.0f;
	tame.offGrid = 0.0f;
	std::array<float, kMaxPartials> wildDetail = detailPattern(), tameDetail = detailPattern();
	wildDetail[0] = 5.0f;   tameDetail[0] = 1.0f;
	wildDetail[2] = -5.0f;  tameDetail[2] = -1.0f;
	wildDetail[4] = nan;    tameDetail[4] = 0.0f;
	PartialSpectrum a {}, b {};
	buildSound(wild, wildDetail, a);
	buildSound(tame, tameDetail, b);
	bool finite = true;
	for (const float v : a.envelope) finite = finite && std::isfinite(v);
	for (int i = 0; i < kMaxPartials; ++i) {
		finite = finite && std::isfinite(a.fine[i]) && std::isfinite(a.ratio[i]) && std::isfinite(a.envAtRatio[i]);
	}
	CHECK(finite);
	CHECK(a == b);
	bool nonNegative = true;
	for (const float v : a.fine) nonNegative = nonNegative && v >= 0.0f;
	CHECK(nonNegative);
}

TEST_CASE("accumulate: a weighted sum, member by member") {
	PartialFields a {}, b {};
	b.rolloff = 0.9f;
	b.offGrid = 0.7f;
	b.oddEven = 0.6f;
	PartialSpectrum sa {}, sb {}, blend {};
	buildSound(a, zeros(), sa);
	buildSound(b, zeros(), sb);
	accumulate(blend, sa, 0.25);
	accumulate(blend, sb, 0.75);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(std::abs(blend.fine[i] - (0.25f * sa.fine[i] + 0.75f * sb.fine[i])) < 1e-6);
		CHECK(std::abs(blend.ratio[i] - (0.25f * sa.ratio[i] + 0.75f * sb.ratio[i])) < 1e-4);
		CHECK(std::abs(blend.envAtRatio[i] - (0.25f * sa.envAtRatio[i] + 0.75f * sb.envAtRatio[i])) < 1e-6);
	}
	for (int j = 0; j < kEnvelopePoints; ++j) {
		const float want = 0.25f * sa.envelope[j] + 0.75f * sb.envelope[j];
		CHECK(std::abs(blend.envelope[j] - want) <= 1e-6f * std::max(1.0f, std::abs(want)));
	}
}

TEST_CASE("smoothness: 0 leaves every field exactly as it was") {
	const PartialFields f = busyFields(); // smoothness 0
	CHECK(smoothed(f) == f);
}

TEST_CASE("smoothness: 0.5 halves each scaled field and puts rolloff half-way to 1") {
	PartialFields f = busyFields();
	f.smoothness = 0.5f;
	const PartialFields g = smoothed(f);
	CHECK(g.detail == f.detail * 0.5f);
	CHECK(g.combDepth == f.combDepth * 0.5f);
	CHECK(g.oddEven == f.oddEven * 0.5f);
	CHECK(g.formant1Gain == f.formant1Gain * 0.5f);
	CHECK(g.formant2Gain == f.formant2Gain * 0.5f);
	CHECK(g.offGrid == f.offGrid * 0.5f);
	const double r = static_cast<double>(f.rolloff);
	CHECK(std::abs(static_cast<double>(g.rolloff) - (r + (1.0 - r) * 0.5)) < 1e-7);
	// Untouched: the formant shapes, the comb spacing, the smoothness itself.
	CHECK(g.formant1Centre == f.formant1Centre);
	CHECK(g.formant1Width == f.formant1Width);
	CHECK(g.formant2Centre == f.formant2Centre);
	CHECK(g.formant2Width == f.formant2Width);
	CHECK(g.combSpacing == f.combSpacing);
}

TEST_CASE("smoothness: 1 builds the smooth spectrum whatever the other fields say") {
	PartialFields f = busyFields(); // off-grid 0.7 among them: σ = 1 must make it harmonic too
	f.smoothness = 1.0f;
	PartialFields plain {};
	plain.rolloff = 1.0f;
	PartialSpectrum a {}, b {};
	buildSound(f, detailPattern(), a);
	buildSound(plain, zeros(), b);
	CHECK(a == b);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(a.ratio[i] == static_cast<float>(i + 1));
	}
}

// ---- PAL stage 1 (additive design §3.7): the knee, the shelf, the body, and sparse country -------------------------------

TEST_CASE("PAL envelope: the knee is flat below 2^(4·knee) and the power law above; the shelf and the body are their dB restated; each at 0 is nothing") {
	PartialFields f {};
	f.rolloff = 0.5f; // p = 1
	const double p = 1.0;
	// EACH AT REST IS EXACTLY THE POWER LAW, below the fundamental too (Formant Shift reads down to k ½).
	for (const double k : { 0.5, 1.0, 3.0, 17.0, 200.0 }) {
		CHECK(envelopeAt(f, k) == std::exp2(-p * std::log2(k))); // to the bit: k^−p as the envelope writes it
	}
	// THE KNEE at 0.5: k0 = 4. Flat from 1 to 4 at 4^−p, the power law above, and below the fundamental the slope kept, offset.
	f.knee = 0.5f;
	CHECK(envelopeAt(f, 1.0) == doctest::Approx(std::pow(4.0, -p)));
	CHECK(envelopeAt(f, 2.5) == doctest::Approx(std::pow(4.0, -p)));
	CHECK(envelopeAt(f, 9.0) == doctest::Approx(std::pow(9.0, -p)));
	CHECK(envelopeAt(f, 0.5) == doctest::Approx(std::pow(0.5, -p) * std::pow(4.0, -p)));
	f.knee = 0.0f;
	// THE SHELF at 1 with shelfAt 0.5: +12 dB from an octave's step centred on 2^3; a full octave above it +12 dB, below it 0.
	f.shelf = 1.0f;
	f.shelfAt = 0.5f;
	CHECK(20.0 * std::log10(envelopeAt(f, std::exp2(3.0 + 0.5)) / std::exp2(-(3.0 + 0.5) * p)) == doctest::Approx(12.0));
	CHECK(20.0 * std::log10(envelopeAt(f, std::exp2(3.0 - 0.5)) / std::exp2(-(3.0 - 0.5) * p)) == doctest::Approx(0.0).epsilon(1e-9));
	CHECK(20.0 * std::log10(envelopeAt(f, std::exp2(3.0)) / std::exp2(-3.0 * p)) == doctest::Approx(6.0)); // half way
	f.shelf = 0.0f;
	// THE BODY at 1 with bodyAt 0.5: ×4 at harmonic 2^2.8, 1 + 3·e^−½ one σ (0.75 octave) up.
	f.body = 1.0f;
	f.bodyAt = 0.5f;
	const double c = 1.0 + 3.6 * 0.5;
	CHECK(envelopeAt(f, std::exp2(c)) == doctest::Approx(std::exp2(-c * p) * 4.0));
	CHECK(envelopeAt(f, std::exp2(c + 0.75)) == doctest::Approx(std::exp2(-(c + 0.75) * p) * (1.0 + 3.0 * std::exp(-0.5))));
}

TEST_CASE("PAL sparse country: at full sparsity only the fundamental and the kept few of 2..7 sound, at drawbar levels; it glides in the set and fades in with the field") {
	PartialFields f {};
	f.rolloff = 0.5f; // p = 1
	f.sparsity = 1.0f;
	for (const float set : { 0.0f, 0.13f, 0.5f, 0.87f }) {
		CAPTURE(set);
		f.sparseSet = set;
		CHECK(fineAt(f, 1, 0.0f) == 1.0);
		int kept = 0;
		for (int k = 2; k <= 7; ++k) {
			CAPTURE(k);
			// THE RESTATEMENT: keep = smoothstep(((tri(k·φ + 1.5·set) − 0.6) / 0.4) + 0.5), the drawbar boost k^(0.8·p).
			const double x = k * std::numbers::phi + 1.5 * static_cast<double>(set);
			const double tri = std::abs(2.0 * (x - std::floor(x)) - 1.0);
			const double t = std::clamp((tri - 0.6) / 0.4 + 0.5, 0.0, 1.0);
			const double keep = t * t * (3.0 - 2.0 * t);
			CHECK(sparseKeep(k, set) == doctest::Approx(keep));
			CHECK(fineAt(f, k, 0.0f) == doctest::Approx(keep * std::pow(static_cast<double>(k), 0.8)));
			kept += keep > 0.5 ? 1 : 0;
		}
		CHECK(kept >= 1);
		CHECK(kept <= 4); // a FEW: sparse country is the fundamental and a handful
		for (const int k : { 8, 9, 20, 320 }) {
			CHECK(fineAt(f, k, 0.0f) == 0.0);
		}
	}
	// IT GLIDES: over a fine sweep of the set, no partial's gain steps (the triangle pattern is continuous where a frac would wrap).
	double worst = 0.0;
	for (int k = 2; k <= 7; ++k) {
		double previous = sparseKeep(k, 0.0f);
		for (int i = 1; i <= 4000; ++i) {
			const double now = sparseKeep(k, static_cast<float>(i) / 4000.0f);
			worst = std::max(worst, std::abs(now - previous));
			previous = now;
		}
	}
	CHECK(worst < 0.02);
	// AND IT FADES IN WITH THE FIELD: at sparsity 0 nothing at all; a hair above, a hair of change.
	f.sparsity = 0.0f;
	CHECK(fineAt(f, 12, 0.0f) == 1.0);
	f.sparsity = 1e-4f;
	CHECK(std::abs(fineAt(f, 12, 0.0f) - 1.0) < 1e-3);
	// SMOOTHNESS TAKES PAL'S FIELDS AWAY with the rest: the smooth spectrum is the plain power law.
	PartialFields s {};
	s.knee = s.shelf = s.body = s.sparsity = 0.8f;
	s.smoothness = 1.0f;
	const PartialFields sm = smoothed(s);
	CHECK(sm.knee == 0.0f);
	CHECK(sm.shelf == 0.0f);
	CHECK(sm.body == 0.0f);
	CHECK(sm.sparsity == 0.0f);
}
