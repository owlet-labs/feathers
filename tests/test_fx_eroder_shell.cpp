/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE ERODER'S SHARED SECTION: EmphasisPair, Smear, LoopLimiter, Slew (src/fx/eroder/shell.h). Every expected value
// is restated here from the definition, not read back from the code.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/eroder/shell.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace feathers::fx::eroder;

namespace {

constexpr double kPi = std::numbers::pi;

bool sameBits(float a, float b) {
	return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

struct Noise {
	std::uint32_t s = 12345u;
	float next() { // uniform in [-1, 1)
		s = s * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f;
	}
};

std::complex<double> dftAt(const std::vector<float>& x, double hz, double rate, std::size_t from = 0) {
	std::complex<double> sum = 0.0;
	const double w = 2.0 * kPi * hz / rate;
	for (std::size_t n = from; n < x.size(); ++n) {
		sum += static_cast<double>(x[n]) * std::polar(1.0, -w * static_cast<double>(n - from));
	}
	return sum;
}

template <class F>
std::vector<float> impulseResponse(F&& f, std::size_t length) {
	std::vector<float> h(length);
	for (std::size_t n = 0; n < length; ++n) {
		h[n] = f(n == 0 ? 1.0f : 0.0f);
	}
	return h;
}

double dbOf(double x) { return 20.0 * std::log10(x); }

// ---- emphasis -------------------------------------------------------------------------------------------------

EmphasisSettings emphasis(EmphasisShape shape, float db, float hz = 1000.0f, float width = 1.0f, bool restore = true) {
	EmphasisSettings s;
	s.shape = shape;
	s.amountDb = db;
	s.hz = hz;
	s.widthOctaves = width;
	s.restore = restore;
	return s;
}

// The tilt, restated: with w = tan(pi f / rate) / tan(pi pivot / rate) and g = 10^(dB/20),
// |H|^2 = (g^2 w^2 + 1) / (w^2 + g^2).
double tiltDb(double db, double pivot, double f, double rate) {
	const double g = std::pow(10.0, db / 20.0);
	const double w = std::tan(kPi * f / rate) / std::tan(kPi * pivot / rate);
	return 10.0 * std::log10((g * g * w * w + 1.0) / (w * w + g * g));
}

// The peaking bell, restated from its transfer function: the analogue prototype
// (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1) evaluated at s = j tan(w/2) / tan(w0/2) is NOT what the digital section is,
// so the digital one is written out directly.
double bellDb(double db, double centre, double octaves, double f, double rate) {
	const double a = std::pow(10.0, db / 40.0);
	const double w0 = 2.0 * kPi * centre / rate;
	const double alpha = std::sin(w0) * std::sinh(std::log(2.0) / 2.0 * octaves * w0 / std::sin(w0));
	const double w = 2.0 * kPi * f / rate;
	const std::complex<double> e1 = std::polar(1.0, -w);
	const std::complex<double> e2 = e1 * e1;
	const std::complex<double> num = (1.0 + alpha * a) - 2.0 * std::cos(w0) * e1 + (1.0 - alpha * a) * e2;
	const std::complex<double> den = (1.0 + alpha / a) - 2.0 * std::cos(w0) * e1 + (1.0 - alpha / a) * e2;
	return dbOf(std::abs(num / den));
}

double measuredEmphasisDb(EmphasisPair& pair, double f, double rate) {
	pair.reset();
	const auto h = impulseResponse([&pair](float x) { return pair.in(0, x); }, 1 << 15);
	return dbOf(std::abs(dftAt(h, f, rate)));
}

double maxInverseError(EmphasisPair& pair, const std::vector<float>& x) {
	pair.reset();
	double peak = 0.0;
	for (float v : x) {
		peak = std::max(peak, static_cast<double>(std::abs(v)));
	}
	double worst = 0.0;
	for (float v : x) {
		const float back = pair.out(0, pair.in(0, v));
		worst = std::max(worst, std::abs(static_cast<double>(back) - v) / peak);
	}
	return worst;
}

} // namespace

TEST_CASE("emphasis: amount 0 is bit-identical in both shapes, both ways") {
	Noise noise;
	for (EmphasisShape shape : {EmphasisShape::Tilt, EmphasisShape::Bell}) {
		EmphasisPair pair;
		pair.prepare(48000.0);
		pair.set(emphasis(shape, 0.0f));
		for (int n = 0; n < 4096; ++n) {
			const float x = noise.next() * 3.0f;
			CHECK(sameBits(pair.in(n & 1, x), x));
			CHECK(sameBits(pair.out(n & 1, x), x));
		}
	}
}

TEST_CASE("emphasis: out undoes in for noise and sines, both shapes, at the largest amounts") {
	for (EmphasisShape shape : {EmphasisShape::Tilt, EmphasisShape::Bell}) {
		for (float db : {18.0f, -18.0f}) {
			EmphasisPair pair;
			pair.prepare(48000.0);
			pair.set(emphasis(shape, db, 1000.0f, 1.0f));
			std::vector<float> noise(20000);
			Noise gen;
			for (float& v : noise) {
				v = gen.next();
			}
			CHECK(maxInverseError(pair, noise) < 1.0e-5);
			for (double f : {100.0, 1000.0, 3000.0, 10000.0}) {
				std::vector<float> sine(20000);
				for (std::size_t n = 0; n < sine.size(); ++n) {
					sine[n] = static_cast<float>(0.8 * std::sin(2.0 * kPi * f * static_cast<double>(n) / 48000.0));
				}
				CAPTURE(f);
				CAPTURE(db);
				CHECK(maxInverseError(pair, sine) < 1.0e-5);
			}
		}
	}
}

TEST_CASE("emphasis: the tilt's magnitude is the shelf the definition gives") {
	EmphasisPair pair;
	pair.prepare(48000.0);
	for (double db : {18.0, -18.0, 7.0}) {
		pair.set(emphasis(EmphasisShape::Tilt, static_cast<float>(db), 1000.0f));
		for (double f : {10.0, 100.0, 500.0, 1000.0, 2000.0, 8000.0, 20000.0}) {
			CAPTURE(db);
			CAPTURE(f);
			CHECK(std::abs(measuredEmphasisDb(pair, f, 48000.0) - tiltDb(db, 1000.0, f, 48000.0)) < 0.01);
		}
		CHECK(std::abs(measuredEmphasisDb(pair, 1000.0, 48000.0)) < 0.01);
		// Far below the pivot the lows are cut by the amount, far above it the highs are lifted by it.
		CHECK(std::abs(measuredEmphasisDb(pair, 10.0, 48000.0) + db) < 0.1 * std::abs(db));
		CHECK(std::abs(measuredEmphasisDb(pair, 20000.0, 48000.0) - db) < 0.1 * std::abs(db));
	}
}

TEST_CASE("emphasis: the bell is +amount at its centre and nothing far from it") {
	EmphasisPair pair;
	pair.prepare(48000.0);
	for (double db : {18.0, -18.0, 6.0}) {
		for (double width : {0.5, 1.0, 3.0}) {
			pair.set(emphasis(EmphasisShape::Bell, static_cast<float>(db), 1000.0f, static_cast<float>(width)));
			CAPTURE(db);
			CAPTURE(width);
			CHECK(std::abs(measuredEmphasisDb(pair, 1000.0, 48000.0) - db) < 0.01);
			for (double f : {20.0, 250.0, 700.0, 1400.0, 4000.0, 15000.0}) {
				CAPTURE(f);
				CHECK(std::abs(measuredEmphasisDb(pair, f, 48000.0) - bellDb(db, 1000.0, width, f, 48000.0)) < 0.01);
			}
		}
	}
	// Two widths away (1 octave wide, so two octaves) the skirt is down to about a decibel (measured 1.06 dB, and the
	// restated response above agrees to 0.01 dB).
	pair.set(emphasis(EmphasisShape::Bell, 18.0f, 1000.0f, 1.0f));
	CHECK(std::abs(measuredEmphasisDb(pair, 250.0, 48000.0)) < 1.5);
	CHECK(std::abs(measuredEmphasisDb(pair, 4000.0, 48000.0)) < 1.5);
	// At half the gain (9 dB) the edges are one width apart: the width is in octaves between the half-gain points.
	const double lo = measuredEmphasisDb(pair, 1000.0 / std::sqrt(2.0), 48000.0);
	const double hi = measuredEmphasisDb(pair, 1000.0 * std::sqrt(2.0), 48000.0);
	CHECK(lo > 8.0);
	CHECK(lo < 10.0);
	CHECK(hi > 8.0);
	CHECK(hi < 10.0);
}

TEST_CASE("emphasis: restore off leaves out a pass-through, bit for bit") {
	Noise noise;
	for (EmphasisShape shape : {EmphasisShape::Tilt, EmphasisShape::Bell}) {
		EmphasisPair pair;
		pair.prepare(48000.0);
		pair.set(emphasis(shape, 12.0f, 1000.0f, 1.0f, false));
		bool inMoved = false;
		for (int n = 0; n < 2000; ++n) {
			const float x = noise.next();
			inMoved |= !sameBits(pair.in(0, x), x);
			CHECK(sameBits(pair.out(0, x), x));
		}
		CHECK(inMoved);
	}
}

TEST_CASE("emphasis: a tail reaches exact zero") {
	for (EmphasisShape shape : {EmphasisShape::Tilt, EmphasisShape::Bell}) {
		EmphasisPair pair;
		pair.prepare(48000.0);
		pair.set(emphasis(shape, 18.0f, 1000.0f, 1.0f));
		Noise noise;
		for (int n = 0; n < 5000; ++n) {
			const float y = pair.out(0, pair.in(0, noise.next()));
			(void)y;
		}
		float last = 1.0f;
		int firstZero = -1;
		for (int n = 0; n < 200000; ++n) {
			last = pair.out(0, pair.in(0, 0.0f));
			if (last == 0.0f && firstZero < 0) {
				firstZero = n;
			}
			if (firstZero >= 0 && last != 0.0f) {
				firstZero = -1;
			}
		}
		CHECK(firstZero >= 0);
		CHECK(sameBits(last, 0.0f));
		// And the filters are empty: a fresh impulse after the tail gives what a fresh pair gives.
		EmphasisPair fresh;
		fresh.prepare(48000.0);
		fresh.set(emphasis(shape, 18.0f, 1000.0f, 1.0f));
		for (int n = 0; n < 64; ++n) {
			const float x = n == 0 ? 1.0f : 0.0f;
			CHECK(sameBits(pair.in(0, x), fresh.in(0, x)));
		}
	}
}

// ---- smear ----------------------------------------------------------------------------------------------------

namespace {

Smear makeSmear(double rate, float amount, float hz) {
	Smear s;
	s.prepare(rate);
	SmearSettings st;
	st.amount = amount;
	st.hz = hz;
	s.set(st);
	return s;
}

} // namespace

TEST_CASE("smear: amount 0 is bit-identical") {
	Noise noise;
	Smear s = makeSmear(48000.0, 0.0f, 1000.0f);
	for (int n = 0; n < 4096; ++n) {
		const float x = noise.next();
		CHECK(sameBits(s.process(n & 1, x), x));
	}
	CHECK(s.delayAtCentre() == 0.0);
}

TEST_CASE("smear: it is an allpass, flat to 0.01 dB") {
	for (float amount : {0.5f, 1.0f}) {
		Smear s = makeSmear(48000.0, amount, 1000.0f);
		const auto h = impulseResponse([&s](float x) { return s.process(0, x); }, 1 << 17);
		for (double f : {50.0, 400.0, 900.0, 1000.0, 1100.0, 2500.0, 9000.0, 20000.0}) {
			CAPTURE(amount);
			CAPTURE(f);
			CHECK(std::abs(dbOf(std::abs(dftAt(h, f, 48000.0)))) < 0.01);
		}
	}
}

TEST_CASE("smear: the group delay at the centre is what delayAtCentre says") {
	double previous = 0.0;
	for (float amount : {0.25f, 0.5f, 1.0f}) {
		Smear s = makeSmear(48000.0, amount, 1000.0f);
		const auto h = impulseResponse([&s](float x) { return s.process(0, x); }, 1 << 17);
		// -d(phase)/dw from two points either side of the centre, off the running filter's own impulse response.
		const double f1 = 1000.0 * 0.998;
		const double f2 = 1000.0 * 1.002;
		const double dphi = std::arg(dftAt(h, f2, 48000.0) / dftAt(h, f1, 48000.0));
		const double measured = -dphi / (2.0 * kPi * (f2 - f1) / 48000.0);
		CAPTURE(amount);
		CHECK(measured == doctest::Approx(s.delayAtCentre()).epsilon(0.02));
		// The analogue allpass has 4 Q / w0 samples of delay at its centre, so eight of them: the figure is of that order.
		const double q = 0.5 * std::pow(16.0, amount);
		const double analogue = 8.0 * 4.0 * q / (2.0 * kPi * 1000.0 / 48000.0);
		CHECK(s.delayAtCentre() == doctest::Approx(analogue).epsilon(0.05));
		CHECK(s.delayAtCentre() > previous);
		previous = s.delayAtCentre();
	}
}

TEST_CASE("smear: a square wave's crest factor rises and its power does not move") {
	const double rate = 48000.0;
	Smear s = makeSmear(rate, 1.0f, 1000.0f);
	const int period = 480; // 100 Hz
	std::vector<float> in;
	std::vector<float> out;
	for (int n = 0; n < period * 40; ++n) {
		const float x = (n % period) < period / 2 ? 1.0f : -1.0f;
		in.push_back(x);
		out.push_back(s.process(0, x));
	}
	double sumIn = 0.0;
	double sumOut = 0.0;
	double peakOut = 0.0;
	for (std::size_t n = in.size() - period * 10; n < in.size(); ++n) {
		sumIn += static_cast<double>(in[n]) * in[n];
		sumOut += static_cast<double>(out[n]) * out[n];
		peakOut = std::max(peakOut, static_cast<double>(std::abs(out[n])));
	}
	const double rmsIn = std::sqrt(sumIn / (period * 10));
	const double rmsOut = std::sqrt(sumOut / (period * 10));
	CHECK(rmsIn == doctest::Approx(1.0));
	CHECK(std::abs(dbOf(rmsOut / rmsIn)) < 0.1);
	CHECK(peakOut / rmsOut > 1.2); // the square's own crest factor is 1.0
}

TEST_CASE("smear: a tail reaches exact zero") {
	Smear s = makeSmear(48000.0, 1.0f, 1000.0f);
	Noise noise;
	for (int n = 0; n < 5000; ++n) {
		(void)s.process(0, noise.next());
		(void)s.process(1, noise.next());
	}
	float l = 1.0f;
	float r = 1.0f;
	for (int n = 0; n < 400000; ++n) {
		l = s.process(0, 0.0f);
		r = s.process(1, 0.0f);
	}
	CHECK(sameBits(l, 0.0f));
	CHECK(sameBits(r, 0.0f));
}

// ---- loop limiter ---------------------------------------------------------------------------------------------

TEST_CASE("limiter: under the threshold it is bit-identical") {
	LoopLimiter lim;
	lim.prepare(48000.0);
	Noise noise;
	for (int n = 0; n < 8192; ++n) {
		float l = noise.next() * 0.99f;
		float r = noise.next() * 0.99f;
		const float l0 = l;
		const float r0 = r;
		lim.process(l, r);
		CHECK(sameBits(l, l0));
		CHECK(sameBits(r, r0));
		CHECK(lim.gain() == 1.0f);
	}
}

TEST_CASE("limiter: a sine at four times the threshold comes out at the threshold, one gain for both channels") {
	LoopLimiter lim;
	lim.prepare(48000.0);
	lim.setThreshold(0.5f);
	float peak = 0.0f;
	for (int n = 0; n < 48000; ++n) {
		const double ph = 2.0 * kPi * 440.0 * n / 48000.0;
		float l = static_cast<float>(2.0 * std::sin(ph));
		float r = static_cast<float>(1.0 * std::sin(ph * 0.5 + 0.3)); // different, and smaller
		const float l0 = l;
		const float r0 = r;
		lim.process(l, r);
		if (n >= 24000) {
			peak = std::max(peak, std::abs(l));
		}
		if (std::abs(l0) > 0.3f && std::abs(r0) > 0.3f) {
			CHECK(l / l0 == doctest::Approx(r / r0).epsilon(1.0e-5));
			CHECK(l / l0 == doctest::Approx(lim.gain()).epsilon(1.0e-5));
		}
	}
	CHECK(peak == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(peak <= 0.5f);
}

TEST_CASE("limiter: the gain is back in step with the release time") {
	// A burst of 4 against a threshold of 1: the envelope is 4 and the gain 0.25 on the last burst frame; the envelope
	// then falls by 1/e per release time, so the gain has risen by e after one release time and sqrt(e) after half of one
	// (a second release time would take it past the threshold's own 1.0).
	LoopLimiter lim;
	lim.prepare(48000.0);
	const int release = 2400; // 50 ms
	for (int n = 0; n < 1000; ++n) {
		float l = 4.0f;
		float r = -4.0f;
		lim.process(l, r);
	}
	const float g0 = lim.gain();
	CHECK(g0 == doctest::Approx(0.25f).epsilon(1.0e-5));
	float gains[2] = {};
	for (int n = 1; n <= release; ++n) {
		float l = 0.0f;
		float r = 0.0f;
		lim.process(l, r);
		if (n == release) {
			gains[0] = lim.gain();
		}
		if (n == release / 2) {
			gains[1] = lim.gain();
		}
	}
	CHECK(gains[0] / g0 == doctest::Approx(std::exp(1.0)).epsilon(0.1));
	CHECK(gains[1] / g0 == doctest::Approx(std::exp(0.5)).epsilon(0.1));
	// A different release time scales the same way.
	LoopLimiter quick;
	quick.prepare(48000.0);
	quick.setRelease(0.010);
	for (int n = 0; n < 1000; ++n) {
		float l = 4.0f;
		float r = 4.0f;
		quick.process(l, r);
	}
	const float q0 = quick.gain();
	for (int n = 1; n <= 480; ++n) {
		float l = 0.0f;
		float r = 0.0f;
		quick.process(l, r);
	}
	CHECK(quick.gain() / q0 == doctest::Approx(std::exp(1.0)).epsilon(0.1));
}

TEST_CASE("limiter: noise never gets past the threshold on any frame") {
	LoopLimiter lim;
	lim.prepare(48000.0);
	lim.setThreshold(0.7f);
	Noise noise;
	float peak = 0.0f;
	for (int n = 0; n < 200000; ++n) {
		const float scale = (n / 5000) % 2 == 0 ? 8.0f : 0.3f;
		float l = noise.next() * scale;
		float r = noise.next() * scale;
		lim.process(l, r);
		CHECK(std::abs(l) <= 0.7f);
		CHECK(std::abs(r) <= 0.7f);
		peak = std::max({peak, std::abs(l), std::abs(r)});
	}
	CHECK(peak > 0.69f);
}

// ---- slew -----------------------------------------------------------------------------------------------------

namespace {

Slew makeSlew(double rate, float hz, float curve, float balance) {
	Slew s;
	s.prepare(rate);
	SlewSettings st;
	st.on = true;
	st.hz = hz;
	st.curve = curve;
	st.balance = balance;
	s.set(st);
	return s;
}

std::vector<float> slewSine(Slew& s, double rate, double f, double amp, int frames) {
	std::vector<float> y(static_cast<std::size_t>(frames));
	for (int n = 0; n < frames; ++n) {
		y[static_cast<std::size_t>(n)] = s.process(0, static_cast<float>(amp * std::sin(2.0 * kPi * f * n / rate)));
	}
	return y;
}

double harmonicAmplitude(const std::vector<float>& y, double f, double rate, std::size_t from) {
	return 2.0 * std::abs(dftAt(y, f, rate, from)) / static_cast<double>(y.size() - from);
}

} // namespace

TEST_CASE("slew: off is bit-identical") {
	Slew s;
	s.prepare(48000.0);
	Noise noise;
	for (int n = 0; n < 4096; ++n) {
		const float x = noise.next();
		CHECK(sameBits(s.process(n & 1, x), x));
	}
	Slew t = makeSlew(48000.0, 100.0f, 0.5f, 0.3f);
	SlewSettings off;
	off.on = false;
	t.set(off);
	for (int n = 0; n < 4096; ++n) {
		const float x = noise.next();
		CHECK(sameBits(t.process(n & 1, x), x));
	}
}

TEST_CASE("slew: at curve 0 a sine under the rate passes, and one far over it is a triangle of slope L") {
	const double rate = 48000.0;
	{
		// The steepest slope of a 0.5-peak sine at f is pi f / rate; L is 2 hz / rate; so anything under 2/pi hz passes.
		Slew s = makeSlew(rate, 1000.0f, 0.0f, 0.0f);
		const auto y = slewSine(s, rate, 500.0, 0.5, 48000);
		double worst = 0.0;
		for (int n = 0; n < 48000; ++n) {
			worst = std::max(worst, std::abs(static_cast<double>(y[static_cast<std::size_t>(n)])
			                                 - 0.5 * std::sin(2.0 * kPi * 500.0 * n / rate)));
		}
		CHECK(worst < 1.0e-6);
	}
	// 3 kHz against a 20 Hz rate: y climbs at L for a half period, falls at L for the next.
	double peaks[2] = {};
	int index = 0;
	for (double r : {48000.0, 96000.0}) {
		Slew s = makeSlew(r, 20.0f, 0.0f, 0.0f);
		const double l = 4.0 * 0.5 * 20.0 / r;
		CHECK(s.limit() == doctest::Approx(static_cast<float>(l)).epsilon(1.0e-6));
		const auto y = slewSine(s, r, 3000.0, 0.5, static_cast<int>(r / 2));
		float peak = 0.0f;
		for (std::size_t n = static_cast<std::size_t>(r / 4); n < y.size(); ++n) {
			CHECK(std::abs(std::abs(static_cast<double>(y[n]) - y[n - 1]) - l) < 1.0e-8);
			peak = std::max(peak, y[n]);
		}
		// Half a period is N = rate / (2 f) samples of rise by L, so the peak-to-peak is N L and the peak N L / 2:
		// rate / (4 f) * 4 * 0.5 * hz / rate = hz / (2 f), whatever the rate.
		CHECK(peak == doctest::Approx(20.0 / (2.0 * 3000.0)).epsilon(0.01));
		peaks[index++] = peak;
	}
	CHECK(peaks[0] == doctest::Approx(peaks[1]).epsilon(0.01));
}

TEST_CASE("slew: at curve 1 it is a one-pole lowpass with its cutoff at the rate, and adds nothing") {
	const double rate = 48000.0;
	Slew s = makeSlew(rate, 1000.0f, 1.0f, 0.0f);
	const double k = 1.0 - std::exp(-2.0 * kPi * 1000.0 / rate);
	for (double f : {300.0, 1000.0, 4000.0}) {
		Slew t = makeSlew(rate, 1000.0f, 1.0f, 0.0f);
		const auto y = slewSine(t, rate, f, 0.1, 48000);
		const double gain = 2.0 * std::abs(dftAt(y, f, rate, 24000)) / 24000.0 / 0.1;
		const double w = 2.0 * kPi * f / rate;
		const double expected = k / std::abs(1.0 - (1.0 - k) * std::polar(1.0, -w));
		CAPTURE(f);
		CHECK(std::abs(dbOf(gain) - dbOf(expected)) < 0.1);
	}
	const auto y = slewSine(s, rate, 1000.0, 0.5, 48000);
	const double fundamental = harmonicAmplitude(y, 1000.0, rate, 24000);
	for (int h = 2; h <= 8; ++h) {
		CAPTURE(h);
		CHECK(dbOf(harmonicAmplitude(y, 1000.0 * h, rate, 24000) / fundamental) < -100.0);
	}
}

TEST_CASE("slew: between 0 and 1 the cap on the step is the blend of the two, continuous in curve") {
	const double rate = 48000.0;
	const double l = 4.0 * 0.5 * 1000.0 / rate;
	const double k = 1.0 - std::exp(-2.0 * kPi * 1000.0 / rate);
	for (float curve : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
		for (double target : {0.01, 0.2, 1.0}) {
			Slew s = makeSlew(rate, 1000.0f, curve, 0.0f);
			const double step = s.process(0, static_cast<float>(target));
			const double expected = std::min(target, (1.0 - curve) * l + curve * k * target);
			CAPTURE(curve);
			CAPTURE(target);
			CHECK(step == doctest::Approx(expected).epsilon(1.0e-5));
		}
	}
}

TEST_CASE("slew: balance makes rising and falling steps differ by the stated ratio, and a second harmonic") {
	const double rate = 48000.0;
	const double f = 1000.0;
	const double hz = 10.0;
	const auto run = [&](float balance) {
		Slew s = makeSlew(rate, static_cast<float>(hz), 0.0f, balance);
		return slewSine(s, rate, f, 0.5, 48000);
	};
	{
		const auto y = run(0.0f);
		const double h1 = harmonicAmplitude(y, f, rate, 24000);
		CHECK(dbOf(harmonicAmplitude(y, 2.0 * f, rate, 24000) / h1) < -80.0);
	}
	for (float balance : {0.5f, -0.5f, 1.0f}) {
		const auto y = run(balance);
		float maxRise = 0.0f;
		float maxFall = 0.0f;
		float hi = -1.0f;
		float lo = 1.0f;
		for (std::size_t n = 24001; n < y.size(); ++n) {
			maxRise = std::max(maxRise, y[n] - y[n - 1]);
			maxFall = std::max(maxFall, y[n - 1] - y[n]);
			hi = std::max(hi, y[n]);
			lo = std::min(lo, y[n]);
		}
		const double ratio = std::exp2(2.0 * balance); // 2^b up over 2^-b down
		CAPTURE(balance);
		CHECK(static_cast<double>(maxRise) / maxFall == doctest::Approx(ratio).epsilon(1.0e-4));
		// The asymmetric triangle: rising for the fraction p = down / (up + down) of the period, peak-to-peak A, has
		// harmonic k of amplitude A sin(k pi p) / (pi^2 k^2 p (1 - p)).
		const double p = std::exp2(-balance) / (std::exp2(balance) + std::exp2(-balance));
		const double a = hi - lo;
		const double expected2 = a * std::sin(2.0 * kPi * p) / (kPi * kPi * 4.0 * p * (1.0 - p));
		const double h2 = harmonicAmplitude(y, 2.0 * f, rate, 24000);
		CHECK(h2 > 0.0);
		CHECK(h2 == doctest::Approx(std::abs(expected2)).epsilon(0.05));
		const double expected1 = a * std::sin(kPi * p) / (kPi * kPi * p * (1.0 - p));
		CHECK(harmonicAmplitude(y, f, rate, 24000) == doctest::Approx(expected1).epsilon(0.05));
	}
}

TEST_CASE("slew: a tail reaches exact zero at every curve") {
	for (float curve : {0.0f, 0.5f, 1.0f}) {
		Slew s = makeSlew(48000.0, 200.0f, curve, 0.3f);
		(void)slewSine(s, 48000.0, 440.0, 0.5, 10000);
		float last = 1.0f;
		for (int n = 0; n < 400000; ++n) {
			last = s.process(0, 0.0f);
		}
		CAPTURE(curve);
		CHECK(sameBits(last, 0.0f));
	}
}
