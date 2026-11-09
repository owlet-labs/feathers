/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/automod/automod_lfo.h"

#include <cmath>
#include <vector>

using namespace feathers::fx::automod;

namespace {

constexpr double kSr = 48000.0;

/// A CONFIGURED LFO, so every case starts from the same declared state rather than from whatever the
/// defaults happen to be. The playbook's entry on default-constructed fixtures: a case that leans on
/// a default is asserting about the default, not about the thing.
Lfo make(float coord = 0.5f, float rateHz = 1.0f, RunMode mode = RunMode::Free) {
	Lfo lfo;
	lfo.prepare(kSr);
	lfo.setShape(coord, 0.0f);
	lfo.setRateHz(rateHz);
	lfo.setRunMode(mode);
	return lfo;
}

/// THE PHASE LIVES ON A CIRCLE, so 1 - eps and +eps are the SAME POINT and a linear comparison of
/// them is simply the wrong metric. Written after a first draft compared them with `==` and reported
/// a failure that was entirely the test's.
double circDist(double a, double b) {
	const double d = std::fabs(a - b);
	return std::min(d, 1.0 - d);
}

/// HOW FAR THE PHASE MAY LEGITIMATELY BE OUT AFTER `frames` SAMPLES — DERIVED, not chosen.
///
/// The increment is `round(hz / sr * 2^32)`, so it is wrong by at most half a count, and that error
/// accumulates linearly: at most `0.5 * frames / 2^32` turns. Deriving it rather than picking a
/// tolerance is what keeps these cases able to fail — a hand-picked epsilon big enough to swallow
/// the quantisation would also swallow a genuinely wrong rate.
double quantisationBound(long frames) {
	return 0.5 * static_cast<double>(frames) / 4294967296.0;
}

} // namespace

TEST_CASE("automod lfo: the rate multiplier is symmetric in ratio about an EXACT unity") {
	// **THE MIDDLE IS THE PART THAT NEEDS PINNING.** A symmetric-in-ratio curve that is subtly wrong
	// between the ends looks completely right AT the ends, so checking 0% and 200% alone would pass
	// against a linear map, a mis-centred one, or a wrong span.

	// THE VALUE IS OCTAVES (backlog DT, 2026-09-15): the readout prints the multiple the curve makes,
	// "0.25X".."4X", so the two cannot disagree the way 0%..200% did.
	// UNITY IS EXACT, NOT NEARLY. `exp2(0)` is exactly one, which is what makes the declared default
	// 1:1 by construction rather than by rounding — and it is why double-clicking Rate resets it to 1:1,
	// since the house gesture resets to the declared default.
	CHECK(rateMultiplierFor(0.0f) == 1.0f);

	// THE ENDS ARE THE DECLARED SPAN.
	CHECK(rateMultiplierFor(-2.0f) == doctest::Approx(0.25f).epsilon(1e-6));
	CHECK(rateMultiplierFor(2.0f) == doctest::Approx(4.0f).epsilon(1e-6));
	CHECK(std::exp2(kRateMultiplierOctaves) == doctest::Approx(4.0f));
	// AND WHAT THE BOX PRINTS AT THE ENDS AND THE MIDDLE IS THE MULTIPLE ITSELF — the value asserted,
	// not inferred from a readout that could be right by coincidence (it was, at 50% and 100%).
	CHECK(rateMultiplierFor(-1.0f) == doctest::Approx(0.5f).epsilon(1e-6));
	CHECK(rateMultiplierFor(1.0f) == doctest::Approx(2.0f).epsilon(1e-6));

	// **RECIPROCAL EITHER SIDE**, which is what "symmetric in ratio" means and what a linear map or a
	// map centred anywhere else would fail. −d and +d octaves are the same distance from unity in
	// opposite directions.
	for (const float d : { 0.25f, 0.5f, 0.75f, 1.5f }) {
		CAPTURE(d);
		CHECK(rateMultiplierFor(-d) * rateMultiplierFor(d) == doctest::Approx(1.0f).epsilon(1e-5));
	}

	// EXPONENTIAL MEANS EQUAL TRAVEL IS EQUAL RATIO, checked across the whole span rather than at one
	// pair — a curve can be reciprocal about the centre and still not be exponential.
	const float a = rateMultiplierFor(-0.5f);
	const float b = rateMultiplierFor(0.0f);
	const float c = rateMultiplierFor(0.5f);
	CHECK(b / a == doctest::Approx(c / b).epsilon(1e-5));

	// Out of range and non-finite: clamped to the ends, and a NaN is UNITY rather than the bottom —
	// a rate control that answered "quarter speed" to a broken value would be a silent slowdown.
	CHECK(rateMultiplierFor(-3.0f) == doctest::Approx(0.25f).epsilon(1e-6));
	CHECK(rateMultiplierFor(9.0f) == doctest::Approx(4.0f).epsilon(1e-6));
	CHECK(rateMultiplierFor(std::nanf("")) == 1.0f);
}

TEST_CASE("automod lfo: the rate ceiling is a measured stop, not the firmware's inherited 20") {
	// MEASURED, and `kRateHzMax`'s own note carries the numbers: the springs pass to 80 Hz cleanly
	// (-0.5 dB at 20, -2.9 at 50) and start eating the modulation at 100 (-8.2 dB). 20 Hz came from a
	// device with a different modulation grid. The spring-side half of this claim lives in
	// `test_fx_automod.cpp`, which can see the springs; this pins the number the LFO enforces.
	CHECK(kRateHzMax == doctest::Approx(80.0f));
	CHECK(kRateHzMin == doctest::Approx(0.01f));
}

TEST_CASE("automod lfo: the phase advances by rate over sample rate, per sample and not per block") {
	// THE VALUE, ASSERTED. One second at 1 Hz is exactly one cycle; a quarter second is a quarter
	// turn. Reading the phase rather than the output is the point — four taps of one waveform cannot
	// be unpicked back into a phase from outside, which is why the accessor exists.
	Lfo lfo = make(0.5f, 1.0f);
	CHECK(lfo.phase() == 0.0);

	const long quarter = static_cast<long>(kSr / 4);
	lfo.tick(static_cast<int>(quarter));
	CHECK(circDist(lfo.phase(), 0.25) <= quantisationBound(quarter));

	lfo.tick(static_cast<int>(quarter));
	CHECK(circDist(lfo.phase(), 0.5) <= quantisationBound(2 * quarter));

	// And it WRAPS rather than running away: another full second returns to the same point.
	lfo.tick(static_cast<int>(kSr));
	CHECK(circDist(lfo.phase(), 0.5) <= quantisationBound(2 * quarter + static_cast<long>(kSr)));
	CHECK(lfo.phase() < 1.0);
}

TEST_CASE("automod lfo: one second of LFO is the same wherever the block boundaries fall") {
	// THE BLOCK-INVARIANCE PIN, and it is the reason this advances per sample where the firmware
	// advances per buffer. `test_synth.cpp` pins the same property for a whole performance in blocks
	// of 1 and 37; this is that rule applied to the one object that would otherwise break it.
	//
	// A FIRMWARE-SHAPED LFO — phase += inc * blockSize, once per block — passes this trivially for a
	// CONSTANT block size and fails it the moment two runs use different ones, which is exactly what
	// a host does when its buffer changes. So the comparison is across block sizes, not across runs.
	const int total = static_cast<int>(kSr);

	// NOT CAPTURED: `total` is a constant expression, so it is usable inside the lambda without being
	// captured — and clang warns (-Wunused-lambda-capture) if it is, which macOS builds as an error.
	auto runInBlocks = [](int block) {
		Lfo lfo = make(0.37f, 3.0f);
		std::vector<double> phases;
		int done = 0;
		while (done < total) {
			const int n = std::min(block, total - done);
			lfo.tick(n);
			done += n;
			phases.push_back(lfo.phase());
		}
		return std::make_pair(lfo.phase(), phases.size());
	};

	const auto [phase1, count1] = runInBlocks(1);
	const auto [phase37, count37] = runInBlocks(37);
	const auto [phase512, count512] = runInBlocks(512);

	// EXACT, not near — the phase is a wrapping uint32 and unsigned multiply-add is modular, so
	// every decomposition of the same sample count gives the SAME BITS. This is the assertion that
	// would have caught the float accumulator this replaced (it was 3.0e-14 out at blocks of 1).
	CHECK(phase1 == phase37);
	CHECK(phase1 == phase512);

	// NON-VACUITY, two ways. The runs really were chopped differently...
	CHECK(count1 == static_cast<size_t>(total));
	CHECK(count37 < count1);
	CHECK(count512 < count37);
	// ...and the phase really did travel, so "all three were zero" cannot pass for agreement.
	// Three cycles at 3 Hz over one second lands back where it started, within the rate's own
	// quantisation.
	CHECK(circDist(phase1, 0.0) <= quantisationBound(total));
}

TEST_CASE("automod lfo: a block reports the LFO at its START, not at its end") {
	// READ-THEN-ADVANCE, AND NOTHING PINNED IT UNTIL NOW. Found by a known-bad: swapping tick() to
	// advance-then-read left all eight cases green, because every other case reads the phase or the
	// taps through a SEPARATE call and so never observes tick's internal order.
	//
	// IT IS A REAL CONTRACT, not a preference. The first block after a trigger must report the shape
	// where the trigger put it; a block that reported one block PAST the start would drop the
	// waveform's opening on every retrigger — inaudible on a fast LFO, and a missing attack on a slow
	// one, which is exactly the case an auto-wah is bought for.
	Lfo lfo = make(0.5f, 1.0f, RunMode::Retrigger);
	lfo.trigger();

	// What the shape reads AT the start phase, computed independently of the LFO.
	const float atStart = feathers::character::evalLfoShape(lfo.shape(), 0.0f);

	const LfoTaps first = lfo.tick(512);
	CHECK(first.left == atStart);

	// NON-VACUITY: one block later the value has genuinely moved, so the equality above is a
	// statement about WHEN it was read rather than about a waveform that happens to be flat here.
	const LfoTaps second = lfo.tick(512);
	CHECK(second.left != atStart);
}

TEST_CASE("automod lfo: the four taps are one waveform read at four offsets") {
	// THE PROPERTY THE WHOLE DESIGN EXISTS FOR. Asserted as an identity against the shape's own
	// evaluator rather than as "the four differ" — four different numbers would also be produced by
	// four independent LFOs, which is the thing this is not.
	Lfo lfo = make(0.61f, 2.0f);
	lfo.setStereoOffset(0.25f);
	lfo.setCombOffset(0.1f);

	lfo.tick(1234);
	const float p = static_cast<float>(lfo.phase());
	const LfoTaps taps = lfo.read();
	const auto& shape = lfo.shape();

	CHECK(taps.left == feathers::character::evalLfoShape(shape, p));
	CHECK(taps.right == feathers::character::evalLfoShape(shape, p + 0.25f));
	CHECK(taps.combLeft == feathers::character::evalLfoShape(shape, p + 0.1f));
	CHECK(taps.combRight == feathers::character::evalLfoShape(shape, p + 0.1f + 0.25f));

	// NON-VACUITY: the offsets actually separate the taps, so the identities above are not four
	// statements of the same number.
	CHECK(taps.left != doctest::Approx(taps.right));
	CHECK(taps.left != doctest::Approx(taps.combLeft));
}

TEST_CASE("automod lfo: a zero stereo offset makes the two channels identical, bit for bit") {
	// THE COMPLEMENT OF THE CASE ABOVE, and it is not redundant with it: that one proves the taps
	// are offset reads of one shape, this one proves the offset is the ONLY thing separating them.
	// A per-channel shape, or a per-channel phase, would pass the first and fail this.
	Lfo lfo = make(0.42f, 5.0f);
	lfo.setStereoOffset(0.0f);
	lfo.setCombOffset(0.0f);

	for (int i = 0; i < 40; ++i) {
		const LfoTaps t = lfo.tick(64);
		CAPTURE(i);
		CHECK(t.left == t.right);
		CHECK(t.combLeft == t.combRight);
		CHECK(t.left == t.combLeft);
	}
}

TEST_CASE("automod lfo: Free ignores a trigger and the other two modes restart on one") {
	// WHAT THE MODES MEAN, asserted on the PHASE rather than on the output — the shape passes
	// through the same level several times a cycle, so a value cannot say whether it restarted.
	SUBCASE("Free keeps running") {
		Lfo lfo = make(0.5f, 1.0f, RunMode::Free);
		lfo.tick(static_cast<int>(kSr / 4));
		const double before = lfo.phase();
		lfo.trigger();
		CHECK(lfo.phase() == before); // exact: a trigger must not nudge it at all
		CHECK(circDist(before, 0.25) <= quantisationBound(static_cast<long>(kSr / 4)));
	}

	SUBCASE("Retrigger goes back to the start phase") {
		Lfo lfo = make(0.5f, 1.0f, RunMode::Retrigger);
		lfo.tick(static_cast<int>(kSr / 4));
		CHECK(circDist(lfo.phase(), 0.25) <= quantisationBound(static_cast<long>(kSr / 4)));
		lfo.trigger();
		CHECK(lfo.phase() == 0.0); // exact: it snaps to the start phase, it does not approach it
	}

	SUBCASE("Once runs exactly one cycle and then holds at the start phase") {
		Lfo lfo = make(0.5f, 1.0f, RunMode::Once);
		CHECK_FALSE(lfo.finished());

		// Just under a full cycle: still running, and NOT yet back at the start.
		lfo.tick(static_cast<int>(kSr) - 1000);
		CHECK_FALSE(lfo.finished());
		CHECK(lfo.phase() > 0.9);

		// Past the cycle: held, at the start phase, and further ticks move nothing.
		lfo.tick(2000);
		CHECK(lfo.finished());
		CHECK(lfo.phase() == 0.0);
		lfo.tick(10000);
		CHECK(lfo.phase() == 0.0);
	}

	SUBCASE("a retrigger releases a finished Once, rather than leaving it stuck") {
		// THE ACCUMULATOR HAD TO BE CLEARED IN TWO PLACES AND WAS CLEARED IN NEITHER. Written after
		// finding it: without the reset in trigger(), a re-fired Once carries the previous cycle's
		// distance and finishes immediately, so the second note gets no sweep at all.
		Lfo lfo = make(0.5f, 1.0f, RunMode::Once);
		lfo.tick(static_cast<int>(kSr) + 100);
		REQUIRE(lfo.finished());

		lfo.trigger();
		CHECK_FALSE(lfo.finished());
		lfo.tick(static_cast<int>(kSr / 4));
		CHECK(circDist(lfo.phase(), 0.25) <= quantisationBound(static_cast<long>(kSr / 4)));
		CHECK_FALSE(lfo.finished());
	}
}

TEST_CASE("automod lfo: a non-finite rate or offset cannot poison the output") {
	Lfo lfo = make(0.5f, 1.0f);
	const float nan = std::nanf("");

	lfo.setRateHz(nan);
	lfo.setStereoOffset(nan);
	lfo.setCombOffset(std::numeric_limits<float>::infinity());
	lfo.tick(4096);

	const LfoTaps t = lfo.read();
	CHECK(std::isfinite(t.left));
	CHECK(std::isfinite(t.right));
	CHECK(std::isfinite(t.combLeft));
	CHECK(std::isfinite(t.combRight));
	CHECK(std::isfinite(lfo.phase()));

	// A good neighbour, so "it returned zero for everything" cannot pass for a fix.
	Lfo good = make(0.5f, 4.0f);
	good.tick(1000);
	CHECK(good.read().left != 0.0f);
}

TEST_CASE("automod lfo: preparing at a new sample rate re-derives the increment rather than the phase") {
	// TWO SAMPLE RATES, ONE FREQUENCY, THE SAME MUSICAL RESULT — which is the claim a rate change
	// has to satisfy, and it is not the same as "the phase is unchanged".
	Lfo a;
	a.prepare(48000.0);
	a.setShape(0.5f, 0.0f);
	a.setRateHz(2.0f);
	a.tick(48000 / 2);

	Lfo b;
	b.prepare(96000.0);
	b.setShape(0.5f, 0.0f);
	b.setRateHz(2.0f);
	b.tick(96000 / 2);

	// Half a second at 2 Hz is one full cycle at either rate. NOT bit-identical, and that is
	// correct rather than a concession: the two rates quantise their increments differently, so the
	// bound is each run's own accumulated quantisation.
	const double bound = quantisationBound(24000) + quantisationBound(48000);
	CHECK(circDist(a.phase(), b.phase()) <= bound);
	CHECK(circDist(a.phase(), 0.0) <= quantisationBound(24000));

	// And prepare() resets, so a rate switch does not carry a phase across.
	a.tick(1000);
	REQUIRE(a.phase() > 0.0);
	a.prepare(44100.0);
	CHECK(a.phase() == 0.0);
}
