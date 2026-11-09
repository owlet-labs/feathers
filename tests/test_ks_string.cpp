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

// THE KARPLUS-STRONG STRING ALONE (stages S1-S2). Every claim is read
// as a VALUE from the string's coefficients (its tuning, its loop gain) against an independent restatement of the
// formula, with a measurement of the render as the second assertion that the first is not vacuous.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numbers>
#include <vector>

#include "dsp/ks_string.h"

using namespace feathers::ks;

namespace {

std::atomic<long> gNews { 0 };

[[gnu::noinline]] void* countedAlloc(std::size_t n) {
	gNews.fetch_add(1, std::memory_order_relaxed);
	if (void* p = feathers::test::heapAlloc(n == 0 ? 1 : n)) {
		return p;
	}
	throw std::bad_alloc();
}

constexpr double kTwoPi = 2.0 * std::numbers::pi;

double cents(double ratio) { return 1200.0 * std::log2(ratio); }

double midiHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

/// A Hann-windowed DFT of x[start, start + len) at `hz`, its angle taken against ABSOLUTE time (reduced modulo one
/// cycle before it becomes an angle), so two windows' phases differ by 2π·(f − hz)·Δt for a sinusoid at f.
std::complex<double> dftAt(const std::vector<float>& x, size_t start, size_t len, double hz, double sr) {
	std::complex<double> acc {};
	double wsum = 0.0;
	for (size_t i = 0; i < len; ++i) {
		const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(len));
		const double cyc = std::fmod(hz * static_cast<double>(start + i) / sr, 1.0);
		acc += w * static_cast<double>(x[start + i]) * std::polar(1.0, -kTwoPi * cyc);
		wsum += w;
	}
	return acc * (2.0 / wsum);
}

/// THE FREQUENCY OF THE COMPONENT NEAR `hz`, from the advance of its phase between two windows.
double measuredHz(const std::vector<float>& x, size_t a, size_t b, size_t len, double hz, double sr) {
	const double dphi = std::arg(dftAt(x, b, len, hz, sr) / dftAt(x, a, len, hz, sr));
	return hz + dphi / (kTwoPi * static_cast<double>(b - a) / sr);
}

KsString made(double sr, double hz, double c, double t60, double disp = 0.0) {
	KsString s;
	s.prepare(sr);
	s.setDamping(c);
	s.setDecay(t60);
	s.setDispersion(disp);
	s.setFrequency(hz);
	return s;
}

/// A noise pluck one period long, through the comb at `pick`.
void pluckNoise(KsString& s, double pick = 0.13, uint64_t seed = 7) {
	std::vector<float> e(static_cast<size_t>(s.capacity()));
	std::vector<float> scratch(e.size());
	Excitation ex;
	ex.noise = 1.0f;
	ex.hardness = 1.0f;
	ex.pick = pick;
	ex.seed = seed;
	const int n = buildExcitation(ex, s.sampleRate() / s.frequencyHz(), e.data(), scratch.data(), s.capacity());
	s.pluck(e.data(), n);
}

std::vector<float> renderMono(KsString& s, size_t n, size_t block = 256) {
	std::vector<float> out(n);
	for (size_t i = 0; i < n; i += block) {
		s.render(out.data() + i, nullptr, static_cast<int>(std::min(block, n - i)));
	}
	return out;
}

} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

TEST_CASE("the Thiran coefficient's phase delay at the frequency it was solved for is exactly the fraction asked") {
	// Restated from the allpass's own phase, H = (η + e^−jω) / (1 + η·e^−jω), not from the solve's formula.
	for (const double d : { 0.25, 0.5, 0.9, 1.0, 1.49, 2.0 }) {
		for (const double w : { 1.0e-4, 0.01, 0.1, 0.5, 1.0, 1.5 }) {
			const double eta = thiranCoefficient(d, w);
			const std::complex<double> z = std::polar(1.0, -w);
			const std::complex<double> h = (eta + z) / (1.0 + eta * z);
			const double tau = -std::arg(h) / w;
			CAPTURE(d);
			CAPTURE(w);
			CHECK(std::abs(tau - d) < 1.0e-9);
		}
	}
}

TEST_CASE("a string's fundamental is the note to a tenth of a cent, at every pitch from C0 to 7 kHz and every rate, "
          "stretched or not") {
	// THE VALUE: the loop's phase delay at ω0, from the string's own tuning, against the period sr / f0.
	for (const double sr : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
		KsString s;
		s.prepare(sr);
		for (const double disp : { 0.0, -0.4, -0.9 }) {
			s.setDispersion(disp);
			for (double note = 12.0; note <= 126.0; note += 1.5) {
				const double hz = midiHz(note);
				if (hz > sr / 7.0) {
					break;
				}
				s.setFrequency(hz);
				const double w0 = kTwoPi * hz / sr;
				const double loop = loopPhaseDelay(s.tuning(), w0);
				CAPTURE(sr);
				CAPTURE(disp);
				CAPTURE(note);
				CHECK(std::abs(loop - sr / hz) < 1.0e-6);
				CHECK(std::abs(cents((sr / hz) / loop)) < 0.1);
				CHECK(s.tuning().d >= 0.5 - 1.0e-9);
				CHECK(s.tuning().d < 1.5 + 1.0e-9);
				CHECK(!s.clamped());
			}
		}
	}
}

TEST_CASE("an undamped string's rendered fundamental is the note to half a cent") {
	// THE SECOND ASSERTION: the phase of the rendered fundamental advancing between two windows half a second apart.
	struct Row {
		double sr;
		double hz;
		double disp;
	};
	for (const Row r : { Row { 48000.0, 55.0, 0.0 }, Row { 48000.0, 220.0, 0.0 }, Row { 48000.0, 880.0, 0.0 },
	                     Row { 48000.0, 3520.0, 0.0 }, Row { 48000.0, 4186.0, 0.0 }, Row { 96000.0, 440.0, 0.0 },
	                     Row { 44100.0, 2000.0, 0.0 }, Row { 48000.0, 220.0, -0.6 }, Row { 48000.0, 1500.0, -0.6 } }) {
		KsString s = made(r.sr, r.hz, 0.0, 0.0, r.disp);
		pluckNoise(s);
		const auto len = static_cast<size_t>(r.sr / 3.0);
		const std::vector<float> y = renderMono(s, static_cast<size_t>(r.sr * 1.2));
		const double f = measuredHz(y, static_cast<size_t>(r.sr * 0.1), static_cast<size_t>(r.sr * 0.6), len, r.hz, r.sr);
		CAPTURE(r.sr);
		CAPTURE(r.hz);
		CAPTURE(r.disp);
		CAPTURE(f);
		CHECK(std::abs(cents(f / r.hz)) < 0.5);
	}
}

TEST_CASE("the loop gain gives the fundamental exactly the T60 asked, and every partial decays at its own formula") {
	const double sr = 48000.0;
	const double hz = 220.0;
	const double c = 0.1;
	const double t60 = 1.5;
	KsString s = made(sr, hz, c, t60);
	// THE VALUE: (g·|H_F(ω0)|)^(f0·T60) = 10^−3, |H_F| restated from the FIR's taps.
	const auto firMag = [&](double w) { return std::abs(c * std::polar(1.0, 0.0) + (1.0 - 2.0 * c) * std::polar(1.0, -w)
		                                                 + c * std::polar(1.0, -2.0 * w)); };
	const double w0 = kTwoPi * hz / sr;
	CHECK(std::abs(std::pow(s.loopGain() * firMag(w0), hz * t60) - 1.0e-3) < 1.0e-9);
	// THE SECOND ASSERTION: the rendered partials' envelopes over 0.1..0.9 s.
	pluckNoise(s, 0.13);
	const std::vector<float> y = renderMono(s, static_cast<size_t>(sr * 1.2));
	const size_t len = 4096;
	const auto a = static_cast<size_t>(sr * 0.1);
	const auto b = static_cast<size_t>(sr * 0.9);
	for (const int k : { 1, 2, 3, 5, 8 }) {
		const double fk = partialHz(s.tuning(), sr, k);
		const double wk = kTwoPi * fk / sr;
		const double perTrip = s.loopGain() * firMag(wk);
		const double want = -3.0 * std::log(10.0) / (hz * std::log(perTrip));
		const double drop = std::abs(dftAt(y, b, len, fk, sr)) / std::abs(dftAt(y, a, len, fk, sr));
		const double got = -3.0 * std::log(10.0) * (static_cast<double>(b - a) / sr) / std::log(drop);
		CAPTURE(k);
		CAPTURE(want);
		CAPTURE(got);
		CHECK(std::abs(got / want - 1.0) < 0.05);
	}
}

TEST_CASE("a decaying string ends on EXACT zero, never a denormal, and then it is idle and stays silent") {
	KsString s = made(48000.0, 330.0, 0.15, 0.08);
	pluckNoise(s);
	bool loud = false;
	bool denormal = false;
	size_t samples = 0;
	std::vector<float> block(64);
	while (!s.idle() && samples < 48000 * 20) {
		s.render(block.data(), nullptr, 64);
		for (const float v : block) {
			loud = loud || std::abs(v) > 0.05f;
			denormal = denormal || (v != 0.0f && std::abs(v) < 1.0e-30f);
		}
		samples += 64;
	}
	CHECK(loud); // not vacuous: it sounded
	CHECK(!denormal);
	REQUIRE(s.idle());
	CHECK(samples < 48000 * 3); // a 80 ms T60 is silent long before 3 s
	s.render(block.data(), nullptr, 64);
	for (const float v : block) {
		CHECK(v == 0.0f);
	}
}

TEST_CASE("a string renders the same samples at any host block size") {
	const auto run = [](size_t block) {
		KsString s = made(48000.0, 196.0, 0.08, 2.0, -0.3);
		s.setPickups(0.1, 0.23);
		pluckNoise(s);
		std::vector<float> l(48000);
		std::vector<float> r(48000);
		for (size_t i = 0; i < l.size(); i += block) {
			const auto n = static_cast<int>(std::min(block, l.size() - i));
			s.render(l.data() + i, r.data() + i, n);
		}
		l.insert(l.end(), r.begin(), r.end());
		return l;
	};
	const std::vector<float> ref = run(4096);
	for (const size_t block : { size_t { 1 }, size_t { 7 }, size_t { 64 }, size_t { 441 } }) {
		CAPTURE(block);
		CHECK(std::memcmp(run(block).data(), ref.data(), ref.size() * sizeof(float)) == 0);
	}
}

TEST_CASE("a glide keeps the fundamental exact at every sample and hands its integer steps off without a click") {
	const double sr = 48000.0;
	// The click's yardstick: the steepest step of the same string ringing still at the glide's top.
	KsString still = made(sr, 440.0, 0.05, 5.0);
	pluckNoise(still);
	const std::vector<float> ref = renderMono(still, static_cast<size_t>(sr * 0.4));
	float refStep = 0.0f;
	for (size_t i = static_cast<size_t>(sr * 0.05); i < ref.size(); ++i) {
		refStep = std::max(refStep, std::abs(ref[i] - ref[i - 1]));
	}
	KsString s = made(sr, 220.0, 0.05, 5.0);
	pluckNoise(s);
	std::vector<float> y = renderMono(s, static_cast<size_t>(sr * 0.2));
	float glideStep = 0.0f;
	double worstTune = 0.0;
	int handoffs = 0;
	bool was = false;
	float prev = y.back();
	const auto n = static_cast<int>(sr * 0.5);
	for (int i = 0; i < n; ++i) {
		const double hz = 220.0 * std::pow(2.0, static_cast<double>(i) / n);
		s.setFrequency(hz);
		if (!s.handingOff()) {
			worstTune = std::max(worstTune, std::abs(loopPhaseDelay(s.tuning(), kTwoPi * hz / sr) - sr / hz));
		}
		float v = 0.0f;
		s.render(&v, nullptr, 1);
		glideStep = std::max(glideStep, std::abs(v - prev));
		prev = v;
		handoffs += s.handingOff() && !was ? 1 : 0;
		was = s.handingOff();
	}
	CAPTURE(refStep);
	CAPTURE(glideStep);
	CHECK(handoffs > 50); // not vacuous: an octave from 220 Hz crosses ~109 integer delays
	CHECK(worstTune < 1.0e-6);
	CHECK(glideStep < 1.5f * refStep);
}

TEST_CASE("a release moves the loop gain in a straight line to its new T60 and lands on it exactly") {
	KsString s = made(48000.0, 220.0, 0.05, 4.0);
	pluckNoise(s);
	std::vector<float> y(480);
	s.render(y.data(), nullptr, 480);
	const double from = s.loopGain();
	s.setDecay(0.2, 240);
	const double to = loopGainFor(48000.0, 220.0, 0.2, 0.05);
	s.render(y.data(), nullptr, 120);
	CHECK(std::abs(s.loopGain() - (from + 0.5 * (to - from))) < 1.0e-9);
	s.render(y.data(), nullptr, 120);
	CHECK(s.loopGain() == to);
	CHECK(to < from);
}

TEST_CASE("the pick notches exactly the harmonics whose multiple of the pick position is whole, and the pluck has "
          "the level asked") {
	std::vector<float> e(4096);
	std::vector<float> scratch(4096);
	Excitation ex;
	ex.noise = 1.0f;
	ex.pick = 0.25;
	ex.level = 0.5f;
	const int n = buildExcitation(ex, 400.0, e.data(), scratch.data(), 4096);
	REQUIRE(n == 400);
	double peak = 0.0;
	std::vector<double> mag(41);
	for (int k = 1; k <= 40; ++k) {
		std::complex<double> acc {};
		for (int i = 0; i < n; ++i) {
			acc += static_cast<double>(e[static_cast<size_t>(i)]) * std::polar(1.0, -kTwoPi * k * i / n);
		}
		mag[static_cast<size_t>(k)] = std::abs(acc);
		peak = std::max(peak, mag[static_cast<size_t>(k)]);
	}
	for (int k = 4; k <= 40; k += 4) {
		CAPTURE(k);
		CHECK(mag[static_cast<size_t>(k)] < 1.0e-4 * peak);
	}
	CHECK(mag[2] > 0.05 * peak); // not vacuous: the others are there
	double sq = 0.0;
	for (int i = 0; i < n; ++i) {
		sq += static_cast<double>(e[static_cast<size_t>(i)]) * e[static_cast<size_t>(i)];
	}
	CHECK(std::abs(std::sqrt(sq / n) - 0.5 * kExcitationRms) < 1.0e-5);
}

TEST_CASE("each ear's pickup nulls its own harmonics, and the mono sum keeps what the other ear has there") {
	// Pickup at 0.2 nulls harmonic 5 in the left ear only; at 0.13 the right ear keeps it. The mono sum at k = 5 is
	// then the right ear's alone (L + R = R where L = 0), and nowhere is it zero unless both are.
	const double sr = 48000.0;
	const double hz = 200.0;
	KsString s = made(sr, hz, 0.0, 0.0);
	s.setPickups(0.2, 0.13);
	pluckNoise(s, 0.37);
	std::vector<float> l(48000);
	std::vector<float> r(48000);
	s.render(l.data(), r.data(), 48000);
	std::vector<float> m(l.size());
	for (size_t i = 0; i < l.size(); ++i) {
		m[i] = l[i] + r[i];
	}
	const auto at = [&](const std::vector<float>& x, int k) {
		return std::abs(dftAt(x, 4800, 32768, partialHz(s.tuning(), sr, k), sr));
	};
	CHECK(at(l, 5) < 0.01 * at(r, 5));
	CHECK(std::abs(at(m, 5) / at(r, 5) - 1.0) < 0.05);
	CHECK(at(l, 3) > 0.1 * at(r, 3)); // not vacuous: the left ear has its other harmonics
}

TEST_CASE("a pluck's noise is a pinned sequence for its seed, and a different seed is a different pluck") {
	PluckNoise a(1);
	PluckNoise b(1);
	PluckNoise c(2);
	bool differs = false;
	for (int i = 0; i < 64; ++i) {
		const float x = a.next();
		CHECK(x == b.next());
		CHECK(x >= -1.0f);
		CHECK(x < 1.0f);
		differs = differs || x != c.next();
	}
	CHECK(differs);
	PluckNoise pin(12345);
	const float first = pin.next();
	const float second = pin.next();
	MESSAGE("pin: " << std::bit_cast<uint32_t>(first) << " " << std::bit_cast<uint32_t>(second));
	CHECK(std::bit_cast<uint32_t>(first) == 3201674148u);
	CHECK(std::bit_cast<uint32_t>(second) == 1064267604u);
}

TEST_CASE("a prepared string never allocates: retuning, plucking, rendering and going idle") {
	KsString s = made(48000.0, 110.0, 0.1, 0.3, -0.2);
	std::vector<float> e(static_cast<size_t>(s.capacity()));
	std::vector<float> scratch(e.size());
	std::vector<float> l(256);
	std::vector<float> r(256);
	const long before = gNews.load();
	Excitation ex;
	for (int i = 0; i < 400; ++i) {
		s.setFrequency(110.0 + i);
		s.setDispersion(-0.001 * i);
		s.setDecay(0.3, 64);
		if (i % 50 == 0) {
			const int n = buildExcitation(ex, 48000.0 / s.frequencyHz(), e.data(), scratch.data(), s.capacity());
			s.pluck(e.data(), n);
		}
		s.render(l.data(), r.data(), 256);
	}
	CHECK(gNews.load() == before);
}
