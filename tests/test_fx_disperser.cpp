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

#include "character/phi_triangle.hpp"
#include "fx/disperser/disperser.h"
#include "fx/disperser/disperser_response.h"
#include "fx/disperser/disperser_types.h"
#include "test_fx_disperser_fw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <cstdint>
#include <iomanip>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

// **A PORT IS A CLAIM ABOUT ANOTHER PROGRAM, SO THIS SUITE RUNS THAT PROGRAM.** `test_fx_disperser_fw.h`
// is the firmware's disperser transcribed in q31 — its loop, not a restatement of its intent — and the
// cases below hold the port to it: the two parameter derivations over grids, the coefficients, and the
// OUTPUT, sample for sample, at the firmware's own signal level. Nothing in the transcription reads the
// port's constants.

namespace {

using namespace feathers;
using namespace feathers::fx::disperser;

constexpr double kRef = 44100.0;

Settings defaults() { return stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings; }

/// A TEST INPUT WITH SOMETHING TO DISPERSE: clicks (the transient the chirp and punch listen for) over
/// two partials (so the allpasses have a steady signal to phase too), at `amp` of full scale.
std::vector<float> probe(size_t frames, float amp, double rate = kRef) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		const double t = static_cast<double>(i) / rate;
		double x = 0.35 * std::sin(2.0 * std::numbers::pi * 196.0 * t) + 0.15 * std::sin(2.0 * std::numbers::pi * 1733.0 * t);
		if (i % 5000 < 3) {
			x += 0.5;
		}
		v[i] = amp * static_cast<float>(x);
	}
	return v;
}

/// ONE COMPARISON SETTING, in the firmware's vocabulary. **COORDINATES ARE MULTIPLES OF 1/64**, exact in
/// the float a patch stores: a coordinate the float rounds is a different knob position from the q31
/// the transcription is handed, and the delay's whole-sample truncation can turn a ninth-digit
/// difference into an echo a sample late.
struct Case {
	double topo = 0.0;  ///< the unbounded coordinate: lap + knob
	double topoLap = 0.0; ///< Topo's phase past its zone (round 3, `topoPlaceOf`): the firmware's offset, not a whole lap
	double twist = 0.0;
	float gamma = 0.0f;
	int stages = 8;
	int freqKnob = 64;  ///< the firmware's 0..127
	int noteCode = -1;  ///< -1: no note
	float amp = 0.5f;
	size_t frames = 22050;
};

struct Compared {
	double portRms = 0.0;
	double fwRms = 0.0;
	double maxErr = 0.0;      ///< worst |port - firmware|, as a fraction of full scale
	size_t firstApart = 0;    ///< first sample more than `kApart` apart
	uint64_t fwPins = 0;      ///< samples the firmware's line pinned at `ssat #30`
	uint64_t portPins = 0;
	uint64_t fwWraps = 0;     ///< times a firmware `vshl` wrapped
	double fwPeak = 0.0;
	bool fullPath = false;
};

constexpr double kApart = 1e-4;

/// THE PORT'S SETTINGS FOR A CASE: Key Track on with a note (the offset is the knob read as one), off
/// without (the knob is the absolute frequency) — the two firmware readings of one knob.
Settings settingsFor(const Case& c) {
	Settings s = defaults();
	s.topo = static_cast<float>(c.topo);
	s.topoLap = static_cast<float>(c.topoLap);
	s.twist = static_cast<float>(c.twist);
	s.gamma = c.gamma;
	s.stages = c.stages;
	s.keyTrack = c.noteCode >= 0;
	s.freqLog2 = static_cast<float>(c.freqKnob * 13.0 / 127.0);
	s.offsetOctaves = static_cast<float>((c.freqKnob - 64.0) / 64.0 * 6.5);
	return s;
}

fwdisp::Fw firmwareFor(const Case& c, const fwdisp::Options& opt) {
	fwdisp::Fw fw;
	fw.opt = opt;
	const double topoLap = std::floor(c.topo);
	const double twistLap = std::floor(c.twist);
	fw.topoValue = static_cast<fwdisp::q31_t>((c.topo - topoLap) * 2147483647.0);
	fw.twistValue = static_cast<fwdisp::q31_t>((c.twist - twistLap) * 2147483647.0);
	fw.topoPhaseOffset = static_cast<float>(topoLap + c.topoLap);
	fw.twistPhaseOffset = static_cast<float>(twistLap);
	fw.gammaPhase = c.gamma;
	fw.freq = static_cast<uint8_t>(c.freqKnob);
	fw.stages = static_cast<uint8_t>(c.stages);
	if (!opt.quantiseIndex) {
		// THE PORT'S KNOB VALUE, restated here: a float log2(Hz), or a float offset from the note.
		const Settings s = settingsFor(c);
		double index = 0.0;
		if (c.noteCode >= 0) {
			const double hz = 440.0 * std::exp2((c.noteCode - 69.0) / 12.0 + static_cast<double>(s.offsetOctaves));
			index = std::log2(std::max(hz, 1.0)) * 127.0 / 13.0;
		}
		else {
			index = static_cast<double>(s.freqLog2) * (127.0 / 13.0);
		}
		fw.effectiveFreqOverride = std::clamp(index, 0.0, 127.0);
	}
	return fw;
}

/// **THE COMPARISON AGAINST THE PORT AS IT SHIPS**: the firmware with only the port's stated deviations
/// taken out — the index bias (by design), the a2 clamp and the chirp loop cap (the runaway decision),
/// the Padé tangent and the 7-bit indices — and the power-on glide replaced by the port's snap. The
/// DOUBLED WRITE STAYS: it is the firmware's long chirp tail. What is left must agree sample for sample.
fwdisp::Options portOptions() {
	fwdisp::Options o;
	o.biasedIndex = false;
	o.fastTan = false;
	o.quantiseIndex = false;
	o.snapFirst = true;
	o.capLoop = true;
	o.clampA2 = false;
	return o;
}

/// **THE PORT'S STAGES RUN IN THE FIRMWARE'S FORM HERE BY DEFAULT (`setDf2tForTest`).** The engine runs a
/// normalized lattice — the same allpass held still, and one no coefficient move can pump (`runStage`). But
/// the two part when the coefficients MOVE, and they move every fourth block under the stereo LFOs, so a
/// sample-for-sample comparison against the transcription (which is DF2T) would be measuring the fix. So
/// these comparisons hold everything around the stages — coefficients, routing, delay road, cap — to the
/// firmware at the tight bounds they always had, and "THE LATTICE IS THE FIRMWARE'S ALLPASS" holds the
/// lattice to the DF2T: identical held still, and how far apart under the LFOs, measured.
Compared compare(const Case& c, const fwdisp::Options& opt = portOptions(), int portChunk = 97, bool df2t = true) {
	const std::vector<float> in = probe(c.frames, c.amp);

	Disperser d;
	d.prepare(kRef);
	d.setDf2tForTest(df2t);
	d.setTarget(settingsFor(c));
	if (c.noteCode >= 0) {
		d.setNoteContext({ static_cast<float>(c.noteCode), true });
	}
	std::vector<float> pl = in;
	std::vector<float> pr = in;
	// AN ODD CHUNK, so the port's grid is proven independent of the host's cut while being compared.
	for (size_t at = 0; at < c.frames; at += static_cast<size_t>(portChunk)) {
		const int n = static_cast<int>(std::min(static_cast<size_t>(portChunk), c.frames - at));
		d.processStereo(pl.data() + at, pr.data() + at, n);
	}

	fwdisp::Fw fw = firmwareFor(c, opt);
	std::vector<fwdisp::q31_t> ql(c.frames);
	for (size_t i = 0; i < c.frames; ++i) {
		ql[i] = fwdisp::cvt(static_cast<float>(static_cast<double>(in[i]) * fwdisp::kZeroDbfs));
	}
	std::vector<fwdisp::q31_t> qr = ql;
	bool full = false;
	for (size_t at = 0; at < c.frames; at += 128) {
		const size_t n = std::min<size_t>(128, c.frames - at);
		fw.process(ql.data() + at, qr.data() + at, n, c.noteCode);
		full = full || fw.fullPath;
	}

	Compared r;
	r.firstApart = c.frames;
	double ps = 0.0;
	double fs = 0.0;
	for (size_t i = 0; i < c.frames; ++i) {
		const double fl = static_cast<double>(ql[i]) / fwdisp::kZeroDbfs;
		const double fr = static_cast<double>(qr[i]) / fwdisp::kZeroDbfs;
		const double e = std::max(std::abs(static_cast<double>(pl[i]) - fl), std::abs(static_cast<double>(pr[i]) - fr));
		if (e > kApart && r.firstApart == c.frames) {
			r.firstApart = i;
		}
		r.maxErr = std::max(r.maxErr, e);
		ps += static_cast<double>(pl[i]) * pl[i] + static_cast<double>(pr[i]) * pr[i];
		fs += fl * fl + fr * fr;
		r.fwPeak = std::max({ r.fwPeak, std::abs(fl), std::abs(fr) });
	}
	const auto n = static_cast<double>(2 * c.frames);
	r.portRms = std::sqrt(ps / n);
	r.fwRms = std::sqrt(fs / n);
	r.fwPins = fw.linePins;
	r.fwWraps = fw.wraps;
	r.portPins = d.linePinsForTest();
	r.fullPath = full;
	return r;
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-30)); }

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE PARAMETER DERIVATIONS.                                                                         //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the phi constants the disperser reads are the firmware's literals, and two more are not the powers their names say") {
	namespace phi = owlet::dsp::phi;
	CHECK(phi::kPhiN025 == fwdisp::kPhiN025);
	CHECK(phi::kPhi025 == fwdisp::kPhi025);
	CHECK(phi::kPhi033 == fwdisp::kPhi033);
	CHECK(phi::kPhi050 == fwdisp::kPhi050);
	CHECK(phi::kPhi067 == fwdisp::kPhi067);
	CHECK(phi::kPhi075 == fwdisp::kPhi075);
	CHECK(phi::kPhi100 == fwdisp::kPhi100);
	CHECK(phi::kPhi150 == fwdisp::kPhi150);
	CHECK(kDetuneLfoRatio == doctest::Approx(1.1746627).epsilon(1e-9));
	// THE EXPONENT EACH LITERAL ACTUALLY IS — printed, not asserted equal to its name: kPhi033 and kPhi067
	// join the Eroder's three (backlog HW). The port takes the literals.
	const double lnPhi = std::log((1.0 + std::sqrt(5.0)) / 2.0);
	const auto exponentOf = [&](float v) { return std::log(static_cast<double>(v)) / lnPhi; };
	MESSAGE("kPhiN025 = phi^" << exponentOf(phi::kPhiN025) << ", kPhi025 = phi^" << exponentOf(phi::kPhi025)
	                          << ", kPhi033 = phi^" << exponentOf(phi::kPhi033) << ", kPhi067 = phi^"
	                          << exponentOf(phi::kPhi067));
	CHECK(std::abs(exponentOf(phi::kPhi067) - 0.67) > 0.005); // NOT phi^0.67 — the finding is real
	CHECK(std::abs(exponentOf(phi::kPhi050) - 0.5) < 1e-6);   // and the check can tell: 0.5 is exact
}

TEST_CASE("Topo: computeTopoParams is the firmware's, over every zone, laps, Gamma and Twist's rotation") {
	int compared = 0;
	for (int i = 0; i <= 800; ++i) {
		const double knob = static_cast<double>(i) / 800.0 * 0.999999;
		for (const double phRaw : { 0.0, 1.0, 3.0, 128.0, -2.0, 1024.0 * 7.0 }) {
			for (const float twistOffset : { 0.0f, 1.3f, 4.9f }) {
				const auto q = static_cast<fwdisp::q31_t>(knob * 2147483647.0);
				const fwdisp::TopoP f = fwdisp::computeTopo(q, phRaw, twistOffset);
				const TopoParams p = computeTopoParams(knob, phRaw, static_cast<double>(twistOffset));
				CAPTURE(knob);
				CAPTURE(phRaw);
				REQUIRE(p.zone == f.zone);
				// THE POSITION IS THE q31's HERE AND A DOUBLE'S THERE — the zone width is ONE_Q31 / 8, 7 short
				// of a power of two, so positions differ by ~3e-9; every quantity below is a triangle of it.
				CHECK(p.q == doctest::Approx(f.q).epsilon(1e-4));
				CHECK(p.param0 == doctest::Approx(f.param0).epsilon(1e-4));
				CHECK(p.param1 == doctest::Approx(f.param1).epsilon(1e-4));
				CHECK(p.param2 == doctest::Approx(f.param2).epsilon(1e-4));
				CHECK(p.lrOffset == doctest::Approx(f.lrOffset).epsilon(1e-4));
				CHECK(p.detuning == doctest::Approx(f.detuning).epsilon(1e-4));
				CHECK(p.harmonicBlend == doctest::Approx(f.harmonicBlend).epsilon(1e-4));
				CHECK(p.emphasis == doctest::Approx(f.emphasis).epsilon(1e-4));
				++compared;
			}
		}
	}
	CHECK(compared > 14000);
}

TEST_CASE("Twist: computeTwistParams is the firmware's — five single zones, three meta, and everything off the first lap") {
	for (int i = 0; i <= 1600; ++i) {
		const double knob = static_cast<double>(i) / 1600.0 * 0.999999;
		for (const double phRaw : { 0.0, 1.0, 2.0, 128.0, -1.0, 1024.0 * 3.0 }) {
			const auto q = static_cast<fwdisp::q31_t>(knob * 2147483647.0);
			const fwdisp::TwistP f = fwdisp::computeTwist(q, phRaw);
			const TwistParams p = computeTwistParams(knob, phRaw);
			CAPTURE(knob);
			CAPTURE(phRaw);
			CHECK(p.width == doctest::Approx(f.width).epsilon(1e-4));
			CHECK(p.punch == doctest::Approx(f.punch).epsilon(1e-4));
			CHECK(p.chirpAmount == doctest::Approx(f.chirpAmount).epsilon(1e-4));
			CHECK(p.spreadCurve == doctest::Approx(f.spreadCurve).epsilon(1e-4));
			CHECK(p.qTilt == doctest::Approx(f.qTilt).epsilon(1e-4));
			CHECK(p.phaseOffset == doctest::Approx(f.phaseOffset).epsilon(1e-4));
			CHECK(p.lfoRateScale == doctest::Approx(f.lfoRateScale).epsilon(1e-4));
		}
	}
	// NON-VACUITY: the single zones are single — at the Punch zone's centre on lap 0 nothing but punch.
	const TwistParams punch = computeTwistParams(0.1875, 0.0);
	CHECK(punch.punch == doctest::Approx(0.5));
	CHECK(punch.chirpAmount == 0.0);
	CHECK(punch.width == 0.0);
	// …and one lap on, the same knob is the combined evolution.
	const TwistParams lapped = computeTwistParams(0.1875, 1.0);
	CHECK(lapped.phaseOffset == doctest::Approx(0.1875 * 5.0));
}

TEST_CASE("the coordinate is the firmware's knob and its auto-wrapped phase offset") {
	const Coordinate a = splitCoordinate(2.375);
	CHECK(a.lap == 2.0);
	CHECK(a.knob == doctest::Approx(0.375));
	const Coordinate b = splitCoordinate(-0.25);
	CHECK(b.lap == -1.0);
	CHECK(b.knob == doctest::Approx(0.75));
	CHECK(zoneOf(0.375).index == 3);
	CHECK(zoneOf(0.375).position == doctest::Approx(0.0));
	CHECK(zoneOf(0.999999).index == 7);
	CHECK(zoneOf(1.0).index == 7);
}

TEST_CASE("routingOf is processDisperser's per-zone switch: spread, L/R offset, Owlpass separation, Cross mix") {
	for (int zone = 0; zone < 8; ++zone) {
		TopoParams t;
		t.zone = zone;
		t.param0 = 0.6;
		t.lrOffset = 0.2;
		TwistParams w;
		w.width = 0.1;
		const Routing r = routingOf(t, w);
		CAPTURE(zone);
		// Restated from the firmware's switch, by hand.
		const double spread[8] = { 0.6, 0.42, 0.6, 0.3, 0.18, 0.72, 0.6, 0.5 };
		const double lr[8] = { 0.1, 0.16, 0.3, 0.1, 0.1, 0.1, 0.1, 0.1 };
		CHECK(r.spread == doctest::Approx(spread[zone]).epsilon(1e-6));
		CHECK(r.lrOffset == doctest::Approx(lr[zone]).epsilon(1e-6));
		CHECK(r.bimodalSeparation == doctest::Approx(zone == 2 ? 0.6 : 0.0));
		CHECK(r.crossMix == doctest::Approx(zone == 3 ? 0.6 : 0.0).epsilon(1e-6));
	}
	// Owlpass's param0 runs to 5 (octaves of separation); the spread clamps at one.
	TopoParams owl;
	owl.zone = 2;
	owl.param0 = 4.0;
	CHECK(routingOf(owl, {}).spread == 1.0);
	CHECK(routingOf(owl, {}).bimodalSeparation == doctest::Approx(4.0));
}

TEST_CASE("the centre: Freq absolute, Offset from a heard note, and the Offset knob read absolutely before any note") {
	Settings s = defaults();
	s.keyTrack = false;
	s.freqLog2 = 10.0f; // 1024 Hz
	CHECK(freqIndexOf(s, true, 69.0) == doctest::Approx(10.0 * 127.0 / 13.0));
	s.keyTrack = true;
	s.offsetOctaves = 0.0f;
	// A4: 440 Hz, log2 = 8.78136.
	CHECK(freqIndexOf(s, true, 69.0) == doctest::Approx(std::log2(440.0) * 127.0 / 13.0));
	s.offsetOctaves = -1.0f;
	CHECK(freqIndexOf(s, true, 69.0) == doctest::Approx(std::log2(220.0) * 127.0 / 13.0));
	// NO NOTE: the knob position the offset sits at, as the reference's absolute knob — 0 octaves is 64.
	s.offsetOctaves = 0.0f;
	CHECK(freqIndexOf(s, false, 69.0) == doctest::Approx(64.0));
	s.offsetOctaves = static_cast<float>(kOffsetMin);
	CHECK(freqIndexOf(s, false, 69.0) == doctest::Approx(0.0));
	s.offsetOctaves = static_cast<float>(kOffsetMax);
	CHECK(freqIndexOf(s, false, 69.0) == doctest::Approx(127.0));
	// The ends of the absolute knob: 1 Hz and 8192 Hz.
	s.keyTrack = false;
	s.freqLog2 = 0.0f;
	CHECK(freqIndexOf(s, false, 0.0) == doctest::Approx(0.0));
	s.freqLog2 = 13.0f;
	CHECK(freqIndexOf(s, false, 0.0) == doctest::Approx(127.0));
}

TEST_CASE("the fold is calculateFoldedDelay at 44.1 kHz, and the same TIME at 96 kHz") {
	for (int i = 1; i < 3000; ++i) {
		const float samples = static_cast<float>(i) * 7.3f;
		CAPTURE(samples);
		CHECK(foldedDelaySamples(samples, kRef) == static_cast<double>(fwdisp::Fw::foldedDelay(samples)));
		const double at96 = foldedDelaySamples(static_cast<double>(samples) * 96000.0 / kRef, 96000.0);
		CHECK(at96 / 96000.0 == doctest::Approx(static_cast<double>(fwdisp::Fw::foldedDelay(samples)) / kRef).epsilon(1e-5));
	}
	// Always inside [10, 200] ms.
	for (double s = 1.0; s < 1e6; s *= 1.37) {
		const double f = foldedDelaySamples(s, kRef);
		CHECK(f >= 441.0 - 1e-3);
		CHECK(f <= 8819.0 + 1e-3);
	}
}

TEST_CASE("the allpass coefficients: the firmware's, bar the Padé tangent and the a2 clamp — and a STABLE allpass") {
	for (double hz : { 5.0, 14.0, 40.0, 100.0, 440.0, 1000.0, 4000.0, 12000.0, 16000.0 }) {
		for (double q : { 0.5, 2.0, 7.0, 20.0 }) {
			for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
				CAPTURE(hz);
				CAPTURE(q);
				CAPTURE(rate);
				const AllpassCoeffs p = allpassCoeffs(hz, q, rate);
				if (rate == kRef) {
					const fwdisp::Coeffs exact =
					    fwdisp::computeCoeffs(static_cast<float>(hz), static_cast<float>(q), 44100.0f, false, false);
					CHECK(p.a1Half == doctest::Approx(static_cast<double>(exact.a1) / 2147483648.0).epsilon(1e-5));
					CHECK(p.a2 == doctest::Approx(static_cast<double>(exact.a2) / 2147483648.0).epsilon(1e-5));
				}
				const double a1 = 2.0 * p.a1Half;
				const double a2 = p.a2;
				// INSIDE THE STABILITY TRIANGLE: |a2| < 1 and |a1| < 1 + a2 — every Q, every angle, every rate.
				CHECK(std::abs(a2) < 1.0);
				CHECK(1.0 + a1 + a2 > 0.0);
				CHECK(1.0 - a1 + a2 > 0.0);
				// UNITY MAGNITUDE AT EVERY FREQUENCY.
				for (double f : { 30.0, 300.0, 3000.0, 15000.0 }) {
					const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * f / rate);
					const std::complex<double> h = (a2 + a1 * z + z * z) / (1.0 + a1 * z + a2 * z * z);
					CHECK(std::abs(h) == doctest::Approx(1.0).epsilon(1e-6));
				}
			}
		}
	}
	// **THE FIRMWARE'S a2 CLAMP LEAVES THE TRIANGLE**: at the 14 Hz floor (48 kHz, Q 20) the clamped
	// coefficients put a pole outside the unit circle. The case the port's decision rests on, measured.
	{
		const double k = std::tan(std::numbers::pi * kAllpassLowHz / 48000.0);
		const double kq = k / 20.0;
		const double norm = 1.0 / (1.0 + kq + k * k);
		const double a1 = 2.0 * (k * k - 1.0) * norm;
		const double a2True = (1.0 - kq + k * k) * norm;
		const double a2Clamped = std::min(a2True, 0.9999);
		MESSAGE("48 kHz, Q 20, 14 Hz: 1 + a1 + a2 = " << 1.0 + a1 + a2True << " true, " << 1.0 + a1 + a2Clamped
		                                              << " with the firmware's clamp");
		CHECK(1.0 + a1 + a2True > 0.0);
		CHECK(1.0 + a1 + a2Clamped < 0.0);
	}
	// THE PADÉ FIT's OWN ERROR, measured rather than assumed: at 12 kHz it moves a2 visibly.
	const fwdisp::Coeffs pade = fwdisp::computeCoeffs(12000.0f, 2.0f, 44100.0f, true);
	const fwdisp::Coeffs exact = fwdisp::computeCoeffs(12000.0f, 2.0f, 44100.0f, false);
	MESSAGE("12 kHz Q2: a2 Pade " << pade.a2 / 2147483648.0 << " exact " << exact.a2 / 2147483648.0);
	// THE ANGLE CLAMP IS HERTZ: at 96 kHz a 10 Hz stage sits where a 14.04 Hz one does, as at 44.1.
	const AllpassCoeffs low96 = allpassCoeffs(10.0, 2.0, 96000.0);
	const AllpassCoeffs at1404 = allpassCoeffs(kAllpassLowHz, 2.0, 96000.0);
	CHECK(low96.a2 == at1404.a2);
}

//--------------------------------------------------------------------------------------------------//
// THE SAMPLE LOOP, AGAINST THE FIRMWARE'S.                                                           //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the stage layout after one block is updateCoefficients': frequencies, Q, gains and delay offsets") {
	for (int tz = 0; tz < 8; ++tz) {
		for (int wz = 0; wz < 8; ++wz) {
			Case c;
			c.topo = (tz + 0.375) / 8.0;
			c.twist = (wz + 0.625) / 8.0;
			c.stages = 13;
			c.freqKnob = 80;
			c.frames = 128 * 9;
			const std::vector<float> in = probe(c.frames, 0.5f);
			Disperser d;
			d.prepare(kRef);
			d.setTarget(settingsFor(c));
			std::vector<float> l = in;
			std::vector<float> r = in;
			d.processStereo(l.data(), r.data(), static_cast<int>(c.frames));
			fwdisp::Fw fw = firmwareFor(c, portOptions());
			std::vector<fwdisp::q31_t> ql(c.frames, 0);
			std::vector<fwdisp::q31_t> qr(c.frames, 0);
			for (size_t at = 0; at < c.frames; at += 128) {
				fw.process(ql.data() + at, qr.data() + at, 128, -1);
			}
			CAPTURE(tz);
			CAPTURE(wz);
			for (int i = 0; i < c.stages; ++i) {
				CAPTURE(i);
				CHECK(d.stageHzForTest(i, 0) == doctest::Approx(fw.stageHzL[i]).epsilon(1e-5));
				CHECK(d.stageHzForTest(i, 1) == doctest::Approx(fw.stageHzR[i]).epsilon(1e-5));
				CHECK(d.stageQForTest(i) == doctest::Approx(fw.stageQ[i]).epsilon(1e-5));
				CHECK(d.stageGainForTest(i) == doctest::Approx(fw.stageGains[static_cast<size_t>(i)] / 1073741823.5).epsilon(1e-5));
				CHECK(d.stageOffsetForTest(i) == fw.stageOffsets[static_cast<size_t>(i)]);
			}
		}
	}
}

// **WHERE "SAMPLE FOR SAMPLE" HOLDS, AND WHY NOT BELOW IT — MEASURED.** Away from the allpass's 14 Hz
// floor the port and the firmware's loop agree to a few millionths of full scale. Near it they part by
// up to ~2e-3, and that residual is the FIRMWARE'S OWN q31 ROUNDING NOISE, not a difference in what the
// two compute: a stage at the floor has its poles 0.001 from the unit circle, its DF2T states amplify
// each `vqrdmulh`'s half-LSB by ~10^6, and the result is an additive noise of fixed absolute size —
// the case "the residual at the floor is the firmware's rounding noise" below holds it to that shape.
// So the zone grid is asserted to the sample with the stages up (Freq knob 110, 2.4 kHz), and to the
// noise floor at the default knob.
constexpr int kHighKnob = 110;
constexpr double kExact = 1e-4;
constexpr double kNoiseFloor = 5e-3;

namespace {

void outputZoneGrid(bool full) {
	int compared = 0;
	int delayRoad = 0;
	double worstHigh = 0.0;
	double worstDefault = 0.0;
	double worstLevel = 0.0;
	for (int tz = 0; tz < 8; ++tz) {
		for (int wz = 0; wz < 8; ++wz) {
			for (const double laps : { 0.0, 1.0 }) {
				for (const int knob : { kHighKnob, 64 }) {
					// THE DEFAULT RUN TAKES ONE OF THE FOUR (lap, knob) COMBINATIONS PER ZONE PAIR, rotated so each
					// is met sixteen times; the MEASURE case takes all four.
					if (!full && static_cast<int>(laps) * 2 + (knob == 64 ? 1 : 0) != (tz + wz) % 4) {
						continue;
					}
					Case c;
					c.topo = (tz + 0.375) / 8.0 + laps;
					c.twist = (wz + 0.5625) / 8.0 + laps;
					c.freqKnob = knob;
					c.frames = 11025;
					const Compared r = compare(c);
					CAPTURE(tz);
					CAPTURE(wz);
					CAPTURE(laps);
					CAPTURE(knob);
					CAPTURE(r.maxErr);
					CAPTURE(r.firstApart);
					CAPTURE(r.fwPins);
					CAPTURE(r.fwPeak);
					REQUIRE(r.fwRms > 1e-3); // the firmware says something here
					CHECK(r.fwPins == 0);    // the grid is not in runaway country; that has its own case
					const double level = db(r.portRms / r.fwRms);
					worstLevel = std::max(worstLevel, std::abs(level));
					CHECK(std::abs(level) < 0.05);
					if (knob == kHighKnob) {
						CHECK(r.maxErr < kExact);
						worstHigh = std::max(worstHigh, r.maxErr);
					}
					else {
						CHECK(r.maxErr < kNoiseFloor);
						worstDefault = std::max(worstDefault, r.maxErr);
					}
					delayRoad += r.fullPath ? 1 : 0;
					++compared;
				}
			}
		}
	}
	MESSAGE("over " << compared << " runs: worst level " << worstLevel << " dB; worst sample error " << worstHigh
	                << " of full scale with the stages up, " << worstDefault << " at the default Freq; the delay road ran in "
	                << delayRoad);
	CHECK(delayRoad >= (full ? 60 : 15)); // Punch, Chirp and the meta zones reach the delay; Width/Curve/QTilt on lap 0 do not
	CHECK(delayRoad <= compared - (full ? 20 : 5));
}

} // namespace

TEST_CASE("THE OUTPUT: every Topo zone against every Twist zone, sample for sample with the firmware's loop") {
	// 64 runs: every zone pair once, the (lap, Freq knob) combination rotated.
	outputZoneGrid(false);
}

TEST_CASE("MEASURE: THE OUTPUT — every zone pair on both laps at both Freq knobs" * doctest::skip()) {
	// WHAT: all 256 comparisons with the transcription (~13 s under load). WHEN: before touching the coefficient
	// derivations, the routings, the delay road or the smoothing.
	outputZoneGrid(true);
}

TEST_CASE("THE OUTPUT: stage counts 1 to 32 in every routing, including the odd counts Parallel, Nested and Spring split") {
	double worst = 0.0;
	for (int tz = 0; tz < 8; ++tz) {
		for (const int stages : { 1, 2, 3, 4, 5, 6, 7, 8, 11, 16, 24, 31, 32 }) {
			Case c;
			c.topo = (tz + 0.6875) / 8.0;
			c.twist = 3.375 / 8.0; // Chirp: the delay road, fed back through every routing
			c.stages = stages;
			c.freqKnob = kHighKnob;
			c.frames = 8820;
			const Compared r = compare(c);
			CAPTURE(tz);
			CAPTURE(stages);
			CAPTURE(r.maxErr);
			CAPTURE(r.fwPins);
			REQUIRE(r.fwRms > 1e-3);
			CHECK(r.fwPins == 0);
			CHECK(std::abs(db(r.portRms / r.fwRms)) < 0.05);
			CHECK(r.maxErr < kExact);
			worst = std::max(worst, r.maxErr);
		}
	}
	MESSAGE("worst sample error over stage counts: " << worst);
}

TEST_CASE("THE OUTPUT: across the Freq knob, with and without a note, and with Gamma") {
	double worst = 0.0;
	for (const int knob : { 0, 10, 30, 50, 64, 80, 100, 117, 127 }) {
		for (const int note : { -1, 36, 60, 84 }) {
			for (const float gamma : { 0.0f, 3.0f }) {
				Case c;
				c.topo = 0.203125;
				c.twist = 3.75 / 8.0;
				c.freqKnob = knob;
				c.noteCode = note;
				c.gamma = gamma;
				c.frames = 8820;
				const Compared r = compare(c);
				CAPTURE(knob);
				CAPTURE(note);
				CAPTURE(gamma);
				CAPTURE(r.maxErr);
				CAPTURE(r.fwPins);
				REQUIRE(r.fwRms > 1e-4);
				CHECK(r.fwPins == 0);
				CHECK(std::abs(db(r.portRms / r.fwRms)) < 0.05);
				CHECK(r.maxErr < kNoiseFloor);
				worst = std::max(worst, r.maxErr);
			}
		}
	}
	MESSAGE("worst sample error over freq/note/gamma: " << worst);
}

TEST_CASE("the residual at the floor is the firmware's rounding noise: its size does not follow the signal") {
	// A DIFFERENCE IN WHAT THE TWO COMPUTE SCALES WITH THE SIGNAL; rounding noise does not. Over a 64x
	// range of input level the residual at the default Freq (stages down at the 14 Hz floor) must stay
	// the same order of size — and with the stages up it must vanish.
	std::vector<double> errs;
	for (const float amp : { 0.0625f, 0.25f, 1.0f, 4.0f }) {
		Case c;
		c.topo = 0.03125;
		c.twist = 0.046875;
		c.amp = amp;
		c.frames = 11025;
		const Compared r = compare(c);
		CAPTURE(amp);
		CAPTURE(r.maxErr);
		errs.push_back(r.maxErr);
	}
	MESSAGE("residual at amp 1/16, 1/4, 1, 4: " << errs[0] << ", " << errs[1] << ", " << errs[2] << ", " << errs[3]);
	CHECK(errs[3] / errs[0] < 8.0); // 64x the signal, well under 8x the residual
	CHECK(errs[0] > 1e-5);          // NON-VACUITY: there is a residual here to be shaped
	Case up;
	up.topo = 0.03125;
	up.twist = 0.046875;
	up.freqKnob = 127;
	up.frames = 11025;
	CHECK(compare(up).maxErr < 2e-5);
}


TEST_CASE("MEASURE: SURVEY — the firmware's level, its runaway, and its wraps, on the transcription as it stands" * doctest::skip()) {
	// WHAT: the firmware as shipped over 48 settings (~6 s of CPU). WHEN: when the port doc's firmware figures
	// are quoted or the transcription changes. It asserts nothing about the port.
	// **AS THE FIRMWARE RUNS, ALL FOUR OF ITS OWN BEHAVIOURS ON** — the bias, the Padé tangent, the 7-bit
	// indices and the power-on glide. This case asserts nothing about the port; it is the measurement
	// the port doc quotes.
	fwdisp::Options asShipped;
	int pinned = 0;
	int wrapped = 0;
	int runs = 0;
	double hottest = 0.0;
	for (int tz = 0; tz < 8; ++tz) {
		for (const double twist : { 1.5 / 8.0, 3.5 / 8.0, 3.95 / 8.0, 5.5 / 8.0, 6.5 / 8.0, 7.5 / 8.0 }) {
			Case c;
			c.topo = (tz + 0.5) / 8.0;
			c.twist = twist;
			c.frames = 44100;
			const Compared r = compare(c, asShipped);
			pinned += r.fwPins > 0 ? 1 : 0;
			wrapped += r.fwWraps > 0 ? 1 : 0;
			hottest = std::max(hottest, r.fwPeak);
			++runs;
			MESSAGE("topo zone " << tz << " twist " << twist * 8.0 << ": out rms " << db(r.fwRms) << " dBFS, peak "
			                     << db(r.fwPeak) << " dBFS, line pins " << r.fwPins << ", vshl wraps " << r.fwWraps);
		}
	}
	MESSAGE("of " << runs << " settings the line pinned in " << pinned << " and a vshl wrapped in " << wrapped
	              << "; hottest output peak " << db(hottest) << " dBFS");
	CHECK(runs == 48);
}


namespace {

std::vector<float> render(Disperser& d, const std::vector<float>& in, int chunk, std::vector<float>* rightOut = nullptr) {
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t at = 0; at < in.size(); at += static_cast<size_t>(chunk)) {
		const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), in.size() - at));
		d.processStereo(l.data() + at, r.data() + at, n);
	}
	if (rightOut != nullptr) {
		*rightOut = r;
	}
	return l;
}

} // namespace

TEST_CASE("THE CAP: the firmware's doubled chirp loop runs away past ~0.9; the port keeps the doubling and caps the loop at 0.97") {
	// **THE FIRMWARE'S `writeAtOffsetFractionalQ16` DOUBLES EVERY DELAY WRITE** (`vshl` after a `vqrdmulh`),
	// and its chirp loop crosses unity near the top of the Chirp zone. The decision: a strategic limiter
	// that does not sacrifice long tails. The port keeps the doubling (it IS the long tail) and holds the
	// loop's bound at `kChirpLoopCeiling`.
	fwdisp::Options faithful = portOptions();
	faithful.capLoop = false;
	Case c;
	c.topo = (2 + 0.875) / 8.0; // Owlpass, harmonic blend up
	c.twist = (3.0 + 0.984375) / 8.0;
	c.amp = 1.0f;
	c.frames = 44100 * 3;
	const Compared uncapped = compare(c, faithful);
	const Compared port = compare(c);
	MESSAGE("chirp " << computeTwistParams(3.984375 / 8.0, 0.0).chirpAmount << ": the firmware's loop rms "
	                 << db(uncapped.fwRms) << " dBFS, line pinned " << uncapped.fwPins << " times; the port "
	                 << db(port.portRms) << " dBFS, pinned " << port.portPins << " (the capped transcription "
	                 << db(port.fwRms) << " dBFS)");
	CHECK(uncapped.fwPins > 0);
	CHECK(db(uncapped.fwRms) > 10.0);
	CHECK(port.fwPins == 0);
	CHECK(port.portPins == 0);
	CHECK(db(port.portRms) < 0.0);
	CHECK(std::abs(db(port.portRms / port.fwRms)) < 0.05);
}

namespace {

/// TIME TO -60 dB after a 0.5 s burst, in 50 ms windows, against the output's peak while the input played.
double t60Of(const std::vector<double>& out, size_t burst, double rate) {
	double atStop = 0.0;
	for (size_t i = 0; i < burst; ++i) {
		atStop = std::max(atStop, std::abs(out[i]));
	}
	const auto window = static_cast<size_t>(rate * 0.05);
	for (size_t at = burst; at + window <= out.size(); at += window) {
		double peak = 0.0;
		for (size_t i = at; i < at + window; ++i) {
			peak = std::max(peak, std::abs(out[i]));
		}
		if (peak < atStop * 1e-3) {
			return static_cast<double>(at - burst) / rate;
		}
	}
	return -1.0;
}

std::vector<double> burstThroughFirmware(const Case& c, const fwdisp::Options& opt, size_t total) {
	fwdisp::Fw fw = firmwareFor(c, opt);
	std::vector<double> out;
	std::vector<fwdisp::q31_t> l(128);
	std::vector<fwdisp::q31_t> r(128);
	for (size_t at = 0; at < total; at += 128) {
		for (size_t i = 0; i < 128; ++i) {
			const size_t n = at + i;
			const double x = n < 22050 ? 0.8 * std::sin(0.05 * static_cast<double>(n)) : 0.0;
			l[i] = r[i] = fwdisp::cvt(static_cast<float>(x * fwdisp::kZeroDbfs));
		}
		fw.process(l.data(), r.data(), 128, -1);
		for (size_t i = 0; i < 128; ++i) {
			out.push_back(static_cast<double>(l[i]) / fwdisp::kZeroDbfs);
		}
	}
	return out;
}

std::vector<double> burstThroughPort(const Case& c, size_t total) {
	Disperser d;
	d.prepare(kRef);
	d.setTarget(settingsFor(c));
	std::vector<double> out;
	std::vector<float> l(128);
	std::vector<float> r(128);
	for (size_t at = 0; at < total; at += 128) {
		for (size_t i = 0; i < 128; ++i) {
			const size_t n = at + i;
			l[i] = r[i] = n < 22050 ? 0.8f * static_cast<float>(std::sin(0.05 * static_cast<double>(n))) : 0.0f;
		}
		d.processStereo(l.data(), r.data(), 128);
		for (const float v : l) {
			out.push_back(static_cast<double>(v));
		}
	}
	return out;
}

} // namespace

namespace {

/// THE CONDITION THE DECISION SET — decay times at settings that did not run away are unchanged — held
/// against the transcription with its doubled write and no cap, over a grid. The REJECTED single write is
/// printed beside it: the "obvious fix" a later session might otherwise re-apply, which shortens every tail.
void tailsKept(std::initializer_list<double> chirps, std::initializer_list<int> zones, std::initializer_list<int> knobs) {
	fwdisp::Options faithful = portOptions();
	faithful.capLoop = false;
	fwdisp::Options single = faithful;
	single.doubledWrite = false;
	const size_t total = 44100 * 9;
	for (const double chirp : chirps) {
		double worstRatioSingle = 1.0;
		for (const int tz : zones) {
			for (const int knob : knobs) {
				Case c;
				c.topo = (tz + 0.875) / 8.0;
				c.twist = (3.0 + chirp) / 8.0;
				c.freqKnob = knob;
				const double firmware = t60Of(burstThroughFirmware(c, faithful, total), 22050, kRef);
				const double port = t60Of(burstThroughPort(c, total), 22050, kRef);
				const double rejected = t60Of(burstThroughFirmware(c, single, total), 22050, kRef);
				CAPTURE(chirp);
				CAPTURE(tz);
				CAPTURE(knob);
				CAPTURE(firmware);
				CAPTURE(port);
				REQUIRE(firmware > 0.0);
				CHECK(std::abs(port - firmware) <= std::max(0.1 * firmware, 0.05)); // 10%, or one 50 ms window
				worstRatioSingle = std::min(worstRatioSingle, rejected / firmware);
			}
		}
		MESSAGE("chirp " << chirp << ": the rejected single write's tail was as short as " << worstRatioSingle
		                 << "x the firmware's");
	}
}

} // namespace

TEST_CASE("LONG TAILS KEPT: below the cap's engage point the port's chirp tail is the doubled-write firmware's") {
	// Chirp 0.5 and 0.85 (the tail is longest just below the engage point), two routings. The whole grid
	// — four Chirp values, four routings, two Freqs — is the MEASURE case below.
	tailsKept({ 0.5, 0.85 }, { 0, 5 }, { 110 });
}

TEST_CASE("MEASURE: LONG TAILS KEPT over the whole grid" * doctest::skip()) {
	tailsKept({ 0.25, 0.5, 0.7, 0.85 }, { 0, 2, 5, 7 }, { 90, 110 });
}

TEST_CASE("the cap is continuous: sweeping Chirp through its engage point steps the write gain no more than the firmware's own") {
	// `min(1, ceiling / loopGain)` has no step; the gains it scales already step once per block in the
	// firmware (the transient and the smoother move them). So the capped per-block write gain may move no
	// more between blocks than the uncapped one does, as Twist sweeps 0.8 -> 0.999 of the Chirp zone.
	Disperser d;
	d.prepare(kRef);
	Settings s = defaults();
	s.topo = static_cast<float>((2 + 0.875) / 8.0); // Owlpass: harmonic blend up, the cap engages earliest
	const std::vector<float> in = probe(44100 * 12, 0.5f);
	std::vector<float> l = in;
	std::vector<float> r = in;
	double lastRaw = -1.0;
	double lastCapped = -1.0;
	double worstRaw = 0.0;
	double worstCapped = 0.0;
	int engaged = 0;
	int blocks = 0;
	for (size_t at = 0; at + 128 <= in.size(); at += 128) {
		const double progress = static_cast<double>(at) / static_cast<double>(in.size());
		s.twist = static_cast<float>((3.0 + 0.8 + 0.199 * progress) / 8.0);
		d.setTarget(s);
		d.processStereo(l.data() + at, r.data() + at, 128);
		const auto& b = d.blockForTest();
		if (lastRaw >= 0.0) {
			worstRaw = std::max(worstRaw, std::abs(b.chirpGainFRaw - lastRaw));
			worstCapped = std::max(worstCapped, std::abs(b.chirpGainF - lastCapped));
		}
		lastRaw = b.chirpGainFRaw;
		lastCapped = b.chirpGainF;
		engaged += b.chirpGainF < b.chirpGainFRaw ? 1 : 0;
		++blocks;
	}
	MESSAGE("largest block-to-block step of the write gain: " << worstCapped << " capped, " << worstRaw
	                                                          << " uncapped; the cap engaged in " << engaged << " of "
	                                                          << blocks << " blocks");
	CHECK(engaged > blocks / 20); // NON-VACUITY: the sweep reached the engage point
	CHECK(engaged < blocks);      // and started below it
	CHECK(worstCapped <= worstRaw * 1.05 + 1e-6);
	CHECK(d.linePinsForTest() == 0);
}

namespace {

/// THE RUNAWAY SWEEP'S BODY. The settings are FOUND, not typed: the first `perCombo` Twist knob positions on
/// each lap, with each Gamma, whose Chirp is above 0.9 — the meta zones and every lap evolve Chirp through the
/// phi patterns. EVERY TOPO ZONE, Parallel, Nested and Spring among them (a first version took four series
/// zones at one position and 8 stages, and missed the runaway a review then found in exactly those three),
/// at each stage count, with the position in the zone and the Freq rotated so each zone meets each of three.
/// Each run: 1.5 s of loud sustained input, then 2.5 s of silence. What must hold: the line never pins, the
/// output stays under +24 dBFS, and after the input stops it does not grow.
void runawaySweep(std::initializer_list<double> laps, std::initializer_list<float> gammas, int perCombo,
                  std::initializer_list<int> stageCounts) {
	int runs = 0;
	double hottest = -200.0;
	for (const double lap : laps) {
		for (const float gamma : gammas) {
			int found = 0;
			for (int k = 0; k < 256 && found < perCombo; ++k) {
				const double knob = (static_cast<double>(k) + 0.5) / 256.0;
				const TwistParams tp = computeTwistParams(knob, lap + 1024.0 * static_cast<double>(gamma));
				if (tp.chirpAmount <= 0.9) {
					continue;
				}
				++found;
				int stageTurn = 0;
				for (const int stages : stageCounts) {
					++stageTurn;
					for (int tz = 0; tz < 8; ++tz) {
						const int turn = (tz + found + stageTurn) % 3;
						Settings s = defaults();
						s.keyTrack = false;
						s.topo = static_cast<float>(lap + (tz + (turn == 0 ? 0.125 : turn == 1 ? 0.5 : 0.875)) / 8.0);
						s.twist = static_cast<float>(lap + knob);
						s.gamma = gamma;
						s.stages = stages;
						s.freqLog2 = turn == 0 ? 2.0f : turn == 1 ? 6.55f : 11.0f;
						Disperser d;
						d.prepare(48000.0);
						d.setTarget(s);
						std::vector<float> in = probe(72000, 1.0f, 48000.0);
						in.resize(192000, 0.0f);
						const std::vector<float> out = render(d, in, 480);
						double peak = 0.0;
						double firstAfter = 0.0;
						double lastWindow = 0.0;
						for (size_t i = 0; i < out.size(); ++i) {
							const double v = std::abs(static_cast<double>(out[i]));
							peak = std::max(peak, v);
							if (i >= 72000 && i < 96000) {
								firstAfter = std::max(firstAfter, v);
							}
							if (i >= out.size() - 24000) {
								lastWindow = std::max(lastWindow, v);
							}
						}
						CAPTURE(lap);
						CAPTURE(gamma);
						CAPTURE(knob);
						CAPTURE(tz);
						CAPTURE(stages);
						CAPTURE(tp.chirpAmount);
						CHECK(d.linePinsForTest() == 0);
						// BOUNDED, NOT QUIET: a loop held at 0.97 resonates. The firmware itself peaks at +20.7 dBFS
						// without pinning (MEASURED on the transcription, a lap-2 meta setting); the line pins at +30.
						CHECK(db(peak) < 24.0);
						CHECK(db(lastWindow) <= db(firstAfter) + 0.5);
						hottest = std::max(hottest, db(peak));
						++runs;
					}
				}
			}
			CHECK(found > 0);
		}
	}
	MESSAGE(runs << " settings above Chirp 0.9: hottest peak " << hottest << " dBFS, none pinned, none grew");
}

} // namespace

TEST_CASE("THE RUNAWAY SWEEP: every Topo zone at the top of Chirp, 8 and 32 stages, one Freq each — bounded, and does not grow") {
	// The default run's representative (16 runs): the first Twist position past Chirp 0.9 on the first lap.
	// **Its known-bads are the cap removed and the cap without the routing's factor** — both pin here.
	runawaySweep({ 0.0 }, { 0.0f }, 1, { 8, 32 });
}

TEST_CASE("MEASURE: THE RUNAWAY SWEEP — two laps × Gamma 0/3 × two Chirp positions each × every zone × 8/32 stages" * doctest::skip()) {
	// WHAT: 128 runs (~90 s under load) — the settings above plus the second lap, Gamma, and a second Twist
	// position each. WHEN: before touching the cap, the routing factor, the delay road or the stages.
	runawaySweep({ 0.0, 1.0 }, { 0.0f, 3.0f }, 2, { 8, 32 });
}

namespace {

/// Half-scale white noise, a fixed seed — every frequency at once, so a routing's hottest band is found.
std::vector<float> noiseBurst(size_t frames, size_t total) {
	std::vector<float> v(total, 0.0f);
	uint32_t x = 0x12345678u;
	for (size_t i = 0; i < frames; ++i) {
		x = x * 1664525u + 1013904223u;
		v[i] = (static_cast<float>(x >> 8) / 16777216.0f - 0.5f);
	}
	return v;
}

/// **THE LOOP'S GAIN AS THE ENGINE HAS IT NOW** — the doubled writes after the cap, the head's gain, and the
/// routing's largest magnitude over frequency, computed by the plot's model from the ENGINE's own stage gains
/// (the plot's cases hold that model to the engine's impulse response in every routing). Sampled at 1,200
/// log-spaced frequencies, so a lower bound on the true peak: a check that it is under the ceiling is a
/// necessary one, and the audio cases carry the rest.
double engineLoopGain(const Disperser& d, const Settings& s, double rate) {
	ResponseModel m = responseModelOf(s, false, 60.0, rate);
	std::array<float, kMaxStages> gains {};
	for (int i = 0; i < static_cast<int>(kMaxStages); ++i) {
		gains[static_cast<size_t>(i)] = static_cast<float>(d.stageGainForTest(i));
	}
	useStageGains(m, gains.data());
	double peak = 0.0;
	for (int k = 0; k < 1200; ++k) {
		const double hz = 5.0 * std::pow(std::min(20000.0, rate * 0.45) / 5.0, k / 1199.0);
		const auto h = routingResponse(m, hz);
		peak = std::max({ peak, std::abs(h[0]), std::abs(h[1]) });
	}
	const auto& b = d.blockForTest();
	const double fb = std::max(b.twist.punch, b.twist.chirpAmount) * 0.99;
	return 2.0 * (b.chirpGainF + b.chirpGain2F) * fb * peak;
}

/// ONE RUN OF THE BOUND CHECK: a second of noise with the LFOs held (so the model and the engine agree), then
/// the loop's gain read off the engine.
double boundRun(int tz, double inZone, double lap, int stages, float freqLog2, double twist) {
	Settings s = defaults();
	s.keyTrack = false;
	s.topo = static_cast<float>(lap + (tz + inZone) / 8.0);
	s.twist = static_cast<float>(twist);
	s.stages = stages;
	s.freqLog2 = freqLog2;
	Disperser d;
	d.prepare(48000.0);
	d.setLfoFrozenForTest(true);
	d.setTarget(s);
	(void)render(d, noiseBurst(48000, 48000), 480);
	return engineLoopGain(d, s, 48000.0);
}

} // namespace

TEST_CASE("THE CAP IS A BOUND IN EVERY ROUTING: the engine's own loop gain stays under 0.97 in Parallel, Nested and Spring too") {
	// A review MEASURED the first cap failing here: it took the routing's gain as 1, which holds in series
	// only — a half-chain, a nested half or a spring tap can carry a product of emphasis gains near 2.3. The
	// loop gain is read off the engine (its stage gains, through the plot's model), not off the cap's own
	// arithmetic: **its known-bad is the old bound**, and every parallel routing goes over.
	double worst = 0.0;
	int runs = 0;
	for (int tz = 0; tz < 8; ++tz) {
		for (const double lap : { -1.0, 0.0 }) {
			for (const float freq : { (tz + static_cast<int>(lap)) % 2 == 0 ? 2.0f : 11.0f }) { // one Freq per zone and lap, alternating; the MEASURE case has all three
				const double g = boundRun(tz, 0.5, lap, 32, freq, 0.4938);
				CAPTURE(tz);
				CAPTURE(lap);
				CAPTURE(freq);
				CHECK(g <= static_cast<double>(kChirpLoopCeiling) + 1e-4);
				worst = std::max(worst, g);
				++runs;
			}
		}
	}
	MESSAGE(runs << " settings at the top of Chirp: the engine's loop gain peaks at " << worst);
	CHECK(worst > 0.5); // non-vacuity: the loop is live, not switched off
}

TEST_CASE("MEASURE: THE CAP IS A BOUND over three positions per zone, two laps, 8/16/32 stages, three Freqs, two Chirps" * doctest::skip()) {
	double worst = 0.0;
	for (int tz = 0; tz < 8; ++tz) {
		for (const double inZone : { 0.125, 0.5, 0.875 }) {
			for (const double lap : { -1.0, 0.0 }) {
				for (const int stages : { 8, 16, 32 }) {
					for (const float freq : { 2.0f, 6.55f, 11.0f }) {
						for (const double twist : { 0.4938, 0.498 }) {
							const double g = boundRun(tz, inZone, lap, stages, freq, twist);
							CAPTURE(tz);
							CAPTURE(inZone);
							CAPTURE(lap);
							CAPTURE(stages);
							CAPTURE(freq);
							CAPTURE(twist);
							CHECK(g <= static_cast<double>(kChirpLoopCeiling) + 1e-4);
							worst = std::max(worst, g);
						}
					}
				}
			}
		}
	}
	MESSAGE("the engine's loop gain peaks at " << worst);
}

namespace {

/// ONE TIME-VARYING RUN, the review's bench made a function: noise (σ 0.3) for 2 s, then silence to 8 s, in
/// 64-sample blocks, `move` changing the settings before every block. Returns the peak, the peak over the
/// first second after the input and over the run's last two seconds (from 3 s at the earliest), and the pins.
struct Varying {
	double peak = 0.0;
	double justAfter = 0.0;
	double late = 0.0;
	uint64_t pins = 0;
	uint64_t rider = 0;
};
template <typename Move>
Varying varyingRun(Settings s, Move move, bool df2t = false, bool writesOff = false, double seconds = 8.0, bool riderOff = false) {
	Disperser d;
	d.prepare(48000.0);
	d.setDf2tForTest(df2t);
	d.setWritesOffForTest(writesOff);
	d.setRiderOffForTest(riderOff);
	uint32_t x = 0x9e3779b9u;
	const auto noise = [&x] {
		x = x * 1664525u + 1013904223u;
		return (static_cast<float>(x >> 8) / 16777216.0f - 0.5f) * 1.04f; // σ 0.3
	};
	constexpr int kB = 64;
	std::array<float, kB> l {};
	std::array<float, kB> r {};
	Varying v;
	const int blocks = static_cast<int>(48000.0 * seconds / kB);
	for (int b = 0; b < blocks; ++b) {
		const double t = static_cast<double>(b) * kB / 48000.0;
		move(s, b, t);
		d.setTarget(s);
		for (int i = 0; i < kB; ++i) {
			l[static_cast<size_t>(i)] = t < 2.0 ? noise() : 0.0f;
			r[static_cast<size_t>(i)] = t < 2.0 ? noise() : 0.0f;
		}
		d.processStereo(l.data(), r.data(), kB);
		double m = 0.0;
		for (int i = 0; i < kB; ++i) {
			m = std::max({ m, std::abs(static_cast<double>(l[static_cast<size_t>(i)])), std::abs(static_cast<double>(r[static_cast<size_t>(i)])) });
		}
		v.peak = std::max(v.peak, m);
		if (t >= 2.0 && t < 3.0) {
			v.justAfter = std::max(v.justAfter, m);
		}
		if (t >= std::max(3.0, seconds - 2.0)) { // the last two seconds, never overlapping the first after
			v.late = std::max(v.late, m);
		}
	}
	v.pins = d.linePinsForTest();
	v.rider = d.riderEngagedForTest();
	return v;
}

Settings varyingBase(float topo, float twist) {
	Settings s = defaults();
	s.keyTrack = false;
	s.topo = topo;
	s.twist = twist;
	s.stages = 32;
	s.freqLog2 = 6.0f;
	return s;
}

} // namespace

TEST_CASE("THE LATTICE IS THE FIRMWARE'S ALLPASS: held still it is the DF2T's to a millionth; moved, it gains nothing") {
	// The engine's stages are a normalized lattice (`runStage`), and the DF2T form the firmware uses is kept
	// behind `setDf2tForTest` as the reference. HELD STILL (the LFOs frozen, so no coefficient moves) the two
	// are one transfer function; MEASURED 2.4e-7 of full scale at worst, which is float rounding.
	for (const float topo : { 0.05f, 0.30f, 0.56f, 0.64f, 0.93f }) {
		for (const float freq : { 3.0f, 6.55f, 11.0f }) {
			for (const int stages : { 1, 17, 32 }) {
				Settings s = varyingBase(topo, 0.3f);
				s.freqLog2 = freq;
				s.stages = stages;
				std::vector<float> in = probe(24000, 0.5f, 48000.0);
				Disperser lattice;
				lattice.prepare(48000.0);
				lattice.setLfoFrozenForTest(true);
				lattice.setTarget(s);
				Disperser df2t;
				df2t.prepare(48000.0);
				df2t.setLfoFrozenForTest(true);
				df2t.setDf2tForTest(true);
				df2t.setTarget(s);
				const std::vector<float> a = render(lattice, in, 128);
				const std::vector<float> b = render(df2t, in, 128);
				double worst = 0.0;
				for (size_t i = 0; i < a.size(); ++i) {
					worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
				}
				CAPTURE(topo);
				CAPTURE(freq);
				CAPTURE(stages);
				CHECK(worst < 1e-6);
			}
		}
	}
	// MOVED — Stages automated between 17 and 32 every 53 ms, the chain alone (every write off): the DF2T
	// throws spikes (MEASURED +24.1 dBFS from a −10 dBFS input), the lattice stays within what the routing
	// can carry (MEASURED +4.6).
	const auto flip = [](Settings& s, int b, double) { s.stages = (b / 40) % 2 ? 32 : 17; };
	const Varying fixedForm = varyingRun(varyingBase(0.64f, 0.4812f), flip, false, true, 3.0);
	const Varying oldForm = varyingRun(varyingBase(0.64f, 0.4812f), flip, true, true, 3.0);
	CAPTURE(db(fixedForm.peak));
	CAPTURE(db(oldForm.peak));
	CHECK(db(fixedForm.peak) < 8.0);
	CHECK(db(oldForm.peak) > 16.0); // NON-VACUITY: the move this is about does pump the old form
}

TEST_CASE("the review's time-varying runaways: Stages automated 17/32 every 53 ms, and a 5 Hz ±3 oct Freq route — bounded, and the tail dies") {
	// MEASURED before the lattice: (a) Nested 0.64, Freq 2^6, Chirp 0.7 (below the cap) — pinned at +42 dBFS,
	// still +42 six to eight seconds after the input; (b) Parallel 0.56, Chirp 0.85+ — +37 dBFS, 8k-29k pins.
	for (const float twist : { 3.7f / 8.0f, 3.97f / 8.0f }) { // below the cap and at its top; 3.85 is in the MEASURE sweep
		const Varying a = varyingRun(varyingBase(0.64f, twist), [](Settings& s, int b, double) {
			s.stages = (b / 40) % 2 ? 32 : 17;
		});
		const Varying b = varyingRun(varyingBase(0.56f, twist), [](Settings& s, int, double t) {
			s.freqLog2 = static_cast<float>(6.0 + 3.0 * std::sin(t * 2.0 * std::numbers::pi * 5.0));
		});
		for (const Varying& v : { a, b }) {
			CAPTURE(twist);
			CAPTURE(db(v.peak));
			CAPTURE(db(v.justAfter));
			CAPTURE(db(v.late));
			CHECK(v.pins == 0);
			CHECK(db(v.peak) < 24.0);
			CHECK(db(v.late) < db(v.justAfter) - 20.0); // it dies
			CHECK(v.rider == 0); // and the lattice did it, not the safety net
		}
	}
}

TEST_CASE("THE SAFETY NET: the loop rider engages only near the line's ceiling, and there it takes most of the loop away") {
	// Asked on the OLD stage form (`setDf2tForTest`), the one place a runaway can still be made to happen:
	// the review's Stages automation at Chirp 0.75. Everywhere the engine really runs, the rider has nothing
	// to do — the cases above count it at zero. MEASURED: without it 46.6k pins at +42 dBFS; with it 4.0k at
	// +38 — a net, not a cure: the old form makes energy of its own and the lattice is what removes that.
	const auto flip = [](Settings& s, int b, double) { s.stages = (b / 40) % 2 ? 32 : 17; };
	const Varying with = varyingRun(varyingBase(0.64f, 0.4812f), flip, true, false, 8.0, false);
	const Varying without = varyingRun(varyingBase(0.64f, 0.4812f), flip, true, false, 8.0, true);
	CAPTURE(with.pins);
	CAPTURE(without.pins);
	CAPTURE(db(with.peak));
	CHECK(with.rider > 0);
	CHECK(without.rider == 0);
	CHECK(with.pins * 5 < without.pins);
}

namespace {

/// A HOT SINE THROUGH THE LOOP, the review's rider bench: 1 s at `dbIn` (10 ms ramps), then 2 s of silence.
struct HotSine {
	double clickProxy = 0.0; ///< max |2nd difference| / peak — how sharp the sharpest corner is
	uint64_t pins = 0;
	uint64_t rider = 0;
};
HotSine hotSine(float twist, float topo, float dbIn, float hz, bool riderOff) {
	Disperser d;
	d.prepare(48000.0);
	Settings s = defaults();
	s.twist = twist;
	s.topo = topo;
	s.keyTrack = false;
	s.freqLog2 = 9.0f;
	s.stages = 16;
	d.setTarget(s);
	d.setRiderOffForTest(riderOff);
	const int n = 48000 * 3;
	std::vector<float> l(static_cast<size_t>(n));
	const float a = std::pow(10.0f, dbIn / 20.0f);
	for (int i = 0; i < n; ++i) {
		const float env = i < 48000 ? std::min(1.0f, static_cast<float>(i) / 480.0f)
		                            : std::max(0.0f, 1.0f - static_cast<float>(i - 48000) / 480.0f);
		l[static_cast<size_t>(i)] = a * env * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * i / 48000.0));
	}
	std::vector<float> r = l;
	for (int i = 0; i < n; i += 256) {
		d.processStereo(l.data() + i, r.data() + i, std::min(256, n - i));
	}
	HotSine out;
	double peak = 0.0;
	double d2 = 0.0;
	for (size_t i = 2; i < l.size(); ++i) {
		peak = std::max(peak, std::abs(static_cast<double>(l[i])));
		d2 = std::max(d2, std::abs(static_cast<double>(l[i]) - 2.0 * l[i - 1] + l[i - 2]));
	}
	out.clickProxy = d2 / peak;
	out.pins = d.linePinsForTest();
	out.rider = d.riderEngagedForTest();
	return out;
}

} // namespace

TEST_CASE("THE RIDER IS CLICK-FREE: engaged, its sharpest corner is within 2x of the rider off — and the line still does not pin") {
	// A review MEASURED the first rider clicking: its gain followed an instant-attack envelope, so it stepped
	// sample by sample and shaped the loop's waveform — click proxy 0.0134 against 0.0008 with the rider off
	// (Chirp top, Parallel, 110 Hz at +30 dBFS), 0.18 against 0.087 (Punch, Cascade, +24 dBFS). The gain is a
	// one-pole now (2 ms down, 5 ms up). MEASURED after: 0.0009 and 0.085.
	struct Setting {
		float twist;
		float topo;
		float db;
	};
	for (const Setting& at : { Setting { 0.496f, 0.56f, 30.0f }, Setting { 0.244f, 0.1f, 24.0f } }) {
		const HotSine on = hotSine(at.twist, at.topo, at.db, 110.0f, false);
		const HotSine off = hotSine(at.twist, at.topo, at.db, 110.0f, true);
		CAPTURE(at.twist);
		CAPTURE(at.topo);
		CAPTURE(on.clickProxy);
		CAPTURE(off.clickProxy);
		REQUIRE(on.rider > 0); // NON-VACUITY: the rider is engaged here
		CHECK(on.clickProxy <= 2.0 * off.clickProxy);
		CHECK(on.pins == 0);
	}
	// AND IT STILL DOES ITS JOB: Chirp at its top, Cascade, +18 and +24 dBFS — MEASURED 6k-53k pins with the
	// rider off, none with it on.
	for (const float dbIn : { 18.0f, 24.0f }) {
		for (const float hz : { 110.0f, 1000.0f }) {
			const HotSine on = hotSine(0.496f, 0.1f, dbIn, hz, false);
			const HotSine off = hotSine(0.496f, 0.1f, dbIn, hz, true);
			CAPTURE(dbIn);
			CAPTURE(hz);
			CAPTURE(off.pins);
			CHECK(on.pins == 0);
			CHECK(off.pins > 1000); // NON-VACUITY: without it, this pins
		}
	}
}

TEST_CASE("a stage leaving the chain leaves silent, so a Stages automation brings it back empty") {
	// Not what fixed the runaways — the lattice did, and they stay fixed with this removed (known-bad 26) —
	// but a stage re-entering with the state it had when it left is a transient from the past played through
	// today's coefficients. Asserted as the state itself.
	Settings s = varyingBase(0.05f, 0.3f);
	Disperser d;
	d.prepare(48000.0);
	d.setTarget(s);
	(void)render(d, probe(4800, 0.5f, 48000.0), 64);
	REQUIRE(d.stageStateEnergyForTest(20) > 0.0); // NON-VACUITY: stage 20 holds something while it runs
	s.stages = 17;
	d.setTarget(s);
	(void)render(d, probe(640, 0.5f, 48000.0), 64);
	for (int i = 17; i < 32; ++i) {
		CAPTURE(i);
		CHECK(d.stageStateEnergyForTest(i) == 0.0);
	}
	CHECK(d.stageStateEnergyForTest(16) > 0.0); // and the ones still running were not touched
}

namespace {

/// THE TIME-VARYING SWEEP'S BODY: every routing × Chirp × Freq-route rate × depth given, a Stages automation
/// under every run, 2 s of noise then 3 s of silence.
void timeVaryingSweep(std::initializer_list<float> topos, std::initializer_list<float> twists,
                      std::initializer_list<double> rates, std::initializer_list<double> depths, double qOffset = 0.0,
                      double qLfoRate = 0.0) {
	int runs = 0;
	double hottest = -200.0;
	uint64_t riderSamples = 0;
	for (const float topo : topos) {
		for (const float twist : twists) {
			for (const double rate : rates) {
				for (const double depth : depths) {
					const Varying v = varyingRun(varyingBase(topo, twist), [rate, depth, qOffset, qLfoRate](Settings& s, int b, double t) {
						s.freqLog2 = static_cast<float>(6.5 + depth * std::sin(t * 2.0 * std::numbers::pi * rate));
						s.stages = (b / 31) % 3 == 0 ? 24 : 32; // and a Stages automation under it
						// THE Q OFFSET (2026-10-03): held at `qOffset`, or swept across its whole range at `qLfoRate`.
						s.qOffsetOctaves = static_cast<float>(
						    qLfoRate > 0.0 ? kQOffsetOctaves * std::sin(t * 2.0 * std::numbers::pi * qLfoRate) : qOffset);
					}, false, false, 5.0);
					CAPTURE(topo);
					CAPTURE(twist);
					CAPTURE(rate);
					CAPTURE(depth);
					CAPTURE(db(v.peak));
					CHECK(v.pins == 0);
					CHECK(db(v.peak) < 24.0);
					CHECK(db(v.late) <= db(v.justAfter) + 0.5); // no growth after the input
					hottest = std::max(hottest, db(v.peak));
					riderSamples += v.rider;
					++runs;
				}
			}
		}
	}
	MESSAGE(runs << " time-varying runs: hottest peak " << hottest << " dBFS, the rider engaged for " << riderSamples << " samples");
	CHECK(riderSamples == 0); // the lattice holds all of it; the safety net never had to move
}

} // namespace

TEST_CASE("THE TIME-VARYING SWEEP: a 5 and an 11 Hz ±3 oct Freq route with Stages automation, in four routings at the top of Chirp") {
	// The default run's representative: the fastest, deepest routes at the Chirp the review found them at.
	// The whole grid is the MEASURE case below.
	timeVaryingSweep({ 0.05f, 0.56f, 0.64f, 0.93f }, { 3.97f / 8.0f }, { 5.0, 11.0 }, { 3.0 });
}

TEST_CASE("THE Q OFFSET: the engine reads the native Q times 2^offset, up sharpens, and at zero the Q is the native one bit for bit") {
	// In the window, where x drags the offset, a y drag modifies Q, added to the native Q setting. The native Q is what Topo's zones and the Q pill make; the offset multiplies it by 2^octaves
	// and the engine's range holds it. Read off the engine's own block (its Q) and its stages (the per-stage Q the
	// coefficients were built from), against an INDEPENDENT restatement: the zones and the pills, recomputed here.
	const auto nativeQ = [](const Settings& s) {
		const Coordinate topoC = splitCoordinate(static_cast<double>(s.topo));
		const Coordinate twistC = splitCoordinate(static_cast<double>(s.twist));
		const double gammaTerm = kGammaScale * static_cast<double>(s.gamma);
		TwistParams twist = computeTwistParams(twistC.knob, twistC.lap + gammaTerm);
		TopoParams topo = computeTopoParams(topoC.knob, topoC.lap + static_cast<double>(s.topoLap) + gammaTerm,
		                                    twist.phaseOffset);
		applyPills(s, topo, twist);
		return topo.q;
	};
	const auto engineAt = [](const Settings& s) {
		Disperser d;
		d.prepare(kRef);
		d.setLfoFrozenForTest(true);
		d.setTarget(s);
		(void)render(d, probe(256, 0.25f), 256);
		return std::pair { d.blockForTest().topo.q, d.stageQForTest(0) };
	};
	int sharpened = 0;
	int clamped = 0;
	for (const uint32_t mode : { 0u, 2u, 5u, 7u }) {
		for (const double phase : { 0.0, 0.4375, 1.0, 3.25 }) {
			for (const float qPill : { 0.5f, 0.2f, 0.9f }) {
				Settings s = defaults();
				const TopoPlace p = topoPlaceOf(mode, phase);
				s.topo = static_cast<float>(p.coordinate);
				s.topoLap = static_cast<float>(p.lap);
				s.pillAmount[static_cast<size_t>(Pill::Q)] = qPill;
				s.stages = 1;
				s.pillOn[static_cast<size_t>(Pill::QTilt)] = false; // no tilt: the stage's Q is the chain's
				const double native = nativeQ(s);
				CAPTURE(mode);
				CAPTURE(phase);
				CAPTURE(qPill);
				CAPTURE(native);
				// AT ZERO: the native Q, to the bit, in the engine and in the plot's model.
				REQUIRE(s.qOffsetOctaves == 0.0f);
				CHECK(engineAt(s).first == native);
				CHECK(responseModelOf(s, false, 60.0, kRef).topo.q == native);
				for (const double octaves : { -3.0, -1.25, 0.5, 1.0, 3.0 }) {
					s.qOffsetOctaves = static_cast<float>(octaves);
					const double want = std::clamp(native * std::exp2(static_cast<double>(s.qOffsetOctaves)), kQMin, kQMax);
					CAPTURE(octaves);
					const auto [blockQ, stageQ] = engineAt(s);
					CHECK(blockQ == doctest::Approx(want).epsilon(1e-12));
					CHECK(stageQ == doctest::Approx(static_cast<double>(static_cast<float>(want))).epsilon(1e-6));
					CHECK(responseModelOf(s, false, 60.0, kRef).topo.q == doctest::Approx(want).epsilon(1e-12));
					// UP SHARPENS, DOWN BROADENS — wherever the range leaves room.
					if (octaves > 0.0 && native < kQMax) {
						CHECK(blockQ > native);
						++sharpened;
					}
					if (octaves < 0.0 && native > kQMin) {
						CHECK(blockQ < native);
					}
					clamped += (want == kQMin || want == kQMax) ? 1 : 0;
				}
				s.qOffsetOctaves = 0.0f;
			}
		}
	}
	CHECK(sharpened >= 30); // non-vacuity: most of the grid had room above
	CHECK(clamped > 0);     // and the range's ends were reached and held
	// THE DECLARATION: ±3 octaves, bipolar, default 0, modulatable, its own key.
	const device::ParamDesc& q = kParamDescs[static_cast<size_t>(Param::QOffset)];
	CHECK(std::string(q.key) == "disperser_q_offset");
	CHECK(q.min == -kQOffsetOctaves);
	CHECK(q.max == kQOffsetOctaves);
	CHECK(kQOffsetOctaves == 3.0);
	CHECK(q.def == 0.0);
	CHECK(q.modulatable);
	CHECK(q.centred);
}

TEST_CASE("THE Q OFFSET GLIDES: a jump in the offset reaches the engine's Q over blocks, as the coordinates do") {
	Settings s = defaults();
	s.stages = 1;
	Disperser d;
	d.prepare(kRef);
	d.setLfoFrozenForTest(true);
	d.setTarget(s);
	(void)render(d, probe(256, 0.25f), 256);
	const double native = d.blockForTest().topo.q;
	s.qOffsetOctaves = 2.0f;
	d.setTarget(s);
	(void)render(d, probe(64, 0.25f), 64);
	const double first = d.blockForTest().topo.q;
	CHECK(first > native);
	CHECK(first < std::clamp(native * 4.0, kQMin, kQMax) * 0.95); // not there yet
	(void)render(d, probe(192000, 0.25f), 64); // four seconds: the one-pole lands well inside it
	CHECK(d.blockForTest().topo.q == doctest::Approx(std::clamp(native * 4.0, kQMin, kQMax)).epsilon(1e-6)); // and it lands
}

TEST_CASE("THE Q OFFSET AT ITS ENDS: the review's runaways and the time-varying sweep at ±3 octaves of Q, and under a Q LFO across the whole range") {
	// The Q offset reaches the stages, and the stages are where the runaways lived. So the two runaway families,
	// held at each end of the offset's range, bounded and dying; then the time-varying sweep at both ends and
	// under a 7 Hz LFO sweeping it end to end.
	for (const double q : { -kQOffsetOctaves, kQOffsetOctaves }) {
		for (const float twist : { 3.7f / 8.0f, 3.97f / 8.0f }) {
			Settings nested = varyingBase(0.64f, twist);
			nested.qOffsetOctaves = static_cast<float>(q);
			Settings parallel = varyingBase(0.56f, twist);
			parallel.qOffsetOctaves = static_cast<float>(q);
			const Varying a = varyingRun(nested, [](Settings& s, int b, double) { s.stages = (b / 40) % 2 ? 32 : 17; });
			const Varying b = varyingRun(parallel, [](Settings& s, int, double t) {
				s.freqLog2 = static_cast<float>(6.0 + 3.0 * std::sin(t * 2.0 * std::numbers::pi * 5.0));
			});
			for (const Varying& v : { a, b }) {
				CAPTURE(q);
				CAPTURE(twist);
				CAPTURE(db(v.peak));
				CAPTURE(db(v.justAfter));
				CAPTURE(db(v.late));
				CHECK(v.pins == 0);
				CHECK(db(v.peak) < 24.0);
				CHECK(db(v.late) < db(v.justAfter) - 20.0); // it dies
				CHECK(v.rider == 0);
			}
		}
	}
	timeVaryingSweep({ 0.05f, 0.56f, 0.64f, 0.93f }, { 3.97f / 8.0f }, { 5.0, 11.0 }, { 3.0 }, -kQOffsetOctaves);
	timeVaryingSweep({ 0.05f, 0.56f, 0.64f, 0.93f }, { 3.97f / 8.0f }, { 5.0, 11.0 }, { 3.0 }, kQOffsetOctaves);
	timeVaryingSweep({ 0.05f, 0.56f, 0.64f, 0.93f }, { 3.97f / 8.0f }, { 5.0, 11.0 }, { 3.0 }, 0.0, 7.0);
}

TEST_CASE("MEASURE: THE TIME-VARYING SWEEP — four rates × two depths × four routings × two Chirps" * doctest::skip()) {
	// WHAT: Freq routes at 0.5, 2, 5 and 11 Hz, ±1 and ±3 octaves, with a Stages automation under each, over
	// Cascade, Parallel, Nested and Spring at Chirp 0.7 and 0.99 — 64 runs (~50 s under load). WHEN: before
	// touching the stages' form, the coefficient update, the cap or the rider. MEASURED 2026-09-26: no pins,
	// hottest +17.9 dBFS, no growth, the rider at 0 samples.
	timeVaryingSweep({ 0.05f, 0.56f, 0.64f, 0.93f }, { 3.7f / 8.0f, 3.97f / 8.0f }, { 0.5, 2.0, 5.0, 11.0 }, { 1.0, 3.0 });
}

namespace {

/// THE ARTIFACT METRICS (many disperser modes were noisy, with bad-sounding artefacts). A sine through the device, at 44.1 kHz so the transcription runs at its own rate:
///  - `residualDb` — what is NOT the tone, after a least-squares sinusoid at the input's frequency is taken out
///    of every 50 ms (a linear, time-invariant allpass chain leaves only the tone), under the output, in dB;
///  - `click` — the sharpest corner, max |2nd difference|, over an ideal sine's at the same level;
///  - `highBand` — the 2nd difference's energy over the input's, in dB: where a stepped coefficient's zipper
///    lives.
struct Artifacts {
	double residualDb = 0.0;
	double click = 0.0;
	double highBand = 0.0;
};

Artifacts artifactsOf(const std::vector<float>& y, const std::vector<float>& x, double hz, double amp, size_t from) {
	Artifacts a;
	const auto win = static_cast<size_t>(kRef * 0.05);
	double res = 0.0;
	double tot = 0.0;
	for (size_t at = from; at + win <= y.size(); at += win) {
		double ss = 0.0, sc = 0.0, cc = 0.0, ys = 0.0, yc = 0.0;
		for (size_t i = at; i < at + win; ++i) {
			const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRef;
			ss += std::sin(w) * std::sin(w);
			cc += std::cos(w) * std::cos(w);
			sc += std::sin(w) * std::cos(w);
			ys += y[i] * std::sin(w);
			yc += y[i] * std::cos(w);
		}
		const double det = ss * cc - sc * sc;
		const double s = (ys * cc - yc * sc) / det;
		const double c = (yc * ss - ys * sc) / det;
		for (size_t i = at; i < at + win; ++i) {
			const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRef;
			const double e = y[i] - (s * std::sin(w) + c * std::cos(w));
			res += e * e;
			tot += static_cast<double>(y[i]) * y[i];
		}
	}
	a.residualDb = 10.0 * std::log10(res / std::max(tot, 1e-30) + 1e-30);
	double maxD2 = 0.0;
	double eY = 0.0;
	double eX = 0.0;
	for (size_t i = std::max<size_t>(from, 2); i < y.size(); ++i) {
		const double dy = static_cast<double>(y[i]) - 2.0 * y[i - 1] + y[i - 2];
		const double dx = static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2];
		maxD2 = std::max(maxD2, std::abs(dy));
		eY += dy * dy;
		eX += dx * dx;
	}
	const double w = 2.0 * std::numbers::pi * hz / kRef;
	a.click = maxD2 / (amp * w * w);
	a.highBand = 10.0 * std::log10(eY / eX);
	return a;
}

std::vector<float> tone(double hz, double amp, size_t frames) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRef));
	}
	return v;
}

/// THE PORT AND THE TRANSCRIPTION (the port's options) UNDER ONE SCRIPT of settings, block by block.
template <typename Script>
std::pair<Artifacts, Artifacts> artifactsBoth(Script script, double hz, size_t frames, size_t from) {
	const double amp = std::pow(10.0, -12.0 / 20.0);
	const std::vector<float> x = tone(hz, amp, frames);
	Disperser d;
	d.prepare(kRef);
	std::vector<float> l = x;
	std::vector<float> r = x;
	fwdisp::Fw fw;
	fw.opt = portOptions();
	std::vector<fwdisp::q31_t> ql(frames);
	for (size_t i = 0; i < frames; ++i) {
		ql[i] = fwdisp::cvt(static_cast<float>(static_cast<double>(x[i]) * fwdisp::kZeroDbfs));
	}
	std::vector<fwdisp::q31_t> qr = ql;
	for (size_t at = 0; at < frames; at += 128) {
		const Settings s = script(static_cast<double>(at) / static_cast<double>(frames));
		const auto n = static_cast<int>(std::min<size_t>(128, frames - at));
		d.setTarget(s);
		d.processStereo(l.data() + at, r.data() + at, n);
		const double tl = std::floor(s.topo);
		const double wl = std::floor(s.twist);
		fw.topoValue = static_cast<fwdisp::q31_t>((s.topo - tl) * 2147483647.0);
		fw.topoPhaseOffset = static_cast<float>(tl);
		fw.twistValue = static_cast<fwdisp::q31_t>((s.twist - wl) * 2147483647.0);
		fw.twistPhaseOffset = static_cast<float>(wl);
		fw.stages = static_cast<uint8_t>(s.stages);
		fw.effectiveFreqOverride = static_cast<double>(s.freqLog2) * 127.0 / 13.0;
		fw.process(ql.data() + at, qr.data() + at, static_cast<size_t>(n), -1);
	}
	std::vector<float> y(frames);
	for (size_t i = 0; i < frames; ++i) {
		y[i] = static_cast<float>(static_cast<double>(ql[i]) / fwdisp::kZeroDbfs);
	}
	return { artifactsOf(l, x, hz, amp, from), artifactsOf(y, x, hz, amp, from) };
}

Settings modeSettings(uint32_t topoMode, uint32_t twistMode, double pos, float freqLog2, int stages) {
	Settings s = defaults();
	s.keyTrack = false;
	s.freqLog2 = freqLog2;
	s.stages = stages;
	s.topo = static_cast<float>(topoCoordinateOf(topoMode, pos));
	s.twist = static_cast<float>(twistCoordinateOf(twistMode, pos, pos));
	return s;
}

} // namespace

TEST_CASE("ARTIFACTS, HELD: in every Topo × Twist mode a sine comes out no noisier than the transcription's") {
	// MEASURED 2026-09-26 over every mode at three positions and two tones: the port within 1 dB of the
	// transcription everywhere the transcription is itself clean, and the residual that remains is the stereo
	// LFOs' own motion (frozen, it falls from -82 to -103 dB on average) — the firmware's design, kept.
	double worst = -200.0;
	for (uint32_t tz = 0; tz < 8; ++tz) {
		for (uint32_t wz = 0; wz < 6; ++wz) {
			const auto [port, firmware] = artifactsBoth(
			    [&](double) { return modeSettings(tz, wz, 0.5, static_cast<float>(64.0 * 13.0 / 127.0), 8); }, 110.0,
			    static_cast<size_t>(kRef * 1.5), static_cast<size_t>(kRef * 0.5));
			CAPTURE(tz);
			CAPTURE(wz);
			CAPTURE(port.residualDb);
			CAPTURE(firmware.residualDb);
			CHECK(port.residualDb <= std::max(firmware.residualDb, -90.0) + 1.5);
			CHECK(port.click <= firmware.click * 1.25 + 0.25);
			worst = std::max(worst, port.residualDb - firmware.residualDb);
		}
	}
	MESSAGE("port against the transcription, 48 modes held: worst residual difference " << worst << " dB");
}

TEST_CASE("ARTIFACTS, MOVING: a Freq sweep and a Topo position sweep do not zipper — the lattice ramps where the firmware steps") {
	// MEASURED 2026-09-26, 110 Hz at -12 dBFS, 4 s: a Freq sweep 2^4 to 2^12 put the stepped lattice at 390x the
	// ideal sine's sharpest corner and +17 dB in the high band, against the transcription's 221x and +9 dB — the
	// first lattice (the runaway fix) stepped its coefficients as the DF2T does, and stepped, it zippers WORSE.
	// Ramped (`advanceLattice`): 33x and +0.6 dB. The Topo position sweep in Parallel: 157x / +9 dB stepped, 68x /
	// +3 dB ramped, the transcription's 68x / +3 dB.
	const size_t frames = 128 * 1378;
	const auto freqSweep = [](double t) { return modeSettings(4, 0, 0.5, static_cast<float>(4.0 + 8.0 * t), 16); };
	const auto topoSweep = [](double t) {
		Settings s = modeSettings(4, 0, 0.5, 7.0f, 16);
		s.topo = static_cast<float>(topoCoordinateOf(4, t));
		return s;
	};
	const std::pair<Artifacts, Artifacts> freq = artifactsBoth(freqSweep, 110.0, frames, 0);
	const std::pair<Artifacts, Artifacts> topo = artifactsBoth(topoSweep, 110.0, frames, 0);
	for (const auto& [name, both] : { std::pair { "Freq sweep", freq }, std::pair { "Topo sweep", topo } }) {
		const Artifacts& port = both.first;
		const Artifacts& firmware = both.second;
		CAPTURE(name);
		CAPTURE(port.click);
		CAPTURE(firmware.click);
		CAPTURE(port.highBand);
		CAPTURE(firmware.highBand);
		CHECK(port.click <= firmware.click * 1.05);
		CHECK(port.highBand <= firmware.highBand + 0.5);
	}
	// **AND THE TWO THE FIRMWARE CLICKS ON ITSELF (the bad-sounding artefacts)** — MEASURED at 110 Hz:
	// stepping the Topo mode through all eight routings, 3,700x the ideal sine's corner in the first port and
	// 4,200x in the transcription (a one-sample rewire, an eighth of every state kept); turning Twist's phase
	// through four laps, 5,200x in both (the delay road's gains stepping once a block, and "drain mode" reading
	// the head whole the moment Punch and Chirp fall under 0.01). Crossfaded (`route`) and glided
	// (`processSample`): 140x and 97x — the Topo position sweep's own level, a thirtieth of the firmware's.
	const auto topoModes = [](double t) {
		return modeSettings(static_cast<uint32_t>(std::min(7.0, t * 8.0)), 0, 0.5, 7.0f, 16);
	};
	const auto twistTurn = [](double t) {
		Settings s = modeSettings(4, 0, 0.5, 7.0f, 16);
		s.twist = static_cast<float>(twistCoordinateOf(kTwistInfinite, 0.0, 4.0 * t));
		return s;
	};
	const size_t skipOnset = static_cast<size_t>(kRef * 0.3);
	for (const auto& [name, both] : { std::pair { "Topo modes", artifactsBoth(topoModes, 110.0, frames, skipOnset) },
	                                  std::pair { "Twist phase", artifactsBoth(twistTurn, 110.0, frames, skipOnset) } }) {
		CAPTURE(name);
		CAPTURE(both.first.click);
		CAPTURE(both.second.click);
		CHECK(both.first.click * 10.0 <= both.second.click);
		CHECK(both.first.click < 250.0);
	}
}

TEST_CASE("TOPO'S INFINITE PHASE: 0 to 1 is round 2's position, and past it the firmware's own phase offset, sample for sample") {
	// Round 3: Topo's value became an unbounded phase — the firmware's in-zone position plus
	// `topoPhaseOffset`, which the reference drove from the hidden "dispMetaTopo" encoder. So the TRANSCRIPTION can
	// say what every phase should sound like: its knob where `topoPlaceOf` rests it, its offset the lap. The
	// port is built through the CODEC (the stored mode and phase), so what is compared is what a patch plays.
	double worst = 0.0;
	int compared = 0;
	for (uint32_t mode = 0; mode < 8; ++mode) {
		for (const double phase : { 0.0, 0.3125, 0.75, 1.0, 1.5, 7.25, -2.5, 50.5, -49.75 }) {
			Row row = rowOf(State {});
			row[static_cast<size_t>(Param::TopoMode)] = static_cast<float>(mode);
			row[static_cast<size_t>(Param::TopoPhase)] = static_cast<float>(phase);
			const Settings coded = stateOf(row, static_cast<size_t>(Param::kCount)).settings;
			CAPTURE(mode);
			CAPTURE(phase);
			// FROM 0 TO 1, ROUND 2 EXACTLY: the fenced position's coordinate, and no lap.
			if (phase >= 0.0 && phase <= 1.0) {
				CHECK(coded.topo == static_cast<float>(topoCoordinateOf(mode, phase)));
				CHECK(coded.topoLap == 0.0f);
			}
			// THE ROUTING IS THE MODE'S at every phase.
			CHECK(zoneOf(splitCoordinate(static_cast<double>(coded.topo)).knob).index == static_cast<int>(mode));
			Case c;
			c.topo = static_cast<double>(coded.topo);
			c.topoLap = static_cast<double>(coded.topoLap);
			c.twist = 1.5 / 8.0; // Punch: the delay road, once through every routing
			c.stages = 16;
			c.freqKnob = kHighKnob; // the stages up, where "sample for sample" holds (`kExact`'s note)
			c.frames = 8820;
			const Compared r = compare(c);
			CAPTURE(r.maxErr);
			REQUIRE(r.fwRms > 1e-3);
			CHECK(r.fwPins == 0);
			CHECK(std::abs(db(r.portRms / r.fwRms)) < 0.05);
			// TO THE SAMPLE (`kExact`) — bar two places, MEASURED and bounded at 1e-3: the top edge of a zone
			// (phase 1; Cascade 1.28e-4 — a round 2 coordinate, the same `Settings` round 2 built, asserted above)
			// and fifty laps out (Diffuse at 50.5, 3.98e-4), where the transcription carries the offset in float
			// and the port in double. Both still hold the level to 0.05 dB.
			const bool edge = phase == 1.0 || std::abs(phase) > 8.0;
			CHECK(r.maxErr < (edge ? 1e-3 : kExact));
			worst = std::max(worst, r.maxErr);
			++compared;
		}
	}
	MESSAGE(compared << " Topo phases against the transcription, worst sample error " << worst);
	// NON-VACUITY: the phase past the zone moves the engine's Topo quantities (it is not a lap nobody reads).
	const auto topoAt = [](double phase) {
		Settings s = defaults();
		const TopoPlace p = topoPlaceOf(3, phase);
		s.topo = static_cast<float>(p.coordinate);
		s.topoLap = static_cast<float>(p.lap);
		Disperser d;
		d.prepare(kRef);
		d.setTarget(s);
		(void)render(d, probe(512, 0.25f), 512);
		return d.blockForTest().topo;
	};
	const TopoParams one = topoAt(1.0);
	const TopoParams past = topoAt(1.37);
	CHECK(past.zone == one.zone);
	CHECK(past.q != doctest::Approx(one.q).epsilon(1e-3));
	CHECK(past.param0 != doctest::Approx(one.param0).epsilon(1e-3));
}

TEST_CASE("TOPO'S PHASE FAR OUT: at ±50 laps, held and under a 5 and an 11 Hz phase LFO, the routing holds and the output is bounded") {
	// Round 3: the routing is the dropdown's however far the phase turns, and a phase that turns fast (an LFO on
	// it, ±3 laps) at the top of Chirp with 32 stages is bounded and dies, as the Freq sweep is.
	const auto place = [](Settings& s, uint32_t mode, double phase) {
		const TopoPlace p = topoPlaceOf(mode, phase);
		s.topo = static_cast<float>(p.coordinate);
		s.topoLap = static_cast<float>(p.lap);
	};
	// THE ROUTING, READ OFF THE ENGINE'S OWN BLOCK every block of a phase LFO around ±50 laps.
	for (uint32_t mode = 0; mode < 8; ++mode) {
		for (const double centre : { -50.0, 50.0 }) {
			Settings s = varyingBase(0.0f, 3.97f / 8.0f);
			Disperser d;
			d.prepare(kRef);
			const std::vector<float> in = probe(256, 0.5f);
			bool held = true;
			for (int b = 0; b < 200; ++b) {
				place(s, mode, centre + 3.0 * std::sin(b * 0.37));
				d.setTarget(s);
				std::vector<float> l = in;
				std::vector<float> r = in;
				d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
				held = held && d.blockForTest().topo.zone == static_cast<int>(mode);
			}
			CAPTURE(mode);
			CAPTURE(centre);
			CHECK(held);
		}
	}
	// BOUNDED, AND IT DIES: four routings (Cascade, Owlpass, Parallel, Spring), both centres, both rates.
	int runs = 0;
	for (const uint32_t mode : { 0u, 2u, 4u, 7u }) {
		for (const double centre : { -50.0, 50.0 }) {
			for (const double rate : { 5.0, 11.0 }) {
				Settings base = varyingBase(0.0f, 3.97f / 8.0f);
				place(base, mode, centre);
				const Varying v = varyingRun(base, [&](Settings& s, int, double t) {
					place(s, mode, centre + 3.0 * std::sin(t * 2.0 * std::numbers::pi * rate));
				}, false, false, 5.0);
				CAPTURE(mode);
				CAPTURE(centre);
				CAPTURE(rate);
				CAPTURE(db(v.peak));
				CHECK(v.pins == 0);
				CHECK(db(v.peak) < 24.0);
				CHECK(db(v.late) <= db(v.justAfter) + 0.5); // no growth after the input
				CHECK(v.rider == 0);
				++runs;
			}
		}
	}
	CHECK(runs == 16);
}

TEST_CASE("the mode cells: no value leaves its mode's zone, a route cannot carry it out, and Topo's and Twist's phases turn on through the laps") {
	// Increasing the offset never changes the zone dropdown's entry; Twist is infinite and the others are
	// fenced. Asked the ENGINE's way — the coordinate the codec builds, split into its lap
	// and q31 knob, then `zoneOf` — after a float carries it, across every mode and position, and through
	// `device::withRouteOffset` (the resolve every host uses) at full depth both ways.
	const auto engine = [](double c) {
		const Coordinate k = splitCoordinate(static_cast<double>(static_cast<float>(c)));
		return std::pair { static_cast<long>(k.lap), zoneOf(k.knob).index };
	};
	const device::ParamDesc& topoPhase = kParamDescs[static_cast<size_t>(Param::TopoPhase)];
	// TOPO'S PLACE (round 3): the routing from the coordinate, and the lap the phase adds past its zone.
	const auto topoEngine = [&](uint32_t mode, double phase) {
		const TopoPlace p = topoPlaceOf(mode, phase);
		return engine(p.coordinate);
	};
	const device::ParamDesc& twistPos = kParamDescs[static_cast<size_t>(Param::TwistPos)];
	CHECK_FALSE(kParamDescs[static_cast<size_t>(Param::TopoMode)].modulatable); // the routing moves only by hand
	CHECK_FALSE(kParamDescs[static_cast<size_t>(Param::TwistMode)].modulatable);
	for (uint32_t mode = 0; mode < 8; ++mode) {
		for (int i = 0; i <= 64; ++i) {
			const double pos = i / 64.0;
			CAPTURE(mode);
			CAPTURE(pos);
			CHECK(engine(topoCoordinateOf(mode, pos)) == std::pair { 0L, static_cast<int>(mode) });
			// FROM 0 TO 1 TOPO'S PHASE IS ROUND 2'S FENCED POSITION, EXACTLY: the same coordinate and no lap.
			CHECK(topoPlaceOf(mode, pos).coordinate == topoCoordinateOf(mode, pos));
			CHECK(topoPlaceOf(mode, pos).lap == 0.0);
			for (const double amount : { -1.0, 1.0 }) {
				// A FULL-DEPTH ROUTE ON TOPO'S PHASE carries it out of 0..1 — and the routing is still the mode's.
				const double routed = device::withRouteOffset(topoPhase, pos, amount * device::routeSpanOf(topoPhase));
				CHECK(routed == doctest::Approx(pos + amount * 1.0));
				CHECK(topoEngine(mode, routed) == std::pair { 0L, static_cast<int>(mode) });
				if (mode < kTwistInfinite) {
					const double r2 = device::withRouteOffset(twistPos, pos, amount * device::routeSpanOf(twistPos));
					CHECK(engine(twistCoordinateOf(mode, r2, 0.0)) == std::pair { 0L, static_cast<int>(mode) });
				}
			}
		}
	}
	// TOPO'S PHASE FAR OUT, BOTH WAYS: the routing is the mode's and the laps are the offset's.
	for (uint32_t mode = 0; mode < 8; ++mode) {
		for (const double phase : { -50.25, -3.5, -0.01, 1.01, 2.5, 50.75, 1000.125 }) {
			CAPTURE(mode);
			CAPTURE(phase);
			CHECK(topoEngine(mode, phase) == std::pair { 0L, static_cast<int>(mode) });
			CHECK(topoPlaceOf(mode, phase).lap == doctest::Approx(phase - std::clamp(phase, 0.0, 1.0)));
		}
	}
	CHECK(topoPhase.min < -1000.0);
	CHECK(topoPhase.max > 1000.0);
	// TWIST'S PHASE: zero is the sixth zone, where the combined evolution starts, and it keeps turning.
	CHECK(engine(twistCoordinateOf(kTwistInfinite, 0.0, 0.0)) == std::pair { 0L, 5 });
	CHECK(engine(twistCoordinateOf(kTwistInfinite, 0.0, 0.2)) == std::pair { 0L, 6 });
	CHECK(engine(twistCoordinateOf(kTwistInfinite, 0.0, 1.0)) == std::pair { 1L, 5 });
	CHECK(engine(twistCoordinateOf(kTwistInfinite, 0.0, 12.5)) == std::pair { 13L, 1 });
	CHECK(twistCoordinateOf(kTwistInfinite, 0.0, -3.0) == doctest::Approx(kTwistInfiniteStart)); // it does not go below
	// AND THE RETIRED KEYS ARE RETIRED.
	// the two coordinates; round 3's Topo position and the pills' old powers
	CHECK(std::size(kRetiredKeys) == 3 + kPillCount);
	CHECK(std::string(kRetiredKeys[0].key) == "disperser_topo");
}

TEST_CASE("the review's two runaways: Nested at 0.64 and Spring at -0.0203, top of Chirp, 32 stages — bounded, and the tail dies") {
	// MEASURED on the first cap: +31 to +39 dBFS, the line pinned 350k-750k times, and still that loud ten
	// seconds after the input stopped. Two seconds of noise here, then five of silence (the late window is 4-5 s
	// after); two Freqs, the ends of the four the review ran.
	for (const float topo : { 0.64f, -0.0203f }) {
		for (const float freq : { 6.0f, 12.0f }) {
			Settings s = defaults();
			s.keyTrack = false;
			s.topo = topo;
			s.twist = 0.4938f;
			s.stages = 32;
			s.freqLog2 = freq;
			Disperser d;
			d.prepare(48000.0);
			d.setTarget(s);
			const std::vector<float> out = render(d, noiseBurst(96000, 336000), 512);
			double peak = 0.0;
			double justAfter = 0.0; // the first second after the input
			double late = 0.0;      // 4 to 5 s after
			for (size_t i = 0; i < out.size(); ++i) {
				const double v = std::abs(static_cast<double>(out[i]));
				peak = std::max(peak, v);
				if (i >= 96000 && i < 144000) {
					justAfter = std::max(justAfter, v);
				}
				if (i >= 96000 + 192000) {
					late = std::max(late, v);
				}
			}
			CAPTURE(topo);
			CAPTURE(freq);
			CAPTURE(db(peak));
			CAPTURE(db(late));
			CHECK(d.linePinsForTest() == 0);
			CHECK(db(peak) < 24.0);
			// IT DIES: the late window is well under the first second after the input (MEASURED: 11 to 47 dB).
			CHECK(db(late) < db(justAfter) - 6.0);
		}
	}
}

TEST_CASE("the rest clears the audio state ONCE per silence, not on every quiet sample after it") {
	// A review MEASURED the first draft clearing on every quiet sample past the window — one second of silence
	// cost 0.17-0.57 s of CPU at 48 kHz, 0.50-0.79 s at 96 kHz. Counted here through a seam.
	Settings s = defaults();
	s.twist = 0.4f; // Chirp: something in the line to clear
	Disperser d;
	d.prepare(48000.0);
	d.setTarget(s);
	std::vector<float> in = probe(24000, 0.5f, 48000.0);
	in.resize(48000 * 40, 0.0f);
	(void)render(d, in, 512);
	CHECK(d.restClearsForTest() == 1);
	// A second burst and a second silence: one more, and only one.
	std::vector<float> again = probe(24000, 0.5f, 48000.0);
	again.resize(48000 * 40, 0.0f);
	(void)render(d, again, 512);
	CHECK(d.restClearsForTest() == 2);
}

TEST_CASE("every supported rate: the Q16 delay does not overflow, and the write's second cell never lands on the head") {
	// A review found both: `uint32(folded * 65536)` passes 2^32 above ~327 kHz (undefined behaviour), and at
	// 22.05 kHz the longest fold, 4409.5 samples, put `offset + 1` on the head of a 4410-sample line — where
	// the punch write, made before the head is read, came straight back out.
	for (const double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 }) {
		CAPTURE(rate);
		Disperser d;
		d.prepare(rate);
		// THE FOLD'S WHOLE OUTPUT RANGE, from the pure function, against the line the device allocated.
		// Its longest output comes from an input just under the top of the range, left where it is (MEASURED:
		// 4409.45 at 22.05 kHz, 8818.90 at 44.1k) — so that stretch is walked densely, the rest coarsely.
		double longest = 0.0;
		for (int k = 0; k <= 4000; ++k) {
			longest = std::max(longest, foldedDelaySamples(rate * 0.001 * std::pow(2000.0, k / 4000.0), rate));
		}
		const double top = rate * 8819.0 / 44100.0;
		for (int k = 0; k <= 20000; ++k) {
			longest = std::max(longest, foldedDelaySamples(top - 2.0 + 2.0 * k / 20000.0, rate));
		}
		CHECK(longest > top - 1.0); // non-vacuity: the walk reached the top of the fold
		CHECK(static_cast<size_t>(longest) + 1 < d.lineSizeForTest());
		// AND THE ENGINE AT THE RATE: punch and chirp live, the Q16 delay what the block says it is.
		Settings s = defaults();
		s.twist = 0.2f; // Punch
		// ONE STAGE, AT THE FREQ WHOSE ECHO FOLDS LONGEST — FOUND ON THE ENGINE, one block per candidate, since
		// the centre stage's hertz carries the spread, the detune and the stereo offset as well as the knob.
		// The point is to pass the 170.7 ms where a 32-bit Q16 wraps at 384 kHz.
		s.keyTrack = false;
		s.stages = 1;
		float bestFreq = 0.0f;
		double bestFold = 0.0;
		for (int k = 0; k <= 520; ++k) {
			Settings t = s;
			t.freqLog2 = static_cast<float>(k) / 40.0f;
			Disperser probeDevice;
			probeDevice.prepare(rate);
			probeDevice.setTarget(t);
			std::vector<float> one(256, 0.1f);
			(void)render(probeDevice, one, 256);
			if (probeDevice.blockForTest().foldedDelaySamples > bestFold) {
				bestFold = probeDevice.blockForTest().foldedDelaySamples;
				bestFreq = t.freqLog2;
			}
		}
		s.freqLog2 = bestFreq;
		d.setTarget(s);
		const size_t frames = static_cast<size_t>(rate * 0.3);
		std::vector<float> in = probe(frames, 0.5f, rate);
		const std::vector<float> out = render(d, in, 256);
		const double folded = d.blockForTest().foldedDelaySamples;
		REQUIRE(folded > 0.0);
		CHECK(static_cast<double>(d.currentDelayQ16ForTest()) / 65536.0 == doctest::Approx(folded).epsilon(1e-3));
		CHECK(folded > rate * 0.1707); // non-vacuity: past where 32 bits of Q16 run out at 384 kHz
		for (const float v : out) {
			REQUIRE(std::isfinite(v));
		}
	}
}

TEST_CASE("the stage gains' doubling is NOT the same defect: the emphasis gain is stored halved") {
	// `processRouting*`'s `vshl(vqrdmulh(proc, stageGains_[i]), 1)` is the idiom the delay write copied —
	// and here it is right, because `stageGains_` holds `gain * ONE_Q31 / 2`. Unity is ONE_Q31 / 2, and a
	// full stage at unity passes its input at unity: asserted on the transcription.
	fwdisp::Fw fw;
	CHECK(fw.stageGains[0] == fwdisp::ONE_Q31 / 2);
	fwdisp::q31_t p[2] = { 123456789, -98765432 };
	fw.gainStage(p, 0);
	CHECK(std::abs(p[0] - 123456789) <= 2);
	CHECK(std::abs(p[1] + 98765432) <= 2);
}


//--------------------------------------------------------------------------------------------------//
// THE DEVICE'S OWN BEHAVIOUR.                                                                        //
//--------------------------------------------------------------------------------------------------//


TEST_CASE("a partition does not change the samples WHILE A CONTROL IS TRAVELLING") {
	// The Eroder's lesson: the partition case must move a control first, or a once-per-call control path
	// passes it. Here Topo, Twist and Freq move after the first block and glide for the rest.
	const std::vector<float> in = probe(44100 / 2, 0.5f);
	std::vector<std::vector<float>> outs;
	for (const int chunk : { 1, 17, 128, 1000, 22050 }) {
		Disperser d;
		d.prepare(kRef);
		Settings s = defaults();
		s.keyTrack = false;
		d.setTarget(s);
		std::vector<float> l(in.begin(), in.begin() + 64);
		std::vector<float> r = l;
		d.processStereo(l.data(), r.data(), 64);
		s.topo = 0.9f;
		s.twist = 0.43f;
		s.freqLog2 = 10.0f;
		d.setTarget(s);
		outs.push_back(render(d, std::vector<float>(in.begin() + 64, in.end()), chunk));
		CAPTURE(chunk);
		CHECK(std::abs(d.smoothedTopoForTest() - 0.9) > 1e-6); // still travelling at the end: the case asks something
	}
	for (size_t k = 1; k < outs.size(); ++k) {
		CAPTURE(k);
		CHECK(outs[k] == outs[0]);
	}
}

TEST_CASE("the first block snaps to the controls; after it a move glides 3% per firmware buffer, as a TIME") {
	for (const double rate : { 44100.0, 96000.0 }) {
		Disperser d;
		d.prepare(rate);
		Settings s = defaults();
		s.topo = 0.25f;
		d.setTarget(s);
		std::vector<float> l(static_cast<size_t>(d.blockFramesForTest()), 0.1f);
		std::vector<float> r = l;
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		CHECK(d.smoothedTopoForTest() == doctest::Approx(0.25)); // snapped, not glided up from zero
		s.topo = 1.25f;
		d.setTarget(s);
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		CAPTURE(rate);
		CHECK(d.smoothedTopoForTest() == doctest::Approx(0.25 + 1.0 * 0.03));
	}
	// THE BLOCK IS A TIME: 128 frames at 44.1 kHz, 279 at 96 kHz (2.9 ms both).
	Disperser a;
	a.prepare(44100.0);
	CHECK(a.blockFramesForTest() == 128);
	Disperser b;
	b.prepare(96000.0);
	CHECK(b.blockFramesForTest() == 279);
}

TEST_CASE("at 96 kHz the stages sit at the same hertz and the line holds the same 200 ms") {
	Settings s = defaults();
	s.keyTrack = false;
	s.topo = 0.1f;
	s.twist = 0.02f;
	s.stages = 16;
	std::vector<double> hz44;
	for (const double rate : { 44100.0, 96000.0 }) {
		Disperser d;
		d.prepare(rate);
		d.setTarget(s);
		std::vector<float> l(4096, 0.0f);
		std::vector<float> r = l;
		d.processStereo(l.data(), r.data(), 1); // one block's control path: the first coefficients
		CHECK(static_cast<double>(d.lineLengthForTest()) == doctest::Approx(0.2 * rate).epsilon(1e-3));
		for (int i = 0; i < 16; ++i) {
			if (rate == 44100.0) {
				hz44.push_back(d.stageHzForTest(i, 0));
			}
			else {
				CHECK(d.stageHzForTest(i, 0) == doctest::Approx(hz44[static_cast<size_t>(i)]).epsilon(1e-6));
			}
		}
	}
}

TEST_CASE("reset is as-if-just-switched-on") {
	const std::vector<float> in = probe(20000, 0.5f);
	Settings s = defaults();
	s.twist = 3.5f / 8.0f; // Chirp: the line fills, so a reset has something to forget
	Disperser fresh;
	fresh.prepare(kRef);
	fresh.setTarget(s);
	const std::vector<float> want = render(fresh, in, 333);
	Disperser used;
	used.prepare(kRef);
	used.setTarget(s);
	(void)render(used, probe(7000, 0.9f), 77);
	used.reset();
	CHECK(render(used, in, 333) == want);
}

TEST_CASE("Enabled off leaves the buffer untouched") {
	Disperser d;
	d.prepare(kRef);
	Settings s = defaults();
	s.enabled = false;
	d.setTarget(s);
	const std::vector<float> in = probe(4000, 0.5f);
	CHECK(render(d, in, 256) == in);
}

TEST_CASE("the note: absolute until one is heard, then the centre follows it, and a lifted key does not un-tune it") {
	Disperser d;
	d.prepare(kRef);
	Settings s = defaults(); // Key Track on, Offset 0
	d.setTarget(s);
	std::vector<float> l(512, 0.1f);
	std::vector<float> r = l;
	// NO NOTE YET: the Offset knob read absolutely — 0 octaves is knob 64, 93.8 Hz.
	d.setNoteContext({ 60.0f, false });
	d.processStereo(l.data(), r.data(), 512);
	CHECK_FALSE(d.heardNoteForTest());
	CHECK(d.blockForTest().freqIndex == doctest::Approx(64.0));
	// A NOTE: A4. The centre goes to 440 Hz.
	d.setNoteContext({ 69.0f, true });
	d.processStereo(l.data(), r.data(), 512);
	CHECK(d.heardNoteForTest());
	const double a4 = std::log2(440.0) * 127.0 / 13.0;
	for (int i = 0; i < 400; ++i) {
		d.processStereo(l.data(), r.data(), 512);
	}
	CHECK(d.blockForTest().freqIndex == doctest::Approx(a4).epsilon(1e-4));
	CHECK(d.blockForTest().centerHz == doctest::Approx(440.0).epsilon(1e-3));
	// THE KEY LIFTS; the host holds the pitch, and so does the device.
	d.setNoteContext({ 69.0f, false });
	d.processStereo(l.data(), r.data(), 512);
	CHECK(d.blockForTest().freqIndex == doctest::Approx(a4).epsilon(1e-4));
	// KEY TRACK OFF: Freq, whatever the note.
	s.keyTrack = false;
	s.freqLog2 = 11.0f;
	d.setTarget(s);
	for (int i = 0; i < 400; ++i) {
		d.processStereo(l.data(), r.data(), 512);
	}
	CHECK(d.blockForTest().centerHz == doctest::Approx(2048.0).epsilon(1e-3));
}

TEST_CASE("Stages reaches the loop: one stage and thirty-two are different sounds, and the count is the one asked for") {
	const std::vector<float> in = probe(8000, 0.5f);
	std::vector<std::vector<float>> outs;
	for (const int stages : { 1, 32 }) {
		Disperser d;
		d.prepare(kRef);
		Settings s = defaults();
		s.stages = stages;
		d.setTarget(s);
		outs.push_back(render(d, in, 512));
		// THE VALUE: the stages past the count were never laid out.
		CHECK(d.stageHzForTest(stages - 1, 0) > 0.0);
		if (stages < 32) {
			CHECK(d.stageHzForTest(stages, 0) == 0.0);
		}
	}
	CHECK(outs[0] != outs[1]);
}

TEST_CASE("the mono road is the LEFT channel of the stereo one") {
	const std::vector<float> in = probe(6000, 0.5f);
	Settings s = defaults();
	s.twist = 0.1f; // Width: the two channels' stages differ, so left and right differ
	Disperser a;
	a.prepare(kRef);
	a.setTarget(s);
	std::vector<float> right;
	const std::vector<float> left = render(a, in, 300, &right);
	CHECK(left != right);
	Disperser m;
	m.prepare(kRef);
	m.setTarget(s);
	std::vector<float> mono = in;
	for (size_t at = 0; at < mono.size(); at += 300) {
		m.processMono(mono.data() + at, static_cast<int>(std::min<size_t>(300, mono.size() - at)));
	}
	CHECK(mono == left);
}

TEST_CASE("the tail ends in exact silence") {
	Settings s = defaults();
	s.twist = 3.6f / 8.0f; // Chirp at 0.6: echoes regenerate, and die
	Disperser d;
	d.prepare(kRef);
	d.setTarget(s);
	std::vector<float> in = probe(22050, 0.8f);
	in.resize(44100 * 12, 0.0f);
	const std::vector<float> out = render(d, in, 512);
	size_t lastLive = 0;
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i] != 0.0f) {
			lastLive = i;
		}
	}
	MESSAGE("last non-zero sample at " << static_cast<double>(lastLive) / kRef << " s");
	CHECK(lastLive > 22050);           // there was a tail
	CHECK(lastLive < out.size() - kRef); // and it ended, to the bit, well before the end
}

TEST_CASE("a hot signal everywhere stays finite") {
	for (int tz = 0; tz < 8; ++tz) {
		for (int wz = 0; wz < 8; ++wz) {
			Settings s = defaults();
			s.topo = static_cast<float>((tz + 0.5) / 8.0);
			s.twist = static_cast<float>((wz + 0.5) / 8.0);
			s.stages = 32;
			Disperser d;
			d.prepare(48000.0);
			d.setTarget(s);
			const std::vector<float> out = render(d, probe(9600, 8.0f, 48000.0), 480);
			CAPTURE(tz);
			CAPTURE(wz);
			CHECK(std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v); }));
		}
	}
}

TEST_CASE("the codec and the declarations") {
	const Settings d = defaults();
	CHECK(d.enabled);
	CHECK(d.stages == kDefaultStages);
	CHECK(d.keyTrack);
	CHECK(d.freqLog2 == doctest::Approx(64.0 * 13.0 / 127.0));
	CHECK(d.offsetOctaves == 0.0f);
	Settings s;
	s.enabled = false;
	s.topoMode = 5;
	s.topoPhase = 3.25f; // past its zone: a lap the codec must carry
	s.twistMode = 5;
	s.twistPhase = 2.5f;
	s.topo = static_cast<float>(topoPlaceOf(s.topoMode, s.topoPhase).coordinate);
	s.topoLap = static_cast<float>(topoPlaceOf(s.topoMode, s.topoPhase).lap);
	CHECK(s.topoLap == 2.25f);
	s.twist = static_cast<float>(twistCoordinateOf(s.twistMode, s.twistPos, s.twistPhase));
	s.pillAmount[2] = 0.8f;
	s.pillOn[2] = false; // off: the default is on (round 3)
	s.gamma = 2.0f;
	s.stages = 23;
	s.keyTrack = false;
	s.freqLog2 = 9.5f;
	s.offsetOctaves = -2.0f;
	State st { s };
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)) == st);
	// A SHORT ROW takes defaults for what it lacks.
	CHECK(stateOf(rowOf(st), 3).settings.stages == kDefaultStages);
	// Stages is 1..32 and whole.
	setValueOf(&st, Param::Stages, 40.0f);
	CHECK(st.settings.stages == kMaxStages);
	setValueOf(&st, Param::Stages, 0.0f);
	CHECK(st.settings.stages == kMinStages);
	CHECK(kTraits.perVoiceControl == -1); // bus only, by design
	CHECK(std::size(kTopoModeNames) == kZones);
	CHECK(std::size(kTwistModeNames) == 6);
	CHECK(std::string(kTopoModeNames[2]) == "Owlpass");
	// FREQ AND OFFSET ARE GATED ON KEY TRACK, and between them every Key Track value shows one of them.
	const auto& freq = kParamDescs[static_cast<size_t>(Param::Freq)];
	const auto& offset = kParamDescs[static_cast<size_t>(Param::Offset)];
	CHECK(freq.gateControl == static_cast<int16_t>(Param::KeyTrack));
	CHECK(offset.gateControl == static_cast<int16_t>(Param::KeyTrack));
	CHECK((freq.gateMask | offset.gateMask) == 0b11u);
	CHECK((freq.gateMask & offset.gateMask) == 0u);
	CHECK(kOffsetMax == doctest::Approx(6.3984375));
}
TEST_CASE("THE PILLS DEFAULT ON AT THE FIRMWARE'S OWN VALUES, and each one switched off disables its extra in the engine") {
	// Round 3: the pills default to on, and switching one off disables its extra. They default to the
	// neutral values that leave Twist as the factory has it. Asserted as the VALUES the engine read in its
	// block (`blockForTest`), against the firmware's own derivation restated here (`computeTwistParams`,
	// `computeTopoParams` at the same coordinate) — EXACTLY, because the default must be the firmware bit for bit.
	Settings s = defaults();
	for (size_t i = 0; i < kPillCount; ++i) {
		CAPTURE(std::string(kPillNames[i]));
		CHECK(s.pillOn[i]);
		CHECK(s.pillAmount[i] == 0.5f);
	}
	// The infinite Twist a lap in, over Owlpass: every derived quantity live, none within a fifth of its off value
	// (scanned: many coordinates leave one of the nine sitting at its off value, which would make "off" vacuous).
	s.twistMode = kTwistInfinite;
	s.twistPhase = 1.1f;
	s.topoMode = 2;
	s.topoPhase = 0.2f;
	s.topo = static_cast<float>(topoCoordinateOf(s.topoMode, s.topoPhase));
	s.twist = static_cast<float>(twistCoordinateOf(s.twistMode, s.twistPos, s.twistPhase));
	const Coordinate topoC = splitCoordinate(static_cast<double>(s.topo));
	const Coordinate twistC = splitCoordinate(static_cast<double>(s.twist));
	const TwistParams fwTwist = computeTwistParams(twistC.knob, twistC.lap);
	const TopoParams fwTopo = computeTopoParams(topoC.knob, topoC.lap, fwTwist.phaseOffset);
	const auto read = [](const Disperser::BlockForTest& b, Pill p) {
		switch (p) {
		case Pill::Width: return b.twist.width;
		case Pill::Punch: return b.twist.punch;
		case Pill::Chirp: return b.twist.chirpAmount;
		case Pill::Curve: return b.twist.spreadCurve;
		case Pill::QTilt: return b.twist.qTilt;
		case Pill::Q: return b.topo.q;
		case Pill::Detune: return b.topo.detuning;
		case Pill::Emphasis: return b.topo.emphasis;
		default: return b.topo.harmonicBlend;
		}
	};
	const auto firmware = [&](Pill p) {
		Disperser::BlockForTest b;
		b.twist = fwTwist;
		b.topo = fwTopo;
		return read(b, p);
	};
	const auto engine = [&](const Settings& settings) {
		Disperser d;
		d.prepare(kRef);
		d.setTarget(settings);
		(void)render(d, probe(512, 0.25f), 512);
		return d.blockForTest();
	};
	const Disperser::BlockForTest atDefault = engine(s);
	for (size_t i = 0; i < kPillCount; ++i) {
		const auto pill = static_cast<Pill>(i);
		CAPTURE(std::string(kPillNames[i]));
		// ON, AT THE DEFAULT: the firmware's own, exactly.
		CHECK(read(atDefault, pill) == firmware(pill));
		// OFF: the extra disabled — `kPillOff` — and the zones' own value was not that already.
		Settings off = s;
		off.pillOn[i] = false;
		const Disperser::BlockForTest b = engine(off);
		CHECK(read(b, pill) == kPillOff[i]);
		CHECK(firmware(pill) != doctest::Approx(kPillOff[i]).epsilon(1e-3));
		// AND ONLY ITS OWN: every other pill is still the firmware's.
		for (size_t j = 0; j < kPillCount; ++j) {
			if (j != i) {
				CAPTURE(std::string(kPillNames[j]));
				CHECK(read(b, static_cast<Pill>(j)) == firmware(static_cast<Pill>(j)));
			}
		}
	}
	// THE OFF VALUES, BY NAME: none for the five amounts, the neutral for the three leans, the firmware's Q.
	CHECK(kPillOff[static_cast<size_t>(Pill::Width)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Punch)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Chirp)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Curve)] == 0.5);
	CHECK(kPillOff[static_cast<size_t>(Pill::QTilt)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Q)] == TopoParams {}.q);
	CHECK(kPillOff[static_cast<size_t>(Pill::Q)] == 2.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Detune)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Emphasis)] == 0.0);
	CHECK(kPillOff[static_cast<size_t>(Pill::Harmonic)] == 0.0);
	// THE DESCRIPTORS SAY THE SAME: every power defaults on.
	for (size_t i = 0; i < kPillCount; ++i) {
		CAPTURE(std::string(kPillNames[i]));
		CHECK(kParamDescs[static_cast<size_t>(pillOnParam(static_cast<Pill>(i)))].def == 1.0);
	}
}

TEST_CASE("SURVEY: the firmware's own tail never falls silent — a q31 limit cycle — and the port's does") {
	// Why the port has a rest (`Disperser::kRestLevel`) the firmware does not: measured here, the
	// firmware's loop after a burst settles at a floor it never leaves.
	Case c;
	c.twist = 3.625 / 8.0; // Chirp 0.625
	fwdisp::Fw fw = firmwareFor(c, fwdisp::Options {});
	std::vector<float> in = probe(22050, 0.8f);
	in.resize(44100 * 10, 0.0f);
	std::vector<fwdisp::q31_t> ql(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		ql[i] = fwdisp::cvt(static_cast<float>(static_cast<double>(in[i]) * fwdisp::kZeroDbfs));
	}
	std::vector<fwdisp::q31_t> qr = ql;
	for (size_t at = 0; at < in.size(); at += 128) {
		fw.process(ql.data() + at, qr.data() + at, std::min<size_t>(128, in.size() - at), -1);
	}
	double lastSecond = 0.0;
	for (size_t i = in.size() - 44100; i < in.size(); ++i) {
		lastSecond = std::max(lastSecond, std::abs(static_cast<double>(ql[i]) / fwdisp::kZeroDbfs));
	}
	MESSAGE("firmware peak in the tenth second after a 0.5 s burst: " << db(lastSecond) << " dBFS");
	CHECK(lastSecond > 1e-6);

	Disperser d;
	d.prepare(kRef);
	d.setTarget(settingsFor(c));
	const std::vector<float> out = render(d, in, 128);
	CHECK(std::all_of(out.end() - 44100, out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("SURVEY: the firmware's Freq and Spread bias — knob 0 is 94 Hz, spread 0 is two octaves, a tracked C4 is 1.4 kHz") {
	// `(uint32(smoothed) + 0x80000000) >> 25` on a UNIPOLAR q31: the knob's 0..127 lands on 64..127. Run
	// on the transcription as the firmware ships it, so the port doc's numbers are measured.
	const auto centreOf = [](int knob, int note, double topo) {
		Case c;
		c.freqKnob = knob;
		c.noteCode = note;
		c.topo = topo;
		c.frames = 128 * 800; // long enough for the power-on glide to land
		fwdisp::Fw fw = firmwareFor(c, fwdisp::Options {});
		std::vector<fwdisp::q31_t> l(128, 0);
		std::vector<fwdisp::q31_t> r(128, 0);
		for (int b = 0; b < 800; ++b) {
			fw.process(l.data(), r.data(), 128, note);
		}
		return fw;
	};
	const fwdisp::Fw low = centreOf(0, -1, 0.0);
	MESSAGE("knob 0: centre " << low.centerHz << " Hz (unbiased: 1 Hz)");
	CHECK(low.centerHz == doctest::Approx(std::exp2(64.0 * 13.0 / 127.0)).epsilon(1e-3));
	const fwdisp::Fw c4 = centreOf(64, 60, 0.0);
	MESSAGE("C4, offset 0: centre " << c4.centerHz << " Hz (unbiased: 261.6 Hz)");
	CHECK(c4.centerHz > 1300.0f); // MEASURED 1390 Hz: the q31 smoother settles one index under its target
	// Topo at the Cascade's start, param0 = 0 there: the stages still spread two octaves each way.
	CHECK(low.stageHzL[0] < low.centerHz / 3.0f);
}

TEST_CASE("the hot road: driven to 0.7 of the q31 ceiling, the port holds the firmware's level — and no shift wraps") {
	// **A WRAP NEEDS A SIGNAL NEAR THE q31 CEILING** — 42 dB over a Sound's full scale. Here the input is
	// driven to 0.7 of it (+37 dBFS), the Twist at Width (no delay road), 32 stages, every Topo zone.
	// MEASURED: the firmware's non-saturating `vshl`s (stage gains, delay writes) wrapped ZERO times, here
	// and in every other survey in this file — a stage's emphasis gain is at most 1.7 and a DF2T allpass's
	// output stays near its input's size. So the port reproduces the wrap (`wrap()` in `runStage`) for
	// fidelity, and no input a q31 can carry reaches it; what this holds is the saturating ceiling, where
	// the two agree in level.
	uint64_t wraps = 0;
	for (int tz = 0; tz < 8; ++tz) {
		for (const double pos : { 0.25, 0.75, 0.96875 }) {
			Case c;
			c.topo = (tz + pos) / 8.0;
			c.twist = 0.5 / 8.0;
			c.freqKnob = kHighKnob;
			c.stages = 32;
			c.amp = 90.0f;
			c.frames = 11025;
			const Compared r = compare(c);
			CAPTURE(tz);
			CAPTURE(pos);
			CAPTURE(r.maxErr);
			CHECK(std::abs(db(r.portRms / r.fwRms)) < 0.1);
			CHECK(r.maxErr / 90.0 < 1e-3); // MEASURED worst 5.9e-4 of the drive: the q31 ceilings clip here
			wraps += r.fwWraps;
		}
	}
	MESSAGE("firmware vshl wraps over 24 hot runs: " << wraps);
	CHECK(wraps == 0);
}


namespace {

/// A 0.5 s BURST, THEN SILENCE; returns the last non-zero sample's time after the input stopped, or a
/// negative number if the output was still sounding at the end.
double secondsToExactSilence(const Settings& s, double rate, double windowSeconds) {
	Disperser d;
	d.prepare(rate);
	d.setTarget(s);
	const auto burst = static_cast<size_t>(rate * 0.5);
	const auto total = static_cast<size_t>(rate * windowSeconds) + burst;
	std::vector<float> l(512);
	std::vector<float> r(512);
	size_t lastLive = 0;
	for (size_t at = 0; at < total; at += 512) {
		for (size_t i = 0; i < 512; ++i) {
			const size_t n = at + i;
			l[i] = r[i] = n < burst ? 0.8f * static_cast<float>(std::sin(0.05 * static_cast<double>(n))) : 0.0f;
		}
		d.processStereo(l.data(), r.data(), 512);
		for (size_t i = 0; i < 512; ++i) {
			if (l[i] != 0.0f || r[i] != 0.0f) {
				lastLive = at + i;
			}
		}
		// A SECOND OF EXACT ZEROS AFTER THE INPUT ENDS IS FOR GOOD: it is five times the 200 ms line, so
		// nothing can still be waiting in it, and with no input and no state a zero stays a zero. Stopping
		// here is what keeps a 180 s window cheap.
		if (at > burst && at + 512 > lastLive + static_cast<size_t>(rate)) {
			return static_cast<double>(lastLive - burst) / rate;
		}
	}
	if (lastLive + 512 >= total) {
		return -1.0;
	}
	return static_cast<double>(lastLive - burst) / rate;
}

} // namespace

namespace {

void silenceOverZones(std::initializer_list<int> zones) {
	double longest = 0.0;
	for (const int tz : zones) {
		for (const float twist : { 3.5f / 8.0f, 1.875f / 8.0f }) {
			Settings s = defaults();
			s.keyTrack = false;
			s.stages = 32;
			s.topo = static_cast<float>((tz + 0.875) / 8.0);
			s.twist = twist;
			const double t = secondsToExactSilence(s, 48000.0, 40.0);
			CAPTURE(tz);
			CAPTURE(twist);
			CAPTURE(t);
			CHECK(t > 0.0); // it ended, to the bit
			longest = std::max(longest, t);
		}
	}
	MESSAGE("longest tail to exact silence at the default Freq: " << longest << " s");
}

} // namespace

TEST_CASE("EXACT SILENCE: Punch and a middling Chirp, 32 stages, the default Freq — three routings") {
	// The plugin sleeps only on an EXACTLY silent block (883813f0), so every tail must end in 0.0f in
	// finite time. Stages at the 14 Hz angle floor carry ~0.9 s of group delay EACH at Q 20, so 32 of
	// them smear the lowest band over tens of seconds and Chirp feeds that back: the sound, not a fault.
	// The rest clears it at -140 dBFS and not earlier. Cascade, Parallel and Spring here; every routing in
	// the MEASURE case below (141 s under load, so not by default).
	silenceOverZones({ 0, 4, 7 });
}

TEST_CASE("MEASURE: EXACT SILENCE over all eight routings" * doctest::skip()) {
	silenceOverZones({ 0, 1, 2, 3, 4, 5, 6, 7 });
}

TEST_CASE("MEASURE: EXACT SILENCE at full Chirp, where the cap holds the loop at 0.97: finite, and long" * doctest::skip()) {
	// WHAT: a tail of ~98 s rendered to its exact zero (~16 s of CPU). WHEN: before touching the cap, the rest or
	// the flush. Out of the default run because "EXACT SILENCE: Punch and a middling Chirp" and "the rest cuts
	// nothing" hold the same mechanism in a few seconds.
	// **THE CAP KEEPS THE LONG TAIL, SO IT IS LONG.** MEASURED over a 27-setting survey: at the top of the
	// Chirp zone and in the meta zones the tail rings 18-500 s at the default Freq and past 600 s at a
	// 4 Hz centre — -0.26 dB per round trip, where a round trip includes the allpasses' own group delay.
	// It never grows (the runaway sweep); asserted finite here at a setting that took ~98 s.
	Settings s = defaults();
	s.keyTrack = false;
	s.stages = 32;
	s.topo = static_cast<float>(0.875 / 8.0);
	s.twist = 3.984375f / 8.0f;
	const double t = secondsToExactSilence(s, 44100.0, 180.0);
	MESSAGE("full Chirp, Cascade, default Freq: exact silence " << t << " s after the input stopped");
	CHECK(t > 0.0);
}


TEST_CASE("the rest cuts nothing above -140 dBFS: the output with it is the output without it, until it is zero") {
	for (const float twist : { 3.5f / 8.0f, 1.5f / 8.0f, 0.1f } /* Chirp 0.5, Punch, Width: tails that end inside the window */) {
		Settings s = defaults();
		s.twist = twist;
		s.stages = 16;
		std::vector<float> in = probe(22050, 0.8f);
		in.resize(44100 * 4, 0.0f); // MEASURED: the three tails end at 0.9-1.9 s
		Disperser a;
		a.prepare(kRef);
		a.setTarget(s);
		Disperser b;
		b.prepare(kRef);
		b.setTarget(s);
		b.setRestEnabledForTest(false);
		const std::vector<float> with = render(a, in, 512);
		const std::vector<float> without = render(b, in, 512);
		float worst = 0.0f;
		size_t firstZero = with.size();
		for (size_t i = 0; i < with.size(); ++i) {
			worst = std::max(worst, std::abs(with[i] - without[i]));
			if (i > 22050 && firstZero == with.size() && with[i] == 0.0f && with[i - 1] == 0.0f) {
				firstZero = i;
			}
		}
		CAPTURE(twist);
		CAPTURE(worst);
		CHECK(worst < Disperser::kRestLevel); // what the rest removed was already under -140 dBFS
		CHECK(firstZero < with.size());       // and it did end the tail
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PICTURE'S NUMBERS — `disperser_response.h`, held to the engine it claims to draw.              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the plot's group delay: a Cascade is the sum of its stages' group delays, by an independent restatement") {
	Settings s = defaults();
	s.keyTrack = false;
	s.freqLog2 = 9.0f;
	s.topo = static_cast<float>(0.4375 / 8.0); // Cascade
	s.twist = 0.05f;
	s.stages = 12;
	const ResponseModel m = responseModelOf(s, false, 60.0, 48000.0);
	REQUIRE(m.topology == Topology::Cascade);
	for (const double hz : { 60.0, 200.0, 512.0, 900.0, 3000.0, 11000.0 }) {
		// ONE STAGE'S PHASE BY ITS OWN FORMULA, differentiated numerically here — not `routingResponse`.
		double sum = 0.0;
		for (int i = 0; i < m.layout.count; ++i) {
			const AllpassCoeffs c = allpassCoeffs(m.layout.hzL[static_cast<size_t>(i)], m.layout.q[static_cast<size_t>(i)], 48000.0);
			const auto phase = [&](double f) {
				const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / 48000.0);
				const double a1 = 2.0 * c.a1Half;
				return std::arg((c.a2 + a1 * z1 + z1 * z1) / (1.0 + a1 * z1 + c.a2 * z1 * z1));
			};
			const double d = hz * 1e-5;
			double dphi = phase(hz + d) - phase(hz - d);
			dphi = std::remainder(dphi, 2.0 * std::numbers::pi);
			sum += -dphi / (2.0 * std::numbers::pi * 2.0 * d) * 1000.0;
		}
		CAPTURE(hz);
		CHECK(groupDelayMs(m, hz)[0] == doctest::Approx(sum).epsilon(1e-3));
	}
}

namespace {

/// THE ENGINE'S IMPULSE RESPONSE, settled first, with its stereo LFOs held at their centre (the plot draws
/// them there) — and the emphasis gains it holds, which the card takes from `Reading::StageGain0`.
struct EngineImpulse {
	std::vector<float> l;
	std::vector<float> r;
	std::array<float, kMaxStages> gains {};
};

EngineImpulse engineImpulse(const Settings& s) {
	constexpr double kRate = 48000.0;
	Disperser d;
	d.prepare(kRate);
	d.setTarget(s);
	d.setLfoFrozenForTest(true);
	// SETTLED FIRST: the emphasis gains glide a sixteenth per update from unity toward their targets.
	EngineImpulse out;
	out.l.assign(96000, 0.0f);
	out.r.assign(96000, 0.0f);
	d.processStereo(out.l.data(), out.r.data(), static_cast<int>(out.l.size()));
	std::fill(out.l.begin(), out.l.end(), 0.0f);
	std::fill(out.r.begin(), out.r.end(), 0.0f);
	out.l[0] = out.r[0] = 0.5f;
	d.processStereo(out.l.data(), out.r.data(), static_cast<int>(out.l.size()));
	for (int i = 0; i < kMaxStages; ++i) {
		out.gains[static_cast<size_t>(i)] = static_cast<float>(d.stageGainForTest(i));
	}
	return out;
}

/// ITS GROUP DELAY AT `hz`: a DFT at two nearby frequencies, the phasor advanced by recurrence.
double groupDelayOfImpulse(const std::vector<float>& h, double hz) {
	constexpr double kRate = 48000.0;
	const auto at = [&](double f) {
		std::complex<double> acc {};
		const std::complex<double> step = std::polar(1.0, -2.0 * std::numbers::pi * f / kRate);
		std::complex<double> w { 1.0, 0.0 };
		for (const float v : h) {
			acc += static_cast<double>(v) * w;
			w *= step;
		}
		return acc;
	};
	const double dHz = hz * 1e-3;
	const double dphi = std::arg(at(hz + dHz) / at(hz - dHz));
	return -dphi / (2.0 * std::numbers::pi * 2.0 * dHz / kRate) / kRate * 1000.0;
}

} // namespace

TEST_CASE("the plot's group delay is the ENGINE's, in every routing — read off the device's own impulse response") {
	// What the plot draws against what the device does: an impulse through the running Disperser (no
	// Punch, no Chirp — the curve does not claim the echoes), its phase measured by DFT. **The emphasis
	// gains are the ENGINE's**, as the card draws them: they rest 1/16 of the way to their targets for a
	// static setting (port doc §4.9), and in Parallel and Spring, which sum paths, the targets drew a curve
	// up to 0.9 ms off. 3% for the DFT's own finite difference.
	for (int tz = 0; tz < 8; ++tz) {
		Settings s = defaults();
		s.keyTrack = false;
		s.freqLog2 = 9.5f;
		s.stages = 8;
		s.topo = static_cast<float>((tz + 0.4375) / 8.0);
		s.twist = 0.05f; // Width: no delay road
		const EngineImpulse imp = engineImpulse(s);
		ResponseModel m = responseModelOf(s, false, 60.0, 48000.0);
		useStageGains(m, imp.gains.data());
		for (const double hz : { 300.0, 724.0, 1500.0 }) {
			for (const int ch : { 0, 1 }) {
				const double plotted = groupDelayMs(m, hz)[static_cast<size_t>(ch)];
				const double measured = groupDelayOfImpulse(ch == 0 ? imp.l : imp.r, hz);
				CAPTURE(tz);
				CAPTURE(hz);
				CAPTURE(ch);
				CAPTURE(plotted);
				CAPTURE(measured);
				CHECK(std::abs(plotted - measured) <= std::max(0.03 * std::abs(measured), 0.02));
			}
		}
	}
}

TEST_CASE("the plot's echo is where the engine writes: the folded, doubled centre stage") {
	for (const float twist : { 3.5f / 8.0f, 1.5f / 8.0f }) {
		Settings s = defaults();
		s.keyTrack = false;
		s.twist = twist;
		const ResponseModel m = responseModelOf(s, false, 60.0, 48000.0);
		Disperser d;
		d.prepare(48000.0);
		d.setTarget(s);
		std::vector<float> l(4800, 0.1f);
		std::vector<float> r = l;
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		CAPTURE(twist);
		CHECK(m.echoMs == doctest::Approx(d.blockForTest().foldedDelaySamples / 48.0).epsilon(1e-4));
	}
}

TEST_CASE("NM: on a playing transport the two wobbles are the song's — a fresh Disperser started mid-song is at the "
          "song's phase, and a stopped one free-runs from 0") {
	// THE INDEPENDENT RESTATEMENT: at song second `t` the width wobble is frac(t * 0.06 Hz * rate scale), the detune
	// wobble the same times `kDetuneLfoRatio`.
	const double t = 37.25;
	const auto firstTick = [&](bool playing) {
		auto d = std::make_unique<Disperser>();
		d->prepare(48000.0);
		d->setTarget(defaults());
		device::TransportClock clock {};
		clock.secondsPerBeat = 0.5;
		clock.playing = playing;
		clock.songPositionSeconds = t;
		d->setTransportClock(clock);
		std::array<float, 1> l { 0.1f };
		std::array<float, 1> r { 0.1f };
		d->processStereo(l.data(), r.data(), 1); // one sample: the first setup, at the block's own position
		return d;
	};
	const auto playing = firstTick(true);
	const double widthHz = kWidthLfoHz * playing->lfoRateScaleForTest();
	REQUIRE(widthHz > 0.0);
	const double width = t * widthHz;
	const double detune = t * widthHz * kDetuneLfoRatio;
	CHECK(std::abs(static_cast<double>(playing->widthLfoPhaseForTest()) - (width - std::floor(width))) < 1e-6);
	CHECK(std::abs(static_cast<double>(playing->detuneLfoPhaseForTest()) - (detune - std::floor(detune))) < 1e-6);
	// STOPPED: the firmware's free-running increment from 0 — not the song's phase.
	const auto stopped = firstTick(false);
	CHECK(stopped->widthLfoPhaseForTest() < 0.001f);
	CHECK(std::abs(static_cast<double>(stopped->widthLfoPhaseForTest()) - (width - std::floor(width))) > 0.01);
}
