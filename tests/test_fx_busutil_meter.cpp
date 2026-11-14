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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/busutil/busutil_meter.h"

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace bu = feathers::fx::busutil;

namespace {

constexpr double kRate = 48000.0;

/// **THE K-WEIGHTING'S GAIN AT ONE FREQUENCY, EVALUATED FROM THE COEFFICIENTS.**
///
/// THIS FILE FIRST CLAIMED "1 kHz IS WHERE K-WEIGHTING IS VERY NEARLY UNITY" AND USED A REFERENCE OF
/// `-0.691 + 20 log10(a)` WITH NOTHING ELSE IN IT. Measured: the weighting is **+0.70 dB** at 1 kHz,
/// so every loudness assertion below missed by that and the first run reported -20.25 against a
/// hand-waved -20.69. **The code was right and the reference was a model** — CLAUDE.md's rule about
/// taking the shape from the model and the number from the instrument, arriving in a test's own
/// expected value.
///
/// So the gain is COMPUTED, from the standard transfer function `H(z)` at `z = e^{jw}`, out of the
/// same coefficients the previous case pins against BS.1770-4's table. That keeps the reference
/// independent of the meter — nothing here runs the meter to find out what to expect — while making
/// it exact rather than approximately true at one frequency somebody guessed was flat.
[[nodiscard]] double magnitudeAt(const bu::Biquad& b, double hz, double fs) {
	const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * std::numbers::pi_v<double> * hz / fs));
	const std::complex<double> num = b.b0 + b.b1 * z + b.b2 * z * z;
	const std::complex<double> den = 1.0 + b.a1 * z + b.a2 * z * z;
	return std::abs(num / den);
}

/// THE WHOLE WEIGHTING'S GAIN AT ONE FREQUENCY — both stages, in dB.
[[nodiscard]] double weightingDb(double hz, double fs) {
	bu::KWeighting kw;
	kw.design(fs);
	return 20.0 * std::log10(magnitudeAt(kw.shelf, hz, fs) * magnitudeAt(kw.highpass, hz, fs));
}

/// THE LOUDNESS A STEREO SINE OF THIS AMPLITUDE MUST READ, from the standard's equation.
///
/// `L = -0.691 + 10 log10(sum G_i z_i)`; a sine of amplitude `a` has mean square `a^2/2`; two
/// identical channels at weight 1.0 sum to `a^2`. So `L = -0.691 + 20 log10(a)`, plus whatever the
/// weighting does at that frequency.
[[nodiscard]] double expectedLufs(double amplitude, double hz = 1000.0) {
	return -0.691 + 20.0 * std::log10(amplitude) + weightingDb(hz, kRate);
}

/// A SINE AT A GIVEN PEAK AMPLITUDE, both channels identical.
void sine(std::vector<float>& left, std::vector<float>& right, double amplitude, double hz = 1000.0) {
	for (size_t i = 0; i < left.size(); ++i) {
		const double t = static_cast<double>(i) / kRate;
		const auto v = static_cast<float>(amplitude
		                                  * std::sin(2.0 * std::numbers::pi_v<double> * hz * t));
		left[i] = v;
		right[i] = v;
	}
}

/// Feed n seconds of a signal in one call.
void feed(bu::Meter& m, double seconds, double amplitude, double hz = 1000.0) {
	const size_t n = static_cast<size_t>(kRate * seconds);
	std::vector<float> l(n), r(n);
	sine(l, r, amplitude, hz);
	m.process(l.data(), r.data(), static_cast<int>(n));
}

} // namespace

TEST_CASE("K-weighting designed at 48 kHz reproduces BS.1770-4's published coefficients") {
	// **THE FILTERS ARE CHECKED AGAINST THE SPEC, NOT AGAINST THEMSELVES.** The standard tabulates
	// its coefficients at 48 kHz only, and this code derives them from the analogue parameters at
	// whatever rate the host gives — so this case is the one place the two meet.
	//
	// **IT ALREADY EARNED ITSELF.** The first draft designed both stages with the RBJ cookbook, on
	// the strength of the parameters matching — same corner, same Q, same gain — and produced a
	// DIFFERENT FILTER: a1 = -1.65378 where Table 1 says -1.69066. Nothing else in this file could
	// have seen it. Every loudness assertion below would have gone on passing, because they measure
	// whatever filter is installed and this one was self-consistent; only a comparison against the
	// SPEC can tell a correct measurement of the wrong instrument from a correct one. The stages are
	// now the bilinear forms the table was generated from.
	bu::KWeighting kw;
	kw.design(48000.0);

	// BS.1770-4 Table 1 — stage 1, the high shelf.
	CHECK(kw.shelf.b0 == doctest::Approx(1.53512485958697).epsilon(1e-9));
	CHECK(kw.shelf.b1 == doctest::Approx(-2.69169618940638).epsilon(1e-9));
	CHECK(kw.shelf.b2 == doctest::Approx(1.19839281085285).epsilon(1e-9));
	CHECK(kw.shelf.a1 == doctest::Approx(-1.69065929318241).epsilon(1e-9));
	CHECK(kw.shelf.a2 == doctest::Approx(0.73248077421585).epsilon(1e-9));

	// BS.1770-4 Table 2 — stage 2, the RLB highpass.
	CHECK(kw.highpass.b0 == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(kw.highpass.b1 == doctest::Approx(-2.0).epsilon(1e-9));
	CHECK(kw.highpass.b2 == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(kw.highpass.a1 == doctest::Approx(-1.99004745483398).epsilon(1e-9));
	CHECK(kw.highpass.a2 == doctest::Approx(0.99007225036621).epsilon(1e-6));
}

TEST_CASE("the design is a function of the RATE, not a table") {
	// A BUILD THAT PASTED THE 48 kHz NUMBERS would weight the wrong frequencies at every other rate.
	// The coefficients must MOVE — asserted as inequality on the numbers rather than as a difference
	// in some output, because the claim is about the filter and not about a signal.
	bu::KWeighting at48;
	bu::KWeighting at96;
	at48.design(48000.0);
	at96.design(96000.0);
	CHECK(at48.shelf.a1 != doctest::Approx(at96.shelf.a1));
	CHECK(at48.highpass.a1 != doctest::Approx(at96.highpass.a1));
}

TEST_CASE("a -20 dBFS tone reads the loudness the standard's equation says it must") {
	// **THE REFERENCE IS THE EQUATION PLUS THE FILTER, BOTH COMPUTED HERE.** See `expectedLufs`: the
	// first version of this case left the weighting out on the grounds that 1 kHz is nearly flat, and
	// missed by the 0.70 dB the weighting actually applies there.
	bu::Meter m;
	m.prepare(kRate);
	feed(m, 4.0, 0.1);

	const double expected = expectedLufs(0.1);
	CHECK(m.reading(bu::Reading::Momentary) == doctest::Approx(expected).epsilon(0.002));
	CHECK(m.reading(bu::Reading::ShortTerm) == doctest::Approx(expected).epsilon(0.002));
	// **THE INTEGRATED FIGURE GETS A WIDER TOLERANCE THAN THE TWO WINDOWS, AND THE WIDTH IS THE
	// HISTOGRAM'S BIN.** It is computed from 0.1 LU bins, so a steady tone — every block in one
	// bin — sits at the worst case of half a bin. Measured at 0.047 LU, which is what set this
	// number; the header claimed under 0.01 until this line disagreed with it.
	CHECK(m.reading(bu::Reading::Integrated) == doctest::Approx(expected).epsilon(0.005));

	// AND THE WEIGHTING IS REALLY DOING SOMETHING AT THIS FREQUENCY, so the case above is not passing
	// because the correction happens to be zero. MEASURED: +0.70 dB at 1 kHz — and this number was
	// itself first written as 0.44, which was the weighting of the RBJ filters this file used before the
	// case above caught them. A measurement of the wrong instrument is not a measurement.
	CHECK(weightingDb(1000.0, kRate) == doctest::Approx(0.698).epsilon(0.01));
}

TEST_CASE("doubling the amplitude moves every loudness reading by 6 dB") {
	// SCALE IS THE PROPERTY A LOUDNESS METER MUST GET RIGHT even where the absolute calibration is
	// hard to check, and it is stated as a DIFFERENCE OF TWO MEASURED VALUES rather than against a
	// constant — so it holds whatever the weighting does at this frequency.
	bu::Meter quiet;
	quiet.prepare(kRate);
	feed(quiet, 4.0, 0.1);

	bu::Meter loud;
	loud.prepare(kRate);
	feed(loud, 4.0, 0.2);

	const float d = loud.reading(bu::Reading::ShortTerm) - quiet.reading(bu::Reading::ShortTerm);
	CHECK(d == doctest::Approx(20.0 * std::log10(2.0)).epsilon(0.005));
}

TEST_CASE("silence reads the floor rather than zero") {
	// **0 LUFS IS A REAL AND VERY LOUD VALUE**, so a meter answering 0 for silence would draw full
	// scale on an empty chain. Checked before any audio at all, and again after feeding actual
	// silence, because those are two different roads to the same wrong answer.
	bu::Meter m;
	m.prepare(kRate);
	CHECK(m.reading(bu::Reading::Momentary) == bu::kSilenceLufs);
	CHECK(m.reading(bu::Reading::Integrated) == bu::kSilenceLufs);

	feed(m, 1.0, 0.0);
	CHECK(m.reading(bu::Reading::Momentary) < -100.0f);
	// AND THE INTEGRATED READING IS STILL THE FLOOR — silence is below the absolute gate, so no block
	// entered the measurement at all. That is the gate working, not a missing measurement.
	CHECK(m.reading(bu::Reading::Integrated) == bu::kSilenceLufs);
}

TEST_CASE("the integrated gate ignores a quiet passage that the short-term window follows") {
	// **THE WHOLE POINT OF A GATED MEASUREMENT, and the one behaviour that separates integrated LUFS
	// from a long average.** Programme material with silence in it must not read quieter for having
	// pauses; a plain mean would drag toward the gaps.
	bu::Meter m;
	m.prepare(kRate);
	feed(m, 4.0, 0.1); // programme
	const float afterLoud = m.reading(bu::Reading::Integrated);
	feed(m, 4.0, 0.0); // a silent passage, far below the absolute gate

	// **THE FIGURE BARELY MOVES, AND "BARELY" IS THE HONEST WORD RATHER THAN "NOT AT ALL".** This
	// case first asserted it was unmoved to a thousandth and failed at 0.16 dB, and the drift is
	// CORRECT: the gating blocks that STRADDLE the programme's end are part loud and part silent, so
	// they read a few dB down and still sit far above the absolute gate. Only the fully silent blocks
	// are excluded. A meter that held perfectly still here would be one that had stopped measuring at
	// the boundary.
	CHECK(m.reading(bu::Reading::Integrated) == doctest::Approx(afterLoud).epsilon(0.02));

	// **AND THE GATE IS DOING REAL WORK, SAID AS A NUMBER RATHER THAN AS A TOLERANCE.** Half the feed
	// is silent, so an UNGATED energy mean over the whole thing would read 3 dB down. The integrated
	// figure is nowhere near that — which is the whole difference between this and a long average.
	const double ungated = afterLoud + 10.0 * std::log10(0.5);
	CHECK(m.reading(bu::Reading::Integrated) > ungated + 2.0);

	// NOT VACUOUS: the short-term window, which is ungated, followed the silence all the way down. If
	// it had not, the readings above would agree for a reason with nothing to do with the gate.
	CHECK(m.reading(bu::Reading::ShortTerm) < -100.0f);
}

TEST_CASE("resetting the integrated measurement leaves the windows alone") {
	// THE HAND'S GESTURE. A reset that also cleared the momentary window would blank the meter a
	// player is watching, and a reset that cleared nothing would be a button that lies.
	bu::Meter m;
	m.prepare(kRate);
	feed(m, 4.0, 0.1);
	REQUIRE(m.reading(bu::Reading::Integrated) > -30.0f);
	const float momentaryBefore = m.reading(bu::Reading::Momentary);

	m.resetIntegrated();

	CHECK(m.reading(bu::Reading::Integrated) == bu::kSilenceLufs);
	CHECK(m.reading(bu::Reading::Momentary) == momentaryBefore);
}

TEST_CASE("a single sample at full scale sets the clip age to zero, and it only grows after") {
	// **THE LED'S WHOLE CLAIM.** One sample in a block of thousands, with no ballistics anywhere near
	// it — an averaging detector would miss exactly this.
	bu::Meter m;
	m.prepare(kRate);
	REQUIRE(m.reading(bu::Reading::ClipAge) == bu::kClipAgeMax);

	std::vector<float> l(512, 0.01f), r(512, 0.01f);
	l[300] = 1.0f; // ONE sample, and exactly AT 0 dBFS rather than over it
	m.process(l.data(), r.data(), static_cast<int>(l.size()));

	// The age is the block's own length, not zero — the age advances by the block after the detect,
	// which is as fine as any UI can read it.
	const float justAfter = m.reading(bu::Reading::ClipAge);
	CHECK(justAfter == doctest::Approx(512.0 / kRate).epsilon(0.001));
	CHECK(justAfter < 0.1f); // the card's core would be lit

	// IT ONLY GROWS. Feeding quiet audio ages the mark rather than clearing it, which is what makes
	// the glow readable after the over has gone.
	feed(m, 1.0, 0.01);
	CHECK(m.reading(bu::Reading::ClipAge) > 1.0f);
	// AND IT IS CLAMPED, so a long idle cannot run the number away.
	feed(m, 20.0, 0.0);
	CHECK(m.reading(bu::Reading::ClipAge) == bu::kClipAgeMax);
}

TEST_CASE("0 dBFS is the ceiling, not the first value past it") {
	// `>=` RATHER THAN `>`, stated as its own case because the difference is one character and the
	// symptom is an LED that never lights on a signal that is exactly, deliberately, at full scale.
	bu::Meter at;
	at.prepare(kRate);
	std::vector<float> l(64, 1.0f), r(64, 1.0f);
	at.process(l.data(), r.data(), 64);
	CHECK(at.reading(bu::Reading::ClipAge) < 0.1f);

	bu::Meter under;
	under.prepare(kRate);
	std::vector<float> ql(64, 0.999f), qr(64, 0.999f);
	under.process(ql.data(), qr.data(), 64);
	CHECK(under.reading(bu::Reading::ClipAge) == bu::kClipAgeMax);
}

TEST_CASE("correlation reads +1 for a mono pair, -1 for an inverted one, and 0 for silence") {
	// ASSERTED AS THE THREE VALUES, not as "it moved". A correlation meter that returned the sign of
	// something would pass a difference check and be useless.
	const size_t n = static_cast<size_t>(kRate * 0.5);

	bu::Meter mono;
	mono.prepare(kRate);
	{
		std::vector<float> l(n), r(n);
		sine(l, r, 0.5);
		mono.process(l.data(), r.data(), static_cast<int>(n));
	}
	CHECK(mono.reading(bu::Reading::Correlation) == doctest::Approx(1.0f).epsilon(0.001));

	bu::Meter inverted;
	inverted.prepare(kRate);
	{
		std::vector<float> l(n), r(n);
		sine(l, r, 0.5);
		for (float& v : r) {
			v = -v;
		}
		inverted.process(l.data(), r.data(), static_cast<int>(n));
	}
	CHECK(inverted.reading(bu::Reading::Correlation) == doctest::Approx(-1.0f).epsilon(0.001));

	// SILENCE HAS NO PHASE RELATIONSHIP, and 0 is the only honest answer — the alternative is a NaN
	// out of a zero denominator, which draws as whatever the card's arithmetic does with one.
	bu::Meter quiet;
	quiet.prepare(kRate);
	feed(quiet, 0.5, 0.0);
	CHECK(quiet.reading(bu::Reading::Correlation) == 0.0f);
}

TEST_CASE("the peak reading is linear, holds the sample, and falls at 20 dB per second") {
	bu::Meter m;
	m.prepare(kRate);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	l[0] = 0.5f;
	r[0] = 0.25f;
	m.process(l.data(), r.data(), 64);

	// LINEAR, NOT dB — the card converts, because the unit a number is DRAWN in is a drawing
	// decision. The 64 samples of decay after the transient are a tiny fraction of a dB.
	CHECK(m.reading(bu::Reading::PeakL) == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(m.reading(bu::Reading::PeakR) == doctest::Approx(0.25f).epsilon(0.01));

	// AND IT FALLS AT THE DECLARED RATE. One second of silence is 20 dB down, a factor of ten.
	feed(m, 1.0, 0.0);
	CHECK(m.reading(bu::Reading::PeakL) == doctest::Approx(0.05f).epsilon(0.02));
}

TEST_CASE("a window shorter than its span averages what exists rather than counting silence") {
	// **A 3 s WINDOW OVER 0.4 s OF AUDIO MUST NOT READ 8 dB LOW.** Treating the blocks that have not
	// happened yet as silence gives a meter that is wrong for exactly as long as anyone is watching
	// it start, and then quietly becomes right — the hardest kind of wrong to notice.
	bu::Meter m;
	m.prepare(kRate);
	feed(m, 0.4, 0.1);

	CHECK(m.reading(bu::Reading::ShortTerm) == doctest::Approx(expectedLufs(0.1)).epsilon(0.002));
	// The momentary window is fully populated at this point, so the two must agree — which is also
	// the check that the short-term reading is not simply the momentary one under another name.
	CHECK(m.reading(bu::Reading::ShortTerm)
	      == doctest::Approx(m.reading(bu::Reading::Momentary)).epsilon(0.001));
}

TEST_CASE("the measurement is independent of how the audio is partitioned into blocks") {
	// THE SAME RULE THE DEVICE ITSELF HAD TO LEARN — `test_fx_devices` caught Bus Utility's ramp
	// crossing a block, and a meter whose windows were driven by the host's buffer size would have
	// the same defect one layer along.
	const auto run = [](int chunk) {
		bu::Meter m;
		m.prepare(kRate);
		const size_t n = static_cast<size_t>(kRate * 2.0);
		std::vector<float> l(n), r(n);
		sine(l, r, 0.3);
		for (size_t off = 0; off < n; off += static_cast<size_t>(chunk)) {
			const int take = static_cast<int>(std::min<size_t>(static_cast<size_t>(chunk), n - off));
			m.process(l.data() + off, r.data() + off, take);
		}
		return m.reading(bu::Reading::ShortTerm);
	};
	CHECK(run(64) == doctest::Approx(run(1024)).epsilon(1e-6));
	CHECK(run(64) == doctest::Approx(run(37)).epsilon(1e-6));
}

TEST_CASE("the raw peak is instantaneous, holds long enough to be seen, and does not decay") {
	// **THE SPECIFICATION IS AN "instantaneous raw amplitude readout", AND THE THREE CLAIMS IN THAT PHRASE
	// ARE ASSERTED SEPARATELY** — it arrives at once, it is the raw sample and not a weighted or
	// smoothed one, and it does not fall the way the bar meters do.
	bu::Meter m;
	m.prepare(kRate);
	CHECK(m.reading(bu::Reading::RawPeak) == 0.0f);

	// ONE SAMPLE, VISIBLE IN THE SAME BLOCK. A value latched only at the end of its window would read
	// zero here for up to 50 ms — the miss this reading exists to prevent, from the other side.
	std::vector<float> l(64, 0.01f), r(64, 0.01f);
	l[9] = 0.83f;
	m.process(l.data(), r.data(), 64);
	CHECK(m.reading(bu::Reading::RawPeak) == doctest::Approx(0.83f));

	// **IT DOES NOT DECAY WHILE THE WINDOW IS OPEN**, which is the difference from `PeakL`. Feed a
	// tenth of the hold in quiet audio and the raw peak is unmoved while the falling bar has already
	// dropped — the two are asserted against each other so this cannot pass by both being frozen.
	const float barBefore = m.reading(bu::Reading::PeakL);
	feed(m, bu::kRawPeakHoldSeconds * 0.1, 0.01);
	CHECK(m.reading(bu::Reading::RawPeak) == doctest::Approx(0.83f));
	CHECK(m.reading(bu::Reading::PeakL) < barBefore);

	// AND IT DOES COME DOWN once the window has closed — a peak that never fell would be a maximum,
	// not a meter.
	feed(m, bu::kRawPeakHoldSeconds * 3.0, 0.01);
	CHECK(m.reading(bu::Reading::RawPeak) < 0.1f);
	CHECK(m.reading(bu::Reading::RawPeak) == doctest::Approx(0.01f).epsilon(0.05));
}

TEST_CASE("the raw peak is the larger of the two channels, and is linear rather than weighted") {
	bu::Meter m;
	m.prepare(kRate);
	std::vector<float> l(32, 0.2f), r(32, 0.6f);
	m.process(l.data(), r.data(), 32);
	// THE LOUDER CHANNEL, not a sum and not an average.
	CHECK(m.reading(bu::Reading::RawPeak) == doctest::Approx(0.6f));

	// **LINEAR, AND UNWEIGHTED.** A 50 Hz tone is heavily attenuated by K-weighting — the loudness
	// readings would report it far quieter than a 1 kHz tone of the same amplitude — and this reading
	// must not. Asserted at the frequency where the difference is largest, which is what makes it a
	// real check rather than one that passes because the filter happens to be flat.
	bu::Meter low;
	low.prepare(kRate);
	feed(low, 0.2, 0.5, 50.0);
	bu::Meter mid;
	mid.prepare(kRate);
	feed(mid, 0.2, 0.5, 1000.0);
	CHECK(low.reading(bu::Reading::RawPeak) == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(mid.reading(bu::Reading::RawPeak) == doctest::Approx(0.5f).epsilon(0.01));
	// AND THE CHECK IS NOT VACUOUS: the WEIGHTED readings really do disagree at those two
	// frequencies, so "both read 0.5" is a fact about this reading rather than about the signal.
	CHECK(low.reading(bu::Reading::ShortTerm) < mid.reading(bu::Reading::ShortTerm) - 3.0f);
}

TEST_CASE("the integrated reading is made when a 100 ms block closes, and every read in between costs no scan") {
	// The histogram is 751 bins with a pow per occupied bin; it used to be rescanned on every host block read, though
	// it moves once per 100 ms block. The reading must equal a fresh scan of the histogram at every read, in a ramp so
	// the gate and the histogram really move, and the scan count must follow the blocks closed (counted here from the
	// audio's length), not the reads.
	bu::Meter m;
	m.prepare(kRate);
	const size_t blockFrames = static_cast<size_t>(kRate * 0.1);
	std::vector<float> l(256);
	std::vector<float> r(256);
	size_t framesFed = 0;
	size_t reads = 0;
	bool differs = false;
	float first = 0.0f;
	for (int chunk = 0; chunk < 4000; ++chunk) {
		const float amp = 0.02f + 0.4f * static_cast<float>(chunk) / 4000.0f;
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = amp * std::sin(0.13f * static_cast<float>(framesFed + i));
			r[i] = l[i];
		}
		m.process(l.data(), r.data(), static_cast<int>(l.size()));
		framesFed += l.size();
		const float got = m.reading(bu::Reading::Integrated);
		++reads;
		CHECK(got == m.integratedRecomputedForTest());
		if (reads == 1) {
			first = got;
		}
		differs = differs || got != first;
	}
	CHECK(differs); // NON-VACUITY: the figure moved as the level rose
	const size_t closed = framesFed / blockFrames;
	CHECK(m.integratedScansForTest() <= closed + 1u); // one scan per block closed (plus the reset's)
	CHECK(m.integratedScansForTest() < reads / 4);    // far fewer scans than reads
	// A RESET reads silence, and a repeated read costs nothing.
	m.resetIntegrated();
	const size_t scans = m.integratedScansForTest();
	CHECK(m.reading(bu::Reading::Integrated) == bu::kSilenceLufs);
	CHECK(m.reading(bu::Reading::Integrated) == bu::kSilenceLufs);
	CHECK(m.integratedScansForTest() == scans);
}
