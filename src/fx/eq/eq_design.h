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

#pragma once

// THE EQ'S DESIGN LIBRARY (step 1). Pure functions and two tiny filter
// states; no device, no params, no allocation. Everything that turns a band's settings into numbers
// lives here, so the device (step 2), the curve (step 3) and the linear-phase kernel (step 7) all read
// ONE magnitude function (R5: the Phase switch changes phase only).
//
// THE DESIGNS, and why each is what it is (the doc's §2.2 has the measurements):
//   - Bell, Low Shelf, High Shelf: MATCHED (Vicanek 2016), not bilinear. Poles by impulse invariance,
//     numerator by matching |H|^2 at DC, Nyquist and f0. Bilinear RBJ is 4.5-7.9 dB off at 12-16 kHz;
//     matched is 0.3-1.3 dB off at the same run-time cost. No oversampling.
//   - THE RECIPROCAL RULE: the naive matched shelf is UNSOLVABLE near Nyquist for a high-shelf boost
//     and a low-shelf cut (48-73 dB errors once clamped). The prototypes satisfy H(-g) = 1/H(+g)
//     exactly, so the member of {H, 1/H} that FALLS toward Nyquist is designed and the other is its
//     inverse. The matched numerator is minimum phase, so the inverse is stable.
//   - Low Cut, High Cut, Notch: BILINEAR (RBJ-equivalent). A cut's bilinear "error" near Nyquist is
//     extra attenuation, harmless; a notch keeps its exact null at f0.
//   - Tilt: seven MATCHED first-order sections, one every two octaves from 5 Hz. Bilinear sections
//     were 9-13 dB off a straight line at +6 dB/oct near 20 kHz; matched ones 1.3.
//
// Every section is realised in a TOPOLOGY-PRESERVING structure (Simper's trapezoidal SVF for second
// order, a TPT one-pole for first order), which is what makes grid-rate coefficient changes safe under
// modulation. `svfFromBiquad` maps ANY stable digital biquad into the SVF exactly — the inverse of the
// bilinear transform onto y = m0 x + m1 v1 + m2 v2.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace feathers::fx::eq {

inline constexpr double kPi = std::numbers::pi_v<double>;

enum class BandType : int { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch, kCount };

/// Cut slopes, in dB per octave. A Butterworth of order n falls 6n dB/oct, so order = slope / 6.
inline constexpr int kSlopeDbPerOctave[] = {6, 12, 18, 24, 36, 48, 72, 96};
inline constexpr int kSlopeCount = static_cast<int>(std::size(kSlopeDbPerOctave));

/// 96 dB/oct is order 16: eight biquads. Every other type needs one section.
inline constexpr int kMaxSectionsPerBand = 8;

inline constexpr double kButterworthQ = 0.70710678118654752;

/// THE SHELF Q RANGE is the range the matched design was measured over (the doc's §2.2): all 5,580
/// shelves there were solvable with a worst error of 1.18 dB. A shelf's Q is clamped into it.
inline constexpr double kShelfMinQ = 0.3;
inline constexpr double kShelfMaxQ = 1.5;

/// THE FLOOR: below it nothing is audible. A plain clamp for every type (R7: it was named no fade at the
/// bottom).
inline constexpr double kFloorHz = 10.0;

/// THE DESIGN LIMIT, 0.45 fs: the highest frequency a band is DESIGNED at. There is no 20 kHz ceiling
/// (R7) — below this a band follows Offset freely, at 96 kHz up to 43.2 kHz. The limit is the
/// realisability margin a matched design needs near Nyquist (an impulse-invariant pole past it aliases),
/// and 0.45 fs is where the design was measured (the grid in the doc's §2.2 and in test_eq_design).
inline constexpr double kDesignLimitFractionOfRate = 0.45;

/// THE NYQUIST FADE (R7, when I say fade, I mean 'when it goes over nyquist'). A fading band's
/// gain falls linearly in octaves from full at the design limit to nothing AT Nyquist — the stretch the
/// design can no longer follow, 0.152 octave (1.8 semitones) — and is bypassed from Nyquist up.
/// ASSUMPTION A1 (provisional, pending the ear): the window is [0.45 fs, 0.5 fs].
[[nodiscard]] inline double designLimitHz(double sampleRate) noexcept {
	return kDesignLimitFractionOfRate * sampleRate;
}

// ---------------------------------------------------------------------------------------------------
// Coefficients
// ---------------------------------------------------------------------------------------------------

/// A digital biquad with a0 == 1. A FIRST-ORDER section is one with b2 == a2 == 0 and `firstOrder` set.
struct Biquad {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct Section {
	Biquad z;
	bool firstOrder = false;
};

/// One band's realisation: up to eight sections in series. `bypass` means the band is exactly identity
/// and must not be run at all — which is how a 0 dB bell stays BIT-exact rather than 1e-14 away (a
/// matched 0 dB bell maps to m1 = 1.35e-14, measured).
struct BandDesign {
	std::array<Section, kMaxSectionsPerBand> sections {};
	int count = 0;
	bool bypass = true;
	/// False only if a matched numerator could not be solved and was clamped. The reciprocal rule makes
	/// this unreachable over the measured ranges; the flag is what lets a test prove it.
	bool solved = true;
};

[[nodiscard]] inline Biquad inverse(const Biquad& q) noexcept {
	return {1.0 / q.b0, q.a1 / q.b0, q.a2 / q.b0, q.b1 / q.b0, q.b2 / q.b0};
}

/// |H(e^jw)|^2 of one section, w in radians per sample.
[[nodiscard]] inline double magnitudeSquared(const Biquad& q, double w) noexcept {
	const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
	const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = q.b1 * s1 + q.b2 * s2;
	const double dr = 1.0 + q.a1 * c1 + q.a2 * c2, di = q.a1 * s1 + q.a2 * s2;
	return (nr * nr + ni * ni) / (dr * dr + di * di);
}

// ---------------------------------------------------------------------------------------------------
// Analog prototypes (RBJ's, normalised so the corner is s = j1) and the matched design
// ---------------------------------------------------------------------------------------------------

/// H(s) = (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0).
struct AnalogBiquad {
	double b2, b1, b0, a2, a1, a0;
};

/// The Bell and shelf prototypes. `A = 10^(dB/40)`. Substituting A -> 1/A turns the numerator into
/// D/A^2 and the denominator into N/A^2, which is the whole of the reciprocal rule.
[[nodiscard]] inline AnalogBiquad analogPrototype(BandType type, double gainDb, double q) noexcept {
	const double a = std::pow(10.0, gainDb / 40.0), sa = std::sqrt(a);
	switch (type) {
	case BandType::LowShelf: return {a, a * sa / q, a * a, a, sa / q, 1.0};
	case BandType::HighShelf: return {a * a, a * sa / q, a, 1.0, sa / q, a};
	default: return {1.0, a / q, 1.0, 1.0, 1.0 / (a * q), 1.0};
	}
}

/// |H(jW)|^2 with W = f / f0.
[[nodiscard]] inline double analogMagnitudeSquared(const AnalogBiquad& p, double w) noexcept {
	const double w2 = w * w;
	const double nr = p.b0 - p.b2 * w2, ni = p.b1 * w, dr = p.a0 - p.a2 * w2, di = p.a1 * w;
	return (nr * nr + ni * ni) / (dr * dr + di * di);
}

struct Matched {
	Biquad z;
	bool solved = true;
};

/// VICANEK'S MATCHED SECOND-ORDER DESIGN, restated for any analog biquad whose poles are in the left
/// half-plane. `f0` is the frequency the prototype's s = j1 maps to.
[[nodiscard]] inline Matched matchedFromAnalog(const AnalogBiquad& p, double f0, double sampleRate) noexcept {
	const double w0 = 2.0 * kPi * f0 / sampleRate;
	// Poles of a2 s^2 + a1 s + a0, scaled by w0 into radians per sample, mapped by z = exp(s).
	double a1 = 0.0, a2 = 0.0;
	const double disc = p.a1 * p.a1 - 4.0 * p.a2 * p.a0;
	const double re = -p.a1 / (2.0 * p.a2) * w0;
	if (disc < 0.0) {
		const double im = std::sqrt(-disc) / (2.0 * p.a2) * w0;
		a1 = -2.0 * std::exp(re) * std::cos(im);
		a2 = std::exp(2.0 * re);
	}
	else {
		const double half = std::sqrt(disc) / (2.0 * p.a2) * w0;
		a1 = -(std::exp(re + half) + std::exp(re - half));
		a2 = std::exp(2.0 * re);
	}
	// |A(e^jw)|^2 = A0 phi0 + A1 phi1 + A2 phi2, with phi1 = sin^2(w/2), phi0 = 1 - phi1, phi2 = 4 phi0 phi1.
	const double bigA0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
	const double bigA1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
	const double bigA2 = -4.0 * a2;
	const double bigB0 = bigA0 * analogMagnitudeSquared(p, 0.0);
	const double bigB1 = bigA1 * analogMagnitudeSquared(p, 0.5 * sampleRate / f0);
	const double s = std::sin(0.5 * w0);
	const double phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
	const double bigB2 =
	    (analogMagnitudeSquared(p, 1.0) * (bigA0 * phi0 + bigA1 * phi1 + bigA2 * phi2) - bigB0 * phi0 - bigB1 * phi1)
	    / phi2;
	const double r0 = std::sqrt(bigB0), r1 = std::sqrt(bigB1), w = 0.5 * (r0 + r1);
	const double d = w * w + bigB2;
	Matched m;
	m.solved = d >= 0.0;
	// THE LARGER ROOT keeps |b2| <= b0: the zeros stay inside the unit circle, so the section is minimum
	// phase and its inverse (the reciprocal rule) is stable.
	const double b0 = 0.5 * (w + std::sqrt(std::max(d, 0.0)));
	m.z = {b0, 0.5 * (r0 - r1), w - b0, a1, a2};
	return m;
}

// ---------------------------------------------------------------------------------------------------
// Per-type designs
// ---------------------------------------------------------------------------------------------------

namespace detail {

inline void push(BandDesign& d, const Biquad& z, bool firstOrder = false) noexcept {
	d.sections[static_cast<std::size_t>(d.count++)] = {z, firstOrder};
}

/// Bell and shelves through the reciprocal rule: `falls` says whether the requested gain's member of
/// {H, 1/H} is the one that falls toward Nyquist, and so can be designed directly.
inline BandDesign reciprocalMatched(BandType type, double f0, double sampleRate, double gainDb, double q,
                                    bool falls) noexcept {
	BandDesign d;
	if (gainDb == 0.0) {
		return d;
	}
	const Matched m = matchedFromAnalog(analogPrototype(type, falls ? gainDb : -gainDb, q), f0, sampleRate);
	d.bypass = false;
	d.solved = m.solved;
	push(d, falls ? m.z : inverse(m.z));
	return d;
}

/// RBJ's bilinear second-order lowpass / highpass (prewarped at the corner).
inline Biquad bilinearPass(bool high, double f0, double sampleRate, double q) noexcept {
	const double w = 2.0 * kPi * f0 / sampleRate, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	const double b0 = (high ? (1.0 + c) : (1.0 - c)) * 0.5;
	return {b0 / a0, (high ? -(1.0 + c) : (1.0 - c)) / a0, b0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

/// The bilinear first-order lowpass / highpass, g = tan(pi f / fs).
inline Biquad bilinearOnePole(bool high, double f0, double sampleRate) noexcept {
	const double g = std::tan(kPi * f0 / sampleRate);
	const double a1 = (g - 1.0) / (g + 1.0);
	if (high) {
		return {1.0 / (1.0 + g), -1.0 / (1.0 + g), 0.0, a1, 0.0};
	}
	return {g / (1.0 + g), g / (1.0 + g), 0.0, a1, 0.0};
}

} // namespace detail

[[nodiscard]] inline BandDesign designBell(double f0, double sampleRate, double gainDb, double q) noexcept {
	return detail::reciprocalMatched(BandType::Bell, f0, sampleRate, gainDb, q, gainDb > 0.0);
}

[[nodiscard]] inline BandDesign designLowShelf(double f0, double sampleRate, double gainDb, double q) noexcept {
	const double qc = std::clamp(q, kShelfMinQ, kShelfMaxQ);
	return detail::reciprocalMatched(BandType::LowShelf, f0, sampleRate, gainDb, qc, gainDb > 0.0);
}

[[nodiscard]] inline BandDesign designHighShelf(double f0, double sampleRate, double gainDb, double q) noexcept {
	const double qc = std::clamp(q, kShelfMinQ, kShelfMaxQ);
	return detail::reciprocalMatched(BandType::HighShelf, f0, sampleRate, gainDb, qc, gainDb < 0.0);
}

/// THE Q OF EACH BUTTERWORTH SECTION of order n: 1 / (2 sin((2k-1) pi / (2n))), k = 1 .. floor(n/2).
/// k = 1 is the highest. (The cos form gives the same SET for even n and the wrong one for odd n: order 3
/// would get Q 0.577 instead of 1, and a -4.77 dB corner.)
[[nodiscard]] inline double butterworthSectionQ(int order, int k) noexcept {
	return 1.0 / (2.0 * std::sin((2.0 * k - 1.0) * kPi / (2.0 * order)));
}

/// A LOW CUT or HIGH CUT: a Butterworth of the slope's order. Q is resonance — at kButterworthQ the cut
/// is flat Butterworth; above it the HIGHEST-Q section's (k = 1) Q is multiplied by q / kButterworthQ, a
/// peak at the corner. A 6 dB/oct cut is one first-order section and has no Q to raise (so its Q control
/// is disabled, by the honesty rule).
[[nodiscard]] inline BandDesign designCut(bool high, double f0, double sampleRate, int slopeIndex, double q) noexcept {
	BandDesign d;
	d.bypass = false;
	const int order = kSlopeDbPerOctave[std::clamp(slopeIndex, 0, kSlopeCount - 1)] / 6;
	// A LOW CUT is a highpass; a HIGH CUT is a lowpass.
	const bool highpass = !high;
	const int pairs = order / 2;
	for (int k = 1; k <= pairs; ++k) {
		double sq = butterworthSectionQ(order, k);
		if (k == 1) {
			sq *= q / kButterworthQ;
		}
		detail::push(d, detail::bilinearPass(highpass, f0, sampleRate, sq));
	}
	if (order % 2 != 0) {
		detail::push(d, detail::bilinearOnePole(highpass, f0, sampleRate), true);
	}
	return d;
}

/// RBJ's bilinear notch: an exact null at f0.
[[nodiscard]] inline BandDesign designNotch(double f0, double sampleRate, double q) noexcept {
	BandDesign d;
	d.bypass = false;
	const double w = 2.0 * kPi * f0 / sampleRate, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	detail::push(d, {1.0 / a0, -2.0 * c / a0, 1.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0});
	return d;
}

[[nodiscard]] inline double bandMagnitudeSquared(const BandDesign& d, double w) noexcept {
	double m = 1.0;
	if (!d.bypass) {
		for (int i = 0; i < d.count; ++i) {
			m *= magnitudeSquared(d.sections[static_cast<std::size_t>(i)].z, w);
		}
	}
	return m;
}

// ---------------------------------------------------------------------------------------------------
// Settings -> effective band (Offset, Amount, the edge fade)
// ---------------------------------------------------------------------------------------------------

/// One band as the params state it.
struct Band {
	BandType type = BandType::Bell;
	bool on = true;
	double freqHz = 1000.0;
	double gainDb = 0.0;
	double q = 1.0;
	int slopeIndex = 3;
};

/// The device-wide controls that act on every band.
struct Globals {
	double offsetOctaves = 0.0;
	/// Scales every Bell, shelf and the Tilt; -1 mirrors the curve exactly. Cuts and Notch are untouched.
	double amount = 1.0;
	double tiltDbPerOctave = 0.0;
	double tiltPivotHz = 1000.0;
};

/// A band after Offset, Amount and the edge rule: what is actually designed.
struct EffectiveBand {
	BandType type = BandType::Bell;
	bool on = false;
	double freqHz = 1000.0;
	double gainDb = 0.0;
	double q = 1.0;
	int slopeIndex = 3;
};

[[nodiscard]] constexpr bool hasGain(BandType t) noexcept {
	return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf;
}

/// HOW MUCH OF A GAIN BAND SURVIVES at an unclamped frequency `hz`: 1 up to the design limit, falling
/// linearly in octaves to 0 at Nyquist, 0 beyond — for the types that FADE at Nyquist (R7).
///
/// THE ONE PLACE THE FADE RULE LIVES, so a change made by ear is one edit here and one case.
/// ASSUMPTION A2 (provisional, against the literal "fade", pending a listening check): only a Bell and a High
/// Shelf fade. A Low Shelf pushed past Nyquist has become a BROADBAND GAIN — everything audible is below
/// its corner — so fading it would move the whole track's level; it keeps its gain, designed at the
/// limit. Cuts and Notch have no gain and clamp (a Low Cut pushed up IS the filter sweep Offset exists
/// for; fading it would bring the sound back). The floor never fades (R7): every type clamps there.
[[nodiscard]] inline double nyquistFade(BandType type, double hz, double sampleRate) noexcept {
	if (type != BandType::Bell && type != BandType::HighShelf) {
		return 1.0;
	}
	const double limit = designLimitHz(sampleRate), nyquist = 0.5 * sampleRate;
	if (hz <= limit) {
		return 1.0;
	}
	return std::clamp(1.0 - std::log2(hz / limit) / std::log2(nyquist / limit), 0.0, 1.0);
}

[[nodiscard]] inline EffectiveBand resolve(const Band& b, const Globals& g, double sampleRate) noexcept {
	EffectiveBand e;
	e.type = b.type;
	e.on = b.on;
	e.q = b.q;
	e.slopeIndex = b.slopeIndex;
	const double hz = b.freqHz * std::exp2(g.offsetOctaves);
	e.freqHz = std::clamp(hz, kFloorHz, designLimitHz(sampleRate));
	e.gainDb = hasGain(b.type) ? b.gainDb * g.amount * nyquistFade(b.type, hz, sampleRate) : 0.0;
	return e;
}

[[nodiscard]] inline BandDesign design(const EffectiveBand& e, double sampleRate) noexcept {
	if (!e.on) {
		return {};
	}
	switch (e.type) {
	case BandType::Bell: return designBell(e.freqHz, sampleRate, e.gainDb, e.q);
	case BandType::LowShelf: return designLowShelf(e.freqHz, sampleRate, e.gainDb, e.q);
	case BandType::HighShelf: return designHighShelf(e.freqHz, sampleRate, e.gainDb, e.q);
	case BandType::LowCut: return designCut(false, e.freqHz, sampleRate, e.slopeIndex, e.q);
	case BandType::HighCut: return designCut(true, e.freqHz, sampleRate, e.slopeIndex, e.q);
	case BandType::Notch: return designNotch(e.freqHz, sampleRate, e.q);
	default: return {};
	}
}

// ---------------------------------------------------------------------------------------------------
// Tilt
// ---------------------------------------------------------------------------------------------------

inline constexpr int kTiltSections = 7;
inline constexpr double kTiltLowestHz = 5.0;
inline constexpr double kTiltSpacingOctaves = 2.0;

/// A constant dB/oct slope from seven MATCHED first-order sections, normalised to unity at the pivot.
struct TiltDesign {
	std::array<Biquad, kTiltSections> sections {};
	/// The linear gain that puts the pivot at exactly 0 dB.
	double gain = 1.0;
	bool bypass = true;
};

/// Each section is a pole/zero pair two octaves wide in placement and `alpha` octaves apart, which
/// tilts by 6.02 dB/oct across its own span; seven of them tile 5 Hz .. 40 kHz. The pole is impulse
/// invariant; the numerator matches the analog |H| at DC (unity) and at Nyquist.
///
/// A NEGATIVE SLOPE IS THE EXACT INVERSE OF THE POSITIVE ONE — the reciprocal rule again, so that Amount
/// -100% undoes +100% to rounding (a test holds it to 1e-9 dB). Designing a negative slope directly (zeros
/// above poles) is just as accurate but is NOT the inverse: it left 0.15 dB between +a and -a, measured.
/// Each matched section is minimum phase (sum and diff are both positive, so |b1| < b0), so each
/// inverse is stable.
[[nodiscard]] inline TiltDesign designTilt(double slopeDbPerOctave, double pivotHz, double sampleRate) noexcept {
	TiltDesign t;
	if (slopeDbPerOctave == 0.0) {
		return t;
	}
	t.bypass = false;
	const double alpha = std::fabs(slopeDbPerOctave) / (20.0 * std::log10(2.0)) * kTiltSpacingOctaves;
	const double nyquist = 0.5 * sampleRate;
	for (int i = 0; i < kTiltSections; ++i) {
		const double pole = kTiltLowestHz * std::exp2(kTiltSpacingOctaves * (i + 0.5));
		const double zero = pole * std::exp2(-alpha);
		const double a1 = -std::exp(-2.0 * kPi * pole / sampleRate);
		const double atNyquist =
		    std::sqrt((1.0 + (nyquist / zero) * (nyquist / zero)) / (1.0 + (nyquist / pole) * (nyquist / pole)));
		const double sum = 1.0 + a1, diff = (1.0 - a1) * atNyquist;
		const Biquad rising {0.5 * (sum + diff), 0.5 * (sum - diff), 0.0, a1, 0.0};
		t.sections[static_cast<std::size_t>(i)] = slopeDbPerOctave > 0.0 ? rising : inverse(rising);
	}
	const double w = 2.0 * kPi * pivotHz / sampleRate;
	double m = 1.0;
	for (const Biquad& s : t.sections) {
		m *= magnitudeSquared(s, w);
	}
	t.gain = 1.0 / std::sqrt(m);
	return t;
}

[[nodiscard]] inline TiltDesign designTilt(const Globals& g, double sampleRate) noexcept {
	return designTilt(g.tiltDbPerOctave * g.amount, g.tiltPivotHz, sampleRate);
}

[[nodiscard]] inline double tiltMagnitudeSquared(const TiltDesign& t, double w) noexcept {
	if (t.bypass) {
		return 1.0;
	}
	double m = t.gain * t.gain;
	for (const Biquad& s : t.sections) {
		m *= magnitudeSquared(s, w);
	}
	return m;
}

// ---------------------------------------------------------------------------------------------------
// THE MAGNITUDE FUNCTION — one, read by the min-phase path's tests, the curve, the crosshair and the FIR
// ---------------------------------------------------------------------------------------------------

[[nodiscard]] inline double responseDb(std::span<const BandDesign> bands, const TiltDesign& tilt, double hz,
                                       double sampleRate) noexcept {
	const double w = 2.0 * kPi * hz / sampleRate;
	double m = tiltMagnitudeSquared(tilt, w);
	for (const BandDesign& d : bands) {
		m *= bandMagnitudeSquared(d, w);
	}
	return 10.0 * std::log10(m);
}

// ---------------------------------------------------------------------------------------------------
// Realisation: Simper SVF (second order) and TPT one-pole (first order)
// ---------------------------------------------------------------------------------------------------

struct SvfCoefs {
	float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
	float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;
};

/// ANY STABLE DIGITAL BIQUAD INTO THE SVF, exactly. With P = 1 + a1 + a2 and M = 1 - a1 + a2 (both
/// positive for stable poles), the inverse bilinear transform gives
///   g = sqrt(P/M),  k = g (2 - 2 a2) / P,
///   m0 = (b0 - b1 + b2) / M,  m1 = g (2 b0 - 2 b2) / P - m0 k,  m2 = (b0 + b1 + b2) / P - m0.
[[nodiscard]] inline SvfCoefs svfFromBiquad(const Biquad& q) noexcept {
	const double p = 1.0 + q.a1 + q.a2, m = 1.0 - q.a1 + q.a2;
	const double g = std::sqrt(p / m), k = g * (2.0 - 2.0 * q.a2) / p;
	const double m0 = (q.b0 - q.b1 + q.b2) / m;
	const double m1 = g * (2.0 * q.b0 - 2.0 * q.b2) / p - m0 * k;
	const double m2 = (q.b0 + q.b1 + q.b2) / p - m0;
	const double a1 = 1.0 / (1.0 + g * (g + k));
	SvfCoefs c;
	c.a1 = static_cast<float>(a1);
	c.a2 = static_cast<float>(g * a1);
	c.a3 = static_cast<float>(g * g * a1);
	c.m0 = static_cast<float>(m0);
	c.m1 = static_cast<float>(m1);
	c.m2 = static_cast<float>(m2);
	return c;
}

/// Simper's trapezoidal SVF (SvfLinearTrapOptimised2), one channel.
struct SvfState {
	float ic1 = 0.0f, ic2 = 0.0f;

	[[nodiscard]] float step(const SvfCoefs& c, float x) noexcept {
		const float v3 = x - ic2;
		const float v1 = c.a1 * ic1 + c.a2 * v3;
		const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
		ic1 = 2.0f * v1 - ic1;
		ic2 = 2.0f * v2 - ic2;
		return c.m0 * x + c.m1 * v1 + c.m2 * v2;
	}
	void clear() noexcept { ic1 = ic2 = 0.0f; }
};

struct OnePoleCoefs {
	float big = 0.0f;
	float m0 = 1.0f, m1 = 0.0f;
};

/// ANY STABLE FIRST-ORDER SECTION (b0 + b1 z^-1) / (1 + a1 z^-1) INTO A TPT ONE-POLE, y = m0 x + m1 lp:
///   g = (1 + a1) / (1 - a1),  m0 = (b0 - b1) / (1 - a1),  m1 = (b0 - m0) (1 + g) / g.
[[nodiscard]] inline OnePoleCoefs onePoleFromBiquad(const Biquad& q) noexcept {
	const double g = (1.0 + q.a1) / (1.0 - q.a1);
	const double m0 = (q.b0 - q.b1) / (1.0 - q.a1);
	OnePoleCoefs c;
	c.big = static_cast<float>(g / (1.0 + g));
	c.m0 = static_cast<float>(m0);
	c.m1 = static_cast<float>((q.b0 - m0) * (1.0 + g) / g);
	return c;
}

struct OnePoleState {
	float s = 0.0f;

	[[nodiscard]] float step(const OnePoleCoefs& c, float x) noexcept {
		const float v = (x - s) * c.big;
		const float lp = v + s;
		s = lp + v;
		return c.m0 * x + c.m1 * lp;
	}
	void clear() noexcept { s = 0.0f; }
};

} // namespace feathers::fx::eq
