/*
 * Copyright © 2024-2025 Owlet Records
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

#pragma once

#include "owltt_ref/fast_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

// 3-band crossover filters, ported from the firmware's allpass_crossover.h and lr_crossover.h.
//
// THE PORT: the firmware processes L/R in parallel as q31_t NEON vectors through fixed-point
// stereo filter components. Feathers runs scalar float, so this is a from-scratch implementation
// of the same filter topologies with float state and float coefficients. The math is identical;
// only the arithmetic is restated.
//
// Ten crossover types, ordered by CPU cost (cheapest first), matching the firmware's crossoverType_
// 0-9 switch in multiband.h:render():
//
//   0  AP 6dB    AllpassCrossover<1>  — 6dB/oct, 2 ops/ch, perfect reconstruction
//   1  Quirky    AllpassCrossover<2>  — creative, mixed-phase
//   2  Twisted   AllpassCrossoverTwisted — 2 stages, twist-blended coefficients
//   3  Weird     AllpassCrossover<3>  — creative, deeper phase smear
//   4  LR2 Fast  LRCrossover<2,false> — 12dB/oct, no phase comp
//   5  LR2 Full  LRCrossover<2,true>  — 12dB/oct, with phase comp
//   6  LR4 Fast  LRCrossover<4,false> — 24dB/oct, no phase comp
//   7  LR4 Full  LRCrossover<4,true>  — 24dB/oct, with phase comp
//   8  Inverted  AllpassCrossover<1> with swapped low/high bands
//   9  Twist3    AllpassCrossoverTwist3 — 3 stages, progressive coefficient blending
//
// Allpass types reconstruct perfectly: low + mid + high IS the input, sample for sample. The LR
// types are Linkwitz-Riley pairs since backlog ER (2026-09-16): low + mid + high has the input's
// MAGNITUDE at every frequency and the input's phase through one allpass per crossover — which is
// what a real 12 or 24 dB/oct crossover costs, and what `LRCrossover::alignDry` hands the blend so
// the wet/dry mix does not pay it twice. The firmware's "LR" (`lr_crossover.h`) was a subtraction,
// `rest = x - LP(x)`, which sums exactly and separates at 6 dB/oct with a half-amplitude leak two
// octaves below the corner; it is not what this file ports any more.

namespace feathers::fx::owltt_ref {

/// Output of a 3-band crossover: the three frequency bands.
struct CrossoverBands {
	float low;
	float mid;
	float high;
};

/// One first-order lowpass section. Transposed direct form II.
struct OnePoleLP {
	float z1 = 0.0f;

	void reset() { z1 = 0.0f; }

	[[nodiscard]] float process(float x, float coeff) {
		z1 += coeff * (x - z1);
		return z1;
	}
};

/// One first-order allpass section.
struct OnePoleAP {
	float z1 = 0.0f;

	void reset() { z1 = 0.0f; }

	[[nodiscard]] float process(float x, float coeff) {
		const float out = coeff * x + z1;
		z1 = x - coeff * out;
		return out;
	}
};

/// Allpass subtraction crossover. ORDER=1 is the correct 6dB/oct crossover; higher orders are
/// "broken" but creatively useful (Quirky, Weird). Perfect reconstruction: low+mid+high = input.
template <int ORDER>
class AllpassCrossover {
public:
	AllpassCrossover() = default;

	void setLowCrossover(float freqHz, float sampleRate) {
		lowCrossoverHz_ = freqHz;
		lowCoeff_ = calculateCoefficient(freqHz, sampleRate);
	}

	void setHighCrossover(float freqHz, float sampleRate) {
		highCrossoverHz_ = freqHz;
		highCoeff_ = calculateCoefficient(freqHz, sampleRate);
	}

	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

	void process(float input, CrossoverBands& out) {
		float apLow = input;
		for (auto& ap : apLow_) {
			apLow = ap.process(apLow, lowCoeff_);
		}
		const float low = (input + apLow) * 0.5f;
		const float rest = (input - apLow) * 0.5f;

		float apHigh = rest;
		for (auto& ap : apHigh_) {
			apHigh = ap.process(apHigh, highCoeff_);
		}
		const float mid = (rest + apHigh) * 0.5f;
		const float high = (rest - apHigh) * 0.5f;

		out.low = low;
		out.mid = mid;
		out.high = high;
	}

	void reset() {
		for (auto& ap : apLow_) {
			ap.reset();
		}
		for (auto& ap : apHigh_) {
			ap.reset();
		}
	}

private:
	/// THE ALLPASS COEFFICIENT, WHICH IS NOT THE LOWPASS ONE.
	///
	/// `OnePoleAP` realises `H(z) = (a + z^-1) / (1 + a z^-1)`. For that form the corner is at
	/// `a = (t - 1) / (t + 1)` with `t = tan(pi * f / fs)`. This returned `t / (1 + t)`, which is the
	/// one-pole LOWPASS coefficient -- correct for `LRCrossover`, which really does lowpass, and
	/// wrong here.
	///
	/// MEASURED, WHICH IS HOW IT WAS FOUND. can you verify the crossover bands are working
	/// properly. Muting the LOW band attenuated EVERY frequency -- 60 Hz by 48 dB but 9 kHz by 5.4
	/// -- while muting MID or HIGH did nothing at all. All the signal was in one band.
	///
	/// THE SIZE OF THE ERROR: at 200 Hz and 48 kHz, `t` is 0.0131, so this returned +0.0129 where
	/// -0.9742 was wanted. With a tiny positive `a` the "allpass" is very nearly a plain one-sample
	/// delay, so `(input + apLow) / 2` is a COMB rather than a lowpass -- which is exactly the
	/// measured shape: everything in LOW, with a slow frequency-dependent leak into the others.
	///
	/// `low + mid + high == input` HELD THROUGHOUT, which is why nothing caught it: the class's own
	/// stated invariant is perfect reconstruction, and a comb splitter reconstructs perfectly too. A
	/// sum that is right says nothing about whether the parts are.
	[[nodiscard]] static float calculateCoefficient(float freqHz, float sampleRate) {
		const float fc = std::clamp(freqHz / sampleRate, 0.001f, 0.49f);
		const float t = fastTan(3.14159265358979f * fc);
		return (t - 1.0f) / (t + 1.0f);
	}

	std::array<OnePoleAP, ORDER> apLow_{};
	std::array<OnePoleAP, ORDER> apHigh_{};

	float lowCoeff_ = calculateCoefficient(200.0f, 48000.0f);
	float highCoeff_ = calculateCoefficient(2000.0f, 48000.0f);

	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

/// "Twisted" crossover — 2 allpass stages with twist-blended coefficients. twist=0 behaves like
/// Quirky (same coeff both stages); twist=1 fully twists (stage 2 uses opposite crossover's coeff).
class AllpassCrossoverTwisted {
public:
	AllpassCrossoverTwisted() = default;

	void setLowCrossover(float freqHz, float sampleRate) {
		lowCrossoverHz_ = freqHz;
		lowCoeff_ = calculateCoefficient(freqHz, sampleRate);
		updateBlendedCoeffs();
	}

	void setHighCrossover(float freqHz, float sampleRate) {
		highCrossoverHz_ = freqHz;
		highCoeff_ = calculateCoefficient(freqHz, sampleRate);
		updateBlendedCoeffs();
	}

	void setTwist(float twist) {
		twist_ = std::clamp(twist, 0.0f, 1.0f);
		updateBlendedCoeffs();
	}

	[[nodiscard]] float getTwist() const { return twist_; }
	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

	void process(float input, CrossoverBands& out) {
		const float apLow1 = apLow1_.process(input, lowCoeff_);
		const float apLow2 = apLow2_.process(apLow1, lowStage2Coeff_);
		const float low = (input + apLow2) * 0.5f;
		const float rest = (input - apLow2) * 0.5f;

		const float apHigh1 = apHigh1_.process(rest, highCoeff_);
		const float apHigh2 = apHigh2_.process(apHigh1, highStage2Coeff_);
		const float mid = (rest + apHigh2) * 0.5f;
		const float high = (rest - apHigh2) * 0.5f;

		out.low = low;
		out.mid = mid;
		out.high = high;
	}

	void reset() {
		apLow1_.reset();
		apLow2_.reset();
		apHigh1_.reset();
		apHigh2_.reset();
	}

private:
	/// THE ALLPASS COEFFICIENT, WHICH IS NOT THE LOWPASS ONE.
	///
	/// `OnePoleAP` realises `H(z) = (a + z^-1) / (1 + a z^-1)`. For that form the corner is at
	/// `a = (t - 1) / (t + 1)` with `t = tan(pi * f / fs)`. This returned `t / (1 + t)`, which is the
	/// one-pole LOWPASS coefficient -- correct for `LRCrossover`, which really does lowpass, and
	/// wrong here.
	///
	/// MEASURED, WHICH IS HOW IT WAS FOUND. can you verify the crossover bands are working
	/// properly. Muting the LOW band attenuated EVERY frequency -- 60 Hz by 48 dB but 9 kHz by 5.4
	/// -- while muting MID or HIGH did nothing at all. All the signal was in one band.
	///
	/// THE SIZE OF THE ERROR: at 200 Hz and 48 kHz, `t` is 0.0131, so this returned +0.0129 where
	/// -0.9742 was wanted. With a tiny positive `a` the "allpass" is very nearly a plain one-sample
	/// delay, so `(input + apLow) / 2` is a COMB rather than a lowpass -- which is exactly the
	/// measured shape: everything in LOW, with a slow frequency-dependent leak into the others.
	///
	/// `low + mid + high == input` HELD THROUGHOUT, which is why nothing caught it: the class's own
	/// stated invariant is perfect reconstruction, and a comb splitter reconstructs perfectly too. A
	/// sum that is right says nothing about whether the parts are.
	[[nodiscard]] static float calculateCoefficient(float freqHz, float sampleRate) {
		const float fc = std::clamp(freqHz / sampleRate, 0.001f, 0.49f);
		const float t = fastTan(3.14159265358979f * fc);
		return (t - 1.0f) / (t + 1.0f);
	}

	void updateBlendedCoeffs() {
		const float t = twist_;
		lowStage2Coeff_ = lowCoeff_ + (highCoeff_ - lowCoeff_) * t;
		highStage2Coeff_ = highCoeff_ + (lowCoeff_ - highCoeff_) * t;
	}

	OnePoleAP apLow1_ {};
	OnePoleAP apLow2_ {};
	OnePoleAP apHigh1_ {};
	OnePoleAP apHigh2_ {};

	float lowCoeff_ = calculateCoefficient(200.0f, 48000.0f);
	float highCoeff_ = calculateCoefficient(2000.0f, 48000.0f);
	float lowStage2Coeff_ = lowCoeff_;
	float highStage2Coeff_ = highCoeff_;
	float twist_ = 1.0f;
	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

/// "Twist3" crossover — 3 allpass stages with progressive coefficient blending. Combines Twisted's
/// coefficient mixing with Weird's 3-stage depth.
class AllpassCrossoverTwist3 {
public:
	AllpassCrossoverTwist3() = default;

	void setLowCrossover(float freqHz, float sampleRate) {
		lowCrossoverHz_ = freqHz;
		lowCoeff_ = calculateCoefficient(freqHz, sampleRate);
		updateBlendedCoeffs();
	}

	void setHighCrossover(float freqHz, float sampleRate) {
		highCrossoverHz_ = freqHz;
		highCoeff_ = calculateCoefficient(freqHz, sampleRate);
		updateBlendedCoeffs();
	}

	void setTwist(float twist) {
		twist_ = std::clamp(twist, 0.0f, 1.0f);
		updateBlendedCoeffs();
	}

	[[nodiscard]] float getTwist() const { return twist_; }
	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

	void process(float input, CrossoverBands& out) {
		const float apLow1 = apLow1_.process(input, lowCoeff_);
		const float apLow2 = apLow2_.process(apLow1, lowStage2Coeff_);
		const float apLow3 = apLow3_.process(apLow2, lowStage3Coeff_);
		const float low = (input + apLow3) * 0.5f;
		const float rest = (input - apLow3) * 0.5f;

		const float apHigh1 = apHigh1_.process(rest, highCoeff_);
		const float apHigh2 = apHigh2_.process(apHigh1, highStage2Coeff_);
		const float apHigh3 = apHigh3_.process(apHigh2, highStage3Coeff_);
		const float mid = (rest + apHigh3) * 0.5f;
		const float high = (rest - apHigh3) * 0.5f;

		out.low = low;
		out.mid = mid;
		out.high = high;
	}

	void reset() {
		apLow1_.reset();
		apLow2_.reset();
		apLow3_.reset();
		apHigh1_.reset();
		apHigh2_.reset();
		apHigh3_.reset();
	}

private:
	/// THE ALLPASS COEFFICIENT, WHICH IS NOT THE LOWPASS ONE.
	///
	/// `OnePoleAP` realises `H(z) = (a + z^-1) / (1 + a z^-1)`. For that form the corner is at
	/// `a = (t - 1) / (t + 1)` with `t = tan(pi * f / fs)`. This returned `t / (1 + t)`, which is the
	/// one-pole LOWPASS coefficient -- correct for `LRCrossover`, which really does lowpass, and
	/// wrong here.
	///
	/// MEASURED, WHICH IS HOW IT WAS FOUND. can you verify the crossover bands are working
	/// properly. Muting the LOW band attenuated EVERY frequency -- 60 Hz by 48 dB but 9 kHz by 5.4
	/// -- while muting MID or HIGH did nothing at all. All the signal was in one band.
	///
	/// THE SIZE OF THE ERROR: at 200 Hz and 48 kHz, `t` is 0.0131, so this returned +0.0129 where
	/// -0.9742 was wanted. With a tiny positive `a` the "allpass" is very nearly a plain one-sample
	/// delay, so `(input + apLow) / 2` is a COMB rather than a lowpass -- which is exactly the
	/// measured shape: everything in LOW, with a slow frequency-dependent leak into the others.
	///
	/// `low + mid + high == input` HELD THROUGHOUT, which is why nothing caught it: the class's own
	/// stated invariant is perfect reconstruction, and a comb splitter reconstructs perfectly too. A
	/// sum that is right says nothing about whether the parts are.
	[[nodiscard]] static float calculateCoefficient(float freqHz, float sampleRate) {
		const float fc = std::clamp(freqHz / sampleRate, 0.001f, 0.49f);
		const float t = fastTan(3.14159265358979f * fc);
		return (t - 1.0f) / (t + 1.0f);
	}

	void updateBlendedCoeffs() {
		const float t = twist_;
		lowStage2Coeff_ = lowCoeff_ + (highCoeff_ - lowCoeff_) * t * 0.5f;
		highStage2Coeff_ = highCoeff_ + (lowCoeff_ - highCoeff_) * t * 0.5f;
		lowStage3Coeff_ = lowCoeff_ + (highCoeff_ - lowCoeff_) * t;
		highStage3Coeff_ = highCoeff_ + (lowCoeff_ - highCoeff_) * t;
	}

	OnePoleAP apLow1_ {};
	OnePoleAP apLow2_ {};
	OnePoleAP apLow3_ {};
	OnePoleAP apHigh1_ {};
	OnePoleAP apHigh2_ {};
	OnePoleAP apHigh3_ {};

	float lowCoeff_ = calculateCoefficient(200.0f, 48000.0f);
	float highCoeff_ = calculateCoefficient(2000.0f, 48000.0f);
	float lowStage2Coeff_ = lowCoeff_;
	float lowStage3Coeff_ = lowCoeff_;
	float highStage2Coeff_ = highCoeff_;
	float highStage3Coeff_ = highCoeff_;
	float twist_ = 1.0f;
	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

//-- THE LINKWITZ-RILEY PAIRS (backlog ER, 2026-09-16).
//
// WHAT WAS WRONG. The port — and the firmware it ported — built its "LR" as `low = LP^N(x)`,
// `rest = x - low`, and called the rest a highpass. It is not one. A lowpass lags, and `x - lagged(x)`
// is the input times |1 - H|, which two octaves below a 200 Hz corner is about a HALF: MEASURED 0.49 of
// a 60 Hz tone in MID, from a splitter named for 24 dB/oct. The sum was exact by construction, which
// is why the only case here — "sums flat" — never saw it. A sum that is right says nothing about
// whether the parts are.
//
// WHAT A LINKWITZ-RILEY PAIR IS. Two Butterworth filters of the same order and corner, SQUARED: the
// lowpass is B² and the highpass its complement B_hp². Each is -6 dB at the corner, they fall at
// 12 (LR2) or 24 (LR4) dB/oct, and their sum is an ALLPASS — |LP + HP| = 1 at every frequency —
// because B_lp² ± B_hp² = D(-s)/D(s) for the Butterworth denominator D. For the second-order pair the
// sign is PLUS; for the first-order pair it is MINUS, so LR2's high side must be inverted for the flat
// sum (`Lr2Split::split` does it; a pair that forgot would have a notch to zero at the corner).
//
// THE THREE-BAND FORM. low = LP_lo(x); rest = HP_lo(x); mid = LP_hi(rest); high = HP_hi(rest). Then
// mid + high = AP_hi(rest), and for the whole sum to be one allpass the LOW band is put through the
// SAME AP_hi (`PHASE_COMPENSATED`): low + mid + high = AP_hi(AP_lo(x)). This is the compensation the
// firmware's "Full" variants gesture at with allpasses on the low band; here it is the exact filter
// (the first-order `OnePoleAP` at (K-1)/(K+1), or the biquad with the reversed denominator), so the
// sum equals `alignDry` to float precision. The "Fast" variants skip it and ripple by well under a
// dB around the low corner, as the firmware's comment promised.
//
// WHY `alignDry` EXISTS. OWLTT blends wet against dry. An allpass reaches -180° — AT the corner for
// LR4, across the mid band for LR2 — and a 50 % blend against the RAW input cancels there: MEASURED,
// a 2 kHz tone through a 2 kHz LR4 crossover goes to zero at half Blend. The dry side runs through the
// same two allpasses and the mix is flat. Phase is the whole price of a real crossover, paid once.

/// One TPT (bilinear) first-order section: a lowpass whose highpass complement `x - lowpass(x)` is
/// EXACT, which is what makes a first-order Butterworth pair out of one state. `g` is
/// `K / (1 + K)`, `K = tan(pi * fc / fs)`.
struct TptOnePole {
	float s = 0.0f;

	void reset() { s = 0.0f; }

	[[nodiscard]] float lowpass(float x, float g) {
		const float v = (x - s) * g;
		const float y = v + s;
		s = y + v;
		return y;
	}
};

/// A biquad's coefficients, `a0` normalised away. Transposed direct form II in `Biquad`.
struct BiquadCoeffs {
	float b0 = 1.0f;
	float b1 = 0.0f;
	float b2 = 0.0f;
	float a1 = 0.0f;
	float a2 = 0.0f;
};

struct Biquad {
	float z1 = 0.0f;
	float z2 = 0.0f;

	void reset() {
		z1 = 0.0f;
		z2 = 0.0f;
	}

	[[nodiscard]] float process(float x, const BiquadCoeffs& c) {
		const float y = c.b0 * x + z1;
		z1 = c.b1 * x - c.a1 * y + z2;
		z2 = c.b2 * x - c.a2 * y;
		return y;
	}
};

/// The corner as the bilinear transform wants it: `K = tan(pi * fc / fs)`, corner held inside the
/// band the transform can express.
[[nodiscard]] inline float bilinearK(float freqHz, float sampleRate) {
	const float fc = std::clamp(freqHz / sampleRate, 0.001f, 0.49f);
	return std::tan(3.14159265358979f * fc);
}

/// A two-way LR2 split: first-order Butterworth SQUARED on each side. `low - high` is the first-order
/// allpass, so the high side is INVERTED here and `low + high` is that allpass.
struct Lr2Split {
	struct Coeffs {
		float g = 0.0f;  // TptOnePole gain
		float ap = 0.0f; // OnePoleAP coefficient for the same corner: (K - 1) / (K + 1)
	};

	[[nodiscard]] static Coeffs coeffsFor(float freqHz, float sampleRate) {
		const float k = bilinearK(freqHz, sampleRate);
		return { k / (1.0f + k), (k - 1.0f) / (k + 1.0f) };
	}

	struct Allpass {
		OnePoleAP ap {};
		void reset() { ap.reset(); }
		[[nodiscard]] float process(float x, const Coeffs& c) { return ap.process(x, c.ap); }
	};

	TptOnePole lp1 {}, lp2 {}, hp1 {}, hp2 {};

	void reset() {
		lp1.reset();
		lp2.reset();
		hp1.reset();
		hp2.reset();
	}

	void split(float x, const Coeffs& c, float& low, float& high) {
		low = lp2.lowpass(lp1.lowpass(x, c.g), c.g);
		const float h1 = x - hp1.lowpass(x, c.g);
		const float h2 = h1 - hp2.lowpass(h1, c.g);
		high = -h2;
	}
};

/// A two-way LR4 split: second-order Butterworth (Q = 1/sqrt 2) SQUARED on each side. `low + high`
/// is the second-order allpass with the same denominator, no inversion.
struct Lr4Split {
	struct Coeffs {
		BiquadCoeffs lp {};
		BiquadCoeffs hp {};
		BiquadCoeffs ap {};
	};

	[[nodiscard]] static Coeffs coeffsFor(float freqHz, float sampleRate) {
		const float k = bilinearK(freqHz, sampleRate);
		const float k2 = k * k;
		const float sqrt2k = 1.41421356237f * k;
		const float norm = 1.0f / (1.0f + sqrt2k + k2);
		const float a1 = 2.0f * (k2 - 1.0f) * norm;
		const float a2 = (1.0f - sqrt2k + k2) * norm;
		Coeffs c;
		c.lp = { k2 * norm, 2.0f * k2 * norm, k2 * norm, a1, a2 };
		c.hp = { norm, -2.0f * norm, norm, a1, a2 };
		// D(-s)/D(s): the numerator is the denominator read backwards.
		c.ap = { a2, a1, 1.0f, a1, a2 };
		return c;
	}

	struct Allpass {
		Biquad b {};
		void reset() { b.reset(); }
		[[nodiscard]] float process(float x, const Coeffs& c) { return b.process(x, c.ap); }
	};

	Biquad lp1 {}, lp2 {}, hp1 {}, hp2 {};

	void reset() {
		lp1.reset();
		lp2.reset();
		hp1.reset();
		hp2.reset();
	}

	void split(float x, const Coeffs& c, float& low, float& high) {
		low = lp2.process(lp1.process(x, c.lp), c.lp);
		high = hp2.process(hp1.process(x, c.hp), c.hp);
	}
};

//-- THE K-BAND CASCADE (the Rack's Bands mode).
//
// The three-band form above, longer: rest_0 = x; band_k = LP_{f_k}(rest_{k-1}) · Π_{j>k} AP_{f_j};
// rest_k = HP_{f_k}(rest_{k-1}); band_K = rest_{K-1}. Then Σ band_k = Π_j AP_{f_j}(x) (DERIVED, and
// the three-band case is the class below, already measured). K is the most bands an instance can
// run; `bands` (2..K) is how many it runs now, so one instance serves the Rack's 2, 3 and 5.
//
// THE CORNERS' COEFFICIENTS ARE THE CALLER'S, passed in: a Rack runs one cascade per channel, one
// band path per band and the dry allpasses over the same corners, and recomputes each corner once.
//
// **OWLTT'S THREE-BAND PATH IS THE K = 3 INSTANCE, BIT FOR BIT** — the same splits, the same
// compensation allpass, the same order of operations (`test_fx_owltt_crossover`, "MultiSplit<Split, 3>
// is OWLTT's crossover bit for bit"), which is why `LinkwitzRileyCrossover` now runs on it.

/// A state below this is flushed to exact zero by `flushTiny`: IIR state decays toward a denormal, not
/// toward zero, and a filter parked there keeps a holder awake for ever (CLAUDE.md's SLEEP rule (2)).
inline constexpr float kFlushBelow = 1.0e-20f;

inline void flushState(float& s) {
	if (s > -kFlushBelow && s < kFlushBelow) {
		s = 0.0f;
	}
}
inline void flushTiny(TptOnePole& f) { flushState(f.s); }
inline void flushTiny(OnePoleAP& f) { flushState(f.z1); }
inline void flushTiny(Biquad& f) {
	flushState(f.z1);
	flushState(f.z2);
}
[[nodiscard]] inline bool atRest(const TptOnePole& f) { return f.s == 0.0f; }
[[nodiscard]] inline bool atRest(const OnePoleAP& f) { return f.z1 == 0.0f; }
[[nodiscard]] inline bool atRest(const Biquad& f) { return f.z1 == 0.0f && f.z2 == 0.0f; }
inline void flushTiny(Lr2Split& s) {
	flushTiny(s.lp1);
	flushTiny(s.lp2);
	flushTiny(s.hp1);
	flushTiny(s.hp2);
}
inline void flushTiny(Lr4Split& s) {
	flushTiny(s.lp1);
	flushTiny(s.lp2);
	flushTiny(s.hp1);
	flushTiny(s.hp2);
}
[[nodiscard]] inline bool atRest(const Lr2Split& s) { return atRest(s.lp1) && atRest(s.lp2) && atRest(s.hp1) && atRest(s.hp2); }
[[nodiscard]] inline bool atRest(const Lr4Split& s) { return atRest(s.lp1) && atRest(s.lp2) && atRest(s.hp1) && atRest(s.hp2); }
inline void flushTiny(Lr2Split::Allpass& a) { flushTiny(a.ap); }
inline void flushTiny(Lr4Split::Allpass& a) { flushTiny(a.b); }
[[nodiscard]] inline bool atRest(const Lr2Split::Allpass& a) { return atRest(a.ap); }
[[nodiscard]] inline bool atRest(const Lr4Split::Allpass& a) { return atRest(a.b); }

//-- HALF A LINKWITZ-RILEY SPLIT (backlog OS: the Rack's Bands "Both"). An LR pair is a Butterworth section SQUARED on each
// side, so ONE section before a chain and the OTHER after it multiply to the LR pair exactly — and with empty chains the
// bands then sum to Π AP, flat, where filtering with the whole pair twice dipped about 6 dB at every corner. The LR2
// pair's inverted high side is inverted once, in the AFTER half. Same coefficients as the whole split.

/// HALF AN LR2 SPLIT: one first-order Butterworth each side; `INVERT_HIGH` for the half that carries the pair's sign.
template <bool INVERT_HIGH> struct Lr2Half {
	using Coeffs = Lr2Split::Coeffs;
	using Allpass = Lr2Split::Allpass;
	TptOnePole lp {}, hp {};
	void reset() {
		lp.reset();
		hp.reset();
	}
	void split(float x, const Coeffs& c, float& low, float& high) {
		low = lp.lowpass(x, c.g);
		const float h = x - hp.lowpass(x, c.g);
		high = INVERT_HIGH ? -h : h;
	}
};

/// HALF AN LR4 SPLIT: one second-order Butterworth section each side.
struct Lr4Half {
	using Coeffs = Lr4Split::Coeffs;
	using Allpass = Lr4Split::Allpass;
	Biquad lp {}, hp {};
	void reset() {
		lp.reset();
		hp.reset();
	}
	void split(float x, const Coeffs& c, float& low, float& high) {
		low = lp.process(x, c.lp);
		high = hp.process(x, c.hp);
	}
};

template <bool I> inline void flushTiny(Lr2Half<I>& s) {
	flushTiny(s.lp);
	flushTiny(s.hp);
}
inline void flushTiny(Lr4Half& s) {
	flushTiny(s.lp);
	flushTiny(s.hp);
}
template <bool I> [[nodiscard]] inline bool atRest(const Lr2Half<I>& s) { return atRest(s.lp) && atRest(s.hp); }
[[nodiscard]] inline bool atRest(const Lr4Half& s) { return atRest(s.lp) && atRest(s.hp); }

/// THE TWO HALVES OF A SPLIT: `Before` (the input's split) and `After` (each band's path on its chain's output).
template <class Split> struct Halves;
template <> struct Halves<Lr2Split> {
	using Before = Lr2Half<false>;
	using After = Lr2Half<true>;
};
template <> struct Halves<Lr4Split> {
	using Before = Lr4Half;
	using After = Lr4Half;
};

template <class Split, int K, bool PHASE_COMPENSATED = true>
struct MultiSplit {
	static_assert(K >= 2, "a crossover has at least two bands");
	using Coeffs = typename Split::Coeffs;
	using Allpass = typename Split::Allpass;
	static constexpr int kMaxBands = K;

	Split splits[K - 1] {};
	/// `comp[k][j]`: band k's compensation through corner j's allpass (j > k).
	Allpass comp[K][K] {};

	void reset() {
		for (Split& s : splits) {
			s.reset();
		}
		for (auto& row : comp) {
			for (Allpass& a : row) {
				a.reset();
			}
		}
	}

	/// `x` into `bands` bands (2..K), lowest first, over `corners[0 .. bands-2]`.
	void process(float x, const Coeffs* corners, int bands, float* out) {
		float rest = x;
		for (int j = 0; j + 1 < bands; ++j) {
			float low = 0.0f;
			float high = 0.0f;
			splits[j].split(rest, corners[j], low, high);
			out[j] = low;
			rest = high;
		}
		out[bands - 1] = rest;
		if constexpr (PHASE_COMPENSATED) {
			for (int k = 0; k + 2 < bands; ++k) {
				for (int j = k + 1; j + 1 < bands; ++j) {
					out[k] = comp[k][j].process(out[k], corners[j]);
				}
			}
		}
	}

	void flush() {
		for (Split& s : splits) {
			flushTiny(s);
		}
		for (auto& row : comp) {
			for (Allpass& a : row) {
				flushTiny(a);
			}
		}
	}
	[[nodiscard]] bool settled() const {
		for (const Split& s : splits) {
			if (!atRest(s)) {
				return false;
			}
		}
		for (const auto& row : comp) {
			for (const Allpass& a : row) {
				if (!atRest(a)) {
					return false;
				}
			}
		}
		return true;
	}
};

/// **ONE BAND OF THE CASCADE, OF A SIGNAL OF ITS OWN** (§4c, After and Both): the same splits below the
/// band (their high side kept), its own lowpass, and the compensation above it — so band k of x here is
/// band k of `MultiSplit` of x, operation for operation.
template <class Split, int K>
struct BandPath {
	using Coeffs = typename Split::Coeffs;
	using Allpass = typename Split::Allpass;

	Split splits[K - 1] {};
	Allpass comp[K] {};

	void reset() {
		for (Split& s : splits) {
			s.reset();
		}
		for (Allpass& a : comp) {
			a.reset();
		}
	}

	[[nodiscard]] float process(float x, const Coeffs* corners, int bands, int band) {
		float rest = x;
		for (int j = 0; j < band; ++j) {
			float low = 0.0f;
			float high = 0.0f;
			splits[j].split(rest, corners[j], low, high);
			rest = high;
		}
		if (band + 1 >= bands) {
			return rest;
		}
		float low = 0.0f;
		float high = 0.0f;
		splits[band].split(rest, corners[band], low, high);
		for (int j = band + 1; j + 1 < bands; ++j) {
			low = comp[j].process(low, corners[j]);
		}
		return low;
	}

	void flush() {
		for (Split& s : splits) {
			flushTiny(s);
		}
		for (Allpass& a : comp) {
			flushTiny(a);
		}
	}
	[[nodiscard]] bool settled() const {
		for (const Split& s : splits) {
			if (!atRest(s)) {
				return false;
			}
		}
		for (const Allpass& a : comp) {
			if (!atRest(a)) {
				return false;
			}
		}
		return true;
	}
};

/// **Π AP_{f_j}: WHAT THE BANDS SUM TO, OF A SIGNAL OF ITS OWN** — the dry a wet/dry mix is made against
/// (`alignDry`'s reason, below), lowest corner first.
template <class Split, int K>
struct AllpassChain {
	using Coeffs = typename Split::Coeffs;
	using Allpass = typename Split::Allpass;

	Allpass ap[K - 1] {};

	void reset() {
		for (Allpass& a : ap) {
			a.reset();
		}
	}
	[[nodiscard]] float process(float x, const Coeffs* corners, int bands) {
		for (int j = 0; j + 1 < bands; ++j) {
			x = ap[j].process(x, corners[j]);
		}
		return x;
	}
	void flush() {
		for (Allpass& a : ap) {
			flushTiny(a);
		}
	}
	[[nodiscard]] bool settled() const {
		for (const Allpass& a : ap) {
			if (!atRest(a)) {
				return false;
			}
		}
		return true;
	}
};

template <class Split, bool PHASE_COMPENSATED>
class LinkwitzRileyCrossover {
public:
	using Coeffs = typename Split::Coeffs;

	LinkwitzRileyCrossover() = default;

	void setLowCrossover(float freqHz, float sampleRate) {
		lowCrossoverHz_ = freqHz;
		corners_[0] = Split::coeffsFor(freqHz, sampleRate);
	}

	void setHighCrossover(float freqHz, float sampleRate) {
		highCrossoverHz_ = freqHz;
		corners_[1] = Split::coeffsFor(freqHz, sampleRate);
	}

	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

	/// THE K = 3 CASCADE (`MultiSplit`): low = LP_lo(x), rest = HP_lo(x), mid + high = the high split of
	/// the rest, and — compensated — the low band through AP_hi so the whole sum is one allpass.
	void process(float input, CrossoverBands& out) {
		float bands[3] = {};
		split_.process(input, corners_, 3, bands);
		out.low = bands[0];
		out.mid = bands[1];
		out.high = bands[2];
	}

	/// THE DRY SIGNAL IN THE BANDS' PHASE: the input through both crossovers' allpasses, which is what
	/// `low + mid + high` is of it before any band gain (to float precision for the compensated
	/// variants, to a ripple under a dB for the Fast ones). A wet/dry blend mixes against THIS, not
	/// against the raw input — see the header note. Call it every sample so its state is warm.
	[[nodiscard]] float alignDry(float input) { return dry_.process(input, corners_, 3); }

	void reset() {
		split_.reset();
		dry_.reset();
	}

private:
	MultiSplit<Split, 3, PHASE_COMPENSATED> split_ {};
	AllpassChain<Split, 3> dry_ {};

	Coeffs corners_[2] = { Split::coeffsFor(200.0f, 48000.0f), Split::coeffsFor(2000.0f, 48000.0f) };

	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

/// The spelling the mode table and the device use: SECTIONS is the LR order, 2 or 4.
template <int SECTIONS, bool PHASE_COMPENSATED = true>
using LRCrossover = LinkwitzRileyCrossover<std::conditional_t<SECTIONS >= 4, Lr4Split, Lr2Split>, PHASE_COMPENSATED>;

using APCrossover1 = AllpassCrossover<1>; // AP 6dB — crossover type 0
using APCrossover2 = AllpassCrossover<2>; // Quirky — crossover type 1
using APCrossover3 = AllpassCrossover<3>; // Weird — crossover type 3
using LR2CrossoverFull = LRCrossover<2, true>;
using LR2CrossoverFast = LRCrossover<2, false>;
using LR4CrossoverFull = LRCrossover<4, true>;
using LR4CrossoverFast = LRCrossover<4, false>;

} // namespace feathers::fx::owltt_ref
