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

#pragma once

#include "fx/filter/cytomic_svf.h"

#include <algorithm>
#include <cmath>
#include <complex>

// THE AUTOMODULATOR'S FILTER — one state-variable filter read as a CONTINUOUS BLEND of its three
// outputs, where a filter dropdown reads it as one of them.
//
// WHAT IS DIFFERENT ABOUT IT, AND WHY IT IS NOT `MultimodeFilter`. That device picks its output mix
// from a `Type`: lowpass IS (0,0,1), bandpass IS (0,1,0). The automodulator's Filter Mix is a free
// (low, band, high) vector normalised to sum to one and moved every buffer by the phi field — a
// weighting no dropdown entry names, sliding continuously between them as the Type knob turns. So
// this is not a type `MultimodeFilter` is missing; it is a different way of ADDRESSING the same
// filter.
//
// SO IT REUSES THE KERNEL AND NOT THE DEVICE, which is the whole of the design. `feathers::fx::filter::
// CytomicSVF` (Andy Simper's SVF, copied under its MIT carve-out) is the same object `multimode_filter.cpp` runs
// (`using SVF = CytomicSVF`), and `m0/m1/m2` are public members of it — the Cytomic formulation's output stage,
// not an implementation accident. `multimode_filter.h` states the property this leans on: "THE STATE
// UPDATE DOES NOT DEPEND ON THE OUTPUT MIX. ic1eq/ic2eq evolve from the input and from a1/a2/a3
// alone." Widening a shared, heavily-tested device's API for one caller would have been the worse
// trade.
//
// ---------------------------------------------------------------------------------------------
// CYTOMIC, NOT THE FIRMWARE'S CHAMBERLIN — DECIDED 2026-08-29: the modern topology on both.
// ---------------------------------------------------------------------------------------------
//
// `automodulator.cpp:1167` is a Chamberlin SVF: `high = in - low - band*Q; band += high*f; low +=
// band*f`, with `f = cutoff << 1` — a LINEAR frequency map, not `tan(pi*fc/sr)`. That is a dated but
// legitimate filter and porting it faithfully was a real option, so it was escalated rather than
// swapped. Two things settled it:
//
//   - The correctness half: Chamberlin goes unstable as f approaches 0.5 and its frequency map is
//     linear, so the same knob position means different pitches at different sample rates.
//   - THE CEILING WAS NEVER A MUSICAL DECISION. `automodulator.cpp:1029` says so outright:
//     `// kCutoffMax must be < 0x40000000 to avoid overflow when shifted left by 1`, giving
//     "~8 kHz (max safe before << 1 overflow)". A filter whose top is set by where a q31 shift
//     overflows is not an instrument somebody voiced.
//
// The frequency map and the resonance law were a genuine choice and made it; the ceiling was an
// artefact and goes with the representation that caused it.
//
// ---------------------------------------------------------------------------------------------
// THE MIX VECTOR, DERIVED FROM THE KERNEL'S OWN TABLE RATHER THAN FROM A PAPER.
// ---------------------------------------------------------------------------------------------
//
// `setCoeffPostGK` defines the three modes as m-vectors over (v0 = input, v1 = band, v2 = low):
//
//     Lowpass   (0,  0, 1)          so  LP = v2
//     Bandpass  (0,  1, 0)          so  BP = v1
//     Highpass  (1, -k, -1)         so  HP = v0 - k*v1 - v2
//
// A blend `wl*LP + wb*BP + wh*HP` is therefore one m-vector:
//
//     m0 = wh          m1 = wb - wh*k          m2 = wl - wh
//
// Taken from the kernel's table rather than from the Cytomic paper, so it cannot drift from what the
// kernel actually does — and `tests/test_automod_filter.cpp` pins the coupling by requiring that a
// pure (1,0,0) mix produce output BIT-IDENTICAL to the kernel's own `Mode::Lowpass`. If `CytomicSVF` is
// reshaped, that reds a test instead of changing a sound.
//
// `k` IS READ BACK FROM THE OBJECT, NEVER RECOMPUTED — AND TODAY THAT IS DEFENSIVE RATHER THAN
// LOAD-BEARING, which is worth stating so nobody "simplifies" it without knowing what they are
// giving up. The kernel's mapping is `k = 2 - 2*res` over a resonance clamped to 0.98, and ours is clamped
// to the same 0.98, so a hand-written copy of that expression computes the identical number at every
// input. MEASURED: a known-bad replacing the read with `2.0f - 2.0f * resonance_` is GREEN across the
// whole suite.
//
// It stays because the point is not today's arithmetic, it is that ONE PLACE OWNS THE MAPPING. The
// day the kernel tapers its resonance curve, adds a Bell divisor we use, or re-clamps, the read tracks and
// the copy silently does not — and the symptom would be a filter whose highpass content is subtly
// wrong at high resonance, which nothing in a spectrum would obviously name.

namespace feathers::fx::automod {

/// HOW MUCH OF EACH BAND IS HEARD. Normalised on the way in, so the filter's overall loudness does
/// not swing as the Type knob slides the weighting around.
struct FilterMix {
	float low = 1.0f;
	float band = 0.0f;
	float high = 0.0f;

	friend bool operator==(const FilterMix&, const FilterMix&) = default;
};

/// WHERE THE CORNER MAY SIT. The bottom is below hearing and the top is a fraction of Nyquist rather
/// than a fixed Hz, because a fixed ceiling is exactly the artefact the firmware's `<< 1` left behind.
inline constexpr float kFilterMinHz = 20.0f;
inline constexpr float kFilterNyquistFraction = 0.45f;

/// The resonance the filter will accept. Below 1 the SVF is tame; the kernel's k is `2 - 2*res`-ish, and
/// the top is short of self-oscillation for the reason every filter in this project stops short of it.
inline constexpr float kFilterMaxResonance = 0.98f;

// ---------------------------------------------------------------------------------------------- //
// WHERE THE CORNER SITS — AND THE FIRST VERSION OF THIS GOT IT BADLY WRONG.
// ---------------------------------------------------------------------------------------------- //
//
// **THIS BLOCK MOVED HERE FROM `automod.cpp`'s ANONYMOUS NAMESPACE, AND THE MOVE IS THE POINT.** The
// card draws where the corner will go over one LFO cycle, and a picture that computed that from its
// own copy of these three constants would be a second implementation of the sound — the exact defect
// CLAUDE.md names and this editor has shipped ten times as "drawn here, grabbable there". One
// definition, two readers: `Automod::processFrame` and `AutomodCard`'s strip.
//
// `filterCutoffBase` IS AN OFFSET FROM A BRIGHT FLOOR, NOT AN ABSOLUTE POSITION. The firmware says
// so in its own words beside the line — `// Scale filterCutoffBase similarly (it's 0 to 0x7FFFFFFF,
// we want it to add modest offset)` — and its arithmetic confirms it:
//
//     scaledCutoffBase = filterCutoffBase * kCutoffHalfRange / 2
//     filterBase       = kCutoffMid + scaledCutoffBase
//
// so the corner spans `kCutoffMid` to `kCutoffMid + halfRange/2` — **50.1% to 75.0% of its range,
// about 4 kHz to 6 kHz, and NEVER below the midpoint.** The field only ever pushes it UP.
//
// THE FIRST VERSION READ IT AS A POSITION ACROSS 60 Hz..12 kHz, so base 0 landed at 60 Hz. And
// `kFilterCutoffBaseTriangle` has a 25% deadzone, so `filterCutoffBase` is EXACTLY 0.0 across whole
// flavour bands — measured at flavour 0.00, 0.40, 0.45, 0.90, 0.95. **Those bands were a 60 Hz
// lowpass**, which on musical material is -24 dB, and it is what heard as "completely quiet".
// MEASURED before the fix, 21^3 coordinates with the device on: 14.69% of the cube below -18 dB,
// init at -24.4 dB, and the dead regions were flavour SHEETS rather than points.
//
// SO THE FLOOR IS ABSOLUTE AND BRIGHT, matching the firmware's frequencies rather than the fraction.
// The midpoint of OUR range would be about 850 Hz — faithful to the arithmetic and wrong about the
// instrument. **A bright floor that the field pushes up from is what makes this an auto-wah rather
// than a lowpass**, and that is character rather than implementation.
inline constexpr float kFilterBaseLowHz = 4000.0f;
inline constexpr float kFilterBaseHighHz = 6000.0f;

/// HOW FAR FULL MODULATION MOVES THE CORNER, in octaves either way.
///
/// IN OCTAVES RATHER THAN IN HERTZ, which is a deliberate departure. The firmware adds its LFO to a
/// LINEAR q31 cutoff, so its sweep spends most of its travel near the top and crowds the bottom
/// together. Every frequency control in this project is logarithmic for the reason
/// `mod/modulator.h` states about times, and it applies to a sweep as much as to a knob: equal
/// motion should be equal RATIOS, or the wah is uneven across its own range.
///
/// FIVE OCTAVES DOWN FROM 4 kHz REACHES 125 Hz, which is the bottom of where a wah is useful. The
/// clamp in `cornerHzFor` is what stops the top running past Nyquist.
inline constexpr float kFilterModOctaves = 5.0f;

/// The corner for a 0..1 base, before modulation.
inline float baseCutoffHzFor(float position) {
	const float p = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;
	return kFilterBaseLowHz * std::exp2(p * std::log2(kFilterBaseHighHz / kFilterBaseLowHz));
}

/// WHERE THE CORNER ACTUALLY LANDS for one reading of the modulation signal. THE one answer.
///
/// @param base       `AutomodDerived::filterCutoffBase`, 0..1
/// @param modDepth   `AutomodDerived::filterModDepth`, 0..1 — how much of the sweep the FIELD allows
/// @param modSignal  the spring-smoothed LFO, depth and Manual already folded in
/// @param pitchRatio note tracking, 1 for a surface that is not playing a note
/// @param sampleRate what the top is a fraction of — see `kFilterNyquistFraction`
///
/// NON-FINITE IN GIVES `kFilterMinHz`, WHICH IS NOT A NEW BEHAVIOUR. `std::clamp(NaN, lo, hi)`
/// returns NaN (the playbook's 8a trap), and the old inline form passed that straight to
/// `Filter::setState`, whose own `clampCutoff` turned it into `kFilterMinHz`. Answering it here
/// moves the same answer earlier, so the picture and the seam see what the kernel sees rather than a
/// NaN the kernel would have swallowed.
inline float cornerHzFor(float base, float modDepth, float modSignal, float pitchRatio,
                         double sampleRate) {
	const float top = static_cast<float>(((sampleRate > 0.0) ? sampleRate : 48000.0)
	                                     * static_cast<double>(kFilterNyquistFraction));
	const float octaves = modSignal * modDepth * kFilterModOctaves;
	const float hz = baseCutoffHzFor(base) * std::exp2(octaves) * pitchRatio;
	if (!std::isfinite(hz)) {
		return kFilterMinHz;
	}
	return std::clamp(hz, kFilterMinHz, std::max(top, kFilterMinHz));
}

/// THE FILTER'S MAGNITUDE RESPONSE AT ONE FREQUENCY — what a picture of this filter is a picture OF.
///
/// **THE DSP DOES NOT NEED THIS, WHICH IS EXACTLY WHY IT IS DANGEROUS.** Every other shared reading in
/// this device has an audio-path caller keeping it honest; a magnitude has none, so nothing would go
/// red if it quietly stopped describing the filter that plays. Two things stand in for that caller:
/// it is computed from the SAME coefficients the kernel runs — `g = fasttan(pi * fc/sr)` and
/// `k = 2 - 2*res`, taken from `CytomicSVF::setCoeff` rather than from a textbook — and
/// `test_automod_filter.cpp` MEASURES the real `Filter`'s steady-state gain at a spread of
/// frequencies and compares. An algebraic restatement checked against algebra proves nothing.
///
/// `fasttan` RATHER THAN `std::tan`, deliberately. It is a rational approximation and the kernel
/// runs it; a curve drawn with the exact tangent would diverge from the filter that plays exactly
/// where the approximation is worst, which is the top of the range where a wah spends its time.
///
/// THE THREE WEIGHTS ARE THE ANALOG OUTPUTS, not the m-vector. `apply()` derives `m0/m1/m2` FROM
/// (low, band, high) so the kernel's output is `low*LP + band*BP + high*HP`; this reads the weights
/// directly, so the two cannot disagree about what the mix means.
///
/// @param freqHz where to evaluate, in Hz. @returns linear gain, 1.0 being unity.
inline float magnitudeAt(float cutoffHz, float resonance, const FilterMix& mix, float freqHz,
                         double sampleRate) {
	const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
	if (!std::isfinite(freqHz) || freqHz <= 0.0f) {
		freqHz = 1.0f;
	}
	const float fc = std::clamp(std::isfinite(cutoffHz) ? cutoffHz : kFilterMinHz, kFilterMinHz,
	                            static_cast<float>(sr) * kFilterNyquistFraction);
	const float res = std::isfinite(resonance) ? std::clamp(resonance, 0.0f, kFilterMaxResonance)
	                                           : 0.0f;
	const double g = feathers::dsp::fasttan(M_PI * static_cast<double>(fc) / sr);
	const double k = 2.0 - 2.0 * static_cast<double>(res);

	// THE TPT STRUCTURE'S OWN TRANSFER FUNCTIONS, checked at the two frequencies where they are known
	// exactly: at DC the denominator is 4g^2 and the lowpass is 1 with the other two at 0; at Nyquist
	// the denominator is 4 and the highpass is 1 with the other two at 0.
	//
	//   LP = g^2 (1 + z^-1)^2 / D     BP = g (1 - z^-2) / D     HP = (1 - z^-1)^2 / D
	//   D  = (1 + gk + g^2) + (2g^2 - 2) z^-1 + (1 - gk + g^2) z^-2
	const double w = 2.0 * M_PI * std::min(static_cast<double>(freqHz), sr * 0.5) / sr;
	const std::complex<double> z1 = std::polar(1.0, -w);
	const std::complex<double> z2 = z1 * z1;
	const std::complex<double> one { 1.0, 0.0 };

	const std::complex<double> den = (1.0 + g * k + g * g) + (2.0 * g * g - 2.0) * z1
	                                 + (1.0 - g * k + g * g) * z2;
	const std::complex<double> lp = g * g * (one + z1) * (one + z1);
	const std::complex<double> bp = g * (one - z2);
	const std::complex<double> hp = (one - z1) * (one - z1);

	const std::complex<double> num = static_cast<double>(mix.low) * lp
	                                 + static_cast<double>(mix.band) * bp
	                                 + static_cast<double>(mix.high) * hp;
	const double d = std::abs(den);
	if (!(d > 0.0)) {
		return 0.0f;
	}
	return static_cast<float>(std::abs(num) / d);
}

class Filter {
public:
	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		srInv_ = static_cast<float>(1.0 / sampleRate_);
		reset();
		apply();
	}

	/// Zero the filter state, leaving the coefficients alone — they are a function of the settings.
	void reset() { svf_.init(); }

	/// EVERYTHING AT ONCE, AND THAT IS THE POINT RATHER THAN A CONVENIENCE.
	///
	/// `setCoeff` REWRITES m0/m1/m2 from the Mode it is given, so a mix written before it is silently
	/// discarded and a caller that set them in the wrong order would get a plain lowpass with no
	/// error anywhere. Rather than write a comment saying "call these in this order", the two are one
	/// call and the wrong order is not expressible — the playbook's rule that a required parameter
	/// beats a remembered convention.
	void setState(float cutoffHz, float resonance, const FilterMix& mix) {
		setState(cutoffHz, cutoffHz, resonance, mix);
	}

	/// **ONE CORNER PER EAR** (backlog IS). The SVF runs left and right in two SIMD lanes, and its
	/// two-lane `setCoeff` gives each lane its own `g` — so a stereo sweep costs one vector tan, as the
	/// mono one did. The resonance is shared, which keeps `k` equal in both lanes and so the mix below
	/// (written from lane 0's `k`) right for both.
	void setState(float cutoffLeftHz, float cutoffRightHz, float resonance, const FilterMix& mix) {
		cutoffHz_ = clampCutoff(cutoffLeftHz);
		cutoffRightHz_ = clampCutoff(cutoffRightHz);
		resonance_ = std::isfinite(resonance) ? std::clamp(resonance, 0.0f, kFilterMaxResonance) : 0.0f;
		mix_ = normalise(mix);
		apply();
	}

	/// ONE SAMPLE, BOTH CHANNELS, each in its own SIMD lane with its own corner — see `setState`.
	void processSample(float& left, float& right) {
		feathers::fx::filter::CytomicSVF::step(svf_, left, right);
	}

	[[nodiscard]] float cutoffHz() const noexcept { return cutoffHz_; }
	[[nodiscard]] float cutoffRightHz() const noexcept { return cutoffRightHz_; }
	[[nodiscard]] float resonance() const noexcept { return resonance_; }
	[[nodiscard]] const FilterMix& mix() const noexcept { return mix_; }

	/// THE THREE WEIGHTS AS THE KERNEL RECEIVED THEM — a test-only seam, because an m-vector is not
	/// recoverable from a filtered buffer and "the mix reached the kernel" is exactly the kind of
	/// claim CLAUDE.md says must be read rather than inferred.
	[[nodiscard]] std::array<float, 3> mixVectorForTest() const {
		return { laneOf(svf_.m0), laneOf(svf_.m1), laneOf(svf_.m2) };
	}

	/// The resonance coefficient the kernel computed, so a case can check the derivation used THIS k rather
	/// than one of its own.
	[[nodiscard]] float kForTest() const { return laneOf(svf_.k); }

private:
	static float laneOf(__m128 v) {
		float out alignas(16)[4];
		_mm_store_ps(out, v);
		return out[0];
	}

	float clampCutoff(float hz) const {
		const float top = static_cast<float>(sampleRate_) * kFilterNyquistFraction;
		if (!std::isfinite(hz)) {
			return kFilterMinHz;
		}
		return std::clamp(hz, kFilterMinHz, top);
	}

	/// CONSTANT POWER — the squares sum to one, NOT the weights.
	///
	/// **IT WAS SUM-TO-ONE AND THAT SILENTLY UNDID THE DERIVE'S NORMALISATION.** `deriveAutomod`
	/// hands over a vector whose squares already sum to 1, for a stated reason: the three bands are
	/// SUMMED, so equal perceived loudness across the blend needs the squares to sum to one rather
	/// than the weights. Dividing by the sum here then re-scaled it by a different rule — MEASURED as
	/// up to **-3.87 dB** at the type coordinates where the mix is spread across all three bands, and
	/// 0 dB where it happens to sit on one. So the loudness the derive guarantees was being thrown
	/// away exactly where it was doing work.
	///
	/// TWO PLACES THAT MUST AGREE ABOUT ONE FACT, and they did not. Both are constant power now, and
	/// the rule is stated in both — a component that normalises differently from its only caller is a
	/// component whose contract is decided by whoever writes the caller.
	static FilterMix normalise(const FilterMix& in) {
		const float l = std::isfinite(in.low) ? in.low : 0.0f;
		const float b = std::isfinite(in.band) ? in.band : 0.0f;
		const float h = std::isfinite(in.high) ? in.high : 0.0f;
		const float sumSquares = l * l + b * b + h * h;
		// A MIX THAT SUMS TO NOTHING IS SILENCE, NOT A DIVISION BY ZERO — and returning a lowpass
		// would be inventing a sound the caller did not ask for.
		if (!(sumSquares > 1e-18f)) {
			return { 0.0f, 0.0f, 0.0f };
		}
		const float invRms = 1.0f / std::sqrt(sumSquares);
		return { l * invRms, b * invRms, h * invRms };
	}

	/// COEFFICIENTS FIRST, THEN THE MIX OVER THE TOP. `setCoeff` establishes g, k, a1, a2, a3 — and
	/// writes an m-vector we then replace, so which Mode it is handed only matters if the Mode
	/// changes something other than the mix.
	///
	/// AN EARLIER DRAFT SAID BELL AND THE SHELVES "ALTER k ITSELF" AND WOULD THEREFORE BE WRONG HERE.
	/// Half right. `setCoeff` does divide k by `bellShelfAmp` for Bell — but `bellShelfAmp` DEFAULTS
	/// TO 1.0 and we do not pass one, so the division is by one and k is unchanged. MEASURED: a
	/// known-bad switching this call to `Mode::Bell` is GREEN across the whole suite.
	///
	/// Lowpass stays because it is the honest label for "establish the coefficients and nothing
	/// else", and because the moment anybody passes a `bellShelfAmp` the earlier draft's warning
	/// becomes true.
	void apply() {
		svf_.setCoeff(feathers::fx::filter::CytomicSVF::Mode::Lowpass, cutoffHz_, cutoffRightHz_, resonance_, resonance_,
		              srInv_, 1.0f, 1.0f);

		const float k = laneOf(svf_.k);
		const float m0 = mix_.high;
		const float m1 = mix_.band - mix_.high * k;
		const float m2 = mix_.low - mix_.high;

		svf_.m0 = _mm_set1_ps(m0);
		svf_.m1 = _mm_set1_ps(m1);
		svf_.m2 = _mm_set1_ps(m2);
	}

	feathers::fx::filter::CytomicSVF svf_;
	double sampleRate_ = 48000.0;
	float srInv_ = 1.0f / 48000.0f;
	float cutoffHz_ = 1000.0f;
	float cutoffRightHz_ = 1000.0f;
	float resonance_ = 0.0f;
	FilterMix mix_ {};
};

} // namespace feathers::fx::automod
