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

#include "fx/drawn/drawn.h"
#include "fx/drawn/drawn_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <cstdio>
#include <vector>

// THE DRAWN-CURVE FILTER, PHASE 1.
//
// EVERY CLAIM HERE IS ABOUT A VALUE, not about "the curve moved". Where a case needs a response it
// computes the reference by an INDEPENDENT restatement of the intended maths — a closed form written
// out from the pole-zero geometry rather than a second call into the code under test — and keeps any
// difference check only as a second assertion proving the first is not vacuous.

namespace dr = feathers::fx::drawn;

namespace {

constexpr double kPi = 3.14159265358979323846;

/// A FRAME OF SIX BYPASSED BANDS AT DIFFERENT PLACES. Flat by construction, and the six poles all
/// differ, so anything that treats a pole and its zero differently shows up immediately.
dr::Frame bypassedFrame(float radiusBase) {
	dr::Frame f;
	for (size_t b = 0; b < dr::kBands; ++b) {
		f.band[b].pole = dr::Point{radiusBase + 0.07f * static_cast<float>(b),
		                           -10.0f + 1.5f * static_cast<float>(b)};
		f.band[b].zero = f.band[b].pole;
	}
	return f;
}

/// A RANDOM STABLE POLE. Radius stops short of the ceiling so the corners are legal by construction
/// and the property under test is the BLEND's, not the generator's.
dr::Point randomPole(std::mt19937& rng) {
	std::uniform_real_distribution<float> r(0.0f, dr::kMaxPoleRadius);
	std::uniform_real_distribution<float> a(dr::kMinAngleLog2, dr::kMaxAngleLog2);
	return dr::Point{r(rng), a(rng)};
}

dr::Point randomZero(std::mt19937& rng) {
	std::uniform_real_distribution<float> r(-2.0f, 2.0f);
	std::uniform_real_distribution<float> a(dr::kMinAngleLog2, dr::kMaxAngleLog2);
	return dr::Point{r(rng), a(rng)};
}

/// THE MAGNITUDE OF ONE SECTION, WRITTEN OUT FROM THE GEOMETRY rather than from the coefficients:
/// the product of the distances from the evaluation point to each zero, over the same for the poles.
/// This is the independent restatement the value assertions compare against — it shares no line of
/// arithmetic with `drawn::magnitudeAt`, which goes through `(1 + c1 z^-1 + c2 z^-2)`.
///
/// Only valid for a NON-NEGATIVE radius pair (a conjugate pair); the split-real branch has its own
/// reference where it is used.
double referenceMagnitude(const dr::Band& band, double nu) {
	const double w = kPi * nu;
	const double thZ = kPi * std::exp2(std::clamp<double>(band.zero.angleLog2, dr::kMinAngleLog2, 0.0));
	const double thP = kPi * std::exp2(std::clamp<double>(band.pole.angleLog2, dr::kMinAngleLog2, 0.0));
	const double rz = band.zero.radius;
	const double rp = band.pole.radius;
	// |e^{jw} - r e^{jt}| * |e^{jw} - r e^{-jt}| / (same for the pole), which is |H| for a monic
	// biquad because the leading z^2 terms cancel.
	auto pairDistance = [](double r, double t, double omega) {
		const double d1 = std::hypot(std::cos(omega) - r * std::cos(t), std::sin(omega) - r * std::sin(t));
		const double d2 = std::hypot(std::cos(omega) - r * std::cos(t), std::sin(omega) + r * std::sin(t));
		return d1 * d2;
	};
	return pairDistance(rz, thZ, w) / pairDistance(rp, thP, w);
}

//--------------------------------------------------------------------------------------------------
// SCHUR-COHN, for the cascade case. No root finding: the recursion answers "are all roots strictly
// inside the unit disc" for a real polynomial of any degree, which is what a stability claim about a
// blended CHAIN needs and what the quadratic conditions cannot reach.
//--------------------------------------------------------------------------------------------------

using Poly = std::vector<double>; // constant term first; monic, so back() == 1

bool schurStable(Poly c) {
	while (c.size() > 1) {
		const double c0 = c.front();
		const double cn = c.back();
		if (std::fabs(c0) >= std::fabs(cn)) {
			return false;
		}
		const Poly reversed(c.rbegin(), c.rend());
		Poly q(c.size() - 1);
		for (size_t i = 1; i < c.size(); ++i) {
			q[i - 1] = cn * c[i] - c0 * reversed[i];
		}
		const double lead = q.back();
		if (lead == 0.0) {
			return false;
		}
		for (auto& v : q) {
			v /= lead;
		}
		c = q;
	}
	return true;
}

Poly denominatorOf(const dr::Point& pole) {
	const double th = kPi * std::exp2(std::clamp<double>(pole.angleLog2, dr::kMinAngleLog2, 0.0));
	const double r = pole.radius;
	return {r * r, -2.0 * r * std::cos(th), 1.0};
}

Poly multiply(const Poly& a, const Poly& b) {
	Poly out(a.size() + b.size() - 1, 0.0);
	for (size_t i = 0; i < a.size(); ++i) {
		for (size_t j = 0; j < b.size(); ++j) {
			out[i + j] += a[i] * b[j];
		}
	}
	return out;
}

/// A FRAME WHOSE BANDS ARE ALL FLAT EXCEPT ONE, so a frame-level magnitude is that band's magnitude
/// exactly — `Bypass` sections contribute a bit-exact 1.0f, which is the property case
/// "Bypass is the zero on the pole" pins.
dr::Frame frameWithOneBand(const dr::Band& band) {
	dr::Frame f;
	f.band[0] = band;
	return f;
}

} // namespace

//==================================================================================================
// 1. THE WEIGHTS. Everything else rests on these four numbers.
//==================================================================================================

TEST_CASE("the four bilinear weights are non-negative and sum to one") {
	// NON-NEGATIVITY IS ASSERTED EXACTLY and the sum within a MEASURED tolerance, because those are
	// the two different things float can promise here: a product of non-negative floats is
	// non-negative with no error at all, while four products summed is not exactly one.
	//
	// THE GRID IS DELIBERATELY NOT A POWER OF TWO, and this is the whole reason the case is worth
	// running. MEASURED: over a 129x129 grid of x = k/128 the worst |sum - 1| is EXACTLY ZERO —
	// every weight is a product of exact binary fractions, so a dyadic grid cannot see the rounding
	// this case exists to bound, and a tolerance checked against it would be a tolerance checked
	// against nothing. Over k/127 and k/1000 the worst deviations are 8.75e-8 and 1.04e-7.
	double worstSum = 0.0;
	float worstX = 0.0f;
	float worstY = 0.0f;
	int negatives = 0;
	double dyadicWorst = 0.0;
	for (const int steps : {127, 1000}) {
		for (int ix = 0; ix <= steps; ++ix) {
			for (int iy = 0; iy <= steps; ++iy) {
				const float x = static_cast<float>(ix) / static_cast<float>(steps);
				const float y = static_cast<float>(iy) / static_cast<float>(steps);
				const auto w = dr::bilinearWeights(x, y);
				for (float v : w) {
					if (!(v >= 0.0f)) {
						++negatives;
					}
				}
				const double sum = static_cast<double>(w[0]) + w[1] + w[2] + w[3];
				if (std::fabs(sum - 1.0) > worstSum) {
					worstSum = std::fabs(sum - 1.0);
					worstX = x;
					worstY = y;
				}
			}
		}
	}
	for (int ix = 0; ix <= 128; ++ix) {
		for (int iy = 0; iy <= 128; ++iy) {
			const auto w = dr::bilinearWeights(static_cast<float>(ix) / 128.0f,
			                                   static_cast<float>(iy) / 128.0f);
			const double sum = static_cast<double>(w[0]) + w[1] + w[2] + w[3];
			dyadicWorst = std::max(dyadicWorst, std::fabs(sum - 1.0));
		}
	}
	CHECK(negatives == 0);
	CAPTURE(worstX);
	CAPTURE(worstY);
	CAPTURE(worstSum);
	// 2e-7 is the measured 1.04e-7 with one bit of margin, NOT a round number picked to pass.
	CHECK(worstSum <= 2e-7);
	// The grid really is exercising the rounding: it found some, and the dyadic one found none.
	CHECK(worstSum > 0.0);
	CHECK(dyadicWorst == 0.0);

	// AND THIS IS THE NUMBER kMaxPoleRadius IS SIZED AGAINST. A blended radius can exceed the largest
	// corner radius by that relative error, so the ceiling has to leave more room than it: 1 - 0.9999
	// is 1e-4, which is three orders above 1.04e-7.
	CHECK(static_cast<double>(1.0f - dr::kMaxPoleRadius) > 100.0 * worstSum);
}

TEST_CASE("out of range, the puck is clamped rather than allowed a negative weight") {
	// A NEGATIVE WEIGHT IS NOT A CONVEX COMBINATION, and everything in this file that says "by
	// construction" stops being true the moment one appears. x = 2 would give (1-x) = -1.
	for (const auto& xy : {std::pair<float, float>{-1.0f, 0.5f}, {2.0f, 0.5f}, {0.5f, -3.0f},
	                      {0.5f, 4.0f}, {-1.0f, -1.0f}, {2.0f, 2.0f}}) {
		const auto w = dr::bilinearWeights(xy.first, xy.second);
		CAPTURE(xy.first);
		CAPTURE(xy.second);
		for (float v : w) {
			CHECK(v >= 0.0f);
		}
		CHECK(static_cast<double>(w[0]) + w[1] + w[2] + w[3] == doctest::Approx(1.0).epsilon(1e-6));
	}
}

//==================================================================================================
// 2. THE CORNERS. An off-by-one in a weight passes every interior check and fails only this.
//==================================================================================================

TEST_CASE("at each of the four corners the blend is that corner, bit for bit") {
	std::mt19937 rng(20260827u);
	std::array<dr::Frame, dr::kFrames> corners{};
	for (auto& frame : corners) {
		for (auto& band : frame.band) {
			band.pole = randomPole(rng);
			band.zero = randomZero(rng);
		}
	}

	// EXACT EQUALITY, not Approx. The blend at a corner is `1 * a + 0 * b + 0 * c + 0 * d`, and both
	// halves of that are exact in IEEE — so anything less than bit equality here is a real defect,
	// and a tolerance would hide exactly the corner-order swap this case exists to catch.
	const std::array<std::pair<float, float>, dr::kFrames> puck{
	    std::pair<float, float>{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
	for (size_t k = 0; k < dr::kFrames; ++k) {
		CAPTURE(k);
		const dr::Frame got = dr::blend(corners, puck[k].first, puck[k].second);
		CHECK(got == corners[k]);
	}

	// AND THE CORNERS ARE DISTINCT, so the four checks above cannot all pass by the frames agreeing.
	for (size_t a = 0; a < dr::kFrames; ++a) {
		for (size_t b = a + 1; b < dr::kFrames; ++b) {
			CHECK_FALSE(corners[a] == corners[b]);
		}
	}
}

TEST_CASE("the corner order is x-then-y, and a transposed weight is visible from the edges") {
	// THE NAMED CORNERS ARE A CONTRACT. X1Y0 is the corner at x = 1, y = 0; a blend that swapped the
	// two off-diagonal weights would still be exact at all four corners and still convex, and would
	// mirror the whole plane. The edge midpoints are where that shows.
	std::array<dr::Frame, dr::kFrames> corners{};
	for (size_t k = 0; k < dr::kFrames; ++k) {
		for (auto& band : corners[k].band) {
			band.pole = dr::Point{0.25f * static_cast<float>(k + 1), -6.0f};
			band.zero = band.pole;
		}
	}
	// Halfway along the y = 0 edge: X0Y0 and X1Y0 only.
	CHECK(dr::blend(corners, 0.5f, 0.0f).band[0].pole.radius
	      == doctest::Approx(0.5f * (0.25f + 0.50f)).epsilon(1e-6));
	// Halfway along the x = 0 edge: X0Y0 and X0Y1 only.
	CHECK(dr::blend(corners, 0.0f, 0.5f).band[0].pole.radius
	      == doctest::Approx(0.5f * (0.25f + 0.75f)).epsilon(1e-6));
	// The centre takes all four.
	CHECK(dr::blend(corners, 0.5f, 0.5f).band[0].pole.radius
	      == doctest::Approx(0.25f * (0.25f + 0.50f + 0.75f + 1.00f)).epsilon(1e-6));
}

//==================================================================================================
// 3. STABILITY BY CONSTRUCTION — the property the whole representation is for.
//==================================================================================================

TEST_CASE("a blended pole is inside the unit circle at every puck position, for randomised corners") {
	std::mt19937 rng(99u);
	const int kTrials = 4000;
	const int kSteps = 20;

	long blends = 0;
	int unstable = 0;
	int outsideHull = 0;
	int juryFailures = 0;
	double worstRadius = 0.0;

	for (int trial = 0; trial < kTrials; ++trial) {
		std::array<dr::Point, dr::kFrames> poles{};
		for (auto& p : poles) {
			p = randomPole(rng);
		}
		float lo = poles[0].radius;
		float hi = poles[0].radius;
		for (const auto& p : poles) {
			lo = std::min(lo, p.radius);
			hi = std::max(hi, p.radius);
		}
		for (int ix = 0; ix <= kSteps; ++ix) {
			for (int iy = 0; iy <= kSteps; ++iy) {
				const float x = static_cast<float>(ix) / static_cast<float>(kSteps);
				const float y = static_cast<float>(iy) / static_cast<float>(kSteps);
				const dr::Point blended = dr::blend(poles, x, y);
				++blends;
				if (!dr::poleIsStable(blended)) {
					++unstable;
				}
				worstRadius = std::max(worstRadius, static_cast<double>(blended.radius));

				// THE VALUE, RESTATED INDEPENDENTLY: a convex combination lies in the convex hull of
				// its inputs, so the blended radius is between the smallest and largest corner
				// radius. That is a stronger statement than "< 1" and it is what "< 1" follows from;
				// checking only "< 1" would pass for a blend that ignored three of the corners.
				if (blended.radius < lo - 1e-6f || blended.radius > hi + 1e-6f) {
					++outsideHull;
				}

				// AND THE REALISED DENOMINATOR, which is the thing the audio path will actually run.
				// Jury: both roots of z^2 + a1 z + a2 are inside the unit disc iff |a2| < 1 and
				// |a1| < 1 + a2. Written out here rather than inferred from the radius, because the
				// claim that matters downstream is about coefficients.
				dr::Band band;
				band.pole = blended;
				band.zero = blended;
				const dr::Biquad c = dr::coefficientsOf(band);
				if (!(std::fabs(c.a2) < 1.0f && std::fabs(c.a1) < 1.0f + c.a2)) {
					++juryFailures;
				}
			}
		}
	}
	CAPTURE(blends);
	CAPTURE(worstRadius);
	CHECK(unstable == 0);
	CHECK(outsideHull == 0);
	CHECK(juryFailures == 0);
	// The measurement is only worth anything if the corners really did reach the ceiling.
	CHECK(worstRadius > 0.999);
	CHECK(worstRadius < 1.0);
}

TEST_CASE("blending one band's DENOMINATOR COEFFICIENTS is ALSO stable — the known-bad that does not fire") {
	// THIS CASE RECORDS A CORRECTION TO THE DESIGN NOTE, and it is here so nobody re-derives the
	// wrong half of it. The drawn-curve design note (§3b) says blending biquad
	// COEFFICIENTS "can leave the stable region in the middle of a morph between two perfectly stable
	// filters". For ONE BAND'S DENOMINATOR that is false, and provably: the Jury conditions
	// |a2| < 1, a2 > a1 - 1, a2 > -a1 - 1 are three half-planes, so the stable set in (a1, a2) is a
	// TRIANGLE with vertices (-2, 1), (2, 1), (0, -1) — and a triangle is convex, so a convex
	// combination of stable denominators is stable.
	//
	// The measurement below is the instrument agreeing with the proof rather than replacing it. The
	// case that follows is where coefficient blending DOES break, and it is the one that carries the
	// design's argument.
	std::mt19937 rng(4242u);
	const int kTrials = 4000;
	const int kSteps = 10;
	long blends = 0;
	int unstable = 0;
	double worstRootMagnitude = 0.0;

	for (int trial = 0; trial < kTrials; ++trial) {
		std::array<float, dr::kFrames> a1{};
		std::array<float, dr::kFrames> a2{};
		for (size_t k = 0; k < dr::kFrames; ++k) {
			dr::Band band;
			band.pole = randomPole(rng);
			band.zero = band.pole;
			const dr::Biquad c = dr::coefficientsOf(band);
			a1[k] = c.a1;
			a2[k] = c.a2;
		}
		for (int ix = 0; ix <= kSteps; ++ix) {
			for (int iy = 0; iy <= kSteps; ++iy) {
				const float x = static_cast<float>(ix) / static_cast<float>(kSteps);
				const float y = static_cast<float>(iy) / static_cast<float>(kSteps);
				const auto w = dr::bilinearWeights(x, y);
				double A1 = 0.0;
				double A2 = 0.0;
				for (size_t k = 0; k < dr::kFrames; ++k) {
					A1 += static_cast<double>(w[k]) * a1[k];
					A2 += static_cast<double>(w[k]) * a2[k];
				}
				++blends;
				const double disc = A1 * A1 - 4.0 * A2;
				const double mag = disc < 0.0 ? std::sqrt(A2)
				                              : std::max(std::fabs((-A1 + std::sqrt(disc)) / 2.0),
				                                         std::fabs((-A1 - std::sqrt(disc)) / 2.0));
				worstRootMagnitude = std::max(worstRootMagnitude, mag);
				if (mag >= 1.0) {
					++unstable;
				}
			}
		}
	}
	CAPTURE(blends);
	CAPTURE(worstRootMagnitude);
	CHECK(unstable == 0);
	CHECK(worstRootMagnitude < 1.0);
	CHECK(worstRootMagnitude > 0.999); // the corners did reach the ceiling, so the silence means something
}

TEST_CASE("blending the CASCADE's expanded denominator IS unstable, which is where the design's argument lives") {
	// SAME GENERATOR, SAME GRID, ONE DIFFERENCE: the six bands are multiplied into ONE polynomial
	// before blending. The Schur region for degree >= 3 is not convex, so the guarantee the previous
	// case found for a single section evaporates. This is the half of §3b that holds.
	std::mt19937 rng(7u);
	const int kTrials = 400;
	const int kSteps = 10;
	long blends = 0;
	long unstable = 0;
	int cornersUnstable = 0;

	for (int trial = 0; trial < kTrials; ++trial) {
		std::array<Poly, dr::kFrames> corner{};
		for (size_t k = 0; k < dr::kFrames; ++k) {
			Poly p{1.0};
			for (size_t b = 0; b < dr::kBands; ++b) {
				p = multiply(p, denominatorOf(randomPole(rng)));
			}
			corner[k] = p;
			// THE PRECONDITION, CHECKED RATHER THAN ASSUMED: every corner is itself stable, so an
			// unstable blend below is the blend's doing and not the generator's.
			if (!schurStable(p)) {
				++cornersUnstable;
			}
		}
		for (int ix = 0; ix <= kSteps; ++ix) {
			for (int iy = 0; iy <= kSteps; ++iy) {
				const float x = static_cast<float>(ix) / static_cast<float>(kSteps);
				const float y = static_cast<float>(iy) / static_cast<float>(kSteps);
				const auto w = dr::bilinearWeights(x, y);
				Poly blended(corner[0].size(), 0.0);
				for (size_t k = 0; k < dr::kFrames; ++k) {
					for (size_t i = 0; i < blended.size(); ++i) {
						blended[i] += static_cast<double>(w[k]) * corner[k][i];
					}
				}
				++blends;
				if (!schurStable(blended)) {
					++unstable;
				}
			}
		}
	}
	CAPTURE(blends);
	CAPTURE(unstable);
	CHECK(cornersUnstable == 0);
	// MEASURED at roughly half of all blends for six bands; asserted loosely because the number is a
	// property of the generator, and what the case claims is that the failure is COMMON rather than
	// exotic — the opposite of the per-band result above.
	CHECK(unstable > blends / 10);
}

//==================================================================================================
// 4. BYPASS — the zero on the pole.
//==================================================================================================

TEST_CASE("Bypass is the zero on the pole, and four DIFFERENT bypassed corners stay exactly flat") {
	// THE STRONG FORM, and it is the one worth having. Flatness at a corner would be true of any
	// blend at all; flatness at EVERY puck position when the four corners' poles are all different
	// is a statement about the blend itself — pole and zero go through the identical linear map, so
	// a zero that starts on its pole stays on it.
	std::array<dr::Frame, dr::kFrames> corners{};
	for (size_t k = 0; k < dr::kFrames; ++k) {
		for (size_t b = 0; b < dr::kBands; ++b) {
			const dr::Point pole{0.2f + 0.19f * static_cast<float>(k),
			                     -10.0f + static_cast<float>(b) + 0.5f * static_cast<float>(k)};
			corners[k].band[b].pole = pole;
			const auto zero = dr::zeroPlacementFor(dr::Type::Bypass, pole);
			REQUIRE(zero.has_value());
			corners[k].band[b].zero = *zero;
			CHECK(*zero == pole);
		}
	}

	int notFlat = 0;
	int zeroOffPole = 0;
	for (int ix = 0; ix <= 16; ++ix) {
		for (int iy = 0; iy <= 16; ++iy) {
			const float x = static_cast<float>(ix) / 16.0f;
			const float y = static_cast<float>(iy) / 16.0f;
			const dr::Frame f = dr::blend(corners, x, y);
			for (const auto& band : f.band) {
				if (!(band.zero == band.pole)) {
					++zeroOffPole;
				}
			}
			for (int i = 0; i <= 64; ++i) {
				const float nu = static_cast<float>(i) / 64.0f;
				// EXACT 1.0f, NOT A TOLERANCE. The numerator and denominator are computed by the same
				// function from bit-identical inputs, so the ratio is exactly one — and a tolerance
				// here would accept a blend that moved the zero a little off its pole, which is
				// precisely the defect this case exists to catch.
				if (dr::magnitudeAt(f, nu) != 1.0f) {
					++notFlat;
				}
			}
		}
	}
	CHECK(zeroOffPole == 0);
	CHECK(notFlat == 0);

	// AND THE CORNERS REALLY DID DIFFER, so "flat everywhere" is not "the same frame four times".
	for (size_t a = 0; a < dr::kFrames; ++a) {
		for (size_t b = a + 1; b < dr::kFrames; ++b) {
			CHECK_FALSE(corners[a] == corners[b]);
		}
	}
}

TEST_CASE("a BYPASSED device is flat, at every puck position") {
	// REPOINTED AT AN EXPLICITLY BYPASSED SETTINGS, and that is a repair rather than a rename. This
	// case used to build `dr::Settings{}` and lean on the default frames being flat; when
	// `kDefaultFrames` became four audibly different corners it silently stopped saying anything about
	// `Bypass` at all.
	//
	// **IT STAYS REPOINTED NOW THAT THE DEFAULT IS FLAT AGAIN**, which is the half worth writing down.
	// Re-coupling it would make it green for the right reason today and for no reason at all the next
	// time the table moves — and this table has now moved twice in two days. The property here was
	// never a property of the DEFAULT: it is that `Bypass` is exactly transparent, and it is asked of
	// a frame this case builds. The default's own flatness is asserted by the case below, which says
	// so in its name.
	dr::Settings s;
	for (auto& frame : s.frame) {
		frame = bypassedFrame(0.5f);
	}
	int notFlat = 0;
	for (int ix = 0; ix <= 8; ++ix) {
		for (int iy = 0; iy <= 8; ++iy) {
			s.morphX = static_cast<float>(ix) / 8.0f;
			s.morphY = static_cast<float>(iy) / 8.0f;
			const dr::Frame f = dr::blend(s);
			for (int i = 0; i <= 32; ++i) {
				if (dr::magnitudeAt(f, static_cast<float>(i) / 32.0f) != 1.0f) {
					++notFlat;
				}
			}
		}
	}
	CHECK(notFlat == 0);
	// **AND THE DEVICE IS ON WHEN YOU ADD IT.** This asserted OFF and called it
	// *"the second of the two guards on the same property"*. There is one guard now, and it is this
	// file's whole subject: THE DEFAULT IS EXACTLY FLAT. That is what makes the decision free for this
	// device — an added drawn filter is audible only once a hand moves it — and it is also why the
	// flatness cases matter more than they did, not less.
	CHECK(dr::Settings{}.enabled);
}

TEST_CASE("the DEFAULT device is EXACTLY flat — at every puck position, and at every Cutoff and Res") {
	// **THE SAFETY PROPERTY, ASKED OF THE SHIPPED DEFAULT.** A device you add to a chain must not
	// change your sound, so the default is flat.
	//
	// THIS CASE IS THE INVERSE OF THE ONE THAT STOOD HERE FOR A DAY, and the inversion is not a
	// retraction. `kDefaultFrames` held four audibly different filters — lowpass, resonant bandpass,
	// notches, highpass — for one specific job: registration had just moved forward a phase so the
	// device could be AUDITIONED, and four copies of one flat frame blend to a wire, so the concept
	// would have been judged on a default patch incapable of expressing it. **That job is done.** The
	// device has been heard and the concept approved, so the demonstration has nobody left to convince
	// and the safety property gets its default back.
	//
	// ASSERTED AS A VALUE, AND AT EXACT EQUALITY. The model is "four DIFFERENT bypassed corners stay
	// exactly flat" above, and exactness is available for the same reason: every default band is
	// `Bypass`, so its numerator and its denominator are computed by one function from bit-identical
	// inputs and the ratio is exactly one. **A tolerance would accept a default that quietly coloured
	// everything**, which is the whole defect this case is here to forbid.
	//
	// AND IT IS ASKED THROUGH `realisedAt`, which is the entire path — blend, then the globals, then
	// coefficient realisation — rather than through `blend` alone. That is what makes the Cutoff and
	// Res sweeps mean anything: §3c's rule is that a global must move pole and zero TOGETHER, and the
	// one time it did not, a poles-only Res measured 148.2 dB of resonance on a patch with nothing
	// drawn on it. This case is where a repeat of that dies.
	dr::Settings s;
	// **SWITCHED ON, or every number below would be about a device that is not running.** `enabled`
	// defaults false and that is a SECOND guard (asserted at the bottom); flatness has to hold on its
	// own or "add it and turn it on" is still a change of sound.
	s.enabled = true;

	int notFlat = 0;
	double worstDb = 0.0;
	const auto sweep = [&](const dr::Settings& at) {
		const dr::Realised r = dr::realisedAt(at);
		for (int i = 0; i <= 64; ++i) {
			const float nu = static_cast<float>(i) / 64.0f;
			const float mag = dr::magnitudeAt(r, nu);
			if (mag != 1.0f) {
				++notFlat;
			}
			worstDb = std::max(worstDb,
			                   std::fabs(20.0 * std::log10(std::max(1e-12, static_cast<double>(mag)))));
		}
	};

	// 5 x 5 puck positions x 5 Cutoffs across the eight octaves the control spans x 3 Res settings.
	for (int ic = -2; ic <= 2; ++ic) {
		for (int ir = 0; ir <= 2; ++ir) {
			for (int ix = 0; ix <= 4; ++ix) {
				for (int iy = 0; iy <= 4; ++iy) {
					dr::Settings at = s;
					at.cutoff = 2.0f * static_cast<float>(ic);
					at.res = static_cast<float>(ir) / 2.0f;
					at.morphX = static_cast<float>(ix) / 4.0f;
					at.morphY = static_cast<float>(iy) / 4.0f;
					sweep(at);
				}
			}
		}
	}
	CAPTURE(worstDb);
	CHECK(notFlat == 0);
	CHECK(worstDb == 0.0);

	// **THE SWEEP IS NOT VACUOUS**, which a flatness claim needs more than any other kind: every
	// assertion above would also pass against a `magnitudeAt` that returned 1.0 for everything, or a
	// `realisedAt` that ignored the globals it was handed. So the SAME sweep is run against a default
	// with ONE band's zero moved off its pole, and it must be nowhere near flat.
	{
		const int flatCount = notFlat;
		notFlat = 0;
		worstDb = 0.0;
		dr::Settings dented = s;
		dented.frame[dr::X0Y0].band[2].zero = dr::Point { 1.0f, dented.frame[dr::X0Y0].band[2].pole.angleLog2 };
		for (int ix = 0; ix <= 4; ++ix) {
			for (int iy = 0; iy <= 4; ++iy) {
				dr::Settings at = dented;
				at.morphX = static_cast<float>(ix) / 4.0f;
				at.morphY = static_cast<float>(iy) / 4.0f;
				sweep(at);
			}
		}
		CAPTURE(worstDb);
		CHECK(flatCount == 0);
		CHECK(notFlat > 0);
		// A NULL IS A BIG NUMBER: one band turned from Bypass into a Notch is tens of dB, not a
		// rounding difference, so the bar says "audible" rather than "different".
		CHECK(worstDb > 12.0);
	}

	// THE MECHANISM, STATED DIRECTLY rather than inferred from the sweep: every band of every default
	// frame is `Bypass` — the zero exactly on the pole — which is what makes the response flat by
	// CONSTRUCTION at every puck position rather than by four curves happening to cancel.
	for (size_t k = 0; k < dr::kFrames; ++k) {
		CAPTURE(k);
		for (size_t b = 0; b < dr::kBands; ++b) {
			CAPTURE(b);
			CHECK(dr::kDefaultFrames[k].band[b].zero == dr::kDefaultFrames[k].band[b].pole);
		}
	}

	// **AND THE DEVICE IS ON WHEN YOU ADD IT — every effect defaults to on when it is added.**
	// This held OFF as the second of two guards, and the note beside it
	// already said which of the two was load-bearing: *"the first thing a hand does to a new device
	// is switch it on."* The switch now starts where the hand was going to put it, so the FLAT
	// default above is the only thing standing between adding this device and changing the sound.
	CHECK(dr::Settings{}.enabled);

	// THE SIX HANDLES ARE SPREAD RATHER THAN STACKED, which is not a sound claim at all — it is what
	// the phase-4 editor opens on. Two bands with identical numbers draw at the same point and one of
	// the six becomes unreachable by any press; that shipped once and the panel's own case caught it.
	// Nothing about flatness depends on WHERE they sit, so this is the assertion that keeps the
	// spreading from being deleted as decoration.
	for (size_t k = 0; k < dr::kFrames; ++k) {
		CAPTURE(k);
		const auto& band = dr::kDefaultFrames[k].band;
		for (size_t b = 1; b < dr::kBands; ++b) {
			CAPTURE(b);
			CHECK(band[b].pole.angleLog2 > band[b - 1].pole.angleLog2);
		}
	}
}

//==================================================================================================
// 5. THE NAMED TYPES — each writes the zero the type means.
//==================================================================================================

TEST_CASE("each named type writes the zero placement that type means") {
	const dr::Point pole{0.9f, -4.0f}; // Nyquist/16

	SUBCASE("Lowpass puts both zeros at z = -1") {
		const auto z = dr::zeroPlacementFor(dr::Type::Lowpass, pole);
		REQUIRE(z.has_value());
		CHECK(z->radius == 1.0f);
		CHECK(z->angleLog2 == dr::kMaxAngleLog2);
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		const dr::Biquad c = dr::coefficientsOf(band);
		// numerator 1 + 2 z^-1 + z^-2 = (1 + z^-1)^2, a double root at z = -1.
		CHECK(c.b1 == doctest::Approx(2.0).epsilon(1e-6));
		CHECK(c.b2 == doctest::Approx(1.0).epsilon(1e-6));
		// The null is at Nyquist and nowhere else.
		CHECK(dr::magnitudeAt(band, 1.0f) == doctest::Approx(0.0).epsilon(1e-5));
		CHECK(dr::magnitudeAt(band, 0.0f) > 1.0f);
	}

	SUBCASE("Highpass puts both zeros at DC — as near as the log coordinate reaches") {
		const auto z = dr::zeroPlacementFor(dr::Type::Highpass, pole);
		REQUIRE(z.has_value());
		CHECK(z->radius == 1.0f);
		CHECK(z->angleLog2 == dr::kMinAngleLog2);
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		// THE FLOOR IS A REAL LIMIT AND THIS IS WHERE IT SHOWS: the DC gain is tiny, not zero.
		// Asserted as a value with a range rather than hidden behind an "approximately zero", so the
		// day somebody changes kMinAngleLog2 this case tells them what it cost.
		const double dc = dr::magnitudeAt(band, 0.0f);
		CAPTURE(dc);
		CHECK(dc > 0.0);
		CHECK(dc < 1e-4);
		// And it is still a highpass: passband well above the stopband.
		CHECK(dr::magnitudeAt(band, 1.0f) > 1.0f);
	}

	SUBCASE("Bandpass puts one zero at DC and one at Nyquist, on the split-real branch") {
		const auto z = dr::zeroPlacementFor(dr::Type::Bandpass, pole);
		REQUIRE(z.has_value());
		CHECK(z->radius < 0.0f); // the branch selector
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		const dr::Biquad c = dr::coefficientsOf(band);
		// numerator 1 + b1 z^-1 + b2 z^-2, wanted 1 - z^-2 so that its roots are exactly {+1, -1}.
		CHECK(c.b1 == doctest::Approx(0.0).epsilon(1e-6));
		CHECK(c.b2 == doctest::Approx(-1.0).epsilon(1e-6));
		// EVALUATED AT THE TWO ROOTS, which is the statement "the zeros are there" rather than
		// "the coefficients look right": 1 + b1 z^-1 + b2 z^-2 at z = +1 and z = -1. ABSOLUTE bounds,
		// because a relative tolerance against an expected 0 means nothing.
		CHECK(std::fabs(1.0 + c.b1 + c.b2) < 1e-6);
		CHECK(std::fabs(1.0 - c.b1 + c.b2) < 1e-6);

		// AND THE NULLS ARE FINITE, WHICH IS A MEASURED FACT ABOUT THIS COORDINATE RATHER THAN A
		// SLOPPY TOLERANCE. theta = pi/2 is angleLog2 = -1, and cosf(pi/2) is -4.37e-8 rather than
		// zero, so the two zeros land at 1 -+ 4.4e-8 instead of exactly +-1. MEASURED for this pole:
		// peak 10.18, DC 1.337e-6, Nyquist 5.92e-8 — so -137.6 dB and -164.7 dB of rejection rather
		// than infinite. Asserted as a RATIO so the claim is "this is a null" and not "this number is
		// small", and left unfixed because a null at -137 dB is a null.
		const double peak = dr::magnitudeAt(band, static_cast<float>(std::exp2(pole.angleLog2)));
		const double atDc = dr::magnitudeAt(band, 0.0f);
		const double atNyquist = dr::magnitudeAt(band, 1.0f);
		CAPTURE(peak);
		CAPTURE(atDc);
		CAPTURE(atNyquist);
		CHECK(atDc / peak < 1e-6);
		CHECK(atNyquist / peak < 1e-6);
		// A NULL AT BOTH ENDS AND SOMETHING IN THE MIDDLE — otherwise "zero at both ends" is
		// satisfied by a section that is zero everywhere.
		CHECK(peak > 1.0);
	}

	SUBCASE("Notch puts the zeros on the circle at the pole's own angle") {
		const auto z = dr::zeroPlacementFor(dr::Type::Notch, pole);
		REQUIRE(z.has_value());
		CHECK(z->radius == 1.0f);
		CHECK(z->angleLog2 == pole.angleLog2);
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		const float nuNotch = std::exp2(pole.angleLog2);
		CHECK(dr::magnitudeAt(band, nuNotch) == doctest::Approx(0.0).epsilon(1e-5));
		// An octave either side it is back near unity — a null, not an attenuation.
		CHECK(dr::magnitudeAt(band, nuNotch * 0.25f) > 0.1f);
		CHECK(dr::magnitudeAt(band, nuNotch * 4.0f) > 0.1f);
	}

	SUBCASE("Allpass mirrors the zero to 1/r, and the magnitude is flat at 1/r^2") {
		const auto z = dr::zeroPlacementFor(dr::Type::Allpass, pole);
		REQUIRE(z.has_value());
		CHECK(z->radius == doctest::Approx(1.0f / pole.radius).epsilon(1e-6));
		CHECK(z->angleLog2 == pole.angleLog2);
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		// THE VALUE, RESTATED: a reversed real polynomial has the same magnitude on the unit circle,
		// so |H| = 1/r^2 everywhere. Nothing here is gain-normalised — see the header on Biquad.
		const double expected = 1.0 / (static_cast<double>(pole.radius) * pole.radius);
		for (int i = 0; i <= 32; ++i) {
			const float nu = static_cast<float>(i) / 32.0f;
			CAPTURE(nu);
			CHECK(dr::magnitudeAt(band, nu) == doctest::Approx(expected).epsilon(1e-5));
		}
	}

	SUBCASE("Bell pulls the zero off the circle by the gain") {
		const float gain = 2.0f;
		const auto z = dr::zeroPlacementFor(dr::Type::Bell, pole, gain);
		REQUIRE(z.has_value());
		CHECK(z->angleLog2 == pole.angleLog2);
		CHECK(z->radius == doctest::Approx(1.0f - gain * (1.0f - pole.radius)).epsilon(1e-6));
		dr::Band band;
		band.pole = pole;
		band.zero = *z;
		// THE VALUE AT THE CORNER, RESTATED INDEPENDENTLY from the pole-zero geometry: at w = theta,
		// |H| = (1-rz)|1 - rz e^{-2j theta}| / ((1-rp)|1 - rp e^{-2j theta}|). The familiar
		// "gain = (1-rz)/(1-rp)" is that with the second factor dropped, which is an approximation;
		// the exact form is what is asserted.
		const double th = kPi * std::exp2(static_cast<double>(pole.angleLog2));
		const double rz = z->radius;
		const double rp = pole.radius;
		const double expected = ((1.0 - rz) * std::hypot(1.0 - rz * std::cos(2.0 * th), rz * std::sin(2.0 * th)))
		                        / ((1.0 - rp) * std::hypot(1.0 - rp * std::cos(2.0 * th), rp * std::sin(2.0 * th)));
		CHECK(dr::magnitudeAt(band, std::exp2(pole.angleLog2)) == doctest::Approx(expected).epsilon(1e-5));
		// A BOOST, and the direction is the whole point of the sign convention.
		CHECK(expected > 1.5);
	}

	SUBCASE("Bypass returns the pole itself") {
		const auto z = dr::zeroPlacementFor(dr::Type::Bypass, pole);
		REQUIRE(z.has_value());
		CHECK(*z == pole);
	}

	SUBCASE("Free has no placement rule at all") {
		CHECK_FALSE(dr::zeroPlacementFor(dr::Type::Free, pole).has_value());
	}
}

TEST_CASE("the closed-form magnitude agrees with the pole-zero geometry it claims to describe") {
	// THE SECOND ASSERTION THAT KEEPS THE FIRST HONEST. Every response claim above goes through
	// drawn::magnitudeAt; this checks that function against a restatement written from distances in
	// the z plane, which shares no arithmetic with it.
	std::mt19937 rng(31415u);
	double worstRelative = 0.0;
	for (int trial = 0; trial < 500; ++trial) {
		dr::Band band;
		band.pole = randomPole(rng);
		std::uniform_real_distribution<float> rz(0.0f, 2.0f);
		band.zero = dr::Point{rz(rng), randomPole(rng).angleLog2};
		for (int i = 1; i < 32; ++i) {
			const double nu = static_cast<double>(i) / 32.0;
			const double got = dr::magnitudeAt(band, static_cast<float>(nu));
			const double want = referenceMagnitude(band, nu);
			const double denom = std::max(1e-9, std::fabs(want));
			worstRelative = std::max(worstRelative, std::fabs(got - want) / denom);
		}
	}
	CAPTURE(worstRelative);
	CHECK(worstRelative < 1e-4);
}

//==================================================================================================
// 6. A NOTCH CORNER AND AN LPF CORNER, BLENDED.
//==================================================================================================

TEST_CASE("a Notch corner and an LPF corner blend continuously, and the midpoint lies between them") {
	// THE CORNERS SHARE A POLE ON PURPOSE. With identical denominators the ratio of any two of these
	// responses is the ratio of their NUMERATORS, so "between" is a statement about the zero and
	// nothing else — which is the thing being blended. With differing poles "between" would not even
	// be true, and asserting it would mean asserting something false with a wide tolerance.
	const dr::Point pole{0.9f, -4.0f}; // Nyquist/16
	const auto notchZero = dr::zeroPlacementFor(dr::Type::Notch, pole);
	const auto lpfZero = dr::zeroPlacementFor(dr::Type::Lowpass, pole);
	REQUIRE(notchZero.has_value());
	REQUIRE(lpfZero.has_value());

	dr::Band notchBand;
	notchBand.pole = pole;
	notchBand.zero = *notchZero;
	dr::Band lpfBand;
	lpfBand.pole = pole;
	lpfBand.zero = *lpfZero;

	// A 1D MORPH EMBEDDED IN THE 2D PUCK: the notch on the whole x = 0 edge, the lowpass on the whole
	// x = 1 edge. So the blend must not depend on y, and that is asserted rather than assumed.
	std::array<dr::Frame, dr::kFrames> corners{};
	corners[dr::X0Y0] = frameWithOneBand(notchBand);
	corners[dr::X0Y1] = frameWithOneBand(notchBand);
	corners[dr::X1Y0] = frameWithOneBand(lpfBand);
	corners[dr::X1Y1] = frameWithOneBand(lpfBand);

	SUBCASE("the blend does not depend on y when the x edges agree") {
		for (int ix = 0; ix <= 8; ++ix) {
			const float x = static_cast<float>(ix) / 8.0f;
			const dr::Frame a = dr::blend(corners, x, 0.0f);
			const dr::Frame b = dr::blend(corners, x, 1.0f);
			CAPTURE(x);
			CHECK(a.band[0].zero.angleLog2 == doctest::Approx(b.band[0].zero.angleLog2).epsilon(1e-7));
		}
	}

	SUBCASE("the blended zero is the value the log coordinate says it is") {
		// AN INDEPENDENT RESTATEMENT: the stored coordinate is a log, so the blended FREQUENCY is the
		// geometric interpolation of the two corners' frequencies. Written in the frequency domain
		// here, on purpose — the code lerps the log, so agreeing in nu is a real second opinion.
		const double nuNotch = std::exp2(static_cast<double>(notchZero->angleLog2));
		const double nuLpf = std::exp2(static_cast<double>(lpfZero->angleLog2));
		for (int ix = 0; ix <= 8; ++ix) {
			const float t = static_cast<float>(ix) / 8.0f;
			const dr::Frame f = dr::blend(corners, t, 0.5f);
			const double want = std::pow(nuNotch, 1.0 - t) * std::pow(nuLpf, t);
			CAPTURE(t);
			CHECK(std::exp2(static_cast<double>(f.band[0].zero.angleLog2))
			      == doctest::Approx(want).epsilon(1e-6));
			// Both corners put the zero ON the circle, so every blend does too.
			CHECK(f.band[0].zero.radius == doctest::Approx(1.0).epsilon(1e-7));
			// The pole is the same at both corners, so it does not move at all.
			CHECK(f.band[0].pole == pole);
		}
	}

	SUBCASE("below the notch, the midpoint magnitude lies strictly between the two corners") {
		// WHY THIS IS TRUE AND NOT MERELY PLAUSIBLE: with rz = 1 the numerator magnitude is
		// 2|cos w - cos theta_z|, and for w below every theta_z in play that is 2(cos w - cos theta_z),
		// which increases monotonically as theta_z increases. The blended zero's angle is between the
		// two corners' angles, so its magnitude is between theirs. The shared pole cancels.
		const double nuNotch = std::exp2(static_cast<double>(pole.angleLog2));
		const dr::Frame mid = dr::blend(corners, 0.5f, 0.5f);
		int violations = 0;
		int probes = 0;
		for (int i = 1; i <= 24; ++i) {
			// Probe frequencies strictly BELOW the notch, where the monotonicity above holds.
			const float nu = static_cast<float>(nuNotch * (static_cast<double>(i) / 32.0));
			const double lo = dr::magnitudeAt(corners[dr::X0Y0], nu);
			const double hi = dr::magnitudeAt(corners[dr::X1Y0], nu);
			const double m = dr::magnitudeAt(mid, nu);
			++probes;
			if (!(m > lo && m < hi)) {
				++violations;
				CAPTURE(nu);
				CAPTURE(lo);
				CAPTURE(m);
				CAPTURE(hi);
			}
			// AND THE TWO CORNERS REALLY DIFFER HERE, so "between" is not a gap of zero width.
			if (!(hi > lo * 1.5)) {
				++violations;
			}
		}
		CHECK(probes == 24);
		CHECK(violations == 0);
	}

	SUBCASE("the response is continuous in the puck: halving the step halves the largest jump") {
		// CONTINUITY, MEASURED RATHER THAN ASSERTED. A Lipschitz function sampled at step h has a
		// largest consecutive difference proportional to h, so the ratio between step h and step h/2
		// is about 2. That is a claim a discontinuity cannot satisfy: a jump does not shrink.
		auto largestJump = [&](int steps) {
			double worst = 0.0;
			std::vector<double> previous;
			for (int ix = 0; ix <= steps; ++ix) {
				const float x = static_cast<float>(ix) / static_cast<float>(steps);
				const dr::Frame f = dr::blend(corners, x, 0.5f);
				std::vector<double> current;
				current.reserve(96);
				for (int i = 0; i < 96; ++i) {
					const float nu = static_cast<float>(std::exp2(-11.0 + 11.0 * i / 95.0));
					current.push_back(dr::magnitudeAt(f, nu));
				}
				if (!previous.empty()) {
					for (size_t i = 0; i < current.size(); ++i) {
						worst = std::max(worst, std::fabs(current[i] - previous[i]));
					}
				}
				previous = std::move(current);
			}
			return worst;
		};
		const double coarse = largestJump(64);
		const double fine = largestJump(128);
		const double finer = largestJump(256);
		CAPTURE(coarse);
		CAPTURE(fine);
		CAPTURE(finer);
		CHECK(coarse > 0.0); // the sweep does something, so the shrinking below is not shrinking zero
		CHECK(fine < coarse * 0.75);
		CHECK(finer < fine * 0.75);
	}
}

//==================================================================================================
// 7. THE SHAPE OF THE STATE — what phase 2's descriptor will be written against.
//==================================================================================================

TEST_CASE("a band is five fields, and the budget has a band-sized block to spare") {
	// PLAN §5.1: `4 frames x N bands x <fields> + globals <= kMaxDeviceFields`. It was four floats and
	// 106 of 128; **the stored type made it five and 127 of 128** — types stick, and Free releases
	// the constraint. **THE SLOT'S MIX AND WET GAIN MADE IT 129, AND THE CARRIER
	// WENT TO 160 FOR THEM** (backlog NR). **IT WENT TO 256 ON 2026-10-31** (Pattern and the
	// Rack's chain slots), and TO 320 ON 2026-11-06 (Pattern's per-tab Span/Fill), so there are 191 fields of headroom: seven
	// more bands' worth of fields (24 each) fit, an eighth does not.
	//
	// EXPRESSED THROUGH `kBandFields` RATHER THAN A LITERAL, because the literal 4 appeared here and
	// in `Param::kCount` and the two would have drifted the moment a band grew a field.
	CHECK(sizeof(dr::Point) == 2 * sizeof(float));
	CHECK(dr::kBandFields == 5);
	CHECK(sizeof(dr::Band) == dr::kBandFields * sizeof(float));
	CHECK(sizeof(dr::Frame) == dr::kBands * dr::kBandFields * sizeof(float));
	CHECK(dr::kBands == 6);
	CHECK(dr::kFrames == 4);
	const size_t fields = dr::kFrames * dr::kBands * dr::kBandFields + 7 + 2;
	CHECK(fields == 129);
	CHECK(fields <= feathers::device::kMaxDeviceFields);
	CHECK(feathers::device::kMaxDeviceFields == 320);
	CHECK(feathers::device::kMaxDeviceFields - fields == 191);
}

TEST_CASE("sanitise clamps a pole into the stable set and leaves a zero its outside-the-circle freedom") {
	dr::Point pole{5.0f, 3.0f};
	dr::sanitisePole(pole);
	CHECK(pole.radius == dr::kMaxPoleRadius);
	CHECK(pole.angleLog2 == dr::kMaxAngleLog2);
	CHECK(dr::poleIsStable(pole));

	dr::Point below{-1.0f, -40.0f};
	dr::sanitisePole(below);
	CHECK(below.radius == 0.0f); // NO SPLIT-REAL BRANCH FOR A POLE — see drawn_types.cpp
	CHECK(below.angleLog2 == dr::kMinAngleLog2);

	// A ZERO MAY SIT OUTSIDE THE CIRCLE (allpass) AND MAY BE NEGATIVE (bandpass); the only bound is
	// what the coordinate can hold.
	dr::Point zero{-1.0f, -1.0f};
	dr::sanitiseZero(zero);
	CHECK(zero.radius == -1.0f);
	dr::Point mirror{4.0f, -3.0f};
	dr::sanitiseZero(mirror);
	CHECK(mirror.radius == 4.0f);
	dr::Point runaway{1000.0f, -3.0f};
	dr::sanitiseZero(runaway);
	CHECK(runaway.radius == dr::kMaxZeroRadius);
}

TEST_CASE("a pole realises INSIDE the unit circle, and Nyquist is where it did not") {
	// **THE REPRESENTATION SAYS A POLE IS AT RADIUS r. THE REALISED COEFFICIENTS DID NOT AGREE.**
	//
	// A pair stored `(r, theta)` with `r >= 0` means a CONJUGATE pair, and the quadratic
	// `1 + a1 z^-1 + a2 z^-2` has complex roots exactly when `a1^2 <= 4 a2`. Those two coefficients
	// are rounded independently — `a1 = -2 r cos(theta)`, `a2 = fl(r * r)` — so the inequality is not
	// automatic. Where it fails the pair becomes two REAL roots, the larger at
	// `r + sqrt(r^2 - fl(r^2))`, and for r near 1 that square root is about **1e-4** — the same order
	// as the whole margin `kMaxPoleRadius` leaves.
	//
	// MEASURED BEFORE THE FIX, at `kMaxPoleRadius` and `kMaxAngleLog2`:
	//
	//     |z|                       exactly 1.0        a pole ON the unit circle
	//     denominator at Nyquist    exactly 0          so |H(Nyquist)| was INFINITY
	//
	// and at every other angle 0.99989998, which is the conjugate branch behaving correctly. The
	// trigger is `|cos theta| == 1` exactly, and there is exactly ONE reachable angle where
	// `std::cos` returns that: `kMaxAngleLog2`. `kMinAngleLog2` is `2^-12` of Nyquist, where cos is
	// 0.999999702 and the discriminant stays negative.
	//
	// **NEITHER Res NOR THE BLEND CONTRIBUTES**, which is worth saying because both were suspected:
	// the measurement reproduces at `res = 0` on a single unblended band, and Res makes it no worse
	// because its warp has its fixed point ON the circle.
	//
	// THE STATE IS REACHABLE BY A HAND. `sanitisePole` pulls anything out of range to exactly the cap
	// and exactly Nyquist, which is the top-right corner of the phase-4 editor's plane.
	const auto rootMagnitude = [](float a1f, float a2f) {
		const double a1 = static_cast<double>(a1f);
		const double a2 = static_cast<double>(a2f);
		const double disc = a1 * a1 - 4.0 * a2;
		if (disc < 0.0) {
			return std::sqrt(std::max(0.0, a2));
		}
		const double q = std::sqrt(disc);
		return std::max(std::fabs((-a1 + q) * 0.5), std::fabs((-a1 - q) * 0.5));
	};
	const auto realisedRoot = [&](float radius, float angleLog2) {
		dr::Band band;
		band.pole = dr::Point { radius, angleLog2 };
		band.zero = dr::Point { 0.5f, -4.0f };
		dr::sanitise(band);
		const dr::Biquad c = dr::coefficientsOf(band);
		return rootMagnitude(c.a1, c.a2);
	};

	SUBCASE("the worst case is the cap at Nyquist, and it is strictly inside") {
		dr::Point pole { 5.0f, 9.0f };
		dr::sanitisePole(pole);
		// THE FIXTURE IS THE REACHABLE STATE, not a number typed here.
		REQUIRE(pole.radius == dr::kMaxPoleRadius);
		REQUIRE(pole.angleLog2 == dr::kMaxAngleLog2);
		const double worst = realisedRoot(pole.radius, pole.angleLog2);
		CAPTURE(worst);
		CHECK(worst < 1.0);
		// AND BY THE MARGIN THE CONSTANT PROMISES, not by an epsilon. MEASURED 0.99990000820921021,
		// which is `kMaxPoleRadius` back to seven figures — the point of the fix is that the realised
		// radius IS the stored one.
		CHECK(worst == doctest::Approx(static_cast<double>(dr::kMaxPoleRadius)).epsilon(1e-6));
	}

	SUBCASE("across every reachable angle and radius, nothing reaches the circle") {
		double worst = 0.0;
		float worstAngle = 0.0f;
		float worstRadius = 0.0f;
		// The representable radii at and just below the cap — where the margin is thinnest — and a
		// fine sweep of angle, because the defect lived at ONE angle and a coarse grid steps over it.
		std::vector<float> radii;
		float r = dr::kMaxPoleRadius;
		for (int i = 0; i < 64; ++i) {
			radii.push_back(r);
			r = std::nextafter(r, 0.0f);
		}
		for (const float extra : { 0.999f, 0.99f, 0.9f, 0.5f, 0.0f }) {
			radii.push_back(extra);
		}
		for (const float radius : radii) {
			for (int ia = 0; ia <= 512; ++ia) {
				const float angle =
				    dr::kMinAngleLog2
				    + (dr::kMaxAngleLog2 - dr::kMinAngleLog2) * static_cast<float>(ia) / 512.0f;
				const double m = realisedRoot(radius, angle);
				if (m > worst) {
					worst = m;
					worstAngle = angle;
					worstRadius = radius;
				}
			}
		}
		CAPTURE(worst);
		CAPTURE(worstAngle);
		CAPTURE(worstRadius);
		CHECK(worst < 1.0);
		// AND THE SWEEP REALLY VISITED THE THIN PLACE — otherwise "nothing reached the circle" is a
		// statement about a grid that stepped over the only angle where it could.
		CHECK(worstAngle == dr::kMaxAngleLog2);
	}

	SUBCASE("the response at Nyquist is FINITE there, which is the same fact from the other side") {
		// A ROOT AT |z| = 1 AND A ZERO DENOMINATOR AT THAT FREQUENCY ARE ONE STATEMENT. This asserts
		// the second, because it is the one the rest of the device actually trips over: `realise`
		// normalises each section by its edge magnitudes, and an INFINITE edge is what silently made
		// the cascade trim decide it had nothing to do.
		dr::Band band;
		band.pole = dr::Point { dr::kMaxPoleRadius, dr::kMaxAngleLog2 };
		band.zero = dr::Point { 0.5f, -4.0f };
		const dr::Biquad c = dr::coefficientsOf(band);
		const double denNyquist = 1.0 - static_cast<double>(c.a1) + static_cast<double>(c.a2);
		CAPTURE(denNyquist);
		CHECK(denNyquist > 0.0);
		CHECK(std::isfinite(dr::magnitudeAt(band, 1.0f)));
		// NOT VACUOUS: the magnitude there is still enormous, because the pole really is 1e-4 from the
		// circle at that frequency. Finite is the claim, not small.
		CHECK(dr::magnitudeAt(band, 1.0f) > 100.0f);
	}

	SUBCASE("and Bypass is still exactly flat, which is what the fix had to not break") {
		// THE GUARD IS APPLIED TO BOTH PAIRS SO THAT A ZERO ON ITS POLE STILL GIVES `b == a` BIT FOR
		// BIT. Applying it to the pole alone would have separated every bypassed band by a ulp and
		// made the flat default quietly non-flat — which no assertion about pole radii would have
		// caught. Asked at the exact place the guard fires.
		dr::Band band;
		band.pole = dr::Point { dr::kMaxPoleRadius, dr::kMaxAngleLog2 };
		band.zero = band.pole;
		const dr::Biquad c = dr::coefficientsOf(band);
		CHECK(c.b1 == c.a1);
		CHECK(c.b2 == c.a2);
		for (int i = 0; i <= 64; ++i) {
			CHECK(dr::magnitudeAt(band, static_cast<float>(i) / 64.0f) == 1.0f);
		}
	}

	SUBCASE("and a Bandpass keeps its split-real zeros, which the guard must not touch") {
		// THE OTHER THING THE GUARD COULD HAVE BROKEN. A negative zero radius is the split-real branch
		// and `b1^2 > 4 b2` is its DEFINITION rather than a rounding failure, so the guard leaves any
		// negative `c2` alone. Without that carve-out a Bandpass would silently become a conjugate
		// pair and stop being a bandpass.
		const auto placement = dr::zeroPlacementFor(dr::Type::Bandpass, dr::Point { 0.9f, -3.0f });
		REQUIRE(placement.has_value());
		REQUIRE(placement->radius < 0.0f);
		dr::Band band;
		band.pole = dr::Point { 0.9f, -3.0f };
		band.zero = *placement;
		const dr::Biquad c = dr::coefficientsOf(band);
		CHECK(c.b2 < 0.0f);
		CHECK(static_cast<double>(c.b1) * c.b1 > 4.0 * static_cast<double>(c.b2));
	}
}

TEST_CASE("sanitise pulls a whole settings block into range, puck included") {
	dr::Settings s;
	s.morphX = 4.0f;
	s.morphY = -2.0f;
	s.frame[dr::X1Y1].band[3].pole = dr::Point{7.0f, 9.0f};
	dr::sanitise(s);
	CHECK(s.morphX == 1.0f);
	CHECK(s.morphY == 0.0f);
	CHECK(dr::poleIsStable(s.frame[dr::X1Y1].band[3].pole));
	for (const auto& frame : s.frame) {
		for (const auto& band : frame.band) {
			CHECK(dr::poleIsStable(band.pole));
			CHECK(band.pole.angleLog2 >= dr::kMinAngleLog2);
			CHECK(band.pole.angleLog2 <= dr::kMaxAngleLog2);
		}
	}
}

//==================================================================================================
// PHASE 2 — THE GLOBALS
//==================================================================================================

namespace {

dr::Frame frameOfOneBand(const dr::Band& band) {
	dr::Frame f;
	f.band[0] = band;
	return f;
}

dr::Band typedBand(dr::Type type, const dr::Point& pole) {
	dr::Band band;
	band.pole = pole;
	const auto zero = dr::zeroPlacementFor(type, pole);
	REQUIRE(zero.has_value());
	band.zero = *zero;
	return band;
}

} // namespace

TEST_CASE("Cutoff is an addition in the log coordinate, and it moves the pole and the zero together") {
	// THE PAYOFF OF STORING THE LOG. Sliding the drawn shape N octaves is `angleLog2 += N`, so the
	// internal spacing survives by construction rather than by care.
	const dr::Frame before = bypassedFrame(0.4f);
	for (const float octaves : {-3.0f, -1.0f, 0.0f, 0.5f, 2.0f}) {
		CAPTURE(octaves);
		const dr::Frame after = dr::withGlobals(before, octaves, 0.0f);
		for (size_t b = 0; b < dr::kBands; ++b) {
			const float want = std::clamp(before.band[b].pole.angleLog2 + octaves, dr::kMinAngleLog2,
			                              dr::kMaxAngleLog2);
			CHECK(after.band[b].pole.angleLog2 == doctest::Approx(want).epsilon(1e-7));
			// AND THE ZERO MOVED BY EXACTLY THE SAME AMOUNT — bit for bit, which is what keeps a
			// Bypass band bypassed and a Notch's null on its pole.
			CHECK(after.band[b].zero.angleLog2 == after.band[b].pole.angleLog2);
		}
		// The spacing between adjacent bands is preserved except where the clamp bites.
		if (octaves == 0.5f) {
			for (size_t b = 1; b < dr::kBands; ++b) {
				CHECK(after.band[b].pole.angleLog2 - after.band[b - 1].pole.angleLog2
				      == doctest::Approx(1.5).epsilon(1e-6));
			}
		}
	}
}

TEST_CASE("Res sharpens the pole with its fixed point ON the unit circle, and that is what keeps the types intact") {
	// THE FINDING THIS CASE EXISTS FOR: plan §3c says Res scales "every POLE's radius". Doing only
	// that BREAKS Bypass — a bypassed band is the zero sitting on the pole, so moving one and not the
	// other turns a transparent band into a resonance, and raising Res on a DEFAULT patch would
	// switch six of them on. The warp is therefore applied to poles AND zeros, and its fixed point at
	// |r| = 1 is what lets that be safe.
	for (int i = 0; i <= 10; ++i) {
		const float res = static_cast<float>(i) / 10.0f;
		const float k = 1.0f - res * dr::kResDepth;
		CAPTURE(res);

		SUBCASE("") {
			// THE POLE MOVES BY THE STATED WARP — an independent restatement, not a second call.
			const dr::Frame before = bypassedFrame(0.4f);
			const dr::Frame after = dr::withGlobals(before, 0.0f, res);
			for (size_t b = 0; b < dr::kBands; ++b) {
				const float want = 1.0f - (1.0f - before.band[b].pole.radius) * k;
				CHECK(after.band[b].pole.radius == doctest::Approx(want).epsilon(1e-6));
				// AND BYPASS SURVIVES IT, bit for bit.
				CHECK(after.band[b].zero == after.band[b].pole);
			}
			// The realised cascade is still EXACTLY flat — the property the whole argument is about.
			const dr::Realised r = dr::realise(after);
			for (int f = 0; f <= 64; ++f) {
				CHECK(dr::magnitudeAt(r, static_cast<float>(f) / 64.0f) == 1.0f);
			}
		}
	}

	SUBCASE("a Notch's zero stays exactly on the circle at every Res") {
		// |r| = 1 IS THE WARP'S FIXED POINT, so an on-circle zero cannot be dragged off it — which is
		// what stops Res from turning every notch into a dip.
		const dr::Frame before = frameOfOneBand(typedBand(dr::Type::Notch, dr::Point{0.7f, -4.0f}));
		for (int i = 0; i <= 10; ++i) {
			const float res = static_cast<float>(i) / 10.0f;
			CAPTURE(res);
			const dr::Frame after = dr::withGlobals(before, 0.0f, res);
			CHECK(after.band[0].zero.radius == 1.0f);
			// still a true null, and still at the pole's angle
			CHECK(after.band[0].zero.angleLog2 == after.band[0].pole.angleLog2);
			// AS A RATIO, NOT AS "APPROXIMATELY ZERO". At full Res the pole sits so close to the
			// circle that the denominator at the null is ~1e-5, which amplifies the last bit of the
			// numerator's cancellation into an absolute reading of 1e-5 — a number that says nothing
			// on its own. Against the response an octave away it is 1e-6 of it, which is a null.
			const float nu = std::exp2(after.band[0].pole.angleLog2);
			const double atNull = dr::magnitudeAt(after.band[0], nu);
			const double away = dr::magnitudeAt(after.band[0], nu * 2.0f);
			CAPTURE(atNull);
			CAPTURE(away);
			CHECK(atNull / away < 1e-4);
		}
	}

	SUBCASE("a Bandpass's split-real zero keeps its sign and its magnitude") {
		// THE WARP ACTS ON THE MAGNITUDE AND CARRIES THE SIGN, so a bandpass is not dragged onto the
		// conjugate branch. |−1| is the fixed point, so it does not move at all.
		const dr::Frame before = frameOfOneBand(typedBand(dr::Type::Bandpass, dr::Point{0.7f, -4.0f}));
		for (int i = 0; i <= 10; ++i) {
			const dr::Frame after = dr::withGlobals(before, 0.0f, static_cast<float>(i) / 10.0f);
			CHECK(after.band[0].zero.radius == -1.0f);
		}
	}

	SUBCASE("and Res never makes a pole unstable, which is why no clamp is needed") {
		// THE WARP IS A CONTRACTION TOWARD 1 OF A RADIUS ALREADY BELOW 1, so it cannot reach the
		// circle. Asserted over the worst case there is: the ceiling radius, at full Res.
		std::mt19937 rng(2718u);
		double worst = 0.0;
		int unstable = 0;
		for (int trial = 0; trial < 20000; ++trial) {
			dr::Point pole = randomPole(rng);
			if (trial % 7 == 0) {
				pole.radius = dr::kMaxPoleRadius; // the ceiling itself, not merely near it
			}
			dr::Frame f = frameOfOneBand(dr::Band{pole, pole});
			const float res = static_cast<float>(trial % 101) / 100.0f;
			const dr::Frame after = dr::withGlobals(f, 0.0f, res);
			worst = std::max(worst, static_cast<double>(after.band[0].pole.radius));
			if (!dr::poleIsStable(after.band[0].pole)) {
				++unstable;
			}
		}
		CAPTURE(worst);
		CHECK(unstable == 0);
		CHECK(worst < 1.0);
		// It really did get close, so the silence above means something. MEASURED: 0.99999 at the
		// ceiling radius and full Res, which is 1e-5 of margin against the blend's 1e-7 error.
		CHECK(worst > 0.9999);
	}
}

//==================================================================================================
// PHASE 2 — REALISATION AND NORMALISATION
//==================================================================================================

TEST_CASE("the closed-form peak agrees with a dense search of the response") {
	// THE CLOSED FORM IS A SAFETY BOUND, so it is checked against the thing it replaces rather than
	// trusted. A dense search is the independent restatement; the closed form must never come in
	// BELOW it, because under-reading the peak is the direction that lets a cascade through.
	std::mt19937 rng(161803u);
	double worstUnder = 0.0;
	double worstRelative = 0.0;
	for (int trial = 0; trial < 3000; ++trial) {
		dr::Band band;
		band.pole = randomPole(rng);
		band.zero = randomZero(rng);
		const dr::Biquad c = dr::coefficientsOf(band);
		const double closed = dr::peakMagnitude(c);

		double searched = 0.0;
		for (int i = 0; i <= 20000; ++i) {
			searched = std::max(searched,
			                    static_cast<double>(dr::magnitudeAt(band, static_cast<float>(i) / 20000.0f)));
		}
		const double denom = std::max(1e-9, searched);
		worstRelative = std::max(worstRelative, std::fabs(closed - searched) / denom);
		worstUnder = std::max(worstUnder, (searched - closed) / denom);
	}
	CAPTURE(worstRelative);
	CAPTURE(worstUnder);
	// The dense search misses the very top of a high-Q peak, so the closed form is allowed to be
	// ABOVE it and barely below it.
	CHECK(worstUnder < 1e-3);
	CHECK(worstRelative < 0.2);
}

TEST_CASE("the normalisation gives each familiar type unity where that type passes") {
	SUBCASE("Bypass is exactly one, and its gain is exactly one") {
		const dr::Realised r = dr::realise(bypassedFrame(0.5f));
		CHECK(r.trim == 1.0f);
		for (const auto& s : r.section) {
			CHECK(s.gain == 1.0f);
		}
		for (int i = 0; i <= 64; ++i) {
			CHECK(dr::magnitudeAt(r, static_cast<float>(i) / 64.0f) == 1.0f);
		}
	}

	SUBCASE("a Lowpass passes DC at unity") {
		const dr::Frame f = frameOfOneBand(typedBand(dr::Type::Lowpass, dr::Point{0.7f, -4.0f}));
		const dr::Realised r = dr::realise(f);
		CHECK(dr::magnitudeAt(r, 0.0f) == doctest::Approx(1.0).epsilon(1e-4));
		CHECK(dr::magnitudeAt(r, 1.0f) < 1e-4); // and it still stops Nyquist
	}

	SUBCASE("a Highpass passes Nyquist at unity") {
		const dr::Frame f = frameOfOneBand(typedBand(dr::Type::Highpass, dr::Point{0.7f, -4.0f}));
		const dr::Realised r = dr::realise(f);
		CHECK(dr::magnitudeAt(r, 1.0f) == doctest::Approx(1.0).epsilon(1e-4));
		CHECK(dr::magnitudeAt(r, 0.0f) < 1e-3);
	}

	SUBCASE("an Allpass is flat at unity, which the unnormalised one is not") {
		const dr::Point pole{0.7f, -4.0f};
		const dr::Frame f = frameOfOneBand(typedBand(dr::Type::Allpass, pole));
		// Unnormalised it sits at 1/r^2 — the phase-1 case asserts exactly that — so this is the
		// normalisation doing visible work rather than agreeing with what was already there.
		CHECK(dr::magnitudeAt(f.band[0], 0.5f)
		      == doctest::Approx(1.0 / (0.7 * 0.7)).epsilon(1e-4));
		const dr::Realised r = dr::realise(f);
		for (int i = 0; i <= 32; ++i) {
			CAPTURE(i);
			CHECK(dr::magnitudeAt(r, static_cast<float>(i) / 32.0f) == doctest::Approx(1.0).epsilon(1e-4));
		}
	}

	SUBCASE("a Bandpass is null at BOTH edges, so it is normalised to the peak instead") {
		// THE CASE THAT KILLS EDGE-ONLY NORMALISATION. |H| is zero at DC and at Nyquist, so dividing
		// by the edge would divide by zero; the peak floor is what catches it.
		const dr::Frame f = frameOfOneBand(typedBand(dr::Type::Bandpass, dr::Point{0.7f, -4.0f}));
		const dr::Realised r = dr::realise(f);
		double peak = 0.0;
		for (int i = 0; i <= 4000; ++i) {
			peak = std::max(peak, static_cast<double>(dr::magnitudeAt(r, static_cast<float>(i) / 4000.0f)));
		}
		CAPTURE(peak);
		CHECK(std::isfinite(peak));
		// Normalised to kPeakHeadroom, because the edge is zero and the clamp's floor is peak/4.
		CHECK(peak == doctest::Approx(static_cast<double>(dr::kPeakHeadroom)).epsilon(0.02));
	}
}

TEST_CASE("six resonant bands cannot exceed the cascade ceiling, and the trim is what stops them") {
	auto peakOf = [](const dr::Realised& r) {
		double peak = 0.0;
		for (int i = 0; i <= 200000; ++i) {
			peak = std::max(peak, static_cast<double>(dr::magnitudeAt(r, static_cast<float>(i) / 200000.0f)));
		}
		return peak;
	};

	SUBCASE("six resonances STACKED on one frequency, which is the dangerous case") {
		// SIX IDENTICAL BANDPASSES AT FULL RESONANCE. Stacked, the cascade's true peak IS the product
		// of the sections' peaks, so this is where the bound the trim is computed from is tight and
		// where the trim has to be exactly right rather than merely conservative.
		dr::Frame f;
		for (size_t b = 0; b < dr::kBands; ++b) {
			f.band[b] = typedBand(dr::Type::Bandpass, dr::Point{dr::kMaxPoleRadius, -4.0f});
		}
		const dr::Realised r = dr::realise(dr::withGlobals(f, 0.0f, 1.0f));
		const double peak = peakOf(r);
		CAPTURE(peak);
		CAPTURE(r.trim);
		CHECK(std::isfinite(peak));
		CHECK(peak <= static_cast<double>(dr::kCascadeCeiling) + 1e-3);

		// AND THE TRIM IS NOT VACUOUS: dividing it back out puts the same cascade far above the
		// ceiling. Computed by division rather than by rebuilding the frame, so the two numbers
		// cannot drift apart.
		CHECK(r.trim < 1.0f);
		CHECK(peak / static_cast<double>(r.trim) > static_cast<double>(dr::kCascadeCeiling));
	}

	SUBCASE("six resonances SPREAD across the spectrum, where the bound is conservative") {
		// THE SAME SIX BANDS AT SIX DIFFERENT FREQUENCIES. The bound the trim uses is the PRODUCT of
		// the sections' peaks, and six peaks at six frequencies never coincide — each band's null
		// sits on its neighbours' peaks — so the true cascade peak is orders below the bound and the
		// trim over-attenuates. **That is the deliberate direction for a safety limit** and it is
		// asserted rather than left as a surprise: the ceiling still holds, and it holds with room.
		dr::Frame f;
		for (size_t b = 0; b < dr::kBands; ++b) {
			f.band[b] = typedBand(dr::Type::Bandpass,
			                      dr::Point{dr::kMaxPoleRadius, -9.0f + 1.5f * static_cast<float>(b)});
		}
		const dr::Realised r = dr::realise(dr::withGlobals(f, 0.0f, 1.0f));
		const double peak = peakOf(r);
		CAPTURE(peak);
		CAPTURE(r.trim);
		CHECK(std::isfinite(peak));
		CHECK(peak <= static_cast<double>(dr::kCascadeCeiling) + 1e-3);
		// THE TRIM NO LONGER FIRES HERE, AND THAT IS THE FIX RATHER THAN A REGRESSION. This subcase
		// was written to document the bound's conservatism as deliberate — "the true cascade peak is
		// orders below the bound and the trim over-attenuates" — and in a host that showed as lost
		// volume. `realise` now sweeps the cascade, so six bandpasses whose nulls sit on each other's
		// peaks are recognised as quiet instead of assumed loud. MEASURED here: the true peak is
		// ~7e-19, so no trim is the correct answer and 13.3 dB is no longer thrown away on the
		// six-lowpass arrangement that prompted the report.
		CHECK(r.trim == doctest::Approx(1.0f).epsilon(1e-4));

		// THE NON-VACUITY IS ABOUT THE BOUND, NOT ABOUT THE RENDERED PEAK, because the bound is what
		// the trim WOULD have been computed from. Restated here from the sections themselves.
		double product = 1.0;
		for (const auto& s : r.section) {
			product *= static_cast<double>(dr::peakMagnitude(s.c)) * static_cast<double>(s.gain);
		}
		CAPTURE(product);
		CHECK(product > static_cast<double>(dr::kCascadeCeiling));
	}

	SUBCASE("and a tame frame is left alone") {
		CHECK(dr::realise(bypassedFrame(0.5f)).trim == 1.0f);
	}
}

//==================================================================================================
// PHASE 2 — THE ROW
//==================================================================================================

TEST_CASE("the row is the settings, and a row of declared defaults is a default device") {
	// THE AGREEMENT THAT NOTHING ELSE CHECKS: `Settings::defaultFrames()` spreads the six handles
	// across the spectrum and `kParamDescs` repeats those angles as declared defaults. If the two
	// disagree, an ABSENT field decodes to a different device than a default-constructed one — the
	// exact failure device_row.h is written about, and invisible from either side alone.
	dr::Row defaults {};
	for (size_t i = 0; i < defaults.size(); ++i) {
		defaults[i] = static_cast<float>(dr::kParamDescs[i].def);
	}
	CHECK(dr::stateOf(defaults, defaults.size()) == dr::Settings{});

	// AND AN EMPTY ROW IS THE SAME DEVICE, which is the rule stated the other way round: an absent
	// field is at its DECLARED default, never at zero.
	CHECK(dr::stateOf(dr::Row{}, 0) == dr::Settings{});

	SUBCASE("and a drawn state round-trips through the row") {
		std::mt19937 rng(90210u);
		dr::Settings s;
		s.enabled = true;
		s.morphX = 0.3f;
		s.morphY = 0.8f;
		s.cutoff = 1.25f;
		s.res = 0.4f;
		s.drive = 0.6f;
		for (auto& frame : s.frame) {
			for (auto& band : frame.band) {
				band.pole = randomPole(rng);
				band.zero = randomZero(rng);
			}
		}
		dr::sanitise(s);
		CHECK(dr::stateOf(dr::rowOf(s), dr::Row{}.size()) == s);
	}
}

TEST_CASE("DRAWN's row has all its fields: seven globals, the slot's Mix and wet Gain, and 120 band fields, inside the carrier") {
	// PLAN §5.1's arithmetic, now that the fields exist, plus the chain furniture (backlog NR, 2026-10-28).
	CHECK(static_cast<size_t>(dr::Param::kCount) == 7 + 2 + dr::kFrames * dr::kBands * dr::kBandFields);
	CHECK(static_cast<size_t>(dr::Param::kCount) == 129);
	// IT DOES NOT FIT 128, which is why the carrier went to 160 — asserted rather than left to a commit message.
	CHECK(static_cast<size_t>(dr::Param::kCount) > 128);
	CHECK(static_cast<size_t>(dr::Param::kCount) <= feathers::device::kMaxDeviceFields);
	CHECK(std::size(dr::kParamDescs) == static_cast<size_t>(dr::Param::kCount));
	// THE FURNITURE IS DECLARED BY ROLE, at its own indices, with the identity defaults.
	CHECK(dr::kTraits.mixControl == static_cast<int>(dr::Param::Mix));
	CHECK(dr::kTraits.wetGainControl == static_cast<int>(dr::Param::WetGain));
	CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::Mix)].def == 1.0);
	CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::WetGain)].def == 0.5);
	CHECK(dr::rowOf(dr::Settings {})[static_cast<size_t>(dr::Param::Mix)] == 1.0f);
	CHECK(dr::rowOf(dr::Settings {})[static_cast<size_t>(dr::Param::WetGain)] == 0.5f);

	SUBCASE("every declared name is distinct, which a flat host parameter list requires") {
		std::vector<std::string> names;
		names.reserve(std::size(dr::kParamDescs));
		for (const auto& d : dr::kParamDescs) {
			names.emplace_back(d.name);
		}
		std::sort(names.begin(), names.end());
		CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
		CHECK(names.size() == 129);
	}

	SUBCASE("only the controls that mean something at render time are modulatable") {
		size_t modulatable = 0;
		for (const auto& d : dr::kParamDescs) {
			modulatable += d.modulatable ? 1u : 0u;
		}
		// MorphX, MorphY, Cutoff, Res, Drive and Wake — the six globals that are AMOUNTS — and the slot's Mix and wet
		// Gain, which the slot reads. **NOT the band fields**: a drawn pole is a shape you author, not a quantity
		// something else should be moving.
		CHECK(modulatable == 9); // + Enabled, a gate (backlog KW)
		CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::MorphX)].modulatable);
		CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::MorphY)].modulatable);
		CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::Wake)].modulatable);
		CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::Mix)].modulatable);
		CHECK(dr::kParamDescs[static_cast<size_t>(dr::Param::WetGain)].modulatable);
		// AND NOT ONE BAND FIELD, asked of the whole range rather than of a sample of it.
		for (size_t i = static_cast<size_t>(dr::Param::FirstBandField);
		     i < static_cast<size_t>(dr::Param::kCount); ++i) {
			CHECK_FALSE(dr::kParamDescs[i].modulatable);
		}
	}
}

//==================================================================================================
// PHASE 2 — THE DEVICE, THROUGH ITS OPS
//==================================================================================================

namespace {

/// THE DEVICE AS THE HOST BUILDS IT: constructed into storage the caller owns, driven only through
/// `setFields`. Nothing here reaches past the ops, because the ops ARE the device's contract.
struct Ops {
	std::vector<std::byte> storage = std::vector<std::byte>(dr::kAudioOps.stateBytes);

	Ops() {
		REQUIRE(dr::kAudioOps.stateBytes <= storage.size());
		REQUIRE(reinterpret_cast<std::uintptr_t>(storage.data()) % dr::kAudioOps.stateAlign == 0);
		dr::kAudioOps.construct(storage.data());
		dr::kAudioOps.prepare(storage.data(), 48000.0, 4096);
	}
	~Ops() { dr::kAudioOps.destruct(storage.data()); }
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	void fields(const dr::Settings& s) {
		const dr::Row row = dr::rowOf(s);
		dr::kAudioOps.setFields(storage.data(), row.data(), row.size());
	}
	/// FIELDS WITH THE SMOOTHER FORGOTTEN FIRST, so the coefficients SNAP to the new state instead of
	/// ramping toward it. Needed by every case that asserts a realised value: after the first update
	/// the device deliberately ramps — that is what stops a modulated puck from zippering — so
	/// reading `realised()` straight after a second `setFields` reads the OLD state, and a case that
	/// did not know this would be asserting against a filter still on its way.
	void snap(const dr::Settings& s) {
		dr::kAudioOps.reset(storage.data());
		fields(s);
	}
	void process(float* left, float* right, int frames) {
		dr::kAudioOps.processStereo(storage.data(), left, right, frames);
	}
	[[nodiscard]] const dr::Settings& target() const { return dr::targetForTest(storage.data()); }
	/// THE COEFFICIENTS THE SAMPLE LOOP IS RUNNING — the seam the value assertions rest on, and the
	/// reason this device has one at all: a pole position is not recoverable from a buffer.
	[[nodiscard]] const dr::Realised& realised() const { return dr::realisedForTest(storage.data()); }
	[[nodiscard]] const dr::Frame& smoothed() const { return dr::smoothedFrameForTest(storage.data()); }
};

/// A DEVICE WHOSE FOUR CORNERS ARE ALL DIFFERENT AND ALL LEGAL.
dr::Settings fourDistinctFrames(std::mt19937& rng) {
	dr::Settings s;
	s.enabled = true;
	for (auto& frame : s.frame) {
		for (auto& band : frame.band) {
			band.pole = randomPole(rng);
			band.zero = randomZero(rng);
		}
	}
	dr::sanitise(s);
	return s;
}

/// Both roots of z^2 + a1 z + a2 strictly inside the unit disc — the Jury conditions, written out.
bool juryStable(const dr::Biquad& c) {
	return std::fabs(c.a2) < 1.0f && std::fabs(c.a1) < 1.0f + c.a2;
}

} // namespace

TEST_CASE("the ops target the settings they were handed, and the puck moves the realised coefficients") {
	std::mt19937 rng(1234u);
	Ops ops;
	dr::Settings s = fourDistinctFrames(rng);

	s.morphX = 0.0f;
	s.morphY = 0.0f;
	ops.snap(s);
	CHECK(ops.target().morphX == 0.0f);
	// THE VALUE, RESTATED INDEPENDENTLY: at a corner the realised cascade is that corner's frame,
	// realised. Nothing here compares two renders.
	CHECK(ops.realised() == dr::realise(dr::withGlobals(s.frame[dr::X0Y0], 0.0f, 0.0f)));

	const dr::Realised atCorner = ops.realised();
	s.morphX = 1.0f;
	s.morphY = 1.0f;
	ops.snap(s);
	CHECK(ops.realised() == dr::realise(dr::withGlobals(s.frame[dr::X1Y1], 0.0f, 0.0f)));
	// AND THE TWO REALLY DIFFER, so the equalities above are not both trivially satisfied.
	CHECK_FALSE(ops.realised() == atCorner);
}

TEST_CASE("the device blends band n from band n — it never multiplies the cascade first") {
	// THE INTEGRATOR'S REQUIREMENT, and it is a correctness property with a number behind it rather
	// than a style preference: blending each band's denominator is unconditionally stable (the Jury
	// region is a convex triangle), and blending the CASCADE's expanded polynomial is unstable for
	// about a third of random corner quadruples. Phase 1 measured both.
	std::mt19937 rng(31337u);

	SUBCASE("the value: each realised section is that band's four corners blended, and nothing else") {
		Ops ops;
		dr::Settings s = fourDistinctFrames(rng);
		s.morphX = 0.37f;
		s.morphY = 0.61f;
		ops.snap(s);
		for (size_t b = 0; b < dr::kBands; ++b) {
			CAPTURE(b);
			// BAND b OF THE FOUR FRAMES, blended on its own — the independent restatement of "per
			// band". A cascade-first implementation could not satisfy this for every b at once.
			const std::array<dr::Band, dr::kFrames> corners{
			    s.frame[dr::X0Y0].band[b], s.frame[dr::X1Y0].band[b], s.frame[dr::X0Y1].band[b],
			    s.frame[dr::X1Y1].band[b]};
			const dr::Band want = dr::blend(corners, s.morphX, s.morphY);
			CHECK(ops.smoothed().band[b] == want);
		}
	}

	SUBCASE("the behaviour: a quadruple whose CASCADE blend is unstable still realises a stable filter") {
		// A WITNESS RATHER THAN AN ARGUMENT. Search for corner frames and a puck position at which
		// the expanded degree-12 denominator blend fails Schur — the thing a cascade-first
		// implementation would run — and then assert the device is stable there anyway.
		dr::Settings witness;
		float wx = 0.0f;
		float wy = 0.0f;
		bool found = false;
		for (int trial = 0; trial < 2000 && !found; ++trial) {
			dr::Settings s = fourDistinctFrames(rng);
			std::array<Poly, dr::kFrames> expanded{};
			for (size_t k = 0; k < dr::kFrames; ++k) {
				Poly p{1.0};
				for (size_t b = 0; b < dr::kBands; ++b) {
					p = multiply(p, denominatorOf(s.frame[k].band[b].pole));
				}
				expanded[k] = p;
				REQUIRE(schurStable(p)); // every corner is a legal filter to begin with
			}
			for (int ix = 0; ix <= 10 && !found; ++ix) {
				for (int iy = 0; iy <= 10 && !found; ++iy) {
					const float x = static_cast<float>(ix) / 10.0f;
					const float y = static_cast<float>(iy) / 10.0f;
					const auto w = dr::bilinearWeights(x, y);
					Poly blended(expanded[0].size(), 0.0);
					for (size_t k = 0; k < dr::kFrames; ++k) {
						for (size_t i = 0; i < blended.size(); ++i) {
							blended[i] += static_cast<double>(w[k]) * expanded[k][i];
						}
					}
					if (!schurStable(blended)) {
						witness = s;
						wx = x;
						wy = y;
						found = true;
					}
				}
			}
		}
		// IF NO WITNESS EXISTS THE CASE PROVES NOTHING, so its absence is a failure and not a pass.
		REQUIRE(found);

		Ops ops;
		witness.morphX = wx;
		witness.morphY = wy;
		witness.enabled = true;
		ops.snap(witness);
		for (size_t b = 0; b < dr::kBands; ++b) {
			CAPTURE(b);
			CHECK(juryStable(ops.realised().section[b].c));
		}

		// AND THE AUDIO STAYS BOUNDED, which is what "unstable" would have cost. A cascade-first
		// build would ring away here rather than settle.
		std::vector<float> left(4096, 0.0f);
		std::vector<float> right(4096, 0.0f);
		left[0] = right[0] = 1.0f; // an impulse, so anything unstable grows without bound
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		float tail = 0.0f;
		for (size_t i = left.size() / 2; i < left.size(); ++i) {
			tail = std::max(tail, std::fabs(left[i]));
		}
		CAPTURE(tail);
		CHECK(std::isfinite(tail));
		CHECK(tail < 1e-2f);
	}
}

TEST_CASE("the smoother walks in the pole-zero domain, so every frame it passes through is stable") {
	// SMOOTHING IS A CONVEX COMBINATION OF TWO FRAMES, which is the blend's own argument applied to
	// time instead of to the puck: a one-pole toward a stable target never leaves the stable set.
	// Smoothing the COEFFICIENTS of a cascade would have no such guarantee.
	std::mt19937 rng(5150u);
	Ops ops;
	dr::Settings a = fourDistinctFrames(rng);
	a.morphX = 0.0f;
	a.morphY = 0.0f;
	ops.fields(a);

	dr::Settings b = a;
	b.morphX = 1.0f;
	b.morphY = 1.0f;
	b.res = 1.0f;
	ops.fields(b);

	std::vector<float> left(64, 0.0f);
	std::vector<float> right(64, 0.0f);
	int unstable = 0;
	int steps = 0;
	for (int block = 0; block < 200; ++block) {
		std::fill(left.begin(), left.end(), 0.0f);
		std::fill(right.begin(), right.end(), 0.0f);
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		++steps;
		for (const auto& band : ops.smoothed().band) {
			if (!dr::poleIsStable(band.pole)) {
				++unstable;
			}
		}
		for (const auto& s : ops.realised().section) {
			if (!juryStable(s.c)) {
				++unstable;
			}
		}
	}
	CHECK(steps == 200);
	CHECK(unstable == 0);
	// AND IT ACTUALLY ARRIVED, so the stability above is not the stability of standing still.
	const dr::Frame want = dr::withGlobals(dr::blend(b), b.cutoff, b.res);
	for (size_t i = 0; i < dr::kBands; ++i) {
		CAPTURE(i);
		CHECK(ops.smoothed().band[i].pole.radius
		      == doctest::Approx(want.band[i].pole.radius).epsilon(1e-4));
	}
}

TEST_CASE("a BYPASSED device is a wire, sample for sample") {
	// EVERY BAND IS BYPASS, EVERY GAIN IS EXACTLY 1 AND THE TRIM IS EXACTLY 1, and a transposed
	// direct form II section whose numerator equals its denominator leaves its state at zero — so
	// this is assertable as EXACT equality rather than as a tolerance, and a tolerance would accept a
	// device that quietly coloured everything.
	//
	// BUILT BYPASSED RATHER THAN TAKEN FROM THE DEFAULT, for the reason its sibling case above gives.
	// It leant on the default until the default became four different filters, at which point it
	// quietly became a case about the default patch instead of about Bypass. **The default is flat
	// again and this still builds its own**, because a case that borrows a default is a case whose
	// meaning is somebody else's to change.
	Ops ops;
	dr::Settings s;
	s.enabled = true;
	for (auto& frame : s.frame) {
		frame = bypassedFrame(0.5f);
	}
	ops.fields(s);

	std::mt19937 rng(777u);
	std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
	std::vector<float> left(512);
	std::vector<float> right(512);
	for (size_t i = 0; i < left.size(); ++i) {
		left[i] = noise(rng);
		right[i] = noise(rng);
	}
	const std::vector<float> before = left;
	ops.process(left.data(), right.data(), static_cast<int>(left.size()));
	int differed = 0;
	for (size_t i = 0; i < left.size(); ++i) {
		if (left[i] != before[i]) {
			++differed;
		}
	}
	CHECK(differed == 0);
}

TEST_CASE("a disabled device does not touch the buffer, and an enabled drawn lowpass does") {
	Ops ops;
	dr::Settings s;
	// SIX LOWPASS BANDS AN OCTAVE APART, so the stopband claim below is about a real shape.
	for (auto& frame : s.frame) {
		for (size_t b = 0; b < dr::kBands; ++b) {
			frame.band[b] = typedBand(dr::Type::Lowpass, dr::Point{0.9f, -6.0f});
		}
	}

	auto tone = [](double nu, size_t n) {
		std::vector<float> v(n);
		for (size_t i = 0; i < n; ++i) {
			v[i] = static_cast<float>(std::sin(kPi * nu * static_cast<double>(i)));
		}
		return v;
	};
	auto rms = [](const std::vector<float>& v) {
		double sum = 0.0;
		for (size_t i = v.size() / 2; i < v.size(); ++i) {
			sum += static_cast<double>(v[i]) * v[i];
		}
		return std::sqrt(sum / static_cast<double>(v.size() - v.size() / 2));
	};

	SUBCASE("disabled is a wire") {
		s.enabled = false;
		ops.fields(s);
		std::vector<float> left = tone(0.5, 2048);
		std::vector<float> right = left;
		const std::vector<float> before = left;
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		CHECK(left == before);
	}

	SUBCASE("enabled, the drawn lowpass passes what is below its corner and stops what is above") {
		s.enabled = true;
		ops.fields(s);
		// nu = 2^-6 is the corner; a tone two octaves below it and one three octaves above.
		std::vector<float> lowL = tone(std::exp2(-8.0), 8192);
		std::vector<float> lowR = lowL;
		ops.process(lowL.data(), lowR.data(), static_cast<int>(lowL.size()));

		Ops fresh;
		fresh.fields(s);
		std::vector<float> highL = tone(std::exp2(-3.0), 8192);
		std::vector<float> highR = highL;
		fresh.process(highL.data(), highR.data(), static_cast<int>(highL.size()));

		const double low = rms(lowL);
		const double high = rms(highL);
		CAPTURE(low);
		CAPTURE(high);
		// AND THE NUMBERS ARE CHECKED AGAINST THE DRAWN CURVE ITSELF rather than against a guess:
		// the closed-form response is the device's own claim about what it does, so the rendered RMS
		// has to match it. That is the assertion; "low is louder than high" is only the second one
		// proving the first is not vacuous.
		const dr::Realised r = dr::realise(dr::withGlobals(dr::blend(s), 0.0f, 0.0f));
		const double wantLow = dr::magnitudeAt(r, static_cast<float>(std::exp2(-8.0)));
		const double wantHigh = dr::magnitudeAt(r, static_cast<float>(std::exp2(-3.0)));
		const double reference = std::sqrt(0.5); // RMS of a unit sine
		CHECK(low == doctest::Approx(wantLow * reference).epsilon(0.05));
		CHECK(high == doctest::Approx(wantHigh * reference).epsilon(0.05));
		// AND THE SHAPE IS A LOWPASS RATHER THAN A GENTLE TILT — asserted about the DRAWN CURVE,
		// which is where the claim belongs, and then the rendered ratio is required to match it. The
		// first draft asserted a bare `low > high * 10` and failed at 6.1, because six poles at
		// radius 0.5 are six very broad ones: the number came from an expectation rather than from
		// the geometry.
		CHECK(wantLow / wantHigh > 20.0);
		CHECK(low / high == doctest::Approx(wantLow / wantHigh).epsilon(0.1));
	}
}

TEST_CASE("Drive is exactly the untouched signal at zero and the stated shaper above it") {
	// A BYPASSED CASCADE MAKES THE DRIVE STAGE DIRECTLY OBSERVABLE — every band is a wire, so the
	// output IS the shaper. That is the only reason a waveshaper buried in front of six biquads can
	// be asserted as a value at all.
	//
	// AND THE BYPASS IS BUILT HERE RATHER THAN TAKEN FROM THE DEFAULT. It used to be the default, and
	// when `kDefaultFrames` became four real filters this case started measuring the shaper THROUGH a
	// lowpass and a bandpass — the third case in this file to have been quietly coupled to a default
	// it never mentioned, and the one nobody predicted. That is the whole hazard: a default is not a
	// fixture, and a case that borrows one has a dependency nothing declares. **It stays built here
	// now that the default is bypassed again**, because "it would work either way" is exactly the
	// reasoning that put it here the first time.
	dr::Settings s;
	s.enabled = true;
	for (auto& frame : s.frame) {
		frame = bypassedFrame(0.5f);
	}

	std::vector<float> input(256);
	for (size_t i = 0; i < input.size(); ++i) {
		input[i] = static_cast<float>(-1.0 + 2.0 * static_cast<double>(i) / 255.0);
	}

	SUBCASE("at zero it is a wire, sample for sample") {
		Ops ops;
		s.drive = 0.0f;
		ops.fields(s);
		std::vector<float> left = input;
		std::vector<float> right = input;
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		CHECK(left == input);
	}

	SUBCASE("above zero it is multimode_filter's mapping, restated") {
		Ops ops;
		s.drive = 0.75f;
		ops.fields(s);
		std::vector<float> left = input;
		std::vector<float> right = input;
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		// THE INDEPENDENT RESTATEMENT: gain 2^(4*drive), a tanh clamped at 4.5, a compensating divide
		// by tanh(min(gain, 4.5)), AND THE BLEND BY THE AMOUNT — `x + drive * (shape(x) - x)`, which is
		// how multimode_filter.cpp's process applies its own stage (backlog EH, shape 5: drawn ran the
		// shaper at full wet, so a drive just above zero was a +2.4 dB step rather than a colour).
		// Written from the filter's refresh() and process() rather than read back out of the device.
		const double gain = std::exp2(4.0 * 0.75);
		const double comp = std::tanh(std::min(gain, 4.5));
		int wrong = 0;
		for (size_t i = 0; i < input.size(); ++i) {
			const double x = static_cast<double>(input[i]);
			const double shaped = std::tanh(std::clamp(x * gain, -4.5, 4.5)) / comp;
			const double want = x + 0.75 * (shaped - x);
			// fasttanh is an approximation of tanh, so the tolerance is the approximation's and is
			// stated rather than widened until it passes.
			if (std::fabs(static_cast<double>(left[i]) - want) > 2e-3) {
				++wrong;
			}
		}
		CHECK(wrong == 0);
		// AND IT DID SOMETHING: at 0.75 the shaper is well into compression.
		CHECK(left[0] != input[0]);
		CHECK(std::fabs(left[input.size() / 4]) > std::fabs(input[input.size() / 4]));
	}

	SUBCASE("backlog EH: the first step past zero is small — no +2.4 dB jump at the rest") {
		// THE DEFECT: at full wet, a drive of 0.0001 ran the shaper at a gain of ~1 with the peak
		// compensation 1/tanh(1), a small-signal boost of 1.313 — a step, not a colour. Blended by the
		// amount, the step is the amount's size.
		Ops ops;
		s.drive = 0.0001f;
		ops.fields(s);
		std::vector<float> left = input;
		std::vector<float> right = input;
		ops.process(left.data(), right.data(), static_cast<int>(left.size()));
		double worst = 0.0;
		for (size_t i = 0; i < input.size(); ++i) {
			worst = std::max(worst, std::fabs(static_cast<double>(left[i]) - static_cast<double>(input[i])));
		}
		CHECK(worst < 1e-3);
		// AND NOT A WIRE: the shaper did run, by the amount — some sample differs from the input.
		CHECK(left != input);
	}
}

TEST_CASE("backlog EH: a bypass clears the drawn filter's states, so re-enabling on silence is silent rather than the tail that was standing") {
	// THE MULTIMODE FILTER'S RULE ("coming back from silence with whatever it held when it was last
	// needed would ring a stale tail into the signal"), applied here: loud through the bands, one
	// bypassed block, then enabled again on SILENCE — nothing out from the first sample.
	Ops ops;
	feathers::fx::drawn::Settings s;
	s.enabled = true;
	// A DRAWN POLE NEAR THE CIRCLE, so the bands hold a tail worth hearing: the default frames are
	// FLAT — every zero on its pole, which IS bypass — and at them the device is an exact wire, so a
	// first draft of this case measured `moved == 0` and could not tell a kept state from a cleared
	// one. The same lowpass pole in all four frames, so the puck has nothing to morph between.
	for (auto& frame : s.frame) {
		frame.band[0].type = feathers::fx::drawn::Type::Lowpass;
		frame.band[0].pole.radius = 0.985f;
		frame.band[0].zero.radius = 0.0f;
	}
	ops.fields(s);
	std::vector<float> left(4096), right(4096);
	for (size_t i = 0; i < left.size(); ++i) {
		left[i] = right[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
	}
	const std::vector<float> input = left;
	ops.process(left.data(), right.data(), static_cast<int>(left.size()));
	// THE FILTER IS DOING SOMETHING AT THESE SETTINGS — the tail below is only worth clearing if there
	// is one: the last loud block differs from its input.
	float moved = 0.0f;
	for (size_t i = left.size() - 256; i < left.size(); ++i) {
		moved = std::max(moved, std::fabs(left[i] - input[i]));
	}
	CAPTURE(moved);
	REQUIRE(moved > 0.05f);
	s.enabled = false;
	ops.fields(s);
	std::fill(left.begin(), left.end(), 0.0f);
	std::fill(right.begin(), right.end(), 0.0f);
	ops.process(left.data(), right.data(), 64);
	s.enabled = true;
	ops.fields(s);
	std::fill(left.begin(), left.end(), 0.0f);
	std::fill(right.begin(), right.end(), 0.0f);
	ops.process(left.data(), right.data(), 256);
	float ring = 0.0f;
	for (size_t i = 0; i < 256; ++i) {
		ring = std::max(ring, std::max(std::fabs(left[i]), std::fabs(right[i])));
	}
	CAPTURE(ring);
	CHECK(ring < 1e-6f);
}

TEST_CASE("realising a pole into float coefficients costs a little of its frequency, and this is how much") {
	// A KNOWN LIMIT, MEASURED AND PINNED — the same kind of statement as the DC floor above, and
	// found the same way: while debugging why a closed-form peak disagreed with a search.
	//
	// A biquad's denominator holds a pole as `a1 = -2 r cos(theta)`, and at the bottom of the range
	// `cos(theta)` is within a ulp of 1, so a float `a1` cannot resolve where the pole is as finely as
	// the stored polar coordinate can. **This is an argument FOR the pole-zero storage rather than
	// against it** — the state keeps the pole exactly and only the realisation rounds — and it is a
	// caution for anyone who moves the floor down.
	//
	// MEASURED across the whole range and four resonances: the realised frequency is within 0.1
	// OCTAVES of the stored one at worst, at the very bottom (12 octaves below Nyquist, ~5.9 Hz at
	// 48 kHz), and within 0.01 octaves everywhere from 10 octaves below Nyquist upward.
	double worst = 0.0;
	double worstAbove10 = 0.0;
	float worstAngle = 0.0f;
	for (const float r : {0.5f, 0.9f, 0.99f, dr::kMaxPoleRadius}) {
		for (int i = 0; i <= 48; ++i) {
			const float angle = dr::kMinAngleLog2 * (1.0f - static_cast<float>(i) / 48.0f);
			dr::Band band;
			band.pole = dr::Point{r, angle};
			band.zero = band.pole;
			const dr::Biquad c = dr::coefficientsOf(band);
			// WHERE THE POLE ACTUALLY LANDED, recovered from the coefficients the DSP will run:
			// radius is sqrt(a2) and the angle is acos(-a1 / 2r).
			const double realisedRadius = std::sqrt(static_cast<double>(c.a2));
			const double cosTheta = realisedRadius > 0.0
			                            ? std::clamp(-static_cast<double>(c.a1) / (2.0 * realisedRadius),
			                                         -1.0, 1.0)
			                            : 1.0;
			const double realisedNu = std::acos(cosTheta) / kPi;
			const double storedNu = std::exp2(static_cast<double>(angle));
			const double octaves = std::fabs(std::log2(realisedNu / storedNu));
			if (octaves > worst) {
				worst = octaves;
				worstAngle = angle;
			}
			if (angle >= -10.0f) {
				worstAbove10 = std::max(worstAbove10, octaves);
			}
		}
	}
	CAPTURE(worst);
	CAPTURE(worstAngle);
	CAPTURE(worstAbove10);
	CHECK(worst < 0.15);
	CHECK(worstAbove10 < 0.02);
	// AND THE RADIUS SURVIVES INTACT, which is the half that would have mattered for stability.
	for (const float r : {0.5f, 0.9f, dr::kMaxPoleRadius}) {
		dr::Band band;
		band.pole = dr::Point{r, dr::kMinAngleLog2};
		band.zero = band.pole;
		CHECK(std::sqrt(static_cast<double>(dr::coefficientsOf(band).a2))
		      == doctest::Approx(static_cast<double>(r)).epsilon(1e-6));
	}
}

//==================================================================================================
// WAKE — the zeros lag the poles while the puck moves. Design §8.
//==================================================================================================

namespace {

/// FOUR BYPASSED CORNERS AT DIFFERENT PLACES. Every frame is transparent, so the TARGET is flat at
/// every puck position — and the poles still move when the puck does, which is what makes this the
/// one patch where `Wake` can be seen on its own. Any deviation from flat here is the zeros trailing
/// and can be nothing else.
dr::Settings bypassedCornersThatMove() {
	dr::Settings s;
	s.enabled = true;
	for (size_t k = 0; k < dr::kFrames; ++k) {
		// EVERY RADIUS STAYS INSIDE `kMaxPoleRadius`, and that is load-bearing rather than tidy.
		// `bypassedFrame` spreads bands by +0.07 each, so a base above 0.65 puts band 5 past the pole
		// ceiling — and `stateOf` sanitises on the way in, which clamps the POLE and not the ZERO (they
		// have different bounds, by §3b's asymmetry). The pair would arrive at the device already
		// separated and the patch would not be bypassed at all.
		//
		// FOUND BY THIS FIXTURE BEING WRONG. A scratch probe that drove `DrawnFilter` directly showed
		// a flat trace, because calling the DSP skips the ingest guard the row path applies; through
		// the ops it read 102.9 dB at Wake 0, where the whole point is that it must read zero. **A
		// probe that bypasses the guard is measuring a device the host cannot ask for.**
		s.frame[k] = bypassedFrame(0.15f + 0.10f * static_cast<float>(k));
	}
	return s;
}

/// Worst |dB| away from unity across the spectrum, for a realised cascade.
double worstDeviationDb(const dr::Realised& r) {
	double worst = 0.0;
	for (int i = 0; i <= 400; ++i) {
		const double m = std::max(1e-9, static_cast<double>(dr::magnitudeAt(r, static_cast<float>(i) / 400.0f)));
		worst = std::max(worst, std::fabs(20.0 * std::log10(m)));
	}
	return worst;
}

} // namespace

TEST_CASE("at Wake 0 the zeros walk the poles' road exactly, block for block") {
	// DESIGN §8's FIRST ASSERTION. At zero the split time constant collapses and the device must
	// behave as though `Wake` had never been added — which is a claim about the TRAJECTORY, not about
	// where it ends up, so it is asserted block by block against an independent restatement of the
	// one-pole rather than against a second call into the smoother.
	//
	// THE RESTATEMENT SHARES `kSmoothingSeconds` ON PURPOSE, because that constant IS the contract:
	// if somebody changes the poles' speed deliberately, both move together and that is correct. What
	// must not change silently is the RELATIONSHIP, so the known-bad for this case perturbs the USE
	// (`alphaFor(kSmoothingSeconds * 2, ...)`) rather than the constant.
	Ops ops;
	dr::Settings s = bypassedCornersThatMove();
	s.wake = 0.0f;
	s.morphX = 0.0f;
	s.morphY = 0.0f;
	ops.snap(s);
	s.morphX = 1.0f;
	s.morphY = 1.0f;
	ops.fields(s);

	// The one-pole, written out here: one step per control block, and the same "park when it can no
	// longer move" rule, which is a property of the smoother rather than of `Wake`.
	const double blocksPerSecond = 48000.0 / static_cast<double>(dr::kControlBlock);
	const double tau = dr::kSmoothingSeconds * blocksPerSecond;
	const float alpha = tau > 1.0 ? static_cast<float>(1.0 - std::exp(-1.0 / tau)) : 1.0f;
	const auto stepTo = [alpha](float current, float target) {
		const float next = current + (target - current) * alpha;
		return (next == current && current != target) ? target : next;
	};

	dr::Frame expected = dr::withGlobals(dr::blend(bypassedCornersThatMove()), 0.0f, 0.0f);
	{
		dr::Settings at = s;
		at.morphX = 0.0f;
		at.morphY = 0.0f;
		expected = dr::withGlobals(dr::blend(at), at.cutoff, at.res);
	}
	const dr::Frame want = dr::withGlobals(dr::blend(s), s.cutoff, s.res);

	std::vector<float> left(dr::kControlBlock, 0.0f);
	std::vector<float> right(dr::kControlBlock, 0.0f);
	int mismatches = 0;
	int blocks = 0;
	for (int b = 0; b < 400; ++b) {
		std::fill(left.begin(), left.end(), 0.0f);
		std::fill(right.begin(), right.end(), 0.0f);
		ops.process(left.data(), right.data(), dr::kControlBlock);
		for (size_t band = 0; band < dr::kBands; ++band) {
			dr::Band& e = expected.band[band];
			const dr::Band& t = want.band[band];
			e.pole.radius = stepTo(e.pole.radius, t.pole.radius);
			e.pole.angleLog2 = stepTo(e.pole.angleLog2, t.pole.angleLog2);
			e.zero.radius = stepTo(e.zero.radius, t.zero.radius);
			e.zero.angleLog2 = stepTo(e.zero.angleLog2, t.zero.angleLog2);
		}
		++blocks;
		if (!(ops.smoothed() == expected)) {
			++mismatches;
		}
	}
	CHECK(blocks == 400);
	CHECK(mismatches == 0);
	// AND THE TRAJECTORY WENT SOMEWHERE, so "identical" is not two things standing still.
	CHECK_FALSE(expected == dr::withGlobals(dr::blend(bypassedCornersThatMove()), 0.0f, 0.0f));
	CHECK(ops.smoothed() == want);
}

TEST_CASE("Wake colours the journey and never the destination — at rest the response IS the drawn curve") {
	// DESIGN §8's SECOND ASSERTION, and the one that keeps the control honest: a knob that
	// permanently coloured the curve would just be a second Res.
	//
	// **IT IS ASSERTED AS BIT EQUALITY, AND IT COST A FIX TO BE ABLE TO.** A one-pole in float parks
	// short of its target by about an ulp over alpha, so the slow road parked further out than the
	// fast one and the zeros came to rest a hair off the poles and STAYED there — MEASURED at 0.000329
	// dB from flat at Wake 0.25 and 0.0147 dB at Wake 1.0, growing with the knob. Inaudible, and
	// exactly the thing this case exists to forbid. `step()` now parks on the target when the one-pole
	// can no longer move, which is what makes the equality below exact rather than a tolerance.
	for (const float wake : { 0.0f, 0.25f, 0.5f, 1.0f }) {
		CAPTURE(wake);
		Ops ops;
		dr::Settings s = bypassedCornersThatMove();
		s.wake = wake;
		s.morphX = 0.0f;
		s.morphY = 0.0f;
		ops.snap(s);
		s.morphX = 1.0f;
		s.morphY = 1.0f;
		ops.fields(s);

		// MEASURED settle times to BIT equality: 110 blocks at Wake 0, 320 at 0.25, 925 at 0.5 and
		// 7523 at 1.0 — about ten time constants, since this is arrival rather than audible arrival.
		std::vector<float> left(dr::kControlBlock, 0.0f);
		std::vector<float> right(dr::kControlBlock, 0.0f);
		for (int b = 0; b < 9000; ++b) {
			std::fill(left.begin(), left.end(), 0.0f);
			std::fill(right.begin(), right.end(), 0.0f);
			ops.process(left.data(), right.data(), dr::kControlBlock);
		}

		const dr::Frame want = dr::withGlobals(dr::blend(s), s.cutoff, s.res);
		CHECK(ops.smoothed() == want);
		CHECK(ops.realised() == dr::realise(want));
		// AND THE DRAWN CURVE HERE IS FLAT, so the destination is checkable as a response and not only
		// as a struct: every corner is Bypass, so the settled cascade is EXACTLY unity.
		for (int i = 0; i <= 64; ++i) {
			CHECK(dr::magnitudeAt(ops.realised(), static_cast<float>(i) / 64.0f) == 1.0f);
		}
	}
}

TEST_CASE("during a move the zeros trail, so a fully-Bypass patch is NOT flat — and Wake 0 proves it is Wake's doing") {
	// DESIGN §8's THIRD ASSERTION: the effect exists at all, asserted on the one patch where it is
	// otherwise impossible. Every frame is Bypass, so the TARGET is flat at every puck position and
	// the settled response is flat to the bit — any deviation in between is the zeros trailing their
	// poles and can be nothing else.
	const auto peakDeviationDuringMove = [](float wake) {
		Ops ops;
		dr::Settings s = bypassedCornersThatMove();
		s.wake = wake;
		s.morphX = 0.0f;
		s.morphY = 0.0f;
		ops.snap(s);
		s.morphX = 1.0f;
		s.morphY = 1.0f;
		ops.fields(s);

		std::vector<float> left(dr::kControlBlock, 0.0f);
		std::vector<float> right(dr::kControlBlock, 0.0f);
		double worst = 0.0;
		for (int b = 0; b < 2000; ++b) {
			std::fill(left.begin(), left.end(), 0.0f);
			std::fill(right.begin(), right.end(), 0.0f);
			ops.process(left.data(), right.data(), dr::kControlBlock);
			worst = std::max(worst, worstDeviationDb(ops.realised()));
		}
		return worst;
	};

	// AT ZERO, NOTHING — and this is the control. Without it "the response moved" would be a claim
	// about the puck rather than about Wake: the poles move either way.
	const double atZero = peakDeviationDuringMove(0.0f);
	CAPTURE(atZero);
	CHECK(atZero == 0.0);

	// TURNED UP, SOMETHING, AND A LOT OF IT. MEASURED: 105.7 dB at Wake 0.25 and 158.1 dB at 0.5 —
	// six bands each briefly separating multiply into a very deep transient notch. The bar is 6 dB
	// because what is asserted is "audible", not the exact depth, which is a taste number.
	for (const float wake : { 0.25f, 0.5f, 1.0f }) {
		CAPTURE(wake);
		CHECK(peakDeviationDuringMove(wake) > 6.0);
	}

	// AND MORE WAKE TRAILS FURTHER, which is the knob having a direction rather than an on/off.
	CHECK(peakDeviationDuringMove(0.5f) > peakDeviationDuringMove(0.25f));
}

TEST_CASE("Wake does not touch the poles, so the stability argument is untouched by construction") {
	// DESIGN §8's FOURTH ASSERTION. §3b's correction is what makes this design safe — stability
	// constrains poles and NOTHING constrains zeros — so the claim that must hold is not "the filter
	// stayed stable at these settings" but "the knob cannot reach the poles at all". That is a
	// statement about the trajectory, and it is asserted as bit equality across settings rather than
	// as a stability measurement, because a measurement would only cover the settings it tried.
	const auto poleTrajectory = [](float wake) {
		Ops ops;
		dr::Settings s = bypassedCornersThatMove();
		s.wake = wake;
		s.morphX = 0.0f;
		s.morphY = 0.0f;
		ops.snap(s);
		s.morphX = 1.0f;
		s.morphY = 1.0f;
		ops.fields(s);

		std::vector<float> left(dr::kControlBlock, 0.0f);
		std::vector<float> right(dr::kControlBlock, 0.0f);
		std::vector<dr::Point> poles;
		poles.reserve(600 * dr::kBands);
		for (int b = 0; b < 600; ++b) {
			std::fill(left.begin(), left.end(), 0.0f);
			std::fill(right.begin(), right.end(), 0.0f);
			ops.process(left.data(), right.data(), dr::kControlBlock);
			for (const auto& band : ops.smoothed().band) {
				poles.push_back(band.pole);
			}
		}
		return poles;
	};

	const std::vector<dr::Point> reference = poleTrajectory(0.0f);
	REQUIRE(reference.size() == 600 * dr::kBands);
	for (const float wake : { 0.25f, 0.5f, 1.0f }) {
		CAPTURE(wake);
		const std::vector<dr::Point> other = poleTrajectory(wake);
		REQUIRE(other.size() == reference.size());
		int differed = 0;
		for (size_t i = 0; i < reference.size(); ++i) {
			if (!(other[i] == reference[i])) {
				++differed;
			}
		}
		CHECK(differed == 0);
	}

	// AND THE POLES REALLY MOVED, so "identical across settings" is not four copies of a standstill.
	CHECK_FALSE(reference.front() == reference.back());
	// AND EVERY ONE OF THEM WAS STABLE THE WHOLE WAY, which is the property the equality above
	// protects rather than replaces.
	int unstable = 0;
	for (const auto& p : reference) {
		if (!dr::poleIsStable(p)) {
			++unstable;
		}
	}
	CHECK(unstable == 0);
}

//==================================================================================================
// THE CONTROL GRID IS ABSOLUTE — OR IT IS NOT A TIME CONSTANT AT ALL
//==================================================================================================

TEST_CASE("the smoother takes one step per kControlBlock SAMPLES, not one per host call") {
	// drawn.h calls `kControlBlock` "how many samples between coefficient updates", and
	// `refreshAlphas` spells out what that buys: *"ONE POLE PER CONTROL BLOCK, not per sample, so the
	// time constants are expressed in blocks."* Every number in the device — `kSmoothingSeconds`,
	// `kWakeMaxSeconds`, `alphaFor`'s `tau` — is calibrated against that one rate. So the rate is the
	// contract, and it can only be honoured if the grid is ABSOLUTE: carried across calls, the way
	// HARM carries `gridPhase_` and `MultimodeFilter` carries `untilTick_`.
	//
	// EVERY OTHER SMOOTHING CASE IN THIS FILE DRIVES AT EXACTLY `kControlBlock`, which is the one
	// block size where an absolute grid and a per-call one give the same answer. This one drives at a
	// quarter of it.
	//
	// A VALUE, NOT A DIFFERENCE. The reference is an independent restatement of the one-pole — the
	// same restatement the Wake-0 case above uses — iterated the number of times the CONTRACT says,
	// which is `samples / kControlBlock`. Both block sizes are then asserted against that same
	// number, so a case that merely showed "the two runs differ" could not stand in for it.
	constexpr int kTotalSamples = dr::kControlBlock * 4; // 128 — four steps' worth of grid.
	constexpr int kSmallBlock = dr::kControlBlock / 4;   // 8 — a size the host really hands out.
	static_assert(kTotalSamples % dr::kControlBlock == 0);
	static_assert(kTotalSamples % kSmallBlock == 0);
	constexpr int kStepsOwed = kTotalSamples / dr::kControlBlock;      // 4
	constexpr int kStepsIfPerCall = kTotalSamples / kSmallBlock;       // 16 — what the bug does

	// Wake 0, so pole and zero share one alpha and the restatement is a single number. Bit-exact:
	// `pow(x, 0)` is exactly 1, which is the invariant the Wake-0 case above already rests on.
	const double blocksPerSecond = 48000.0 / static_cast<double>(dr::kControlBlock);
	const double tau = dr::kSmoothingSeconds * blocksPerSecond;
	const float alpha = tau > 1.0 ? static_cast<float>(1.0 - std::exp(-1.0 / tau)) : 1.0f;
	const auto stepTo = [alpha](float current, float target) {
		const float next = current + (target - current) * alpha;
		return (next == current && current != target) ? target : next;
	};

	dr::Settings from = bypassedCornersThatMove();
	from.wake = 0.0f;
	from.morphX = 0.0f;
	from.morphY = 0.0f;
	dr::Settings to = from;
	to.morphX = 1.0f;
	to.morphY = 1.0f;

	const dr::Frame start = dr::withGlobals(dr::blend(from), from.cutoff, from.res);
	const dr::Frame want = dr::withGlobals(dr::blend(to), to.cutoff, to.res);

	const auto after = [&](int steps) {
		dr::Frame f = start;
		for (int n = 0; n < steps; ++n) {
			for (size_t band = 0; band < dr::kBands; ++band) {
				dr::Band& e = f.band[band];
				const dr::Band& t = want.band[band];
				e.pole.radius = stepTo(e.pole.radius, t.pole.radius);
				e.pole.angleLog2 = stepTo(e.pole.angleLog2, t.pole.angleLog2);
				e.zero.radius = stepTo(e.zero.radius, t.zero.radius);
				e.zero.angleLog2 = stepTo(e.zero.angleLog2, t.zero.angleLog2);
			}
		}
		return f;
	};
	const dr::Frame owed = after(kStepsOwed);
	const dr::Frame perCall = after(kStepsIfPerCall);

	// NON-VACUITY FIRST, because this case is worthless if the two predictions coincide — which is
	// exactly what happens if you pick a journey long enough for both to have arrived. Four steps and
	// sixteen are 0.587 and 0.119 of the distance remaining, so they are far apart; the assertions
	// below can tell them apart, and they can both tell either from standing still.
	REQUIRE_FALSE(owed == start);
	REQUIRE_FALSE(owed == perCall);
	REQUIRE_FALSE(perCall == start);

	const auto run = [&](int framesPerCall) {
		Ops ops;
		ops.snap(from);
		ops.fields(to);
		std::vector<float> left(static_cast<size_t>(framesPerCall), 0.0f);
		std::vector<float> right(static_cast<size_t>(framesPerCall), 0.0f);
		for (int done = 0; done < kTotalSamples; done += framesPerCall) {
			std::fill(left.begin(), left.end(), 0.0f);
			std::fill(right.begin(), right.end(), 0.0f);
			ops.process(left.data(), right.data(), framesPerCall);
		}
		return ops.smoothed();
	};

	// THE GRID-ALIGNED RUN IS THE CONTROL. It has to agree with the contract, or the restatement is
	// wrong rather than the device — and then nothing below means anything.
	const dr::Frame atGrid = run(dr::kControlBlock);
	CHECK(atGrid == owed);
	CHECK(atGrid.band[0].pole.radius == doctest::Approx(owed.band[0].pole.radius));

	// AND THE SMALL BLOCK MUST LAND IN THE SAME PLACE. 128 samples is four control blocks whoever
	// asks for them; a device that took sixteen steps has run its 5 ms smoother at 1.25 ms.
	const dr::Frame atSmall = run(kSmallBlock);
	CHECK(atSmall == owed);
	// NAMED, so the failure says WHAT rather than `{?} == {?}` — the field is band 0's pole radius,
	// and the two predictions differ in it by a wide margin.
	CHECK(atSmall.band[0].pole.radius == doctest::Approx(owed.band[0].pole.radius));
	CHECK_FALSE(atSmall == perCall);
}

//==================================================================================================
// THE SAFETY BOUND MUST FAIL SAFE — AND WHAT HAPPENS WHEN IT DOES NOT
//==================================================================================================

namespace {

/// |H| FOR A MONIC BIQUAD AT ONE FREQUENCY, WRITTEN OUT FROM THE DEFINITION.
///
/// `H(z) = (1 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)` on `z = e^{jw}`, evaluated as two
/// complex sums in double. An INDEPENDENT restatement: it shares no line with `peakMagnitude`, which
/// gets its answer from a closed-form extremum of `P(c)/Q(c)`. That independence is the whole point —
/// a reference computed by the code under test is not a reference.
double magnitudeOfBiquad(const dr::Biquad& c, double w) {
	const double cw = std::cos(w);
	const double sw = std::sin(w);
	const double c2w = std::cos(2.0 * w);
	const double s2w = std::sin(2.0 * w);
	const double nRe = 1.0 + static_cast<double>(c.b1) * cw + static_cast<double>(c.b2) * c2w;
	const double nIm = -(static_cast<double>(c.b1) * sw + static_cast<double>(c.b2) * s2w);
	const double dRe = 1.0 + static_cast<double>(c.a1) * cw + static_cast<double>(c.a2) * c2w;
	const double dIm = -(static_cast<double>(c.a1) * sw + static_cast<double>(c.a2) * s2w);
	const double den = std::hypot(dRe, dIm);
	return den > 0.0 ? std::hypot(nRe, nIm) / den : std::numeric_limits<double>::infinity();
}

/// THE LARGEST |H| A DENSE SWEEP CAN FIND. A LOWER bound on the true maximum by construction — a
/// grid can only miss a peak, never invent one — which is exactly the direction that makes it usable
/// as a reference for a function that claims to return the maximum.
double sampledPeak(const dr::Biquad& c, int points) {
	double best = 0.0;
	for (int i = 0; i <= points; ++i) {
		const double w = kPi * static_cast<double>(i) / static_cast<double>(points);
		best = std::max(best, magnitudeOfBiquad(c, w));
	}
	return best;
}

/// EVERY BAND OF EVERY FRAME AT THE TOP OF ITS DECLARED RANGE.
///
/// NOT AN EXOTIC STATE. All 103 of this device's controls are declared `ParamDesc`s with a `max`, so
/// a host can automate every one of them there, and the poles are draggable on the card besides.
dr::Settings everyControlAtItsCeiling() {
	dr::Settings s;
	s.enabled = true;
	for (auto& frame : s.frame) {
		for (auto& band : frame.band) {
			band.pole = dr::Point { dr::kMaxPoleRadius, dr::kMaxAngleLog2 };
			band.zero = dr::Point { dr::kMaxZeroRadius, dr::kMaxAngleLog2 };
		}
	}
	s.morphX = 1.0f;
	s.morphY = 1.0f;
	s.cutoff = 4.0f;
	s.res = 1.0f;
	s.drive = 1.0f;
	s.wake = 1.0f;
	return s;
}

} // namespace

TEST_CASE("peakMagnitude never UNDER-reads, and a vanishing denominator is where it used to") {
	// `peakMagnitude` is documented as *"THE LARGEST |H| ANYWHERE ON THE UNIT CIRCLE"* and it is the
	// number the cascade trim is computed from — so it is a SAFETY bound, and the only direction that
	// matters is that it must not read LOW. Its own header says as much: *"this number is a SAFETY
	// bound, where under-reading is the direction that lets a cascade of six resonances through."*
	//
	// IT UNDER-READS BY SIXTEEN ORDERS OF MAGNITUDE AT THE ONE SECTION IT MOST NEEDS TO CATCH.
	// `ratioAt` returns `den > 0.0 ? p.at(cc) / den : 0.0` — and for a repeated pole ON the unit
	// circle's frequency axis at radius 0.99999, the denominator at Nyquist is mathematically zero, so
	// in double it comes out as roundoff of EITHER SIGN. Negative, and the bound reports the most
	// resonant section representable as the least resonant one.
	//
	// THIS IS THE SAME BUG THE FILE ALREADY RECORDS FIXING. drawn_types.cpp: *"MEASURED,
	// `peakMagnitude` returned ZERO for such sections, which then made their normalisation the
	// identity and let a cascade of six of them reach a gain of 8.7e7 with the safety trim reporting
	// that it had nothing to do."* The remedy taken was double precision, which fixed the near-DC
	// instance; **the `: 0.0` fallback survived it.** Double buys precision, not a sign guarantee.
	SUBCASE("a benign section agrees with a dense sweep, so the comparison below is not vacuous") {
		dr::Biquad c;
		c.a1 = 1.9f;
		c.a2 = 0.91f;
		const float closed = dr::peakMagnitude(c);
		const double swept = sampledPeak(c, 400000);
		CAPTURE(closed);
		CAPTURE(swept);
		// The closed form is the exact maximum and the sweep is a lower bound on it, so this is the
		// direction that must hold with no slack at all beyond float/double rounding.
		CHECK(static_cast<double>(closed) >= swept * (1.0 - 1e-6));
		// AND THE SWEEP FOUND SOMETHING REAL, so "closed >= swept" is not two numbers near zero.
		CHECK(swept > 100.0);
	}

	SUBCASE("the section the device actually runs at maximum Res and maximum Pole Q") {
		// THE COEFFICIENTS ARE READ OFF THE RUNNING DEVICE rather than invented here, so this cannot
		// drift into asserting something about a section the device never builds.
		Ops ops;
		ops.snap(everyControlAtItsCeiling());
		std::vector<float> l(dr::kControlBlock, 0.0f);
		std::vector<float> r(dr::kControlBlock, 0.0f);
		ops.process(l.data(), r.data(), dr::kControlBlock);

		const dr::Biquad c = ops.realised().section[0].c;
		CAPTURE(c.a1);
		CAPTURE(c.a2);
		// A REPEATED REAL POLE JUST INSIDE THE CIRCLE: a1 ~ 2, a2 ~ 1. Asserted so a later reader knows
		// which section this case is about, and so it fails loudly if the device stops building one.
		CHECK(c.a2 > 0.9999f);
		CHECK(c.a1 > 1.9998f);

		const double swept = sampledPeak(c, 400000);
		const float closed = dr::peakMagnitude(c);
		CAPTURE(swept);
		CAPTURE(closed);
		// THE SWEEP HAS TO HAVE FOUND THE RESONANCE, or this case is asserting nothing.
		REQUIRE(swept > 1.0e6);

		// **A BOUND ON PURPOSE, AND THIS IS THE ONE PLACE IN THIS FILE WHERE THAT IS THE HONEST FORM.**
		// The playbook's rule is that a bound stands in for a value that IS recoverable. Here none is:
		// `Q(-1)` equals `(1 - a1 + a2)^2`, which for this pole is `(1 - r)^2 = 1e-10` — and `a1` and
		// `a2` are FLOATS carrying ~1e-7 of relative error, so they do not encode `r` finely enough for
		// any arithmetic to recover it. The dense sweep and the closed form disagree by four orders
		// here because they cancel differently, not because one of them is right.
		//
		// SO THE CLAIM IS DIRECTIONAL, and the direction is the whole of what the bound is for:
		//
		//     before   1.82252     the section reads as barely resonant, trim = 1, device -> inf
		//     after    4.9e+14     the section reads as enormously resonant, and is normalised away
		//     the sweep's answer   1.99787e+20
		//
		// A test that demanded the sweep's number would be tuning `kMinDenominator` until it matched a
		// figure that is itself cancellation noise. What must never happen again is a SMALL answer.
		CHECK(static_cast<double>(closed) > 1.0e6);
	}
}

TEST_CASE("at every control's ceiling the device stays finite through a second of silence") {
	// **A NaN OUT OF A DEVICE POISONS THE BUS FOR THE REST OF THE SESSION.** Every arithmetic
	// operation downstream of one is a NaN, the master fader cannot rescue it, and the user has no
	// control that clears it. So this is a safety property rather than a quality bar.
	//
	// MEASURED BEFORE THE FIX, +12 dBFS for 512 samples and then SILENCE:
	//
	//     pass 0   peak 2.69928e+19
	//     pass 1   peak 8.86739e+22      <- nothing going in from here on
	//     pass 5   peak 2.19263e+28
	//     pass 50  NON-FINITE at sample 25600 = 0.533 s
	//
	// **The input stops and the output climbs to infinity anyway**, because the cascade trim was
	// computed from a `peakMagnitude` that reported 0.250003 for a section whose true peak is 9.0e15
	// (see the case above), so `trim` came out at exactly 1 and six near-circle resonances ran
	// untrimmed.
	Ops ops;
	ops.snap(everyControlAtItsCeiling());

	constexpr int kBlock = 512;
	std::vector<float> l(kBlock, 0.0f);
	std::vector<float> r(kBlock, 0.0f);

	// A HOT BURST FIRST. +12 dBFS is what a chain of devices in front of this one can hand over.
	for (int i = 0; i < kBlock; ++i) {
		l[i] = 4.0f * static_cast<float>(std::sin(2.0 * kPi * 0.017 * static_cast<double>(i)));
		r[i] = l[i];
	}
	ops.process(l.data(), r.data(), kBlock);

	// AND THEN A SECOND OF NOTHING. 94 blocks of 512 is 48128 samples, past the 25600 where the
	// unfixed device reached infinity.
	int nonFinite = 0;
	int firstBadBlock = -1;
	float worst = 0.0f;
	for (int block = 0; block < 94; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		ops.process(l.data(), r.data(), kBlock);
		for (int i = 0; i < kBlock; ++i) {
			for (const float x : { l[i], r[i] }) {
				if (!std::isfinite(x)) {
					++nonFinite;
					if (firstBadBlock < 0) {
						firstBadBlock = block;
					}
					continue;
				}
				worst = std::max(worst, std::fabs(x));
			}
		}
	}
	CAPTURE(firstBadBlock);
	CAPTURE(worst);
	CHECK(nonFinite == 0);

	// AND WHERE THE NORMALISATION WAS DONE, which only a VALUE can say. `realise` sets each section's
	// gain from `clamp(edge, peak / kPeakHeadroom, peak)`, so the question is which of the three the
	// clamp picked.
	//
	// **BOTH ASSERTIONS BELOW USED TO SAY SOMETHING ELSE, AND BOTH WERE WRONG. Recorded rather than
	// quietly retracked, because each was wrong in a way this file has a rule about.**
	//
	// THE FIRST ASSERTED `trim == 1.0f`, and its comment read that as "the edge was measured
	// correctly, so the guard had nothing to do". MEASURED, at this exact state: the denominator at
	// Nyquist was **exactly 0**, so `|H(Nyquist)|` was **INFINITY**, and `clamp(inf, peak/4, peak)`
	// is `peak`. The trim of 1 was an infinite edge being clamped down — the case was pinning a
	// divide-by-zero as though it were the healthy branch. That zero denominator IS the pole sitting
	// exactly ON the unit circle; see "a pole realises INSIDE the unit circle" for the same fact from
	// the other side and for why it happened only at Nyquist.
	//
	// NOW THE EDGE IS FINITE (8.2e6 measured), the peak bound is 4.9e14, and `edge < peak / 4` — so
	// `reference` is the `peak / kPeakHeadroom` fallback for every section and the trim claws back
	// `kPeakHeadroom^6`. That is the guard WORKING on the most resonant cascade this device can be
	// asked for, which is what a ceiling case should be seeing.
	const float trim = ops.realised().trim;
	CAPTURE(trim);

	// THIS ASSERTED `trim < 1` AND `trim == kCascadeCeiling / kPeakHeadroom^6` — 9.76563e-4 — AND
	// BOTH PINNED A FORMULA THAT IS NO LONGER THE ONE, deliberately. In a host, Drawn lost a lot of
	// volume; the trim divided by the PRODUCT of the six sections' peaks, which
	// is an upper bound rather than the peak, and on six lowpass bands at r = 0.95 it cost 13.3 dB
	// while the cascade's true peak was 0.409 — comfortably under the ceiling. `realise` now sweeps
	// the realised cascade instead, so a chain that never gets loud is not trimmed at all.
	//
	// SO THE ASSERTION MOVES TO THE CONTRACT RATHER THAN THE ARITHMETIC, which is what this case was
	// always about: the ceiling holds, and it holds because of the trim rather than by luck. This
	// arrangement's own true peak is what decides whether a trim is needed, and the case reads that
	// rather than restating a closed form that a later change to the normalisation would falsify
	// again.
	float sweptPeak = 0.0f;
	for (int i = 0; i <= 4000; ++i) {
		const float m = dr::magnitudeAt(ops.realised(), 0.5f * static_cast<float>(i) / 4000.0f);
		if (std::isfinite(m)) {
			sweptPeak = std::max(sweptPeak, m);
		}
	}
	CAPTURE(sweptPeak);
	CHECK(sweptPeak <= dr::kCascadeCeiling + 1e-3f);
	// AND THE TRIM IS THE THING HOLDING IT: divide it back out and the same cascade is over the
	// ceiling — or the trim is 1 because nothing needed trimming, which is now a legitimate answer
	// and was not before.
	CHECK((trim == 1.0f || sweptPeak / trim > dr::kCascadeCeiling));

	// THE SECOND ASSERTED `gain == Approx(1 / sectionPeak).epsilon(1e-4)` AND IT COULD NOT FAIL.
	// **`doctest::Approx` is not a relative comparison**: it passes when
	// `|a - b| < epsilon * (scale + max(|a|, |b|))` with `scale` defaulting to 1. Both sides here are
	// around 1e-15, so the bar was `1e-4 * (1 + 1e-15)` = 1e-4 — eight orders of magnitude larger than
	// either number. MEASURED: it reported `8.16326e-15 == Approx(2.04081e-15)` as **SUCCESS**, a
	// factor of four apart, which is exactly the factor it existed to detect.
	//
	// SO THE COMPARISON IS MADE SCALE-FREE by asserting the RATIO, which is the quantity the branch
	// is about and is order-one whatever the magnitudes are.
	const float sectionPeak = dr::peakMagnitude(ops.realised().section[0].c);
	CAPTURE(sectionPeak);
	const double normalised = static_cast<double>(ops.realised().section[0].gain)
	                          * static_cast<double>(sectionPeak);
	CAPTURE(normalised);
	CHECK(normalised == doctest::Approx(static_cast<double>(dr::kPeakHeadroom)).epsilon(1e-4));
	// AND THE TRAP IS SHUT: the same claim spelled the old way would have been satisfied by any two
	// numbers this small, so this line asserts that the ratio is NOT the other branch's 1.
	CHECK(normalised > 2.0);
}

TEST_CASE("the whole Res x Pole Q plane is finite, and the part below the ceiling still sounds") {
	// THE CORNER ABOVE IS NOT A CORNER. Swept across four pole radii and five Res positions, SEVEN of
	// the twenty points reached infinity before the fix — including **`Pole Q` at its declared maximum
	// with `Res` at ZERO**, which needs no global at all. Measured, tail peak after the input stops:
	//
	//     q      res    before                    after
	//     0.9    any    finite, |H|max 1          unchanged
	//     0.99   any    finite, |H|max 1          unchanged
	//     0.999  <=0.9  finite, |H|max 1          unchanged
	//     0.999  >=0.99 NON-FINITE, 3.4e38        finite
	//     0.9999 any    NON-FINITE, 3.4e38        finite
	//
	// THE BOUNDARY IS THE EFFECTIVE POLE RADIUS AND NOTHING ELSE, which the grid shows twice over: Res
	// warps `r -> 1 - (1 - r) * k`, so `q = 0.99, res = 1` and `q = 0.999, res = 0` are the SAME POLE
	// — and they measure the same tail peak to six figures (0.000242877 against 0.000242876). Two
	// coordinates for one filter, agreeing, which is what says the axis is the radius rather than
	// either knob.
	//
	// AND THE SECOND HALF IS THE ONE THAT KEEPS THE FIX HONEST. Silencing the device everywhere would
	// satisfy "finite" perfectly, so the benign region is asserted to still have unity gain. A fix
	// that trims the whole plane into the noise goes red here.
	int nonFinite = 0;
	int audible = 0;
	int checked = 0;
	for (const float q : { 0.90f, 0.99f, 0.999f, 0.9999f }) {
		for (const float res : { 0.0f, 0.5f, 0.9f, 0.99f, 1.0f }) {
			CAPTURE(q);
			CAPTURE(res);
			dr::Settings s = everyControlAtItsCeiling();
			for (auto& frame : s.frame) {
				for (auto& band : frame.band) {
					band.pole = dr::Point { q, dr::kMaxAngleLog2 };
				}
			}
			s.res = res;
			Ops ops;
			ops.snap(s);

			constexpr int kBlock = 512;
			std::vector<float> l(kBlock, 0.0f);
			std::vector<float> r(kBlock, 0.0f);
			for (int i = 0; i < kBlock; ++i) {
				l[i] = 4.0f * static_cast<float>(std::sin(2.0 * kPi * 0.017 * static_cast<double>(i)));
				r[i] = l[i];
			}
			ops.process(l.data(), r.data(), kBlock);
			for (int block = 0; block < 94; ++block) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				ops.process(l.data(), r.data(), kBlock);
				for (int i = 0; i < kBlock; ++i) {
					if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
						++nonFinite;
					}
				}
			}
			++checked;

			// THE REALISED CASCADE'S OWN CURVE, which is also what the card draws.
			double worstCurve = 0.0;
			for (int i = 0; i <= 2000; ++i) {
				const float m = dr::magnitudeAt(ops.realised(), static_cast<float>(i) / 2000.0f);
				if (std::isfinite(m)) {
					worstCurve = std::max(worstCurve, static_cast<double>(m));
				}
				else {
					++nonFinite;
				}
			}
			// BELOW THE REALISABLE CEILING the cascade is normalised to unity, and that is a VALUE
			// rather than a direction: `kCascadeCeiling`'s whole claim is that six normalised sections
			// multiply to at most 4, and six sections each normalised at their edge multiply to 1.
			//
			// **0.9995 IS WHERE THE CLAIM IS DEFENSIBLE, NOT WHERE THE TEST GOES GREEN**, and the
			// distinction matters because the two are easy to confuse. `warpRadius` is restated here
			// from `withGlobals` rather than called, so the coordinate is independent of the device.
			// MEASURED, the device is at unity up to an effective radius of 0.99981 and silenced from
			// 0.999891 — so the transition is inside (0.9998, 0.9999), which is precisely where
			// `drawn_types.h` measures the FLOAT realisation of `a1` to stop resolving the pole
			// (radius "survives to 1e-6", and 1 - r here is 1e-4 falling to 1e-5). Asserting unity
			// inside a band where the representation is known to be failing would be asserting
			// something no arithmetic can deliver. The unasserted band is named, not hidden.
			const float effective = 1.0f - (1.0f - q) * (1.0f - res * dr::kResDepth);
			if (effective <= 0.9995f) {
				CHECK(worstCurve == doctest::Approx(1.0).epsilon(1e-4));
				++audible;
			}
		}
	}
	CHECK(nonFinite == 0);
	// FLOORS, so a loop that stopped iterating cannot pass by doing nothing.
	CHECK(checked == 20);
	CHECK(audible >= 12);
}

TEST_CASE("the cascade is trimmed by what it actually peaks at, not by what six sections could do") {
	// SEEN IN A HOST, 2026-08-28: Drawn lost a lot of volume. It was real and most of it was
	// one line: the trim divided by the PRODUCT of the six sections' normalised peaks, which is an
	// upper bound on the cascade's peak rather than the peak — the sections peak at six different
	// frequencies and never all at once. `realise`'s own header said so and it read as a caveat.
	//
	// WHAT IS ASSERTED IS THE GAIN, not "the buffers differ". A render comparison passes whenever
	// anything changes; the claim here is a quantity — how much level the device throws away — so
	// that is what is read, off the realised cascade.
	namespace dr = feathers::fx::drawn;

	const auto sweptPeak = [](const dr::Realised& r) {
		float peak = 0.0f;
		for (int i = 0; i <= 4000; ++i) {
			const float m = dr::magnitudeAt(r, 0.5f * static_cast<float>(i) / 4000.0f);
			if (std::isfinite(m)) {
				peak = std::max(peak, m);
			}
		}
		return peak;
	};

	SUBCASE("a chain that never gets loud is not trimmed at all") {
		// SIX LOWPASS BANDS AT r = 0.95 — an ordinary thing to draw, and the arrangement the report
		// came from. MEASURED before the fix: trim 0.2165 (-13.3 dB) on a cascade whose true peak was
		// 0.409, so the trim was protecting against nothing and cost thirteen decibels.
		dr::Frame f = dr::kDefaultFrames[0];
		for (size_t b = 0; b < dr::kBands; ++b) {
			f.band[b].pole.radius = 0.95f;
			const auto z = dr::zeroPlacementFor(dr::Type::Lowpass, f.band[b].pole);
			REQUIRE(z.has_value());
			f.band[b].zero = *z;
		}
		const dr::Realised r = dr::realise(f);
		const float peak = sweptPeak(r);
		// NON-VACUITY FIRST: this frame really is well under the ceiling, or "no trim" would be the
		// right answer for a reason that has nothing to do with the fix.
		REQUIRE(peak < dr::kCascadeCeiling);
		CHECK(r.trim == doctest::Approx(1.0f).epsilon(1e-4));
	}

	SUBCASE("a chain that does get loud is still capped, and capped exactly") {
		// SIX COINCIDENT HIGH-Q RESONANCES — the arrangement the product bound was imagining, where
		// the bound and the truth agree. The guard must still fire, or the fix traded a quiet device
		// for an unsafe one.
		dr::Frame f = dr::kDefaultFrames[0];
		for (size_t b = 0; b < dr::kBands; ++b) {
			f.band[b].pole = dr::Point { 0.995f, -5.0f };
			f.band[b].zero = dr::Point { 0.0f, -5.0f };
		}
		const dr::Realised r = dr::realise(f);
		CHECK(r.trim < 0.01f);
		// AND THE RESULT LANDS ON THE CEILING RATHER THAN BELOW IT, which is the half the product
		// bound could never do: it over-trimmed by however loose the bound happened to be.
		CHECK(sweptPeak(r) == doctest::Approx(dr::kCascadeCeiling).epsilon(0.02));
	}

	SUBCASE("bypass is untouched, and that is checked rather than hoped for") {
		const dr::Realised r = dr::realise(dr::kDefaultFrames[0]);
		CHECK(r.trim == doctest::Approx(1.0f).epsilon(1e-6));
		CHECK(sweptPeak(r) == doctest::Approx(1.0f).epsilon(1e-4));
	}
}

TEST_CASE("two frames whose bands differ only by TYPE sound identical, at every puck position") {
	// **THIS IS THE CASE §3b ALWAYS WANTED AND COULD NOT HAVE**, because until 2026-08-29 there was
	// no stored type to wire into the blend by mistake.
	//
	// THE COMMENT THAT STOOD OVER `Type` FORBADE STORING IT: *"must not become stored ... the moment
	// it were state the blend would be back to interpolating a discrete thing."* The design note does not say
	// that — what it says is *"the type is only where that corner started"*, which describes a stored
	// annotation. The inference is true only if the BLEND consumes the type, so this case is the
	// thing that keeps it from ever doing so: **the type is state the sound cannot hear.**
	//
	// ASSERTED AS BIT-IDENTICAL RATHER THAN CLOSE. `realisedAt` is the whole device as one function,
	// and a blend that touched the type at all would move a coefficient — approximately, on some
	// grid position, which an epsilon would forgive. Equality is the only assertion that cannot.
	//
	// **THE SINGLE-SITE MUTATION IS INERT AND SOMEBODY WILL TRY IT.** Making `realise` consume the
	// type — scaling a coefficient by `frame.band[b].type` — comes back GREEN, and the case is not at
	// fault: the blend produces a NEW `Frame` whose type is the default on both sides, because it
	// correctly does not carry one. A mutation there is reading a field that has already been
	// discarded. **The honest known-bad is two-part**: the blend must propagate the type AND
	// `realise` must consume it, and then this goes red.
	//
	// So the invariant is guarded by a masked pair BY CONSTRUCTION — the same structure as
	// `bindingOf(Free)` in the editor's own case, arriving from the other direction. Found by the
	// integrator trying exactly the one-site mutation and reporting that the mutation was wrong
	// rather than that the case was weak.
	dr::Settings plain;
	plain.enabled = true;
	// A DRAWN SHAPE, not the flat default: identical frames would make the blend trivially equal and
	// the case would pass on a device that ignored its frames entirely.
	plain.frame[0].band[0].pole = dr::Point { 0.80f, -4.0f };
	plain.frame[0].band[0].zero = dr::Point { 1.00f, -4.0f };
	plain.frame[1].band[2].pole = dr::Point { 0.60f, -2.0f };
	plain.frame[1].band[2].zero = dr::Point { 0.25f, -6.0f };
	plain.frame[3].band[5].pole = dr::Point { 0.95f, -1.5f };
	plain.frame[3].band[5].zero = dr::Point { -1.00f, -1.0f };

	// THE SAME GEOMETRY WEARING DIFFERENT LABELS. Every band of every frame is given a type, and none
	// of them agrees with the plain copy's.
	dr::Settings labelled = plain;
	for (size_t f = 0; f < dr::kFrames; ++f) {
		for (size_t b = 0; b < dr::kBands; ++b) {
			labelled.frame[f].band[b].type =
			    static_cast<dr::Type>((f * dr::kBands + b) % static_cast<size_t>(dr::Type::kCount));
		}
	}
	// THE TWO REALLY DO DIFFER, so the equality below is not comparing a thing with itself. This is
	// the non-vacuity half, and it is why `Band`'s `operator==` had to start counting the type.
	REQUIRE_FALSE(plain.frame[0].band[0] == labelled.frame[0].band[0]);

	// EVERY PUCK POSITION ON A GRID, corners included — a blend that read the type could do it
	// anywhere, and the corners are where three weights are exactly zero.
	for (int ix = 0; ix <= 8; ++ix) {
		for (int iy = 0; iy <= 8; ++iy) {
			plain.morphX = static_cast<float>(ix) / 8.0f;
			plain.morphY = static_cast<float>(iy) / 8.0f;
			labelled.morphX = plain.morphX;
			labelled.morphY = plain.morphY;
			CAPTURE(plain.morphX);
			CAPTURE(plain.morphY);
			CHECK(dr::realisedAt(plain) == dr::realisedAt(labelled));
		}
	}
}

TEST_CASE("a band's type survives the row, and an unnameable one reads as Free") {
	// THE TYPE IS PATCH STATE NOW, so it takes the road every other field takes — and a stored field
	// nobody round-trips is the shape `test_gui_bridge` has caught twice.
	dr::Settings settings;
	settings.frame[2].band[4].type = dr::Type::Allpass;
	settings.frame[0].band[0].type = dr::Type::Free;
	const dr::Settings back = dr::stateOf(dr::rowOf(settings), static_cast<size_t>(dr::Param::kCount));
	CHECK(back.frame[2].band[4].type == dr::Type::Allpass);
	CHECK(back.frame[0].band[0].type == dr::Type::Free);
	// AND THE WHOLE SETTINGS SURVIVES, so the type did not arrive at the cost of a neighbour.
	CHECK(back.frame == settings.frame);

	SUBCASE("a type this build cannot name reads as Free, not as whatever it indexes") {
		// A ROW IS WHATEVER REACHED THE TRANSPORT — a file, a host's automation, a build that
		// declared more types than this one. An out-of-range type would index `kTypeNames` off the
		// end the moment a menu drew it, and a NaN would convert to something undefined.
		//
		// FREE IS THE HONEST FALLBACK because it binds nothing: a type this build does not
		// understand cannot then be trusted to place a zero, and Free is the one that places none.
		dr::Row row = dr::rowOf(settings);
		const size_t at = dr::bandFieldIndex(1, 1, dr::TypeIndex);
		for (const float bad : { 99.0f, -3.0f, std::numeric_limits<float>::quiet_NaN(),
			                     std::numeric_limits<float>::infinity() }) {
			row[at] = bad;
			const dr::Settings loaded = dr::stateOf(row, static_cast<size_t>(dr::Param::kCount));
			CHECK(loaded.frame[1].band[1].type == dr::Type::Free);
			// AND THE BAND'S GEOMETRY IS UNTOUCHED — the bad field costs its own value and nothing
			// else, which is the rule the loaders learned this week.
			CHECK(loaded.frame[1].band[1].pole == settings.frame[1].band[1].pole);
		}
	}
}

TEST_CASE("the type menu offers every type, including the one that releases") {
	// `Free` USED TO BE ABSENT and the `static_assert` enforced its absence: `std::size(kTypeNames)
	// == static_cast<size_t>(Type::Free)`. That was right while nothing was stored — choosing Free
	// would have written nothing — and it is wrong now that every other entry BINDS the zero, because
	// Free became the only way to unbind it.
	//
	// THE ASSERT STILL ENFORCES COVERAGE, which is why it was changed rather than deleted: a menu
	// that could enter six constraints and leave none of them is the control that lies.
	CHECK(std::size(dr::kTypeNames) == static_cast<size_t>(dr::Type::kCount));
	CHECK(std::string(dr::kTypeNames[static_cast<size_t>(dr::Type::Free)]) == "Free");
	// EVERY NAME IS DISTINCT AND NONE IS EMPTY — a menu with two "Notch" rows is unusable in a way
	// the count above cannot see.
	std::vector<std::string> names;
	for (const char* name : dr::kTypeNames) {
		REQUIRE(name != nullptr);
		CHECK(std::strlen(name) > 0);
		names.emplace_back(name);
	}
	std::sort(names.begin(), names.end());
	CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("the default device stays flat when a hand MOVES something, which is what the default type buys") {
	// **THE HALF THE DEFAULT-FLATNESS CASE CANNOT SEE, and the one the stored type introduced.**
	// "The default device is EXACTLY flat" asks an UNTOUCHED device; this asks one a hand has moved.
	//
	// A NEW FIELD WITH A WRONG DEFAULT SILENTLY CHANGES EVERY EXISTING SOUND — and the type's wrong
	// default would not show up in the untouched case at all, because the type does not enter the
	// sound (that is asserted separately, and on purpose). It shows up on the FIRST POLE DRAG: with
	// the default `Bypass` the zero is carried and the band stays a wire; with `Free` it would be
	// left behind and an untouched device would start filtering the moment somebody grabbed a handle.
	//
	// SO THE DEFAULT IS ASSERTED THROUGH ITS CONSEQUENCE rather than as `type == Bypass`, which would
	// pass on a build where Bypass had stopped carrying the zero. The equality check below is the
	// same exactness the sibling case uses and for the same reason: a tolerance would accept a
	// default that quietly coloured everything.
	dr::Settings s;
	s.enabled = true;

	// THE DEFAULT TYPE, NAMED, so a reader can see which one is under test — but it is the moving
	// that does the asserting, and this is a CHECK for exactly that reason. As a `REQUIRE` it ended
	// the case at the first line under the mutation that matters, and the consequence below — the
	// half this case exists for — never ran. **A fatal premise hides the assertion it is a premise
	// for**, which is the same shape as a crashing known-bad reporting a short tally.
	CHECK(dr::Settings {}.frame[0].band[0].type == dr::Type::Bypass);

	// MOVE EVERY BAND OF EVERY FRAME, the way the editor moves a pole: the pole goes somewhere new
	// and the type carries the zero with it. Written here as the placement the type means rather than
	// by driving the panel, because this is the DEVICE's claim and the panel has its own case.
	for (size_t f = 0; f < dr::kFrames; ++f) {
		for (size_t b = 0; b < dr::kBands; ++b) {
			const dr::Point moved { 0.55f + 0.05f * static_cast<float>(b),
				                    -10.0f + 1.5f * static_cast<float>(f * dr::kBands + b) };
			s.frame[f].band[b].pole = moved;
			// EXACTLY WHAT THE EDITOR DOES, FOR WHATEVER TYPE THE DEFAULT IS — placed if the type
			// places, left alone if it does not. Written as the general rule rather than as
			// `REQUIRE(placed)` because the assertion this case exists for is BELOW, and a premise
			// that fails under the mutation that matters hides it: with a `Free` default there is no
			// placement to carry, the zero stays behind, and **that is the defect rather than a
			// reason the case cannot run.**
			const auto placed = dr::zeroPlacementFor(s.frame[f].band[b].type, moved);
			if (placed.has_value()) {
				s.frame[f].band[b].zero = *placed;
			}
		}
	}
	// THE POLES REALLY MOVED AND THE FOUR CORNERS REALLY DIFFER, or "flat everywhere" is the
	// untouched device again wearing a different name.
	REQUIRE_FALSE(s.frame[0].band[0].pole == dr::Settings {}.frame[0].band[0].pole);
	REQUIRE_FALSE(s.frame[0].band[0] == s.frame[1].band[0]);

	int notFlat = 0;
	for (int ix = 0; ix <= 8; ++ix) {
		for (int iy = 0; iy <= 8; ++iy) {
			s.morphX = static_cast<float>(ix) / 8.0f;
			s.morphY = static_cast<float>(iy) / 8.0f;
			const dr::Realised r = dr::realisedAt(s);
			for (int i = 0; i <= 32; ++i) {
				if (dr::magnitudeAt(r, static_cast<float>(i) / 32.0f) != 1.0f) {
					++notFlat;
				}
			}
		}
	}
	CHECK(notFlat == 0);
}

TEST_CASE("Cutoff slides a drawn shape, and a flat one has nothing to slide") {
	// The symptom: in Drawn (and apparently Automod), moving the Cutoff knob did not seem to affect
	// the drawn curve.
	//
	// **THE SYMPTOM IS REAL AND THE MECHANISM IS NOT BROKEN — the default patch is a bypass.** The
	// parameter table says so in as many words: *"THE ZERO DEFAULTS ON TOP OF THE POLE, which is
	// `Bypass` — so a row of pure defaults is flat."* A flat response is flat at every Cutoff, because
	// Cutoff is an ADDITION in the log-angle coordinate and a pole at radius 0 has no angle worth
	// moving. So a fresh Drawn draws a straight line and the knob appears dead.
	//
	// MEASURED, and this is the point of doing it by VALUE: a ±3-octave Cutoff sweep on the default
	// patch left the card's graph BYTE-IDENTICAL, while the whole-window capture differed — entirely
	// because the knob itself had rotated. A difference check on the picture would have called that
	// a working curve. This case asks the number instead.
	const auto sweepDiffers = [](const dr::Settings& base) {
		dr::Settings lo = base;
		dr::Settings hi = base;
		lo.cutoff = -3.0f;
		hi.cutoff = 3.0f;
		const dr::Realised rl = dr::realisedAt(lo);
		const dr::Realised rh = dr::realisedAt(hi);
		double worst = 0.0;
		for (int i = 1; i < 64; ++i) {
			const float nu = static_cast<float>(i) / 64.0f;
			worst = std::max(worst, std::fabs(static_cast<double>(dr::magnitudeAt(rl, nu))
			                                  - static_cast<double>(dr::magnitudeAt(rh, nu))));
		}
		return worst;
	};

	SUBCASE("a default patch is flat, so Cutoff cannot move it") {
		dr::Settings s;
		s.enabled = true;
		// NOT A BUG TO BE FIXED HERE. Whether Drawn should SHIP flat is a design question about its
		// init point — the same question automod's measured defaults answered — and it is recorded in
		// the backlog rather than decided by a test. What this pins is that the flatness explains the
		// dead knob, so nobody hunts a phantom in `realisedAt` again.
		CHECK(sweepDiffers(s) == doctest::Approx(0.0).epsilon(1e-9));
	}

	SUBCASE("give it a pole and Cutoff moves the response") {
		dr::Settings s;
		s.enabled = true;
		// ONE POLE OFF THE ORIGIN, IN EVERY FRAME, so the blend cannot average it away wherever the
		// puck happens to sit — the puck's default is the centre, which mixes all four.
		for (size_t f = 0; f < dr::kFrames; ++f) {
			dr::Band& band = s.frame[f].band[0];
			band.type = dr::Type::Free;
			band.pole.radius = 0.9f;
			band.pole.angleLog2 = -3.0f;
			// THE ZERO OFF THE POLE, or this is Bypass again with a bigger radius: a zero sitting on
			// its pole cancels it exactly, which is the identity the default relies on.
			band.zero.radius = 0.0f;
			band.zero.angleLog2 = -3.0f;
		}
		// **A REAL DIFFERENCE, NOT MERELY A NONZERO ONE.** 0.05 in linear magnitude is about half a dB
		// at unity — well above anything float noise produces and well below what a 6-octave slide of
		// a Q-0.9 resonance actually does, so the bar tests the mechanism rather than the tolerance.
		CHECK(sweepDiffers(s) > 0.05);
	}
}

TEST_CASE("backlog FK: a moving Drive is followed per sample inside the call — the same samples as setting Drive before every sample") {
	// Restated independently: a twin handed Drive through `setTarget` before EACH one-sample call hears the drive a
	// window describes, by the same mapping. The windowed device, handed the window once per 64-sample call through
	// the ops road, must render those samples. Drive is nonlinear, so a stair or a ramp in the wrong place differs.
	constexpr int kFrames = 64;
	constexpr int kTotal = 4096;
	dr::Settings s;
	s.enabled = true;
	s.cutoff = 1.0f;
	s.drive = 0.3f; // the scalar the window replaces
	std::array<float, kFrames> run {};
	for (int i = 0; i < kFrames; ++i) {
		run[static_cast<size_t>(i)] = 0.9f * static_cast<float>(i) / static_cast<float>(kFrames - 1);
	}
	std::vector<float> in(kTotal);
	for (int i = 0; i < kTotal; ++i) {
		in[static_cast<size_t>(i)] = 0.8f * std::sin(0.03f * static_cast<float>(i));
	}

	std::vector<unsigned char> raw(dr::kAudioOps.stateBytes + dr::kAudioOps.stateAlign);
	void* mem = raw.data();
	dr::kAudioOps.construct(mem);
	dr::kAudioOps.prepare(mem, 48000.0, 4096);
	REQUIRE(dr::kAudioOps.setFieldWindows != nullptr);
	std::array<float, std::size(dr::kParamDescs)> row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(dr::kParamDescs[i].def);
	}
	row[static_cast<size_t>(dr::Param::Enabled)] = 1.0f;
	row[static_cast<size_t>(dr::Param::Cutoff)] = s.cutoff;
	row[static_cast<size_t>(dr::Param::Drive)] = s.drive;
	dr::kAudioOps.setFields(mem, row.data(), row.size());
	std::array<const float*, std::size(dr::kParamDescs)> runs {};
	runs[static_cast<size_t>(dr::Param::Drive)] = run.data();
	std::vector<float> windowedL = in, windowedR = in;
	for (int at = 0; at < kTotal; at += kFrames) {
		dr::kAudioOps.setFieldWindows(mem, runs.data(), runs.size(), kFrames);
		dr::kAudioOps.processStereo(mem, windowedL.data() + at, windowedR.data() + at, kFrames);
	}

	dr::DrawnFilter twin;
	twin.prepare(48000.0);
	twin.setTarget(dr::targetForTest(mem)); // the same settings the ops decoded, drive aside
	std::vector<float> twinL = in, twinR = in;
	float apart = 0.0f;
	for (int i = 0; i < kTotal; ++i) {
		dr::Settings at = dr::targetForTest(mem);
		at.drive = run[static_cast<size_t>(i % kFrames)];
		twin.setTarget(at);
		twin.processStereo(twinL.data() + i, twinR.data() + i, 1);
		apart = std::max(apart, std::abs(twinL[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]));
	}
	dr::kAudioOps.destruct(mem);
	float worst = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		worst = std::max(worst, std::abs(windowedL[i] - twinL[i]));
	}
	CAPTURE(worst);
	CHECK(apart > 0.05f); // not vacuous
	CHECK(worst < 1e-6f);
}

//==================================================================================================
// THE IDLE SKIP (backlog PB 3, 2026-10-29): an unmoved frame is not realised again, to the bit
//==================================================================================================

namespace {

/// ONE SCRIPT, ON THE REFERENCE ROAD (every control block realised) OR NOT: rest at the defaults, a drawn curve the smoother
/// walks to (Wake up, so the zeros are still walking after the poles have landed), rest, then the puck automated from rest
/// block by block, then rest again — noise in, in host blocks that never line up with the 32-sample grid. Returns both
/// channels interleaved, and the realisations at the end of each of the six phases.
struct IdleScript {
	std::vector<float> out;
	std::array<uint64_t, 6> realisations {};
};

IdleScript idleScript(bool reference) {
	Ops ops;
	dr::setReferenceForTest(ops.storage.data(), reference);
	IdleScript r;
	std::mt19937 rng(77u);
	std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
	const int blocks[] = { 256, 97, 1, 33, 512, 7 };
	size_t b = 0;
	const auto run = [&](int samples, auto&& beforeBlock) {
		for (int at = 0; at < samples;) {
			const int n = std::min(blocks[b++ % std::size(blocks)], samples - at);
			beforeBlock(at, n);
			std::vector<float> l(static_cast<size_t>(n));
			std::vector<float> rr(static_cast<size_t>(n));
			for (int i = 0; i < n; ++i) {
				l[static_cast<size_t>(i)] = noise(rng);
				rr[static_cast<size_t>(i)] = noise(rng);
			}
			ops.process(l.data(), rr.data(), n);
			for (int i = 0; i < n; ++i) {
				r.out.push_back(l[static_cast<size_t>(i)]);
				r.out.push_back(rr[static_cast<size_t>(i)]);
			}
			at += n;
		}
	};
	const auto none = [](int, int) {};
	ops.fields(dr::Settings {});
	run(24000, none); // AT REST, the defaults
	r.realisations[0] = dr::realisationsForTest(ops.storage.data());
	std::mt19937 curves(4321u);
	dr::Settings drawn = fourDistinctFrames(curves);
	drawn.wake = 0.7f;
	drawn.morphX = 0.2f;
	ops.fields(drawn); // A DRAWN CURVE, walked to
	run(24000, none);
	r.realisations[1] = dr::realisationsForTest(ops.storage.data());
	run(192000, none); // AT REST again, 4 s: the zeros (a 0.13 s pole at Wake 0.7) take ~17 time constants to park on the ulp
	r.realisations[2] = dr::realisationsForTest(ops.storage.data());
	run(24000, [&](int at, int) { // THE PUCK AUTOMATED FROM REST, a block at a time
		dr::Settings s = drawn;
		s.morphX = 0.2f + 0.6f * static_cast<float>(at) / 24000.0f;
		s.cutoff = 0.25f * std::sin(static_cast<float>(at) * 0.0005f);
		ops.fields(s);
	});
	r.realisations[3] = dr::realisationsForTest(ops.storage.data());
	run(192000, none); // AND REST
	r.realisations[4] = dr::realisationsForTest(ops.storage.data());
	run(24000, none); // AND PARKED: nothing moves now
	r.realisations[5] = dr::realisationsForTest(ops.storage.data());
	return r;
}

} // namespace

TEST_CASE("the idle skip: an unmoved frame is not realised again, and the output is the reference road's to the bit") {
	const IdleScript reference = idleScript(true);
	const IdleScript fast = idleScript(false);
	REQUIRE(reference.out.size() == fast.out.size());
	size_t differing = 0;
	for (size_t i = 0; i < fast.out.size(); ++i) {
		differing += std::memcmp(&reference.out[i], &fast.out[i], sizeof(float)) != 0 ? 1 : 0;
	}
	CHECK(differing == 0);
	// THE SKIP SKIPS: at rest the fast road realises (nearly) nothing, where the reference realises every control block.
	const auto phase = [](const IdleScript& s, size_t k) { return s.realisations[k] - (k == 0 ? 0 : s.realisations[k - 1]); };
	CHECK(phase(reference, 0) >= 24000 / dr::kControlBlock - 1);
	CHECK(phase(fast, 0) == 0);
	// PARKED within each rest (the frame lands in its first ~2 s), and then nothing at all.
	CHECK(phase(fast, 2) < phase(reference, 2) / 2);
	CHECK(phase(fast, 4) < phase(reference, 4) / 2);
	CHECK(phase(fast, 5) == 0);
	CHECK(phase(reference, 5) >= 24000 / dr::kControlBlock - 1);
	// AND IT WAKES: the drawn curve and the automation each realise, every block they move.
	CHECK(phase(fast, 1) > 100);
	CHECK(phase(fast, 3) >= phase(reference, 3) * 9 / 10);
	// NON-VACUITY: the output is not silence, and the curve and the automation changed it.
	float peak = 0.0f;
	for (const float v : fast.out) {
		peak = std::max(peak, std::fabs(v));
	}
	CHECK(peak > 0.05f);
}
