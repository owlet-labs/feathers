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

// THE HAND DRUM AND THE FM PERC, HIT ALONE. Every claim is asserted on the value the hit is using (its seams), restated here from the physics or the
// design's formula: the Bessel zeros and the Bessel functions come from the literature's tables and the standard library, never from the code under test.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/drum_voice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace feathers;
using namespace feathers::drum;

namespace {

constexpr double kSr = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The Bessel zeros j_0n..j_mn in ascending order (Abramowitz and Stegun, table 9.5): a free membrane's mode ratios are these over the first.
constexpr double kJ[12] { 2.404825557695773, 3.831705970207512, 5.135622301840683, 5.520078110286311, 6.380161895923984, 7.015586669815619,
	                      7.588342434503805, 8.417244140399865, 8.653727912911012, 8.771483815959954, 9.761023129981670, 9.936109524217684 };

struct Rig {
	Hit hit;
	ks::KsString string;
	ks::PluckBuilder builder;
	Rig() {
		string.prepare(kSr);
		builder.prepare(kSr);
		hit.prepare(kSr);
		hit.setString(&string, &builder);
	}
};

Laws handLaws(double hz = 200.0, double t60 = 0.6) {
	Laws l;
	l.hz = hz;
	l.t60 = t60;
	l.sweepOctaves = 0.0;
	l.sweepTau = 0.02;
	l.tone = 0.5f;
	l.noise = 0.0f;
	l.snap = 1.0f; // the hardest contact: the mallet weights are within a hair of 1 at these pitches
	l.malletSeconds = 0.005 * std::pow(0.06, 1.0);
	l.metal = 0.0f;
	l.shell = 0.0;
	l.pot = 0.0;
	return l;
}

Laws fmLaws(int play, double hz, double ratio, double index, double indexT60, double t60 = 0.5) {
	Laws l;
	l.hz = hz;
	l.t60 = t60;
	l.sweepOctaves = 0.0;
	l.sweepTau = 0.01;
	l.tone = 0.5f;
	l.noise = 0.0f;
	l.snap = 0.5f;
	l.play = play;
	l.fmRatio = ratio;
	l.fmIndex = index;
	l.fmIndexT60 = indexT60;
	l.fmFeedback = 0.0;
	return l;
}

/// The raised-cosine contact's weight at x = frequency x contact time: sinc(x) / (1 - x^2), restated.
double mallet(double x) {
	const double ax = std::abs(x);
	if (ax < 1.0e-9) {
		return 1.0;
	}
	if (std::abs(ax - 1.0) < 1.0e-6) {
		return 0.5;
	}
	return std::abs(std::sin(std::numbers::pi * ax) / (std::numbers::pi * ax) / (1.0 - ax * ax));
}

/// One block's worth past the strike: render `n` samples.
std::vector<float> run(Hit& h, size_t n) {
	std::vector<float> out(n);
	for (size_t i = 0; i < n; i += 256) {
		h.render(out.data() + i, static_cast<int>(std::min<size_t>(256, n - i)));
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------
// THE HAND DRUM
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("Hand: the head's modes are a loaded head's near-harmonic set at Metal 0 and a free membrane's Bessel set at 1, geometrically between") {
	for (const float metal : { 0.0f, 0.25f, 0.5f, 1.0f }) {
		CAPTURE(metal);
		Rig r;
		Laws l = handLaws(200.0);
		l.metal = metal;
		r.hit.strike(Kind::Hand, l, 7, 1.0f);
		REQUIRE(r.hit.liveModes() == kHandHeadModes + 1);
		for (int k = 0; k < kHandHeadModes; ++k) {
			CAPTURE(k);
			const double harmonic = k + 1.0;
			const double bessel = kJ[k] / kJ[0];
			const double want = std::pow(harmonic, 1.0 - metal) * std::pow(bessel, static_cast<double>(metal));
			CHECK(std::abs(r.hit.currentHz(k) / (200.0 * want) - 1.0) < 1.0e-12);
		}
	}
}

TEST_CASE("Hand: the bend starts hz x 2^(+-octaves) and lands on hz, falling or rising, at each moment (by value)") {
	for (const bool up : { false, true }) {
		for (const double octaves : { 0.25, 0.8 }) {
			CAPTURE(up);
			CAPTURE(octaves);
			Rig r;
			Laws l = handLaws(200.0, 2.0);
			l.sweepOctaves = octaves;
			l.sweepTau = 0.03;
			l.bendUp = up;
			r.hit.strike(Kind::Hand, l, 7, 1.0f);
			const double stored = r.hit.laws().sweepOctaves; // the hit's own +-2% on the depth, restated below from what it holds
			const double sign = up ? -1.0 : 1.0;
			// THE START: the first block's bend is the full depth.
			CHECK(std::abs(r.hit.currentHz(0) / (200.0 * std::exp2(sign * std::round(stored * 2400.0) / 2400.0)) - 1.0) < 1.0e-12);
			CHECK((r.hit.handBendForTest() < 0.0) == up);
			// AT TIMES ALONG IT: one sample into a control block, so the block's start time is the one the bend was computed at.
			size_t done = 0;
			for (const double at : { 0.01, 0.03, 0.09, 0.2 }) {
				const auto target = static_cast<size_t>(std::lround(at * kSr / 16.0)) * 16 + 1;
				(void)run(r.hit, target - done);
				done = target;
				const double t = static_cast<double>(done - 1) / kSr;
				const double bend = std::round(stored * std::exp(-t / 0.03) * 2400.0) / 2400.0;
				CHECK(std::abs(r.hit.currentHz(0) / (200.0 * std::exp2(sign * bend)) - 1.0) < 1.0e-12);
			}
			// THE LANDING: a second on, the pitch is the pitch.
			(void)run(r.hit, 48000);
			CHECK(r.hit.currentHz(0) == doctest::Approx(200.0).epsilon(1.0e-12));
		}
	}
}

TEST_CASE("Hand: the strike position sets the modal balance, mode k at ratio^-(2.2 - 2 x tone) weighted by the contact, and the stroke weights the fundamental") {
	for (const int stroke : { kStrokeOpen, kStrokeSlap }) {
		for (const float tone : { 0.1f, 0.5f, 0.9f }) {
			CAPTURE(stroke);
			CAPTURE(tone);
			Rig r;
			Laws l = handLaws(200.0);
			l.tone = tone;
			l.metal = 0.5f;
			l.handStroke = stroke;
			r.hit.strike(Kind::Hand, l, 7, 1.0f);
			const double t = static_cast<double>(r.hit.laws().tone); // the hit's seeded +-3% on the position
			const double fundamental = stroke == kStrokeSlap ? 0.35 : 1.0;
			const auto ratio = [&](int k) { return std::pow(k + 1.0, 0.5) * std::pow(kJ[k] / kJ[0], 0.5); };
			const auto weight = [&](int k) { return std::pow(ratio(k), -(2.2 - 2.0 * t)) * (k == 0 ? fundamental : 1.0) * mallet(200.0 * ratio(k) * l.malletSeconds); };
			const double a0 = static_cast<double>(r.hit.modeAmplitude(0));
			REQUIRE(a0 > 0.0);
			for (int k = 1; k < kHandHeadModes; ++k) {
				CAPTURE(k);
				CHECK(static_cast<double>(r.hit.modeAmplitude(k)) / a0 == doctest::Approx(weight(k) / weight(0)).epsilon(1.0e-4));
			}
		}
	}
	// AND THE POSITION MOVES IT THE RIGHT WAY: a rim strike (high Tone) leaves the upper modes a larger share than a centre strike.
	Rig centre;
	Rig rim;
	Laws l = handLaws(200.0);
	l.tone = 0.1f;
	centre.hit.strike(Kind::Hand, l, 7, 1.0f);
	l.tone = 0.9f;
	rim.hit.strike(Kind::Hand, l, 7, 1.0f);
	CHECK(rim.hit.modeAmplitude(5) / rim.hit.modeAmplitude(0) > 2.0f * centre.hit.modeAmplitude(5) / centre.hit.modeAmplitude(0));
}

TEST_CASE("Hand: each stroke moves the laws by its table, and a damped stroke rings for its share") {
	Controls c;
	c.decaySeconds = 0.6;
	c.sweepSeconds = 0.05;
	c.direct = true;
	c.velTone = 0.0;
	c.velSnap = 0.0;
	c.velTension = 0.0;
	Fields f;
	f.tone = 0.5f;
	f.snap = 0.5f;
	f.sweep = 0.5f;
	const Laws open = lawsFor(Kind::Hand, f, c, 200.0, 0.5);
	struct Row {
		int stroke;
		double toneShift, snapShift, t60Mul, bendMul;
	};
	for (const Row row : { Row { kStrokeMuted, -0.10, 0.0, 0.12, 1.0 }, Row { kStrokeSlap, 0.35, 0.30, 0.5, 0.6 }, Row { kStrokeBass, -0.40, -0.20, 1.3, 1.5 } }) {
		CAPTURE(row.stroke);
		c.handStroke = row.stroke;
		const Laws l = lawsFor(Kind::Hand, f, c, 200.0, 0.5);
		CHECK(static_cast<double>(l.tone) == doctest::Approx(std::clamp(0.5 + row.toneShift, 0.0, 1.0)).epsilon(1.0e-6));
		CHECK(static_cast<double>(l.snap) == doctest::Approx(std::clamp(0.5 + row.snapShift, 0.0, 1.0)).epsilon(1.0e-6));
		CHECK(l.t60 == doctest::Approx(open.t60 * row.t60Mul).epsilon(1.0e-9));
		CHECK(l.sweepOctaves == doctest::Approx(open.sweepOctaves * row.bendMul).epsilon(1.0e-9));
		// THE RING: the lowest mode's per-sample decay is 10^(-3 / (T60 x sr)) at this stroke's T60 (the skin's law, ratio^-0.6, is 1 at ratio 1).
		Rig r;
		r.hit.strike(Kind::Hand, l, 3, 1.0f);
		CHECK(r.hit.modeDecayPerSample(0) == doctest::Approx(std::pow(10.0, -3.0 / (l.t60 * kSr))).epsilon(1.0e-12));
	}
	CHECK(open.t60 == doctest::Approx(0.6).epsilon(1.0e-9)); // Open is the table's identity
}

TEST_CASE("Hand: the shell is one more mode, at the head's pitch (a conga tube) through to 0.45 of it (a udu pot), ringing for its share, and it does not bend") {
	for (const double pot : { 0.0, 0.5, 1.0 }) {
		CAPTURE(pot);
		Rig r;
		Laws l = handLaws(200.0, 1.0);
		l.shell = 1.0;
		l.pot = pot;
		l.sweepOctaves = 1.0;
		l.sweepTau = 0.05;
		r.hit.strike(Kind::Hand, l, 7, 1.0f);
		const int shell = r.hit.shellModeForTest();
		REQUIRE(shell == kHandHeadModes);
		CHECK(r.hit.currentHz(shell) == doctest::Approx(200.0 * std::exp2(-1.152003093445049 * pot)).epsilon(1.0e-9));
		CHECK(r.hit.modeDecayPerSample(shell) == doctest::Approx(std::pow(10.0, -3.0 / (1.0 * (0.15 + 0.85 * pot) * kSr))).epsilon(1.0e-9));
		// THE HEAD BENDS AND THE SHELL DOES NOT: later on, the head has moved and the shell has not.
		const double shellBefore = r.hit.currentHz(shell);
		const double headBefore = r.hit.currentHz(0);
		(void)run(r.hit, 4801);
		CHECK(r.hit.currentHz(shell) == shellBefore);
		CHECK(r.hit.currentHz(0) < 0.9 * headBefore);
	}
	// THE SHELL'S LEVEL IS LINEAR IN SHELL, and none is none.
	Rig none;
	Rig half;
	Rig full;
	Laws l = handLaws(200.0);
	l.shell = 0.0;
	none.hit.strike(Kind::Hand, l, 7, 1.0f);
	l.shell = 0.5;
	half.hit.strike(Kind::Hand, l, 7, 1.0f);
	l.shell = 1.0;
	full.hit.strike(Kind::Hand, l, 7, 1.0f);
	CHECK(none.hit.modeAmplitude(kHandHeadModes) == 0.0f);
	CHECK(full.hit.modeAmplitude(kHandHeadModes) == doctest::Approx(2.0f * half.hit.modeAmplitude(kHandHeadModes)).epsilon(1.0e-6));
	CHECK(half.hit.modeAmplitude(kHandHeadModes) > 0.0f);
}

TEST_CASE("Hand: velocity moves the bend's depth by its square and the brightness by Vel Tone, not only the level") {
	Controls c;
	c.direct = true;
	c.velTone = 0.5;
	c.velSnap = 0.0;
	c.velTension = 1.0;
	c.sweepSeconds = 0.05;
	Fields f;
	f.tone = 0.4f;
	f.sweep = 0.5f;
	const Laws soft = lawsFor(Kind::Hand, f, c, 200.0, 0.5);
	const Laws hard = lawsFor(Kind::Hand, f, c, 200.0, 1.0);
	// DEPTH GOES AS v^2: a hit at 0.5 is a quarter of a hit at 1.
	CHECK(hard.sweepOctaves / soft.sweepOctaves == doctest::Approx(1.0 / 0.25).epsilon(1.0e-9));
	// BRIGHTNESS: Tone moves by depth x (v - 0.5).
	CHECK(static_cast<double>(hard.tone) - static_cast<double>(soft.tone) == doctest::Approx(0.5 * 0.5).epsilon(1.0e-6));
	// AND THE BEND'S TIME IS ITS OWN (Sweep Time), not the ring's: a Membrane's follows its T60, this one's is the control's.
	CHECK(hard.sweepTau == doctest::Approx(0.05 / 3.0).epsilon(1.0e-9));
}

TEST_CASE("Hand: One-shot ends in exact silence, a release damps the ring, the same seed is the same hit") {
	for (const int stroke : { kStrokeOpen, kStrokeMuted, kStrokeSlap, kStrokeBass }) {
		CAPTURE(stroke);
		Rig r;
		Laws l = handLaws(200.0, 0.4);
		l.handStroke = stroke;
		l.shell = 0.7;
		l.pot = 0.8;
		l.noise = 0.5f;
		l.sweepOctaves = 0.5;
		r.hit.strike(Kind::Hand, l, 11, 1.0f);
		bool silent = false;
		for (int block = 0; block < 400 && !silent; ++block) {
			(void)run(r.hit, 4800);
			silent = r.hit.idle();
		}
		CHECK(silent);
		std::vector<float> after = run(r.hit, 512);
		CHECK(*std::max_element(after.begin(), after.end()) == 0.0f);
		CHECK(*std::min_element(after.begin(), after.end()) == 0.0f);
	}
	Rig a;
	Rig b;
	Laws l = handLaws(200.0, 0.4);
	l.noise = 0.5f;
	l.shell = 0.5;
	a.hit.strike(Kind::Hand, l, 21, 1.0f);
	b.hit.strike(Kind::Hand, l, 21, 1.0f);
	CHECK(run(a.hit, 4096) == run(b.hit, 4096));
	// A RELEASE DAMPS: a long hit released at once is idle sooner than the same hit left to ring.
	Rig held;
	Rig rung;
	l.t60 = 3.0;
	held.hit.strike(Kind::Hand, l, 21, 1.0f);
	rung.hit.strike(Kind::Hand, l, 21, 1.0f);
	held.hit.release(0.05);
	(void)run(held.hit, 48000);
	(void)run(rung.hit, 48000);
	CHECK(held.hit.idle());
	CHECK_FALSE(rung.hit.idle());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// THE FM PERC
// ---------------------------------------------------------------------------------------------------------------------------------------------

namespace {

/// The amplitude of the line at `hz` in a one-second render whose frequencies are whole hertz: a single DFT bin, exact because the window holds whole cycles.
double lineAmplitude(const std::vector<float>& x, double hz) {
	double re = 0.0;
	double im = 0.0;
	for (size_t n = 0; n < x.size(); ++n) {
		const double ph = kTwoPi * hz * static_cast<double>(n) / kSr;
		re += static_cast<double>(x[n]) * std::cos(ph);
		im += static_cast<double>(x[n]) * std::sin(ph);
	}
	return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

} // namespace

TEST_CASE("FM: in a Drone the sidebands stand at the Bessel values J_n(index), at the held index, for the carrier plus n modulator frequencies") {
	// carrier 4800 Hz, modulator 600 Hz (ratio 0.125): every line is a whole number of hertz, so a one-second window holds whole cycles, and the
	// sidebands that fold back through zero (n <= -8) are of the order of J_8 and below, far under the tolerance.
	const double carrier = 4800.0;
	const double ratio = 0.125;
	const double spacing = 600.0;
	for (const double held : { 1.5, 2.404825557695773 /* J_0's first zero */ }) {
		CAPTURE(held);
		Rig r;
		r.hit.strike(Kind::FM, fmLaws(kPlayDrone, carrier, ratio, held / 0.5, 0.2), 5, 1.0f);
		const std::vector<float> y = run(r.hit, 48000);
		// THE INDEX THE HIT IS USING: half the peak, scaled by Tone (restated: 2^(2 (tone - 0.5)), the hit's own tone), constant in a Drone.
		const double actual = held * std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5));
		CHECK(r.hit.fmIndexForTest() == doctest::Approx(actual).epsilon(1.0e-6));
		const double a1 = lineAmplitude(y, carrier + spacing);
		REQUIRE(a1 > 0.01);
		for (int n = -3; n <= 3; ++n) {
			CAPTURE(n);
			const double line = lineAmplitude(y, carrier + spacing * n);
			const double want = std::abs(std::cyl_bessel_j(static_cast<double>(std::abs(n)), actual)) / std::abs(std::cyl_bessel_j(1.0, actual));
			CHECK(line / a1 == doctest::Approx(want).epsilon(2.0e-3).scale(1.0)); // an absolute 2e-3 where J is near zero
			if (std::abs(want) < 1.0e-6) {
				CHECK(line / a1 < 2.0e-3);
			}
		}
	}
}

TEST_CASE("FM: the index is peak x (held share + the rest falling by its own T60), restated: One-shot to nothing, Gate to half, Drone at half") {
	const double peak = 6.0;
	const double t60 = 0.2;
	for (const int play : { kPlayOneShot, kPlayGate, kPlayDrone }) {
		CAPTURE(play);
		Rig r;
		r.hit.strike(Kind::FM, fmLaws(play, 400.0, 1.5, peak, t60, 5.0), 5, 1.0f);
		size_t done = 0;
		for (const size_t sample : { size_t { 0 }, size_t { 480 }, size_t { 4800 }, size_t { 24000 } }) {
			(void)run(r.hit, sample + 1 - done);
			done = sample + 1;
			const double held = play == kPlayOneShot ? 0.0 : 0.5;
			const double fall = play == kPlayDrone ? 0.0 : std::pow(10.0, -3.0 * static_cast<double>(sample) / (t60 * kSr));
			const double toneScale = std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5)); // Tone's reach, on the hit's own tone
			const double want = peak * toneScale * (held + (1.0 - held) * fall);
			CAPTURE(sample);
			CHECK(r.hit.fmIndexForTest() == doctest::Approx(want).epsilon(1.0e-4).scale(peak * 2.0));
		}
	}
	// THE PICTURE'S STATEMENT OF IT AGREES (one place for the voice and the picture).
	CHECK(fmIndexAt(peak, t60, kPlayOneShot, 0.1) == doctest::Approx(peak * std::pow(10.0, -1.5)).epsilon(1.0e-9));
	CHECK(fmIndexAt(peak, t60, kPlayGate, 10.0) == doctest::Approx(3.0).epsilon(1.0e-9));
	CHECK(fmIndexAt(peak, t60, kPlayDrone, 0.0) == doctest::Approx(3.0).epsilon(1.0e-9));
}

TEST_CASE("FM: the carrier falls onto its pitch by the pitch envelope and the modulator keeps its ratio to it, at each moment (by value)") {
	Rig r;
	Laws l = fmLaws(kPlayOneShot, 300.0, 1.5, 3.0, 0.1, 2.0);
	l.sweepOctaves = 2.0;
	l.sweepTau = 0.02;
	r.hit.strike(Kind::FM, l, 5, 1.0f);
	CHECK(r.hit.fmCarrierHzForTest() == doctest::Approx(300.0 * 4.0).epsilon(1.0e-9)); // the start: two octaves up
	size_t done = 0;
	for (const double at : { 0.01, 0.04, 0.1, 0.3 }) {
		const auto target = static_cast<size_t>(std::lround(at * kSr / 16.0)) * 16 + 1;
		(void)run(r.hit, target - done);
		done = target;
		const double t = static_cast<double>(done - 1) / kSr;
		double bend = 2.0 * std::exp(-t / 0.02);
		bend = bend < 1.0e-4 ? 0.0 : bend; // below a tenth of a cent the envelope has landed and is taken as none
		const double carrier = 300.0 * std::exp2(bend);
		CHECK(r.hit.fmCarrierHzForTest() == doctest::Approx(carrier).epsilon(1.0e-9));
		CHECK(r.hit.fmModulatorHzForTest() == doctest::Approx(carrier * 1.5).epsilon(1.0e-9));
	}
	(void)run(r.hit, 48000);
	CHECK(r.hit.fmCarrierHzForTest() == doctest::Approx(300.0).epsilon(1.0e-9));
}

TEST_CASE("FM: the ratio is Ratio x 2^(Metal/2 + Fine/1200), a Drone has no envelope falling onto it, and the laws clamp what they are given") {
	Controls c;
	c.fmRatio = 2.0;
	c.fmFineCents = 100.0;
	Fields f;
	f.metal = 0.5f;
	f.sweep = 1.0f;
	const Laws l = lawsFor(Kind::FM, f, c, 220.0, 0.5);
	CHECK(l.fmRatio == doctest::Approx(2.0 * std::exp2(0.25 + 100.0 / 1200.0)).epsilon(1.0e-12));
	CHECK(l.sweepOctaves == doctest::Approx(3.0).epsilon(1.0e-12)); // Sweep 1 is three octaves of fall
	c.play = kPlayDrone;
	CHECK(lawsFor(Kind::FM, f, c, 220.0, 0.5).sweepOctaves == 0.0);
	// AT NO LEAN AND NO FINE the ratio is exactly the Ratio.
	Fields plain;
	Controls p;
	p.fmRatio = 3.5;
	CHECK(lawsFor(Kind::FM, plain, p, 220.0, 0.5).fmRatio == 3.5);
	// THE CLAMPS: index to 12, feedback to 1, the index's T60 off zero.
	p.fmIndex = 99.0;
	p.fmFeedback = 5.0;
	p.fmIndexSeconds = 0.0;
	const Laws g = lawsFor(Kind::FM, plain, p, 220.0, 0.5);
	CHECK(g.fmIndex == 12.0);
	CHECK(g.fmFeedback == 1.0);
	CHECK(g.fmIndexT60 > 0.0);
}

TEST_CASE("FM: with no feedback the voice is sin(2 pi phi_c + I sin(2 pi phi_m)) sample by sample, and feedback changes it") {
	const double carrier = 1000.0;
	const double ratio = 1.5;
	Rig r;
	r.hit.strike(Kind::FM, fmLaws(kPlayDrone, carrier, ratio, 4.0, 0.2), 5, 1.0f);
	const double held = 2.0 * std::exp2(2.0 * (static_cast<double>(r.hit.laws().tone) - 0.5)); // the held index in a Drone at a peak of 4, Tone applied
	const std::vector<float> y = run(r.hit, 512);
	// THE SCALE (the gain x the hit's level) from one sample, then every sample against the restated phase modulation.
	const auto pred = [&](size_t n) {
		const double k = static_cast<double>(n); // the phase is read before it advances: sample 0 is sin(0)
		return std::sin(kTwoPi * carrier * k / kSr + held * std::sin(kTwoPi * carrier * ratio * k / kSr));
	};
	size_t pivot = 0;
	for (size_t n = 0; n < y.size(); ++n) {
		if (std::abs(pred(n)) > std::abs(pred(pivot))) {
			pivot = n;
		}
	}
	const double scale = static_cast<double>(y[pivot]) / pred(pivot);
	CHECK(scale > 0.07); // the Drone's gain, 0.08, times the hit's level (within a quarter of a decibel of 1)
	CHECK(scale < 0.09);
	double worst = 0.0;
	for (size_t n = 0; n < y.size(); ++n) {
		worst = std::max(worst, std::abs(static_cast<double>(y[n]) - scale * pred(n)));
	}
	CHECK(worst < 2.0e-4);
	// FEEDBACK MOVES IT (the second assertion: the first shows the equation, this that feedback is in the road).
	Rig fb;
	Laws l = fmLaws(kPlayDrone, carrier, ratio, 4.0, 0.2);
	l.fmFeedback = 1.0;
	fb.hit.strike(Kind::FM, l, 5, 1.0f);
	const std::vector<float> z = run(fb.hit, 512);
	double apart = 0.0;
	for (size_t n = 0; n < z.size(); ++n) {
		apart = std::max(apart, std::abs(static_cast<double>(z[n]) - static_cast<double>(y[n])));
	}
	CHECK(apart > 0.05);
}

TEST_CASE("FM: One-shot ends in exact silence and idle, Gate holds until released, a Drone is never idle, the same seed is the same hit") {
	Rig one;
	Laws l = fmLaws(kPlayOneShot, 330.0, 1.5, 5.0, 0.1, 0.3);
	l.noise = 0.6f;
	l.snap = 0.8f;
	l.sweepOctaves = 1.0;
	one.hit.strike(Kind::FM, l, 9, 1.0f);
	bool silent = false;
	for (int block = 0; block < 200 && !silent; ++block) {
		(void)run(one.hit, 4800);
		silent = one.hit.idle();
	}
	CHECK(silent);
	const std::vector<float> after = run(one.hit, 512);
	CHECK(*std::max_element(after.begin(), after.end()) == 0.0f);
	CHECK(*std::min_element(after.begin(), after.end()) == 0.0f);

	Rig gate;
	l.play = kPlayGate;
	gate.hit.strike(Kind::FM, l, 9, 1.0f);
	(void)run(gate.hit, 48000 * 2);
	CHECK_FALSE(gate.hit.idle()); // held
	const std::vector<float> held = run(gate.hit, 4800);
	CHECK(*std::max_element(held.begin(), held.end()) > 0.2f);
	gate.hit.release(0.05);
	(void)run(gate.hit, 48000);
	CHECK(gate.hit.idle());

	Rig drone;
	l.play = kPlayDrone;
	drone.hit.strike(Kind::FM, l, 9, 1.0f);
	(void)run(drone.hit, 48000 * 3);
	CHECK_FALSE(drone.hit.idle());

	Rig a;
	Rig b;
	l.play = kPlayOneShot;
	a.hit.strike(Kind::FM, l, 31, 1.0f);
	b.hit.strike(Kind::FM, l, 31, 1.0f);
	CHECK(run(a.hit, 8192) == run(b.hit, 8192));
}

TEST_CASE("the new kinds' tables: what each reads, which modes it offers, and where its sweep time is read") {
	for (const Kind k : { Kind::Hand, Kind::FM }) {
		const FieldUse u = kindReads(k);
		CHECK((u.pitch && u.length && u.sweep && u.tone && u.noise && u.snap && u.metal));
		CHECK(kindControls(k).sweepTime);
	}
	CHECK(kindControls(Kind::Hand).tension);
	CHECK_FALSE(kindControls(Kind::FM).tension);
	CHECK(kindSustains(Kind::FM));
	CHECK_FALSE(kindSustains(Kind::Hand)); // a struck head does not sustain: Gate damps it, there is no Drone
	// A DRONE ASKED OF A HAND DRUM IS A ONE-SHOT; ASKED OF AN FM PERC IT IS A DRONE.
	Controls c;
	c.play = kPlayDrone;
	CHECK(lawsFor(Kind::Hand, Fields {}, c, 200.0, 0.5).play == kPlayOneShot);
	CHECK(lawsFor(Kind::FM, Fields {}, c, 220.0, 0.5).play == kPlayDrone);
	CHECK(kKindNames[static_cast<size_t>(Kind::Hand)] == std::string("Hand drum"));
	CHECK(kKindNames[static_cast<size_t>(Kind::FM)] == std::string("FM perc"));
	// A STRIKE NEVER CLICKS BY ITSELF: the FM's first sample is exactly zero (both phases start at 0 and are read before they advance), and a Hand
	// drum's modes start within a tenth of a radian, so at most a tenth of the level.
	{
		Rig r;
		Laws l = fmLaws(kPlayOneShot, 220.0, 1.5, 3.0, 0.1);
		l.snap = 0.0f; // no tap and no noise: those are a strike's own, and may sound on its first sample
		r.hit.strike(Kind::FM, l, 5, 1.0f);
		std::array<float, 1> first {};
		r.hit.render(first.data(), 1);
		CHECK(first[0] == 0.0f);
	}
	{
		Rig r;
		Laws l = handLaws(200.0);
		l.snap = 0.0f; // no tap: the tap is noise, which a strike's first sample may carry
		r.hit.strike(Kind::Hand, l, 5, 1.0f);
		std::array<float, 1> first {};
		r.hit.render(first.data(), 1);
		CHECK(std::abs(first[0]) < 0.2f);
	}
}

TEST_CASE("headroom: a Hand drum and an FM perc stay inside full scale at the extremes of their controls") {
	double worst = 0.0;
	for (const int stroke : { kStrokeOpen, kStrokeMuted, kStrokeSlap, kStrokeBass }) {
		for (const float metal : { 0.0f, 1.0f }) {
			for (const float noise : { 0.0f, 1.0f }) {
				for (const double shell : { 0.0, 1.0 }) {
					Laws l = handLaws(120.0, 1.0);
					l.handStroke = stroke;
					l.metal = metal;
					l.noise = noise;
					l.shell = shell;
					l.snap = 1.0f;
					l.tone = 0.5f;
					Rig r;
					r.hit.strike(Kind::Hand, l, 5, 1.0f);
					for (const float v : run(r.hit, 48000)) {
						worst = std::max(worst, static_cast<double>(std::abs(v)));
						CHECK(std::abs(v) <= 1.0f);
					}
				}
			}
		}
	}
	for (const int play : { kPlayOneShot, kPlayGate, kPlayDrone }) {
		for (const double index : { 0.0, 12.0 }) {
			for (const double ratio : { 0.5, 8.0 }) {
				for (const double fb : { 0.0, 1.0 }) {
					for (const float noise : { 0.0f, 1.0f }) {
						Laws l = fmLaws(play, 220.0, ratio, index, 0.3, 1.0);
						l.fmFeedback = fb;
						l.noise = noise;
						l.snap = 1.0f;
						Rig r;
						r.hit.strike(Kind::FM, l, 5, 1.0f);
						for (const float v : run(r.hit, 24000)) {
							worst = std::max(worst, static_cast<double>(std::abs(v)));
							CHECK(std::abs(v) <= 1.0f);
						}
					}
				}
			}
		}
	}
	CHECK(worst > 0.3); // they sound
}
