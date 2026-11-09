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

// THE SPECTRAL DRAWER'S OPERATORS, asserted by value against restatements written in this file.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/spectral_shaper.h"
#include "dsp/vox_anchors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace feathers::spectral;

namespace {
// A sweep that covers the low partials, off-grid fractions and the top of the spectrum.
constexpr double kKs[] = {1.0, 1.5, 2.0, 3.0, 7.25, 16.0, 63.0, 64.0, 100.0, 320.0};
constexpr double kF0s[] = {27.5, 110.0, 440.0, 3520.0};
} // namespace

TEST_CASE("a default drawer is the identity, bit for bit") {
	const ShaperParams p {};
	for (double f0 : kF0s) {
		for (double k : kKs) {
			CHECK(gain(p, k, k * f0, f0) == 1.0f);
			CHECK(formantRatio(p, k * f0) == 1.0);
		}
	}
}

TEST_CASE("a disabled drawer is the identity whatever its operators say") {
	ShaperParams p {};
	p.enabled = false;
	p.filter = {true, FilterMode::LowPass, 200.0f, 1.0f, 1.0f, 0.0f};
	p.comb = {true, 3.0f, 1.0f, 0.0f};
	p.tilt = {true, -6.0f, 1000.0f};
	p.oddEven = {true, 1.0f};
	p.formantShift = {true, 12.0f, 12.0f, 1000.0f, true};
	p.curve.on = true;
	p.curve.gain.fill(0.0f);
	for (double k : kKs) {
		CHECK(gain(p, k, k * 110.0, 110.0) == 1.0f);
		CHECK(formantRatio(p, k * 110.0) == 1.0);
	}
}

namespace {
double refLp(double x, double slope) {
	const double n = 1.0 + 15.0 * slope;
	return 1.0 / std::sqrt(1.0 + std::pow(x, 2.0 * n));
}
} // namespace

TEST_CASE("Filter: a lowpass is -3 dB at its cutoff and falls at its order") {
	FilterOp f {true, FilterMode::LowPass, 1000.0f, 0.0f, 0.0f, 0.0f};
	CHECK(std::abs(filterGain(f, 1000.0, 110.0) - 1.0 / std::sqrt(2.0)) < 1e-6);
	CHECK(std::abs(filterGain(f, 2000.0, 110.0) - 1.0 / std::sqrt(5.0)) < 1e-6); // order 1 at x = 2
	f.slope = 1.0f;
	CHECK(std::abs(filterGain(f, 2000.0, 110.0) - refLp(2.0, 1.0)) < 1e-6);
	CHECK(filterGain(f, 4000.0, 110.0) < 1e-9f); // order 16, two octaves up: brick-wall territory
}

TEST_CASE("Filter: a highpass mirrors the lowpass about its cutoff") {
	FilterOp lp {true, FilterMode::LowPass, 500.0f, 0.0f, 0.4f, 0.0f};
	FilterOp hp = lp;
	hp.mode = FilterMode::HighPass;
	for (double r : {0.25, 0.5, 2.0, 4.0}) {
		CHECK(std::abs(filterGain(hp, 500.0 * r, 110.0) - filterGain(lp, 500.0 / r, 110.0)) < 1e-6);
	}
}

TEST_CASE("Filter: a bandpass peaks at exactly 1 and a notch reaches exactly 0 at the cutoff") {
	FilterOp bp {true, FilterMode::BandPass, 800.0f, 0.0f, 0.5f, 0.0f};
	FilterOp notch = bp;
	notch.mode = FilterMode::Notch;
	CHECK(std::abs(filterGain(bp, 800.0, 110.0) - 1.0) < 1e-6);
	CHECK(std::abs(filterGain(notch, 800.0, 110.0)) < 1e-6);
}

TEST_CASE("Filter: resonance adds kMaxResonanceGain at the cutoff and nothing an octave away") {
	FilterOp f {true, FilterMode::LowPass, 1000.0f, 1.0f, 0.25f, 0.0f};
	CHECK(std::abs(filterGain(f, 1000.0, 110.0) - (1.0 / std::sqrt(2.0) + 3.0)) < 1e-6);
	const double away = 3.0 * std::exp(-1.0 / (2.0 * 0.15 * 0.15)); // one octave: e^-22, negligible
	CHECK(std::abs(filterGain(f, 2000.0, 110.0) - (refLp(2.0, 0.25) + away)) < 1e-6);
}

TEST_CASE("Filter: key track moves the cutoff with the note, pivoting at middle C") {
	FilterOp tracked {true, FilterMode::LowPass, 1000.0f, 0.0f, 0.25f, 1.0f};
	FilterOp fixedCutoff = tracked;
	fixedCutoff.keyTrack = 0.0f;
	const double c = 261.6255653005986;
	// A note an octave above middle C doubles the cutoff, so 2 kHz on it is where 1 kHz was.
	CHECK(std::abs(filterGain(tracked, 2000.0, 2.0 * c) - filterGain(fixedCutoff, 1000.0, c)) < 1e-6);
	// At middle C, tracking changes nothing.
	CHECK(std::abs(filterGain(tracked, 1500.0, c) - filterGain(fixedCutoff, 1500.0, c)) < 1e-6);
}

TEST_CASE("Comb: full depth puts zeros every `spacing` harmonics, and phase slides them") {
	CombOp c {true, 3.0f, 1.0f, 0.0f};
	CHECK(std::abs(combGain(c, 3.0)) < 1e-6);
	CHECK(std::abs(combGain(c, 6.0)) < 1e-6);
	CHECK(std::abs(combGain(c, 1.5) - 1.0) < 1e-6); // half-way between teeth
	c.phase = 1.0f / 3.0f;                           // slide a whole harmonic
	CHECK(std::abs(combGain(c, 2.0)) < 1e-6);
	c.depth = 0.5f;
	c.phase = 0.0f;
	CHECK(std::abs(combGain(c, 3.0) - 0.5) < 1e-6);
}

TEST_CASE("Tilt: dB per octave about the pivot") {
	TiltOp t {true, -6.0f, 1000.0f};
	CHECK(std::abs(tiltGain(t, 1000.0) - 1.0) < 1e-6);
	CHECK(std::abs(20.0 * std::log10(tiltGain(t, 2000.0)) - (-6.0)) < 1e-4);
	CHECK(std::abs(20.0 * std::log10(tiltGain(t, 250.0)) - 12.0) < 1e-4);
}

TEST_CASE("Odd/Even: -1 keeps the odd harmonics, +1 the even, 0 both, rounding off-grid partials") {
	OddEvenOp o {true, -1.0f};
	CHECK(oddEvenGain(o, 1.0) == 1.0f);
	CHECK(oddEvenGain(o, 2.0) == 0.0f);
	CHECK(oddEvenGain(o, 2.2) == 0.0f); // rounds to 2
	o.balance = 1.0f;
	CHECK(oddEvenGain(o, 3.0) == 0.0f);
	CHECK(oddEvenGain(o, 4.0) == 1.0f);
	o.balance = 0.0f;
	CHECK(oddEvenGain(o, 3.0) == 1.0f);
	CHECK(oddEvenGain(o, 4.0) == 1.0f);
	o.balance = -0.5f;
	CHECK(std::abs(oddEvenGain(o, 4.0) - 0.5) < 1e-6);
}

TEST_CASE("Drawn curve: band b is harmonic b+1, linear between, the last band holds above 64") {
	DrawnCurve c {};
	c.on = true;
	c.gain[0] = 0.0f;
	c.gain[1] = 1.0f;
	c.gain[63] = 0.25f;
	CHECK(curveGain(c, 1.0) == 0.0f);
	CHECK(std::abs(curveGain(c, 1.5) - 0.5) < 1e-6);
	CHECK(curveGain(c, 64.0) == 0.25f);
	CHECK(curveGain(c, 200.0) == 0.25f);
}

TEST_CASE("the operators multiply") {
	ShaperParams p {};
	p.tilt = {true, -6.0f, 1000.0f};
	p.comb = {true, 4.0f, 0.5f, 0.1f};
	const double k = 5.0, f0 = 220.0;
	const double want = static_cast<double>(tiltGain(p.tilt, k * f0)) * static_cast<double>(combGain(p.comb, k));
	CHECK(std::abs(gain(p, k, k * f0, f0) - want) < 1e-6);
}

TEST_CASE("Formant Shift: linked, one ratio everywhere, 2^(semitones/12)") {
	ShaperParams p {};
	p.formantShift = {true, 12.0f, -24.0f, 1000.0f, true}; // high is ignored while linked
	for (double hz : {50.0, 1000.0, 15000.0}) {
		CHECK(std::abs(formantRatio(p, hz) - 2.0) < 1e-12);
	}
}

TEST_CASE("Formant Shift: unlinked, each band its own ratio, blended geometrically across the crossover") {
	ShaperParams p {};
	p.formantShift = {true, 12.0f, -12.0f, 1000.0f, false};
	const double width = 2.0 / 3.0; // the blend's TOTAL width, centred on the crossover
	// At the blend's lower edge, the low ratio exactly; at its upper edge, the high ratio exactly.
	CHECK(std::abs(formantRatio(p, 1000.0 * std::exp2(-width / 2.0)) - 2.0) < 1e-12);
	CHECK(std::abs(formantRatio(p, 1000.0 * std::exp2(width / 2.0)) - 0.5) < 1e-12);
	// At the crossover, the geometric mean: sqrt(2 * 0.5) = 1.
	CHECK(std::abs(formantRatio(p, 1000.0) - 1.0) < 1e-12);
	// A quarter-width above it, t = smoothstep(0.75) = 0.84375, and 2^(1-t) * 0.5^t = 2^(1 - 2t).
	CHECK(std::abs(formantRatio(p, 1000.0 * std::exp2(width / 4.0)) - std::exp2(1.0 - 2.0 * 0.84375)) < 1e-12);
}

// ---- The second round: the cases that pin what the first left loose -------------------------------------

namespace {
// Every operator on and away from neutral; Formant Shift unlinked.
ShaperParams allOn(FilterMode mode) {
	ShaperParams p {};
	p.filter = {true, mode, 2000.0f, 0.6f, 0.3f, 0.5f};
	p.comb = {true, 3.5f, 0.7f, 0.2f};
	p.tilt = {true, -4.5f, 800.0f};
	p.oddEven = {true, 0.3f};
	p.formantShift = {true, 7.0f, -5.0f, 1500.0f, false};
	p.curve.on = true;
	for (int b = 0; b < kCurveBands; ++b) {
		p.curve.gain[static_cast<std::size_t>(b)] = 0.6f + 0.4f * std::cos(0.3f * static_cast<float>(b));
	}
	return p;
}

constexpr int kPartials = 320;

struct Spectrum {
	float k[kPartials];
	float hz[kPartials];
	float base[kPartials];
};

Spectrum harmonics(double f0, int count = kPartials) {
	Spectrum s {};
	for (int i = 0; i < count; ++i) {
		s.k[i] = static_cast<float>(i + 1);
		s.hz[i] = static_cast<float>((i + 1) * f0);
		s.base[i] = 1.0f / static_cast<float>(i + 1); // saw-like
	}
	return s;
}

bool allExactlyOne(const float* v, int n) {
	for (int i = 0; i < n; ++i) {
		if (v[i] != 1.0f) {
			return false;
		}
	}
	return true;
}

double relErr(double got, double want) {
	return std::abs(got - want) / std::abs(want);
}
} // namespace

TEST_CASE("the batch equals the reference: every operator but Tilt on, each switched off in turn, linked and unlinked Formant Shift, every filter mode, k = 1..320") {
	// Variant 0: every operator but Tilt on. 1-4: one amplitude operator switched OFF with its settings left
	// non-neutral, so a prepare() that ignores an on-switch is caught. 5: Formant Shift LINKED with high != low.
	// Tilt stays off throughout (with its settings non-neutral): the batch compensates Tilt and the reference
	// cannot, so Tilt has its own cases, and a prepare() that switches it on regardless is caught here.
	int flushed = 0;
	for (FilterMode mode : {FilterMode::LowPass, FilterMode::HighPass, FilterMode::BandPass, FilterMode::Notch}) {
		for (int variant = 0; variant <= 5; ++variant) {
			ShaperParams p = allOn(mode);
			p.tilt.on = false;
			p.filter.on = variant != 1;
			p.comb.on = variant != 2;
			p.oddEven.on = variant != 3;
			p.curve.on = variant != 4;
			p.formantShift.link = variant == 5;
			for (double f0 : {27.5, 110.0, 440.0, 3520.0}) {
				const Spectrum s = harmonics(f0);
				const PreparedShaper prepared = prepare(p, f0);
				float out[kPartials];
				float ratios[kPartials];
				gains(prepared, s.k, s.hz, s.base, kPartials, out);
				formantRatios(prepared, s.hz, kPartials, ratios);
				for (int i = 0; i < kPartials; ++i) {
					CAPTURE(variant);
					CAPTURE(f0);
					CAPTURE(i);
					const double want = gain(p, s.k[i], s.hz[i], f0);
					const double got = out[i];
					if (want < 1e-3) {
						CHECK(std::abs(got - want) <= 1e-7);
					} else {
						CHECK(relErr(got, want) <= 1e-5);
					}
					// The flush: below 1e-7 exactly 0, and nothing non-zero below it.
					if (want < 0.99e-7) {
						CHECK(got == 0.0);
					}
					if (got != 0.0) {
						CHECK(got >= 1e-7);
					} else {
						++flushed;
					}
					CHECK(relErr(ratios[i], formantRatio(p, s.hz[i])) <= 1e-5);
				}
			}
		}
	}
	CHECK(flushed > 0); // the sweep reaches the flush, so the flush assertions are not vacuous
}

TEST_CASE("the batch's identity is exact: default, every operator off, and disabled") {
	const Spectrum s = harmonics(110.0);
	float out[kPartials];
	float ratios[kPartials];

	const PreparedShaper byDefault = prepare(ShaperParams {}, 110.0);
	gains(byDefault, s.k, s.hz, s.base, kPartials, out);
	formantRatios(byDefault, s.hz, kPartials, ratios);
	CHECK(allExactlyOne(out, kPartials));
	CHECK(allExactlyOne(ratios, kPartials));

	ShaperParams off = allOn(FilterMode::LowPass);
	off.filter.on = off.comb.on = off.tilt.on = off.oddEven.on = off.formantShift.on = off.curve.on = false;
	const PreparedShaper everyOff = prepare(off, 110.0);
	gains(everyOff, s.k, s.hz, s.base, kPartials, out);
	formantRatios(everyOff, s.hz, kPartials, ratios);
	CHECK(allExactlyOne(out, kPartials));
	CHECK(allExactlyOne(ratios, kPartials));

	ShaperParams disabled = allOn(FilterMode::LowPass);
	disabled.enabled = false;
	const PreparedShaper whenDisabled = prepare(disabled, 110.0);
	gains(whenDisabled, s.k, s.hz, s.base, kPartials, out);
	formantRatios(whenDisabled, s.hz, kPartials, ratios);
	CHECK(allExactlyOne(out, kPartials));
	CHECK(allExactlyOne(ratios, kPartials));
}

TEST_CASE("gain() reaches every operator, and switching one off removes exactly its factor") {
	const ShaperParams p = allOn(FilterMode::LowPass);
	const double f0 = 220.0;
	for (int n = 1; n <= 24; ++n) {
		CAPTURE(n);
		const double k = n, hz = n * f0;
		const float factors[5] = {filterGain(p.filter, hz, f0), combGain(p.comb, k), tiltGain(p.tilt, hz),
		                          oddEvenGain(p.oddEven, k), curveGain(p.curve, k)};
		float all = 1.0f;
		for (float f : factors) {
			all *= f;
		}
		CHECK(relErr(gain(p, k, hz, f0), all) <= 1e-6);
		for (int off = 0; off < 5; ++off) {
			CAPTURE(off);
			ShaperParams q = p;
			bool* switches[5] = {&q.filter.on, &q.comb.on, &q.tilt.on, &q.oddEven.on, &q.curve.on};
			*switches[off] = false;
			float without = 1.0f;
			for (int j = 0; j < 5; ++j) {
				if (j != off) {
					without *= factors[j];
				}
			}
			CHECK(relErr(gain(q, k, hz, f0), without) <= 1e-6);
		}
	}
}

TEST_CASE("Filter: bandpass and notch one octave either side of the cutoff, and they sum to 1") {
	const double s = 0.5;
	FilterOp bp {true, FilterMode::BandPass, 800.0f, 0.0f, static_cast<float>(s), 0.0f};
	FilterOp notch = bp;
	notch.mode = FilterMode::Notch;
	const double want = 2.0 * refLp(2.0, s) * refLp(0.5, s);
	for (double hz : {400.0, 1600.0}) {
		CHECK(std::abs(filterGain(bp, hz, 110.0) - want) < 1e-6);
		CHECK(std::abs(filterGain(notch, hz, 110.0) - (1.0 - want)) < 1e-6);
	}
	for (double hz = 50.0; hz < 20000.0; hz *= 1.25) {
		CHECK(std::abs(filterGain(bp, hz, 110.0) + filterGain(notch, hz, 110.0) - 1.0) < 1e-6);
	}
}

TEST_CASE("Filter: resonance's width and linear scale, and a Notch ignores resonance") {
	FilterOp f {true, FilterMode::LowPass, 1000.0f, 0.5f, 0.25f, 0.0f};
	const double x = std::exp2(0.15); // one width above the cutoff: the bump is e^-1/2 of its peak
	CHECK(std::abs(filterGain(f, 1000.0 * x, 110.0) - (refLp(x, 0.25) + 1.5 * std::exp(-0.5))) < 1e-6);
	FilterOp notch {true, FilterMode::Notch, 1000.0f, 1.0f, 0.25f, 0.0f};
	CHECK(std::abs(filterGain(notch, 1000.0, 110.0)) < 1e-6);
}

TEST_CASE("Filter: key track 0.5 moves the cutoff by sqrt(2) for a note an octave above the pivot") {
	FilterOp tracked {true, FilterMode::LowPass, 1000.0f, 0.0f, 0.25f, 0.5f};
	FilterOp fixedCutoff = tracked;
	fixedCutoff.keyTrack = 0.0f;
	const double c = 261.6255653005986;
	CHECK(std::abs(filterGain(tracked, 1000.0 * std::sqrt(2.0), 2.0 * c) - filterGain(fixedCutoff, 1000.0, c)) < 1e-6);
}

TEST_CASE("bounds: every control clamped alike on both roads, Tilt finite, NaN and huge k safe") {
	// Tilt's ratio stops at 2^-12 and 2^12, so even a steep tilt far outside the audio band stays finite.
	// At the slope's clamp edge, ±48 dB/oct, the factor is 2^(±12 · 48/6.02): about 2^±96, finite.
	TiltOp t {true, -48.0f, 1000.0f};
	const double exponent = -48.0 / 6.020599913279624;
	CHECK(relErr(tiltGain(t, 1e-9), std::pow(std::exp2(-12.0), exponent)) < 1e-6);
	CHECK(relErr(tiltGain(t, 1e12), std::pow(std::exp2(12.0), exponent)) < 1e-6);
	CHECK(std::isfinite(tiltGain(t, 0.0)));
	// Beyond it, 200 dB/oct with the pivot at 1 Hz (where an unclamped slope reaches inf): finite, and exactly
	// the ±48 result.
	for (float db : {200.0f, -200.0f}) {
		CAPTURE(db);
		const TiltOp steep {true, db, 1.0f};
		const TiltOp edge {true, db > 0.0f ? 48.0f : -48.0f, 1.0f};
		for (double hz : {1e-3, 1e9}) {
			CHECK(std::isfinite(tiltGain(steep, hz)));
			CHECK(tiltGain(steep, hz) == tiltGain(edge, hz));
		}
	}
	// The batch, at 200 dB/oct: finite, and its shape is the ±48 reference's.
	{
		ShaperParams steep {};
		steep.tilt = {true, 200.0f, 1.0f};
		const Spectrum s = harmonics(110.0, 64);
		float out[64];
		gains(prepare(steep, 110.0), s.k, s.hz, s.base, 64, out);
		const TiltOp edge {true, 48.0f, 1.0f};
		for (int i = 0; i < 64; ++i) {
			CHECK(std::isfinite(out[i]));
		}
		CHECK(relErr(static_cast<double>(out[63]) / static_cast<double>(out[31]),
		             static_cast<double>(tiltGain(edge, s.hz[63])) / static_cast<double>(tiltGain(edge, s.hz[31])))
		      <= 1e-5);
	}
	// A NaN harmonic index reads as harmonic 1 on the harmonic-indexed operators.
	const CombOp comb {true, 3.0f, 0.8f, 0.1f};
	CHECK(combGain(comb, std::nan("")) == combGain(comb, 1.0));
	const OddEvenOp oddEven {true, 0.5f};
	CHECK(oddEvenGain(oddEven, std::nan("")) == oddEvenGain(oddEven, 1.0));

	DrawnCurve c {};
	c.gain[0] = 0.5f;
	c.gain[63] = 0.25f;
	CHECK(curveGain(c, std::nan("")) == 0.5f);
	CHECK(curveGain(c, 1e30) == 0.25f);

	CHECK(combGain({true, 3.0f, 2.0f, 0.0f}, 3.0) == combGain({true, 3.0f, 1.0f, 0.0f}, 3.0));
	CHECK(filterGain({true, FilterMode::LowPass, 1000.0f, 5.0f, 0.25f, 0.0f}, 1000.0, 110.0)
	      == filterGain({true, FilterMode::LowPass, 1000.0f, 1.0f, 0.25f, 0.0f}, 1000.0, 110.0));
	CHECK(std::isfinite(filterGain({true, FilterMode::BandPass, 1000.0f, 0.5f, 0.25f, 0.0f}, std::nan(""), 110.0)));

	// The batch agrees with the reference on out-of-range settings: its clamps are the reference's.
	ShaperParams p {};
	p.filter = {true, FilterMode::BandPass, -50.0f, 4.0f, 3.0f, 2.0f};
	p.comb = {true, 0.25f, -1.0f, 0.3f};
	p.formantShift = {true, 5.0f, -7.0f, 0.0f, false};
	const Spectrum s = harmonics(110.0, 64);
	const PreparedShaper prepared = prepare(p, 110.0);
	float out[64];
	float ratios[64];
	gains(prepared, s.k, s.hz, s.base, 64, out);
	formantRatios(prepared, s.hz, 64, ratios);
	for (int i = 0; i < 64; ++i) {
		CAPTURE(i);
		const double want = gain(p, s.k[i], s.hz[i], 110.0);
		CHECK((want < 1e-3 ? std::abs(out[i] - want) <= 1e-7 : relErr(out[i], want) <= 1e-5));
		CHECK(relErr(ratios[i], formantRatio(p, s.hz[i])) <= 1e-5);
	}
}

namespace {
double power(const float* gain, const float* base, int n) {
	double sum = 0.0;
	for (int i = 0; i < n; ++i) {
		const double a = static_cast<double>(gain[i]) * static_cast<double>(base[i]);
		sum += a * a;
	}
	return sum;
}
} // namespace

TEST_CASE("Tilt keeps the power: it changes colour, not loudness") {
	const Spectrum s = harmonics(110.0, 64);
	float ones[64];
	std::fill(ones, ones + 64, 1.0f);
	const double before = power(ones, s.base, 64);
	for (float db : {-6.0f, 6.0f}) {
		CAPTURE(db);
		ShaperParams p {};
		p.tilt = {true, db, 1000.0f};
		float out[64];
		gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
		CHECK(relErr(power(out, s.base, 64), before) <= 1e-5);
		CHECK(std::abs(out[63] / out[0] - 1.0f) > 0.5f); // and it did tilt: not vacuous
	}
}

TEST_CASE("Tilt's SHAPE is the reference tilt: compensation is one scale for every partial") {
	const Spectrum s = harmonics(110.0, 64);
	ShaperParams p {};
	p.tilt = {true, -6.0f, 1000.0f};
	float out[64];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	for (const auto& [i, j] : {std::pair {0, 1}, std::pair {2, 16}, std::pair {9, 63}, std::pair {40, 5}}) {
		CAPTURE(i);
		CAPTURE(j);
		const double want = static_cast<double>(tiltGain(p.tilt, s.hz[i])) / static_cast<double>(tiltGain(p.tilt, s.hz[j]));
		CHECK(relErr(static_cast<double>(out[i]) / static_cast<double>(out[j]), want) <= 1e-5);
	}
}

TEST_CASE("Tilt compensation sees the other operators: the power kept is base times filter, not base") {
	const Spectrum s = harmonics(110.0, 64);
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 1000.0f, 0.0f, 0.25f, 0.0f};
	p.tilt = {true, -6.0f, 1000.0f};
	float filtered[64];
	for (int i = 0; i < 64; ++i) {
		filtered[i] = filterGain(p.filter, s.hz[i], 110.0);
	}
	float out[64];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	const double kept = power(filtered, s.base, 64);
	float ones[64];
	std::fill(ones, ones + 64, 1.0f);
	CHECK(relErr(power(out, s.base, 64), kept) <= 1e-5);
	CHECK(relErr(kept, power(ones, s.base, 64)) > 0.01); // the filter changed the power, so the two differ
}

TEST_CASE("Tilt over a silent base: no power to hold, so c = 1 and the gains are the uncompensated ones") {
	Spectrum s = harmonics(110.0, 64);
	std::fill(s.base, s.base + 64, 0.0f);
	ShaperParams p {};
	p.tilt = {true, -6.0f, 1000.0f};
	float out[64];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	for (int i = 0; i < 64; ++i) {
		CAPTURE(i);
		CHECK(std::isfinite(out[i]));
		CHECK(relErr(out[i], gain(p, s.k[i], s.hz[i], 110.0)) <= 1e-5);
	}
}

TEST_CASE("Tilt's path flushes too: after compensation, a gain below 1e-7 is exactly 0") {
	const Spectrum s = harmonics(110.0, 64);
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 500.0f, 0.0f, 1.0f, 0.0f}; // order 16: the top partials fall far below
	p.tilt = {true, -6.0f, 1000.0f};
	float out[64];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	int zeros = 0;
	for (int i = 0; i < 64; ++i) {
		CAPTURE(i);
		if (out[i] == 0.0f) {
			++zeros;
		} else {
			CHECK(out[i] >= 1e-7f);
		}
	}
	CHECK(zeros > 0);
	CHECK(out[0] > 0.1f); // the passband survives: not every partial flushed
}

// ---- The kernels and the cache (backlog LY, the drawer's cost) -------------------------------------------------
//
// The batch's per-partial work is polynomial log2/exp2/cos in vectors (spectral_shaper.cpp states each truncation
// bound), and the voice takes it through GainCache. So: each kernel against the reference, per operator, as a MAXIMUM
// ERROR IN dB over a sweep of k, f0 and settings; the cache against gains() to the bit through every change that must
// reach it; and the work the cache does, by its own counters.

namespace {
// The sweep's frequencies: every harmonic of each f0 up to 24 kHz, and the same at an off-grid ratio (k + 0.37).
struct Sweep {
	std::vector<float> k, hz;
	std::vector<double> f0;
};

Sweep sweepOf(std::initializer_list<double> f0s) {
	Sweep s;
	for (double f0 : f0s) {
		for (int i = 1; i <= kPartials && i * f0 < 24000.0; ++i) {
			for (double frac : {0.0, 0.37}) {
				s.k.push_back(static_cast<float>(i + frac));
				s.hz.push_back(static_cast<float>((i + frac) * f0));
				s.f0.push_back(f0);
			}
		}
	}
	return s;
}

/// |20·log10(got / want)|; an error against a value at or below the flush (where dB means nothing) is held absolutely
/// by the caller instead.
double dbError(double got, double want) {
	return std::abs(20.0 * std::log10(got / want));
}

// THE BOUNDS ASSERTED (dB), each ~10x the maximum measured on 2026-10-10 (MESSAGE prints it): Filter 5.2e-7 dB, where
// the error is the polynomials' (~1e-9 relative) magnified near a Notch's zero and by a steep slope; Comb 8.0e-7 dB,
// Odd/Even, the curve and Tilt 0 — those three factors are floats, and the polynomials' error is below a float's half
// ulp nearly everywhere, so the kernel rounds to the reference's own float. Both bounds sit far below anything
// audible (1e-5 dB is a gain error of 1.2e-6) and below the batch-vs-reference cases' 1e-5 relative.
constexpr double kFilterMaxDb = 5e-6;
constexpr double kFloatFactorMaxDb = 1e-5;
constexpr double kBelowFlushAbs = 1e-12; ///< where the reference is below kFlushGain: an absolute bound
} // namespace

TEST_CASE("the kernels are the reference per operator, in dB: Filter in every mode, slope, resonance, cutoff and key track") {
	const Sweep sw = sweepOf({27.5, 110.0, 440.0, 3520.0});
	double worst = 0.0;
	int checked = 0, below = 0;
	for (FilterMode mode : {FilterMode::LowPass, FilterMode::HighPass, FilterMode::BandPass, FilterMode::Notch}) {
		for (float cutoff : {50.0f, 800.0f, 5000.0f, 18000.0f}) {
			for (float res : {0.0f, 0.6f, 1.0f}) {
				for (float slope : {0.0f, 0.4f, 1.0f}) {
					for (float track : {0.0f, 1.0f}) {
						const FilterOp op {true, mode, cutoff, res, slope, track};
						ShaperParams p {};
						p.filter = op;
						for (std::size_t i = 0; i < sw.k.size();) {
							// one f0 at a time: prepare() reads it (key track)
							const double f0 = sw.f0[i];
							std::size_t n = 0;
							while (i + n < sw.k.size() && sw.f0[i + n] == f0) {
								++n;
							}
							std::vector<double> got(n);
							// NEAR AN OPEN END THE KERNEL FADES THE FILTER OUT, AND AT IT RUNS NONE (filterOpenness, Z7d) — the
							// key-tracked corner of 18 kHz at 3520 Hz, or of 50 Hz at 27.5 Hz — so this SHAPE comparison is
							// made away from the ends; the fade has its own case ("Z7d: the filter's open end", below).
							if (const double open = filterOpenness(op, f0); open > 0.0) {
								CHECK(prepare(p, f0).filter.on == (open < 1.0));
								i += n;
								continue;
							}
							filterFactors(prepare(p, f0).filter, sw.hz.data() + i, static_cast<int>(n), got.data());
							for (std::size_t j = 0; j < n; ++j) {
								const double want = filterGain(op, sw.hz[i + j], f0);
								if (want <= static_cast<double>(kFlushGain)) {
									CHECK(std::abs(got[j] - want) <= kBelowFlushAbs);
									++below;
									continue;
								}
								worst = std::max(worst, dbError(got[j], want));
								++checked;
							}
							i += n;
						}
					}
				}
			}
		}
	}
	MESSAGE("Filter: worst " << worst << " dB over " << checked << " factors above the flush (" << below << " below)");
	CHECK(worst <= kFilterMaxDb);
	CHECK(checked > 10000);
	CHECK(below > 1000); // the sweep reaches the flush, so the absolute bound is not vacuous
}

TEST_CASE("the kernels are the reference per operator, in dB: Comb, Odd/Even, the curve, and Tilt (uncompensated)") {
	const Sweep sw = sweepOf({27.5, 110.0, 440.0, 3520.0});
	const int n = static_cast<int>(sw.k.size());
	std::vector<float> got(sw.k.size());
	double worstComb = 0.0, worstOddEven = 0.0, worstCurve = 0.0, worstTilt = 0.0;
	int combZeros = 0;
	for (float spacing : {1.0f, 2.5f, 3.0f, 7.0f}) {
		for (float depth : {0.3f, 0.7f, 1.0f}) {
			for (float phase : {0.0f, 0.2f, 0.75f, 3.4f, -1.3f}) {
				ShaperParams p {};
				p.comb = {true, spacing, depth, phase};
				harmonicFactors(prepare(p, 110.0).harmonic, sw.k.data(), n, got.data());
				for (int i = 0; i < n; ++i) {
					const double want = combGain(p.comb, sw.k[size_t(i)]);
					if (want <= static_cast<double>(kFlushGain)) {
						CHECK(std::abs(got[size_t(i)] - want) <= 1e-9); // a float's rounding of a value near 1, cancelled
						++combZeros;
						continue;
					}
					worstComb = std::max(worstComb, dbError(got[size_t(i)], want));
				}
			}
		}
	}
	for (float balance : {-1.0f, -0.4f, 0.0f, 0.6f, 1.0f}) {
		ShaperParams p {};
		p.oddEven = {true, balance};
		harmonicFactors(prepare(p, 110.0).harmonic, sw.k.data(), n, got.data());
		for (int i = 0; i < n; ++i) {
			const double want = oddEvenGain(p.oddEven, sw.k[size_t(i)]);
			if (want == 0.0) {
				CHECK(got[size_t(i)] == 0.0f);
				continue;
			}
			worstOddEven = std::max(worstOddEven, dbError(got[size_t(i)], want));
		}
	}
	{
		ShaperParams p {};
		p.curve.on = true;
		for (int b = 0; b < kCurveBands; ++b) {
			p.curve.gain[size_t(b)] = 0.05f + 0.9f * std::abs(std::sin(0.37f * static_cast<float>(b)));
		}
		harmonicFactors(prepare(p, 110.0).harmonic, sw.k.data(), n, got.data());
		for (int i = 0; i < n; ++i) {
			worstCurve = std::max(worstCurve, dbError(got[size_t(i)], curveGain(p.curve, sw.k[size_t(i)])));
		}
	}
	for (float db : {-48.0f, -12.0f, -3.0f, 3.0f, 12.0f, 48.0f}) {
		for (float pivot : {50.0f, 1000.0f, 8000.0f}) {
			ShaperParams p {};
			p.tilt = {true, db, pivot};
			tiltFactors(prepare(p, 110.0).tilt, sw.hz.data(), n, got.data());
			for (int i = 0; i < n; ++i) {
				worstTilt = std::max(worstTilt, dbError(got[size_t(i)], tiltGain(p.tilt, sw.hz[size_t(i)])));
			}
		}
	}
	MESSAGE("Comb " << worstComb << " dB (" << combZeros << " teeth at the flush), Odd/Even " << worstOddEven
	                << " dB, curve " << worstCurve << " dB, Tilt " << worstTilt << " dB");
	CHECK(worstComb <= kFloatFactorMaxDb);
	CHECK(worstOddEven <= kFloatFactorMaxDb);
	CHECK(worstCurve <= kFloatFactorMaxDb);
	CHECK(worstTilt <= kFloatFactorMaxDb);
	CHECK(combZeros > 0); // full depth at spacing 1 or 2.5 lands teeth on harmonics: the near-zero branch is reached
}

TEST_CASE("the kernels: the index road is the k road at k = i + 1, and the fused Filter+Tilt pass is the two passes, to the bit") {
	const ShaperParams p = allOn(FilterMode::BandPass);
	const PreparedShaper s = prepare(p, 196.0);
	const Spectrum sp = harmonics(196.0);
	float byK[kPartials], byIndex[kPartials];
	harmonicFactors(s.harmonic, sp.k, kPartials, byK);
	harmonicFactorsAtIndex(s.harmonic, kPartials, byIndex);
	double fAlone[kPartials], fFused[kPartials];
	float tAlone[kPartials], tFused[kPartials];
	filterFactors(s.filter, sp.hz, kPartials, fAlone);
	tiltFactors(s.tilt, sp.hz, kPartials, tAlone);
	filterTiltFactors(s.filter, s.tilt, sp.hz, kPartials, fFused, tFused);
	for (int i = 0; i < kPartials; ++i) {
		CAPTURE(i);
		CHECK(byK[i] == byIndex[i]);
		CHECK(fAlone[i] == fFused[i]);
		CHECK(tAlone[i] == tFused[i]);
	}
}

namespace {
/// One cache step: update, then apply against gains() over the same inputs, every output compared BITWISE.
template <int N>
void checkCacheIsGains(GainCache<N>& cache, const ShaperParams& p, double f0, const Spectrum& s, int count) {
	cache.update(p, f0, s.hz, count);
	const PreparedShaper fresh = prepare(p, f0);
	REQUIRE(cache.prepared().identity == fresh.identity);
	if (fresh.identity) {
		return; // the voice does not apply an identity shaper (its gain is exactly 1, which gains() writes)
	}
	float got[kPartials], want[kPartials];
	cache.apply(s.base, count, got);
	gains(fresh, s.k, s.hz, s.base, count, want);
	int differ = 0;
	for (int i = 0; i < count; ++i) {
		differ += std::memcmp(&got[i], &want[i], sizeof(float)) != 0 ? 1 : 0;
	}
	CHECK(differ == 0);
}
} // namespace

TEST_CASE("GainCache is gains() to the bit through every change that must reach it: each control, each switch, f0, the frequencies, the count") {
	GainCache<kPartials> cache;
	ShaperParams p = allOn(FilterMode::LowPass);
	p.formantShift.on = false;
	double f0 = 110.0;
	Spectrum s = harmonics(f0);
	int count = kPartials;
	checkCacheIsGains(cache, p, f0, s, count);

	// Each change, one at a time, every one of which moves the gains (asserted below, so no step is vacuous).
	struct Step {
		const char* what;
		void (*apply)(ShaperParams&, double&, Spectrum&, int&);
	};
	const Step steps[] = {
		{"filter cutoff", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.cutoffHz *= 1.5f; }},
		{"filter resonance", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.resonance = 0.95f; }},
		{"filter slope", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.slope = 0.8f; }},
		{"filter key track", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.keyTrack = 0.9f; }},
		{"filter mode", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.mode = FilterMode::HighPass; }},
		{"comb spacing", [](ShaperParams& q, double&, Spectrum&, int&) { q.comb.spacing = 4.5f; }},
		{"comb depth", [](ShaperParams& q, double&, Spectrum&, int&) { q.comb.depth = 0.2f; }},
		{"comb phase", [](ShaperParams& q, double&, Spectrum&, int&) { q.comb.phase = 0.45f; }},
		{"tilt slope", [](ShaperParams& q, double&, Spectrum&, int&) { q.tilt.dbPerOctave = 7.0f; }},
		{"tilt pivot", [](ShaperParams& q, double&, Spectrum&, int&) { q.tilt.pivotHz = 300.0f; }},
		{"odd/even balance", [](ShaperParams& q, double&, Spectrum&, int&) { q.oddEven.balance = 0.8f; }},
		{"a curve band", [](ShaperParams& q, double&, Spectrum&, int&) { q.curve.gain[5] = 0.1f; }},
		{"f0 with key track (the frequencies move with it)",
		 [](ShaperParams&, double& f, Spectrum& sp, int&) {
			 f = 123.0;
			 sp = harmonics(f);
		 }},
		{"f0 alone, the frequencies held (only key track can see it)", [](ShaperParams&, double& f, Spectrum&, int&) { f = 140.0; }},
		{"the frequencies alone, off-grid (f0 held)",
		 [](ShaperParams&, double&, Spectrum& sp, int&) {
			 for (int i = 0; i < kPartials; ++i) {
				 sp.hz[i] *= 1.0f + 0.002f * static_cast<float>(i % 7);
			 }
		 }},
		{"the count shrinks", [](ShaperParams&, double&, Spectrum&, int& c) { c = 200; }},
		{"the count grows past what was held", [](ShaperParams&, double&, Spectrum&, int& c) { c = kPartials; }},
		{"filter off", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.on = false; }},
		{"comb off", [](ShaperParams& q, double&, Spectrum&, int&) { q.comb.on = false; }},
		{"tilt off", [](ShaperParams& q, double&, Spectrum&, int&) { q.tilt.on = false; }},
		{"filter back on", [](ShaperParams& q, double&, Spectrum&, int&) { q.filter.on = true; }},
		{"tilt back on", [](ShaperParams& q, double&, Spectrum&, int&) { q.tilt.on = true; }},
		{"the drawer disabled", [](ShaperParams& q, double&, Spectrum&, int&) { q.enabled = false; }},
		{"the drawer enabled again", [](ShaperParams& q, double&, Spectrum&, int&) { q.enabled = true; }},
	};
	for (const Step& step : steps) {
		INFO(step.what);
		const ShaperParams before = p;
		const double f0Before = f0;
		const Spectrum sBefore = s;
		const int countBefore = count;
		step.apply(p, f0, s, count);
		// Not vacuous: the change moves gains() (or the identity flag) for the partials both counts cover.
		{
			const int n = std::min(count, countBefore);
			float a[kPartials], b[kPartials];
			const PreparedShaper pa = prepare(before, f0Before), pb = prepare(p, f0);
			gains(pa, sBefore.k, sBefore.hz, sBefore.base, n, a);
			gains(pb, s.k, s.hz, s.base, n, b);
			bool moved = pa.identity != pb.identity || count != countBefore;
			for (int i = 0; i < n && !moved; ++i) {
				moved = a[i] != b[i];
			}
			CHECK(moved);
		}
		checkCacheIsGains(cache, p, f0, s, count);
		// The same inputs again: nothing recomputed, nothing prepared, and the same bits.
		const auto w = cache.workForTest();
		checkCacheIsGains(cache, p, f0, s, count);
		const auto w2 = cache.workForTest();
		CHECK(w2.prepares == w.prepares);
		CHECK(w2.harmonic == w.harmonic);
		CHECK(w2.filter == w.filter);
		CHECK(w2.tilt == w.tilt);
	}
}

TEST_CASE("the kernels and GainCache on non-finite and out-of-range hz, k and params: the reference's clamps, and a finite batch") {
	const float inf = std::numeric_limits<float>::infinity();
	const float nan = std::numeric_limits<float>::quiet_NaN();
	// Partials 0..15 are the special values (hz and k each), the rest ordinary harmonics, so every special value sits in
	// a vector beside both another special and an ordinary lane.
	const float specialHz[] = {nan, inf, -inf, 0.0f, -5.0f, 1e-30f, 1e30f, 3.4e38f, nan, 440.0f, inf, 20.0f, -0.0f, 1e-45f, 1.0f, nan};
	const float specialK[] = {nan, inf, -inf, 0.0f, -3.0f, 1e-30f, 1e30f, 2147483648.0f, nan, 4.0f, inf, 0.5f, -0.0f, 2.5f, 1073741824.0f, -7.5f};
	constexpr int n = 64;
	float k[n], hz[n], base[n];
	for (int i = 0; i < n; ++i) {
		k[i] = i < 16 ? specialK[i] : static_cast<float>(i + 1);
		hz[i] = i < 16 ? specialHz[i] : static_cast<float>((i + 1) * 110.0);
		base[i] = 0.3f / static_cast<float>(i + 1);
	}
	const auto close = [](double got, double want) {
		if (std::isnan(want)) {
			return std::isnan(got); // the reference's own NaN (cos of an infinite phase): the batch's flush takes it
		}
		return std::abs(got - want) <= 1e-6 * std::abs(want) + 1e-12; // the references round to float
	};
	for (FilterMode mode : {FilterMode::LowPass, FilterMode::HighPass, FilterMode::BandPass, FilterMode::Notch}) {
		CAPTURE(int(mode));
		ShaperParams p {};
		p.filter = {true, mode, 1000.0f, 0.7f, 0.6f, 0.5f};
		p.tilt = {true, 12.0f, 500.0f};
		const PreparedShaper s = prepare(p, 110.0);
		double f[n];
		float t[n];
		filterFactors(s.filter, hz, n, f);
		tiltFactors(s.tilt, hz, n, t);
		for (int i = 0; i < 16; ++i) {
			CAPTURE(i);
			CAPTURE(hz[i]);
			CHECK(std::isfinite(f[i]));
			INFO("i " << i << " hz " << hz[i] << " got " << f[i] << " want " << filterGain(p.filter, hz[i], 110.0));
			CHECK(close(f[i], filterGain(p.filter, hz[i], 110.0)));
			CHECK(std::isfinite(t[i]));
			CHECK(close(t[i], tiltGain(p.tilt, hz[i])));
		}
	}
	{
		ShaperParams p {};
		p.comb = {true, 2.5f, 0.8f, 0.3f};
		p.oddEven = {true, 0.6f};
		p.curve.on = true;
		p.curve.gain[0] = 0.5f;
		p.curve.gain[63] = 0.25f;
		float h[n];
		harmonicFactors(prepare(p, 110.0).harmonic, k, n, h);
		for (int i = 0; i < 16; ++i) {
			CAPTURE(i);
			CAPTURE(k[i]);
			const double want = double(combGain(p.comb, k[i])) * double(oddEvenGain(p.oddEven, k[i])) * double(curveGain(p.curve, k[i]));
			CHECK(close(h[i], want));
		}
	}
	// THE BATCH AND THE CACHE: every operator on, Tilt compensating over the special partials, and a NaN Comb phase (a
	// param a host could deliver: every Comb factor is NaN, as the reference's is): every gain finite and >= 0, and the
	// cache equal to gains() to the bit.
	for (float phase : {0.3f, nan, inf}) {
		CAPTURE(phase);
		ShaperParams p = allOn(FilterMode::BandPass);
		p.formantShift.on = false;
		p.comb.phase = phase;
		p.filter.cutoffHz = nan; // the reference floors it at 1 Hz; so must the batch
		p.tilt.dbPerOctave = nan; // no tilt, as the reference reads it
		float out[n];
		gains(prepare(p, 110.0), k, hz, base, n, out); // the special k too
		for (int i = 0; i < n; ++i) {
			CAPTURE(i);
			CHECK(std::isfinite(out[i]));
			CHECK(out[i] >= 0.0f);
		}
		// The cache's k is i + 1 (the voice's contract), so it is held to gains() at those k.
		float index[n];
		for (int i = 0; i < n; ++i) {
			index[i] = static_cast<float>(i + 1);
		}
		gains(prepare(p, 110.0), index, hz, base, n, out);
		GainCache<n> cache;
		cache.update(p, 110.0, hz, n);
		float got[n];
		cache.apply(base, n, got);
		CHECK(std::memcmp(got, out, sizeof out) == 0);
		// and the NaN hz partials repeat bitwise, so a held note with them recomputes nothing
		const auto w = cache.workForTest();
		cache.update(p, 110.0, hz, n);
		CHECK(cache.workForTest().filter == w.filter);
	}
}

TEST_CASE("the harmonic kernel is elementwise: an ordinary k beside a huge one gives the same bits as beside an ordinary one") {
	// A |k| >= 2^30 takes the scalar road (libm); its NEIGHBOUR in the vector stays on the polynomial road. Every k
	// 1..320, in each lane, paired once with a huge or infinite partner and once with an ordinary one, over a full-depth
	// comb at 2000 phases (so harmonics land beside teeth, where the factor is small).
	// WHAT THIS CAN AND CANNOT SEE, MEASURED 2026-10-10: the pre-fix kernel, which sent BOTH lanes down the scalar road,
	// also passes it (known-bad run with --each: green, 33 of 33). The two roads agree to ~1e-15 and the factor is a
	// float, so the neighbour's road does not reach its bits anywhere this sweep looks. The case pins the property at the
	// resolution the voice uses; the fix is what makes it true below that.
	int differ = 0;
	int nearTooth = 0;
	for (int ph = 0; ph < 2000; ++ph) {
		ShaperParams p {};
		p.comb = {true, 2.5f, 1.0f, static_cast<float>(ph) / 2000.0f};
		p.oddEven = {true, 0.6f};
		const PreparedShaper::Harmonic h = prepare(p, 110.0).harmonic;
		const float huge = ph % 3 == 0 ? 1e30f : (ph % 3 == 1 ? 2147483648.0f : std::numeric_limits<float>::infinity());
		for (int i = 1; i <= kPartials; ++i) {
			const float kk = static_cast<float>(i);
			float withHuge[2], withOrdinary[2];
			const bool hugeRight = (i + ph) % 2 == 0; // the ordinary k in either lane
			const float pair[2] = {hugeRight ? kk : huge, hugeRight ? huge : kk};
			const float pairRef[2] = {hugeRight ? kk : 7.0f, hugeRight ? 7.0f : kk};
			harmonicFactors(h, pair, 2, withHuge);
			harmonicFactors(h, pairRef, 2, withOrdinary);
			const int lane = hugeRight ? 0 : 1;
			differ += std::memcmp(&withHuge[lane], &withOrdinary[lane], sizeof(float)) != 0 ? 1 : 0;
			nearTooth += withOrdinary[lane] < 1e-4f ? 1 : 0;
		}
	}
	CHECK(differ == 0);
	CHECK(nearTooth > 1000); // the sweep does reach the small factors
}

TEST_CASE("GainCache does the work a change needs and no more: a group recomputes only when what it reads moved, and an operator off costs nothing") {
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 2500.0f, 0.6f, 0.4f, 0.5f};
	p.comb = {true, 3.0f, 0.7f, 0.2f};
	p.tilt = {true, -3.0f, 1000.0f};
	p.oddEven = {true, -0.4f};
	Spectrum s = harmonics(130.0);
	GainCache<kPartials> cache;
	cache.update(p, 130.0, s.hz, 180);
	auto w = cache.workForTest();
	CHECK(w.prepares == 1);
	CHECK(w.harmonic == 1);
	CHECK(w.filter == 1);
	CHECK(w.tilt == 1);
	// A held note: a thousand blocks, nothing.
	for (int b = 0; b < 1000; ++b) {
		cache.update(p, 130.0, s.hz, 180);
	}
	auto w2 = cache.workForTest();
	CHECK(w2.prepares == 1);
	CHECK(w2.harmonic == 1);
	CHECK(w2.filter == 1);
	CHECK(w2.tilt == 1);
	// A moving cutoff: the filter's factors only.
	p.filter.cutoffHz = 3000.0f;
	cache.update(p, 130.0, s.hz, 180);
	w = cache.workForTest();
	CHECK(w.prepares == 2);
	CHECK(w.harmonic == 1);
	CHECK(w.filter == 2);
	CHECK(w.tilt == 1);
	// A moving comb: the harmonic group only.
	p.comb.depth = 0.5f;
	cache.update(p, 130.0, s.hz, 180);
	w = cache.workForTest();
	CHECK(w.harmonic == 2);
	CHECK(w.filter == 2);
	CHECK(w.tilt == 1);
	// A glide: f0 and the frequencies move, so Filter and Tilt recompute; the harmonic group reads k alone and stays.
	s = harmonics(131.0);
	cache.update(p, 131.0, s.hz, 180);
	w = cache.workForTest();
	CHECK(w.harmonic == 2);
	CHECK(w.filter == 3);
	CHECK(w.tilt == 2);
	// A count that shrinks keeps everything (the factors are per partial); one past what is held recomputes.
	cache.update(p, 131.0, s.hz, 150);
	CHECK(cache.workForTest().filter == 3);
	cache.update(p, 131.0, s.hz, 181);
	CHECK(cache.workForTest().filter == 4);
	// A switched-off operator's controls move nothing: its group keeps its defaults (the filter here).
	p.filter.on = false;
	cache.update(p, 131.0, s.hz, 181);
	const auto off = cache.workForTest();
	for (float c : {500.0f, 900.0f, 4000.0f}) {
		p.filter.cutoffHz = c;
		p.filter.resonance = c / 5000.0f;
		cache.update(p, 131.0, s.hz, 181);
	}
	const auto still = cache.workForTest();
	CHECK(still.filter == off.filter);
	CHECK(still.harmonic == off.harmonic);
	CHECK(still.tilt == off.tilt);
	// A neutral drawer (every operator off) and a disabled one: no group work at all, however the frequencies move.
	GainCache<kPartials> neutral;
	ShaperParams disabled = p;
	disabled.enabled = false;
	disabled.filter.on = true;
	for (int b = 0; b < 8; ++b) {
		s = harmonics(100.0 + b);
		neutral.update(b % 2 == 0 ? ShaperParams {} : disabled, 100.0 + b, s.hz, kPartials);
		CHECK(neutral.prepared().identity);
	}
	const auto nw = neutral.workForTest();
	CHECK(nw.harmonic == 0);
	CHECK(nw.filter == 0);
	CHECK(nw.tilt == 0);
}

TEST_CASE("the per-sound morph's filter group: a filter's mix fades its gain in dB, further modes multiply, and the batch and the cache are the reference") {
	// THE PER-SOUND DRAWER (Synth::drawerFor) fades a Filter by its MIX — its gain to that power — and runs one filter per
	// Mode the sounds disagree on, the gains multiplied. The reference states it directly (gain^mix · gain^mix ...); the
	// batch and GainCache must match it, and a mix of 1 must be the plain filter to the bit.
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 1500.0f, 0.4f, 0.5f, 0.0f};
	p.filter.mix = 0.6f;
	p.moreFilters[0] = {true, FilterMode::HighPass, 250.0f, 0.2f, 0.4f, 0.0f};
	p.moreFilters[0].mix = 0.4f;
	p.comb = {true, 3.0f, 0.5f, 0.1f}; // another group alongside, so the product is exercised
	for (double f0 : {55.0, 220.0, 880.0}) {
		CAPTURE(f0);
		const Spectrum s = harmonics(f0);
		float out[kPartials];
		gains(prepare(p, f0), s.k, s.hz, s.base, kPartials, out);
		GainCache<kPartials> cache;
		cache.update(p, f0, s.hz, kPartials);
		float cached[kPartials];
		cache.apply(s.base, kPartials, cached);
		for (int i = 0; i < kPartials; ++i) {
			CAPTURE(i);
			// THE RESTATEMENT, written from the definition rather than through gain(): each filter to its mix, then the comb.
			const double lp = std::pow(double(filterGain(p.filter, s.hz[i], f0)), 0.6);
			const double hp = std::pow(double(filterGain(p.moreFilters[0], s.hz[i], f0)), 0.4);
			const double want = lp * hp * double(combGain(p.comb, s.k[i]));
			CHECK(double(gain(p, s.k[i], s.hz[i], f0)) == doctest::Approx(want).epsilon(1e-6));
			if (want < 1e-3) {
				CHECK(std::abs(double(out[i]) - want) <= 1e-7);
			} else {
				CHECK(relErr(out[i], want) <= 1e-5);
			}
			CHECK(cached[i] == out[i]); // the cache is gains() to the bit
		}
	}
	// A MIX OF 1 IS THE PLAIN FILTER TO THE BIT, and a mix of 0 is no filter at all (gain exactly 1 from the group).
	const Spectrum s = harmonics(110.0);
	ShaperParams plain {};
	plain.filter = {true, FilterMode::BandPass, 900.0f, 0.3f, 0.6f, 0.0f};
	ShaperParams one = plain;
	one.filter.mix = 1.0f;
	float a[kPartials], b[kPartials];
	gains(prepare(plain, 110.0), s.k, s.hz, s.base, kPartials, a);
	gains(prepare(one, 110.0), s.k, s.hz, s.base, kPartials, b);
	CHECK(std::memcmp(a, b, sizeof(a)) == 0);
	ShaperParams none = plain;
	none.filter.mix = 0.0f;
	gains(prepare(none, 110.0), s.k, s.hz, s.base, kPartials, a);
	CHECK(allExactlyOne(a, kPartials));
}

TEST_CASE("GainCache: a moved or added further filter mode recomputes the filter group, and the gains are a fresh gains()") {
	// THE CACHE'S KEY FOR THE FILTER GROUP is the primary filter AND every further mode (PreparedShaper::sameFilterGroup).
	// Keyed on the primary alone, a morph moving only its second mode's cutoff — or bringing in a third mode — would keep
	// stale factors and play the old filter (quality review, 2026-10-10: that known-bad stayed green until this case).
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 2000.0f, 0.3f, 0.5f, 0.0f};
	p.filter.mix = 0.6f;
	p.moreFilters[0] = {true, FilterMode::HighPass, 200.0f, 0.2f, 0.4f, 0.0f};
	p.moreFilters[0].mix = 0.4f;
	constexpr double f0 = 110.0;
	const Spectrum s = harmonics(f0);
	GainCache<kPartials> cache;
	const auto check = [&](const ShaperParams& now, uint32_t filterWork, const char* when) {
		INFO(when);
		cache.update(now, f0, s.hz, kPartials);
		CHECK(cache.workForTest().filter == filterWork);
		float cached[kPartials];
		float fresh[kPartials];
		cache.apply(s.base, kPartials, cached);
		gains(prepare(now, f0), s.k, s.hz, s.base, kPartials, fresh);
		CHECK(std::memcmp(cached, fresh, sizeof(fresh)) == 0);
	};
	check(p, 1, "primed");
	check(p, 1, "held: nothing recomputed");
	ShaperParams moved = p;
	moved.moreFilters[0].cutoffHz = 600.0f; // ONLY the further mode moves
	check(moved, 2, "the further mode's cutoff moved");
	ShaperParams added = moved;
	added.moreFilters[1] = {true, FilterMode::BandPass, 900.0f, 0.1f, 0.3f, 0.0f};
	added.moreFilters[1].mix = 0.2f;
	check(added, 3, "a third mode came in");
	ShaperParams mixed = added;
	mixed.moreFilters[1].mix = 0.5f; // only a further mode's mix moves
	check(mixed, 4, "a further mode's mix moved");
}

// ---- The vowel (the Formant operator, design §3.2; spectral-ops 2026-10-12) ------------------------------------------

namespace {
/// THE VOWEL RESTATED from the design's words, independently of vowelGain: a floor of 1, each formant a resonance's power
/// response 1 / (1 + ((f² − c²) / (f·B))²) of bandwidth B = 50 + 0.1·c Hz standing 12 × its level above it (F1's level
/// 0.62, F2's 0.62 × 0.6), the whole to the depth.
double vowelRestated(double f1, double f2, double depth, double hz) {
	const auto peak = [hz](double c) {
		const double b = 50.0 + 0.1 * c;
		const double x = (hz * hz - c * c) / (hz * b);
		return 1.0 / (1.0 + x * x);
	};
	return std::pow(1.0 + 12.0 * (0.62 * peak(f1) + 0.62 * 0.6 * peak(f2)), depth);
}
} // namespace

TEST_CASE("the vowel's walk: an integer position is PhiVox's anchor exactly, between two it glides in log-Hz, and it clamps") {
	for (std::size_t i = 0; i < feathers::kVoxAnchorCount; ++i) {
		CAPTURE(i);
		const feathers::VowelFormants v = feathers::vowelFormants(static_cast<double>(i));
		CHECK(v.f1Hz == static_cast<double>(feathers::kVoxAnchorF1[i]));
		CHECK(v.f2Hz == static_cast<double>(feathers::kVoxAnchorF2[i]));
	}
	// PhiVox's own anchors, written out (slice_build.cpp's table before it moved): the move changed no value.
	CHECK(feathers::kVoxAnchorF1 == std::array<float, 8> {300.0f, 350.0f, 450.0f, 750.0f, 550.0f, 900.0f, 250.0f, 700.0f});
	CHECK(feathers::kVoxAnchorF2 == std::array<float, 8> {2200.0f, 800.0f, 900.0f, 1150.0f, 1900.0f, 1600.0f, 600.0f, 3400.0f});
	// Halfway from Round (450, 900) to Open (750, 1150): the geometric means, not the arithmetic ones.
	const feathers::VowelFormants mid = feathers::vowelFormants(2.5);
	CHECK(mid.f1Hz == doctest::Approx(std::sqrt(450.0 * 750.0)).epsilon(1e-12));
	CHECK(mid.f2Hz == doctest::Approx(std::sqrt(900.0 * 1150.0)).epsilon(1e-12));
	CHECK(std::abs(mid.f1Hz - 600.0) > 5.0);
	// A quarter of the way from Growl (250) to Rasp (700): 250 · (700/250)^0.25.
	CHECK(feathers::vowelFormants(6.25).f1Hz == doctest::Approx(250.0 * std::pow(700.0 / 250.0, 0.25)).epsilon(1e-12));
	CHECK(feathers::vowelFormants(-3.0).f1Hz == 300.0);
	CHECK(feathers::vowelFormants(99.0).f2Hz == 3400.0);
	CHECK(feathers::vowelFormants(std::numeric_limits<double>::quiet_NaN()).f1Hz == 300.0);
}

TEST_CASE("the vowel's reference is the restated peaks to the depth: F1 and F2 stand out, and depth 0 is exactly 1") {
	for (const auto& [f1, f2] : {std::pair {300.0f, 2200.0f}, std::pair {750.0f, 1150.0f}, std::pair {250.0f, 600.0f}}) {
		for (float depth : {0.25f, 0.7f, 1.0f}) {
			const VowelOp v {true, f1, f2, depth};
			for (double hz : {40.0, 150.0, double(f1), 0.5 * (f1 + f2), double(f2), 5000.0, 19000.0}) {
				CAPTURE(f1);
				CAPTURE(depth);
				CAPTURE(hz);
				CHECK(relErr(vowelGain(v, hz), vowelRestated(f1, f2, depth, hz)) <= 1e-6);
			}
			// The peaks are peaks: F1's centre is louder than an octave either side of it.
			CHECK(vowelGain(v, f1) > vowelGain(v, 2.0 * f1 > f2 ? 0.5 * f1 : 2.0 * f1));
		}
	}
	// F1's peak at full depth is the floor plus 12 × 0.62 (+ F2's skirt there), so +18.5 dB and change.
	CHECK(20.0 * std::log10(vowelGain(VowelOp {true, 300.0f, 2200.0f, 1.0f}, 300.0)) == doctest::Approx(18.53).epsilon(1e-2));
	// DEPTH 0 IS NO VOWEL, exactly, on the reference AND the batch (prepare turns the group off: no pow trusted to land on 1).
	ShaperParams p {};
	p.vowel = {true, 700.0f, 1150.0f, 0.0f};
	CHECK(vowelGain(p.vowel, 700.0) == 1.0f);
	CHECK(gain(p, 1.0, 700.0, 110.0) == 1.0f);
	const PreparedShaper s = prepare(p, 110.0);
	CHECK(s.identity);
	CHECK_FALSE(s.vowel.on);
	const Spectrum sp = harmonics(110.0);
	float out[kPartials];
	gains(s, sp.k, sp.hz, sp.base, kPartials, out);
	CHECK(allExactlyOne(out, kPartials));
	// And off, at any depth.
	p.vowel = {false, 700.0f, 1150.0f, 1.0f};
	CHECK(prepare(p, 110.0).identity);
	CHECK(gain(p, 1.0, 700.0, 110.0) == 1.0f);
}

TEST_CASE("the vowel kernel is the reference in dB over the sweep, every vowel and depth") {
	const Sweep sw = sweepOf({27.5, 110.0, 440.0, 3520.0});
	const int n = static_cast<int>(sw.k.size());
	std::vector<float> got(sw.k.size());
	double worst = 0.0;
	for (std::size_t a = 0; a < feathers::kVoxAnchorCount; ++a) {
		for (float depth : {0.05f, 0.5f, 1.0f}) {
			ShaperParams p {};
			p.vowel = {true, feathers::kVoxAnchorF1[a], feathers::kVoxAnchorF2[a], depth};
			vowelFactors(prepare(p, 110.0).vowel, sw.hz.data(), n, got.data());
			for (int i = 0; i < n; ++i) {
				worst = std::max(worst, dbError(got[size_t(i)], vowelGain(p.vowel, sw.hz[size_t(i)])));
			}
		}
	}
	MESSAGE("vowel " << worst << " dB");
	CHECK(worst <= kFloatFactorMaxDb);
}

TEST_CASE("the vowel keeps the power, its shape is the reference's, and with Tilt the two are compensated together") {
	const Spectrum s = harmonics(110.0, 64);
	float ones[64];
	std::fill(ones, ones + 64, 1.0f);
	const double before = power(ones, s.base, 64);
	ShaperParams p {};
	p.vowel = {true, 750.0f, 1150.0f, 0.8f};
	float out[64];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	CHECK(relErr(power(out, s.base, 64), before) <= 1e-5);
	for (const auto& [i, j] : {std::pair {6, 0}, std::pair {9, 30}, std::pair {40, 5}}) {
		CAPTURE(i);
		CAPTURE(j);
		const double want = vowelRestated(750.0, 1150.0, 0.8, s.hz[i]) / vowelRestated(750.0, 1150.0, 0.8, s.hz[j]);
		CHECK(relErr(static_cast<double>(out[i]) / static_cast<double>(out[j]), want) <= 1e-5);
	}
	CHECK(out[6] / out[0] > 3.0f); // not vacuous: harmonic 7 (770 Hz) sits on F1, harmonic 1 does not
	// WITH TILT: one compensation over the product, so the power is still held and the shape is Tilt's times the vowel's.
	p.tilt = {true, -6.0f, 1000.0f};
	gains(prepare(p, 110.0), s.k, s.hz, s.base, 64, out);
	CHECK(relErr(power(out, s.base, 64), before) <= 1e-5);
	const double want = static_cast<double>(tiltGain(p.tilt, s.hz[6])) * vowelRestated(750.0, 1150.0, 0.8, s.hz[6])
	                    / (static_cast<double>(tiltGain(p.tilt, s.hz[40])) * vowelRestated(750.0, 1150.0, 0.8, s.hz[40]));
	CHECK(relErr(static_cast<double>(out[6]) / static_cast<double>(out[40]), want) <= 1e-5);
	// AND WITH A FILTER, the power held is base × filter (the vowel, like Tilt, compensates against the other operators).
	ShaperParams q {};
	q.filter = {true, FilterMode::LowPass, 1000.0f, 0.0f, 0.25f, 0.0f};
	q.vowel = {true, 300.0f, 2200.0f, 1.0f};
	float filtered[64];
	for (int i = 0; i < 64; ++i) {
		filtered[i] = filterGain(q.filter, s.hz[i], 110.0);
	}
	gains(prepare(q, 110.0), s.k, s.hz, s.base, 64, out);
	CHECK(relErr(power(out, s.base, 64), power(filtered, s.base, 64)) <= 1e-5);
}

TEST_CASE("GainCache with the vowel: gains() to the bit through each vowel control, and the vowel group recomputes only when what it reads moves") {
	GainCache<kPartials> cache;
	ShaperParams p = allOn(FilterMode::BandPass);
	p.formantShift.on = false;
	double f0 = 110.0;
	Spectrum s = harmonics(f0);
	checkCacheIsGains(cache, p, f0, s, kPartials);
	CHECK(cache.workForTest().vowel == 0); // off costs nothing
	const auto step = [&](const char* what, auto&& change, bool vowelWork) {
		INFO(what);
		const uint32_t before = cache.workForTest().vowel;
		float a[kPartials];
		gains(prepare(p, f0), s.k, s.hz, s.base, kPartials, a);
		change();
		float b[kPartials];
		gains(prepare(p, f0), s.k, s.hz, s.base, kPartials, b);
		CHECK(std::memcmp(a, b, sizeof(a)) != 0); // not vacuous: the change moves the gains
		checkCacheIsGains(cache, p, f0, s, kPartials);
		CHECK(cache.workForTest().vowel == before + (vowelWork ? 1u : 0u));
		checkCacheIsGains(cache, p, f0, s, kPartials); // held: nothing more
		CHECK(cache.workForTest().vowel == before + (vowelWork ? 1u : 0u));
	};
	step("the vowel switched on", [&] { p.vowel = {true, 450.0f, 900.0f, 0.6f}; }, true);
	step("F1", [&] { p.vowel.f1Hz = 550.0f; }, true);
	step("F2", [&] { p.vowel.f2Hz = 1900.0f; }, true);
	step("depth", [&] { p.vowel.depth = 0.9f; }, true);
	step("the comb (another group: the vowel's factors are kept)", [&] { p.comb.depth = 0.2f; }, false);
	step("the frequencies (a glide)", [&] { s = harmonics(f0 = 116.0); }, true);
	step("tilt off: the vowel alone is compensated", [&] { p.tilt.on = false; }, false);
	step("the vowel off", [&] { p.vowel.on = false; }, false);
	// THE VOWEL AS THE ONLY OPERATOR THAT READS hz: no filter or Tilt to copy the frequencies in for it, so a glide must
	// reach the vowel's factors on the vowel's own account.
	p = ShaperParams {};
	p.vowel = {true, 300.0f, 2200.0f, 1.0f};
	s = harmonics(f0 = 110.0);
	GainCache<kPartials> alone;
	checkCacheIsGains(alone, p, f0, s, kPartials);
	CHECK(alone.workForTest().vowel == 1);
	s = harmonics(f0 = 131.0);
	checkCacheIsGains(alone, p, f0, s, kPartials);
	CHECK(alone.workForTest().vowel == 2);
}

TEST_CASE("the vowel on non-finite and extreme hz and params: finite factors, the reference's floor and clamps") {
	const float inf = std::numeric_limits<float>::infinity();
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float hz[] = {inf, nan, -5.0f, 0.0f, 1e-30f, 3e38f, 700.0f, 24000.0f};
	constexpr int n = 8;
	for (const VowelOp& v : {VowelOp {true, 700.0f, 1150.0f, 1.0f}, VowelOp {true, -40.0f, nan, 2.0f},
	                         VowelOp {true, inf, 0.0f, 0.5f}}) {
		ShaperParams p {};
		p.vowel = v;
		const PreparedShaper s = prepare(p, 110.0);
		float got[n];
		vowelFactors(s.vowel, hz, n, got);
		for (int i = 0; i < n; ++i) {
			CAPTURE(i);
			CHECK(std::isfinite(got[i]));
			CHECK(got[i] >= 1.0f);
			const float want = vowelGain(v, hz[i]);
			CHECK(std::isfinite(want));
			CHECK(std::abs(got[i] - want) <= 1e-5f * want);
		}
	}
}

TEST_CASE("exp2Scaled is 2^(scale·q) to the kernels' precision, held at ±1020 in the exponent, for any count") {
	std::vector<float> q;
	for (int i = 0; i < 37; ++i) {
		q.push_back(0.01f * static_cast<float>(i * i));
	}
	for (const double scale : { -0.3, -12.0, 0.0, 5.5 }) {
		CAPTURE(scale);
		std::vector<float> out(q.size(), -1.0f);
		exp2Scaled(q.data(), scale, static_cast<int>(q.size()), out.data());
		for (size_t i = 0; i < q.size(); ++i) {
			CAPTURE(i);
			const double want = std::exp2(std::clamp(scale * double(q[i]), -1020.0, 1020.0));
			CHECK(std::abs(double(out[i]) - static_cast<float>(want)) <= 1e-6 * std::max(want, 1e-30));
		}
	}
	float big[2] = { 1.0e6f, 2.0f };
	float out[2] = { -1.0f, -1.0f };
	exp2Scaled(big, -1.0, 2, out);
	CHECK(out[0] == 0.0f); // 2^−1020 is below a float: an exact zero, never a NaN
	CHECK(out[1] == 0.25f);
}

TEST_CASE("Z7d: the filter's open end — Low Pass at 20 kHz and High Pass at 20 Hz are off, by the control or by the tracked corner; gain() and prepare() agree") {
	const double top = std::log2(kFilterOpenHighHz);
	const double bottom = std::log2(kFilterOpenLowHz);
	// THE RAMP, by value: 0 until the last third of an octave, linear in octaves to 1 at the end; either road; Notch at both ends;
	// Band Pass never.
	CHECK(filterOpenness(FilterMode::LowPass, top - kFilterOpenOctaves - 0.01, top - 2.0) == 0.0);
	CHECK(filterOpenness(FilterMode::LowPass, top - 0.5 * kFilterOpenOctaves, top - 2.0) == doctest::Approx(0.5));
	CHECK(filterOpenness(FilterMode::LowPass, top - 2.0, top) == 1.0);            // the corner's road
	CHECK(filterOpenness(FilterMode::LowPass, top - 2e-6, top - 2.0) == 1.0);     // the stored float's few 1e-7: the end
	CHECK(filterOpenness(FilterMode::HighPass, bottom + 0.25 * kFilterOpenOctaves, bottom + 3.0) == doctest::Approx(0.75));
	CHECK(filterOpenness(FilterMode::HighPass, bottom, bottom + 3.0) == 1.0);
	CHECK(filterOpenness(FilterMode::BandPass, top, top) == 0.0);
	CHECK(filterOpenness(FilterMode::Notch, bottom, bottom) == 1.0); // Z9b: a notch opens at both ends
	CHECK(filterOpenness(FilterMode::Notch, top, top) == 1.0);
	CHECK(filterOpenness(FilterMode::Notch, top - 0.5 * kFilterOpenOctaves, 10.0) == doctest::Approx(0.5));
	CHECK(filterOpenness(FilterMode::Notch, 10.0, 10.0) == 0.0);
	// AT THE END: gain() is exactly 1 at every frequency, at any slope and resonance, and prepare() runs no filter (identity).
	for (const FilterOp op : { FilterOp { true, FilterMode::LowPass, 20000.0f, 1.0f, 0.0f, 0.0f },
	                           FilterOp { true, FilterMode::HighPass, 20.0f, 1.0f, 1.0f, 0.0f },
	                           FilterOp { true, FilterMode::Notch, 20000.0f, 0.0f, 0.0f, 0.0f },
	                           FilterOp { true, FilterMode::Notch, 20.0f, 0.0f, 1.0f, 0.0f },
	                           FilterOp { true, FilterMode::LowPass, 10000.0f, 0.5f, 0.3f, 1.0f } }) { // 10 kHz tracked an octave up
		const double f0 = op.keyTrack > 0.0f ? 2.0 * kKeyTrackPivotHz : kKeyTrackPivotHz;
		ShaperParams p {};
		p.filter = op;
		for (const double hz : { 20.0, 100.0, 1000.0, 10000.0, 15000.0, 20000.0 }) {
			CHECK(gain(p, hz / f0, hz, f0) == 1.0f);
		}
		CHECK(prepare(p, f0).identity);
	}
	// HALF WAY: gain() is the filter's own gain to the power one half — prepare()'s mix, the same rule on both roads.
	ShaperParams half {};
	half.filter = FilterOp { true, FilterMode::LowPass, static_cast<float>(std::exp2(top - 0.5 * kFilterOpenOctaves)), 0.0f, 0.0f, 0.0f };
	const double f0 = kKeyTrackPivotHz;
	CHECK(prepare(half, f0).filter.mix == doctest::Approx(0.5).epsilon(1e-6));
	CHECK(static_cast<double>(gain(half, 18000.0 / f0, 18000.0, f0))
	      == doctest::Approx(std::sqrt(static_cast<double>(filterGain(half.filter, 18000.0, f0)))).epsilon(1e-6));
}

// ---- F2 (additive design §3.6): the Bell, the second filter, Series / Parallel ------------------------------------------

namespace {
/// THE BELL RESTATED from the design's words, independently of filterGain: A^b with A = 10^(dB/20), b = 1/(1 + |d/h|^(2n)), d the
/// octaves from the (untracked) centre, h half of a bandwidth running geometrically from 3 octaves (Resonance 0) to 1/6 (1), n the
/// skirt order 2^(4·(Slope − 0.25)/0.75): 1 (the classic bell) at the default Slope 0.25, 16 at Slope 1.
double bellRestated(double centre, double db, double res, double slope, double hz) {
	const double h = 0.5 * 3.0 * std::pow((1.0 / 6.0) / 3.0, res);
	const double n = std::exp2(4.0 * (slope - 0.25) / 0.75);
	const double d = std::log2(hz / centre);
	return std::pow(10.0, db / 20.0 / (1.0 + std::pow(std::abs(d) / h, 2.0 * n)));
}
FilterOp bell(float centre, float db, float res, float slope) {
	FilterOp f {true, FilterMode::Bell, centre, res, slope, 0.0f};
	f.gainDb = db;
	return f;
}
} // namespace

TEST_CASE("F2 Bell: its Gain at the centre, half its dB at half the bandwidth, Resonance the width and Slope the skirt") {
	for (float db : {-24.0f, -6.0f, 9.0f, 24.0f}) {
		for (float res : {0.0f, 0.5f, 1.0f}) {
			for (float slope : {0.0f, 0.25f, 1.0f}) {
				CAPTURE(db);
				CAPTURE(res);
				CAPTURE(slope);
				const FilterOp f = bell(1000.0f, db, res, slope);
				CHECK(double(filterGain(f, 1000.0, kKeyTrackPivotHz)) == doctest::Approx(std::pow(10.0, db / 20.0)).epsilon(1e-6));
				const double h = bellHalfWidthOctaves(res);
				for (double hz : {1000.0 * std::exp2(h), 1000.0 * std::exp2(-h)}) {
					CHECK(20.0 * std::log10(double(filterGain(f, hz, kKeyTrackPivotHz))) == doctest::Approx(db / 2.0).epsilon(1e-5));
				}
				for (double hz : {40.0, 300.0, 700.0, 1100.0, 2500.0, 12000.0}) {
					CHECK(double(filterGain(f, hz, kKeyTrackPivotHz))
					      == doctest::Approx(bellRestated(1000.0, db, res, slope, hz)).epsilon(1e-6));
				}
			}
		}
	}
	// THE WIDTH'S ENDS AS THE DESIGN NAMES THEM: 3 octaves at Resonance 0, a sixth of one at 1.
	CHECK(2.0 * bellHalfWidthOctaves(0.0) == doctest::Approx(3.0));
	CHECK(2.0 * bellHalfWidthOctaves(1.0) == doctest::Approx(1.0 / 6.0));
	// SLOPE IS THE SKIRT, AND THE DEFAULT IS THE CLASSIC BELL (decided 2026-10-19): two half-widths out, the bell at the
	// filters' default Slope (0.25, order exactly 1) keeps a fifth of its dB; Slope 0 softer (more), the steepest almost none.
	CHECK(bellSkirtOrder(0.25) == 1.0);
	CHECK(bellSkirtOrder(1.0) == doctest::Approx(16.0));
	CHECK(bellSkirtOrder(0.0) < 0.5);
	const FilterOp classic = bell(1000.0f, 12.0f, 0.5f, 0.25f);
	const FilterOp soft = bell(1000.0f, 12.0f, 0.5f, 0.0f);
	const FilterOp box = bell(1000.0f, 12.0f, 0.5f, 1.0f);
	const double twoH = 1000.0 * std::exp2(2.0 * bellHalfWidthOctaves(0.5));
	CHECK(20.0 * std::log10(double(filterGain(classic, twoH, kKeyTrackPivotHz))) == doctest::Approx(12.0 / 5.0).epsilon(1e-5));
	CHECK(20.0 * std::log10(double(filterGain(soft, twoH, kKeyTrackPivotHz))) > 12.0 / 5.0 + 1.0);
	CHECK(20.0 * std::log10(double(filterGain(box, twoH, kKeyTrackPivotHz))) < 1e-8);
	// AND ITS FLAT TOP: half a half-width in, the steepest skirt is still within 1e-8 dB of the full Gain.
	CHECK(20.0 * std::log10(double(filterGain(box, 1000.0 * std::exp2(0.5 * bellHalfWidthOctaves(0.5)), kKeyTrackPivotHz)))
	      == doctest::Approx(12.0).epsilon(1e-8));
	// RESONANCE ADDS NO BUMP TO A BELL: its Resonance is the width.
	CHECK(prepare(ShaperParams { .filter = bell(1000.0f, 6.0f, 1.0f, 0.2f) }, 110.0).filter.resonanceAmp == 0.0);
}

TEST_CASE("F2 Bell at 0 dB is no filter: exactly 1, open, and nothing prepared, whatever its centre, width and skirt") {
	ShaperParams p {};
	p.filter = bell(3000.0f, 0.0f, 0.7f, 0.9f);
	CHECK(filterOpenness(p.filter, 110.0) == 1.0);
	CHECK(prepare(p, 110.0).identity);
	const Spectrum s = harmonics(110.0);
	float out[kPartials];
	gains(prepare(p, 110.0), s.k, s.hz, s.base, kPartials, out);
	CHECK(allExactlyOne(out, kPartials));
	for (int i = 0; i < kPartials; ++i) {
		CHECK(gain(p, s.k[i], s.hz[i], 110.0) == 1.0f);
	}
	// A HAIR OFF 0 dB IT ACTS (it has no frequency open end: a bell at 20 kHz still lifts the top).
	p.filter.gainDb = 0.01f;
	CHECK(filterOpenness(p.filter, 110.0) == 0.0);
	CHECK_FALSE(prepare(p, 110.0).identity);
	p.filter.cutoffHz = 20000.0f;
	CHECK(filterOpenness(p.filter, 110.0) == 0.0);
}

TEST_CASE("F2 the Bell kernel is the reference in dB over the sweep: gain, width, skirt, centre and key track") {
	const Sweep sw = sweepOf({27.5, 110.0, 440.0, 3520.0});
	double worst = 0.0;
	int checked = 0;
	for (float db : {-24.0f, -3.0f, 12.0f, 24.0f}) {
		for (float centre : {50.0f, 800.0f, 5000.0f, 18000.0f}) {
			for (float res : {0.0f, 0.6f, 1.0f}) {
				for (float slope : {0.0f, 0.4f, 1.0f}) {
					for (float track : {0.0f, 1.0f}) {
						FilterOp op = bell(centre, db, res, slope);
						op.keyTrack = track;
						ShaperParams p {};
						p.filter = op;
						for (std::size_t i = 0; i < sw.k.size();) {
							const double f0 = sw.f0[i];
							std::size_t n = 0;
							while (i + n < sw.k.size() && sw.f0[i + n] == f0) {
								++n;
							}
							std::vector<double> got(n);
							filterFactors(prepare(p, f0).filter, sw.hz.data() + i, static_cast<int>(n), got.data());
							for (std::size_t j = 0; j < n; ++j) {
								worst = std::max(worst, dbError(got[j], filterGain(op, sw.hz[i + j], f0)));
								++checked;
							}
							i += n;
						}
					}
				}
			}
		}
	}
	MESSAGE("Bell: worst " << worst << " dB over " << checked << " factors");
	CHECK(worst <= kFilterMaxDb);
	CHECK(checked > 10000);
}

TEST_CASE("F2 parallelGain: the measured cases — two band-passes two bands at unity, an LP/HP crossover flat, a boost kept") {
	const double f0 = kKeyTrackPivotHz;
	const auto op = [](FilterMode m, float hz) { return FilterOp {true, m, hz, 0.0f, 0.25f, 0.0f}; };
	const auto par = [&](const FilterOp& a, const FilterOp& b, double hz) {
		return parallelGain(double(filterGain(a, hz, f0)), double(filterGain(b, hz, f0)));
	};
	// BP 300 + BP 3k: each centre at unity (the other path is far below it there).
	CHECK(par(op(FilterMode::BandPass, 300.0f), op(FilterMode::BandPass, 3000.0f), 300.0) == doctest::Approx(1.0).epsilon(1e-6));
	CHECK(par(op(FilterMode::BandPass, 300.0f), op(FilterMode::BandPass, 3000.0f), 3000.0) == doctest::Approx(1.0).epsilon(1e-6));
	CHECK(20.0 * std::log10(par(op(FilterMode::BandPass, 300.0f), op(FilterMode::BandPass, 3000.0f), 1000.0)) < -30.0);
	// LP 1k + HP 1k: flat (within a float's rounding of each factor) across the spectrum, the crossover included.
	for (double hz : {50.0, 300.0, 700.0, 1000.0, 1400.0, 5000.0, 15000.0}) {
		CAPTURE(hz);
		CHECK(par(op(FilterMode::LowPass, 1000.0f), op(FilterMode::HighPass, 1000.0f), hz) == doctest::Approx(1.0).epsilon(1e-6));
	}
	// LP 500 + HP 2k: a band reject, unity in both passbands and the dip between (−22.6 dB at 1 kHz, measured in the design).
	CHECK(20.0 * std::log10(par(op(FilterMode::LowPass, 500.0f), op(FilterMode::HighPass, 2000.0f), 1000.0))
	      == doctest::Approx(-22.6).epsilon(0.005));
	// THE RULE'S TWO HOLDS: never above unity unless a path boosts, and then never above that path.
	CHECK(parallelGain(0.8, 0.7) == 1.0);
	CHECK(parallelGain(4.0, 1.0) == 4.0);
	CHECK(parallelGain(0.25, 0.5) == 0.75);
	CHECK(parallelGain(0.0, 0.0) == 0.0);
	// A BELL'S BOOST IN PARALLEL WITH A PATH THAT PASSES THERE IS THE BELL'S BOOST, EXACTLY.
	CHECK(par(bell(1000.0f, 12.0f, 0.5f, 0.0f), op(FilterMode::LowPass, 20000.0f), 1000.0) == double(filterGain(bell(1000.0f, 12.0f, 0.5f, 0.0f), 1000.0, f0)));
}

TEST_CASE("F2 a second filter switched off is the old road to the bit: gain(), gains() and the cache, Series or Parallel") {
	ShaperParams old = allOn(FilterMode::LowPass);
	old.moreFilters[0] = {true, FilterMode::HighPass, 300.0f, 0.2f, 0.5f, 0.0f};
	old.moreFilters[0].mix = 0.4f;
	old.filter.mix = 0.6f;
	ShaperParams withOff = old;
	withOff.filter2 = {false, FilterMode::BandPass, 700.0f, 0.5f, 0.6f, 0.3f};
	withOff.moreFilters2[0] = {false, FilterMode::Bell, 900.0f, 0.5f, 0.6f, 0.3f};
	withOff.moreFilters2[0].gainDb = 9.0f;
	withOff.parallel = 1.0f;
	withOff.filterPresence = {0.3f, 0.7f};
	for (double f0 : {55.0, 440.0}) {
		const Spectrum s = harmonics(f0);
		float a[kPartials], b[kPartials];
		gains(prepare(old, f0), s.k, s.hz, s.base, kPartials, a);
		gains(prepare(withOff, f0), s.k, s.hz, s.base, kPartials, b);
		CHECK(std::memcmp(a, b, sizeof(a)) == 0);
		GainCache<kPartials> cache;
		cache.update(withOff, f0, s.hz, kPartials);
		cache.apply(s.base, kPartials, b);
		CHECK(std::memcmp(a, b, sizeof(a)) == 0);
		for (int i = 0; i < kPartials; ++i) {
			CHECK(gain(old, s.k[i], s.hz[i], f0) == gain(withOff, s.k[i], s.hz[i], f0));
		}
	}
	// AND THE DEFAULTS ARE THAT: filter 2 off, Series, both presences whole.
	const ShaperParams d {};
	CHECK_FALSE(d.filter2.on);
	CHECK(d.parallel == 0.0f);
	CHECK(d.filterPresence[0] == 1.0f);
	CHECK(d.filterPresence[1] == 1.0f);
	CHECK(d.filter2.gainDb == 0.0f);
}

TEST_CASE("F2 filter 2 alone is filter 1's road: the same op in either slot gives the same bits") {
	const FilterOp op {true, FilterMode::Notch, 1200.0f, 0.0f, 0.4f, 0.5f};
	ShaperParams one {};
	one.filter = op;
	one.tilt = {true, 3.0f, 1000.0f}; // the fused Filter+Tilt road too
	ShaperParams two {};
	two.filter2 = op;
	two.tilt = one.tilt;
	two.parallel = 1.0f; // routing reads nothing with one filter
	for (double f0 : {110.0, 880.0}) {
		const Spectrum s = harmonics(f0);
		float a[kPartials], b[kPartials];
		gains(prepare(one, f0), s.k, s.hz, s.base, kPartials, a);
		gains(prepare(two, f0), s.k, s.hz, s.base, kPartials, b);
		CHECK(std::memcmp(a, b, sizeof(a)) == 0);
		CHECK(prepare(two, f0).side2Count == 0);
		CHECK(prepare(two, f0).singleFilter());
	}
}

TEST_CASE("F2 Series multiplies the two filters, Parallel is their held sum, and a blend is the two in dB; batch and cache are the reference") {
	ShaperParams p {};
	p.filter = {true, FilterMode::BandPass, 300.0f, 0.3f, 0.25f, 0.0f};
	p.filter2 = bell(2500.0f, 9.0f, 0.4f, 0.1f);
	p.comb = {true, 3.0f, 0.4f, 0.1f};
	for (float r : {0.0f, 1.0f, 0.35f}) {
		for (std::array<float, 2> pres : {std::array<float, 2> {1.0f, 1.0f}, std::array<float, 2> {0.6f, 0.25f}}) {
			CAPTURE(r);
			CAPTURE(pres[0]);
			CAPTURE(pres[1]);
			p.parallel = r;
			p.filterPresence = pres;
			p.filter.mix = pres[0]; // as the morph gives it: each side's mix is its presence (no open end here)
			p.filter2.mix = pres[1];
			for (double f0 : {55.0, 440.0}) {
				const Spectrum s = harmonics(f0);
				float out[kPartials], cached[kPartials];
				gains(prepare(p, f0), s.k, s.hz, s.base, kPartials, out);
				GainCache<kPartials> cache;
				cache.update(p, f0, s.hz, kPartials);
				cache.apply(s.base, kPartials, cached);
				for (int i = 0; i < kPartials; ++i) {
					CAPTURE(i);
					// THE RESTATEMENT from the design's words: each side at full presence g1, g2; Series their product faded by
					// presence; Parallel s1^(1−p2)·s2^(1−p1)·P(g1, g2)^(p1·p2); the blend Series^(1−r)·Parallel^r.
					const double g1 = double(filterGain(p.filter, s.hz[i], f0));
					const double g2 = double(filterGain(p.filter2, s.hz[i], f0));
					const double p1 = pres[0], p2 = pres[1];
					const double s1 = std::pow(g1, p1), s2 = std::pow(g2, p2);
					const double held = std::min(g1 + g2, std::max({1.0, g1, g2}));
					const double parallel = std::pow(s1, 1.0 - p2) * std::pow(s2, 1.0 - p1) * std::pow(held, p1 * p2);
					const double routed = std::pow(s1 * s2, 1.0 - r) * std::pow(parallel, r);
					const double want = routed * double(combGain(p.comb, s.k[i]));
					CHECK(double(gain(p, s.k[i], s.hz[i], f0)) == doctest::Approx(want).epsilon(1e-5));
					if (want < 1e-3) {
						CHECK(std::abs(double(out[i]) - want) <= 1e-7);
					} else {
						CHECK(relErr(out[i], want) <= 2e-5);
					}
					CHECK(cached[i] == out[i]);
				}
			}
		}
	}
}

TEST_CASE("F2 in Parallel a side leaves by its presence, and an open end makes a filter a wire beside the other") {
	constexpr double f0 = 110.0;
	ShaperParams p {};
	p.filter = {true, FilterMode::BandPass, 500.0f, 0.0f, 0.25f, 0.0f};
	p.filter2 = {true, FilterMode::BandPass, 4000.0f, 0.0f, 0.25f, 0.0f};
	p.parallel = 1.0f;
	// PRESENCE TO NOTHING: filter 2's share shrinking takes the Parallel result to filter 1 alone, continuously.
	const double alone = double(filterGain(p.filter, 4000.0, f0));
	double last = 0.0;
	for (float pr : {1.0f, 0.5f, 0.1f, 0.001f}) {
		p.filterPresence = {1.0f, pr};
		p.filter2.mix = pr;
		const double g = double(gain(p, 4000.0 / f0, 4000.0, f0));
		if (pr < 1.0f) {
			CHECK(g < last); // filter 2's band leaving
		}
		last = g;
	}
	CHECK(last == doctest::Approx(alone).epsilon(0.02));
	// AN OPEN END IS A WIRE: a Low Pass at its top in Parallel with a band-pass is flat, not the band-pass alone.
	ShaperParams wire {};
	wire.filter = {true, FilterMode::BandPass, 500.0f, 0.0f, 0.25f, 0.0f};
	wire.filter2 = {true, FilterMode::LowPass, 19500.0f, 0.0f, 0.25f, 0.0f}; // inside its last third of an octave
	wire.parallel = 1.0f;
	CHECK(filterOpenness(wire.filter2, f0) > 0.0);
	CHECK(filterOpenness(wire.filter2, f0) < 1.0);
	for (double hz : {100.0, 500.0, 3000.0, 15000.0}) {
		CAPTURE(hz);
		CHECK(double(gain(wire, hz / f0, hz, f0)) == doctest::Approx(1.0).epsilon(0.01)); // within 0.09 dB: the LP's own skirt at 15 kHz, faded
	}
}

TEST_CASE("F2 GainCache: filter 2's controls, its switch and the routing each recompute the filter group, and the gains are a fresh gains()") {
	ShaperParams p {};
	p.filter = {true, FilterMode::LowPass, 2000.0f, 0.3f, 0.5f, 0.0f};
	constexpr double f0 = 110.0;
	const Spectrum s = harmonics(f0);
	GainCache<kPartials> cache;
	const auto check = [&](const ShaperParams& now, uint32_t filterWork, const char* when) {
		INFO(when);
		cache.update(now, f0, s.hz, kPartials);
		CHECK(cache.workForTest().filter == filterWork);
		float cached[kPartials];
		float fresh[kPartials];
		cache.apply(s.base, kPartials, cached);
		gains(prepare(now, f0), s.k, s.hz, s.base, kPartials, fresh);
		CHECK(std::memcmp(cached, fresh, sizeof(fresh)) == 0);
	};
	check(p, 1, "primed");
	ShaperParams q = p;
	q.filter2 = {false, FilterMode::HighPass, 300.0f, 0.2f, 0.4f, 0.0f};
	check(q, 1, "filter 2 set up while off: nothing recomputed");
	q.filter2.on = true;
	check(q, 2, "filter 2 switched on");
	q.filter2.cutoffHz = 400.0f;
	check(q, 3, "filter 2's cutoff moved");
	q.parallel = 1.0f;
	check(q, 4, "routed in Parallel");
	q.filterPresence[1] = 0.5f;
	check(q, 5, "a presence moved");
	q.filter2.mode = FilterMode::Bell;
	q.filter2.gainDb = -6.0f;
	check(q, 6, "filter 2 a Bell");
	q.filter2.gainDb = -3.0f;
	check(q, 7, "the Bell's gain moved");
}

TEST_CASE("MEASURE F2: the drawer's filter group per control block through GainCache — one filter, two in Series, two in Parallel, a Bell" * doctest::skip()) {
	// THE DRAWER'S ROAD (OscVoice → GainCache): 320 partials, the cutoff moving every block (so the filter group recomputes, the
	// cost F2 adds to), and held (nothing recomputes: the product alone). The median of 2000 blocks, in rounds alternating the order
	// so a shared host's load lands on every configuration (benchmarking-on-a-shared-host).
	struct Config {
		const char* name;
		bool second;
		bool parallel;
		bool bell;
		bool moving;
	};
	const std::vector<Config> configs {
		{ "one filter, moving", false, false, false, true },  { "two Series, moving", true, false, false, true },
		{ "two Parallel, moving", true, true, false, true },  { "two Parallel, Bell 2, moving", true, true, true, true },
		{ "one filter, held", false, false, false, false },   { "two Parallel, held", true, true, false, false },
	};
	const Spectrum s = harmonics(110.0);
	std::vector<std::vector<double>> medians(configs.size());
	for (int round = 0; round < 6; ++round) {
		for (size_t n = 0; n < configs.size(); ++n) {
			const size_t c = round % 2 == 0 ? n : configs.size() - 1 - n;
			const Config& cfg = configs[c];
			ShaperParams p {};
			p.filter = { true, FilterMode::LowPass, 2000.0f, 0.3f, 0.4f, 0.0f };
			if (cfg.second) {
				p.filter2 = { true, cfg.bell ? FilterMode::Bell : FilterMode::HighPass, 300.0f, 0.2f, 0.3f, 0.0f };
				p.filter2.gainDb = 6.0f;
				p.parallel = cfg.parallel ? 1.0f : 0.0f;
			}
			GainCache<kPartials> cache;
			float out[kPartials];
			std::vector<double> times;
			for (int b = 0; b < 2200; ++b) {
				if (cfg.moving) {
					p.filter.cutoffHz = 1500.0f + 500.0f * static_cast<float>(b % 64) / 64.0f;
				}
				const auto t0 = std::chrono::steady_clock::now();
				cache.update(p, 110.0, s.hz, kPartials);
				cache.apply(s.base, kPartials, out);
				const auto t1 = std::chrono::steady_clock::now();
				if (b >= 200) {
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
