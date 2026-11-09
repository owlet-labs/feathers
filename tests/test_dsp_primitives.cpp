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
 */

// THE THREE PRIMITIVES THE LICENCE MIGRATION WROTE (2026-09-24) — fast tan/tanh, the TPT one-pole
// and the windowed-sinc delay line — each held to the NUMBER its header states, not to "it renders
// something". The filter suites above them check the devices; these check that the parts are what
// they say they are, so a regression names the part rather than a device three layers up.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/elliptic_highpass.h"
#include "dsp/fast_math_approx.h"
#include "dsp/sinc_delay_line.h"
#include "dsp/tpt_one_pole.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace feathers::dsp;

namespace {

double dB(double x) { return 20.0 * std::log10(x); }

/// |X(f)|·e^{i·arg} of a finite response at f cycles per sample.
std::complex<double> dft(const std::vector<double>& h, double f) {
	std::complex<double> sum {0.0, 0.0};
	for (size_t n = 0; n < h.size(); ++n) {
		sum += h[n] * std::exp(std::complex<double>(0.0, -2.0 * std::numbers::pi * f * static_cast<double>(n)));
	}
	return sum;
}

} // namespace

//--------------------------------------------------------------------------------------------//
// fasttan / fasttanh                                                                           //
//--------------------------------------------------------------------------------------------//

TEST_CASE("fast math: tan stays inside its stated bound over the prewarp's whole range") {
	// THE PREWARP'S RANGE IS fc/sr IN [0, 0.499], so the argument is π·[0, 0.499] — up to within a
	// hair of the pole. The header's bound is 3e-7 relative; MEASURED 2.5e-7, at 0.489·π. Without the
	// range reduction the same sweep reads 1.4e-5 at 0.499·π, which is what this bound is for.
	double worst = 0.0;
	double worstAt = 0.0;
	int checked = 0;
	for (int i = 1; i <= 499000; ++i) {
		const float x = static_cast<float>(std::numbers::pi * (static_cast<double>(i) * 1e-6));
		const double truth = std::tan(static_cast<double>(x));
		const double rel = std::abs(static_cast<double>(fasttan(x)) - truth) / truth;
		if (rel > worst) {
			worst = rel;
			worstAt = static_cast<double>(i) * 1e-6;
		}
		++checked;
	}
	CAPTURE(worst);
	CAPTURE(worstAt);
	CHECK(worst < 3e-7);
	CHECK(checked == 499000);

	// ODD, and exact at zero.
	for (float x : {0.1f, 0.7f, 1.2f, 1.55f}) {
		CAPTURE(x);
		CHECK(fasttan(-x) == -fasttan(x));
	}
	CHECK(fasttan(0.0f) == 0.0f);
}

TEST_CASE("fast math: the four-lane tan is the scalar tan, lane for lane, to the bit") {
	// The SVF's two-corner setCoeff takes its g from the four-lane form and the drawn curve from the
	// scalar one; the automod's bit-identity cases rest on the two agreeing exactly.
	const float in[4] = {0.01f, 0.5f, 1.3f, 1.567f};
	alignas(16) float out[4];
	_mm_store_ps(out, fasttanSSE(_mm_loadu_ps(in)));
	for (int i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(out[i] == fasttan(in[i]));
	}
}

TEST_CASE("fast math: tanh stays inside its stated bound over the drive stage's range") {
	// EVERY CALLER CLAMPS TO ±4.5, so that is the range the bound covers. MEASURED: 4.21e-5 at the
	// ends, under 1e-6 inside ±2. The [7/6] Padé form exceeds 1 past about ±5 — asserted too, so the
	// "callers must clamp" note in the header stays true rather than merely stated.
	double worstAll = 0.0;
	double worstInner = 0.0;
	for (int i = -450000; i <= 450000; ++i) {
		const float x = static_cast<float>(i) * 1e-5f;
		const double err = std::abs(static_cast<double>(fasttanh(x)) - std::tanh(static_cast<double>(x)));
		worstAll = std::max(worstAll, err);
		if (std::abs(x) <= 2.0f) {
			worstInner = std::max(worstInner, err);
		}
	}
	CAPTURE(worstAll);
	CAPTURE(worstInner);
	CHECK(worstAll < 4.3e-5);
	CHECK(worstInner < 1e-6);
	CHECK(fasttanh(0.0f) == 0.0f);
	CHECK(fasttanh(-1.5f) == -fasttanh(1.5f));
	// The values at the drive stage's makeup end, which is where the curve's shape is set.
	CHECK(std::abs(static_cast<double>(fasttanh(4.5f)) - 0.999795198) < 1e-6);
	CHECK(fasttanh(6.0f) > 1.0f);
}

//--------------------------------------------------------------------------------------------//
// the TPT one-pole                                                                             //
//--------------------------------------------------------------------------------------------//

TEST_CASE("one-pole: its coefficient is g/(1+g) with the prewarped g") {
	constexpr float kSr = 48000.0f;
	for (float fc : {20.0f, 440.0f, 5000.0f, 20000.0f}) {
		CAPTURE(fc);
		TptOnePole p;
		p.setCutoff(fc, kSr);
		const double g = std::tan(std::numbers::pi * static_cast<double>(fc) / static_cast<double>(kSr));
		CHECK(static_cast<double>(p.gainForTest()) == doctest::Approx(g / (1.0 + g)).epsilon(1e-6));
	}
}

TEST_CASE("one-pole: the corner is at -3.01 dB, DC is unity, and the highpass is the complement") {
	// THE BILINEAR TRANSFORM'S PROMISE with the prewarp: at exactly fc the discrete one-pole has the
	// analog prototype's |H| = 1/√2, i.e. −3.0103 dB. Measured off the impulse response of the running
	// filter — a value, at the frequency the claim is about.
	constexpr double kSr = 48000.0;
	for (double fc : {100.0, 1000.0, 8000.0}) {
		CAPTURE(fc);
		OnePoleLP lp;
		OnePoleHP hp;
		lp.setCutoff(static_cast<float>(fc), static_cast<float>(kSr));
		hp.setCutoff(static_cast<float>(fc), static_cast<float>(kSr));
		std::vector<double> hl(1 << 16), hh(1 << 16);
		for (size_t n = 0; n < hl.size(); ++n) {
			const float x = n == 0 ? 1.0f : 0.0f;
			hl[n] = lp.step(x);
			hh[n] = hp.step(x);
		}
		const double f = fc / kSr;
		CHECK(dB(std::abs(dft(hl, f))) == doctest::Approx(-3.0103).epsilon(1e-3));
		CHECK(dB(std::abs(dft(hh, f))) == doctest::Approx(-3.0103).epsilon(1e-3));
		// DC: the lowpass passes it whole and the highpass removes it.
		double sumL = 0.0, sumH = 0.0;
		for (size_t n = 0; n < hl.size(); ++n) {
			sumL += hl[n];
			sumH += hh[n];
		}
		CHECK(sumL == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(std::abs(sumH) < 1e-5);
		// And at Nyquist the lowpass has its zero.
		CHECK(std::abs(dft(hl, 0.5)) < 1e-4);
		// The closed form H(z) = g(z+1) / ((1+g)z + (g−1)), at a frequency that is not the corner.
		const double g = std::tan(std::numbers::pi * fc / kSr);
		const double f2 = 0.9 * f;
		const std::complex<double> z = std::exp(std::complex<double>(0.0, 2.0 * std::numbers::pi * f2));
		const std::complex<double> closed = g * (z + 1.0) / ((1.0 + g) * z + (g - 1.0));
		CHECK(std::abs(dft(hl, f2)) == doctest::Approx(std::abs(closed)).epsilon(1e-4));
	}
}

//--------------------------------------------------------------------------------------------//
// the windowed-sinc delay line                                                                 //
//--------------------------------------------------------------------------------------------//

namespace {

const SincTable& table() {
	static const SincTable t;
	return t;
}

/// The line's response to an impulse, read at `delay`: h[m] is the read m writes after the impulse
/// went in (m = 1 is the first read after it, when it is the most recent write).
std::vector<double> lineKernel(float delay, int length = 48) {
	SincDelayLine<1024> line(table());
	line.clear();
	std::vector<double> h(static_cast<size_t>(length), 0.0);
	line.write(1.0f);
	for (int m = 1; m < length; ++m) {
		h[static_cast<size_t>(m)] = line.read(delay);
		line.write(0.0f);
	}
	return h;
}

} // namespace

TEST_CASE("sinc line: a read of d returns the kernel's taps on the samples d writes back") {
	// THE CONTRACT BOTH COMBS DEPEND ON, as values: the tap on the sample `lag` writes back is
	// kernel(frac)[i + 6 − lag], where the most recent write is ONE write back. The comb reads before
	// it writes, so this is what makes a delay of d a delay of d.
	for (float delay : {6.0f, 20.0f, 20.25f, 20.5f, 37.9f, 1000.0f}) {
		CAPTURE(delay);
		const int whole = static_cast<int>(delay);
		float taps[SincTable::kTaps];
		table().kernel(delay - static_cast<float>(whole), taps);
		const std::vector<double> h = lineKernel(delay, whole + 16);
		for (int m = 1; m < static_cast<int>(h.size()); ++m) {
			const int j = whole + SincTable::kHalfTaps - m;
			const double expected = (j >= 0 && j < SincTable::kTaps) ? static_cast<double>(taps[j]) : 0.0;
			CAPTURE(m);
			CHECK(h[static_cast<size_t>(m)] == expected);
		}
	}
}

TEST_CASE("sinc line: the table rows are the stated windowed sinc, and it interpolates between them") {
	// Rows are exact samples of tapAt; between two rows the taps are the straight line between them,
	// so the delay moves continuously rather than in 1/256-sample steps.
	float row[SincTable::kTaps];
	float next[SincTable::kTaps];
	float mid[SincTable::kTaps];
	const float f0 = 37.0f / static_cast<float>(SincTable::kPhases);
	const float f1 = 38.0f / static_cast<float>(SincTable::kPhases);
	table().kernel(f0, row);
	table().kernel(f1, next);
	table().kernel(0.5f * (f0 + f1), mid);
	for (int j = 0; j < SincTable::kTaps; ++j) {
		CAPTURE(j);
		const double t = static_cast<double>(SincTable::kHalfTaps - j) - static_cast<double>(f0);
		CHECK(static_cast<double>(row[j]) == doctest::Approx(SincTable::tapAt(t)).epsilon(1e-6));
		CHECK(static_cast<double>(mid[j]) == doctest::Approx(0.5 * (row[j] + next[j])).epsilon(1e-6));
	}
	// The window: zero at its ends, and the centre tap of an integer delay is the bandwidth itself.
	CHECK(SincTable::tapAt(6.0) == 0.0);
	CHECK(SincTable::tapAt(0.0) == doctest::Approx(SincTable::kBandwidth));
}

TEST_CASE("sinc line: the interpolator's response is the parity numbers it was voiced with") {
	// THE PARITY DECISION, PINNED. The line this replaced was measured as a black box on 2026-09-24:
	// DC 0.99950 to 0.99955, −0.057 dB at 0.05·sr, −0.406 dB at 0.1·sr, −3.93 dB at 0.2·sr, −15.06 dB
	// at 0.3·sr, at every fraction to within 0.005 dB below 0.25·sr. Those are what the comb's decay and
	// brightness were tuned against, so a change here is a sound change and must be made on purpose.
	struct Point {
		double f;
		double db;
	};
	constexpr Point kPoints[] = {{0.05, -0.057}, {0.1, -0.406}, {0.2, -3.930}, {0.3, -15.06}};
	for (float frac : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f}) {
		CAPTURE(frac);
		const std::vector<double> h = lineKernel(20.0f + frac);
		double dc = 0.0;
		for (double v : h) {
			dc += v;
		}
		CHECK(dc == doctest::Approx(0.99953).epsilon(1e-4));
		for (const Point& p : kPoints) {
			CAPTURE(p.f);
			CHECK(std::abs(dB(std::abs(dft(h, p.f))) - p.db) < 0.01);
		}
	}
}

TEST_CASE("sinc line: a fractional delay is that delay, measured as phase on a low tone") {
	// WHAT A FRACTIONAL DELAY IS FOR. A tone at 0.01·sr read at d must come out d samples late — the
	// phase of the line's response at that frequency, divided by ω, is the group of samples it lags.
	// Linear-phase by symmetry at the row fractions, and within a thousandth of a sample between them.
	for (float delay : {6.0f, 6.37f, 20.5f, 100.125f, 300.9f}) {
		CAPTURE(delay);
		const std::vector<double> h = lineKernel(delay, static_cast<int>(delay) + 16);
		const double f = 0.01;
		const std::complex<double> H = dft(h, f);
		const double lag = -std::arg(H) / (2.0 * std::numbers::pi * f);
		// arg is only known modulo 2π, so fold the measured lag onto the one nearest the ask.
		const double period = 1.0 / f;
		const double folded = lag + period * std::round((static_cast<double>(delay) - lag) / period);
		CAPTURE(folded);
		CHECK(std::abs(folded - static_cast<double>(delay)) < 1e-3);
	}
}

//------------------------------------------------------------------------------------------------//
// THE KEYTRACKED BRICKWALL HIGH-PASS (backlog NL) — dsp/elliptic_highpass.h, which HARM's High-pass  //
// mode and the Filter's "Brickwall HP" type both run.                                              //
//------------------------------------------------------------------------------------------------//

namespace {

constexpr double kBwRate = 48000.0;

double noteHz(double note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }

/// THE AMPLITUDE A SINE AT `freq` COMES OUT AT, measured: a fresh brickwall at `f0`, three seconds to settle
/// (the sharpest section rings for tenths of a second at a 50 Hz passband edge), then a quadrature correlation
/// over a whole number of periods, so the reading has no leakage of its own.
double measuredGain(double f0, double freq) {
	EllipticHighpass hp;
	hp.setCorner(f0, kBwRate);
	const double w = 2.0 * std::numbers::pi * freq / kBwRate;
	const auto settle = static_cast<int64_t>(3.0 * kBwRate);
	const double periods = std::max(20.0, std::round(freq)); // about a second's worth
	const auto span = static_cast<int64_t>(std::llround(periods * kBwRate / freq));
	std::complex<double> acc {0.0, 0.0};
	for (int64_t n = 0; n < settle + span; ++n) {
		const float y = hp.process(static_cast<float>(std::sin(w * static_cast<double>(n))));
		if (n >= settle) {
			acc += static_cast<double>(y) * std::exp(std::complex<double>(0.0, -w * static_cast<double>(n)));
		}
	}
	return 2.0 * std::abs(acc) / static_cast<double>(span);
}

} // namespace

TEST_CASE("NL: the elliptic prototype meets its own spec — 0.1 dB passband from 2·f0 up, 60 dB stopband from f0 down") {
	const EllipticHighpassDesign& d = kEllipticHighpass;
	CAPTURE(d.selectivity);
	CAPTURE(d.margin);
	// ORDER 8 AT 0.1 dB / 60 dB BUYS A 1.18 TRANSITION — so the octave holds it with ~4.5 semitones either side.
	CHECK(d.selectivity == doctest::Approx(0.847).epsilon(0.005));
	CHECK(d.margin > 1.25);
	// THE TWO EDGES, exactly where the design puts them (the analog prototype, so no warp in the way).
	CHECK(dB(d.magnitudeAtRatio(2.0 / d.margin)) == doctest::Approx(-0.1).epsilon(1e-6));
	CHECK(dB(d.magnitudeAtRatio(d.margin)) == doctest::Approx(-60.0).epsilon(1e-6));
	// AND EVERYWHERE ELSE: every point of the passband from the octave up inside the ripple, every point of the
	// stopband from f0 down at least 60 dB under.
	double passWorst = 0.0;
	double passBest = -1.0;
	double stopWorst = -1000.0;
	for (int i = 0; i <= 2000; ++i) {
		const double up = std::exp2(1.0 + 7.0 * i / 2000.0);  // 2 .. 256
		const double down = std::exp2(-7.0 * i / 2000.0);     // 1 .. 1/128
		passWorst = std::min(passWorst, dB(d.magnitudeAtRatio(up)));
		passBest = std::max(passBest, dB(d.magnitudeAtRatio(up)));
		stopWorst = std::max(stopWorst, dB(d.magnitudeAtRatio(down)));
	}
	CAPTURE(passWorst);
	CAPTURE(passBest);
	CAPTURE(stopWorst);
	CHECK(passWorst >= -0.1 - 1e-9);
	CHECK(passBest <= 1e-9);
	CHECK(stopWorst <= -60.0 + 1e-9);
}

TEST_CASE("NL: the brickwall, measured, at every octave of the keyboard: f0 and below 60 dB down, 2·f0 and above within 0.1 dB") {
	// THE CORNER FOLLOWS THE NOTE: the same object retuned to six notes, each measured at its own harmonics.
	for (const double note : { 24.0, 36.0, 48.0, 60.0, 72.0, 84.0 }) {
		const double f0 = noteHz(note);
		CAPTURE(note);
		for (const double h : { 0.25, 0.5, 1.0 }) {
			CAPTURE(h);
			const double got = dB(measuredGain(f0, h * f0));
			const double drawn = dB(kEllipticHighpass.digitalMagnitude(f0, kBwRate, h * f0));
			CAPTURE(got);
			CAPTURE(drawn);
			CHECK(got <= -60.0);
			CHECK(drawn <= -60.0);
		}
		for (const double h : { 2.0, 3.0, 4.0, 6.0 }) {
			if (h * f0 > 0.45 * kBwRate) {
				continue;
			}
			CAPTURE(h);
			const double got = dB(measuredGain(f0, h * f0));
			const double drawn = dB(kEllipticHighpass.digitalMagnitude(f0, kBwRate, h * f0));
			CAPTURE(got);
			CAPTURE(drawn);
			CHECK(got >= -0.1 - 0.01);
			CHECK(got <= 0.01);
			// THE CLOSED FORM IS THE FILTER THAT RUNS — the curve the cards draw.
			CHECK(std::abs(got - drawn) < 0.01);
		}
		EllipticHighpass hp;
		hp.setCorner(f0, kBwRate);
		CHECK(hp.cornerHz() == f0);
	}
}

TEST_CASE("NL: jumping the brickwall's corner across eight octaves every 16 samples under full-scale noise stays bounded and settles") {
	// THE KEYTRACK SWEEP AT ITS WORST: a new corner every 16 samples (both owners' control grid), drawn at random
	// from 16 Hz to 4 kHz, for ten seconds of full-scale white noise. A direct-form section whose coefficients
	// jumped like this could blow up; the SVF's state is integrator charge and cannot be reinterpreted.
	EllipticHighpass hp;
	uint32_t seed = 0x2545F491u;
	const auto next = [&] {
		seed = seed * 1664525u + 1013904223u;
		return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
	};
	float peak = 0.0f;
	bool finite = true;
	const int total = static_cast<int>(10.0 * kBwRate);
	for (int n = 0; n < total; ++n) {
		if (n % 16 == 0) {
			hp.setCorner(16.0 * std::exp2(8.0 * next()), kBwRate);
		}
		const float y = hp.process(static_cast<float>(2.0 * next() - 1.0));
		finite = finite && std::isfinite(y);
		peak = std::max(peak, std::fabs(y));
	}
	// THE SAME NOISE THROUGH A CORNER THAT NEVER MOVES, for scale: what a still brickwall makes of it.
	float stillPeak = 0.0f;
	{
		EllipticHighpass still;
		uint32_t again = 0x2545F491u;
		const auto nextAgain = [&] {
			again = again * 1664525u + 1013904223u;
			return static_cast<double>(again >> 8) / static_cast<double>(1u << 24);
		};
		still.setCorner(256.0, kBwRate);
		for (int n = 0; n < total; ++n) {
			if (n % 16 == 0) {
				(void)nextAgain(); // keep the noise draws aligned with the moving run's
			}
			stillPeak = std::max(stillPeak, std::fabs(still.process(static_cast<float>(2.0 * nextAgain() - 1.0))));
		}
	}
	CAPTURE(peak);
	CAPTURE(stillPeak);
	CHECK(finite);
	// MEASURED: 3.46 moving against 1.78 still — the jumps add about 6 dB of transient and nothing grows.
	CHECK(peak < 4.0f);
	CHECK(peak < 2.5f * stillPeak);
	// AND IT COMES TO REST: a second of silence at a fixed corner leaves the output at EXACT zero (the state
	// flush in setCorner), which is what the plugin's SLEEP promise needs of a tailed device.
	hp.setCorner(100.0, kBwRate);
	float last = 1.0f;
	for (int n = 0; n < static_cast<int>(2.0 * kBwRate); ++n) {
		if (n % 16 == 0) {
			hp.setCorner(100.0, kBwRate);
		}
		last = hp.process(0.0f);
	}
	CHECK(last == 0.0f);
}
