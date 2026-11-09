/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE SPECTRAL EFFECT DEVICE. Every claim is a VALUE: the latency in
// samples, where an impulse lands, the gain each bin is given, the level a sine comes out at.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/sidechain.h"
#include "dsp/partial_spectrum.h"
#include "fx/spectral/spectral.h"
#include "fx/spectral/spectral_stft.h"
#include "pffft/pffft.h"

#include <chrono>
#include <memory>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <numbers>
#include <string>
#if defined(__GLIBC__)
#include <malloc.h> // mallinfo2: glibc only, so the heap MEASURE case is Linux-only
#endif
#include <vector>

using namespace feathers::fx::spectral;

namespace {

constexpr Resolution kStops[] = { Resolution::Fast, Resolution::Balanced, Resolution::Fine };
constexpr double kRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

std::vector<float> noise(size_t n, uint32_t seed = 0x1234567u) {
	std::vector<float> v(n);
	for (auto& x : v) {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		x = static_cast<float>(seed) / 4294967296.0f * 2.0f - 1.0f;
	}
	return v;
}

/// Run `stft` over a stereo pair in irregular blocks (1..300), so nothing leans on a block lining up with a hop.
void run(Stft& stft, std::vector<float>& l, std::vector<float>& r) {
	size_t i = 0;
	int block = 1;
	while (i < l.size()) {
		const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - i));
		stft.process(l.data() + i, r.data() + i, n);
		i += static_cast<size_t>(n);
		block = block * 7 % 301 + 1;
	}
}

Stft& prepared(Stft& s, double sr, Resolution r) {
	s.prepare(sr);
	s.setResolution(r);
	return s;
}

} // namespace

TEST_CASE("spectral: latency is round(ms·sr/1000) at every stop and rate, and N is a pffft size no larger") {
	for (const double sr : kRates) {
		for (int i = 0; i < kResolutionCount; ++i) {
			const auto r = static_cast<Resolution>(i);
			const uint32_t l = latencyFor(r, sr);
			const int n = fftSizeFor(r, sr);
			CAPTURE(sr);
			CAPTURE(i);
			CHECK(l == static_cast<uint32_t>(std::lround(kResolutionMs[i] * sr / 1000.0)));
			CHECK(n <= static_cast<int>(l));
			CHECK(isRealFftSize(n));
			CHECK(pffft_is_valid_size(n, PFFFT_REAL) != 0);
			for (int m = n + 1; m <= static_cast<int>(l); ++m) {
				CHECK_FALSE(isRealFftSize(m)); // the LARGEST size that fits
			}
			Stft s;
			prepared(s, sr, r);
			CHECK(s.latency() == l);
			CHECK(s.fftSize() == n);
		}
	}
	// The table in the design, at 48 kHz.
	CHECK(fftSizeFor(Resolution::Fast, 48000.0) == 192);
	CHECK(fftSizeFor(Resolution::Balanced, 48000.0) == 480);
	CHECK(fftSizeFor(Resolution::Fine, 48000.0) == 1920);
}

TEST_CASE("spectral: an impulse comes out at exactly the reported latency") {
	for (const Resolution r : kStops) {
		for (const bool identity : { true, false }) {
			Stft s;
			prepared(s, 48000.0, r);
			s.setIdentity(identity);
			const auto l = static_cast<size_t>(s.latency());
			const size_t warm = 8 * l; // past the switch's dry hold and crossfade
			std::vector<float> a(warm + 4 * l, 0.0f);
			a[warm] = 1.0f;
			std::vector<float> b = a;
			run(s, a, b);
			CAPTURE(static_cast<int>(r));
			CAPTURE(identity);
			if (!identity) {
				CHECK(s.wetWeightForTest() == 1.0f);
			}
			const auto peak = static_cast<size_t>(std::max_element(a.begin(), a.end()) - a.begin());
			CHECK(peak == warm + l);
			CHECK(a[peak] == doctest::Approx(1.0).epsilon(1e-5));
			CHECK(b[peak] == a[peak]);
		}
	}
}

TEST_CASE("spectral: identity with every operator off is the input delayed by L, bit for bit") {
	for (const Resolution r : kStops) {
		Stft s;
		prepared(s, 44100.0, r);
		s.setIdentity(true);
		const std::vector<float> in = noise(20000);
		std::vector<float> l = in;
		std::vector<float> rr = noise(20000, 99u);
		const std::vector<float> inR = rr;
		run(s, l, rr);
		const auto lat = static_cast<size_t>(s.latency());
		size_t bad = 0;
		for (size_t t = 0; t < in.size(); ++t) {
			const float wantL = t >= lat ? in[t - lat] : 0.0f;
			const float wantR = t >= lat ? inR[t - lat] : 0.0f;
			bad += (l[t] != wantL) + (rr[t] != wantR);
		}
		CHECK(bad == 0);
		CHECK(s.framesForTest() == 0); // no frame was computed
	}
}

TEST_CASE("spectral: a unity-gain active path is the input delayed by L within 1e-5") {
	for (const Resolution r : kStops) {
		Stft s;
		prepared(s, 48000.0, r);
		s.setIdentity(false); // gains are 1 after setResolution
		const std::vector<float> in = noise(30000);
		std::vector<float> l = in;
		std::vector<float> rr = in;
		run(s, l, rr);
		const auto lat = static_cast<size_t>(s.latency());
		const size_t from = 8 * lat; // past the dry hold and the crossfade
		double worst = 0.0;
		for (size_t t = from; t < in.size(); ++t) {
			worst = std::max(worst, static_cast<double>(std::fabs(l[t] - in[t - lat])));
		}
		CAPTURE(static_cast<int>(r));
		MESSAGE("worst |active − delayed input| = " << worst);
		CHECK(worst < 1e-5);
		CHECK(s.framesForTest() > 0);
	}
}

TEST_CASE("spectral: a sine at a bin centre comes out at that bin's gain within 0.5 dB") {
	for (const Resolution r : kStops) {
		Stft s;
		prepared(s, 48000.0, r);
		const int bins = s.binCount();
		// A gain LINEAR IN THE BIN, 0.1 at DC to 1 at Nyquist: a sine's leakage into its two neighbours averages to its
		// own bin's gain, so the expected level is exactly g(k).
		for (int i = 0; i < bins; ++i) {
			s.gains()[i] = 0.1f + 0.9f * static_cast<float>(i) / static_cast<float>(bins - 1);
		}
		s.setIdentity(false);
		const int n = s.fftSize();
		for (const int k : { 3, n / 16, n / 5 }) {
			const double hz = k * 48000.0 / n;
			s.reset();
			std::vector<float> l(20 * static_cast<size_t>(n) + 20000);
			for (size_t t = 0; t < l.size(); ++t) {
				l[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t) / 48000.0));
			}
			std::vector<float> rr = l;
			run(s, l, rr);
			float peak = 0.0f;
			for (size_t t = l.size() / 2; t < l.size(); ++t) {
				peak = std::max(peak, std::fabs(l[t]));
			}
			const float want = s.gains()[k];
			CAPTURE(k);
			CHECK(std::fabs(20.0 * std::log10(peak / want)) < 0.5);
		}
	}
}

TEST_CASE("spectral: silence in gives exact zero out once the last frame holding sound has left (N + L)") {
	for (const Resolution r : kStops) {
		Stft s;
		prepared(s, 48000.0, r);
		for (int i = 0; i < s.binCount(); ++i) {
			s.gains()[i] = i % 3 == 0 ? 0.3f : 1.7f; // a curve that rings across the frame
		}
		s.setIdentity(false);
		const auto lat = static_cast<size_t>(s.latency());
		const auto n = static_cast<size_t>(s.fftSize());
		const size_t loud = 10 * lat;
		std::vector<float> l = noise(loud + 4 * lat + 2 * n);
		for (size_t t = loud; t < l.size(); ++t) {
			l[t] = 0.0f;
		}
		std::vector<float> rr = l;
		run(s, l, rr);
		// The last frame holding input sample loud − 1 ends N − 1 samples after it starts, and leaves L later.
		const size_t quietFrom = loud - 1 + n + lat;
		size_t nonzero = 0;
		for (size_t t = quietFrom; t < l.size(); ++t) {
			nonzero += (l[t] != 0.0f) + (rr[t] != 0.0f);
		}
		CHECK(nonzero == 0);
		CHECK(l[loud + lat - 1] != 0.0f); // the bound is not vacuous: the tail itself sounds
	}
}

//--------------------------------------------------------------------------------------------------//
// T2: THE SHAPER ROAD — the settings and the note made into a gain per bin.                          //
//--------------------------------------------------------------------------------------------------//

namespace {

/// One block through a prepared device, so its curve is computed.
void settle(SpectralDevice& d) {
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	d.processStereo(l.data(), r.data(), 64);
}

/// THE REFERENCE, RESTATED from the design (§3.3) rather than read from the device's own helpers: hz_i = i·sr/N floored at
/// kMinHz, k_i = hz_i / f0, base_i = 1/sqrt(max(hz_i, 20)), and the shaper's batch over all N/2 + 1 bins.
std::vector<float> restatedGains(const feathers::spectral::ShaperParams& p, double f0, double sr, int n) {
	const int bins = n / 2 + 1;
	std::vector<float> k(static_cast<size_t>(bins)), hz(static_cast<size_t>(bins)), base(static_cast<size_t>(bins)),
	    out(static_cast<size_t>(bins));
	for (int i = 0; i < bins; ++i) {
		const double h = std::max(i * sr / n, feathers::spectral::kMinHz);
		hz[static_cast<size_t>(i)] = static_cast<float>(h);
		k[static_cast<size_t>(i)] = static_cast<float>(h / f0);
		base[static_cast<size_t>(i)] = static_cast<float>(1.0 / std::sqrt(std::max(h, 20.0)));
	}
	feathers::spectral::gains(feathers::spectral::prepare(p, f0), k.data(), hz.data(), base.data(), bins, out.data());
	return out;
}

} // namespace

TEST_CASE("spectral: per-bin gains equal gains() restated at each bin's Hz, k and base") {
	Settings s;
	s.resolution = Resolution::Balanced;
	s.filterOn = true;
	s.filterMode = 0;
	s.filterCutoff = static_cast<float>(std::log2(1500.0));
	s.filterResonance = 0.4f;
	s.filterSlope = 0.5f;
	s.combOn = true;
	s.keyOffset = 7.0f; // G3: f0 with Key Track off
	s.combSpacing = 3.0f;
	s.combDepth = 0.6f;
	s.combPhase = 0.2f;
	s.vowelOn = true;
	s.vowel = 4.0f; // Bright: F1 550, F2 1900
	s.vowelDepth = 0.8f;
	s.tiltOn = true;
	s.tiltAmount = 4.5f;
	s.oddEvenOn = true;
	s.oddEvenBalance = 0.5f;

	// The params, restated by hand from the controls.
	feathers::spectral::ShaperParams p;
	p.filter = { .on = true, .mode = feathers::spectral::FilterMode::LowPass, .cutoffHz = static_cast<float>(std::exp2(static_cast<double>(s.filterCutoff))),
	             .resonance = 0.4f, .slope = 0.5f, .keyTrack = 1.0f }; // every operator follows f0: the cutoff tracks it
	p.comb = { .on = true, .spacing = 3.0f, .depth = 0.6f, .phase = 0.2f };
	p.oddEven = { .on = true, .balance = 0.5f };
	// THE VOWEL MOVES WITH f0: Bright's anchors (550, 1900 Hz) scaled by f0 / C3 — set per subcase.
	const auto withVowelAt = [](feathers::spectral::ShaperParams q, double f0) {
		const double scale = f0 / 261.6255653005986;
		q.vowel = { .on = true, .f1Hz = static_cast<float>(550.0 * scale), .f2Hz = static_cast<float>(1900.0 * scale), .depth = 0.8f };
		return q;
	};
	p.tilt = { .on = true, .dbPerOctave = 4.5f, .pivotHz = 1000.0f };

	for (const double sr : { 44100.0, 48000.0 }) {
		SUBCASE("Key Track off: in harmonics of C3 raised by Offset") {
			SpectralDevice d;
			d.prepare(sr);
			d.setTarget(s);
			settle(d);
			REQUIRE_FALSE(d.perVoiceForTest());
			const int n = fftSizeFor(s.resolution, sr);
			REQUIRE(d.binCountForTest() == n / 2 + 1);
			const double f0 = 261.6255653005986 * std::exp2(7.0 / 12.0);
			const std::vector<float> want = restatedGains(withVowelAt(p, f0), f0, sr, n);
			size_t bad = 0;
			for (int i = 0; i < n / 2 + 1; ++i) {
				bad += d.gainsForTest()[i] != want[static_cast<size_t>(i)];
			}
			CHECK(bad == 0);
		}
		SUBCASE("Key Track on, Poly: in harmonics of the voice's note") {
			SpectralDevice d;
			d.prepare(sr);
			Settings sv = s;
			sv.keyTrack = true;
			sv.poly = true;
			d.setTarget(sv);
			feathers::device::NoteContext note;
			note.pitchSemitones = 57.0f; // A3, 220 Hz
			d.setNoteContext(note);
			settle(d);
			REQUIRE(d.perVoiceForTest());
			feathers::spectral::ShaperParams q = p;
			const int n = fftSizeFor(s.resolution, sr);
			const double f0 = 220.0 * std::exp2(7.0 / 12.0); // the voice's note, raised by Offset
			const std::vector<float> want = restatedGains(withVowelAt(q, f0), f0, sr, n);
			size_t bad = 0;
			for (int i = 0; i < n / 2 + 1; ++i) {
				bad += d.gainsForTest()[i] != want[static_cast<size_t>(i)];
			}
			CHECK(bad == 0);
		}
	}
}

TEST_CASE("spectral: Key Track off, the comb notches every Spacing harmonics of C3 raised by Offset") {
	// 48 kHz at Balanced is N = 480: bins every 100 Hz. An Offset that puts f0 at 300 Hz, Spacing 1: notches on bins 3, 6, 9...
	Settings s;
	s.combOn = true;
	s.combSpacing = 1.0f;
	s.keyOffset = static_cast<float>(12.0 * std::log2(300.0 / 261.6255653005986));
	s.combDepth = 1.0f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	settle(d);
	REQUIRE(d.binCountForTest() == 241);
	for (int j = 1; j <= 60; ++j) {
		CAPTURE(j);
		CHECK(d.gainsForTest()[3 * j] == 0.0f);   // a full-depth notch on every multiple of 300 Hz
		CHECK(d.gainsForTest()[3 * j + 1] > 0.5f); // and open between them
	}
}

TEST_CASE("spectral: a hand's curve, applied on the message thread and pulled, reaches the device's gains at its harmonics") {
	// 48 kHz at Balanced: bins every 100 Hz. f0 = 300 Hz, so harmonic 3 is bin 9 (900 Hz) and band 2 of the curve.
	Settings s;
	s.curveOn = true;
	s.keyOffset = static_cast<float>(12.0 * std::log2(300.0 / 261.6255653005986));
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	settle(d);
	REQUIRE(d.binCountForTest() == 241);
	CHECK(d.gainsForTest()[9] == 1.0f); // a flat curve: nothing drawn
	std::array<float, 64> drawn {};
	drawn[2] = kCurveFloorDb; // harmonic 3 silenced, every other band at 0 dB
	REQUIRE(d.applyCurve(drawn.data(), drawn.size()));
	settle(d);
	CHECK(d.gainsForTest()[9] == 1.0f); // a write is not heard until the audio thread pulls it
	d.pullCurve(d);
	settle(d);
	CHECK(d.gainsForTest()[9] == 0.0f);
	CHECK(d.gainsForTest()[12] == 1.0f); // harmonic 4: untouched
	// A ROW REWRITE (the next block's setFields) KEEPS THE DRAWN CURVE: it is not the row's to take back.
	d.setTarget(s);
	settle(d);
	CHECK(d.gainsForTest()[9] == 0.0f);
}

TEST_CASE("spectral: a note context moves the harmonic operators to the new f0 (Odd/Even at the new harmonics)") {
	// 48 kHz at Fine is N = 1920: bins every 25 Hz. A 200 Hz note's odd harmonics are bins 8, 24, 40...; a 300 Hz note's
	// are 12, 36, 60...
	Settings s;
	s.resolution = Resolution::Fine;
	s.oddEvenOn = true;
	s.oddEvenBalance = 1.0f; // even only
	s.keyTrack = true;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	feathers::device::NoteContext note;
	note.pitchSemitones = static_cast<float>(69.0 + 12.0 * std::log2(200.0 / 440.0));
	d.setNoteContext(note);
	settle(d);
	CHECK(d.noteHzForTest() == doctest::Approx(200.0).epsilon(1e-5));
	CHECK(d.gainsForTest()[8] == 0.0f);
	CHECK(d.gainsForTest()[24] == 0.0f);
	CHECK(d.gainsForTest()[16] == 1.0f);
	note.pitchSemitones = static_cast<float>(69.0 + 12.0 * std::log2(300.0 / 440.0));
	d.setNoteContext(note);
	settle(d);
	CHECK(d.gainsForTest()[12] == 0.0f);
	CHECK(d.gainsForTest()[36] == 0.0f);
	CHECK(d.gainsForTest()[24] == 1.0f); // 600 Hz: the new note's second harmonic, open
	CHECK(d.gainsForTest()[8] == 0.0f);  // 200 Hz rounds to the new note's first (odd) harmonic
}

TEST_CASE("spectral: with Key Track off the harmonic operators run at C3 raised by Offset, whatever note the host hands it") {
	// A BUS HOST HANDS ITS DEVICES THE LAST NOTE (the Eroder tracks it); with Key Track off it must move nothing.
	Settings s;
	s.oddEvenOn = true;
	s.oddEvenBalance = 1.0f;
	s.keyOffset = -5.0f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	feathers::device::NoteContext note;
	note.pitchSemitones = 57.0f;
	d.setNoteContext(note);
	settle(d);
	REQUIRE_FALSE(d.perVoiceForTest());
	feathers::spectral::ShaperParams p;
	p.oddEven = { .on = true, .balance = 1.0f };
	const int n = fftSizeFor(s.resolution, 48000.0);
	const std::vector<float> want = restatedGains(p, 261.6255653005986 * std::exp2(-5.0 / 12.0), 48000.0, n);
	size_t bad = 0;
	for (int i = 0; i < n / 2 + 1; ++i) {
		bad += d.gainsForTest()[i] != want[static_cast<size_t>(i)];
	}
	CHECK(bad == 0);
	// A NEW NOTE CHANGES NOTHING.
	note.pitchSemitones = 70.0f;
	d.setNoteContext(note);
	settle(d);
	bad = 0;
	for (int i = 0; i < n / 2 + 1; ++i) {
		bad += d.gainsForTest()[i] != want[static_cast<size_t>(i)];
	}
	CHECK(bad == 0);
}

//--------------------------------------------------------------------------------------------------//
// T3: THE DEVICE — its row, and the latency it reports.                                              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("spectral: the reported latency moves only with Resolution") {
	for (const double sr : kRates) {
		for (int stop = 0; stop < kResolutionCount; ++stop) {
			Row row = rowOf(Settings {});
			row[static_cast<size_t>(Param::Resolution)] = static_cast<float>(stop);
			const uint32_t want = latencyFor(static_cast<Resolution>(stop), sr);
			CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), sr) == want);
			// EVERY OTHER CONTROL, at its min and its max, Enabled included (L3): the report does not move.
			for (size_t p = 0; p < kParamCount; ++p) {
				if (p == static_cast<size_t>(Param::Resolution)) {
					continue;
				}
				for (const double v : { kParamDescs[p].min, kParamDescs[p].max }) {
					Row moved = row;
					moved[p] = static_cast<float>(v);
					CAPTURE(p);
					CHECK(kAudioOps.latencySamples(nullptr, moved.data(), moved.size(), sr) == want);
				}
			}
		}
	}
	// And Resolution is a stored control, never a modulated one (L1).
	CHECK_FALSE(kParamDescs[static_cast<size_t>(Param::Resolution)].modulatable);
}

TEST_CASE("spectral: the row round-trips and absent fields read as their defaults") {
	Settings s;
	s.enabled = false;
	s.resolution = Resolution::Fine;
	s.mix = 0.25f;
	s.filterOn = true;
	s.filterMode = 3;
	s.vowel = 6.5f;
	const Row row = rowOf(s);
	CHECK(stateOf(row, row.size()) == s);
	CHECK(stateOf(row, 0) == Settings {});
	for (size_t p = 0; p < kParamCount; ++p) {
		CAPTURE(p);
		CHECK(rowOf(Settings {})[p] == static_cast<float>(kParamDescs[p].def));
	}
}

TEST_CASE("spectral: Mix blends against the dry delayed by L") {
	// A filter that removes everything above 20 Hz: the wet of a 1037 Hz sine is ~0, so the output at Mix m is (1 − m) of
	// the dry, delayed by L — not of the undelayed input. NOT 1 kHz: L = 480 samples is exactly ten of its periods at 48 kHz,
	// so the undelayed sine equals the delayed one and the case could not tell them apart (a known-bad found it).
	Settings s;
	s.filterOn = true;
	s.filterMode = 0;
	s.filterCutoff = static_cast<float>(kLog2Of20);
	s.filterSlope = 1.0f;
	s.mix = 0.5f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	std::vector<float> l(48000);
	for (size_t t = 0; t < l.size(); ++t) {
		l[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1037.0 * static_cast<double>(t) / 48000.0));
	}
	const std::vector<float> in = l;
	std::vector<float> r = l;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	const auto lat = static_cast<size_t>(d.stftForTest().latency());
	double worst = 0.0;
	for (size_t t = 24000; t < l.size(); ++t) {
		worst = std::max(worst, static_cast<double>(std::fabs(l[t] - 0.5f * in[t - lat])));
	}
	CHECK(worst < 1e-3);
}

//--------------------------------------------------------------------------------------------------//
// T6: WHAT EACH STOP RESOLVES — measured, and the tooltip says what was measured.                    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// The amplitude of `hz` in `x[from, to)` by a single DFT bin (Goertzel), at `sr`.
double toneAmplitude(const std::vector<float>& x, size_t from, size_t to, double hz, double sr) {
	double re = 0.0;
	double im = 0.0;
	for (size_t t = from; t < to; ++t) {
		const double ph = 2.0 * std::numbers::pi * hz * static_cast<double>(t) / sr;
		re += x[t] * std::cos(ph);
		im += x[t] * std::sin(ph);
	}
	return 2.0 * std::hypot(re, im) / static_cast<double>(to - from);
}

struct Resolved {
	double cutDb;   ///< how far the fundamental fell (positive: quieter)
	double keepDb;  ///< how far the second harmonic moved (|dB|)
};

/// A SAWTOOTH AT `midi` THROUGH ODD/EVEN AT +1 (even harmonics only), per voice, at `stop`: does the device take its odd
/// fundamental out and leave its even second harmonic? That is what "resolves the note's harmonics" means for an operator.
Resolved resolve(Resolution stop, int midi, double sr) {
	Settings s;
	s.resolution = stop;
	s.oddEvenOn = true;
	s.oddEvenBalance = 1.0f;
	s.keyTrack = true;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(s);
	feathers::device::NoteContext note;
	note.pitchSemitones = static_cast<float>(midi);
	d.setNoteContext(note);
	const double f0 = 440.0 * std::exp2((midi - 69) / 12.0);
	const size_t len = static_cast<size_t>(sr); // one second
	std::vector<float> l(len);
	double ph = 0.0;
	for (auto& x : l) {
		x = static_cast<float>(0.5 * (2.0 * ph - 1.0)); // naive saw: aliasing far below the harmonics read
		ph += f0 / sr;
		ph -= std::floor(ph);
	}
	const std::vector<float> in = l;
	std::vector<float> r = l;
	d.processStereo(l.data(), r.data(), static_cast<int>(len));
	const auto lat = static_cast<size_t>(d.stftForTest().latency());
	const size_t from = len / 2;
	const auto at = [&](const std::vector<float>& v, size_t shift, double hz) {
		std::vector<float> w(v.begin() + static_cast<std::ptrdiff_t>(from - shift), v.end() - static_cast<std::ptrdiff_t>(shift));
		return toneAmplitude(w, 0, w.size(), hz, sr);
	};
	const double in1 = at(in, lat, f0);
	const double in2 = at(in, lat, 2.0 * f0);
	const double out1 = at(l, 0, f0);
	const double out2 = at(l, 0, 2.0 * f0);
	return { 20.0 * std::log10(in1 / std::max(out1, 1e-12)), std::fabs(20.0 * std::log10(out2 / in2)) };
}

} // namespace

TEST_CASE("spectral: the resolution tooltip names the measured lowest notes") {
	// RESOLVED: Odd/Even at +1 cuts a sawtooth's fundamental by 20 dB and keeps its second harmonic within 1 dB, for the
	// note and every note up to an octave above it. The higher of 44.1 and 48 kHz is what the tooltip promises.
	constexpr const char* kNames[] = { "G#4", "F#3", "E1" };
	const std::string tip = kParamDescs[static_cast<size_t>(Param::Resolution)].tooltip;
	for (const Resolution stop : kStops) {
		int worst = -1;
		for (const double sr : { 44100.0, 48000.0 }) {
			int lowest = -1;
			for (int midi = 12; midi <= 96; ++midi) {
				bool all = true;
				for (int m = midi; m <= std::min(midi + 12, 108); ++m) {
					const Resolved r = resolve(stop, m, sr);
					all = all && r.cutDb >= 20.0 && r.keepDb <= 1.0;
				}
				if (all) {
					lowest = midi;
					break;
				}
			}
			MESSAGE("sr " << sr << " stop " << static_cast<int>(stop) << ": lowest resolved MIDI " << lowest);
			worst = std::max(worst, lowest);
		}
		const auto i = static_cast<size_t>(stop);
		CHECK(worst == kResolvedMidi[i]);
		CHECK(tip.find(kNames[i]) != std::string::npos);
	}
}

//--------------------------------------------------------------------------------------------------//
// T5: THE DRAWN CURVE — row state after the controls; log Hz on the mix.                             //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("spectral: the row is the controls alone — the curve left it for the hand's block, and a row decodes to a flat curve") {
	Settings s;
	s.curveOn = true;
	const Row row = rowOf(s);
	CHECK(row.size() == kParamCount);
	CHECK(stateOf(row, row.size()).curveDb == Settings {}.curveDb); // the row cannot carry a curve: whoever has the block fills it in
	CHECK(stateOf(row, row.size()) == s);
	CHECK(curveGainOf(0.0f) == 1.0f);
	CHECK(curveGainOf(kCurveFloorDb) == 0.0f);
	CHECK(kTraits.extraState == 0);
	// THE OLD KEY IS RETIRED, dated, with its reason; the block is declared a hand's and is the curve's length.
	bool retired = false;
	for (const feathers::device::RetiredKey& k : kTraits.retiredKeys) {
		retired = retired || std::string(k.key) == "spectral_curve";
	}
	CHECK(retired);
	REQUIRE(kStateOps.blocks.size() == 2);
	CHECK(std::string(kStateOps.blocks[1].key) == "spectral_curve_db");
	CHECK(kStateOps.blocks[1].handEdited);
	CHECK(kStateOps.blocks[1].floats == static_cast<uint32_t>(feathers::spectral::kCurveBands));
	CHECK(kParamCount + 0 < feathers::device::kMaxDeviceFields);
	MESSAGE("spectral row: " << kParamCount << " of " << feathers::device::kMaxDeviceFields << " fields");
}

TEST_CASE("spectral: a hand's curve applied on the message thread reaches the bus instance and every voice by pulling, value by value") {
	using feathers::device::AudioOps;
	std::vector<std::byte> bus(sizeof(SpectralDevice) + 64), voice(sizeof(SpectralDevice) + 64);
	void* busP = bus.data();
	void* voiceP = voice.data();
	busP = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(busP) + alignof(SpectralDevice) - 1) & ~(alignof(SpectralDevice) - 1));
	voiceP = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(voiceP) + alignof(SpectralDevice) - 1) & ~(alignof(SpectralDevice) - 1));
	const AudioOps& ops = kAudioOps;
	ops.construct(busP);
	ops.construct(voiceP);
	auto& b = *static_cast<SpectralDevice*>(busP);
	auto& v = *static_cast<SpectralDevice*>(voiceP);
	// NOTHING DRAWN: flat.
	ops.pullHandBlocks(voiceP, busP);
	for (const float db : v.curveForTest()) {
		CHECK(db == 0.0f);
	}
	std::array<float, 64> drawn {};
	for (size_t i = 0; i < drawn.size(); ++i) {
		drawn[i] = -36.0f + static_cast<float>(i) * 0.75f; // spans the floor to above the ceiling: the ceiling is held
	}
	REQUIRE(kStateOps.apply(busP, 1, drawn.data(), drawn.size()));
	CHECK_FALSE(kStateOps.apply(busP, 0, drawn.data(), drawn.size()));      // the freeze is not a hand's block
	CHECK_FALSE(kStateOps.apply(busP, 1, drawn.data(), drawn.size() - 1)); // nor the wrong length
	// BEFORE A PULL the audio side still holds the old curve (a write is not heard until the audio thread takes it):
	CHECK(b.curveForTest()[10] == 0.0f);
	ops.pullHandBlocks(busP, busP);
	ops.pullHandBlocks(voiceP, busP);
	for (size_t i = 0; i < drawn.size(); ++i) {
		CAPTURE(i);
		const float want = std::clamp(drawn[i], kCurveFloorDb, kCurveMaxDb);
		CHECK(b.curveForTest()[i] == want);
		CHECK(v.curveForTest()[i] == want);
	}
	// AND THE RESULT: the device's own target now carries it into the gains (setFields next block keeps it).
	Row row = rowOf(Settings {});
	ops.setFields(busP, row.data(), row.size());
	CHECK(b.curveForTest()[10] == std::clamp(drawn[10], kCurveFloorDb, kCurveMaxDb));
	// NaN AND INFINITY ARE REPAIRED (to flat), never stored.
	std::array<float, 64> bad = drawn;
	bad[2] = std::numeric_limits<float>::quiet_NaN();
	bad[3] = std::numeric_limits<float>::infinity();
	REQUIRE(kStateOps.apply(busP, 1, bad.data(), bad.size()));
	ops.pullHandBlocks(busP, busP);
	CHECK(b.curveForTest()[2] == 0.0f);
	CHECK(b.curveForTest()[3] == 0.0f);
	ops.destruct(voiceP);
	ops.destruct(busP);
}

TEST_CASE("spectral: a drawn curve is saved by the block road and restored into a FRESH instance, value by value") {
	using feathers::device::AudioOps;
	const auto build = [](std::vector<std::byte>& store) {
		void* p = store.data();
		p = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(p) + alignof(SpectralDevice) - 1) & ~(alignof(SpectralDevice) - 1));
		kAudioOps.construct(p);
		return p;
	};
	std::vector<std::byte> a(sizeof(SpectralDevice) + 64), b(sizeof(SpectralDevice) + 64);
	void* first = build(a);
	std::array<float, 64> drawn {};
	for (size_t i = 0; i < drawn.size(); ++i) {
		drawn[i] = (i % 7 == 0) ? kCurveFloorDb : -20.0f + static_cast<float>(i) * 0.4f;
	}
	REQUIRE(kStateOps.apply(first, 1, drawn.data(), drawn.size()));
	// SAVE: what `read` publishes under its serial is what the host stores.
	std::array<float, 64> saved {};
	CHECK(kStateOps.read(first, 1, saved.data(), saved.size()) != 0);
	// LOAD INTO A FRESH INSTANCE, the converger's road (restore on an instance nothing runs yet).
	void* second = build(b);
	REQUIRE(kStateOps.restore(second, 1, saved.data(), saved.size()));
	auto& fresh = *static_cast<SpectralDevice*>(second);
	for (size_t i = 0; i < drawn.size(); ++i) {
		CAPTURE(i);
		CHECK(fresh.curveForTest()[i] == std::clamp(drawn[i], kCurveFloorDb, kCurveMaxDb));
	}
	// AND IT READS BACK THE SAME (so a second save writes the same block).
	std::array<float, 64> again {};
	CHECK(kStateOps.read(second, 1, again.data(), again.size()) != 0);
	CHECK(again == saved);
	CHECK_FALSE(kStateOps.restore(second, 1, saved.data(), saved.size() - 1)); // a block of another length is refused
	kAudioOps.destruct(second);
	kAudioOps.destruct(first);
}

TEST_CASE("spectral: the input spectrum reads a full-scale sine at 0 dBFS in its band, and floors elsewhere") {
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(Settings {}); // the identity: the analysis runs on its own
	std::vector<float> got;
	feathers::device::ReadingsSink sink { &got, [](void* c, const float* v, size_t n) {
		                                     static_cast<std::vector<float>*>(c)->assign(v, v + n);
	                                     } };
	d.setReadingsSink(sink);
	const int n = d.stftForTest().fftSize();
	const double hz = 25.0 * 48000.0 / n; // bin 25 of 480: 2.5 kHz, a bin centre
	std::vector<float> l(8 * static_cast<size_t>(n));
	for (size_t t = 0; t < l.size(); ++t) {
		l[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t) / 48000.0));
	}
	std::vector<float> r = l;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	REQUIRE(got.size() == kReadingCount);
	const auto band = static_cast<size_t>(std::log2(hz / kBandLowHz) / std::log2(kBandHighHz / kBandLowHz) * kSpectrumBands);
	CHECK(std::fabs(got[band]) < 0.1f);   // 0 dBFS, within 0.1 dB
	CHECK(got[2] < -40.0f);                // ~25 Hz, far from it
	CHECK(d.stftForTest().framesForTest() == 0); // analysed without shaping anything
}

//--------------------------------------------------------------------------------------------------//
// THE REVIEW'S ADDS: the switch-on gate, Mix's ramp, the analysis only while a card listens.        //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("spectral: switched on, the output stays the exact delayed dry until the fourth frame, then crossfades over one hop") {
	Stft s;
	prepared(s, 48000.0, Resolution::Balanced);
	for (int i = 0; i < s.binCount(); ++i) {
		s.gains()[i] = 0.5f; // a wet that is NOT the dry, so a premature wet shows
	}
	s.setIdentity(true);
	const std::vector<float> in = noise(12000);
	const auto lat = static_cast<size_t>(s.latency());
	const auto hop = static_cast<size_t>(s.hop());
	std::vector<float> l = in;
	std::vector<float> r = in;
	const size_t on = 3001; // not on a hop boundary
	s.process(l.data(), r.data(), static_cast<int>(on));
	s.setIdentity(false);
	// ONE SAMPLE AT A TIME, so the frame count and the weight are read at every sample.
	size_t fourthAt = 0;
	size_t fullAt = 0;
	size_t dryBad = 0;
	for (size_t t = on; t < in.size(); ++t) {
		const uint64_t framesBefore = s.framesForTest();
		const float wBefore = s.wetWeightForTest();
		s.process(l.data() + t, r.data() + t, 1);
		if (fourthAt == 0) {
			// UNTIL THE FOURTH FRAME HAS OVERLAPPED: no weight at all, and the delayed dry to the bit.
			CHECK(wBefore == 0.0f);
			dryBad += l[t] != in[t - lat];
			if (framesBefore < 4 && s.framesForTest() == 4) {
				fourthAt = t;
			}
		}
		else if (fullAt == 0 && s.wetWeightForTest() == 1.0f) {
			fullAt = t;
		}
	}
	CHECK(dryBad == 0);
	REQUIRE(fourthAt > 0);
	CHECK(l[fourthAt + 1] == in[fourthAt + 1 - lat]); // the sample after the fourth frame still reads weight 0
	CHECK(fullAt - fourthAt == hop);                  // and the weight reaches 1 one hop later
}

TEST_CASE("spectral: Mix ramps over a fixed 32 samples, whatever the host block") {
	// THE SAME SAMPLES IN TWO PARTITIONS — one block of 256, and blocks of 8 — must come out the same: a ramp keyed to the host
	// block made the output depend on it (test_fx_devices' partition sweep caught that).
	const auto render = [](int block) {
		Settings st;
		st.filterOn = true;
		st.filterCutoff = static_cast<float>(kLog2Of20);
		st.filterSlope = 1.0f; // the wet of a 1037 Hz sine is ~0: the output is (1 − mix) of the delayed dry
		st.mix = 1.0f;
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(st);
		std::vector<float> l(20256);
		for (size_t t = 0; t < l.size(); ++t) {
			l[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1037.0 * static_cast<double>(t) / 48000.0));
		}
		std::vector<float> r = l;
		d.processStereo(l.data(), r.data(), 20000);
		st.mix = 0.0f;
		d.setTarget(st);
		for (int at = 20000; at < 20256; at += block) {
			d.processStereo(l.data() + at, r.data() + at, block);
		}
		return l;
	};
	const std::vector<float> one = render(256);
	const std::vector<float> eights = render(8);
	CHECK(one == eights);
	// AND THE RAMP ITSELF: the dry's gain at sample j after the change is (j + 1) / 32, then 1 from sample 31 on.
	std::vector<float> in(20256);
	for (size_t t = 0; t < in.size(); ++t) {
		in[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1037.0 * static_cast<double>(t) / 48000.0));
	}
	const size_t lat = latencyFor(Resolution::Balanced, 48000.0);
	double worst = 0.0;
	for (size_t j = 0; j < 256; ++j) {
		const double dryGain = std::min(1.0, static_cast<double>(j + 1) / 32.0); // 32, restated: not the constant under test
		worst = std::max(worst, std::fabs(one[20000 + j] - dryGain * in[20000 + j - lat]));
	}
	CHECK(worst < 2e-3);
	CHECK(one[20031] == in[20031 - lat]); // the ramp's last sample: Mix 0, the dry exactly
}

TEST_CASE("spectral: the input is analysed only while a card listens") {
	Settings st;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	std::vector<float> l = noise(4800);
	std::vector<float> r = l;
	d.processStereo(l.data(), r.data(), 4800); // the identity, no sink
	CHECK(d.stftForTest().analysedForTest() == 0);
	st.filterOn = true;
	d.setTarget(st);
	d.processStereo(l.data(), r.data(), 4800); // shaping, no sink
	CHECK(d.stftForTest().framesForTest() > 0);
	CHECK(d.stftForTest().analysedForTest() == 0);
	std::vector<float> got;
	d.setReadingsSink({ &got, [](void* c, const float* v, size_t n) { static_cast<std::vector<float>*>(c)->assign(v, v + n); } });
	d.processStereo(l.data(), r.data(), 4800);
	CHECK(d.stftForTest().analysedForTest() > 0);
	d.setReadingsSink({});
	const uint64_t before = d.stftForTest().analysedForTest();
	d.processStereo(l.data(), r.data(), 4800);
	CHECK(d.stftForTest().analysedForTest() == before);
}

//--------------------------------------------------------------------------------------------------//
// PHASE 2: KEY TRACKING ON THE BUS.                                                                  //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("spectral: Key Track follows the host's note plus Key Offset, in harmonics") {
	Settings s;
	s.keyTrack = true;
	s.keyOffset = 12.0f;
	s.combOn = true;
	s.combSpacing = 3.0f;
	s.combDepth = 0.7f;
	s.oddEvenOn = true;
	s.oddEvenBalance = 0.5f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(s);
	feathers::device::NoteContext note;
	note.pitchSemitones = 57.0f; // A3, 220 Hz; an octave up is 440 Hz
	d.setNoteContext(note);
	settle(d);
	REQUIRE_FALSE(d.perVoiceForTest()); // the bus: the units come from Key Track, not the placement
	feathers::spectral::ShaperParams p;
	p.comb = { .on = true, .spacing = 3.0f, .depth = 0.7f, .phase = 0.0f };
	p.oddEven = { .on = true, .balance = 0.5f };
	const int n = fftSizeFor(s.resolution, 48000.0);
	const std::vector<float> want = restatedGains(p, 440.0, 48000.0, n);
	size_t bad = 0;
	for (int i = 0; i < n / 2 + 1; ++i) {
		bad += d.gainsForTest()[i] != want[static_cast<size_t>(i)];
	}
	CHECK(bad == 0);
	// A NEW NOTE MOVES THEM: a fifth up.
	note.pitchSemitones = 64.0f;
	d.setNoteContext(note);
	settle(d);
	const std::vector<float> moved = restatedGains(p, 440.0 * std::exp2(7.0 / 12.0), 48000.0, n);
	bad = 0;
	for (int i = 0; i < n / 2 + 1; ++i) {
		bad += d.gainsForTest()[i] != moved[static_cast<size_t>(i)];
	}
	CHECK(bad == 0);
}

TEST_CASE("spectral: Key Offset reads as the pitch it sets with Key Track off, in semitones with it on; nothing is gated by Key Track") {
	const auto print = [](double value, float keyTrack) {
		static float kt = 0.0f;
		kt = keyTrack;
		feathers::device::TextView view;
		view.field = [](const void*, size_t control) { return control == static_cast<size_t>(Param::KeyTrack) ? kt : 0.0f; };
		char out[32] = {};
		REQUIRE(kTextOps.format(static_cast<size_t>(Param::KeyOffset), value, view, out, sizeof(out)));
		return std::string(out);
	};
	CHECK(print(0.0, 0.0f) == "C3 +0");
	CHECK(print(7.0, 0.0f) == "G3 +7");
	CHECK(print(-36.0, 0.0f) == "C0 -36"); // the range: C0 ...
	CHECK(print(36.0, 0.0f) == "C6 +36");  // ... to C6
	CHECK(print(7.0, 1.0f) == "+7 st");
	CHECK(kParamDescs[static_cast<size_t>(Param::KeyOffset)].min == -36.0);
	CHECK(kParamDescs[static_cast<size_t>(Param::KeyOffset)].max == 36.0);
	// NO CONTROL IS GATED BY KEY TRACK: the harmonic ones are always available (only Resonance has a gate, on the mode).
	for (size_t i = 0; i < kParamCount; ++i) {
		CAPTURE(std::string(kParamDescs[i].name));
		CHECK(kParamDescs[i].gateControl != static_cast<int16_t>(Param::KeyTrack));
	}
}

#if defined(__GLIBC__)
TEST_CASE("spectral: MEASURE one instance's heap at 48 and 192 kHz (the per-voice road's memory)") {
	for (const double sr : { 48000.0, 192000.0 }) {
		const size_t before = mallinfo2().uordblks;
		auto* d = new SpectralDevice();
		d->prepare(sr);
		const size_t after = mallinfo2().uordblks;
		MESSAGE("sr " << sr << ": one Spectral holds " << (after - before) << " bytes of heap");
		CHECK(after > before);
		delete d;
	}
}
#endif

TEST_CASE("spectral: every operator follows f0 — Offset moves the cutoff, the vowel and the comb, and leaves Tilt alone") {
	// FROM THE AUDITION (2026-10-13): Key Track did not seem to follow the filter, and Offset moves every keytracked control. An octave
	// of Offset (Key Track off: f0 = C4) must double the tracked cutoff and both formants, and not move Tilt's pivot.
	Settings s;
	s.filterOn = true;
	s.filterCutoff = static_cast<float>(std::log2(1000.0));
	s.vowelOn = true;
	s.vowel = 3.0f; // Open: 750, 1150 Hz at C3
	s.vowelDepth = 1.0f;
	s.tiltOn = true;
	s.tiltAmount = 6.0f;
	for (const float offset : { 0.0f, 12.0f, -7.0f }) {
		CAPTURE(offset);
		s.keyOffset = offset;
		const double f0 = shaperF0(s, false, 0.0);
		const double ratio = std::exp2(offset / 12.0);
		CHECK(f0 == doctest::Approx(261.6255653005986 * ratio));
		const feathers::spectral::PreparedShaper ps = feathers::spectral::prepare(shaperParamsOf(s, f0), f0);
		CHECK(std::exp2(ps.filter.log2Cutoff) == doctest::Approx(1000.0 * ratio).epsilon(1e-6));
		CHECK(ps.vowel.f1 == doctest::Approx(750.0 * ratio).epsilon(1e-5));
		CHECK(ps.vowel.f2 == doctest::Approx(1150.0 * ratio).epsilon(1e-5));
		CHECK(std::exp2(ps.tilt.log2Pivot) == doctest::Approx(1000.0)); // Tilt has no frequency to follow
	}
	// WITH KEY TRACK ON, THE NOTE: A3 (220 Hz) plus a fifth.
	s.keyOffset = 7.0f;
	const double f0 = shaperF0(s, true, 220.0);
	const feathers::spectral::PreparedShaper ps = feathers::spectral::prepare(shaperParamsOf(s, f0), f0);
	CHECK(std::exp2(ps.filter.log2Cutoff) == doctest::Approx(1000.0 * f0 / 261.6255653005986).epsilon(1e-6));
}

//--------------------------------------------------------------------------------------------------//
// THE VOCODER (design §10).                                                                          //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("an unconnected sidechain reads as none: no buffer, a constant-0 mask, or a second of exact silence") {
	feathers::device::SidechainPresence p;
	p.reset(48000.0);
	std::vector<float> sig(480, 0.25f), zero(480, 0.0f);
	CHECK_FALSE(p.update(nullptr, nullptr, 480, false, 48000.0));       // no buffer
	CHECK_FALSE(p.update(zero.data(), zero.data(), 480, true, 48000.0)); // flagged constant 0
	CHECK(p.update(sig.data(), sig.data(), 480, false, 48000.0));        // a signal
	// SILENT, BUT FOR LESS THAN A SECOND: still connected (a vocoder between words).
	for (int b = 0; b < 99; ++b) {
		CHECK(p.update(zero.data(), zero.data(), 480, false, 48000.0));
	}
	CHECK_FALSE(p.update(zero.data(), zero.data(), 480, false, 48000.0)); // the hundredth block: a second of silence
	CHECK(p.update(sig.data(), sig.data(), 480, false, 48000.0));         // and back the moment it sounds
}

#include "fx/spectral/spectral_envelope.h"
#include "pffft/pffft.h"

namespace {

/// THE ENVELOPE, RESTATED from the design (§10.2) with no sliding window: every bin from 20 Hz up against every grid point, the
/// triangle of half-width 10/Bands octaves over log2 Hz, the dB mean, the nearest bin where none falls under it.
std::array<float, feathers::fx::spectral::kEnvelopePoints> restatedEnvelope(const std::vector<float>& mag, double binHz, double bands,
                                                                       bool energy = false) {
	namespace sp = feathers::fx::spectral;
	std::array<float, sp::kEnvelopePoints> out {};
	const double half = 10.0 / bands;
	for (int g = 0; g < sp::kEnvelopePoints; ++g) {
		const double centre = std::log2(20.0) + 10.0 * g / (sp::kEnvelopePoints - 1);
		double sum = 0.0, weight = 0.0, best = 1e9, nearestDb = 0.0;
		for (size_t i = 1; i < mag.size(); ++i) {
			const double hz = static_cast<double>(i) * binHz;
			if (hz < 20.0) {
				continue;
			}
			const double d = std::log2(hz) - centre;
			const double db = 20.0 * std::log10(std::max(static_cast<double>(mag[i]), 1e-6));
			if (std::fabs(d) < best) {
				best = std::fabs(d);
				nearestDb = db;
			}
			if (std::fabs(d) <= half) {
				const double w = 1.0 - std::fabs(d) / half;
				sum += w * (energy ? std::max(static_cast<double>(mag[i]) * mag[i], 1e-12) : db);
				weight += w;
			}
		}
		out[static_cast<size_t>(g)] = static_cast<float>(weight > 0.0 ? (energy ? 10.0 * std::log10(std::max(sum / weight, 1e-12)) : sum / weight) : nearestDb);
	}
	return out;
}

/// The mean power per bin in each octave band from 100 Hz to 12.8 kHz of `x`, by averaged 4096-point FFTs (a Hann window), in dB.
std::vector<double> octaveBandsDb(const std::vector<float>& x, double sr) {
	constexpr int kN = 4096;
	PFFFT_Setup* setup = pffft_new_setup(kN, PFFFT_REAL);
	auto* in = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	auto* out = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	std::vector<double> power(kN / 2, 0.0);
	for (size_t at = 0; at + kN <= x.size(); at += kN / 2) {
		for (int j = 0; j < kN; ++j) {
			in[j] = x[at + static_cast<size_t>(j)] * static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / kN)));
		}
		pffft_transform_ordered(setup, in, out, nullptr, PFFFT_FORWARD);
		for (int i = 1; i < kN / 2; ++i) {
			power[static_cast<size_t>(i)] += static_cast<double>(out[2 * i]) * out[2 * i] + static_cast<double>(out[2 * i + 1]) * out[2 * i + 1];
		}
	}
	std::vector<double> bands;
	for (double lo = 100.0; lo < 12800.0; lo *= 2.0) {
		double p = 0.0;
		int count = 0;
		for (int i = 1; i < kN / 2; ++i) {
			const double hz = i * sr / kN;
			if (hz >= lo && hz < 2.0 * lo) {
				p += power[static_cast<size_t>(i)];
				++count;
			}
		}
		bands.push_back(10.0 * std::log10(p / std::max(count, 1) + 1e-30)); // the MEAN power per bin: white noise is flat
	}
	pffft_aligned_free(in);
	pffft_aligned_free(out);
	pffft_destroy_setup(setup);
	return bands;
}

} // namespace

TEST_CASE("vocoder: the envelope estimate is the design's, restated with no sliding window") {
	namespace sp = feathers::fx::spectral;
	// A HARMONIC SPECTRUM UNDER TWO FORMANTS: 110 Hz partials, peaks near 700 Hz and 2.4 kHz, as a voice's.
	const int bins = 961;
	const double binHz = 25.0;
	std::vector<float> mag(static_cast<size_t>(bins), 1e-4f);
	for (int k = 1; k * 110.0 < bins * binHz; ++k) {
		const double hz = k * 110.0;
		const double f = std::exp(-std::pow(std::log2(hz / 700.0) / 0.5, 2.0)) + 0.5 * std::exp(-std::pow(std::log2(hz / 2400.0) / 0.4, 2.0));
		mag[static_cast<size_t>(std::lround(hz / binHz))] = static_cast<float>(0.01 + f);
	}
	sp::EnvelopeEstimator est;
	est.prepare(bins);
	for (const double bands : { 8.0, 24.0, 64.0 }) {
		CAPTURE(bands);
		std::array<float, sp::kEnvelopePoints> got {};
		est.estimate(mag.data(), bins, binHz, bands, got);
		const auto want = restatedEnvelope(mag, binHz, bands);
		double worst = 0.0;
		for (size_t g = 0; g < got.size(); ++g) {
			worst = std::max(worst, static_cast<double>(std::fabs(got[g] - want[g])));
		}
		CHECK(worst < 1e-3); // dB
	}
}

TEST_CASE("vocoder: with a white-noise carrier the output takes the sidechain's envelope, within 3 dB per octave from 100 Hz to 12.8 kHz") {
	namespace sp = feathers::fx::spectral;
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	// THE SIDECHAIN: white noise through a one-pole low pass at 800 Hz — a slope of 6 dB/oct above it, an envelope to take.
	std::vector<float> carrier = noise(len, 0x13579bdu);
	std::vector<float> side = noise(len, 0x2468aceu);
	const double a = std::exp(-2.0 * std::numbers::pi * 800.0 / sr);
	double z = 0.0;
	for (auto& s : side) {
		z = (1.0 - a) * s + a * z;
		s = static_cast<float>(z);
	}
	Settings st;
	st.resolution = Resolution::Fine;
	st.vocodeOn = true;
	st.vocodeBands = 24.0f;
	st.vocodeFlatten = 1.0f;
	st.vocodeSibilance = 0.0f;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = carrier, r = carrier;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const std::vector<float> settled(l.begin() + 48000, l.end());
	const std::vector<float> sideSettled(side.begin() + 48000, side.end());
	const auto out = octaveBandsDb(settled, sr);
	const auto want = octaveBandsDb(sideSettled, sr);
	// SHAPE, NOT LEVEL: both referred to their own 800 Hz band (index 3).
	double worst = 0.0;
	for (size_t b = 0; b < out.size(); ++b) {
		const double diff = (out[b] - out[3]) - (want[b] - want[3]);
		CAPTURE(b);
		CAPTURE(diff);
		worst = std::max(worst, std::fabs(diff));
	}
	MESSAGE("worst octave-band shape error " << worst << " dB");
	CHECK(worst < 3.0);
	CHECK(want.front() - want.back() > 20.0); // the sidechain did have a shape to take
}

TEST_CASE("vocoder: Vocode off leaves the output bit for bit as without a sidechain, whatever the sidechain carries") {
	Settings off;
	off.filterOn = true;
	off.filterCutoff = static_cast<float>(std::log2(3000.0));
	off.vocodeOn = false;
	const std::vector<float> carrier = noise(30000);
	std::vector<float> side = noise(30000, 77u);
	const auto render = [&](bool giveBuffers) {
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(off);
		std::vector<float> l = carrier, r = carrier;
		for (size_t at = 0; at < l.size(); at += 256) {
			const int m = static_cast<int>(std::min<size_t>(256, l.size() - at));
			d.setSidechain(giveBuffers ? side.data() + at : nullptr, giveBuffers ? side.data() + at : nullptr, giveBuffers);
			d.processStereo(l.data() + at, r.data() + at, m);
		}
		return l;
	};
	CHECK(render(true) == render(false));
}

TEST_CASE("vocoder: Vocode on with a silent sidechain is silent while the carrier sounds — never the dry carrier") {
	// A SINGER'S PAUSE IS SILENCE, NOT A BYPASS. With no buffer at all, and with a buffer of zeros the presence calls silent, the
	// output reaches EXACT zero within the release, and stays there, while the carrier keeps sounding.
	Settings st;
	st.vocodeOn = true;
	st.vocodeSibilance = 1.0f;
	const auto lat = static_cast<size_t>(latencyFor(st.resolution, 48000.0));
	const auto n = static_cast<size_t>(fftSizeFor(st.resolution, 48000.0));
	for (const bool giveBuffers : { false, true }) {
		CAPTURE(giveBuffers);
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(st);
		std::vector<float> l = noise(48000), r = l, zeros(l.size(), 0.0f);
		for (size_t at = 0; at < l.size(); at += 256) {
			const int m = static_cast<int>(std::min<size_t>(256, l.size() - at));
			d.setSidechain(giveBuffers ? zeros.data() + at : nullptr, giveBuffers ? zeros.data() + at : nullptr, false);
			d.processStereo(l.data() + at, r.data() + at, m);
		}
		size_t nonzero = 0;
		for (size_t t = 2 * n + lat + 24000; t < l.size(); ++t) {
			nonzero += (l[t] != 0.0f) + (r[t] != 0.0f);
		}
		CHECK(nonzero == 0);
	}
}

TEST_CASE("vocoder: the tail reaches exact silence when the carrier and the sidechain stop") {
	Settings st;
	st.vocodeOn = true;
	st.vocodeSibilance = 1.0f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	const auto lat = static_cast<size_t>(latencyFor(st.resolution, 48000.0));
	const auto n = static_cast<size_t>(fftSizeFor(st.resolution, 48000.0));
	const size_t loud = 24000;
	std::vector<float> l = noise(loud + 4 * lat + 2 * n), side = noise(l.size(), 99u);
	for (size_t t = loud; t < l.size(); ++t) {
		l[t] = 0.0f;
		side[t] = 0.0f;
	}
	std::vector<float> r = l;
	for (size_t at = 0; at < l.size(); at += 128) {
		const int m = static_cast<int>(std::min<size_t>(128, l.size() - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	size_t nonzero = 0;
	for (size_t t = loud - 1 + n + lat; t < l.size(); ++t) {
		nonzero += (l[t] != 0.0f) + (r[t] != 0.0f);
	}
	CHECK(nonzero == 0);
	CHECK(l[loud + lat - 1] != 0.0f);
}

TEST_CASE("vocoder: Flatten takes the carrier's own envelope out — a sloped carrier under a flat sidechain comes out flat") {
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	std::vector<float> carrier = noise(len, 0x13579bdu);
	const std::vector<float> side = noise(len, 0x2468aceu); // white: an envelope with no shape
	const double a = std::exp(-2.0 * std::numbers::pi * 800.0 / sr);
	double z = 0.0;
	for (auto& s : carrier) {
		z = (1.0 - a) * s + a * z;
		s = static_cast<float>(z);
	}
	Settings st;
	st.resolution = Resolution::Fine;
	st.vocodeOn = true;
	st.vocodeFlatten = 1.0f;
	st.vocodeSibilance = 0.0f;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = carrier, r = carrier;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const auto out = octaveBandsDb(std::vector<float>(l.begin() + 48000, l.end()), sr);
	const auto in = octaveBandsDb(std::vector<float>(carrier.begin() + 48000, carrier.end()), sr);
	const double outSpread = *std::max_element(out.begin(), out.end()) - *std::min_element(out.begin(), out.end());
	const double inSpread = *std::max_element(in.begin(), in.end()) - *std::min_element(in.begin(), in.end());
	MESSAGE("carrier spread " << inSpread << " dB, output spread " << outSpread << " dB");
	CHECK(inSpread > 20.0); // the carrier had a slope to take out
	CHECK(outSpread < 3.0); // and it is gone
}

//--------------------------------------------------------------------------------------------------//
// FORMANT SHIFT (design §10.4).                                                                      //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A harmonic tone at f0 under one formant peak at `peakHz` (a Gaussian in log frequency), `len` samples.
std::vector<float> formantTone(double f0, double peakHz, size_t len, double sr) {
	std::vector<float> x(len, 0.0f);
	for (int k = 1; k * f0 < 0.45 * sr; ++k) {
		const double hz = k * f0;
		const double amp = 0.02 + std::exp(-std::pow(std::log2(hz / peakHz) / 0.35, 2.0));
		for (size_t t = 0; t < len; ++t) {
			x[t] += static_cast<float>(0.05 * amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t) / sr));
		}
	}
	return x;
}

} // namespace

TEST_CASE("formant shift: an octave up moves the envelope's peak an octave while the partials stay where they were") {
	constexpr double sr = 48000.0;
	constexpr size_t len = 48000;
	const std::vector<float> tone = formantTone(200.0, 1000.0, len, sr);
	const auto render = [&](bool shift) {
		Settings st;
		st.resolution = Resolution::Fine;
		st.fshiftOn = shift;
		st.fshiftLow = 12.0f;
		st.fshiftLink = true;
		st.vocodeBands = 32.0f;
		SpectralDevice d;
		d.prepare(sr);
		d.setTarget(st);
		std::vector<float> l = tone, r = tone;
		for (size_t at = 0; at < len; at += 512) {
			const int m = static_cast<int>(std::min<size_t>(512, len - at));
			d.processStereo(l.data() + at, r.data() + at, m);
		}
		return l;
	};
	const std::vector<float> plain = render(false);
	const std::vector<float> shifted = render(true);
	const size_t from = len / 2;
	const auto peakHarmonic = [&](const std::vector<float>& x) {
		int best = 1;
		double bestAmp = 0.0;
		for (int k = 1; k <= 40; ++k) {
			const double a = toneAmplitude(x, from, len, 200.0 * k, sr);
			if (a > bestAmp) {
				bestAmp = a;
				best = k;
			}
		}
		return best;
	};
	const int before = peakHarmonic(plain);
	const int after = peakHarmonic(shifted);
	CAPTURE(before);
	CAPTURE(after);
	CHECK(std::abs(before - 5) <= 1);  // 1 kHz is harmonic 5
	CHECK(std::abs(after - 10) <= 1);  // an octave up: harmonic 10, 2 kHz
	// THE PARTIALS STAY: between the harmonics there is next to nothing.
	double harmonic = 0.0, between = 0.0;
	for (int k = 3; k <= 20; ++k) {
		harmonic += toneAmplitude(shifted, from, len, 200.0 * k, sr);
		between += toneAmplitude(shifted, from, len, 200.0 * k + 100.0, sr);
	}
	CHECK(20.0 * std::log10(between / harmonic) < -30.0);
}

TEST_CASE("formant shift: while vocoding it moves the sidechain's envelope, so the output's shape is the sidechain's an octave up") {
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	std::vector<float> carrier = noise(len, 0x13579bdu);
	std::vector<float> side = noise(len, 0x2468aceu);
	// A PEAK TO MOVE: the sidechain through a resonant band pass at 1.1 kHz (a two-pole), inside the 800 Hz octave band.
	const double w = 2.0 * std::numbers::pi * 1100.0 / sr;
	const double rad = 0.995;
	double y1 = 0.0, y2 = 0.0;
	for (auto& s : side) {
		const double y = s + 2.0 * rad * std::cos(w) * y1 - rad * rad * y2;
		y2 = y1;
		y1 = y;
		s = static_cast<float>(0.01 * y);
	}
	Settings st;
	st.resolution = Resolution::Fine;
	st.vocodeOn = true;
	st.vocodeSibilance = 0.0f;
	st.fshiftOn = true;
	st.fshiftLow = 12.0f;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = carrier, r = carrier;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const auto out = octaveBandsDb(std::vector<float>(l.begin() + 48000, l.end()), sr);
	const auto want = octaveBandsDb(std::vector<float>(side.begin() + 48000, side.end()), sr);
	const auto summit = [](const std::vector<double>& v) { return std::max_element(v.begin(), v.end()) - v.begin(); };
	CAPTURE(summit(want));
	CAPTURE(summit(out));
	CHECK(summit(want) == 3);           // the sidechain's summit: the 800 Hz–1.6 kHz octave
	CHECK(summit(out) == summit(want) + 1); // the output's: an octave up
}

TEST_CASE("vocoder carrier: Input is the default, and vocoding the input is bit for bit what it was before the Carrier choice") {
	// THE REFERENCE IS A HASH OF THIS RENDER, so a carrier road that leaked into the Input one, by a sample or a bit, changes it.
	// Taken first before the carrier existed (ed8d0b81 plus the param alone); RE-PINNED 2026-10-17 when Flatten moved to energy per
	// band, which changes the Input carrier's sound on purpose (by design). RE-PINNED AGAIN for Z1b (the estimator on prefix sums,
	// fast log2/exp2): a precision-only change, the renders within -133 dB of the previous build's (measured, four scenes).
	CHECK(Settings {}.vocodeCarrier == static_cast<int>(Carrier::Input));
	CHECK(kParamDescs[static_cast<size_t>(Param::VocodeCarrier)].def == 0.0);
	Settings st;
	st.vocodeOn = true;
	st.vocodeSibilance = 0.5f;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	std::vector<float> l = noise(24000, 0xabcdefu), r = noise(24000, 0x123457u), side = noise(24000, 0x777u);
	for (size_t at = 0; at < l.size(); at += 300) {
		const int m = static_cast<int>(std::min<size_t>(300, l.size() - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < l.size(); ++i) {
		for (const float v : { l[i], r[i] }) {
			uint32_t bits = 0;
			std::memcpy(&bits, &v, sizeof bits);
			h = (h ^ bits) * 1099511628211ull;
		}
	}
	MESSAGE("input-carrier render hash " << h);
	// THE BITS ARE THIS PLATFORM'S (pffft's SIMD road and libm differ elsewhere), as `vox-voice` and `flagship` pin theirs.
#if defined(__x86_64__) && defined(__linux__)
	CHECK(h == 443368943146717043ull);
#endif
}

TEST_CASE("vocoder carrier: White and Pink each take the sidechain's envelope after flattening, within 3 dB per octave, with a silent input") {
	// THE SAME MEASURE AS THE INPUT CARRIER'S CASE (mean power per bin, octave by octave, shape referred to 800 Hz) with the input
	// SILENT, so whatever sounds is the internal carrier. MEASURED with Flatten by energy: 1.6 dB at 100-200 Hz, under 1 above
	// 400 Hz (the dB mean was 0.45 throughout: a power mean of a kernel's few low bins is the noisier estimate). The square has its
	// own case below: octave power is the wrong ruler for a line spectrum.
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	std::vector<float> side = noise(len, 0x2468aceu);
	const double a = std::exp(-2.0 * std::numbers::pi * 800.0 / sr);
	double z = 0.0;
	for (auto& s : side) {
		z = (1.0 - a) * s + a * z;
		s = static_cast<float>(z);
	}
	const std::vector<float> sideSettled(side.begin() + 48000, side.end());
	const auto want = octaveBandsDb(sideSettled, sr);
	for (const Carrier c : { Carrier::White, Carrier::Pink }) {
		CAPTURE(static_cast<int>(c));
		Settings st;
		st.resolution = Resolution::Fine;
		st.vocodeOn = true;
		st.vocodeCarrier = static_cast<int>(c);
		st.vocodeBands = 24.0f;
		st.vocodeFlatten = 1.0f;
		st.vocodeSibilance = 0.0f;
		SpectralDevice d;
		d.prepare(sr);
		d.setTarget(st);
		std::vector<float> l(len, 0.0f), r(len, 0.0f);
		for (size_t at = 0; at < len; at += 512) {
			const int m = static_cast<int>(std::min<size_t>(512, len - at));
			d.setSidechain(side.data() + at, side.data() + at, true);
			d.processStereo(l.data() + at, r.data() + at, m);
		}
		const std::vector<float> settled(l.begin() + 48000, l.end());
		const auto out = octaveBandsDb(settled, sr);
		double worst = 0.0;
		for (size_t b = 0; b < out.size(); ++b) {
			const double diff = (out[b] - out[3]) - (want[b] - want[3]);
			CAPTURE(b);
			CAPTURE(diff);
			worst = std::max(worst, std::fabs(diff));
		}
		MESSAGE("carrier " << static_cast<int>(c) << ": worst octave-band shape error " << worst << " dB");
		CHECK(worst < 3.0);
		CHECK(std::fabs(*std::max_element(settled.begin(), settled.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); })) > 0.01f); // it sounded
	}
}

TEST_CASE("vocoder carrier: the square's fundamental is the note, by value — C3 untracked, the played note tracked, and Offset never moves it (Z9)") {
	// THE CARRIER'S OWN SAMPLES, read from its ring: the time between the first and last rising zero crossing (each placed by
	// linear interpolation) over the crossings between them is its period. The span stays inside the ring (4096 at 48 kHz: 2048 is safe).
	const auto crossingsHz = [](const SpectralDevice& d, int span) {
		double first = -1.0;
		double last = -1.0;
		int count = 0;
		for (int ago = span - 1; ago > 0; --ago) {
			const float a = d.stftForTest().carrierAgoForTest(ago);
			const float b = d.stftForTest().carrierAgoForTest(ago - 1);
			if (a < 0.0f && b >= 0.0f) {
				const double t = static_cast<double>(span - 1 - ago) + static_cast<double>(a / (a - b));
				first = first < 0.0 ? t : first;
				last = t;
				++count;
			}
		}
		return count > 1 ? 48000.0 * (count - 1) / (last - first) : 0.0;
	};
	struct Want {
		bool keyTrack;
		float offset;
		int note;
		double hz;
	};
	// (WAS C3 / the note PLUS Offset until Z9: by decision the Square follows the note only.)
	for (const Want w : { Want { false, 7.0f, 0, kFixedF0Hz }, Want { true, 0.0f, 69, 440.0 }, Want { true, -12.0f, 69, 440.0 } }) {
		CAPTURE(w.hz);
		Settings st;
		st.vocodeOn = true;
		st.vocodeCarrier = static_cast<int>(Carrier::Square);
		st.keyTrack = w.keyTrack;
		st.keyOffset = w.offset;
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(st);
		if (w.keyTrack) {
			feathers::device::NoteContext note {};
			note.pitchSemitones = static_cast<float>(w.note);
			d.setNoteContext(note);
		}
		std::vector<float> l(96000, 0.0f), r(96000, 0.0f), side = noise(96000, 3u);
		for (size_t at = 0; at < l.size(); at += 256) {
			d.setSidechain(side.data() + at, side.data() + at, true);
			d.processStereo(l.data() + at, r.data() + at, 256);
		}
		CHECK(std::fabs(crossingsHz(d, 2048) - w.hz) <= 0.1);
	}
	// AND THE INPUT CARRIER WRITES NOTHING TO THE RING.
	Settings in;
	in.vocodeOn = true;
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(in);
	std::vector<float> l = noise(4096), r = l;
	d.processStereo(l.data(), r.data(), 4096);
	CHECK(d.stftForTest().carrierAgoForTest(0) == 0.0f);
}

TEST_CASE("vocoder carrier: switching Vocode on or off with an internal carrier crossfades from the INPUT, never puffing the raw carrier") {
	// SILENT INPUT, SILENT SIDECHAIN: both sides of the fade are silence, so any sample at all is the raw carrier leaking through
	// the crossfade (MEASURED before the fix: fx-devices heard the Carrier choice in the switch-on window).
	for (const Carrier c : { Carrier::White, Carrier::Pink }) {
		CAPTURE(static_cast<int>(c));
		Settings st;
		st.vocodeCarrier = static_cast<int>(c);
		SpectralDevice d;
		d.prepare(48000.0);
		size_t nonzero = 0;
		std::vector<float> l(256, 0.0f), r(256, 0.0f);
		for (int b = 0; b < 400; ++b) {
			st.vocodeOn = (b / 100) % 2 == 1; // off, on, off, on
			d.setTarget(st);
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.processStereo(l.data(), r.data(), 256);
			for (size_t i = 0; i < l.size(); ++i) {
				nonzero += (l[i] != 0.0f) + (r[i] != 0.0f);
			}
		}
		CHECK(nonzero == 0);
	}
}

namespace {

/// A SINE'S LEVEL in a whole signal, dB: the Hann-windowed 8192-point power at its bin (the strongest within three), averaged
/// over half-overlapping frames. Its own restatement, sharing nothing with the device but pffft.
double sineLevelDb(const std::vector<float>& x, double sr, double hz) {
	constexpr int kN = 8192;
	PFFFT_Setup* setup = pffft_new_setup(kN, PFFFT_REAL);
	auto* in = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	auto* out = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	const auto centre = static_cast<int>(std::lround(hz * kN / sr));
	std::vector<double> power(7, 0.0);
	for (size_t at = 0; at + kN <= x.size(); at += kN / 2) {
		for (int j = 0; j < kN; ++j) {
			in[j] = x[at + static_cast<size_t>(j)] * static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / kN)));
		}
		pffft_transform_ordered(setup, in, out, nullptr, PFFFT_FORWARD);
		for (int d = -3; d <= 3; ++d) {
			const int i = centre + d;
			power[static_cast<size_t>(d + 3)] += static_cast<double>(out[2 * i]) * out[2 * i] + static_cast<double>(out[2 * i + 1]) * out[2 * i + 1];
		}
	}
	pffft_aligned_free(in);
	pffft_aligned_free(out);
	pffft_destroy_setup(setup);
	return 10.0 * std::log10(*std::max_element(power.begin(), power.end()) + 1e-30);
}

struct Sine {
	double hz;
	float amp;
};

/// THE SINES, summed, through a Spectral with Partials set as given; the input and the output, each after a second's settling.
std::pair<std::vector<float>, std::vector<float>> partialsRender(const std::vector<Sine>& sines, Settings st) {
	constexpr double sr = 48000.0;
	constexpr size_t len = 3 * 48000;
	std::vector<float> x(len, 0.0f);
	for (size_t t = 0; t < len; ++t) {
		for (const Sine& s : sines) {
			x[t] += s.amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * s.hz * static_cast<double>(t) / sr));
		}
	}
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = x, r = x;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const auto lat = static_cast<size_t>(latencyFor(st.resolution, sr));
	std::vector<float> in(x.begin() + 48000, x.end() - static_cast<std::ptrdiff_t>(lat));
	std::vector<float> out(l.begin() + 48000 + static_cast<std::ptrdiff_t>(lat), l.end());
	return { in, out };
}

} // namespace

TEST_CASE("partials, Peaks: the N strongest peaks stay at their level, the rest go, and a fractional N weights the last one") {
	// FIVE SINES, EACH 6 dB BELOW THE LAST, none harmonic. N = 3: the three loudest stay within 0.5 dB, the two quietest fall by
	// at least 30. N = 3.5: the fourth comes back at half its amplitude (-6 dB, within 0.5).
	const std::vector<Sine> sines { { 330.0, 0.4f }, { 770.0, 0.2f }, { 1370.0, 0.1f }, { 2230.0, 0.05f }, { 3710.0, 0.025f } };
	for (const double n : { 3.0, 3.5 }) {
		CAPTURE(n);
		Settings st;
		st.resolution = Resolution::Fine;
		st.partialsOn = true;
		st.partialsMode = 0;
		st.partialsCount = static_cast<float>(n);
		st.partialsDepth = 1.0f;
		const auto [in, out] = partialsRender(sines, st);
		for (size_t k = 0; k < sines.size(); ++k) {
			CAPTURE(sines[k].hz);
			const double change = sineLevelDb(out, 48000.0, sines[k].hz) - sineLevelDb(in, 48000.0, sines[k].hz);
			CAPTURE(change);
			if (k < 3) {
				CHECK(std::fabs(change) < 0.5);
			}
			else if (k == 3 && n == 3.5) {
				CHECK(std::fabs(change - 20.0 * std::log10(0.5)) < 0.5);
			}
			else {
				CHECK(change < -30.0);
			}
		}
	}
}

TEST_CASE("partials, Harmonics: harmonics 1..N of f0 stay, every other partial goes — whether or not it is loud — and Depth sets how far") {
	// f0 IS C3 PLUS OFFSET (Key Track off): harmonics 1-3 of it and two loud inharmonic sines between them. N = 2 keeps
	// harmonics 1 and 2 and takes the third harmonic and both inharmonics down by at least 30 dB; at Depth 50% they fall by 6.
	const double f0 = kFixedF0Hz * std::exp2(2.0 / 12.0);
	const std::vector<Sine> sines { { f0, 0.1f }, { 2.0 * f0, 0.1f }, { 3.0 * f0, 0.1f }, { 1.6 * f0, 0.4f }, { 4.4 * f0, 0.4f } };
	for (const float depth : { 1.0f, 0.5f }) {
		CAPTURE(depth);
		Settings st;
		st.resolution = Resolution::Fine;
		st.keyOffset = 2.0f;
		st.partialsOn = true;
		st.partialsMode = 1;
		st.partialsCount = 2.0f;
		st.partialsDepth = depth;
		const auto [in, out] = partialsRender(sines, st);
		for (size_t k = 0; k < sines.size(); ++k) {
			CAPTURE(sines[k].hz);
			const double change = sineLevelDb(out, 48000.0, sines[k].hz) - sineLevelDb(in, 48000.0, sines[k].hz);
			CAPTURE(change);
			if (k < 2) {
				CHECK(std::fabs(change) < 0.5);
			}
			else if (depth == 1.0f) {
				CHECK(change < -30.0);
			}
			else {
				CHECK(std::fabs(change - 20.0 * std::log10(0.5)) < 0.5);
			}
		}
	}
}

namespace {

/// THE VOCODER'S STANDARD SCENE: a white-noise carrier under a sidechain of white noise through a one-pole low pass at 800 Hz
/// (6 dB/oct above it), four seconds; `st` vocodes. Returns the output and the sidechain, each after a second's settling.
std::pair<std::vector<float>, std::vector<float>> vocodedScene(Settings st, SpectralDevice* keep = nullptr) {
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	std::vector<float> carrier = noise(len, 0x13579bdu);
	std::vector<float> side = noise(len, 0x2468aceu);
	const double a = std::exp(-2.0 * std::numbers::pi * 800.0 / sr);
	double z = 0.0;
	for (auto& s : side) {
		z = (1.0 - a) * s + a * z;
		s = static_cast<float>(z);
	}
	SpectralDevice local;
	SpectralDevice& d = keep != nullptr ? *keep : local;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = carrier, r = carrier;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	return { std::vector<float>(l.begin() + 48000, l.end()), std::vector<float>(side.begin() + 48000, side.end()) };
}

Settings vocodeBasics() {
	Settings st;
	st.resolution = Resolution::Fine;
	st.vocodeOn = true;
	st.vocodeBands = 24.0f;
	st.vocodeFlatten = 1.0f;
	st.vocodeSibilance = 0.0f;
	return st;
}

} // namespace

TEST_CASE("vocoder Depth: 0 leaves the carrier's shape, 50% imposes half the sidechain's in dB, 200% doubles it below the peak") {
	// SHAPE AGAINST THE 100-200 Hz OCTAVE (the sidechain's loudest), octave by octave from 100 Hz to 12.8 kHz.
	for (const float depth : { 0.0f, 0.5f, 2.0f }) {
		CAPTURE(depth);
		Settings st = vocodeBasics();
		st.vocodeDepth = depth;
		const auto [out, side] = vocodedScene(st);
		const auto o = octaveBandsDb(out, 48000.0);
		const auto w = octaveBandsDb(side, 48000.0);
		double worst = 0.0;
		for (size_t b = 1; b < o.size(); ++b) {
			const double want = static_cast<double>(depth) * (w[b] - w[0]);
			const double diff = (o[b] - o[0]) - want;
			CAPTURE(b);
			CAPTURE(diff);
			worst = std::max(worst, std::fabs(diff));
		}
		MESSAGE("depth " << depth << ": worst octave-band error " << worst << " dB");
		// 200% DOUBLES THE ESTIMATE'S ERROR TOO: MEASURED 4.19 dB there with Flatten by energy.
		CHECK(worst < (depth == 2.0f ? 4.5 : 2.0));
	}
}

TEST_CASE("vocoder Range and Gate, by value: outside Low..High the imposed envelope is its edge level; below the Gate a point closes") {
	SpectralDevice d;
	Settings st = vocodeBasics();
	st.vocodeLow = static_cast<float>(std::log2(1000.0));
	st.vocodeHigh = static_cast<float>(std::log2(4000.0));
	(void)vocodedScene(st, &d);
	const auto& s = d.stftForTest().sidechainEnvelope();
	const auto& e = d.stftForTest().imposedEnvelopeForTest();
	// THE EDGES AS THE DEVICE HOLDS THEM: the stored log2 Hz, raised (exp2 of log2(1000) in float is not 1000).
	const double lowHz = std::exp2(static_cast<double>(st.vocodeLow));
	const double highHz = std::exp2(static_cast<double>(st.vocodeHigh));
	const float low = static_cast<float>(EnvelopeEstimator::sampleDb(s, lowHz));
	const float high = static_cast<float>(EnvelopeEstimator::sampleDb(s, highHz));
	size_t bad = 0;
	for (int g = 0; g < kEnvelopePoints; ++g) {
		const double hz = envelopeGridHz(g);
		const float want = hz < lowHz ? low : hz > highHz ? high : s[static_cast<size_t>(g)];
		bad += e[static_cast<size_t>(g)] != want;
	}
	CHECK(bad == 0);
	// THE GATE, halfway in dBFS between the envelope at 150 Hz and at 10 kHz: the points below it are the floor, the rest as they
	// were. dBFS is the card's scale: the grid plus 20 log10(pi / N).
	SpectralDevice g;
	Settings gt = vocodeBasics();
	(void)vocodedScene(gt, &g);
	const double toFs = 20.0 * std::log10(std::numbers::pi / fftSizeFor(Resolution::Fine, 48000.0));
	const auto& open = g.stftForTest().sidechainEnvelope();
	const double gate = 0.5 * (EnvelopeEstimator::sampleDb(open, 150.0) + EnvelopeEstimator::sampleDb(open, 10000.0)) + toFs;
	gt.vocodeGateDb = static_cast<float>(gate);
	SpectralDevice h;
	(void)vocodedScene(gt, &h);
	const auto& gs = h.stftForTest().sidechainEnvelope();
	const auto& ge = h.stftForTest().imposedEnvelopeForTest();
	size_t closed = 0;
	bad = 0;
	for (size_t p = 0; p < gs.size(); ++p) {
		const bool below = static_cast<double>(gs[p]) + toFs < static_cast<double>(gt.vocodeGateDb);
		closed += below;
		bad += ge[p] != (below ? kEnvelopeFloorDb : gs[p]);
	}
	CHECK(bad == 0);
	CHECK(closed > 10);                 // the gate did close something
	CHECK(closed < kEnvelopePoints - 10); // and left something open
}

TEST_CASE("vocoder Freeze, by value: the sidechain's envelope stays exactly as it was while the sidechain changes") {
	Settings st = vocodeBasics();
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	std::vector<float> l(512), r(512), side = noise(48000, 0x51u);
	for (size_t at = 0; at + 512 <= side.size(); at += 512) {
		std::fill(l.begin(), l.end(), 0.0f);
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data(), r.data(), 512);
	}
	const auto held = d.stftForTest().sidechainEnvelope();
	st.vocodeFreeze = true;
	d.setTarget(st);
	std::vector<float> other = noise(48000, 0x77u);
	for (auto& v : other) {
		v *= 0.1f; // 20 dB quieter: an envelope that moved would show it
	}
	for (size_t at = 0; at + 512 <= other.size(); at += 512) {
		d.setSidechain(other.data() + at, other.data() + at, true);
		d.processStereo(l.data(), r.data(), 512);
	}
	CHECK(d.stftForTest().sidechainEnvelope() == held);
	st.vocodeFreeze = false;
	d.setTarget(st);
	for (size_t at = 0; at + 512 <= other.size(); at += 512) {
		d.setSidechain(other.data() + at, other.data() + at, true);
		d.processStereo(l.data(), r.data(), 512);
	}
	CHECK(d.stftForTest().sidechainEnvelope() != held); // and it follows again once Freeze is off
}

namespace {

/// THE MEAN POWER PER BIN, dB, from `lo` to `hi` Hz: Hann frames of 8192, half-overlapping, averaged.
double bandPowerDb(const std::vector<float>& x, double sr, double lo, double hi) {
	constexpr int kN = 8192;
	PFFFT_Setup* setup = pffft_new_setup(kN, PFFFT_REAL);
	auto* in = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	auto* out = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	double p = 0.0;
	int count = 0;
	for (size_t at = 0; at + kN <= x.size(); at += kN / 2) {
		for (int j = 0; j < kN; ++j) {
			in[j] = x[at + static_cast<size_t>(j)] * static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / kN)));
		}
		pffft_transform_ordered(setup, in, out, nullptr, PFFFT_FORWARD);
		for (int i = 1; i < kN / 2; ++i) {
			const double hz = i * sr / kN;
			if (hz >= lo && hz < hi) {
				p += static_cast<double>(out[2 * i]) * out[2 * i] + static_cast<double>(out[2 * i + 1]) * out[2 * i + 1];
				++count;
			}
		}
	}
	pffft_aligned_free(in);
	pffft_aligned_free(out);
	pffft_destroy_setup(setup);
	return 10.0 * std::log10(p / std::max(count, 1) + 1e-30);
}

/// A STEREO VOCODE: white-noise carrier in both channels, the sidechain's two channels given; returns both outputs settled.
std::pair<std::vector<float>, std::vector<float>> vocodedStereo(Settings st, const std::vector<float>& sideL,
                                                               const std::vector<float>& sideR) {
	const size_t len = sideL.size();
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	std::vector<float> l = noise(len, 0x13579bdu), r = noise(len, 0x97531u);
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(sideL.data() + at, sideR.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	return { std::vector<float>(l.begin() + 48000, l.end()), std::vector<float>(r.begin() + 48000, r.end()) };
}

/// White noise through a one-pole low pass (`lowPass`) or high pass at `hz`.
std::vector<float> filteredNoise(size_t len, uint32_t seed, double hz, bool lowPass) {
	std::vector<float> x = noise(len, seed);
	const double a = std::exp(-2.0 * std::numbers::pi * hz / 48000.0);
	double z = 0.0;
	for (auto& s : x) {
		z = (1.0 - a) * s + a * z;
		s = lowPass ? static_cast<float>(z) : static_cast<float>(s - z);
	}
	return x;
}

} // namespace

TEST_CASE("vocoder Stereo: each channel takes its own sidechain channel's envelope; Mono gives both the sum's") {
	// THE LEFT SIDECHAIN FALLS ABOVE 800 Hz, THE RIGHT RISES BELOW 4 kHz: their 6.4-12.8 kHz octave against their 100-200 Hz one
	// differs by tens of dB, in opposite directions. Stereo carries each to its own channel (within 3 dB); Mono gives both the
	// same shape (within 1 dB of each other).
	constexpr size_t len = 4 * 48000;
	const auto sideL = filteredNoise(len, 0x2468aceu, 800.0, true);
	const auto sideR = filteredNoise(len, 0x1357u, 4000.0, false);
	const auto tilt = [](const std::vector<float>& x) {
		return bandPowerDb(x, 48000.0, 6400.0, 12800.0) - bandPowerDb(x, 48000.0, 100.0, 200.0);
	};
	const std::vector<float> settledL(sideL.begin() + 48000, sideL.end()), settledR(sideR.begin() + 48000, sideR.end());
	Settings st = vocodeBasics();
	st.vocodeStereo = 1;
	const auto [l, r] = vocodedStereo(st, sideL, sideR);
	MESSAGE("stereo: left tilt " << tilt(l) << " (sidechain " << tilt(settledL) << "), right " << tilt(r) << " (" << tilt(settledR) << ")");
	CHECK(std::fabs(tilt(l) - tilt(settledL)) < 3.0);
	CHECK(std::fabs(tilt(r) - tilt(settledR)) < 3.0);
	CHECK(tilt(r) - tilt(l) > 30.0);
	st.vocodeStereo = 0;
	const auto [ml, mr] = vocodedStereo(st, sideL, sideR);
	MESSAGE("mono: left tilt " << tilt(ml) << ", right " << tilt(mr));
	CHECK(std::fabs(tilt(ml) - tilt(mr)) < 1.0);
}

TEST_CASE("vocoder Spread: the bands alternate between the channels — even bands left, odd bands right") {
	// A FLAT SIDECHAIN, 8 bands of the Even layout over the default Range (band b centred at 20 Hz · 1000^((b + 0.5) / 8)): within a tenth of an octave of
	// each centre from 200 Hz to 10 kHz, the band's own channel is at least 15 dB above the other.
	constexpr size_t len = 4 * 48000;
	const auto side = noise(len, 0x4242u);
	Settings st = vocodeBasics();
	st.vocodeBands = 8.0f;
	st.vocodeStereo = 2;
	const auto [l, r] = vocodedStereo(st, side, side);
	int checked = 0;
	for (int b = 0; b < 8; ++b) {
		const double centre = 20.0 * std::pow(1000.0, (b + 0.5) / 8.0);
		if (centre < 200.0 || centre > 10000.0) {
			continue;
		}
		CAPTURE(b);
		const double lo = centre * std::exp2(-0.1);
		const double hi = centre * std::exp2(0.1);
		const double lr = bandPowerDb(l, 48000.0, lo, hi) - bandPowerDb(r, 48000.0, lo, hi);
		CAPTURE(lr);
		CHECK((b % 2 == 0 ? lr : -lr) > 15.0);
		++checked;
	}
	CHECK(checked >= 4);
}

TEST_CASE("Z1: the band weights are a partition of unity at every bin, for any count, Bandwidth and Range") {
	// DECIDED (§10.7.1): at every bin the bands' weights sum to exactly 1 — level and tilt can never move with the layout.
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	for (const float bands : { 8.0f, 24.0f, 128.0f }) {
		for (const float bw : { 0.0f, 0.5f, 0.99f }) {
			for (const bool ranged : { false, true }) {
				CAPTURE(bands);
				CAPTURE(bw);
				CAPTURE(ranged);
				Settings st = vocodeBasics();
				st.vocodeBands = bands;
				st.vocodeBandwidth = bw;
				if (ranged) {
					st.vocodeLow = static_cast<float>(std::log2(200.0));
					st.vocodeHigh = static_cast<float>(std::log2(8000.0));
				}
				SpectralDevice d;
				d.prepare(48000.0);
				d.setTarget(st);
				std::vector<float> l = noise(9600), r = l, side = noise(9600, 5u);
				for (size_t at = 0; at < l.size(); at += 480) {
					d.setSidechain(side.data() + at, side.data() + at, true);
					d.processStereo(l.data() + at, r.data() + at, 480);
				}
				double worst = 0.0;
				for (int i = 0; i <= n / 2; ++i) {
					worst = std::max(worst, std::fabs(static_cast<double>(d.stftForTest().bandWeightSumForTest(i)) - 1.0));
				}
				CHECK(worst < 1e-6);
			}
		}
	}
}

TEST_CASE("Z1: a flat sidechain comes out flat at its level through the bands, at any count and Bandwidth, edges included") {
	// WHITE CARRIER, WHITE SIDECHAIN: the output's octave shape, 100 Hz to 12.8 kHz, is flat within 0.5 dB per octave against the
	// same render on the smooth road (Bandwidth 100%) — the bands change detail, never level or tilt.
	constexpr size_t len = 3 * 48000;
	const auto side = noise(len, 0x4242u);
	const auto render = [&](float bands, float bw) {
		Settings st = vocodeBasics();
		st.vocodeBands = bands;
		st.vocodeBandwidth = bw;
		return vocodedStereo(st, side, side).first;
	};
	for (const float bands : { 8.0f, 24.0f, 128.0f }) {
		const auto smooth = octaveBandsDb(render(bands, 1.0f), 48000.0);
		for (const float bw : { 0.0f, 0.5f }) {
			CAPTURE(bands);
			CAPTURE(bw);
			const auto banded = octaveBandsDb(render(bands, bw), 48000.0);
			double worst = 0.0;
			for (size_t b = 0; b < banded.size(); ++b) {
				worst = std::max(worst, std::fabs(banded[b] - smooth[b]));
			}
			MESSAGE("bands " << bands << ", bandwidth " << bw << ": worst octave difference " << worst << " dB");
			CHECK(worst < 0.5);
		}
	}
}

TEST_CASE("Z1: on the banded road a Square carrier, measured per band on the same weights, puts its partials at the sidechain's level within 1 dB per octave from 1.6 kHz") {
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	const auto side = filteredNoise(len, 0x2468aceu, 800.0, true);
	Settings st = vocodeBasics();
	st.vocodeCarrier = static_cast<int>(Carrier::Square);
	st.vocodeBandwidth = 0.5f;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l(len, 0.0f), r(len, 0.0f);
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const std::vector<float> out(l.begin() + 48000, l.end());
	const std::vector<float> sideSettled(side.begin() + 48000, side.end());
	const double f0 = kFixedF0Hz;
	const auto partial = [&](double hz) { return sineLevelDb(out, sr, hz); };
	const auto wanted = [&](double hz) { return bandPowerDb(sideSettled, sr, hz * std::exp2(-1.0 / 12.0), hz * std::exp2(1.0 / 12.0)); };
	const double outRef = partial(5.0 * f0);
	const double wantRef = wanted(5.0 * f0);
	for (double lo = 1600.0; lo < 12800.0; lo *= 2.0) {
		double sum = 0.0;
		int count = 0;
		for (int k = 1; k * f0 < 2.0 * lo; k += 2) {
			const double hz = k * f0;
			if (hz >= lo) {
				sum += (partial(hz) - outRef) - (wanted(hz) - wantRef);
				++count;
			}
		}
		const double meanError = sum / std::max(count, 1);
		CAPTURE(lo);
		CAPTURE(meanError);
		MESSAGE("banded square, octave from " << lo << " Hz: mean error " << meanError << " dB");
		CHECK(std::fabs(meanError) < 1.0);
	}
}

TEST_CASE("vocoder Bandwidth (partition of unity): narrow bands make a staircase of the envelope, wide bands a smooth one — and neither moves a flat sidechain") {
	// THE RINGING COMB IS GONE (§10.7.1's decision: normalised per bin; a "Retro" unnormalised toggle is for later). What Bandwidth
	// does now: narrow, each bin takes its own band's level, so within a band the imposed envelope is flat; wide, it slopes on
	// through. Asserted on the imposed envelope at the bins a quarter-band either side of a band's centre, by value.
	const auto step = [&](float bw) {
		Settings st = vocodeBasics();
		st.vocodeBands = 8.0f;
		st.vocodeBandwidth = bw;
		SpectralDevice d;
		(void)vocodedScene(st, &d);
		// Band 5 of 8 over 20 Hz-20 kHz: centre 20 Hz · 1000^(5.5/8); a quarter of the spacing either side.
		const double spacing = std::log2(1000.0) / 8.0;
		const double centre = std::log2(20.0) + spacing * 5.5;
		const auto at = [&](double l2) {
			const int n = fftSizeFor(Resolution::Fine, 48000.0);
			const auto bin = static_cast<int>(std::lround(std::exp2(l2) * n / 48000.0));
			return d.stftForTest().sidechainBinEnvelopeForTest(bin);
		};
		return 20.0 * std::log10(std::max(at(centre - 0.25 * spacing), 1e-9f) / std::max(at(centre + 0.25 * spacing), 1e-9f));
	};
	const double narrow = step(0.0f);
	const double wide = step(0.99f);
	MESSAGE("level across half a band: narrow " << narrow << " dB, wide " << wide << " dB");
	CHECK(std::fabs(narrow) < 0.5); // a plateau
	CHECK(std::fabs(wide) > 2.0);   // a slope (the sidechain falls 6 dB/oct above 800 Hz; band 5 sits near 2.3 kHz)
}

TEST_CASE("vocoder Bands 128: at Fine, a high formant's envelope is finer at 128 bands than at 64, by value") {
	// A NARROW FORMANT AT 6 kHz (a sixth of an octave wide) on a flat floor, at Fine's bin spacing. The envelope's contrast — its
	// level at 6 kHz against a third of an octave either side — must rise by at least 2 dB from 64 bands to 128, and the 128 must
	// really be read as 128 (not clamped back to 64): its contrast differs from 64's.
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	const int bins = n / 2 + 1;
	const double binHz = 48000.0 / n;
	std::vector<float> mag(static_cast<size_t>(bins), 0.01f);
	for (int i = 1; i < bins; ++i) {
		const double oct = std::log2(i * binHz / 6000.0);
		mag[static_cast<size_t>(i)] += static_cast<float>(std::exp(-0.5 * std::pow(oct / (1.0 / 12.0), 2.0)));
	}
	EnvelopeEstimator est;
	est.prepare(bins);
	const auto contrast = [&](double bands) {
		std::array<float, kEnvelopePoints> grid {};
		est.estimate(mag.data(), bins, binHz, bands, grid);
		const double peak = EnvelopeEstimator::sampleDb(grid, 6000.0);
		const double side = 0.5 * (EnvelopeEstimator::sampleDb(grid, 6000.0 * std::exp2(-1.0 / 3.0))
		                           + EnvelopeEstimator::sampleDb(grid, 6000.0 * std::exp2(1.0 / 3.0)));
		return peak - side;
	};
	const double c64 = contrast(64.0);
	const double c128 = contrast(128.0);
	MESSAGE("formant contrast at 64 bands " << c64 << " dB, at 128 " << c128 << " dB");
	CHECK(c128 > c64 + 2.0);
	CHECK(kParamDescs[static_cast<size_t>(Param::VocodeBands)].max == 128.0);
}

TEST_CASE("partials past 64: Harmonics keeps harmonic 80 of C3 at N = 80, half of it at 79.5, by value") {
	// f0 = C3 at Fine (25 Hz bins): harmonic 80 sits at 20.9 kHz, ten bins from 79, so the lobes (two bins each side) leave the bin
	// halfway between them untouched. Read the gain the stage gave each.
	const double f0 = kFixedF0Hz;
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	const double binHz = 48000.0 / n;
	const auto at = [&](double hz) { return static_cast<int>(std::lround(hz / binHz)); };
	CHECK(kParamDescs[static_cast<size_t>(Param::PartialsCount)].max == 320.0);
	for (const float count : { 80.0f, 79.5f }) {
		CAPTURE(count);
		Settings st;
		st.resolution = Resolution::Fine;
		st.partialsOn = true;
		st.partialsMode = 1;
		st.partialsCount = count;
		st.partialsDepth = 1.0f;
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(st);
		std::vector<float> l = noise(9600), r = l;
		for (size_t s = 0; s < l.size(); s += 480) {
			d.processStereo(l.data() + s, r.data() + s, 480);
		}
		CHECK(d.stftForTest().partialsGainForTest(at(80.0 * f0)) == doctest::Approx(count == 80.0f ? 1.0f : 0.5f));
		CHECK(d.stftForTest().partialsGainForTest(at(79.5 * f0)) == 0.0f);
	}
}

TEST_CASE("MEASURE: Partials' cost per block at Fine, N 64 against 320, Peaks and Harmonics, Mono and 8-voice Poly" * doctest::skip()) {
	// TIME PER 256-FRAME BLOCK, the median of 600 blocks, against Balanced at 64 (the reference), in rounds that alternate the order
	// of the configurations so a shared host's load swings land on all of them (benchmarking-on-a-shared-host).
	struct Config {
		const char* name;
		Resolution res;
		float count;
		int mode;
		bool poly;
	};
	const std::vector<Config> configs {
		{ "Balanced N64 Peaks mono", Resolution::Balanced, 64.0f, 0, false },
		{ "Fine N64 Peaks mono", Resolution::Fine, 64.0f, 0, false },
		{ "Fine N320 Peaks mono", Resolution::Fine, 320.0f, 0, false },
		{ "Fine N64 Harmonics mono", Resolution::Fine, 64.0f, 1, false },
		{ "Fine N320 Harmonics mono", Resolution::Fine, 320.0f, 1, false },
		{ "Balanced N64 Peaks poly8", Resolution::Balanced, 64.0f, 0, true },
		{ "Fine N64 Peaks poly8", Resolution::Fine, 64.0f, 0, true },
		{ "Fine N320 Peaks poly8", Resolution::Fine, 320.0f, 0, true },
		{ "Fine N64 Harmonics poly8", Resolution::Fine, 64.0f, 1, true },
		{ "Fine N320 Harmonics poly8", Resolution::Fine, 320.0f, 1, true },
	};
	const std::vector<float> input = noise(48000, 0x5eedu);
	std::vector<std::vector<double>> medians(configs.size());
	for (int round = 0; round < 6; ++round) {
		for (size_t k = 0; k < configs.size(); ++k) {
			const size_t c = round % 2 == 0 ? k : configs.size() - 1 - k;
			const Config& cfg = configs[c];
			Settings st;
			st.resolution = cfg.res;
			st.partialsOn = true;
			st.partialsMode = cfg.mode;
			st.partialsCount = cfg.count;
			const int devices = cfg.poly ? 8 : 1;
			std::vector<std::unique_ptr<SpectralDevice>> d;
			for (int v = 0; v < devices; ++v) {
				d.push_back(std::make_unique<SpectralDevice>());
				d.back()->prepare(48000.0);
				d.back()->setTarget(st);
			}
			std::vector<float> l(256), r(256);
			std::vector<double> times;
			for (int b = 0; b < 700; ++b) {
				const size_t at = (static_cast<size_t>(b) * 256) % (input.size() - 256);
				const auto t0 = std::chrono::steady_clock::now();
				for (int v = 0; v < devices; ++v) {
					std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, l.begin());
					if (cfg.poly) {
						d[static_cast<size_t>(v)]->processMono(l.data(), 256);
					}
					else {
						std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, r.begin());
						d[static_cast<size_t>(v)]->processStereo(l.data(), r.data(), 256);
					}
				}
				const auto t1 = std::chrono::steady_clock::now();
				if (b >= 100) {
					times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
				}
			}
			std::nth_element(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2), times.end());
			medians[c].push_back(times[times.size() / 2]);
		}
	}
	for (size_t c = 0; c < configs.size(); ++c) {
		auto m = medians[c];
		std::sort(m.begin(), m.end());
		const auto ref = [&](size_t r) {
			auto x = medians[r];
			std::sort(x.begin(), x.end());
			return x[x.size() / 2];
		};
		const double base = ref(configs[c].poly ? 5 : 0);
		MESSAGE(std::string(configs[c].name) << ": median " << m[m.size() / 2] << " us/block (min " << m.front() << ", max " << m.back()
		                        << "), " << m[m.size() / 2] / base << "x the Balanced N64 reference");
	}
}

TEST_CASE("vocoder carrier: a Square, flattened by energy per band, puts each partial at the sidechain's level, within 1 dB per octave to 12.8 kHz") {
	// THE RULER FOR A LINE SPECTRUM IS ITS PARTIALS. Each odd harmonic of the C3 square from 1.6 to 12.8 kHz: its level in the
	// output against the sidechain's mean power per bin within a sixth of an octave of it, both referred to the harmonic nearest
	// 1.3 kHz; the octave's mean error is under 1 dB in every octave. BELOW 1.6 kHz A BAND (24 of them) HOLDS ONE ODD PARTIAL OR
	// NONE (they are 523 Hz apart), so its energy is that partial's and not a density: MEASURED -1.8 dB for the one partial at 785 Hz,
	// the filterbank's own behaviour, so it is reported, not asserted. (Flattened by the dB mean it was 6.2 dB bright by 12.8 kHz:
	// the mean counted the gaps between partials, and there are more partials per kernel the higher it looks.)
	constexpr double sr = 48000.0;
	constexpr size_t len = 4 * 48000;
	const auto side = filteredNoise(len, 0x2468aceu, 800.0, true);
	Settings st = vocodeBasics();
	st.vocodeCarrier = static_cast<int>(Carrier::Square);
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l(len, 0.0f), r(len, 0.0f);
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const std::vector<float> out(l.begin() + 48000, l.end());
	const std::vector<float> sideSettled(side.begin() + 48000, side.end());
	const auto partial = [&](double hz) { return sineLevelDb(out, sr, hz); };
	const auto wanted = [&](double hz) { return bandPowerDb(sideSettled, sr, hz * std::exp2(-1.0 / 12.0), hz * std::exp2(1.0 / 12.0)); };
	const double f0 = kFixedF0Hz;
	const double ref = 5.0 * f0; // 1308 Hz
	const double outRef = partial(ref);
	const double wantRef = wanted(ref);
	for (double lo = 400.0; lo < 12800.0; lo *= 2.0) {
		const bool dense = lo >= 1600.0;
		double sum = 0.0;
		int count = 0;
		for (int k = 1; k * f0 < 2.0 * lo; k += 2) {
			const double hz = k * f0;
			if (hz < lo) {
				continue;
			}
			sum += (partial(hz) - outRef) - (wanted(hz) - wantRef);
			++count;
		}
		CAPTURE(lo);
		CAPTURE(count);
		const double meanError = count > 0 ? sum / count : 0.0;
		CAPTURE(meanError);
		MESSAGE("square, octave from " << lo << " Hz: " << count << " partials, mean error " << meanError << " dB");
		CHECK(count > 0);
		if (dense) {
			CHECK(std::fabs(meanError) < 1.0);
		}
	}
}

TEST_CASE("vocoder Gate: at its bottom it reads Off and gates nothing; a hair above, it is a level") {
	char text[32] = {};
	const feathers::device::TextView view {};
	const auto gate = static_cast<size_t>(Param::VocodeGate);
	REQUIRE(kTextOps.format(gate, kVocodeGateOffDb, view, text, sizeof(text)));
	CHECK(std::string(text) == "Off");
	CHECK_FALSE(kTextOps.format(gate, -40.0, view, text, sizeof(text))); // the declared unit prints it: a level in dB
	// AND THE ENGINE AGREES: at the bottom nothing is imposed through a copy (the gate is off), so the imposed envelope is gridS.
	SpectralDevice d;
	Settings st = vocodeBasics();
	st.vocodeGateDb = static_cast<float>(kVocodeGateOffDb);
	(void)vocodedScene(st, &d);
	CHECK(&d.stftForTest().imposedEnvelopeForTest() == &d.stftForTest().sidechainEnvelope());
}

TEST_CASE("Z1: band by band, the output's energy is the sidechain's — a loud partial inside one band moves no other band's level") {
	// NARROW BANDS (8, Bandwidth 0), A FLAT SIDECHAIN, and a carrier of white noise with a loud sine at band 5's centre
	// (its leakage stays in its own band). Measured per band on the layout's own weights (restated here: Gaussians of sigma 0.15 of the spacing, normalised per
	// bin), every band's output energy is the same within 1.5 dB: the carrier's energy is taken where the band takes it, so the
	// sine is flattened inside its band and no neighbour pays for it. (Measured off the bands, by the smooth kernel 1.25 octaves
	// wide, the sine's energy lowers its neighbours too.)
	constexpr double sr = 48000.0;
	constexpr size_t len = 3 * 48000;
	const double spacing = std::log2(1000.0) / 8.0;
	const double sineHz = 20.0 * std::exp2(spacing * (5.0 + 0.5)); // band 5's centre
	std::vector<float> carrier = noise(len, 0x13579bdu);
	for (size_t t = 0; t < len; ++t) {
		carrier[t] = 0.05f * carrier[t] + 0.15f * static_cast<float>(std::sin(2.0 * std::numbers::pi * sineHz * static_cast<double>(t) / sr));
	}
	const auto side = noise(len, 0x4242u);
	Settings st = vocodeBasics();
	st.vocodeBands = 8.0f;
	st.vocodeBandwidth = 0.0f;
	SpectralDevice d;
	d.prepare(sr);
	d.setTarget(st);
	std::vector<float> l = carrier, r = carrier;
	for (size_t at = 0; at < len; at += 512) {
		const int m = static_cast<int>(std::min<size_t>(512, len - at));
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data() + at, r.data() + at, m);
	}
	const std::vector<float> out(l.begin() + 48000, l.end());
	// THE OUTPUT'S POWER PER BIN, Hann 8192, averaged; each band's W-weighted mean.
	constexpr int kN = 8192;
	PFFFT_Setup* setup = pffft_new_setup(kN, PFFFT_REAL);
	auto* in = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	auto* spec = static_cast<float*>(pffft_aligned_malloc(kN * sizeof(float)));
	std::vector<double> power(kN / 2, 0.0);
	for (size_t at = 0; at + kN <= out.size(); at += kN / 2) {
		for (int j = 0; j < kN; ++j) {
			in[j] = out[at + static_cast<size_t>(j)] * static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * j / kN)));
		}
		pffft_transform_ordered(setup, in, spec, nullptr, PFFFT_FORWARD);
		for (int i = 1; i < kN / 2; ++i) {
			power[static_cast<size_t>(i)] += static_cast<double>(spec[2 * i]) * spec[2 * i] + static_cast<double>(spec[2 * i + 1]) * spec[2 * i + 1];
		}
	}
	pffft_aligned_free(in);
	pffft_aligned_free(spec);
	pffft_destroy_setup(setup);
	std::array<double, 8> e {}, wsum {};
	const double sigma = 0.15 * spacing;
	for (int i = 1; i < kN / 2; ++i) {
		const double hz = i * sr / kN;
		if (hz < 100.0 || hz > 12800.0) {
			continue;
		}
		const double u = (std::log2(hz) - std::log2(20.0)) / spacing - 0.5;
		std::array<double, 8> g {};
		double sum = 0.0;
		for (int b = 0; b < 8; ++b) {
			g[static_cast<size_t>(b)] = std::exp(-0.5 * std::pow((u - b) * spacing / sigma, 2.0));
			sum += g[static_cast<size_t>(b)];
		}
		for (int b = 0; b < 8; ++b) {
			const double w = sum > 0.0 ? g[static_cast<size_t>(b)] / sum : 0.0;
			e[static_cast<size_t>(b)] += w * power[static_cast<size_t>(i)];
			wsum[static_cast<size_t>(b)] += w;
		}
	}
	// BANDS 2 TO 6 (100 Hz-12.8 kHz covers them whole), each against band 3.
	std::array<double, 8> db {};
	for (int b = 2; b <= 6; ++b) {
		db[static_cast<size_t>(b)] = 10.0 * std::log10(e[static_cast<size_t>(b)] / wsum[static_cast<size_t>(b)] + 1e-30);
	}
	for (int b = 2; b <= 6; ++b) {
		CAPTURE(b);
		const double diff = db[static_cast<size_t>(b)] - db[3];
		CAPTURE(diff);
		MESSAGE("band " << b << ": " << diff << " dB against band 3");
		CHECK(std::fabs(diff) < 1.5);
	}
}

TEST_CASE("MEASURE: the vocoder's cost per block at Fine, the smooth road against the banded one (8, 24, 128 bands), one instance and eight" * doctest::skip()) {
	// TIME PER 256-FRAME BLOCK, the median of 500 blocks, rounds alternating the order (benchmarking-on-a-shared-host). The
	// banded road rebuilds its layout only when the layout moves, so a still one costs the per-bin weighted sums alone.
	struct Config {
		const char* name;
		float bands;
		float bw;
		int instances;
		int zone = 0;
	};
	const std::vector<Config> configs {
		{ "smooth 24 x1", 24.0f, 1.0f, 1 },  { "banded 8 x1", 8.0f, 0.5f, 1 },  { "banded 24 x1", 24.0f, 0.5f, 1 },
		{ "banded 128 x1", 128.0f, 0.5f, 1 }, { "smooth 24 x8", 24.0f, 1.0f, 8 }, { "banded 24 x8", 24.0f, 0.5f, 8 },
		{ "banded 128 x8", 128.0f, 0.5f, 8 },
		{ "speech 24 x1", 24.0f, 1.0f, 1, 1 }, { "speech 24 x8", 24.0f, 1.0f, 8, 1 },
	};
	const std::vector<float> input = noise(48000, 0x5eedu);
	const std::vector<float> side = noise(48000, 0x5eeeu);
	std::vector<std::vector<double>> medians(configs.size());
	for (int round = 0; round < 6; ++round) {
		for (size_t k = 0; k < configs.size(); ++k) {
			const size_t c = round % 2 == 0 ? k : configs.size() - 1 - k;
			const Config& cfg = configs[c];
			Settings st = vocodeBasics();
			st.vocodeBands = cfg.bands;
			st.vocodeBandwidth = cfg.bw;
			st.vocodeZone = cfg.zone;
			st.vocodeZoneAmount = 1.0f;
			std::vector<std::unique_ptr<SpectralDevice>> d;
			for (int v = 0; v < cfg.instances; ++v) {
				d.push_back(std::make_unique<SpectralDevice>());
				d.back()->prepare(48000.0);
				d.back()->setTarget(st);
			}
			std::vector<float> l(256), r(256);
			std::vector<double> times;
			for (int b = 0; b < 600; ++b) {
				const size_t at = (static_cast<size_t>(b) * 256) % (input.size() - 256);
				const auto t0 = std::chrono::steady_clock::now();
				for (auto& dev : d) {
					std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, l.begin());
					std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, r.begin());
					dev->setSidechain(side.data() + at, side.data() + at, true);
					dev->processStereo(l.data(), r.data(), 256);
				}
				const auto t1 = std::chrono::steady_clock::now();
				if (b >= 100) {
					times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
				}
			}
			std::nth_element(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2), times.end());
			medians[c].push_back(times[times.size() / 2]);
		}
	}
	for (size_t c = 0; c < configs.size(); ++c) {
		auto m = medians[c];
		std::sort(m.begin(), m.end());
		MESSAGE(std::string(configs[c].name) << ": median " << m[m.size() / 2] << " us/block (min " << m.front() << ", max " << m.back() << ")");
	}
}
TEST_CASE("Z1b: fastLog2 is within 1e-5 of log2 and fastExp2 within 2e-7 of exp2, relative, over the vocoder's ranges") {
	double worstLog = 0.0;
	for (double e = -40.0; e <= 20.0; e += 0.001) {
		const double x = std::exp2(e) * (1.0 + 0.37 * std::fabs(std::sin(e * 7.1)));
		worstLog = std::max(worstLog, std::fabs(fastLog2(x) - std::log2(x)));
	}
	double worstExp = 0.0;
	for (double y = -60.0; y <= 30.0; y += 0.0007) {
		worstExp = std::max(worstExp, std::fabs(fastExp2(y) / std::exp2(y) - 1.0));
	}
	MESSAGE("fastLog2 worst " << worstLog << ", fastExp2 worst " << worstExp << " relative");
	CHECK(worstLog < 1e-5);
	CHECK(worstExp < 2e-7);
}

TEST_CASE("Z1b: the prefix-sum estimator is the design's envelope, restated with a loop per grid point, within 1e-3 dB (the energy mean 5e-3) — every count") {
	// RANDOM SPECTRA spanning 120 dB, at Fine's and Balanced's bin spacing: the estimator (prefix sums, fast log2) against the
	// restatement (a kernel loop per grid point, std::log10), both the dB mean and the energy mean.
	uint32_t seed = 12345;
	for (const Resolution res : { Resolution::Fine, Resolution::Balanced }) {
		const int n = fftSizeFor(res, 48000.0);
		const int bins = n / 2 + 1;
		const double binHz = 48000.0 / n;
		std::vector<float> mag(static_cast<size_t>(bins));
		for (auto& m : mag) {
			seed = seed * 1664525u + 1013904223u;
			m = static_cast<float>(std::pow(10.0, -6.0 + 6.0 * (seed >> 8) / 16777216.0));
		}
		EnvelopeEstimator est;
		est.prepare(bins);
		for (const double bands : { 8.0, 24.0, 64.0, 128.0 }) {
			for (const bool energy : { false, true }) {
				CAPTURE(static_cast<int>(res));
				CAPTURE(bands);
				CAPTURE(energy);
				std::array<float, kEnvelopePoints> got {};
				est.estimate(mag.data(), bins, binHz, bands, got, energy);
				const auto want = restatedEnvelope(mag, binHz, bands, energy);
				double worst = 0.0;
				for (size_t g = 0; g < got.size(); ++g) {
					worst = std::max(worst, static_cast<double>(std::fabs(got[g] - want[g])));
				}
				// THE ENERGY MEAN'S PREFIX SUMS CANCEL MORE: power spans 120 dB here, and a quiet kernel after loud bins is a small
				// difference of large sums. MEASURED worst 1.5e-3 dB (128 bands); the dB mean's stays under 1e-3.
				CHECK(worst < (energy ? 5e-3 : 1e-3));
			}
		}
	}
}

namespace {

/// A VOCODING DEVICE WITH `st`, run a fifth of a second so its layout is built; for reading the layout by value.
std::unique_ptr<SpectralDevice> laidOut(const Settings& st) {
	auto d = std::make_unique<SpectralDevice>();
	d->prepare(48000.0);
	d->setTarget(st);
	std::vector<float> l = noise(9600), r = l, side = noise(9600, 5u);
	for (size_t at = 0; at < l.size(); at += 480) {
		d->setSidechain(side.data() + at, side.data() + at, true);
		d->processStereo(l.data() + at, r.data() + at, 480);
	}
	return d;
}

} // namespace

TEST_CASE("Z2: Bands always sets the count; amount 0 is Even exactly; at full Speech the centres are even in Bark") {
	const auto bark = [](double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; };
	for (const int zone : { 1, 2, 3 }) {
		for (const float bands : { 8.0f, 24.0f, 128.0f }) {
			CAPTURE(zone);
			CAPTURE(bands);
			Settings st = vocodeBasics();
			st.vocodeBands = bands;
			st.vocodeBandwidth = 0.5f;
			st.vocodeZone = zone;
			st.vocodeZoneAmount = 0.0f;
			auto even = laidOut(st);
			CHECK(even->stftForTest().bandCountForTest() == static_cast<int>(bands));
			// AMOUNT 0 IS EVEN: centre b at log2(20) + log2(1000) (b + 0.5) / count.
			double worst = 0.0;
			for (int b = 0; b < static_cast<int>(bands); ++b) {
				const double want = std::log2(20.0) + std::log2(1000.0) * (b + 0.5) / bands;
				worst = std::max(worst, std::fabs(even->stftForTest().bandCentreForTest(b) - want));
			}
			CHECK(worst < 1e-6); // the Range is stored as log2 Hz in a float: exp2 of it is 20 and 20000 within 1e-6 octave
			st.vocodeZoneAmount = 1.0f;
			auto full = laidOut(st);
			CHECK(full->stftForTest().bandCountForTest() == static_cast<int>(bands));
			if (zone == 1) {
				// FULL SPEECH: equal steps in Traunmüller's Bark between Range Low and High.
				const double zl = bark(20.0);
				const double zh = bark(20000.0);
				double worstZ = 0.0;
				for (int b = 0; b < static_cast<int>(bands); ++b) {
					const double z = bark(std::exp2(full->stftForTest().bandCentreForTest(b)));
					worstZ = std::max(worstZ, std::fabs(z - (zl + (zh - zl) * (b + 0.5) / bands)));
				}
				CHECK(worstZ < 0.01);
			}
		}
	}
}

TEST_CASE("Z2: Low crowds the bands toward the lows and High toward the highs, more with the amount") {
	// THE BANDS' MEAN CENTRE in log2 Hz, 24 over 20 Hz-20 kHz: Low lowers it at every step of its amount, High raises it.
	const auto below = [](const SpectralDevice& d) {
		double sum = 0.0;
		for (int b = 0; b < d.stftForTest().bandCountForTest(); ++b) {
			sum += d.stftForTest().bandCentreForTest(b);
		}
		return sum / d.stftForTest().bandCountForTest();
	};
	double lastLow = 1000.0;
	double lastHigh = -1.0;
	for (const float amount : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		CAPTURE(amount);
		Settings st = vocodeBasics();
		st.vocodeBandwidth = 0.5f;
		st.vocodeZoneAmount = amount;
		st.vocodeZone = 2;
		const double low = below(*laidOut(st));
		st.vocodeZone = 3;
		const double high = below(*laidOut(st));
		MESSAGE("amount " << amount << ": mean centre, Low " << std::exp2(low) << " Hz, High " << std::exp2(high) << " Hz");
		if (amount > 0.0f) {
			CHECK(low < lastLow);
			CHECK(high > lastHigh);
		}
		lastLow = low;
		lastHigh = high;
	}
}

TEST_CASE("Z2: every Zone keeps the partition of unity and a flat sidechain's level, and puts the vocoder on the banded road at any Bandwidth") {
	constexpr size_t len = 3 * 48000;
	const auto side = noise(len, 0x4242u);
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	Settings smoothSt = vocodeBasics();
	const auto smooth = octaveBandsDb(vocodedStereo(smoothSt, side, side).first, 48000.0);
	for (const int zone : { 1, 2, 3 }) {
		CAPTURE(zone);
		Settings st = vocodeBasics(); // Bandwidth 100%: the Zone alone takes the banded road
		st.vocodeZone = zone;
		st.vocodeZoneAmount = 1.0f;
		auto d = laidOut(st);
		CHECK(d->stftForTest().banded());
		double worstSum = 0.0;
		for (int i = 0; i <= n / 2; ++i) {
			worstSum = std::max(worstSum, std::fabs(static_cast<double>(d->stftForTest().bandWeightSumForTest(i)) - 1.0));
		}
		CHECK(worstSum < 1e-6);
		const auto banded = octaveBandsDb(vocodedStereo(st, side, side).first, 48000.0);
		double worst = 0.0;
		for (size_t b = 0; b < banded.size(); ++b) {
			worst = std::max(worst, std::fabs(banded[b] - smooth[b]));
		}
		MESSAGE("zone " << zone << ": flat sidechain, worst octave difference " << worst << " dB");
		// WITHIN 1 dB, NOT Z1's 0.5: MEASURED worst 0.93 dB (Speech, 200-400 Hz), all of it below 800 Hz and all of it louder. The
		// weights partition unity (asserted above, 1e-6); what differs is how noisily a white carrier's energy is estimated from the
		// few bins a low band holds at Fine, which the smooth road's kernel does differently (it is itself +1.6 dB there against the
		// sidechain). A shape, not a level or a tilt the layout makes; filed in the design's status.
		CHECK(worst < 1.0);
	}
	// AND EVEN AT 100% IS THE SMOOTH ROAD, as it always was.
	CHECK_FALSE(laidOut(smoothSt)->stftForTest().banded());
}

namespace {

/// THE STFT ALONE, vocoding with Zone Harmonic at `bias`, f0 `f0`, `bands` bands, the carrier and sidechain given, run over
/// `frames` samples; for reading the layout by value. Fine resolution.
std::unique_ptr<Stft> harmonicStft(double bias, double f0, int bands, const std::vector<float>& carrier, const std::vector<float>& side,
                                   size_t frames, std::unique_ptr<Stft> keep = nullptr) {
	const bool fresh = keep == nullptr;
	auto s = fresh ? std::make_unique<Stft>() : std::move(keep);
	if (fresh) {
		s->prepare(48000.0);
	}
	s->setResolution(Resolution::Fine);
	Stft::Vocode v;
	v.on = true;
	v.bands = bands;
	v.flatten = 1.0f;
	v.bandwidth = 0.5;
	v.zone = bias < 0.0 ? 5 : 4; // Harmonic, or Inharmonic (two menu entries)
	v.zoneAmount = std::fabs(bias);
	s->setVocode(v);
	s->setF0Hz(f0);
	s->setIdentity(false);
	std::vector<float> l(256), r(256);
	for (size_t at = 0; at + 256 <= frames; at += 256) {
		const size_t k = at % (carrier.size() - 256);
		std::copy_n(carrier.begin() + static_cast<std::ptrdiff_t>(k), 256, l.begin());
		std::copy_n(carrier.begin() + static_cast<std::ptrdiff_t>(k), 256, r.begin());
		s->setSidechain(side.data() + k, side.data() + k, true);
		s->process(l.data(), r.data(), 256);
	}
	return s;
}

/// A harmonic carrier at f0 (partials 1..40 at 1/k), one partial made loud.
std::vector<float> harmonicCarrier(double f0, int loud, size_t len) {
	std::vector<float> x(len, 0.0f);
	for (int k = 1; k <= 40; ++k) {
		const double a = (k == loud ? 1.0 : 0.1) / k;
		for (size_t t = 0; t < len; ++t) {
			x[t] += static_cast<float>(a * std::sin(2.0 * std::numbers::pi * k * f0 * static_cast<double>(t) / 48000.0));
		}
	}
	return x;
}

} // namespace

TEST_CASE("Z3: at full Harmonic the bands sit on the carrier's harmonics — one each at the bottom, groups above centred on the loudest — none below f0, none twice") {
	const double f0 = 110.0;
	const auto side = noise(48000, 9u);
	// HARMONIC 16 LOUD: the group that holds it (13-16 at 24 bands over 20 Hz-20 kHz) is centred on it, not on its middle.
	const auto carrier = harmonicCarrier(f0, 16, 48000);
	auto s = harmonicStft(1.0, f0, 24, carrier, side, 24000);
	const int count = s->bandCountForTest();
	REQUIRE(count == 24);
	int singles = 0;
	std::vector<int> seen;
	for (int b = 0; b < count; ++b) {
		CAPTURE(b);
		const double centreHz = std::exp2(s->bandCentreForTest(b));
		const auto [first, last] = s->groupForTest(b);
		CHECK(centreHz >= f0 * 0.999); // none below f0
		CHECK(first <= last);
		// THE CENTRE IS ONE OF ITS GROUP'S HARMONICS, exactly.
		const double k = centreHz / f0;
		CHECK(std::fabs(k - std::round(k)) < 1e-6);
		CHECK(std::round(k) >= first);
		CHECK(std::round(k) <= last);
		if (first == last) {
			++singles;
		}
		if (first <= 16 && 16 <= last) {
			CHECK(last > first); // a real group, so the pick had a choice
			CHECK(std::round(k) == 16.0); // centred on its loudest member
		}
		for (int h = first; h <= last; ++h) {
			CHECK(std::find(seen.begin(), seen.end(), h) == seen.end()); // none twice
			seen.push_back(h);
		}
	}
	CHECK(singles >= 6); // the bottom harmonics get a band each
	CHECK(s->groupForTest(0).first == 1); // starting at f0 itself
	// AND THE WEIGHTS STILL PARTITION UNITY, however unevenly the harmonics space the bands.
	double worstSum = 0.0;
	for (int i = 0; i < s->binCount(); ++i) {
		worstSum = std::max(worstSum, std::fabs(static_cast<double>(s->bandWeightSumForTest(i)) - 1.0));
	}
	CHECK(worstSum < 1e-6);
}

TEST_CASE("Z3: at full Inharmonic every band sits in a gap between harmonics; and the grouping re-lays on a new note, through a glide, and not over a held one") {
	const auto side = noise(48000, 9u);
	const auto carrier = harmonicCarrier(110.0, 0, 48000);
	auto s = harmonicStft(-1.0, 110.0, 24, carrier, side, 24000);
	for (int b = 0; b < s->bandCountForTest(); ++b) {
		CAPTURE(b);
		const double k = std::exp2(s->bandCentreForTest(b)) / 110.0;
		CHECK(std::fabs((k - std::floor(k)) - 0.5) < 1e-6); // halfway between two harmonics
	}
	// HELD: two more seconds at the same f0 lay nothing new.
	const uint64_t held = s->groupingsForTest();
	s = harmonicStft(-1.0, 110.0, 24, carrier, side, 96000, std::move(s));
	CHECK(s->groupingsForTest() == held);
	// A NEW NOTE re-lays once.
	s = harmonicStft(-1.0, 165.0, 24, carrier, side, 4800, std::move(s));
	CHECK(s->groupingsForTest() == held + 1);
	// A GLIDE (a semitone over 20 steps, 5 cents each) re-lays at every step past a cent.
	uint64_t before = s->groupingsForTest();
	for (int step = 1; step <= 20; ++step) {
		s = harmonicStft(-1.0, 165.0 * std::exp2(step * 0.05 / 12.0), 24, carrier, side, 2048, std::move(s));
	}
	CHECK(s->groupingsForTest() == before + 20);
}

TEST_CASE("Z3: the device's Harmonic Zone follows the operators' f0 — C3 plus Offset untracked, the played note with Key Track — at every amount from Even") {
	// THE FIRST BAND AT FULL HARMONIC IS f0 ITSELF, by value; at amount 0 the layout is Even, exactly as Z2's.
	for (const bool tracked : { false, true }) {
		CAPTURE(tracked);
		Settings st = vocodeBasics();
		st.vocodeBandwidth = 0.5f;
		st.vocodeZone = 4;
		st.vocodeZoneAmount = 1.0f;
		st.keyTrack = tracked;
		st.keyOffset = tracked ? 0.0f : 5.0f;
		SpectralDevice d;
		d.prepare(48000.0);
		d.setTarget(st);
		if (tracked) {
			feathers::device::NoteContext note {};
			note.pitchSemitones = 45.0f; // A1, 110 Hz
			d.setNoteContext(note);
		}
		std::vector<float> l = noise(9600), r = l, side = noise(9600, 5u);
		for (size_t at = 0; at < l.size(); at += 480) {
			d.setSidechain(side.data() + at, side.data() + at, true);
			d.processStereo(l.data() + at, r.data() + at, 480);
		}
		const double f0 = tracked ? 110.0 : kFixedF0Hz * std::exp2(5.0 / 12.0);
		CHECK(std::exp2(d.stftForTest().bandCentreForTest(0)) == doctest::Approx(f0).epsilon(1e-6));
		CHECK(d.stftForTest().groupForTest(0).first == 1);
	}
	// AND AT AMOUNT 0 IT IS EVEN.
	Settings st = vocodeBasics();
	st.vocodeBandwidth = 0.5f;
	st.vocodeZone = 5;
	st.vocodeZoneAmount = 0.0f;
	auto d = laidOut(st);
	CHECK(d->stftForTest().bandCentreForTest(0) == doctest::Approx(std::log2(20.0) + std::log2(1000.0) * 0.5 / 24.0).epsilon(1e-6));
}

namespace {
/// THE FREE AXES, RESTATED: unipolar phi triangles (0 -> 1 -> 0 over one cycle) at 0.5 phi^1..4; the last bipolar, its cycle's
/// first half positive, its second negative.
std::array<double, 4> restatedFreeAxes(double x) {
	const auto tri = [](double ph) {
		const double w = ph - std::floor(ph);
		return w < 0.5 ? 2.0 * w : 2.0 - 2.0 * w;
	};
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	std::array<double, 4> a {};
	for (int i = 0; i < 3; ++i) {
		a[static_cast<size_t>(i)] = tri(0.5 * std::pow(phi, i + 1) * x);
	}
	const double ph = 0.5 * std::pow(phi, 4) * x;
	const double w = ph - std::floor(ph);
	a[3] = w < 0.5 ? tri(2.0 * w) : -tri(2.0 * w - 1.0);
	return a;
}
} // namespace

TEST_CASE("Z4: Free's axes are phi triangles of the coordinate — 0 is Even, each named axis peaks somewhere in the sweep, and none moves in step") {
	for (double x = 0.0; x <= 1.0; x += 0.0125) {
		const auto got = Stft::freeAxesForTest(x);
		const auto want = restatedFreeAxes(x);
		for (size_t i = 0; i < 4; ++i) {
			CAPTURE(x);
			CAPTURE(i);
			CHECK(got[i] == doctest::Approx(want[i]).epsilon(1e-5));
		}
	}
	const auto zero = Stft::freeAxesForTest(0.0);
	for (const double v : zero) {
		CHECK(v == 0.0);
	}
	// EVERY AXIS REACHED: each unipolar one peaks near 1 in [0, 1]; Low (the second) also comes back near 0 there; Harmonic
	// reaches both ends.
	std::array<double, 4> top { -2.0, -2.0, -2.0, -2.0 };
	double bottomHarmonic = 2.0;
	double lowAfterPeak = 2.0;
	bool lowPeaked = false;
	for (double x = 0.0; x <= 1.0; x += 0.0005) {
		const auto a = Stft::freeAxesForTest(x);
		for (size_t i = 0; i < 4; ++i) {
			top[i] = std::max(top[i], a[i]);
		}
		bottomHarmonic = std::min(bottomHarmonic, a[3]);
		lowPeaked = lowPeaked || a[1] > 0.99;
		if (lowPeaked) {
			lowAfterPeak = std::min(lowAfterPeak, a[1]);
		}
	}
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(top[i] > 0.99);
	}
	CHECK(bottomHarmonic < -0.99);
	CHECK(lowAfterPeak < 0.01);
}

TEST_CASE("Z4: the Free layout is the convex mix of the named modes' full layouts by the axes — by value where Harmonic's axis is 0 — and moves without jumps") {
	// WHERE THE HARMONIC AXIS IS EXACTLY 0 (x = k / (2 r4)), the layout is Even plus the Speech, Low and High offsets weighted by
	// their axes, divided by the axes' sum once it passes 1: restated here and compared centre by centre.
	const double r4 = 0.5 * std::pow((1.0 + std::sqrt(5.0)) / 2.0, 4);
	const auto bark = [](double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; };
	const auto fromBark = [](double z) { return 1960.0 * (z + 0.53) / (26.28 - z); };
	for (int k = 1; k <= 6; ++k) {
		const double x = k / (2.0 * r4);
		CAPTURE(x);
		Settings st = vocodeBasics();
		st.vocodeBandwidth = 0.5f;
		st.vocodeZone = 6;
		st.vocodeZoneFree = static_cast<float>(x);
		auto d = laidOut(st);
		const double xs = static_cast<double>(st.vocodeZoneFree); // the coordinate as the float stores it
		const auto a = restatedFreeAxes(xs);
		REQUIRE(std::fabs(a[3]) < 1e-5);
		const double lowLog = std::log2(20.0);
		const double span = std::log2(1000.0);
		double worst = 0.0;
		for (int b = 0; b < 24; ++b) {
			const double p = (b + 0.5) / 24.0;
			const double even = lowLog + span * p;
			const double speech = std::log2(fromBark(bark(20.0) + (bark(20000.0) - bark(20.0)) * p));
			const double lowFull = lowLog + span * std::pow(p, 4.0);
			const double highFull = lowLog + span * (1.0 - std::pow(1.0 - p, 4.0));
			const double off = a[0] * (speech - even) + a[1] * (lowFull - even) + a[2] * (highFull - even);
			const double want = even + off / std::max(1.0, a[0] + a[1] + a[2] + std::fabs(a[3]));
			worst = std::max(worst, std::fabs(d->stftForTest().bandCentreForTest(b) - want));
		}
		CHECK(worst < 1e-4); // octaves; the Range is stored as float log2 Hz
	}
	// NO JUMPS: the largest centre move per step halves (within 60%) when the step halves — a continuous layout, where a jump
	// would stay the same size however fine the sweep.
	const auto largestMove = [&](double stepSize) {
		double jump = 0.0;
		std::array<double, 24> last {};
		const int steps = static_cast<int>(std::lround(1.0 / stepSize));
		for (int step = 0; step <= steps; ++step) {
			Settings st = vocodeBasics();
			st.vocodeBandwidth = 0.5f;
			st.vocodeZone = 6;
			st.vocodeZoneFree = static_cast<float>(step * stepSize);
			auto d = laidOut(st);
			for (int b = 0; b < 24; ++b) {
				const double c = d->stftForTest().bandCentreForTest(b);
				if (step > 0) {
					jump = std::max(jump, std::fabs(c - last[static_cast<size_t>(b)]));
				}
				last[static_cast<size_t>(b)] = c;
			}
		}
		return jump;
	};
	const double coarse = largestMove(0.01);
	const double fine = largestMove(0.005);
	MESSAGE("Free sweep: largest centre move per step, 0.01: " << coarse << " octaves, 0.005: " << fine);
	CHECK(fine < 0.6 * coarse);
}

namespace {
/// THE BANDWIDTH ZONE'S FREE AXES, RESTATED: two bipolar triangles at 0.5 phi^1.5 and 0.5 phi^2.5.
std::array<double, 2> restatedFreeAxesW(double x) {
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	const auto bipolar = [](double ph) {
		const double w = ph - std::floor(ph);
		const auto tri = [](double q) {
			const double v = q - std::floor(q);
			return v < 0.5 ? 2.0 * v : 2.0 - 2.0 * v;
		};
		return w < 0.5 ? tri(2.0 * w) : -tri(2.0 * w - 1.0);
	};
	return { bipolar(0.5 * std::pow(phi, 1.5) * x), bipolar(0.5 * std::pow(phi, 2.5) * x) };
}
} // namespace

namespace {
/// THE WIDTH FACTOR, RESTATED (§10.7.2, Z5): 2^(1.5 e), e from the band's place p (tilt 2p - 1) and its distance from f0's harmonics
/// (between = 4 d - 1, d the distance to the nearest harmonic number).
double restatedWidthFactor(int zone, double a, double p, double hz, double f0, std::array<double, 2> free = {}) {
	const double tilt = 2.0 * p - 1.0;
	const double k = hz / f0;
	const double between = 4.0 * std::fabs(k - std::round(k)) - 1.0;
	double e = 0.0;
	switch (zone) {
	case 1: e = a * tilt; break;
	case 2: e = -a * tilt; break;
	case 3: e = a * between; break;
	case 4: e = -a * between; break;
	case 5: e = free[0] * tilt + free[1] * between; break;
	default: break;
	}
	return std::exp2(1.5 * e);
}
} // namespace

TEST_CASE("Z5: each band's width is Bandwidth's share of its local spacing times the Bandwidth zone's factor, by value — every zone, and Free's own axes") {
	const double f0 = kFixedF0Hz;
	for (const int wz : { 1, 2, 3, 4, 5 }) {
		for (const float amount : { 0.0f, 0.4f, 1.0f }) {
			CAPTURE(wz);
			CAPTURE(amount);
			Settings st = vocodeBasics();
			st.vocodeBandwidth = 0.5f;
			st.vocodeWidthZone = wz;
			st.vocodeWidthAmount = amount; // a named mode reads it
			st.vocodeWidthFree = amount;   // Free reads its own coordinate (Z7)
			auto d = laidOut(st);
			const int count = d->stftForTest().bandCountForTest();
			const auto axes = restatedFreeAxesW(static_cast<double>(st.vocodeWidthFree));
			double worst = 0.0;
			for (int b = 0; b < count; ++b) {
				const double c = d->stftForTest().bandCentreForTest(b);
				const double left = b > 0 ? c - d->stftForTest().bandCentreForTest(b - 1) : 2.0 * (c - std::log2(20.0));
				const double right = b + 1 < count ? d->stftForTest().bandCentreForTest(b + 1) - c : 2.0 * (std::log2(20000.0) - c);
				const double want = (0.15 + 0.85 * 0.5) * 0.5 * (left + right)
				                    * restatedWidthFactor(wz, amount, (b + 0.5) / count, std::exp2(c), f0, axes);
				worst = std::max(worst, std::fabs(d->stftForTest().bandSigmaForTest(b) / want - 1.0));
			}
			CHECK(worst < 1e-4);
		}
	}
	// NARROW LOW AT FULL: the bottom band is 2^-1.5 x its even width, the top 2^+1.5 x, very nearly.
	Settings st = vocodeBasics();
	st.vocodeBandwidth = 0.5f;
	st.vocodeWidthZone = 1;
	st.vocodeWidthAmount = 1.0f;
	auto d = laidOut(st);
	const double even = (0.15 + 0.85 * 0.5) * std::log2(1000.0) / 24.0;
	CHECK(d->stftForTest().bandSigmaForTest(12) / even == doctest::Approx(std::exp2(1.5 * (2.0 * 12.5 / 24.0 - 1.0))).epsilon(1e-3));
	CHECK(d->stftForTest().bandSigmaForTest(0) < d->stftForTest().bandSigmaForTest(23));
}

TEST_CASE("Z7: each mode reads only its own value — a named mode the fenced amount, Free its own coordinate, Even neither") {
	const auto centres = [](const Settings& st) {
		auto d = laidOut(st);
		std::array<double, 24> c {};
		for (int b = 0; b < 24; ++b) {
			c[static_cast<size_t>(b)] = d->stftForTest().bandCentreForTest(b);
		}
		return c;
	};
	const auto sigmas = [](const Settings& st) {
		auto d = laidOut(st);
		std::array<double, 24> s {};
		for (int b = 0; b < 24; ++b) {
			s[static_cast<size_t>(b)] = d->stftForTest().bandSigmaForTest(b);
		}
		return s;
	};
	Settings st = vocodeBasics();
	st.vocodeBandwidth = 0.5f;
	// A NAMED ZONE IGNORES FREE'S COORDINATE.
	st.vocodeZone = 1; // Speech
	st.vocodeZoneAmount = 0.6f;
	st.vocodeZoneFree = 0.0f;
	const auto speech = centres(st);
	st.vocodeZoneFree = 3.3f;
	CHECK(centres(st) == speech);
	// FREE IGNORES THE AMOUNT.
	st.vocodeZone = 6;
	st.vocodeZoneFree = 2.7f;
	st.vocodeZoneAmount = 0.1f;
	const auto free = centres(st);
	st.vocodeZoneAmount = 0.9f;
	CHECK(centres(st) == free);
	CHECK(free != speech); // and the coordinate is read: not vacuous
	// THE SAME FOR THE WIDTHS.
	st.vocodeZone = 0;
	st.vocodeWidthZone = 1; // Narrow Low
	st.vocodeWidthAmount = 0.7f;
	st.vocodeWidthFree = 0.0f;
	const auto narrow = sigmas(st);
	st.vocodeWidthFree = 4.1f;
	CHECK(sigmas(st) == narrow);
	st.vocodeWidthZone = 5;
	st.vocodeWidthAmount = 0.2f;
	const auto wfree = sigmas(st);
	st.vocodeWidthAmount = 0.8f;
	CHECK(sigmas(st) == wfree);
	// EVEN READS NEITHER, AND FREE AT 0 IS EVEN: at Bandwidth 100% neither takes the banded road, whatever the other values.
	Settings even = vocodeBasics();
	even.vocodeZoneAmount = 1.0f;
	even.vocodeZoneFree = 5.0f;
	even.vocodeWidthAmount = 1.0f;
	even.vocodeWidthFree = 5.0f;
	CHECK_FALSE(laidOut(even)->stftForTest().banded());
	even.vocodeZone = 6;
	even.vocodeWidthZone = 5;
	even.vocodeZoneFree = 0.0f;
	even.vocodeWidthFree = 0.0f;
	CHECK_FALSE(laidOut(even)->stftForTest().banded());
	even.vocodeZoneFree = 0.25f;
	CHECK(laidOut(even)->stftForTest().banded());
}

TEST_CASE("Z7: Free's coordinates are unbounded — past 1 and below 0 the layout and the widths are the restated mix, by value") {
	// THE CENTRES, where Harmonic's axis is exactly 0 (x = k / (2 r4)), as Z4's case restates them — now for coordinates far
	// outside Z4's 0..1, either side of 0.
	const double r4 = 0.5 * std::pow((1.0 + std::sqrt(5.0)) / 2.0, 4);
	const auto bark = [](double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; };
	const auto fromBark = [](double z) { return 1960.0 * (z + 0.53) / (26.28 - z); };
	for (const int k : { 9, 17, 40, -5, -23 }) {
		Settings st = vocodeBasics();
		st.vocodeBandwidth = 0.5f;
		st.vocodeZone = 6;
		st.vocodeZoneFree = static_cast<float>(k / (2.0 * r4));
		CAPTURE(st.vocodeZoneFree);
		auto d = laidOut(st);
		const auto a = restatedFreeAxes(static_cast<double>(st.vocodeZoneFree));
		REQUIRE(std::fabs(a[3]) < 1e-4);
		REQUIRE(a[0] + a[1] + a[2] > 0.05); // somewhere other than Even
		const double lowLog = std::log2(20.0);
		const double span = std::log2(1000.0);
		double worst = 0.0;
		for (int b = 0; b < 24; ++b) {
			const double p = (b + 0.5) / 24.0;
			const double even = lowLog + span * p;
			const double speech = std::log2(fromBark(bark(20.0) + (bark(20000.0) - bark(20.0)) * p));
			const double lowFull = lowLog + span * std::pow(p, 4.0);
			const double highFull = lowLog + span * (1.0 - std::pow(1.0 - p, 4.0));
			const double off = a[0] * (speech - even) + a[1] * (lowFull - even) + a[2] * (highFull - even);
			const double want = even + off / std::max(1.0, a[0] + a[1] + a[2] + std::fabs(a[3]));
			worst = std::max(worst, std::fabs(d->stftForTest().bandCentreForTest(b) - want));
		}
		CHECK(worst < 1e-3); // octaves: the stored float coordinate, wrapped in float by the phi layer
	}
	// THE WIDTHS, by Z5's restated factor at Free's own coordinate.
	for (const float x : { 3.37f, 11.9f, -2.71f }) {
		CAPTURE(x);
		Settings st = vocodeBasics();
		st.vocodeBandwidth = 0.5f;
		st.vocodeWidthZone = 5;
		st.vocodeWidthFree = x;
		auto d = laidOut(st);
		const auto axes = restatedFreeAxesW(static_cast<double>(x));
		REQUIRE(std::fabs(axes[0]) + std::fabs(axes[1]) > 0.05);
		const int count = d->stftForTest().bandCountForTest();
		double worst = 0.0;
		for (int b = 0; b < count; ++b) {
			const double c = d->stftForTest().bandCentreForTest(b);
			const double left = b > 0 ? c - d->stftForTest().bandCentreForTest(b - 1) : 2.0 * (c - std::log2(20.0));
			const double right = b + 1 < count ? d->stftForTest().bandCentreForTest(b + 1) - c : 2.0 * (std::log2(20000.0) - c);
			const double want = (0.15 + 0.85 * 0.5) * 0.5 * (left + right)
			                    * restatedWidthFactor(5, 0.0, (b + 0.5) / count, std::exp2(c), kFixedF0Hz, axes);
			worst = std::max(worst, std::fabs(d->stftForTest().bandSigmaForTest(b) / want - 1.0));
		}
		CHECK(worst < 1e-3);
	}
}

TEST_CASE("Z7: the descriptors — Zone and Widths are mode cells; each named mode gates the fenced amount, Free the unbounded coordinate") {
	const auto& zone = kParamDescs[static_cast<size_t>(Param::VocodeZone)];
	const auto& widths = kParamDescs[static_cast<size_t>(Param::VocodeWidthZone)];
	CHECK(zone.modeCell);
	CHECK(widths.modeCell);
	const auto& amount = kParamDescs[static_cast<size_t>(Param::VocodeZoneAmount)];
	const auto& free = kParamDescs[static_cast<size_t>(Param::VocodeZoneFree)];
	const auto& wAmount = kParamDescs[static_cast<size_t>(Param::VocodeWidthAmount)];
	const auto& wFree = kParamDescs[static_cast<size_t>(Param::VocodeWidthFree)];
	CHECK(amount.min == 0.0);
	CHECK(amount.max == 1.0);
	CHECK(wAmount.max == 1.0);
	CHECK(feathers::device::isDragWindow(free));
	CHECK(feathers::device::isDragWindow(wFree));
	CHECK(free.min < -1.0e6);
	for (int m = 0; m < static_cast<int>(std::size(kZoneNames)); ++m) {
		CAPTURE(m);
		const bool named = m != 0 && m != 6;
		CHECK(feathers::device::relevantFor(amount, m) == named);
		CHECK(feathers::device::relevantFor(free, m) == (m == 6));
	}
	for (int m = 0; m < static_cast<int>(std::size(kWidthZoneNames)); ++m) {
		CAPTURE(m);
		const bool named = m != 0 && m != 5;
		CHECK(feathers::device::relevantFor(wAmount, m) == named);
		CHECK(feathers::device::relevantFor(wFree, m) == (m == 5));
	}
	// THE CODEC CARRIES THE COORDINATE PAST 0..1 AND BACK, unclamped.
	Settings s;
	s.vocodeZoneFree = -37.25f;
	s.vocodeWidthFree = 1234.5f;
	const Row row = rowOf(s);
	const Settings back = stateOf(row, row.size());
	CHECK(back.vocodeZoneFree == -37.25f);
	CHECK(back.vocodeWidthFree == 1234.5f);
}

TEST_CASE("Z5: uneven widths keep the partition of unity and a flat sidechain's level, at every Bandwidth zone") {
	constexpr size_t len = 3 * 48000;
	const auto side = noise(len, 0x4242u);
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	Settings smoothSt = vocodeBasics();
	const auto smooth = octaveBandsDb(vocodedStereo(smoothSt, side, side).first, 48000.0);
	for (const int wz : { 1, 2, 3, 4, 5 }) {
		CAPTURE(wz);
		Settings st = vocodeBasics(); // Bandwidth 100%: the width zone alone takes the banded road
		st.vocodeWidthZone = wz;
		st.vocodeWidthAmount = 1.0f;
		st.vocodeWidthFree = 1.0f;
		auto d = laidOut(st);
		CHECK(d->stftForTest().banded());
		double worstSum = 0.0;
		for (int i = 0; i <= n / 2; ++i) {
			worstSum = std::max(worstSum, std::fabs(static_cast<double>(d->stftForTest().bandWeightSumForTest(i)) - 1.0));
		}
		CHECK(worstSum < 1e-6);
		const auto banded = octaveBandsDb(vocodedStereo(st, side, side).first, 48000.0);
		double worst = 0.0;
		for (size_t b = 0; b < banded.size(); ++b) {
			worst = std::max(worst, std::fabs(banded[b] - smooth[b]));
		}
		MESSAGE("width zone " << wz << ": flat sidechain, worst octave difference " << worst << " dB");
		CHECK(worst < 1.0); // the same bound and the same reason as Z2's
	}
}

TEST_CASE("Z7c: Low Pass at the top and High Pass at the bottom are exactly open — by the control or by the corner at the plot's edge") {
	// MEASURED BEFORE (binGains, 48 kHz, n 8192): Low Pass with the cutoff at 20 kHz was -3.0 dB at 20 kHz at every slope, and at
	// 6 dB/oct -1.0 dB at 10 kHz and -1.9 at 15; with Offset -12 the corner sat at 10 kHz (-3.0 dB there). High Pass at 20 Hz was
	// -3.6 dB at the 17.6 Hz bin. So neither end was open.
	constexpr int n = 8192;
	std::vector<float> k(n / 2 + 1), hz(n / 2 + 1), base(n / 2 + 1), out(n / 2 + 1);
	const auto worstDb = [&](const Settings& s) {
		(void)binGains(s, false, 0.0, 48000.0, n, BinScratch { k.data(), hz.data(), base.data() }, out.data());
		double worst = 0.0;
		for (const float g : out) {
			worst = std::max(worst, std::fabs(20.0 * std::log10(static_cast<double>(g))));
		}
		return worst;
	};
	for (const float slope : { 0.0f, 0.25f, 1.0f }) {
		for (const float res : { 0.0f, 1.0f }) {
			for (const float offset : { 0.0f, -12.0f, 12.0f }) {
				CAPTURE(slope);
				CAPTURE(res);
				CAPTURE(offset);
				Settings s;
				s.filterOn = true;
				s.filterSlope = slope;
				s.filterResonance = res;
				s.keyOffset = offset;
				s.filterMode = 0;
				s.filterCutoff = static_cast<float>(kLog2Of20k);
				CHECK(worstDb(s) == 0.0); // exactly 1 in every bin
				s.filterMode = 1;
				s.filterCutoff = static_cast<float>(kLog2Of20);
				CHECK(worstDb(s) == 0.0);
				s.filterMode = 3; // Notch, both ends (Z9b)
				CHECK(worstDb(s) == 0.0);
				s.filterCutoff = static_cast<float>(kLog2Of20k);
				CHECK(worstDb(s) == 0.0);
			}
		}
	}
	// THE OTHER ROAD: Offset +12 puts the corner an octave above the control, so a handle dragged to the plot's right edge (the
	// corner at 20 kHz) leaves the control at 10 kHz — and that is open too.
	Settings s;
	s.filterOn = true;
	s.keyOffset = 12.0f;
	s.filterCutoff = static_cast<float>(kLog2Of20k - 1.0);
	CHECK(worstDb(s) == 0.0);
	// THE FADE (the shared shaper's, `filterOpenness`): the full filter until the last third of an octave, half way through it half
	// the filter in dB, then off.
	namespace sh = feathers::spectral;
	const double f0 = shaperF0(Settings {}, false, 0.0);
	const auto prepared = [&](const Settings& st) { return sh::prepare(shaperParamsOf(st, f0), f0); };
	Settings f;
	f.filterOn = true;
	f.filterCutoff = static_cast<float>(kLog2Of20k - sh::kFilterOpenOctaves - 0.01);
	CHECK(prepared(f).filter.on);
	CHECK(prepared(f).filter.mix == 1.0);
	f.filterCutoff = static_cast<float>(kLog2Of20k - 0.5 * sh::kFilterOpenOctaves);
	CHECK(prepared(f).filter.mix == doctest::Approx(0.5).epsilon(1e-5));
	f.filterCutoff = static_cast<float>(kLog2Of20k);
	CHECK_FALSE(prepared(f).filter.on);
	CHECK(prepared(f).identity);
	// BAND PASS HAS NO OPEN END (it passes the band at the edge): untouched at the top. NOTCH OPENS AT BOTH (Z9b).
	f.filterMode = 2;
	CHECK(prepared(f).filter.on);
	CHECK(prepared(f).filter.mix == 1.0);
	f.filterMode = 3;
	CHECK(prepared(f).identity);
	f.filterCutoff = static_cast<float>(kLog2Of20);
	CHECK(prepared(f).identity);
	f.filterCutoff = static_cast<float>(kLog2Of2k);
	CHECK(prepared(f).filter.on);
	CHECK(prepared(f).filter.mix == 1.0);
}

TEST_CASE("Z9: the Square carrier follows the note only — Offset moves the operators' f0, never the Square's pitch") {
	// THE PITCH BY VALUE (the seam), and AS HEARD: the generated square's rising edges counted over 4800 samples (0.1 s).
	const auto run = [](float offset, bool tracked, float pitch) {
		Settings st = vocodeBasics();
		st.vocodeCarrier = 1; // Square
		st.keyOffset = offset;
		st.keyTrack = tracked;
		auto d = std::make_unique<SpectralDevice>();
		d->prepare(48000.0);
		d->setTarget(st);
		feathers::device::NoteContext note;
		note.pitchSemitones = pitch;
		note.gate = true;
		d->setNoteContext(note);
		std::vector<float> l = noise(9600), r = l, side = noise(9600, 5u);
		for (size_t at = 0; at < l.size(); at += 480) {
			d->setSidechain(side.data() + at, side.data() + at, true);
			d->processStereo(l.data() + at, r.data() + at, 480);
		}
		int rising = 0;
		for (int ago = 4799; ago > 0; --ago) {
			rising += d->stftForTest().carrierAgoForTest(ago) < 0.0f && d->stftForTest().carrierAgoForTest(ago - 1) >= 0.0f ? 1 : 0;
		}
		return std::pair { d->stftForTest().carrierHzForTest(), rising };
	};
	for (const bool tracked : { false, true }) {
		CAPTURE(tracked);
		const double want = tracked ? 440.0 : kFixedF0Hz; // A4 played, or C3
		const auto [hz0, edges0] = run(0.0f, tracked, 69.0f);
		const auto [hz12, edges12] = run(12.0f, tracked, 69.0f);
		const auto [hzDown, edgesDown] = run(-7.0f, tracked, 69.0f);
		CHECK(hz0 == doctest::Approx(want).epsilon(1e-9));
		CHECK(hz12 == doctest::Approx(want).epsilon(1e-9));
		CHECK(hzDown == doctest::Approx(want).epsilon(1e-9));
		CHECK(std::abs(edges0 - static_cast<int>(std::lround(want * 0.1))) <= 1);
		CHECK(edges12 == edges0);
		CHECK(edgesDown == edges0);
	}
	// AND OFFSET STILL MOVES THE OPERATORS' f0: Offset +12 doubles shaperF0, which the layouts and Partials read.
	Settings st;
	st.keyOffset = 12.0f;
	CHECK(shaperF0(st, false, 0.0) == doctest::Approx(2.0 * kFixedF0Hz));
}

namespace {
/// A SIDECHAIN WITH ONE FORMANT: harmonics of 110 Hz under a floor of -26 dB and a peak at `peakHz` a third of an octave wide.
std::vector<float> formantSidechain(size_t frames, double peakHz) {
	std::vector<float> s(frames, 0.0f);
	for (int k = 1; 110.0 * k < 16000.0; ++k) {
		const double hz = 110.0 * k;
		const double oct = std::log2(hz / peakHz);
		const double a = 0.05 + std::exp(-0.5 * (oct / 0.15) * (oct / 0.15));
		for (size_t n = 0; n < frames; ++n) {
			s[n] += static_cast<float>(0.02 * a * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0 + 0.7 * k * k));
		}
	}
	return s;
}

/// Where the imposed (frozen) envelope peaks between 300 Hz and 8 kHz, Hz (a parabola through the top bin and its neighbours,
/// in dB, for a sub-bin answer).
double envelopePeakHz(const SpectralDevice& d) {
	const int n = fftSizeFor(Resolution::Fine, 48000.0);
	const double binHz = 48000.0 / n;
	int best = 0;
	float top = -1.0f;
	for (int i = static_cast<int>(300.0 / binHz); i < static_cast<int>(8000.0 / binHz); ++i) {
		const float v = d.stftForTest().sidechainBinEnvelopeForTest(i);
		if (v > top) {
			top = v;
			best = i;
		}
	}
	const auto db = [&](int i) { return 20.0 * std::log10(std::max(d.stftForTest().sidechainBinEnvelopeForTest(i), 1e-12f)); };
	const double a = db(best - 1), b = db(best), c = db(best + 1);
	const double shift = 0.5 * (a - c) / (a - 2.0 * b + c);
	return (static_cast<double>(best) + (std::isfinite(shift) ? shift : 0.0)) * binHz;
}

/// THE FREEZE SCENE: a formant at 1 kHz caught with Freeze at note 60, then the note moved to `noteAfter` (and the rest of
/// `after` applied) with a SILENT sidechain, so whatever the envelope does then is the frozen one's. Returns the peak before
/// and after, Hz.
std::pair<double, double> freezeScene(Settings st, float noteAfter, const std::function<void(Settings&)>& after) {
	SpectralDevice d;
	d.prepare(48000.0);
	st.vocodeFreeze = false;
	d.setTarget(st);
	feathers::device::NoteContext note;
	note.pitchSemitones = 60.0f;
	note.gate = true;
	d.setNoteContext(note);
	const std::vector<float> side = formantSidechain(24000, 1000.0);
	const std::vector<float> silent(4800, 0.0f);
	std::vector<float> l(480, 0.0f), r(480, 0.0f);
	for (size_t at = 0; at + 480 <= side.size(); at += 480) {
		d.setSidechain(side.data() + at, side.data() + at, true);
		d.processStereo(l.data(), r.data(), 480);
	}
	st.vocodeFreeze = true;
	d.setTarget(st);
	for (int i = 0; i < 10; ++i) {
		d.setSidechain(side.data(), side.data(), true);
		d.processStereo(l.data(), r.data(), 480);
	}
	const double before = envelopePeakHz(d);
	note.pitchSemitones = noteAfter;
	d.setNoteContext(note);
	after(st);
	d.setTarget(st);
	for (size_t at = 0; at + 480 <= silent.size(); at += 480) {
		d.setSidechain(silent.data() + at, silent.data() + at, true);
		d.processStereo(l.data(), r.data(), 480);
	}
	return { before, envelopePeakHz(d) };
}
} // namespace

TEST_CASE("Z10: Freeze respects Key Track — with it on, the frozen envelope moves with the note (an octave up, its formant an octave up); off, it stays; Offset never moves it") {
	Settings st = vocodeBasics();
	st.vocodeBands = 64.0f;
	const auto none = [](Settings&) {};
	// KEY TRACK ON: +12 semitones moves the 1 kHz formant to 2 kHz, by value.
	st.keyTrack = true;
	const auto [before, up] = freezeScene(st, 72.0f, none);
	MESSAGE("Key Track on: formant " << before << " Hz caught, " << up << " Hz after +12");
	CHECK(before == doctest::Approx(1000.0).epsilon(0.05));
	CHECK(up / before == doctest::Approx(2.0).epsilon(0.02));
	// AND DOWN A FIFTH: 2^(-7/12).
	const auto [b2, down] = freezeScene(st, 53.0f, none);
	CHECK(down / b2 == doctest::Approx(std::exp2(-7.0 / 12.0)).epsilon(0.02));
	// OFFSET NEVER MOVES IT (it moves the operators only): the note unchanged, Offset +12.
	const auto [b3, offset] = freezeScene(st, 60.0f, [](Settings& s) { s.keyOffset = 12.0f; });
	CHECK(offset / b3 == doctest::Approx(1.0).epsilon(0.01));
	// KEY TRACK OFF: fixed in Hz, whatever the note.
	st.keyTrack = false;
	const auto [b4, fixed] = freezeScene(st, 72.0f, none);
	CHECK(fixed / b4 == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("Z10: the frozen envelope's transposition composes with Formant Shift — their semitones add, on one re-read") {
	Settings st = vocodeBasics();
	st.vocodeBands = 64.0f;
	st.keyTrack = true;
	st.fshiftOn = true;
	st.fshiftLink = true;
	st.fshiftLow = 5.0f; // a fourth up, on the whole envelope
	// THE NOTE +7 WHILE FROZEN, Formant Shift +5 throughout: the formant ends 12 semitones above where it was caught, and Formant
	// Shift alone (the note unchanged) moves it its own 5.
	const auto [caught, both] = freezeScene(st, 67.0f, [](Settings&) {});
	MESSAGE("Formant Shift +5, note +7: " << caught << " Hz -> " << both << " Hz");
	CHECK(both / caught == doctest::Approx(std::exp2(7.0 / 12.0)).epsilon(0.02)); // caught already carries the +5
	CHECK(caught == doctest::Approx(1000.0 * std::exp2(5.0 / 12.0)).epsilon(0.05));
}

TEST_CASE("Z10: a note change while frozen re-reads the envelope smoothly — no step in the output") {
	// A SQUARE CARRIER UNDER A FROZEN FORMANT, Key Track on; the note jumps +12 between two blocks. The largest sample-to-sample
	// step across the change is held against the largest in a steady stretch before it (the overlap-add crossfades the frames).
	Settings st = vocodeBasics();
	st.vocodeBands = 64.0f;
	st.keyTrack = true;
	st.vocodeCarrier = 2; // White: a carrier whose own pitch does not follow the note, so only the envelope moves
	SpectralDevice d;
	d.prepare(48000.0);
	d.setTarget(st);
	feathers::device::NoteContext note;
	note.pitchSemitones = 60.0f;
	note.gate = true;
	d.setNoteContext(note);
	const std::vector<float> side = formantSidechain(24000, 1000.0);
	std::vector<float> out;
	std::vector<float> l(480, 0.0f), r(480, 0.0f);
	for (size_t at = 0; at + 480 <= side.size(); at += 480) {
		d.setSidechain(side.data() + at, side.data() + at, true);
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		d.processStereo(l.data(), r.data(), 480);
	}
	st.vocodeFreeze = true;
	d.setTarget(st);
	const auto run = [&](int blocks) {
		for (int b = 0; b < blocks; ++b) {
			d.setSidechain(side.data(), side.data(), true);
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.processStereo(l.data(), r.data(), 480);
			out.insert(out.end(), l.begin(), l.end());
		}
	};
	run(20);
	const size_t change = out.size();
	note.pitchSemitones = 72.0f;
	d.setNoteContext(note);
	run(20);
	// THE WINDOW ACROSS THE CHANGE (the 4096 samples after it: the latency and two frames) against the steady stretches either
	// side: the moved formant brightens the noise, so the steady AFTER is the fair bound for what follows the change.
	const auto largest = [&](size_t from, size_t to) {
		double m = 0.0;
		for (size_t i = std::max<size_t>(from, 1); i < to && i < out.size(); ++i) {
			m = std::max(m, std::fabs(static_cast<double>(out[i]) - static_cast<double>(out[i - 1])));
		}
		return m;
	};
	const double before = largest(0, change);
	const double across = largest(change, change + 4096);
	const double afterwards = largest(change + 4096, out.size());
	MESSAGE("largest step: steady before " << before << ", across the note change " << across << ", steady after " << afterwards);
	CHECK(before > 0.0);
	CHECK(across < 1.25 * std::max(before, afterwards));
}

// ======================================================================================================================
// Z10b: THE FROZEN ENVELOPE PERSISTS WITH THE PATCH (backlog NM's block road; CLAUDE.md "RANDOM AND CAPTURED STATE
// PERSISTS WITH THE PATCH"). Captured, saved, loaded into a FRESH rack, the same envelope by value.
// ======================================================================================================================

#include "persist_rig.h"

namespace {
namespace z10b {

/// `blocks` of the rig's 64 frames, each with the next 64 samples of `side` as the sidechain (wrapping at its end).
void run(feathers::persist::Rig& rig, const std::vector<float>& side, int blocks, size_t& at) {
	for (int b = 0; b < blocks; ++b) {
		if (at + 64 > side.size()) {
			at = 0;
		}
		rig.engine.setSidechain(side.data() + at, side.data() + at, true);
		at += 64;
		rig.block();
	}
}

std::vector<float> quieter(std::vector<float> x, float by) {
	for (float& v : x) {
		v *= by;
	}
	return x;
}

/// The largest difference between two grids read in dBFS (each plus its own stop's offset).
double worstDbfs(const std::array<float, kEnvelopePoints>& a, double aToFs, const std::array<float, kEnvelopePoints>& b, double bToFs) {
	double worst = 0.0;
	for (size_t g = 0; g < a.size(); ++g) {
		worst = std::max(worst, std::fabs((static_cast<double>(a[g]) + aToFs) - (static_cast<double>(b[g]) + bToFs)));
	}
	return worst;
}

/// A rig that has frozen a loud noise's envelope; `held` and `toFs` are what it holds, and `saved` the patch.
struct Frozen {
	std::array<float, kEnvelopePoints> held {};
	double toFs = 0.0;
	std::string saved;
};
Frozen freezeAndSave(Settings st) {
	st.vocodeFreeze = false;
	feathers::persist::Rig run;
	run.seat(kTraits.tag, rowOf(st));
	const std::vector<float> loud = noise(48000, 0x51u);
	size_t at = 0;
	z10b::run(run, loud, 400, at);
	run.bridge->setField(run.id, static_cast<size_t>(Param::VocodeFreeze), 1.0f);
	z10b::run(run, loud, 4, at);
	const SpectralDevice& d = run.as<SpectralDevice>();
	REQUIRE(d.stftForTest().frozen(0));
	Frozen f;
	f.held = d.stftForTest().sidechainEnvelope(0);
	f.toFs = d.stftForTest().envelopeToFs();
	f.saved = run.save();
	return f;
}

/// The saved patch with one control's label replaced (`key` → `label`), or the block taken out (`key` "spectral_freeze").
std::string edited(const std::string& saved, const std::string& key, const std::string& label) {
	feathers::rack::RackPatch patch;
	REQUIRE(feathers::rack::parseRackText(saved, &patch));
	REQUIRE(patch.device.size() == 1u);
	auto& dev = patch.device[0];
	if (key == "spectral_freeze") {
		const size_t before = dev.extra.size();
		std::erase_if(dev.extra, [&](const feathers::rack::RackPatch::Value& v) { return v.key == key; });
		REQUIRE(dev.extra.size() == before - 1);
	}
	else {
		bool found = false;
		for (auto& v : dev.values) {
			if (v.key == key) {
				REQUIRE(v.kind == feathers::rack::RackPatch::Value::Kind::Label);
				v.text = label;
				found = true;
			}
		}
		REQUIRE(found);
	}
	std::string text;
	REQUIRE(feathers::rack::rackToText(patch, &text));
	return text;
}

} // namespace z10b
} // namespace

TEST_CASE("Z10b: a frozen envelope saved with the patch loads into a fresh rack frozen at once — the same envelope, by value") {
	const z10b::Frozen f = z10b::freezeAndSave(vocodeBasics());
	feathers::persist::Rig back;
	back.load(f.saved);
	REQUIRE(back.as<SpectralDevice>().freezePendingForTest()); // held for the first block, past any prepare or reset
	// THE FIRST BLOCK, AND A SIDECHAIN 20 dB QUIETER: a live envelope would show it, the held one does not.
	const std::vector<float> quiet = z10b::quieter(noise(48000, 0x77u), 0.1f);
	size_t at = 0;
	z10b::run(back, quiet, 1, at);
	const SpectralDevice& b = back.as<SpectralDevice>();
	CHECK_FALSE(b.freezePendingForTest());
	CHECK(b.stftForTest().frozen(0));
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), b.stftForTest().envelopeToFs(), f.held, f.toFs) < 1e-3);
	z10b::run(back, quiet, 200, at); // and it HOLDS
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), b.stftForTest().envelopeToFs(), f.held, f.toFs) < 1e-3);

	// THE CONTROL: the same patch without its block freezes what the fresh rack hears — so it was the block that carried it.
	feathers::persist::Rig bare;
	bare.load(z10b::edited(f.saved, "spectral_freeze", ""));
	size_t at2 = 0;
	z10b::run(bare, quiet, 400, at2);
	CHECK(z10b::worstDbfs(bare.as<SpectralDevice>().stftForTest().sidechainEnvelope(0), f.toFs, f.held, f.toFs) > 10.0);
}

TEST_CASE("Z10b: a patch loaded with Freeze OFF ignores the stored shape — the envelope follows the sidechain, and a later "
          "Freeze catches the live one") {
	const z10b::Frozen f = z10b::freezeAndSave(vocodeBasics());
	feathers::persist::Rig back;
	back.load(z10b::edited(f.saved, "spectral_vocode_freeze", "Off"));
	const std::vector<float> quiet = z10b::quieter(noise(48000, 0x77u), 0.1f);
	size_t at = 0;
	z10b::run(back, quiet, 1, at);
	const SpectralDevice& b = back.as<SpectralDevice>();
	CHECK_FALSE(b.freezePendingForTest()); // taken, and dropped
	CHECK_FALSE(b.stftForTest().frozen(0));
	// DROPPED, NOT PRIMED: the envelope after the first block is not the stored one (a primed one would be the start of the
	// live envelope's attack and release, and stay near it for a while).
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), b.stftForTest().envelopeToFs(), f.held, f.toFs) > 10.0);
	z10b::run(back, quiet, 400, at);
	back.bridge->setField(back.id, static_cast<size_t>(Param::VocodeFreeze), 1.0f);
	z10b::run(back, quiet, 4, at);
	REQUIRE(b.stftForTest().frozen(0));
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), b.stftForTest().envelopeToFs(), f.held, f.toFs) > 10.0);
}

TEST_CASE("Z10b: the stored levels are dBFS — a frozen envelope saved at Fine loads at Fast as the same envelope in dBFS") {
	const z10b::Frozen f = z10b::freezeAndSave(vocodeBasics()); // Fine
	feathers::persist::Rig back;
	back.load(z10b::edited(f.saved, "spectral_resolution", "Fast"));
	const std::vector<float> quiet = z10b::quieter(noise(48000, 0x77u), 0.1f);
	size_t at = 0;
	z10b::run(back, quiet, 1, at);
	const SpectralDevice& b = back.as<SpectralDevice>();
	REQUIRE(b.stftForTest().frozen(0));
	const double bToFs = b.stftForTest().envelopeToFs();
	CHECK(std::fabs(bToFs - f.toFs) > 3.0); // NOT VACUOUS: the two stops' raw scales differ
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), bToFs, f.held, f.toFs) < 1e-3);
}

TEST_CASE("Z10b: Key Track's note as Freeze took hold is saved too — the reloaded envelope is transposed by the same distance") {
	Settings st = vocodeBasics();
	st.keyTrack = true;
	SpectralDevice a;
	a.prepare(48000.0);
	a.setTarget(st);
	feathers::device::NoteContext note;
	note.pitchSemitones = 64.0f;
	note.gate = true;
	a.setNoteContext(note);
	const std::vector<float> side = noise(48000, 0x51u);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	const auto play = [&](SpectralDevice& d, const float* sc, int blocks) {
		for (int b = 0; b < blocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.setSidechain(sc, sc, sc != nullptr);
			d.processStereo(l.data(), r.data(), 512);
		}
	};
	play(a, side.data(), 60);
	st.vocodeFreeze = true;
	a.setTarget(st);
	play(a, side.data(), 2);
	note.pitchSemitones = 70.0f;
	a.setNoteContext(note);
	play(a, side.data(), 1);
	REQUIRE(a.stftForTest().freezeShiftForTest() == doctest::Approx(6.0));
	std::vector<float> words(kFreezeWords);
	REQUIRE(a.readFreeze(words.data(), words.size()) != 0u);

	SpectralDevice b;
	b.prepare(48000.0);
	b.setTarget(st);
	b.setNoteContext(note); // the note is 70 now; Freeze took hold at 64
	REQUIRE(b.restoreFreeze(words.data(), words.size()));
	play(b, nullptr, 1);
	CHECK(b.stftForTest().freezeShiftForTest() == doctest::Approx(6.0));
	// THE SAME ENVELOPE, to a float's rounding through dBFS and back.
	CHECK(z10b::worstDbfs(b.stftForTest().sidechainEnvelope(0), 0.0, a.stftForTest().sidechainEnvelope(0), 0.0) < 1e-4);
}

TEST_CASE("Z10b: a damaged or mis-sized frozen block is refused, and the device plays as freshly built") {
	Settings st = vocodeBasics();
	st.vocodeFreeze = true;
	SpectralDevice a;
	a.prepare(48000.0);
	a.setTarget(st);
	const std::vector<float> side = noise(48000, 0x51u);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	for (int b = 0; b < 60; ++b) {
		a.setSidechain(side.data(), side.data(), true);
		a.processStereo(l.data(), r.data(), 512);
	}
	std::vector<float> words(kFreezeWords);
	REQUIRE(a.readFreeze(words.data(), words.size()) != 0u);
	SpectralDevice b;
	b.prepare(48000.0);
	b.setTarget(st);
	CHECK_FALSE(b.restoreFreeze(words.data(), words.size() - 1)); // mis-sized
	std::vector<float> torn = words;
	torn[3] = 0.5f; // not a whole word
	CHECK_FALSE(b.restoreFreeze(torn.data(), torn.size()));
	std::vector<float> nan = words;
	nan[1] = 65535.0f; // a held grid point's bits made a NaN: 0x7FFF'FFFF
	nan[2] = 32767.0f;
	CHECK_FALSE(b.restoreFreeze(nan.data(), nan.size()));
	CHECK_FALSE(b.freezePendingForTest());
	CHECK(b.restoreFreeze(words.data(), words.size())); // and the whole one is taken
}

namespace {
namespace z10c {

/// A device that has frozen a loud noise's envelope at `st`'s stop (Freeze on), and `play` to run it on.
struct Held {
	SpectralDevice d;
	std::vector<float> l = std::vector<float>(512, 0.0f), r = std::vector<float>(512, 0.0f);
	void play(const float* sc, int blocks) {
		for (int b = 0; b < blocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.setSidechain(sc, sc, sc != nullptr);
			d.processStereo(l.data(), r.data(), 512);
		}
	}
};
void freeze(Held& h, Settings st) {
	h.d.prepare(48000.0);
	st.vocodeFreeze = false;
	h.d.setTarget(st);
	const std::vector<float> loud = noise(48000, 0x51u);
	h.play(loud.data(), 60);
	st.vocodeFreeze = true;
	h.d.setTarget(st);
	h.play(loud.data(), 2);
	REQUIRE(h.d.stftForTest().frozen(0));
}

} // namespace z10c
} // namespace

TEST_CASE("Z10c: a Resolution change while frozen keeps the freeze — the same envelope in dBFS at the new stop") {
	z10c::Held h;
	Settings st = vocodeBasics(); // Fine
	z10c::freeze(h, st);
	const auto held = h.d.stftForTest().sidechainEnvelope(0);
	const double toFs = h.d.stftForTest().envelopeToFs();
	st.vocodeFreeze = true;
	st.resolution = Resolution::Fast;
	h.d.setTarget(st);
	const std::vector<float> quiet = z10b::quieter(noise(48000, 0x77u), 0.1f);
	h.play(quiet.data(), 1);
	REQUIRE(h.d.stftForTest().frozen(0));
	const double nowToFs = h.d.stftForTest().envelopeToFs();
	CHECK(std::fabs(nowToFs - toFs) > 3.0); // NOT VACUOUS: the stop did move, and the STFT was reset with it
	CHECK(z10b::worstDbfs(h.d.stftForTest().sidechainEnvelope(0), nowToFs, held, toFs) < 1e-3);
	h.play(quiet.data(), 60); // and it holds against a sidechain 20 dB quieter
	CHECK(z10b::worstDbfs(h.d.stftForTest().sidechainEnvelope(0), nowToFs, held, toFs) < 1e-3);
}

TEST_CASE("Z10c: a host reset mid-freeze keeps the held envelope; with Freeze off a reset leaves nothing held") {
	z10c::Held h;
	Settings st = vocodeBasics();
	z10c::freeze(h, st);
	const auto held = h.d.stftForTest().sidechainEnvelope(0);
	const double toFs = h.d.stftForTest().envelopeToFs();
	h.d.reset();
	const std::vector<float> quiet = z10b::quieter(noise(48000, 0x77u), 0.1f);
	h.play(quiet.data(), 1);
	REQUIRE(h.d.stftForTest().frozen(0));
	CHECK(z10b::worstDbfs(h.d.stftForTest().sidechainEnvelope(0), toFs, held, toFs) < 1e-4);
	h.play(quiet.data(), 60);
	CHECK(z10b::worstDbfs(h.d.stftForTest().sidechainEnvelope(0), toFs, held, toFs) < 1e-4);
	// AND A RE-ACTIVATION (a restart re-prepares) keeps it too.
	h.d.prepare(48000.0);
	h.play(quiet.data(), 1);
	REQUIRE(h.d.stftForTest().frozen(0));
	CHECK(z10b::worstDbfs(h.d.stftForTest().sidechainEnvelope(0), toFs, held, toFs) < 1e-4);
	// FREEZE OFF, THEN A RESET: the published block holds nothing, so nothing comes back, and the envelope follows the sidechain.
	st.vocodeFreeze = false;
	h.d.setTarget(st);
	h.play(quiet.data(), 1);
	h.d.reset();
	h.play(quiet.data(), 1);
	CHECK_FALSE(h.d.stftForTest().frozen(0));
	CHECK_FALSE(h.d.freezePendingForTest());
}

// ---- F2 (additive design §3.6): the Bell, the second filter and the routing on the device ----------------------------

TEST_CASE("F2 spectral: filter 2, each Gain and the routing round-trip in the row, and their defaults are off, Series and 0 dB") {
	Settings s;
	s.filterMode = 4; // Bell
	s.filterGain = 9.0f;
	s.filter2On = true;
	s.filter2Mode = 2;
	s.filter2Cutoff = static_cast<float>(std::log2(700.0));
	s.filter2Resonance = 0.3f;
	s.filter2Slope = 0.7f;
	s.filter2Gain = -4.0f;
	s.filterParallel = true;
	const Row row = rowOf(s);
	CHECK(stateOf(row, row.size()) == s);
	const Settings d {};
	CHECK_FALSE(d.filter2On);
	CHECK_FALSE(d.filterParallel);
	CHECK(d.filterGain == 0.0f);
	CHECK(d.filter2Gain == 0.0f);
	const feathers::spectral::ShaperParams p = shaperParamsOf(d, kFixedF0Hz);
	CHECK_FALSE(p.filter2.on);
	CHECK(p.parallel == 0.0f);
	// THE SHAPER READS THEM AS SET: filter 2 the first's road (Key track at 1), the routing a whole Parallel.
	const feathers::spectral::ShaperParams q = shaperParamsOf(s, kFixedF0Hz);
	CHECK(q.filter.mode == feathers::spectral::FilterMode::Bell);
	CHECK(q.filter.gainDb == 9.0f);
	CHECK(q.filter2.on);
	CHECK(q.filter2.mode == feathers::spectral::FilterMode::BandPass);
	CHECK(q.filter2.cutoffHz == static_cast<float>(std::exp2(static_cast<double>(s.filter2Cutoff))));
	CHECK(q.filter2.resonance == 0.3f);
	CHECK(q.filter2.slope == 0.7f);
	CHECK(q.filter2.gainDb == -4.0f);
	CHECK(q.filter2.keyTrack == 1.0f);
	CHECK(q.parallel == 1.0f);
	CHECK(q.filterPresence[0] == 1.0f);
	CHECK(q.filterPresence[1] == 1.0f);
}

TEST_CASE("F2 spectral: per-bin gains with two filters in Parallel equal gains() restated, and filter 2 off is the old gains to the bit") {
	Settings s;
	s.resolution = Resolution::Balanced;
	s.filterOn = true;
	s.filterMode = 2; // Band Pass
	s.filterCutoff = static_cast<float>(std::log2(400.0));
	s.filterResonance = 0.2f;
	s.filterSlope = 0.4f;
	const double sr = 48000.0;
	const int n = fftSizeFor(s.resolution, sr);
	const auto gainsOf = [&](const Settings& at) {
		SpectralDevice d;
		d.prepare(sr);
		d.setTarget(at);
		settle(d);
		return std::vector<float>(d.gainsForTest(), d.gainsForTest() + n / 2 + 1);
	};
	const std::vector<float> one = gainsOf(s);
	// FILTER 2 SET UP BUT OFF, IN PARALLEL: the same bits as filter 1 alone.
	Settings off = s;
	off.filter2Mode = 4;
	off.filter2Cutoff = static_cast<float>(std::log2(3000.0));
	off.filter2Gain = 12.0f;
	off.filterParallel = true;
	off.filterGain = -9.0f; // filter 1 is no Bell: inert
	CHECK(gainsOf(off) == one);
	// BOTH ON, IN PARALLEL: the shaper's gains at each bin, restated from the controls.
	Settings two = off;
	two.filter2On = true;
	feathers::spectral::ShaperParams p;
	p.filter = { .on = true, .mode = feathers::spectral::FilterMode::BandPass,
		         .cutoffHz = static_cast<float>(std::exp2(static_cast<double>(s.filterCutoff))), .resonance = 0.2f, .slope = 0.4f,
		         .keyTrack = 1.0f };
	p.filter.gainDb = -9.0f;
	p.filter2 = { .on = true, .mode = feathers::spectral::FilterMode::Bell,
		          .cutoffHz = static_cast<float>(std::exp2(static_cast<double>(off.filter2Cutoff))), .resonance = 0.0f,
		          .slope = 0.25f, .keyTrack = 1.0f };
	p.filter2.gainDb = 12.0f;
	p.parallel = 1.0f;
	const std::vector<float> want = restatedGains(p, kFixedF0Hz, sr, n);
	const std::vector<float> got = gainsOf(two);
	size_t bad = 0;
	for (size_t i = 0; i < want.size(); ++i) {
		bad += got[i] != want[i];
	}
	CHECK(bad == 0);
	CHECK(got != one); // not vacuous: the second filter is heard
}

TEST_CASE("MEASURE F2: Spectral's cost per block at Fine with one filter, two in Series and two in Parallel, Mono and 8-voice Poly, the cutoff moving" * doctest::skip()) {
	// TIME PER 256-FRAME BLOCK, the median of 600 blocks after 100 to settle, in rounds alternating the order (benchmarking-on-a-
	// shared-host). THE CUTOFF MOVES EVERY BLOCK, so the per-bin gains are recomputed each frame (the cost F2 adds to); with nothing
	// moving the gains are kept and a second filter costs nothing per block.
	struct Config {
		const char* name;
		bool second;
		bool parallel;
		bool poly;
	};
	const std::vector<Config> configs {
		{ "Fine one filter mono", false, false, false }, { "Fine two Series mono", true, false, false },
		{ "Fine two Parallel mono", true, true, false }, { "Fine one filter poly8", false, false, true },
		{ "Fine two Series poly8", true, false, true },  { "Fine two Parallel poly8", true, true, true },
	};
	const std::vector<float> input = noise(48000, 0x5eedu);
	std::vector<std::vector<double>> medians(configs.size());
	for (int round = 0; round < 6; ++round) {
		for (size_t k = 0; k < configs.size(); ++k) {
			const size_t c = round % 2 == 0 ? k : configs.size() - 1 - k;
			const Config& cfg = configs[c];
			Settings st;
			st.resolution = Resolution::Fine;
			st.filterOn = true;
			st.filterMode = 0;
			st.filter2On = cfg.second;
			st.filter2Mode = 1;
			st.filterParallel = cfg.parallel;
			const int devices = cfg.poly ? 8 : 1;
			std::vector<std::unique_ptr<SpectralDevice>> d;
			for (int v = 0; v < devices; ++v) {
				d.push_back(std::make_unique<SpectralDevice>());
				d.back()->prepare(48000.0);
				d.back()->setTarget(st);
			}
			std::vector<float> l(256), r(256);
			std::vector<double> times;
			for (int b = 0; b < 700; ++b) {
				st.filterCutoff = static_cast<float>(std::log2(1500.0 + 500.0 * static_cast<double>(b % 64) / 64.0));
				const size_t at = (static_cast<size_t>(b) * 256) % (input.size() - 256);
				const auto t0 = std::chrono::steady_clock::now();
				for (int v = 0; v < devices; ++v) {
					d[static_cast<size_t>(v)]->setTarget(st);
					std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, l.begin());
					if (cfg.poly) {
						d[static_cast<size_t>(v)]->processMono(l.data(), 256);
					}
					else {
						std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 256, r.begin());
						d[static_cast<size_t>(v)]->processStereo(l.data(), r.data(), 256);
					}
				}
				const auto t1 = std::chrono::steady_clock::now();
				if (b >= 100) {
					times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
				}
			}
			std::nth_element(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2), times.end());
			medians[c].push_back(times[times.size() / 2]);
		}
	}
	for (size_t c = 0; c < configs.size(); ++c) {
		auto m = medians[c];
		std::sort(m.begin(), m.end());
		const auto ref = [&](size_t r) {
			auto x = medians[r];
			std::sort(x.begin(), x.end());
			return x[x.size() / 2];
		};
		const double base = ref(configs[c].poly ? 3 : 0);
		MESSAGE(std::string(configs[c].name) << ": median " << m[m.size() / 2] << " us/block (min " << m.front() << ", max " << m.back()
		                        << "), " << m[m.size() / 2] / base << "x one filter");
	}
}

TEST_CASE("OI partials, Centers: the harmonics at the centre set stay — the generator the terrain's sparse country uses — the rest go, and a set half way keeps a harmonic half way") {
	// THE SET'S WEIGHTS FROM THE SHARED GENERATOR (partials::sparseKeep): a set where every one of 2..7 is crisply in or out, then
	// one where a harmonic stands at about half (its partial at −6 dB at Depth 100%).
	const auto crispAt = [] {
		for (int i = 0; i < 4000; ++i) {
			const float set = static_cast<float>(i) / 1000.0f;
			bool crisp = true;
			int kept = 0;
			for (int k = 2; k <= 7; ++k) {
				const double w = feathers::partials::sparseKeep(k, set);
				crisp = crisp && (w == 0.0 || w == 1.0);
				kept += w == 1.0 ? 1 : 0;
			}
			if (crisp && kept >= 1) {
				return set;
			}
		}
		return -1.0f;
	};
	const float set = crispAt();
	REQUIRE(set >= 0.0f);
	const double f0 = kFixedF0Hz * std::exp2(2.0 / 12.0);
	std::vector<Sine> sines;
	for (int k = 1; k <= 7; ++k) {
		sines.push_back({ k * f0, 0.08f });
	}
	Settings st;
	st.resolution = Resolution::Fine;
	st.keyOffset = 2.0f;
	st.partialsOn = true;
	st.partialsMode = 2;
	st.partialsCount = 1.0f; // not read by Centers
	st.partialsDepth = 1.0f;
	st.partialsSet = set;
	const auto [in, out] = partialsRender(sines, st);
	for (int k = 1; k <= 7; ++k) {
		CAPTURE(k);
		const double change = sineLevelDb(out, 48000.0, k * f0) - sineLevelDb(in, 48000.0, k * f0);
		CAPTURE(change);
		if (feathers::partials::sparseKeep(k, set) == 1.0) {
			CHECK(std::fabs(change) < 0.5);
		}
		else {
			CHECK(change < -30.0);
		}
	}
	// HALF WAY: a set where some harmonic of 2..7 is kept at ~0.5 — its partial at the keep's level, −6 dB, not in or out.
	float half = -1.0f;
	int halfK = 0;
	for (int i = 0; i < 4000 && half < 0.0f; ++i) {
		const float s = static_cast<float>(i) / 1000.0f;
		for (int k = 2; k <= 7; ++k) {
			if (std::fabs(feathers::partials::sparseKeep(k, s) - 0.5) < 0.01) {
				half = s;
				halfK = k;
				break;
			}
		}
	}
	REQUIRE(half >= 0.0f);
	st.partialsSet = half;
	const auto [in2, out2] = partialsRender(sines, st);
	const double change = sineLevelDb(out2, 48000.0, halfK * f0) - sineLevelDb(in2, 48000.0, halfK * f0);
	CHECK(change == doctest::Approx(20.0 * std::log10(feathers::partials::sparseKeep(halfK, half))).epsilon(0.1));
}
