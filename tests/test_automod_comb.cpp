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

#include "fx/automod/automod_comb.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace feathers::fx::automod;

namespace {

/// A comb with no modulation and a whole-number delay, which is the configuration whose output can
/// be predicted EXACTLY — a fractional delay interpolates, and an interpolated value is a claim
/// about the interpolator rather than about the comb.
Comb makeStatic(float delaySamples, float feedback, float mix) {
	Comb c;
	c.prepare(48000.0);
	CombSettings s;
	s.baseDelaySamples = delaySamples;
	s.modRangeSamples = 0.0f;
	s.feedback = feedback;
	s.mix = mix;
	s.monoCollapse = 0.0f;
	c.setSettings(s);
	return c;
}

/// Push one impulse and then silence, returning what came out of the left channel.
std::vector<float> impulseResponse(Comb& c, int n, float amplitude = 1.0f) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		float l = (i == 0) ? amplitude : 0.0f;
		float r = l;
		c.processSample(l, r, 0.0f, 0.0f);
		out.push_back(l);
	}
	return out;
}

/// THE WET RESPONSE ONLY — the dry passthrough stripped, since the mix is additive.
std::vector<float> wetResponse(Comb& c, int n, float amplitude = 1.0f) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		const float dry = (i == 0) ? amplitude : 0.0f;
		float l = dry;
		float r = dry;
		c.processSample(l, r, 0.0f, 0.0f);
		out.push_back(l - dry);
	}
	return out;
}

/// The same, keeping both ears — for the stereo cases, where the claim is about L against R.
std::pair<std::vector<float>, std::vector<float>> wetResponseStereo(Comb& c, int n, float dryL,
                                                                    float dryR) {
	std::vector<float> outL, outR;
	for (int i = 0; i < n; ++i) {
		const float l0 = (i == 0) ? dryL : 0.0f;
		const float r0 = (i == 0) ? dryR : 0.0f;
		float l = l0;
		float r = r0;
		c.processSample(l, r, 0.0f, 0.0f);
		outL.push_back(l - l0);
		outR.push_back(r - r0);
	}
	return { outL, outR };
}

/// HOW MUCH ECHO ARRIVED AROUND `centre`, SUMMED OVER THE INTERPOLATOR'S WINDOW.
///
/// A SINC READ DOES NOT RETURN AN IMPULSE AS AN IMPULSE, and that is the whole reason this helper
/// exists rather than a subscript. MEASURED at an integer delay of 8: the response is symmetric
/// about the requested sample and spread across the twelve-tap window — 0.455 at the centre, 0.281
/// either side, 0.028, then -0.033 — summing to 1.008. So the interpolator has UNITY DC GAIN and
/// smooths; the energy is all there, distributed.
///
/// Summing the window is therefore the interpolator-independent measurement: it is what a linear read
/// and a sinc read agree about, and it is the quantity that means "how loud is the echo".
float echoAround(const std::vector<float>& ir, int centre, int halfWidth = 8) {
	float sum = 0.0f;
	for (int i = std::max(0, centre - halfWidth);
	     i <= std::min(static_cast<int>(ir.size()) - 1, centre + halfWidth); ++i) {
		sum += ir[static_cast<size_t>(i)];
	}
	return sum;
}

} // namespace

TEST_CASE("automod comb: an impulse comes back one delay later, at the mix amount") {
	// THE VALUE, COMPUTED INDEPENDENTLY, not "the output changed". A delay line's whole contract is
	// WHEN and HOW LOUD, and both are asserted here against arithmetic rather than a reference render.
	//
	// MEASURED AS A WINDOW RATHER THAN A SAMPLE, because the sinc read spreads an impulse across its
	// twelve taps with unity DC gain — see `echoAround`. The centre still lands exactly on the
	// requested delay, which is the WHEN, and the window still sums to the mix, which is the HOW LOUD.
	constexpr float kDelay = 16.0f;
	constexpr float kMix = 0.5f;
	Comb c = makeStatic(kDelay, /*feedback=*/0.0f, kMix);

	const std::vector<float> ir = wetResponse(c, 64);

	// WHEN: the response peaks exactly at the requested delay, and is symmetric about it.
	int peakAt = 0;
	for (size_t i = 1; i < ir.size(); ++i) {
		if (std::fabs(ir[i]) > std::fabs(ir[static_cast<size_t>(peakAt)])) {
			peakAt = static_cast<int>(i);
		}
	}
	CHECK(peakAt == static_cast<int>(kDelay));
	CHECK(ir[static_cast<size_t>(kDelay) - 1] == doctest::Approx(ir[static_cast<size_t>(kDelay) + 1]).epsilon(1e-4));

	// HOW LOUD: the window carries exactly the mix, no more and no less.
	CHECK(echoAround(ir, static_cast<int>(kDelay)) == doctest::Approx(kMix).epsilon(0.02));

	// NON-VACUITY: it is genuinely local. Far from the echo there is nothing, so "the whole buffer
	// sums to 0.5" cannot pass for "the echo is at 16".
	CHECK(std::fabs(echoAround(ir, 40, 8)) < 1e-4f);
}

TEST_CASE("automod comb: the echo is sinc-interpolated, which is a design decision and now has a case") {
	// SINC OVER LINEAR IS A DELIBERATE CHOICE (the modern interpolator on both reads), and until this case
	// existed NOTHING PROTECTED IT. Found by a known-bad: reverting `read()` to `readLinear()` left all nine
	// cases green, because every other assertion measures the echo as a WINDOW SUM and the two
	// interpolators agree about that exactly. A decision no test protects can be undone by anyone
	// tidying up, and the diff would look like a simplification.
	//
	// WHAT DISTINGUISHES THEM IS THE SPREAD, and it is a value rather than a vibe. At an integer
	// delay a linear read returns the impulse as ONE SAMPLE at full amplitude; the sinc read spreads
	// it symmetrically across its twelve taps — MEASURED at 0.455 in the centre, 0.281 either side,
	// 0.028, then -0.033, summing to unity. So: the peak is well under the total, and the neighbours
	// are not zero.
	constexpr float kDelay = 32.0f;
	Comb c = makeStatic(kDelay, /*feedback=*/0.0f, /*mix=*/1.0f);
	const std::vector<float> ir = wetResponse(c, 128);

	const size_t at = static_cast<size_t>(kDelay);
	const float total = echoAround(ir, static_cast<int>(kDelay));

	// The whole echo is there...
	CHECK(total == doctest::Approx(1.0f).epsilon(0.02));
	// ...but it is NOT in one sample, which is exactly what a linear read would give.
	CHECK(ir[at] < 0.6f);
	CHECK(ir[at] > 0.3f);
	// ...and its immediate neighbours carry real energy, symmetrically.
	CHECK(ir[at - 1] > 0.15f);
	CHECK(ir[at + 1] == doctest::Approx(ir[at - 1]).epsilon(1e-3));
}

TEST_CASE("automod comb: the mix is added to the dry signal rather than crossfaded with it") {
	// THE CHARACTER DECISION, PINNED. A crossfade is the obvious reading and it is the wrong one —
	// `automodulator.cpp:1255` adds. The two are indistinguishable at mix = 1 and differ everywhere
	// else, so the case reads the DRY sample at a partial mix, which is exactly where they part.
	Comb c = makeStatic(16.0f, 0.0f, 0.5f);
	float l = 1.0f;
	float r = 1.0f;
	c.processSample(l, r, 0.0f, 0.0f);

	// Additive: the dry sample is untouched at 1.0. A crossfade would have made it 0.5.
	CHECK(l == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(r == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("automod comb: feedback returns the echo at a decaying ratio, and the ratio is the setting") {
	// A GEOMETRIC SERIES, ASSERTED TERM BY TERM. "It rings" is satisfied by any feedback value; the
	// RATIO between successive echoes is what says the setting reached the line.
	//
	// EACH ECHO IS A WINDOW, for the reason `echoAround` gives — and the windows must not overlap, so
	// the delay is comfortably wider than the interpolator's reach.
	constexpr float kDelay = 32.0f;
	constexpr float kFb = 0.5f;
	constexpr float kMix = 1.0f;
	Comb c = makeStatic(kDelay, kFb, kMix);

	const std::vector<float> ir = wetResponse(c, 256);

	float expected = kMix;
	for (int n = 1; n <= 5; ++n) {
		const int at = n * static_cast<int>(kDelay);
		CAPTURE(n);
		CHECK(echoAround(ir, at) == doctest::Approx(expected).epsilon(0.05));
		expected *= kFb;
	}

	// NON-VACUITY: the series actually decayed.
	CHECK(echoAround(ir, 5 * static_cast<int>(kDelay))
	      < echoAround(ir, static_cast<int>(kDelay)) * 0.15f);
}

TEST_CASE("automod comb: zero feedback gives exactly one echo and no second one") {
	// THE COMPLEMENT, and not redundant with the case above: that one proves feedback DOES something,
	// this proves it is the ONLY thing producing repeats.
	Comb c = makeStatic(32.0f, 0.0f, 1.0f);
	const std::vector<float> ir = wetResponse(c, 256);

	CHECK(echoAround(ir, 32) == doctest::Approx(1.0f).epsilon(0.02));
	CHECK(std::fabs(echoAround(ir, 64)) < 1e-4f);
	CHECK(std::fabs(echoAround(ir, 96)) < 1e-4f);
}

TEST_CASE("automod comb: the delay a given LFO value asks for is the base plus the range, clamped") {
	// READ THE VALUE. A delay is not recoverable from a comb's output — the output carries every
	// delay the line has ever held — so the claim "this LFO value means this delay" needs a seam,
	// which is the same argument `Synth::voicePhaseForTest` was added under.
	Comb c;
	CombSettings s;
	s.baseDelaySamples = 100.0f;
	s.modRangeSamples = 50.0f;
	c.setSettings(s);

	CHECK(c.delayFor(0.0f) == doctest::Approx(100.0f));
	CHECK(c.delayFor(1.0f) == doctest::Approx(150.0f));
	CHECK(c.delayFor(-1.0f) == doctest::Approx(50.0f));
	CHECK(c.delayFor(0.5f) == doctest::Approx(125.0f));

	// THE CLAMP IS THE BUFFER'S SAFETY AND IS ASSERTED AT BOTH ENDS. A delay past the line's length
	// is a read out of bounds, not merely a wrong pitch.
	s.baseDelaySamples = 10.0f;
	s.modRangeSamples = 10000.0f;
	c.setSettings(s);
	CHECK(c.delayFor(1.0f) == doctest::Approx(kCombMaxDelaySamples));
	CHECK(c.delayFor(-1.0f) == doctest::Approx(kCombMinDelaySamples));

	// A non-finite LFO reads as no modulation rather than propagating into an index.
	CHECK(c.delayFor(std::nanf("")) == doctest::Approx(10.0f));
}

TEST_CASE("automod comb: mono collapse narrows the comb and leaves the dry signal's image alone") {
	// TWO CLAIMS, AND THE SECOND IS THE ONE THAT WOULD BE MISSED. Collapsing the whole device would
	// pass any "the channels got closer" check while destroying the dry stereo image, which is not
	// what a comb's width control is for.
	CombSettings s;
	s.baseDelaySamples = 32.0f;
	s.modRangeSamples = 0.0f;
	s.feedback = 0.0f;
	s.mix = 1.0f;
	s.monoCollapse = 1.0f;

	Comb c;
	c.prepare(48000.0);
	c.setSettings(s);

	// A hard-panned impulse: left only.
	float l = 1.0f;
	float r = 0.0f;
	c.processSample(l, r, 0.0f, 0.0f);

	// THE DRY SIGNAL IS STILL HARD LEFT. This is the assertion a whole-device collapse fails.
	CHECK(l == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(r == doctest::Approx(0.0f).epsilon(1e-6));

	// And the ECHO is fully collapsed: both ears carry the same half. Measured as windows, because
	// the sinc read spreads — see `echoAround`.
	Comb d;
	d.prepare(48000.0);
	d.setSettings(s);
	const auto [wl, wr] = wetResponseStereo(d, 128, 1.0f, 0.0f);
	CHECK(echoAround(wl, 32) == doctest::Approx(echoAround(wr, 32)).epsilon(1e-3));
	CHECK(echoAround(wl, 32) == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("automod comb: with collapse off the two channels stay independent") {
	// THE NON-VACUITY PARTNER of the case above — proving collapse is what narrowed it, rather than
	// the comb being mono all along.
	CombSettings s;
	s.baseDelaySamples = 32.0f;
	s.feedback = 0.0f;
	s.mix = 1.0f;
	s.monoCollapse = 0.0f;

	Comb c;
	c.prepare(48000.0);
	c.setSettings(s);

	const auto [wl, wr] = wetResponseStereo(c, 128, 1.0f, 0.0f);
	CHECK(echoAround(wl, 32) == doctest::Approx(1.0f).epsilon(0.02));
	CHECK(std::fabs(echoAround(wr, 32)) < 1e-4f);
}

TEST_CASE("automod comb: feedback is clamped below one, so a driven comb decays rather than growing") {
	// STABILITY, AS A VALUE RATHER THAN AS AN ABSENCE OF NaN. A feedback comb at exactly 1 never
	// decays and above 1 grows without bound, so the clamp is the difference between an effect and
	// a fault. Driven well past the ceiling and asserted to still be shrinking.
	Comb c = makeStatic(8.0f, /*feedback=*/50.0f, /*mix=*/1.0f);
	const std::vector<float> ir = impulseResponse(c, 4096);

	float peak = 0.0f;
	for (float v : ir) {
		peak = std::max(peak, std::fabs(v));
	}
	CHECK(std::isfinite(peak));
	CHECK(peak <= 1.001f);

	// And it is genuinely decaying by the end rather than merely bounded.
	CHECK(std::fabs(ir[4088]) < std::fabs(ir[8]));
}

TEST_CASE("automod comb: reset empties the line, so a new note cannot inherit the last one's echo") {
	Comb c = makeStatic(32.0f, 0.7f, 1.0f);
	wetResponse(c, 4);

	c.reset();

	// After the reset the line is empty, so the next 32 samples of silence produce silence — the
	// echo that was in flight is gone.
	for (int i = 0; i < 32; ++i) {
		float l = 0.0f;
		float r = 0.0f;
		c.processSample(l, r, 0.0f, 0.0f);
		CAPTURE(i);
		CHECK(std::fabs(l) < 1e-9f);
		CHECK(std::fabs(r) < 1e-9f);
	}

	// NON-VACUITY: the same comb without a reset DOES carry the echo, so the silence above is the
	// reset's doing and not the comb being inert.
	Comb d = makeStatic(32.0f, 0.7f, 1.0f);
	const std::vector<float> ir = wetResponse(d, 128);
	CHECK(echoAround(ir, 32) > 0.9f);
}

TEST_CASE("IS.5: the comb's pitch does not depend on the host's sample rate — delays scale, and the line holds 192 kHz") {
	// BEFORE: delays were raw samples, so a comb (sampleRate / delay in pitch) rang an octave higher at 96 kHz.
	CombSettings s;
	s.baseDelaySamples = kCombMaxDelaySamples - kCombMaxModRangeSamples; // the rest floor
	s.modRangeSamples = kCombMaxModRangeSamples;
	for (const double sr : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
		CAPTURE(sr);
		Comb c;
		c.prepare(sr);
		c.setSettings(s);
		// PITCH AT REST: sampleRate / delay is the same D-flat 1 at every rate.
		CHECK(sr / static_cast<double>(c.delayFor(0.0f)) == doctest::Approx(34.6478).epsilon(1e-3));
		// AND THE BOTTOM OF THE SWEEP IS NOT CUT BY THE LINE, even at 192 kHz.
		CHECK(c.delayFor(1.0f) == doctest::Approx(kCombMaxDelaySamples * static_cast<float>(sr / kCombReferenceRate)));
	}
	// THE FLOOR IS NOT SCALED — it is the interpolator's window, the same six samples at any rate.
	Comb fast;
	fast.prepare(192000.0);
	CombSettings tiny;
	tiny.baseDelaySamples = 0.0f;
	fast.setSettings(tiny);
	CHECK(fast.delayFor(0.0f) == kCombMinDelaySamples);
}

TEST_CASE("IS.5: the comb RINGS at the same pitch at 48 and 96 kHz — measured from the output, not the delay") {
	// THE VALUE THE DELAY STANDS FOR: an impulse through the comb comes back after `delay` samples, so the echo's
	// time in SECONDS must not move with the rate. Found as the output's PEAK after the impulse, not a threshold
	// crossing — the first draft thresholded at 0.5, the sinc read's echo never reached it, and "no echo" came
	// back as -1 / rate at both rates.
	//
	// **AND COMPARED WITH ABSOLUTE BOUNDS, NOT `doctest::Approx`**: that first draft passed a KNOWN-BAD that
	// ignored the rate entirely, because Approx adds a default scale of 1.0 to its tolerance — so
	// `-2e-05 == Approx(0.01).epsilon(1e-2)` holds. Any small quantity passes an Approx against a small target.
	const auto echoSeconds = [](double sr) {
		Comb c;
		c.prepare(sr);
		CombSettings s;
		s.baseDelaySamples = 480.0f; // 10 ms at the reference rate
		s.feedback = 0.0f;
		s.mix = 1.0f;
		c.setSettings(s);
		int peakAt = -1;
		float peak = 0.0f;
		const int window = static_cast<int>(sr * 0.05);
		for (int i = 0; i < window; ++i) {
			float l = i == 0 ? 1.0f : 0.0f;
			float r = l;
			c.processSample(l, r, 0.0f, 0.0f);
			if (i > 0 && std::fabs(l) > peak) {
				peak = std::fabs(l);
				peakAt = i;
			}
		}
		REQUIRE(peak > 0.1f); // an echo was found at all
		return static_cast<double>(peakAt) / sr;
	};
	const double at48 = echoSeconds(48000.0);
	const double at96 = echoSeconds(96000.0);
	CAPTURE(at48);
	CAPTURE(at96);
	CHECK(std::fabs(at48 - 0.010) < 0.0003); // 10 ms, within the interpolator's centring
	CHECK(std::fabs(at96 - at48) < 2.0 / 48000.0);
}
