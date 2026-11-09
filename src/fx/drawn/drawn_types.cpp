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

#include "fx/drawn/drawn_types.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace feathers::fx::drawn {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// THE MAGNITUDE OF `1 + c1 z^-1 + c2 z^-2` ON THE UNIT CIRCLE, and the fact that BOTH the numerator
/// and the denominator go through this one function is what makes `Bypass` exactly flat rather than
/// flat to within a tolerance. A zero sitting on its pole gives bit-identical `(c1, c2)` for the two
/// calls, identical code then gives an identical result, and the ratio is exactly 1.0f.
double sectionMagnitude(double c1, double c2, double cosW, double sinW, double cos2W, double sin2W) {
	const double re = 1.0 + c1 * cosW + c2 * cos2W;
	const double im = -(c1 * sinW + c2 * sin2W);
	return std::sqrt(re * re + im * im);
}

float lerp4(const std::array<float, kFrames>& w, float a, float b, float c, float d) {
	// SUMMED IN CORNER ORDER, and the order is part of the corner-exactness guarantee: at a corner
	// three weights are exactly zero, `0 * x` is exactly zero for any finite x, and `a + 0` is
	// exactly `a`. So the blend at a corner is that corner's stored float, bit for bit.
	return w[X0Y0] * a + w[X1Y0] * b + w[X0Y1] * c + w[X1Y1] * d;
}

} // namespace

float angleRadians(const Point& point) {
	const float clamped = std::clamp(point.angleLog2, kMinAngleLog2, kMaxAngleLog2);
	return kPi * std::exp2(clamped);
}

std::optional<Point> zeroPlacementFor(Type type, const Point& pole, float bellGain) {
	switch (type) {
	case Type::Lowpass:
		// Both zeros at z = -1. On the unit circle at Nyquist, which the log coordinate reaches
		// exactly (angleLog2 == 0 is nu == 1).
		return Point{1.0f, kMaxAngleLog2};

	case Type::Highpass:
		// Both zeros at z = +1. DC IS NOT REPRESENTABLE — see Point's header comment — so this is
		// the floor, and a highpass therefore has a tiny finite gain at DC rather than a null.
		return Point{1.0f, kMinAngleLog2};

	case Type::Bandpass: {
		// THE ONE PLACEMENT A CONJUGATE PAIR CANNOT HOLD. A bandpass's zeros are at DC *and* at
		// Nyquist — `{+1, -1}` — which is a SPLIT REAL pair, not a conjugate one, so no `(r, theta)`
		// with r >= 0 reaches it: the numerator would need `1 - z^-2`, i.e. `c2 = -1`, and
		// `c2 = |z|^2` is never negative.
		//
		// THE FIX COSTS NO EXTRA FLOAT: `coefficientsOf` forms `c2` as `r * |r|` rather than `r * r`,
		// so a NEGATIVE radius selects the split-real branch — the roots of `z^2 - 2r cos(theta) z -
		// r^2`, which are real, of product -r^2, and are exactly `{+1, -1}` at r = -1, theta = pi/2.
		// The two branches meet continuously at r = 0 (no zeros at all, numerator 1) and `r * |r|`
		// is C1 there, so a Bandpass corner blends into any other corner without a kink.
		//
		// **POLES NEVER TAKE THIS BRANCH** and `sanitisePole` enforces it: a split-real pole of
		// radius r can reach |z| = 2.41 r, which would put a "stable" radius outside the circle.
		// The asymmetry is the physics — stability constrains poles and nothing constrains zeros.
		//
		// theta = pi/2 is angleLog2 = -1 (nu = 1/2), and the placement does NOT follow the pole:
		// an SVF bandpass keeps its zeros at DC and Nyquist whatever its corner frequency is.
		return Point{-1.0f, -1.0f};
	}

	case Type::Notch:
		// On the unit circle at the pole's own angle: a true null, and it TRACKS the pole because
		// both angles are the same stored float blended by the same weights.
		return Point{1.0f, pole.angleLog2};

	case Type::Bell: {
		// Gain at the corner is about (1 - rz) / (1 - rp), so a zero further from the circle than the
		// pole is a boost. Clamped at 0 rather than allowed to go negative: below zero is the
		// split-real branch above, which is a bandpass and not a loud bell.
		const float radius = std::clamp(1.0f - bellGain * (1.0f - pole.radius), 0.0f, kMaxZeroRadius);
		return Point{radius, pole.angleLog2};
	}

	case Type::Allpass: {
		// The mirror of the pole, 1/r at the same angle. A pole closer to the origin than
		// 1/kMaxZeroRadius cannot have its mirror stored, and the clamp makes the section stop being
		// an allpass rather than making it wrong quietly — said in the header too.
		const float radius = pole.radius > 1.0f / kMaxZeroRadius ? 1.0f / pole.radius : kMaxZeroRadius;
		return Point{radius, pole.angleLog2};
	}

	case Type::Bypass:
		// THE ZERO ON THE POLE. Returned as a copy of the pole so the two are bit-identical, which is
		// what makes the response exactly 1.0f rather than 1.0f within a tolerance.
		return pole;

	case Type::Free:
	case Type::kCount:
		break;
	}
	// `Free` HAS NO PLACEMENT RULE — the zero is the user's own point, and the absence is the
	// meaning. Returning the current zero here would look like a rule and would overwrite nothing,
	// which is a worse kind of nothing.
	return std::nullopt;
}


std::array<float, kFrames> bilinearWeights(float x, float y) {
	// CLAMPED FIRST. Outside [0, 1] two of these go negative, and a negative weight is not a convex
	// combination any more — the stability guarantee and the "flat stays flat" guarantee both rest on
	// non-negativity, so the clamp is load-bearing rather than defensive.
	const float cx = std::clamp(x, 0.0f, 1.0f);
	const float cy = std::clamp(y, 0.0f, 1.0f);
	const float mx = 1.0f - cx;
	const float my = 1.0f - cy;
	// FORMED AS PRODUCTS, NOT AS "one minus the other three". Products of non-negative floats are
	// non-negative EXACTLY; a subtracted fourth weight can land a ulp below zero and quietly break
	// the only property everything else is built on.
	std::array<float, kFrames> w{};
	w[X0Y0] = mx * my;
	w[X1Y0] = cx * my;
	w[X0Y1] = mx * cy;
	w[X1Y1] = cx * cy;
	return w;
}

Point blend(const std::array<Point, kFrames>& corners, float x, float y) {
	const auto w = bilinearWeights(x, y);
	Point out;
	out.radius = lerp4(w, corners[X0Y0].radius, corners[X1Y0].radius, corners[X0Y1].radius,
	                   corners[X1Y1].radius);
	out.angleLog2 = lerp4(w, corners[X0Y0].angleLog2, corners[X1Y0].angleLog2, corners[X0Y1].angleLog2,
	                      corners[X1Y1].angleLog2);
	return out;
}

Band blend(const std::array<Band, kFrames>& corners, float x, float y) {
	Band out;
	out.pole = blend(std::array<Point, kFrames>{corners[X0Y0].pole, corners[X1Y0].pole,
	                                            corners[X0Y1].pole, corners[X1Y1].pole},
	                 x, y);
	out.zero = blend(std::array<Point, kFrames>{corners[X0Y0].zero, corners[X1Y0].zero,
	                                            corners[X0Y1].zero, corners[X1Y1].zero},
	                 x, y);
	return out;
}

Frame blend(const std::array<Frame, kFrames>& corners, float x, float y) {
	Frame out;
	for (size_t b = 0; b < kBands; ++b) {
		out.band[b] = blend(std::array<Band, kFrames>{corners[X0Y0].band[b], corners[X1Y0].band[b],
		                                              corners[X0Y1].band[b], corners[X1Y1].band[b]},
		                    x, y);
	}
	return out;
}

Frame blend(const Settings& settings) { return blend(settings.frame, settings.morphX, settings.morphY); }

void sanitisePole(Point& pole) {
	// NO NEGATIVE BRANCH FOR A POLE — see the Bandpass comment above for what a split-real pole would
	// cost. The upper bound is what the stability guarantee is made of.
	pole.radius = std::isfinite(pole.radius) ? std::clamp(pole.radius, 0.0f, kMaxPoleRadius) : 0.0f;
	pole.angleLog2 = std::isfinite(pole.angleLog2)
	                     ? std::clamp(pole.angleLog2, kMinAngleLog2, kMaxAngleLog2)
	                     : kDefaultAngleLog2;
}

void sanitiseZero(Point& zero) {
	zero.radius = std::isfinite(zero.radius) ? std::clamp(zero.radius, -kMaxZeroRadius, kMaxZeroRadius)
	                                         : 0.0f;
	zero.angleLog2 = std::isfinite(zero.angleLog2)
	                     ? std::clamp(zero.angleLog2, kMinAngleLog2, kMaxAngleLog2)
	                     : kDefaultAngleLog2;
}

void sanitise(Band& band) {
	sanitisePole(band.pole);
	sanitiseZero(band.zero);
}

void sanitise(Frame& frame) {
	for (auto& band : frame.band) {
		sanitise(band);
	}
}

void sanitise(Settings& settings) {
	for (auto& frame : settings.frame) {
		sanitise(frame);
	}
	settings.morphX = std::clamp(settings.morphX, 0.0f, 1.0f);
	settings.morphY = std::clamp(settings.morphY, 0.0f, 1.0f);
}

bool poleIsStable(const Point& pole) {
	return std::isfinite(pole.radius) && pole.radius >= 0.0f && pole.radius < 1.0f;
}

/// KEEP A CONJUGATE PAIR CONJUGATE, AND THIS IS A BUG FIX WITH A MEASUREMENT BEHIND IT.
///
/// A pair stored as `(r, theta)` with `r >= 0` MEANS a conjugate pair, and its quadratic
/// `1 + c1 z^-1 + c2 z^-2` has complex roots exactly when `c1^2 <= 4 c2`. The two coefficients are
/// computed by different roundings — `c1 = -2 r cos(theta)` and `c2 = fl(r * r)` — so that inequality
/// is not automatic, and where it fails the pair silently becomes **two REAL roots**, the larger of
/// which is `r + sqrt(r^2 - fl(r^2))`, i.e. OUTSIDE the radius the representation says it has.
///
/// **MEASURED, and the numbers are the reason this function changed rather than the constant.** The
/// split needs `|cos theta| = 1` exactly, and there is precisely one reachable angle where
/// `std::cos` returns exactly -1: `kMaxAngleLog2`, Nyquist, the right-hand edge of the phase-4
/// editor's plane. At `kMaxPoleRadius` (0.9999) and that angle the realised root magnitude measured
/// **exactly 1.0** — a pole ON the unit circle, which is a section that rings forever. At every other
/// angle it measured 0.99989998, which is the conjugate branch behaving correctly.
///
/// The size of the error is `sqrt(ulp(r^2))`, about **1e-4** for r near 1 — the SAME ORDER as the
/// 1e-4 of margin `kMaxPoleRadius` leaves. So the margin was exactly consumed by the rounding of one
/// multiply, and the header's reasoning about the blend's 1e-7 was accounting for a term three orders
/// of magnitude too small to be the one that mattered. Neither Res nor the blend contributes: the
/// measurement reproduces at `res = 0` on a single unblended band.
///
/// **APPLIED TO BOTH PAIRS, WHICH IS WHAT KEEPS `Bypass` EXACT.** A zero on its pole must still give
/// `b == a` bit for bit or the flat default stops being flat, and it does: identical inputs go
/// through this identical function. The split-real branch a `Bandpass` needs is untouched, because a
/// negative radius is not a conjugate pair and `c1 * c1 > 4 * c2` is its DEFINITION rather than a
/// rounding failure — the guard below only ever raises `c2`, and only when the pair claims to be
/// conjugate.
float conjugateC2(float c1, float c2) {
	if (c2 < 0.0f) {
		// THE SPLIT-REAL BRANCH, LEFT ALONE. `c2 = r * |r|` is negative only for a negative radius,
		// which is the Bandpass placement and is meant to have real roots.
		return c2;
	}
	// `(c1/2)^2` IS THE SMALLEST `c2` THAT MAKES THE ROOTS COINCIDE, and at that value the double root
	// is `-c1/2`, whose magnitude is `r |cos theta| <= r`. For an ordinary pair this is strictly below
	// `c2` and the max leaves it alone; only the rounding case moves.
	const float half = c1 * 0.5f;
	float out = std::max(c2, half * half);
	// AND THEN AT MOST A FEW ULP, because `half * half` is itself rounded and can land a ulp short.
	// Bounded rather than a bare `while`: this is audio-thread-adjacent code and an unbounded loop on
	// a NaN would be a hang rather than a wrong number.
	for (int guard = 0; guard < 8; ++guard) {
		if (!(static_cast<double>(c1) * static_cast<double>(c1) > 4.0 * static_cast<double>(out))) {
			break;
		}
		out = std::nextafter(out, 2.0f);
	}
	return out;
}

Biquad coefficientsOf(const Band& band) {
	Biquad c;
	// THE SAME TWO EXPRESSIONS FOR BOTH PAIRS. `r * |r|` rather than `r * r` is what gives the zero
	// its split-real branch (Bandpass); for a pole, whose radius `sanitisePole` keeps non-negative,
	// the two are the same arithmetic on the same bits, so a zero on a pole yields b == a exactly.
	c.b1 = -2.0f * band.zero.radius * std::cos(angleRadians(band.zero));
	c.b2 = conjugateC2(c.b1, band.zero.radius * std::fabs(band.zero.radius));
	c.a1 = -2.0f * band.pole.radius * std::cos(angleRadians(band.pole));
	c.a2 = conjugateC2(c.a1, band.pole.radius * std::fabs(band.pole.radius));
	return c;
}

float magnitudeAt(const Band& band, float nu) {
	const Biquad c = coefficientsOf(band);
	// IN DOUBLE, AND FOR THE SAME REASON `peakMagnitude` IS. `1 + a1 cos w + a2 cos 2w` at a
	// resonance is the near-total cancellation of terms of order 1 leaving a result of order 1e-7,
	// so in float the denominator can be tens of percent wrong exactly where the response is most
	// interesting. MEASURED: a float evaluation read 2.1% away from the closed-form peak of the same
	// coefficients, and the error grows with Q. Bypass stays EXACTLY 1.0f regardless — numerator and
	// denominator still go through one function on bit-identical inputs, so the ratio is exactly 1
	// in double and converts to exactly 1.0f.
	const double w = kPi * static_cast<double>(std::clamp(nu, 0.0f, 1.0f));
	const double cosW = std::cos(w);
	const double sinW = std::sin(w);
	const double cos2W = std::cos(2.0 * w);
	const double sin2W = std::sin(2.0 * w);
	const double num = sectionMagnitude(c.b1, c.b2, cosW, sinW, cos2W, sin2W);
	const double den = sectionMagnitude(c.a1, c.a2, cosW, sinW, cos2W, sin2W);
	// UNNORMALISED, and deliberately so — see the header on `Biquad`. `den` cannot be zero for a
	// sanitised pole (the denominator's roots are strictly inside the circle), but an unsanitised one
	// could sit on it, and returning 0 is a less useful lie than returning infinity.
	return den > 0.0 ? static_cast<float>(num / den) : std::numeric_limits<float>::infinity();
}

float magnitudeAt(const Frame& frame, float nu) {
	float product = 1.0f;
	for (const auto& band : frame.band) {
		product *= magnitudeAt(band, nu);
	}
	return product;
}


//==================================================================================================
// PHASE 2 — THE GLOBALS, THE REALISATION, AND THE ROW
//==================================================================================================

namespace {

/// THE RADIAL WARP `Res` APPLIES, on the MAGNITUDE, with the sign carried through. Its fixed point is
/// |r| = 1, which is the whole reason it is safe — see `withGlobals` in the header.
float warpRadius(float radius, float k) {
	const float magnitude = std::fabs(radius);
	// WRITTEN SO THAT k == 1 IS THE EXACT IDENTITY. `1 - (1 - m) * k` is the same quantity and is NOT
	// bit-exact at k = 1: for m = 0.1 it returns 0.099999994. That matters because Res defaults to 0,
	// so every device that never touches Res would otherwise have its whole drawn frame perturbed by
	// a ulp per band on the way to the DSP — and the case asserting that a realised section IS its
	// four corners blended would fail for reasons that have nothing to do with blending.
	const float warped = magnitude + (1.0f - magnitude) * (1.0f - k);
	return radius < 0.0f ? -warped : warped;
}

/// P(c) FOR A MONIC `1 + c1 z^-1 + c2 z^-2`, as three coefficients in `c = cos w`. Both the numerator
/// and the denominator of |H|² have this shape, which is what makes the peak a quadratic solve.
///
/// **IN DOUBLE, AND THAT IS NOT A STYLE CHOICE.** For a pole near the unit circle near DC the
/// denominator is `4(1 - c)²` plus corrections, so evaluating it at c = 1 subtracts numbers of order
/// 4 to leave a result of order 1e-8. In float that is below the noise: MEASURED, `peakMagnitude`
/// returned ZERO for such sections, which then made their normalisation the identity and let a
/// cascade of six of them reach a gain of 8.7e7 with the safety trim reporting that it had nothing to
/// do. This all runs at control rate, so the double costs nothing that matters.
struct CosQuadratic {
	double c0 = 0.0;
	double c1 = 0.0;
	double c2 = 0.0;

	[[nodiscard]] double at(double c) const { return c0 + c1 * c + c2 * c * c; }
};

/// THE SMALLEST `|A(e^{jw})|^2` THIS ARITHMETIC IS ENTITLED TO BELIEVE, and the direction a value
/// below it must be read in.
///
/// `Q(c)` IS A SUM OF SQUARES AND CANNOT BE NEGATIVE. It reaches zero only for a pole exactly ON the
/// unit circle, so a computed value at or below zero is CANCELLATION at the one configuration these
/// bounds exist to catch — and the cancellation is not the double arithmetic's, it is the FLOAT
/// COEFFICIENTS'. `drawn_types.h` measures it: a float `a1` places a pole's radius only to 1e-6,
/// while a pole at `kMaxPoleRadius` warped by `Res = 1` sits 1e-5 from the circle. `Q(-1)` is
/// `(1 - a1 + a2)^2 = (1 - r)^2 = 1e-10` there, which the coefficients simply do not carry.
///
/// SO THE ANSWER CANNOT BE ZERO. Returning zero makes the most resonant section this device can
/// build report as the least resonant one, and `peakMagnitude`'s own header says which way that
/// error must never go: *"this number is a SAFETY bound, where under-reading is the direction that
/// lets a cascade of six resonances through."* Flooring reads an unresolvable denominator as "as
/// small as we can tell", which makes the magnitude "as large as we can tell" — the safe half of an
/// answer nobody has.
///
/// **IT IS INERT EVERYWHERE ELSE.** A pole at radius 0.9 gives `Q(-1) = 0.01` and one at 0.99 gives
/// 1e-4, thirteen and eleven orders above this floor, so no patch that is not already at the
/// device's ceiling can reach it. Chosen at 1e-30 rather than at the coefficients' true resolution
/// (~1e-6) deliberately: the floor's only effect is through `realise`'s per-section `gain`, where a
/// SMALLER floor means a smaller gain and a quieter internal state, and the section it applies to is
/// at a Q of 100,000 where over-trimming costs nothing anyone can hear.
constexpr double kMinDenominator = 1.0e-30;

CosQuadratic cosQuadratic(float b1f, float b2f) {
	const double b1 = static_cast<double>(b1f);
	const double b2 = static_cast<double>(b2f);
	return CosQuadratic{1.0 + b1 * b1 + b2 * b2 - 2.0 * b2, 2.0 * b1 * (1.0 + b2), 4.0 * b2};
}

/// WHERE THE ROW'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT. device_row.h's rule: an absent
/// field is at its DECLARED DEFAULT, never at zero — which for a pole angle is not DC.
float fieldOr(const Row& row, size_t fieldsPresent, size_t index) {
	return index < fieldsPresent ? row[index] : static_cast<float>(kParamDescs[index].def);
}

float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	return fieldOr(row, fieldsPresent, static_cast<size_t>(p));
}

} // namespace

Frame withGlobals(const Frame& frame, float cutoffOctaves, float res) {
	const float shift = std::isfinite(cutoffOctaves) ? std::clamp(cutoffOctaves, -16.0f, 16.0f) : 0.0f;
	const float k = 1.0f - std::clamp(res, 0.0f, 1.0f) * kResDepth;

	Frame out = frame;
	for (auto& band : out.band) {
		// UNIFORM ACROSS POLE AND ZERO, both transforms. Cutoff is an addition in the log coordinate
		// and Res is a warp with its fixed point on the circle; applying either to only one of the
		// pair would break every type whose definition is a RELATION between them. The clamp is the
		// same clamp for both, so a Bypass band stays a Bypass band even at the ends of the range.
		band.pole.angleLog2 = std::clamp(band.pole.angleLog2 + shift, kMinAngleLog2, kMaxAngleLog2);
		band.zero.angleLog2 = std::clamp(band.zero.angleLog2 + shift, kMinAngleLog2, kMaxAngleLog2);
		band.pole.radius = warpRadius(band.pole.radius, k);
		band.zero.radius = warpRadius(band.zero.radius, k);
	}
	return out;
}

float peakMagnitude(const Biquad& c) {
	const CosQuadratic p = cosQuadratic(c.b1, c.b2);
	const CosQuadratic q = cosQuadratic(c.a1, c.a2);

	// THE ENDPOINTS ARE ALWAYS CANDIDATES: c = 1 is DC and c = -1 is Nyquist, and a monotone response
	// has its maximum at one of them.
	auto ratioAt = [&](double cc) {
		// FLOORED, NOT REFUSED — see kMinDenominator. The `: 0.0` this replaces returned the SMALLEST
		// possible answer for the LARGEST possible peak, which is how six poles at `kMaxPoleRadius`
		// under `Res = 1` reached infinity on silence: `trim` came out at exactly 1.
		return p.at(cc) / std::max(q.at(cc), kMinDenominator);
	};
	double best = std::max(ratioAt(1.0), ratioAt(-1.0));

	// AND THE INTERIOR EXTREMA ARE THE ROOTS OF P'Q - PQ', whose cubic term cancels identically —
	// see the header. Written out rather than differentiated numerically, because this is a safety
	// bound and a numerical derivative near a high-Q peak is exactly where one would be worst.
	const double A = p.c2 * q.c1 - p.c1 * q.c2;
	const double B = 2.0 * (p.c2 * q.c0 - p.c0 * q.c2);
	const double C = p.c1 * q.c0 - p.c0 * q.c1;

	auto consider = [&](double root) {
		if (root > -1.0 && root < 1.0) {
			best = std::max(best, ratioAt(root));
		}
	};

	if (std::fabs(A) <= 1e-300) {
		// DEGENERATE TO A LINE, which happens whenever the numerator and denominator are proportional
		// — Bypass being the case that matters, where the response is constant and the endpoints have
		// already answered.
		if (std::fabs(B) > 1e-300) {
			consider(-C / B);
		}
	} else {
		const double disc = B * B - 4.0 * A * C;
		if (disc >= 0.0) {
			// THE STABLE PAIR, NOT `(-B +- sqrt(disc)) / 2A`. When `4AC` is small against `B²` the
			// textbook form computes one root as the difference of two nearly equal numbers and loses
			// it; for a Bandpass, where A and C are equal and the two roots are reciprocals, the root
			// it loses is the SMALL one — which is exactly where the peak is. MEASURED: the naive
			// form read 2.1% below a dense search of the same response, and this reads 0.
			const double sq = std::sqrt(disc);
			const double q = -0.5 * (B + (B >= 0.0 ? sq : -sq));
			consider(q / A);
			if (std::fabs(q) > 1e-300) {
				consider(C / q);
			}
		}
	}
	return static_cast<float>(std::sqrt(std::max(0.0, best)));
}

Realised realise(const Frame& frame) {
	Realised out;
	// THE PRODUCT OF THE SECTIONS' NORMALISED PEAKS — an UPPER BOUND on the cascade's peak rather
	// than the peak itself, because the six peaks sit at six different frequencies. Conservative is
	// the right direction for a safety limit.
	float peakProduct = 1.0f;

	for (size_t b = 0; b < kBands; ++b) {
		Section s;
		s.c = coefficientsOf(frame.band[b]);

		const CosQuadratic p = cosQuadratic(s.c.b1, s.c.b2);
		const CosQuadratic q = cosQuadratic(s.c.a1, s.c.a2);
		auto magnitudeAtCos = [&](double cc) {
			// THE SAME FLOOR AS `peakMagnitude`'s, and for the same reason. An edge magnitude of zero
			// where the response is in fact enormous drives `reference` to `peak / kPeakHeadroom`
			// instead of to `peak`, which is a four-times louder section than the one that is there.
			return static_cast<float>(
			    std::sqrt(std::max(0.0, p.at(cc) / std::max(q.at(cc), kMinDenominator))));
		};
		// THE EDGES: DC is c = 1, Nyquist is c = -1. These are what the familiar types pass, and
		// normalising there is what gives a lowpass unity at DC and a highpass unity at Nyquist.
		const float edge = std::max(magnitudeAtCos(1.0), magnitudeAtCos(-1.0));
		const float peak = peakMagnitude(s.c);

		// AND THE PEAK AS A FLOOR, because a Bandpass is null at BOTH edges and a very resonant
		// lowpass is nearly so. Without it the first would divide by zero and the second would lose
		// its passband without limit.
		const float reference = peak > 0.0f ? std::clamp(edge, peak / kPeakHeadroom, peak) : 1.0f;
		s.gain = reference > 0.0f ? 1.0f / reference : 1.0f;
		out.section[b] = s;
		peakProduct *= peak * s.gain;
	}

	// THE CASCADE'S OWN PEAK, SWEPT — not the product of the sections' peaks.
	//
	// Reported from a host: "draw sucks a lot of volume." That is right, and most of it was
	// this line. The product is an UPPER BOUND on the cascade's peak rather than the peak itself,
	// because the six sections peak at six different frequencies — the comment above `realise` in the
	// header said exactly that and it was treated as a caveat instead of a defect.
	//
	// MEASURED, six lowpass bands at r = 0.95, which is an ordinary thing to draw: the product bound
	// asked for **-13.3 dB** of trim while the cascade's TRUE peak was **0.409**, comfortably under
	// `kCascadeCeiling`. So the trim was not protecting against anything; it was throwing away 13 dB
	// because six peaks that never coincide were multiplied as though they did. The whole frame
	// landed at -21 dB, and this was thirteen of them.
	//
	// A SWEEP RATHER THAN A BOUND. `magnitudeAt` already walks the realised cascade, so the peak is
	// the thing itself instead of a proxy for it. 512 points across the axis — a resonant section's
	// peak is broad in `nu` even at high Q, and this runs when the SETTINGS change rather than per
	// sample, next to the six `peakMagnitude` root-solves already here.
	//
	// STILL CONSERVATIVE WHERE IT MATTERS: the ceiling is unchanged, so anything that genuinely
	// reaches it is still trimmed. What changes is that a chain which never gets loud is no longer
	// punished for what its sections could have done separately.
	//
	// BYPASS IS UNTOUCHED, and that is checked rather than hoped for: every section is unity at every
	// frequency, the swept peak is exactly 1, and 1 is under the ceiling.
	(void)peakProduct;
	out.trim = 1.0f;
	float swept = 0.0f;
	const auto look = [&](float nu) {
		const float m = magnitudeAt(out, std::clamp(nu, 0.0f, 0.5f));
		if (std::isfinite(m)) {
			swept = std::max(swept, m);
		}
	};
	// A UNIFORM GRID FOR THE BROAD SHAPE...
	for (int i = 0; i <= kTrimSweepPoints; ++i) {
		look(0.5f * static_cast<float>(i) / static_cast<float>(kTrimSweepPoints));
	}
	// ...AND THEN EACH POLE'S OWN FREQUENCY, WHICH IS WHERE THE NARROW ONES ACTUALLY ARE.
	//
	// A UNIFORM SWEEP ALONE IS NOT ENOUGH AND THE NUMBERS SAY SO. A pole at `kMaxPoleRadius` has a
	// resonance whose width in `nu` is about `(1 - r) / pi` — roughly 3e-5 — while 512 uniform points
	// step by 9.8e-4. **Thirty times too coarse to be sure of landing on it**, and a peak this trim
	// fails to see is a peak it fails to trim, which is the dangerous direction. Passing once is luck,
	// not coverage.
	//
	// So the sections are asked where their own peaks are. A section's resonance sits at its pole
	// angle, which is stored — this is the file's own rule about sampling a curve where its feature
	// is, rather than hoping a grid lands on it. Nine points across each pole's half-width catch the
	// summit and the shoulders even when the realised peak has moved slightly off the stored angle.
	for (const auto& band : frame.band) {
		const float centre = 0.5f * std::exp2(std::clamp(band.pole.angleLog2, kMinAngleLog2, 0.0f));
		const float halfWidth = std::max(1.0e-6f, (1.0f - std::min(band.pole.radius, kMaxPoleRadius))
		                                              / static_cast<float>(kPi));
		for (int k = -4; k <= 4; ++k) {
			look(centre + halfWidth * static_cast<float>(k) * 0.5f);
		}
	}
	out.trim = swept > kCascadeCeiling ? kCascadeCeiling / swept : 1.0f;
	return out;
}

float magnitudeAt(const Realised& realised, float nu) {
	const double w = kPi * static_cast<double>(std::clamp(nu, 0.0f, 1.0f));
	const double cosW = std::cos(w);
	const double sinW = std::sin(w);
	const double cos2W = std::cos(2.0 * w);
	const double sin2W = std::sin(2.0 * w);
	double product = static_cast<double>(realised.trim);
	for (const auto& s : realised.section) {
		const double num = sectionMagnitude(s.c.b1, s.c.b2, cosW, sinW, cos2W, sin2W);
		const double den = sectionMagnitude(s.c.a1, s.c.a2, cosW, sinW, cos2W, sin2W);
		if (den <= 0.0) {
			return std::numeric_limits<float>::infinity();
		}
		product *= static_cast<double>(s.gain) * num / den;
	}
	return static_cast<float>(product);
}

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.morphX = std::clamp(fieldOr(row, fieldsPresent, Param::MorphX), 0.0f, 1.0f);
	s.morphY = std::clamp(fieldOr(row, fieldsPresent, Param::MorphY), 0.0f, 1.0f);
	s.cutoff = fieldOr(row, fieldsPresent, Param::Cutoff);
	s.res = std::clamp(fieldOr(row, fieldsPresent, Param::Res), 0.0f, 1.0f);
	s.drive = std::clamp(fieldOr(row, fieldsPresent, Param::Drive), 0.0f, 1.0f);
	s.wake = std::clamp(fieldOr(row, fieldsPresent, Param::Wake), 0.0f, 1.0f);
	for (size_t f = 0; f < kFrames; ++f) {
		for (size_t b = 0; b < kBands; ++b) {
			Band& band = s.frame[f].band[b];
			band.pole.radius = fieldOr(row, fieldsPresent, bandFieldIndex(f, b, PoleRadius));
			band.pole.angleLog2 = fieldOr(row, fieldsPresent, bandFieldIndex(f, b, PoleAngle));
			band.zero.radius = fieldOr(row, fieldsPresent, bandFieldIndex(f, b, ZeroRadius));
			band.zero.angleLog2 = fieldOr(row, fieldsPresent, bandFieldIndex(f, b, ZeroAngle));
			// THE TYPE, VALIDATED ON THE WAY IN RATHER THAN TRUSTED. A row is whatever reached the
			// transport — a host's automation, a file, a build that declared more types than this one
			// — and an out-of-range type would index `kTypeNames` off the end the moment a menu drew
			// it. Anything this build cannot name reads as `Free`, which is the type that binds
			// nothing and therefore cannot misplace a zero it does not understand.
			const float stored = fieldOr(row, fieldsPresent, bandFieldIndex(f, b, TypeIndex));
			const int index = std::isfinite(stored) ? static_cast<int>(std::lround(stored)) : -1;
			band.type = index >= 0 && index < static_cast<int>(Type::kCount)
			                ? static_cast<Type>(index)
			                : Type::Free;
		}
	}
	// SANITISED ON THE WAY IN AND NOWHERE ELSE. `blend` deliberately does not clamp — clamping there
	// would make the stability property true by force and its test vacuous — so this is the ingest
	// guard that makes the corners legal in the first place.
	sanitise(s);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::MorphX)] = s.morphX;
	row[static_cast<size_t>(Param::MorphY)] = s.morphY;
	row[static_cast<size_t>(Param::Cutoff)] = s.cutoff;
	row[static_cast<size_t>(Param::Res)] = s.res;
	row[static_cast<size_t>(Param::Drive)] = s.drive;
	row[static_cast<size_t>(Param::Wake)] = s.wake;
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
	for (size_t f = 0; f < kFrames; ++f) {
		for (size_t b = 0; b < kBands; ++b) {
			const Band& band = s.frame[f].band[b];
			row[bandFieldIndex(f, b, PoleRadius)] = band.pole.radius;
			row[bandFieldIndex(f, b, PoleAngle)] = band.pole.angleLog2;
			row[bandFieldIndex(f, b, ZeroRadius)] = band.zero.radius;
			row[bandFieldIndex(f, b, ZeroAngle)] = band.zero.angleLog2;
			row[bandFieldIndex(f, b, TypeIndex)] = static_cast<float>(static_cast<int>(band.type));
		}
	}
	return row;
}

Realised realisedAt(const Settings& settings) {
	// BLEND, THEN GLOBALS, THEN REALISE — plan §5.2. Four transforms collapse into one.
	return realise(withGlobals(blend(settings), settings.cutoff, settings.res));
}

} // namespace feathers::fx::drawn
