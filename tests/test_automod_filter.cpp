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

#include "fx/automod/automod_filter.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace feathers::fx::automod;

namespace {

constexpr double kSr = 48000.0;

Filter make(float hz, float res, const FilterMix& mix) {
	Filter f;
	f.prepare(kSr);
	f.setState(hz, res, mix);
	return f;
}

/// Run the kernel's OWN filter, in one of its declared modes, over the same input. This is the reference
/// the coupling cases compare against — not a formula, not a recorded buffer, but the vendored
/// object doing the thing it says it does.
std::vector<float> kernelReference(feathers::fx::filter::CytomicSVF::Mode mode, float hz, float res,
                                const std::vector<float>& input) {
	feathers::fx::filter::CytomicSVF svf;
	svf.init();
	svf.setCoeff(mode, hz, res, static_cast<float>(1.0 / kSr));
	std::vector<float> out;
	out.reserve(input.size());
	for (float v : input) {
		float l = v;
		float r = v;
		feathers::fx::filter::CytomicSVF::step(svf, l, r);
		out.push_back(l);
	}
	return out;
}

std::vector<float> runOurs(Filter& f, const std::vector<float>& input) {
	std::vector<float> out;
	out.reserve(input.size());
	for (float v : input) {
		float l = v;
		float r = v;
		f.processSample(l, r);
		out.push_back(l);
	}
	return out;
}

/// An impulse plus a couple of cycles, so the comparison covers the transient and the steady state.
std::vector<float> probe(int n = 512) {
	std::vector<float> in(static_cast<size_t>(n), 0.0f);
	in[0] = 1.0f;
	for (int i = 0; i < n; ++i) {
		in[static_cast<size_t>(i)] +=
		    0.3f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(kSr));
	}
	return in;
}

} // namespace

/// ONE MODE, CHECKED ACROSS THE RESONANCE RANGE, against the kernel's own filter in its own declared mode.
///
/// ACROSS THE RANGE AND NOT AT ONE POINT, and that is a correction rather than thoroughness for its
/// own sake. The first draft checked res = 0.5 only — where the kernel's `k = 2 - 2*res` happens to equal 1
/// — so a known-bad that RECOMPUTED k instead of reading it back went green for a reason that had
/// nothing to do with the derivation being right. Both ends of the range are now included.
void checkModeIdentity(feathers::fx::filter::CytomicSVF::Mode mode, const FilterMix& mix) {
	const std::vector<float> in = probe();
	for (float res : { 0.0f, 0.3f, 0.5f, 0.98f }) {
		CAPTURE(res);
		Filter f = make(1000.0f, res, mix);

		const std::vector<float> ours = runOurs(f, in);
		const std::vector<float> theirs = kernelReference(mode, 1000.0f, res, in);

		REQUIRE(ours.size() == theirs.size());
		for (size_t i = 0; i < ours.size(); ++i) {
			CAPTURE(i);
			CHECK(ours[i] == theirs[i]);
		}

		// NON-VACUITY: the filter did something. Two silent buffers are also bit-identical.
		float energy = 0.0f;
		for (float v : ours) {
			energy += v * v;
		}
		CHECK(energy > 0.01f);
	}
}

TEST_CASE("automod filter: a pure lowpass mix is BIT-IDENTICAL to the kernel's own Mode::Lowpass") {
	// THE COUPLING CASE, and it is the reason this file exists.
	//
	// The mix vector is derived from `setCoeffPostGK`'s table — Lowpass (0,0,1), Bandpass (0,1,0),
	// Highpass (1,-k,-1) — giving m0 = wh, m1 = wb - wh*k, m2 = wl - wh. That derivation is only
	// valid while the kernel's own table says what it says today. IF `CytomicSVF` IS RESHAPED
	// UPSTREAM, THIS REDS instead of quietly changing a sound, which is the whole point of pinning a
	// coupling rather than trusting a version number.
	//
	// BIT-IDENTICAL, NOT NEAR: at a (1,0,0) mix the derivation gives exactly (0,0,1), which is exactly
	// what the kernel writes for Mode::Lowpass. Any drift in either direction breaks equality and there is
	// no tolerance to hide behind.
	checkModeIdentity(feathers::fx::filter::CytomicSVF::Mode::Lowpass, { 1.0f, 0.0f, 0.0f });
}

TEST_CASE("automod filter: a pure bandpass mix is bit-identical to the kernel's own Mode::Bandpass") {
	checkModeIdentity(feathers::fx::filter::CytomicSVF::Mode::Bandpass, { 0.0f, 1.0f, 0.0f });
}

TEST_CASE("automod filter: a pure highpass mix is bit-identical to the kernel's own Mode::Highpass") {
	// THE ONE THAT ACTUALLY EXERCISES THE DERIVATION. Lowpass and bandpass fall out of the algebra
	// with `k` multiplied by zero, so they would pass even if `k` were read wrongly. Highpass is
	// (1, -k, -1), so this is the only one of the three that can see a bad `k` — and only now that it
	// runs at more than one resonance.
	checkModeIdentity(feathers::fx::filter::CytomicSVF::Mode::Highpass, { 0.0f, 0.0f, 1.0f });
}

TEST_CASE("automod filter: the mix vector handed to the kernel is the derived one, and it uses the kernel's k") {
	// READ THE VALUE. An m-vector is not recoverable from a filtered buffer, and "the derivation used
	// the kernel's own k rather than a second copy of the resonance mapping" is exactly the sort of
	// claim CLAUDE.md says must be asserted rather than inferred.
	const FilterMix mix { 0.2f, 0.3f, 0.5f };
	Filter f = make(1000.0f, 0.5f, mix);

	const float k = f.kForTest();
	const auto m = f.mixVectorForTest();

	// CONSTANT-POWER NORMALISED FIRST, so the weights the derivation sees are the input over its own
	// RMS — not over its sum, which is what this case assumed before the two normalisations were
	// found to disagree.
	const float inv = 1.0f / std::sqrt(0.2f * 0.2f + 0.3f * 0.3f + 0.5f * 0.5f);
	const float wl = 0.2f * inv;
	const float wb = 0.3f * inv;
	const float wh = 0.5f * inv;

	CHECK(m[0] == doctest::Approx(wh).epsilon(1e-5));              // m0 = wh
	CHECK(m[1] == doctest::Approx(wb - wh * k).epsilon(1e-5));     // m1 = wb - wh*k
	CHECK(m[2] == doctest::Approx(wl - wh).epsilon(1e-5));         // m2 = wl - wh

	// NON-VACUITY: k is a real number doing real work, so the m1 assertion is not `0.3 - 0`.
	CHECK(k > 0.01f);
}

TEST_CASE("automod filter: the weights are normalised, so sliding the blend does not change the level") {
	// THE REASON NORMALISATION EXISTS: the phi field moves this vector every buffer, and a blend
	// whose loudness swung with the weighting would make the Type knob a volume control.
	// CONSTANT POWER, NOT SUM-TO-ONE — the squares sum to one, because the three bands are SUMMED and
	// equal perceived loudness across the blend needs the squares rather than the weights. This case
	// asserted sum-to-one until a measurement showed the two normalisations disagreeing: the derive
	// hands over a constant-power vector and this one re-scaled it by a different rule, losing up to
	// 3.87 dB exactly where the mix was spread across all three bands.
	Filter f = make(1000.0f, 0.3f, { 2.0f, 2.0f, 4.0f });
	const auto m = f.mixVectorForTest();
	const FilterMix& normalised = f.mix();

	const float expectedInv = 1.0f / std::sqrt(2.0f * 2.0f + 2.0f * 2.0f + 4.0f * 4.0f);
	CHECK(normalised.low == doctest::Approx(2.0f * expectedInv));
	CHECK(normalised.band == doctest::Approx(2.0f * expectedInv));
	CHECK(normalised.high == doctest::Approx(4.0f * expectedInv));
	// AND THE SQUARES SUM TO ONE, which is the property itself rather than three consequences of it.
	CHECK(normalised.low * normalised.low + normalised.band * normalised.band
	          + normalised.high * normalised.high
	      == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(m[0] == doctest::Approx(4.0f * expectedInv).epsilon(1e-6));

	// And a mix scaled by any factor gives the SAME vector — which is what "normalised" means.
	Filter g = make(1000.0f, 0.3f, { 1.0f, 1.0f, 2.0f });
	const auto mg = g.mixVectorForTest();
	CHECK(mg[0] == doctest::Approx(m[0]).epsilon(1e-6));
	CHECK(mg[1] == doctest::Approx(m[1]).epsilon(1e-6));
	CHECK(mg[2] == doctest::Approx(m[2]).epsilon(1e-6));
}

TEST_CASE("automod filter: a blend really is between the bands, not one of them") {
	// THE CLAIM THE WHOLE COMPONENT MAKES, and the one a dropdown cannot satisfy. A 50/50 low-high
	// blend must differ from BOTH a pure lowpass and a pure highpass — and must equal their sum,
	// because the blend is linear in the m-vector and the state is shared.
	const std::vector<float> in = probe();

	Filter blended = make(1000.0f, 0.4f, { 0.5f, 0.0f, 0.5f });
	Filter lowOnly = make(1000.0f, 0.4f, { 1.0f, 0.0f, 0.0f });
	Filter highOnly = make(1000.0f, 0.4f, { 0.0f, 0.0f, 1.0f });

	const std::vector<float> b = runOurs(blended, in);
	const std::vector<float> l = runOurs(lowOnly, in);
	const std::vector<float> h = runOurs(highOnly, in);

	// THE VALUE: the blend is exactly the weighted sum of the two, sample for sample. This is the
	// property `multimode_filter.h` states — the state update does not depend on the output mix — and
	// it is what makes a continuous blend free rather than two filters.
	//
	// THE WEIGHT IS 1/sqrt(2), NOT 0.5, because the mix is CONSTANT POWER: a (0.5, 0, 0.5) request
	// normalises to (0.707, 0, 0.707) so the two bands still sum to unit power. Writing 0.5 here was
	// the sum-to-one assumption showing through, and it is the same assumption that let the derive's
	// normalisation be silently undone.
	const float w = 1.0f / std::sqrt(2.0f);
	for (size_t i = 0; i < b.size(); ++i) {
		CAPTURE(i);
		CHECK(b[i] == doctest::Approx(w * l[i] + w * h[i]).epsilon(1e-4));
	}

	// NON-VACUITY: the blend is not simply equal to either of its parts.
	float diffLow = 0.0f;
	float diffHigh = 0.0f;
	for (size_t i = 0; i < b.size(); ++i) {
		diffLow += std::fabs(b[i] - l[i]);
		diffHigh += std::fabs(b[i] - h[i]);
	}
	CHECK(diffLow > 0.1f);
	CHECK(diffHigh > 0.1f);
}

TEST_CASE("automod filter: a lowpass passes DC and a highpass does not") {
	// THE SANITY THE BIT-IDENTITY CASES CANNOT GIVE. Those prove we agree with the kernel; this proves the kernel's
	// lowpass is a lowpass, so an upstream change that broke BOTH would still be caught.
	std::vector<float> dc(4096, 1.0f);

	Filter low = make(1000.0f, 0.0f, { 1.0f, 0.0f, 0.0f });
	Filter high = make(1000.0f, 0.0f, { 0.0f, 0.0f, 1.0f });

	const std::vector<float> lowOut = runOurs(low, dc);
	const std::vector<float> highOut = runOurs(high, dc);

	CHECK(lowOut.back() == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(std::fabs(highOut.back()) < 1e-3f);
}

TEST_CASE("automod filter: the cutoff is clamped to a fraction of Nyquist rather than a fixed Hz") {
	// THE FIRMWARE'S ~8 kHz CEILING WAS A q31 OVERFLOW ARTEFACT — `automodulator.cpp:1029` says so —
	// so ours is a fraction of the sample rate and RISES with it. Asserted as values at two rates,
	// because "it is clamped" is satisfied by the artefact this replaced.
	Filter at48;
	at48.prepare(48000.0);
	at48.setState(1.0e9f, 0.0f, { 1.0f, 0.0f, 0.0f });
	CHECK(at48.cutoffHz() == doctest::Approx(48000.0f * kFilterNyquistFraction));

	Filter at96;
	at96.prepare(96000.0);
	at96.setState(1.0e9f, 0.0f, { 1.0f, 0.0f, 0.0f });
	CHECK(at96.cutoffHz() == doctest::Approx(96000.0f * kFilterNyquistFraction));
	CHECK(at96.cutoffHz() > at48.cutoffHz());

	// And the bottom.
	at48.setState(-100.0f, 0.0f, { 1.0f, 0.0f, 0.0f });
	CHECK(at48.cutoffHz() == doctest::Approx(kFilterMinHz));
}

TEST_CASE("automod filter: a mix summing to nothing is silence, not a lowpass") {
	// THE HONEST ANSWER TO A DEGENERATE INPUT. Returning a lowpass would be inventing a sound nobody
	// asked for; dividing by the sum would be a NaN in the audio.
	Filter f = make(1000.0f, 0.3f, { 0.0f, 0.0f, 0.0f });
	const auto m = f.mixVectorForTest();
	CHECK(m[0] == 0.0f);
	CHECK(m[1] == 0.0f);
	CHECK(m[2] == 0.0f);

	const std::vector<float> out = runOurs(f, probe());
	for (float v : out) {
		CHECK(v == 0.0f);
	}
}

TEST_CASE("automod filter: non-finite settings cannot reach the kernel") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Filter f = make(1000.0f, 0.3f, { 1.0f, 0.0f, 0.0f });

	f.setState(nan, nan, { nan, nan, nan });
	CHECK(std::isfinite(f.cutoffHz()));
	CHECK(std::isfinite(f.resonance()));
	const auto m = f.mixVectorForTest();
	CHECK(std::isfinite(m[0]));
	CHECK(std::isfinite(m[1]));
	CHECK(std::isfinite(m[2]));

	// And a good neighbour, so "everything became zero" cannot pass for a fix.
	f.setState(1000.0f, 0.3f, { 1.0f, 0.0f, 0.0f });
	const std::vector<float> out = runOurs(f, probe());
	float energy = 0.0f;
	for (float v : out) {
		energy += v * v;
	}
	CHECK(energy > 0.01f);
}

TEST_CASE("automod filter: the drawn magnitude is the gain the real filter applies") {
	// **MEASURED AGAINST THE RUNNING FILTER, NOT CHECKED AGAINST ITS OWN ALGEBRA.** `magnitudeAt` has
	// no audio-path caller — the DSP never needs a magnitude — so nothing else in this repo would go
	// red if it stopped describing the filter that plays. This case is the caller that keeps it
	// honest: drive the real `Filter` with a sine, measure the steady-state amplitude, compare.
	//
	// `filter_card.cpp` records the same discipline for the multimode device's `gainProfile`: "the
	// module's own evaluator, checked against the measured impulse response of the real sample loop".
	constexpr double kRate = 48000.0;

	// THREE MIXES THAT ARE NOT EACH OTHER, and none of them a plain lowpass — a free (low, band,
	// high) vector is the whole reason this filter cannot borrow the multimode device's curve, so a
	// case that only checked a lowpass would not be testing the thing that is different.
	struct Case {
		const char* what;
		FilterMix mix;
		float cutoff;
		float res;
	};
	const Case cases[] = {
		{ "lowpass-ish", { 0.95f, 0.20f, 0.05f }, 800.0f, 0.30f },
		{ "band-heavy", { 0.10f, 0.90f, 0.15f }, 2000.0f, 0.55f },
		{ "high-heavy", { 0.05f, 0.25f, 0.92f }, 500.0f, 0.15f },
	};

	for (const Case& c : cases) {
		CAPTURE(c.what);
		Filter filter;
		filter.prepare(kRate);
		filter.setState(c.cutoff, c.res, c.mix);

		// THE WEIGHTS THE FILTER ACTUALLY KEPT, not the ones handed in. `setState` NORMALISES the mix
		// (constant power), so asking `magnitudeAt` about the raw vector would compare the drawing of
		// one filter against the sound of another — and it would agree to within the normalisation
		// factor, which is exactly the kind of near-miss that reads as a rounding difference.
		const FilterMix& kept = filter.mix();

		for (const float hz : { 60.0f, 150.0f, 400.0f, 800.0f, 1500.0f, 3000.0f, 7000.0f, 14000.0f }) {
			CAPTURE(hz);
			filter.reset();

			// SETTLE, THEN MEASURE. A resonant filter rings for many periods; measuring from the first
			// sample would read the transient and report a gain the filter never settles at.
			const double step = 2.0 * M_PI * static_cast<double>(hz) / kRate;
			const int settle = 20000;
			const int measure = 8000;
			float peak = 0.0f;
			for (int n = 0; n < settle + measure; ++n) {
				float l = static_cast<float>(std::sin(step * n));
				float r = l;
				filter.processSample(l, r);
				if (n >= settle) {
					peak = std::max(peak, std::abs(l));
				}
			}

			const float want = magnitudeAt(c.cutoff, c.res, kept, hz, kRate);
			// 4% OF THE MEASURED GAIN, PLUS AN ABSOLUTE FLOOR. The floor is what lets a deep stopband
			// be compared at all: at -60 dB a relative tolerance is comparing two numbers that are
			// both nearly zero, and a proportional bound there is far tighter than the measurement
			// deserves.
			const float tol = std::max(0.04f * peak, 0.002f);
			CHECK(std::abs(peak - want) <= tol);
		}
	}
}

TEST_CASE("automod filter: the magnitude is not a flat line, and it moves with every input") {
	// THE CASE ABOVE WOULD PASS IF BOTH SIDES WERE CONSTANT, so this is the half that says the
	// function has any shape at all — 8b-bis's "a case that cannot fail is decoration".
	constexpr double kRate = 48000.0;
	const FilterMix lp { 0.95f, 0.20f, 0.05f };

	// THE CORNER MOVES THE CURVE.
	CHECK(magnitudeAt(400.0f, 0.3f, lp, 2000.0f, kRate)
	      < magnitudeAt(4000.0f, 0.3f, lp, 2000.0f, kRate));
	// RESONANCE LIFTS THE CORNER.
	CHECK(magnitudeAt(1000.0f, 0.9f, lp, 1000.0f, kRate)
	      > magnitudeAt(1000.0f, 0.1f, lp, 1000.0f, kRate));
	// AND THE MIX DECIDES WHICH END SURVIVES: a high-heavy vector must pass 10 kHz better than a
	// low-heavy one, and lose at 60 Hz.
	const FilterMix hi { 0.05f, 0.25f, 0.92f };
	CHECK(magnitudeAt(1000.0f, 0.3f, hi, 10000.0f, kRate)
	      > magnitudeAt(1000.0f, 0.3f, lp, 10000.0f, kRate));
	CHECK(magnitudeAt(1000.0f, 0.3f, hi, 60.0f, kRate)
	      < magnitudeAt(1000.0f, 0.3f, lp, 60.0f, kRate));

	// A NON-FINITE FREQUENCY HAS NO GAIN TO REPORT AND MUST NOT PRODUCE ONE.
	CHECK(std::isfinite(magnitudeAt(1000.0f, 0.3f, lp, std::numeric_limits<float>::quiet_NaN(), kRate)));
	CHECK(std::isfinite(magnitudeAt(std::numeric_limits<float>::quiet_NaN(), 0.3f, lp, 1000.0f, kRate)));
}
