/*
 * Copyright © 2026 Owlet Records
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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/dyn/oversampler.h"
#include "fx/eroder/drive_stage.h"
#include "fx/eroder/eroder.h"
#include "fx/eroder/eroder_types.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

// **THE THIRD ROW OF EVERY TYPE'S CELL** (Hard, Sharp, Bias, Rate on Lean, and the Key and Spread of the four rate Types). Every expectation is a restatement
// in double precision, written here from the description in `eroder_types.h` and not from the code under test.

namespace {

using namespace feathers;
using namespace feathers::fx::eroder;

constexpr double kRate = 48000.0;
constexpr double kPi = std::numbers::pi;

//--------------------------------------------------------------------------------------------------//
// THE CURVES, RESTATED.                                                                              //
//--------------------------------------------------------------------------------------------------//

/// A triangle folder of period four through the origin at a slope of one, written by its pieces (not by asin(sin)).
double refTriangle(double x) {
	double t = std::fmod(x + 1.0, 4.0);
	if (t < 0.0) {
		t += 4.0;
	}
	return t < 2.0 ? t - 1.0 : 3.0 - t;
}
double refKnee(double x, double hard) { return std::tanh(x) + hard * (std::clamp(x, -1.0, 1.0) - std::tanh(x)); }

struct RefExtras {
	double hard = 0.0;
	double sharp = 0.0;
	double bias = 0.0;
};
/// The curve of `mode` for the driven input `u` (the stage's gain already applied), Shape `shape`, with the third row's values.
double refCurve(DriveMode mode, double u, double shape, const RefExtras& x) {
	switch (mode) {
	case DriveMode::Soft: {
		const double gainFree = 0.0; // (the dead zone is a Shape matter; these cases use Shape 0)
		(void)gainFree;
		return refKnee(u, x.hard);
	}
	case DriveMode::Tube: {
		if (u >= 0.0) {
			return refKnee(u, x.hard);
		}
		const double r = 1.5 * std::pow(8.0 / 1.5, shape);
		return refKnee(r * u, x.hard) / r;
	}
	case DriveMode::Fold: {
		const double b = 0.75 * shape;
		const double sine = std::sin(0.5 * kPi * (u + b)) - std::sin(0.5 * kPi * b);
		const double tri = refTriangle(u + b) - refTriangle(b);
		return sine + x.sharp * (tri - sine);
	}
	case DriveMode::Wreck: {
		// (the bias point's own output is taken out: the curve goes through zero)
		const double f = 1.0 + 4.0 * shape;
		return std::sin(0.5 * kPi * std::tanh(u + x.bias) * f) - std::sin(0.5 * kPi * std::tanh(x.bias) * f);
	}
	case DriveMode::Jag: {
		const double f = 1.0 + 4.0 * shape;
		return refTriangle(std::tanh(u + x.bias) * f) - refTriangle(std::tanh(x.bias) * f);
	}
	default:
		return u;
	}
}
/// The make-up: the reference sine (peak 0.5) through the curve at gain `g`, its mean taken out, back to the RMS it went in with.
double refMakeUp(DriveMode mode, double g, double shape, const RefExtras& x) {
	constexpr int kN = 4096;
	double sum = 0.0;
	double sq = 0.0;
	for (int i = 0; i < kN; ++i) {
		const double y = refCurve(mode, g * 0.5 * std::sin(2.0 * kPi * (i + 0.5) / kN), shape, x);
		sum += y;
		sq += y * y;
	}
	const double mean = sum / kN;
	return std::sqrt(0.125 / (sq / kN - mean * mean));
}

//--------------------------------------------------------------------------------------------------//
// A STAGE RUN.                                                                                       //
//--------------------------------------------------------------------------------------------------//

template <class Gen>
std::vector<float> runLeft(const DriveSettings& d, size_t frames, Gen gen, std::vector<float>* right = nullptr) {
	DriveStage stage;
	stage.prepare(kRate);
	stage.setTrimOnForTest(false);
	stage.set(d);
	std::vector<float> out;
	out.reserve(frames);
	for (size_t i = 0; i < frames; ++i) {
		float l = 0.0f;
		float r = 0.0f;
		gen(i, l, r);
		stage.process(l, r);
		out.push_back(l);
		if (right != nullptr) {
			right->push_back(r);
		}
	}
	return out;
}
float sineAt(size_t i, double hz, double peak) {
	return static_cast<float>(peak * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
}
/// The magnitude of the component at `hz` over `x[from, from + n)` (a whole number of its periods).
double toneAt(const std::vector<float>& x, size_t from, size_t n, double hz) {
	double re = 0.0;
	double im = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double w = 2.0 * kPi * hz * static_cast<double>(from + i) / kRate;
		re += x[from + i] * std::cos(w);
		im += x[from + i] * std::sin(w);
	}
	return 2.0 * std::hypot(re, im) / static_cast<double>(n);
}
DriveSettings driveWith(float knob, DriveMode mode, float shape, const CurveExtras& c) {
	return deriveDrive(knob, mode, shape, kRate, nullptr, 1.0f, DriveExtras { c });
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// (a) HARD, SHARP, BIAS.                                                                             //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("Hard: 0% is the tanh the Type always was, to the bit; 100% is the hard clip at the same ceiling; between is the restated blend") {
	for (const float u : { -7.3f, -1.6f, -0.4f, 0.05f, 0.7f, 1.3f, 2.9f, 11.0f }) {
		CAPTURE(u);
		// TODAY'S BITS: the tanh itself, and Tube's two halves as they were written.
		CHECK(shapeDrive(DriveMode::Soft, u, 0.0f, 1.0f, CurveExtras { 0.0f, 0.0f, 0.0f }) == std::tanh(u));
		const float ratio = 1.5f * std::pow(8.0f / 1.5f, 0.6f);
		const float oldTube = u >= 0.0f ? std::tanh(u) : std::tanh(ratio * u) / ratio;
		CHECK(shapeDrive(DriveMode::Tube, u, 0.6f, 1.0f, CurveExtras {}) == oldTube);
		for (const float hard : { 0.25f, 0.5f, 1.0f }) {
			CAPTURE(hard);
			const CurveExtras c { hard, 0.0f, 0.0f };
			CHECK(std::abs(static_cast<double>(shapeDrive(DriveMode::Soft, u, 0.0f, 1.0f, c)) -
			               refCurve(DriveMode::Soft, u, 0.0, { hard })) < 2.0e-6);
			CHECK(std::abs(static_cast<double>(shapeDrive(DriveMode::Tube, u, 0.6f, 1.0f, c)) -
			               refCurve(DriveMode::Tube, u, 0.6, { hard })) < 2.0e-6);
		}
		// THE HARD CLIP ITSELF at 100%, written as a clamp (Tube's negative half at its own ceiling, 1 / r).
		CHECK(shapeDrive(DriveMode::Soft, u, 0.0f, 1.0f, CurveExtras { 1.0f, 0.0f, 0.0f }) == std::clamp(u, -1.0f, 1.0f));
	}
	// THE SMALL-SIGNAL SLOPE STAYS ONE, and the ceiling stays one.
	for (const float hard : { 0.0f, 0.4f, 1.0f }) {
		const CurveExtras c { hard, 0.0f, 0.0f };
		CHECK(std::abs(shapeDrive(DriveMode::Soft, 1.0e-4f, 0.0f, 1.0f, c) / 1.0e-4f - 1.0f) < 1.0e-3f);
		CHECK(std::abs(shapeDrive(DriveMode::Tube, -1.0e-4f, 0.5f, 1.0f, c) / -1.0e-4f - 1.0f) < 1.0e-3f);
		CHECK(shapeDrive(DriveMode::Soft, 1000.0f, 0.0f, 1.0f, c) == doctest::Approx(1.0).epsilon(1.0e-6));
	}
}

TEST_CASE("Hard: the stage is level-matched at every Hard (the make-up is the restated one) and the oversampled output has the restated harmonics") {
	for (const DriveMode mode : { DriveMode::Soft, DriveMode::Tube }) {
		for (const float hard : { 0.0f, 0.5f, 1.0f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(hard);
			const float shape = mode == DriveMode::Soft ? 0.0f : 0.4f; // (Soft's Shape is a dead zone, which the restatement leaves out)
			const DriveSettings d = driveWith(0.8f, mode, shape, CurveExtras { hard, 0.0f, 0.0f });
			const double want = refMakeUp(mode, d.gain, static_cast<double>(shape), { hard });
			CHECK(static_cast<double>(d.norm) == doctest::Approx(want).epsilon(2.0e-3));
			// THE LEVEL: a 0.5-peak sine comes out at the RMS it went in with.
			const std::vector<float> y = runLeft(d, 48000, [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); });
			double sq = 0.0;
			double sum = 0.0;
			for (size_t i = 24000; i < 48000; ++i) {
				sum += y[i];
				sq += static_cast<double>(y[i]) * y[i];
			}
			const double mean = sum / 24000.0;
			CHECK(std::sqrt(sq / 24000.0 - mean * mean) == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
		}
	}
	// THE HARD CLIP IS BRIGHTER: its 5th harmonic at 100% exceeds the tanh's (the restated ratio is also exceeded, as a bound on the sign of the effect).
	const auto third = [](float hard) {
		const DriveSettings d = driveWith(0.8f, DriveMode::Soft, 0.0f, CurveExtras { hard, 0.0f, 0.0f });
		const std::vector<float> y = runLeft(d, 48000, [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); });
		return toneAt(y, 24000, 4800, 5000.0);
	};
	const double soft = third(0.0f);
	const double hard = third(1.0f);
	// RESTATED: the same sine through the reference curve and its make-up, the 3rd component by DFT in double.
	const auto refThird = [](double h) {
		const double g = std::exp2(-3.0 + 7.0 * 0.8);
		const double mk = refMakeUp(DriveMode::Soft, g, 0.0, { h });
		double re = 0.0;
		double im = 0.0;
		constexpr int kN = 4800;
		for (int i = 0; i < kN; ++i) {
			const double y = mk * refCurve(DriveMode::Soft, g * 0.5 * std::sin(2.0 * kPi * i / 48.0), 0.0, { h });
			re += y * std::cos(2.0 * kPi * 5.0 * i / 48.0);
			im += y * std::sin(2.0 * kPi * 5.0 * i / 48.0);
		}
		return 2.0 * std::hypot(re, im) / kN;
	};
	CHECK(soft == doctest::Approx(refThird(0.0)).epsilon(0.05));
	CHECK(hard == doctest::Approx(refThird(1.0)).epsilon(0.12));
	CHECK(hard > 1.2 * soft); // NON-VACUOUS: the knob moved something
}

TEST_CASE("Sharp: 0% is the sine folder to the bit; 100% is the restated triangle folder with the same fold points; the stage runs it") {
	for (const float shape : { 0.0f, 0.5f, 1.0f }) {
		for (const float u : { -5.1f, -1.2f, -0.3f, 0.2f, 0.9f, 1.4f, 3.3f, 6.0f }) {
			CAPTURE(shape);
			CAPTURE(u);
			const float bias = 0.75f * shape;
			const float oldFold = std::sin(static_cast<float>(kPi * 0.5) * (u + bias)) - std::sin(static_cast<float>(kPi * 0.5) * bias);
			CHECK(shapeDrive(DriveMode::Fold, u, shape, 1.0f, CurveExtras {}) == oldFold);
			for (const float sharp : { 0.3f, 1.0f }) {
				CHECK(std::abs(static_cast<double>(shapeDrive(DriveMode::Fold, u, shape, 1.0f, CurveExtras { 0.0f, sharp, 0.0f })) -
				               refCurve(DriveMode::Fold, u, shape, { 0.0, sharp })) < 5.0e-6);
			}
		}
	}
	// THE FOLD POINTS ARE THE SINE'S: both peak at u = 1 (no bias) at one, and cross zero at 0 and 2.
	for (const float sharp : { 0.0f, 1.0f }) {
		const CurveExtras c { 0.0f, sharp, 0.0f };
		CHECK(shapeDrive(DriveMode::Fold, 1.0f, 0.0f, 1.0f, c) == doctest::Approx(1.0).epsilon(1.0e-6));
		CHECK(std::abs(shapeDrive(DriveMode::Fold, 2.0f, 0.0f, 1.0f, c)) < 1.0e-5f);
		CHECK(shapeDrive(DriveMode::Fold, 3.0f, 0.0f, 1.0f, c) == doctest::Approx(-1.0).epsilon(1.0e-5));
	}
	// THE STAGE: the triangle folder's odd harmonics fall off as 1/n^2, the sine's far faster (Bessel); the 5th is the cheap witness.
	const auto fifth = [](float sharp) {
		const DriveSettings d = driveWith(0.8f, DriveMode::Fold, 0.0f, CurveExtras { 0.0f, sharp, 0.0f });
		const std::vector<float> y = runLeft(d, 48000, [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); });
		return toneAt(y, 24000, 4800, 5000.0);
	};
	const double sine5 = fifth(0.0f);
	const double tri5 = fifth(1.0f);
	const auto refFifth = [](double s) {
		const double g = std::exp2(-3.0 + 7.0 * 0.8);
		const double mk = refMakeUp(DriveMode::Fold, g, 0.0, { 0.0, s });
		double re = 0.0;
		double im = 0.0;
		constexpr int kN = 4800;
		for (int i = 0; i < kN; ++i) {
			const double y = mk * refCurve(DriveMode::Fold, g * 0.5 * std::sin(2.0 * kPi * i / 48.0), 0.0, { 0.0, s });
			re += y * std::cos(2.0 * kPi * 5.0 * i / 48.0);
			im += y * std::sin(2.0 * kPi * 5.0 * i / 48.0);
		}
		return 2.0 * std::hypot(re, im) / kN;
	};
	CHECK(tri5 == doctest::Approx(refFifth(1.0)).epsilon(0.12));
	CHECK(sine5 == doctest::Approx(refFifth(0.0)).epsilon(0.12));
	CHECK(std::abs(tri5 - sine5) > 0.01); // NON-VACUOUS
}

TEST_CASE("Bias: 0 is Wreck and Jag to the bit; above 0 a bias into the tanh gives the restated 2nd harmonic on a sine, and the offset goes to the DC block") {
	for (const float u : { -3.0f, -0.7f, 0.1f, 0.8f, 2.4f }) {
		for (const float shape : { 0.0f, 0.6f }) {
			const float f = 1.0f + 4.0f * shape;
			const float hp = static_cast<float>(kPi * 0.5);
			CHECK(shapeDrive(DriveMode::Wreck, u, shape, 1.0f, CurveExtras {}) == std::sin(hp * std::tanh(u) * f));
			CHECK(shapeDrive(DriveMode::Jag, u, shape, 1.0f, CurveExtras {}) == std::asin(std::sin(hp * (std::tanh(u) * f))) / hp);
			for (const float bias : { 0.3f, 1.0f }) {
				for (const DriveMode m : { DriveMode::Wreck, DriveMode::Jag }) {
					CHECK(std::abs(static_cast<double>(shapeDrive(m, u, shape, 1.0f, CurveExtras { 0.0f, 0.0f, bias })) -
					               refCurve(m, u, shape, { 0.0, 0.0, static_cast<double>(bias) })) < 3.0e-5);
				}
			}
		}
	}
	// THE 2nd HARMONIC of a sine through the biased pair, by DFT of the restated curve, against the stage's (DC block on, so the offset is gone).
	for (const DriveMode mode : { DriveMode::Wreck, DriveMode::Jag }) {
		const auto second = [mode](double bias, bool stage) {
			const double g = std::exp2(-3.0 + 7.0 * 0.5);
			constexpr int kN = 4800;
			if (stage) {
				const DriveSettings d = driveWith(0.5f, mode, 0.0f, CurveExtras { 0.0f, 0.0f, static_cast<float>(bias) });
				const std::vector<float> y = runLeft(d, 96000, [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); });
				return toneAt(y, 72000, kN, 2000.0);
			}
			const double mk = refMakeUp(mode, g, 0.0, { 0.0, 0.0, bias });
			double re = 0.0;
			double im = 0.0;
			for (int i = 0; i < kN; ++i) {
				const double y = mk * refCurve(mode, g * 0.5 * std::sin(2.0 * kPi * i / 48.0), 0.0, { 0.0, 0.0, bias });
				re += y * std::cos(2.0 * kPi * 2.0 * i / 48.0);
				im += y * std::sin(2.0 * kPi * 2.0 * i / 48.0);
			}
			return 2.0 * std::hypot(re, im) / kN;
		};
		CAPTURE(static_cast<int>(mode));
		const double none = second(0.0, true);
		const double some = second(1.0, true);
		MESSAGE("2nd harmonic at Bias 0: " << none << ", at 100%: " << some << " (restated " << second(1.0, false) << ")");
		CHECK(none < 1.0e-3); // an odd curve has none
		CHECK(some == doctest::Approx(second(1.0, false)).epsilon(0.08));
		CHECK(some > 0.02); // NON-VACUOUS
		// A SIGNAL THAT IS NOT THERE MAKES NO OFFSET: the curve goes through zero at zero, at any Bias.
		CHECK(shapeDrive(mode, 0.0f, 0.3f, 1.0f, CurveExtras { 0.0f, 0.0f, 1.0f }) == 0.0f);
	}
	// THE OFFSET IS THE DC BLOCK'S: Bias above 0 makes Wreck and Jag offset-making Types (so the one block runs), and the stage's mean is then ~0.
	for (const DriveMode mode : { DriveMode::Wreck, DriveMode::Jag }) {
		CHECK_FALSE(driveMakesOffset(mode));
		CHECK(driveMakesOffset(mode, 0.2f));
		Settings s = stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings;
		s.drive = 0.6f;
		s.driveMode = mode;
		CHECK_FALSE(chainMakesOffset(s));
		s.driveBias = 0.5f;
		CHECK(chainMakesOffset(s));
		const DriveSettings d = driveWith(0.6f, mode, 0.0f, CurveExtras { 0.0f, 0.0f, 1.0f });
		CHECK(makesOffset(d));
		const std::vector<float> y = runLeft(d, 96000, [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); });
		double mean = 0.0;
		for (size_t i = 72000; i < 76800; ++i) {
			mean += y[i];
		}
		CHECK(std::abs(mean / 4800.0) < 0.01);
	}
}

TEST_CASE("The third row's curve values count only for their own Types, and sit in the row and the state like any control") {
	// ANOTHER TYPE'S VALUE NEVER MOVES THE DERIVATION.
	const DriveSettings plain = deriveDrive(0.7f, DriveMode::Tube, 0.4f, kRate);
	const DriveSettings other = deriveDrive(0.7f, DriveMode::Tube, 0.4f, kRate, nullptr, 1.0f, DriveExtras { CurveExtras { 0.0f, 0.9f, 0.9f } });
	CHECK(other.norm == plain.norm);
	CHECK(other.curve == plain.curve);
	// ROW AND STATE round trip, defaults all zero.
	State st {};
	Settings& s = st.settings;
	s.driveHard = 0.1f;
	s.slotA.hard = 0.2f;
	s.slotB.hard = 0.3f;
	s.driveSharp = 0.4f;
	s.slotA.sharp = 0.5f;
	s.slotB.sharp = 0.6f;
	s.driveBias = 0.7f;
	s.slotA.bias = 0.8f;
	s.slotB.bias = 0.9f;
	const State back = stateOf(rowOf(st), static_cast<size_t>(Param::kCount));
	CHECK(back.settings.driveHard == 0.1f);
	CHECK(back.settings.slotB.hard == 0.3f);
	CHECK(back.settings.slotA.sharp == 0.5f);
	CHECK(back.settings.slotB.bias == 0.9f);
	const Settings fresh = stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings;
	CHECK(fresh.driveHard == 0.0f);
	CHECK(fresh.slotA.sharp == 0.0f);
	CHECK(fresh.slotB.bias == 0.0f);
}

//--------------------------------------------------------------------------------------------------//
// (b) LEAN'S RATE.                                                                                   //
//--------------------------------------------------------------------------------------------------//

namespace {
struct Complex2 {
	double re = 0.0;
	double im = 0.0;
};
/// The complex component at `hz` over `x[from, from + n)`, at the absolute sample phase (so two runs are comparable).
Complex2 componentAt(const std::vector<float>& x, size_t from, size_t n, double hz) {
	Complex2 c;
	for (size_t i = 0; i < n; ++i) {
		const double w = 2.0 * kPi * hz * static_cast<double>(from + i) / kRate;
		c.re += x[from + i] * std::cos(w);
		c.im += x[from + i] * std::sin(w);
	}
	c.re *= 2.0 / static_cast<double>(n);
	c.im *= 2.0 / static_cast<double>(n);
	return c;
}
DriveSettings leanWith(float rateHz) {
	DriveSettings d = deriveDrive(0.7f, DriveMode::Lean, 1.0f, kRate);
	d.leaveDc = true; // the offset is not this case's business
	d.rateHz = rateHz;
	return d;
}
} // namespace

TEST_CASE("Lean's Rate: off is today's Lean to the bit (the restated oversampled curve, both sides)") {
	const DriveSettings d = leanWith(0.0f);
	DriveStage stage;
	stage.prepare(kRate);
	stage.setTrimOnForTest(false);
	stage.set(d);
	fx::dyn::Oversampler4x over;
	over.reset();
	for (size_t i = 0; i < 9600; ++i) {
		const float x = sineAt(i, 700.0, 0.6) + 0.1f * sineAt(i, 1900.0, 1.0);
		float l = x;
		float r = 0.8f * x;
		stage.process(l, r);
		float wl = x;
		float wr = 0.8f * x;
		const float gain = d.gain;
		const float shape = d.shape;
		over.process(&wl, &wr, 1, [gain, shape](float& a, float& b) {
			a = shapeDrive(DriveMode::Lean, a * gain, shape, gain);
			b = shapeDriveRight(DriveMode::Lean, b * gain, shape, gain);
		});
		wl *= d.norm;
		wr *= d.norm;
		REQUIRE(l == wl);
		REQUIRE(r == wr);
	}
}

TEST_CASE("Lean's Rate: the lean swaps sides by cos(phase) - the left's 2nd harmonic is c E, the right's -c E, the mono sum has none at any instant, the sides stay level over a turn") {
	constexpr double kLfoHz = 1.0;
	constexpr size_t kFrames = 48000; // exactly one turn
	const auto gen = [](size_t i, float& l, float& r) { l = r = sineAt(i, 1000.0, 0.5); };
	std::vector<float> still;
	std::vector<float> stillR;
	const std::vector<float> stillL = runLeft(leanWith(0.0f), kFrames, gen, &stillR);
	std::vector<float> turnR;
	const std::vector<float> turnL = runLeft(leanWith(static_cast<float>(kLfoHz)), kFrames, gen, &turnR);
	// WINDOWS of 480 samples (ten periods); the LFO moves 1 % of a turn across one, so c is read at its middle.
	size_t checked = 0;
	for (size_t from = 2400; from + 480 <= kFrames; from += 2400) { // (not the first window: the oversampler is settling)
		const double mid = (static_cast<double>(from) + 240.0) / kRate;
		const double c = std::cos(2.0 * kPi * kLfoHz * mid);
		const Complex2 e = componentAt(stillL, from, 480, 2000.0);
		const Complex2 l = componentAt(turnL, from, 480, 2000.0);
		const Complex2 r = componentAt(turnR, from, 480, 2000.0);
		const double eMag = std::hypot(e.re, e.im);
		REQUIRE(eMag > 0.05); // NON-VACUOUS: Lean at Shape 100% has a 2nd harmonic
		CAPTURE(from);
		CAPTURE(c);
		// THE LEFT'S IS c TIMES THE UNROTATED, THE RIGHT'S MINUS c TIMES IT.
		CHECK(std::abs(l.re - (c * e.re)) < 0.03 * eMag);
		CHECK(std::abs(l.im - (c * e.im)) < 0.03 * eMag);
		CHECK(std::abs(r.re - (-c * e.re)) < 0.03 * eMag);
		CHECK(std::abs(r.im - (-c * e.im)) < 0.03 * eMag);
		// THE MONO SUM'S 2nd harmonic is nil.
		CHECK(std::hypot(l.re + r.re, l.im + r.im) < 0.01 * eMag);
		++checked;
	}
	CHECK(checked == 19);
	// OVER THE WHOLE TURN THE TWO SIDES ARE LEVEL (the mean square of each, DC removed).
	const auto power = [](const std::vector<float>& x) {
		double sum = 0.0;
		double sq = 0.0;
		for (const float v : x) {
			sum += v;
			sq += static_cast<double>(v) * v;
		}
		const double m = sum / static_cast<double>(x.size());
		return sq / static_cast<double>(x.size()) - m * m;
	};
	CHECK(power(turnL) == doctest::Approx(power(turnR)).epsilon(0.01));
	// AND THE DIRECTION SWAPS: the first quarter turn leans left (+), the third leans right.
	const Complex2 early = componentAt(turnL, 0, 480, 2000.0);
	const Complex2 late = componentAt(turnL, 24000, 480, 2000.0);
	const Complex2 e0 = componentAt(stillL, 0, 480, 2000.0);
	CHECK(early.re * e0.re + early.im * e0.im > 0.0);
	CHECK(late.re * e0.re + late.im * e0.im < 0.0);
}

//--------------------------------------------------------------------------------------------------//
// (c) KEY, SPREAD AND THE TUNED EDGE.                                                                //
//--------------------------------------------------------------------------------------------------//

namespace {

constexpr double kNoteHz = 220.0;

/// A deterministic noise in [-0.5, 0.5): a different stream per `seed`.
float noiseAt(size_t i, uint32_t seed) {
	uint32_t x = static_cast<uint32_t>(i) * 2654435761u + seed * 40503u + 12345u;
	x ^= x >> 15;
	x *= 2246822519u;
	x ^= x >> 13;
	x *= 3266489917u;
	x ^= x >> 16;
	return static_cast<float>(static_cast<int32_t>(x) >> 8) * (1.0f / 16777216.0f);
}
DriveSettings keyed(DriveMode mode, float knob, float shape, bool key, float spread, double noteHz = kNoteHz) {
	return deriveDrive(knob, mode, shape, kRate, nullptr, 1.0f, DriveExtras { {}, key, noteHz, spread });
}
/// The two channels of a run of noise (distinct streams), `frames` long.
void runNoise(const DriveSettings& d, size_t frames, std::vector<float>& l, std::vector<float>& r) {
	DriveStage stage;
	stage.prepare(kRate);
	stage.setTrimOnForTest(false);
	stage.set(d);
	for (size_t i = 0; i < frames; ++i) {
		float a = noiseAt(i, 1);
		float b = noiseAt(i, 2);
		stage.process(a, b);
		l.push_back(a);
		r.push_back(b);
	}
}
/// THE EDGE TIMES OF A TUNED HOLD, read from the output: an edge sample is the one between two held runs, `old + over (new - old)`, so the edge happened
/// `over` samples before it, at `m - over`, `over = (y[m] - y[m-1]) / (y[m+1] - y[m-1])`. (Holds are at least three frames here, so the edges are apart.)
std::vector<double> edgeTimes(const std::vector<float>& y) {
	std::vector<double> t;
	for (size_t m = 3; m + 3 < y.size(); ++m) {
		if (y[m - 1] == y[m - 2] && y[m + 1] == y[m + 2] && y[m] != y[m - 1] && y[m + 1] != y[m] && y[m + 1] != y[m - 1]) {
			t.push_back(static_cast<double>(m) - static_cast<double>(y[m] - y[m - 1]) / static_cast<double>(y[m + 1] - y[m - 1]));
		}
	}
	return t;
}
/// The mean spacing of edge times (they are consecutive holds, so a missed edge would double a spacing: the spacing is taken between neighbours and the
/// median-sized ones kept).
double meanSpacing(const std::vector<double>& t) {
	std::vector<double> gaps;
	for (size_t i = 1; i < t.size(); ++i) {
		gaps.push_back(t[i] - t[i - 1]);
	}
	std::sort(gaps.begin(), gaps.end());
	const double median = gaps[gaps.size() / 2];
	double sum = 0.0;
	size_t n = 0;
	for (const double g : gaps) {
		if (g < 1.5 * median) {
			sum += g;
			++n;
		}
	}
	return sum / static_cast<double>(n);
}
/// The number of changes of an unkeyed (whole-sample) hold per second: its rate.
double changesPerSecond(const std::vector<float>& y) {
	size_t n = 0;
	for (size_t i = 1; i < y.size(); ++i) {
		n += y[i] != y[i - 1] ? 1 : 0;
	}
	return static_cast<double>(n) * kRate / static_cast<double>(y.size());
}

//--- Today's Crush, Decimate and Slew, restated -------------------------------------------------------

struct OldHold {
	double phase = 0.0;
	float held = 0.0f;
	float prev = 0.0f;
};
void oldCrush(OldHold& h, double hold, float step, float in, float& out) {
	h.phase += 1.0;
	if (h.phase >= hold) {
		h.phase -= hold;
		h.held = std::round(in / step) * step;
	}
	out = h.held;
}
void oldDecimate(OldHold& h, double hold, float shape, float in, float& out) {
	h.phase += 1.0;
	if (h.phase >= hold) {
		h.phase -= hold;
		h.prev = h.held;
		h.held = in;
	}
	if (shape <= 0.0f) {
		out = h.held;
		return;
	}
	const auto t = static_cast<float>(h.phase / hold);
	const float line = h.prev + (h.held - h.prev) * t;
	out = shape >= 1.0f ? line : h.held + shape * (line - h.held);
}
struct OldSlew {
	float up = 0.0f;
	float down = 0.0f;
	float linUp = 0.0f;
	float linDown = 0.0f;
	float y = 0.0f;
	OldSlew(float hz, float curve, float balance) {
		const double h = std::clamp(static_cast<double>(hz), 0.01, kRate * 0.45);
		const float limit = static_cast<float>(4.0 * 0.5 * h / kRate);
		const float k = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * h / kRate));
		const float u = std::exp2(balance);
		const float d = std::exp2(-balance);
		up = (1.0f - curve) * limit * u;
		down = (1.0f - curve) * limit * d;
		linUp = curve * k * u;
		linDown = curve * k * d;
	}
	float process(float x) {
		const float e = x - y;
		const float m = std::abs(e);
		if (e >= 0.0f) {
			y += std::min(m, up + linUp * m);
		}
		else {
			y -= std::min(m, down + linDown * m);
		}
		return y;
	}
};

} // namespace

TEST_CASE("Key off and Spread 0: Crush, Decimate, Slew and Lopside are today's bits, both sides") {
	for (const float knob : { 0.2f, 0.5f, 0.83f }) {
		for (const float shape : { 0.0f, 0.4f, 1.0f }) {
			CAPTURE(knob);
			CAPTURE(shape);
			{
				const DriveSettings d = keyed(DriveMode::Crush, knob, shape, false, 0.0f);
				std::vector<float> l;
				std::vector<float> r;
				runNoise(d, 4000, l, r);
				OldHold hl;
				OldHold hr;
				for (size_t i = 0; i < l.size(); ++i) {
					float a = 0.0f;
					float b = 0.0f;
					oldCrush(hl, d.crushHoldFrames, d.crushStep, noiseAt(i, 1), a);
					oldCrush(hr, d.crushHoldFrames, d.crushStep, noiseAt(i, 2), b);
					REQUIRE(l[i] == a);
					REQUIRE(r[i] == b);
				}
			}
			{
				const DriveSettings d = keyed(DriveMode::Decimate, knob, shape, false, 0.0f);
				std::vector<float> l;
				std::vector<float> r;
				runNoise(d, 4000, l, r);
				OldHold hl;
				OldHold hr;
				for (size_t i = 0; i < l.size(); ++i) {
					float a = 0.0f;
					float b = 0.0f;
					oldDecimate(hl, d.crushHoldFrames, d.shape, noiseAt(i, 1), a);
					oldDecimate(hr, d.crushHoldFrames, d.shape, noiseAt(i, 2), b);
					REQUIRE(l[i] == a);
					REQUIRE(r[i] == b);
				}
			}
			for (const DriveMode mode : { DriveMode::Slew, DriveMode::Lopside }) {
				const DriveSettings d = keyed(mode, knob, shape, false, 0.0f);
				std::vector<float> l;
				std::vector<float> r;
				runNoise(d, 4000, l, r);
				OldSlew sl(d.slewHz, d.slewCurve, d.slewBalance);
				OldSlew sr(d.slewHz, d.slewCurve, d.slewBalance);
				for (size_t i = 0; i < l.size(); ++i) {
					REQUIRE(l[i] == sl.process(noiseAt(i, 1)));
					REQUIRE(r[i] == sr.process(noiseAt(i, 2)));
				}
			}
		}
	}
}

TEST_CASE("Key: the multiple runs exponentially from 32x the note at the bottom of the knob to a quarter at the top, and the derived rates are note x multiple") {
	CHECK(keyMultiple(0.0) == doctest::Approx(32.0));
	CHECK(keyMultiple(1.0) == doctest::Approx(0.25));
	CHECK(keyMultiple(0.5) == doctest::Approx(std::sqrt(32.0 * 0.25)));
	// EXPONENTIAL: equal steps of the knob are equal ratios; CONTINUOUS: nothing snaps.
	const double step = keyMultiple(0.30) / keyMultiple(0.31);
	CHECK(keyMultiple(0.62) / keyMultiple(0.63) == doctest::Approx(step));
	CHECK(keyMultiple(0.300) / keyMultiple(0.301) > 1.0);
	for (const float knob : { 0.1f, 0.37f, 0.5f, 0.711f, 0.95f }) {
		CAPTURE(knob);
		const double mult = 32.0 * std::pow(0.25 / 32.0, static_cast<double>(knob)); // restated
		const DriveSettings c = keyed(DriveMode::Crush, knob, 0.5f, true, 0.0f);
		CHECK(c.crushHoldFrames == doctest::Approx(kRate / (kNoteHz * mult)).epsilon(1.0e-9));
		const DriveSettings s = keyed(DriveMode::Slew, knob, 0.0f, true, 0.0f);
		CHECK(static_cast<double>(s.slewHz) == doctest::Approx(kNoteHz * mult).epsilon(1.0e-6));
		CHECK(c.keyed);
		CHECK(c.crushHoldFramesRight == c.crushHoldFrames);
		CHECK(s.slewHzRight == s.slewHz);
	}
	// WITHOUT A NOTE, or on another Type, the switch is nothing.
	CHECK_FALSE(keyed(DriveMode::Crush, 0.5f, 0.0f, true, 0.0f, 0.0).keyed);
	CHECK_FALSE(keyed(DriveMode::Soft, 0.5f, 0.0f, true, 0.5f).keyed);
	CHECK(keyed(DriveMode::Crush, 0.5f, 0.0f, false, 0.0f).crushHoldFrames == deriveDrive(0.5f, DriveMode::Crush, 0.0f, kRate).crushHoldFrames);
}

TEST_CASE("Key: the hold rate MEASURED from the output is the note times the multiple, in Crush and Decimate, at several knob positions") {
	// 220 Hz: holds of 18 to 270 frames across these; a non-integer multiple (0.5 is x2.83) and sub-octave ones (0.9 is x0.41) are among them.
	for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate }) {
		for (const float knob : { 0.25f, 0.5f, 0.63f, 0.9f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(knob);
			const double mult = 32.0 * std::pow(0.25 / 32.0, static_cast<double>(knob));
			const DriveSettings d = keyed(mode, knob, 0.0f, true, 0.0f);
			std::vector<float> l;
			std::vector<float> r;
			runNoise(d, 96000, l, r);
			const std::vector<double> edges = edgeTimes(l);
			REQUIRE(edges.size() > 100);
			const double measuredHz = kRate / meanSpacing(edges);
			MESSAGE("knob " << knob << ": measured " << measuredHz << " Hz, note x multiple " << kNoteHz * mult);
			CHECK(measuredHz == doctest::Approx(kNoteHz * mult).epsilon(0.002));
		}
	}
}

TEST_CASE("Key: Slew's and Lopside's rate frequency MEASURED from a step's ramp is the note times the multiple") {
	for (const DriveMode mode : { DriveMode::Slew, DriveMode::Lopside }) {
		for (const float knob : { 0.2f, 0.5f, 0.63f, 0.9f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(knob);
			const double mult = 32.0 * std::pow(0.25 / 32.0, static_cast<double>(knob));
			// A HARD LIMIT (Slew's curve at 0; Lopside's is always hard) at balance 0: Slew Shape 0, Lopside Shape 0.5.
			const DriveSettings d = keyed(mode, knob, mode == DriveMode::Slew ? 0.0f : 0.5f, true, 0.0f);
			DriveStage stage;
			stage.prepare(kRate);
			stage.set(d);
			std::vector<float> y;
			for (size_t i = 0; i < 40; ++i) {
				float l = 0.8f;
				float r = 0.8f;
				stage.process(l, r);
				y.push_back(l);
			}
			// THE FIRST STEPS (the ramp is far from its top): the hard limit is 4 * 0.5 * hz / rate a sample.
			const double slope = static_cast<double>(y[2]) - static_cast<double>(y[1]);
			CHECK(slope * kRate / 2.0 == doctest::Approx(kNoteHz * mult).epsilon(0.002));
		}
	}
}

TEST_CASE("Spread: the left and right rates differ by the restated ratio, either way of the Key, and the sides stay level") {
	const auto ratioOf = [](double spread) { return std::exp2(spread * 50.0 / 1200.0); };
	for (const double spread : { 0.5, 1.0 }) {
		CAPTURE(spread);
		// CRUSH AND DECIMATE, Key off: the left holds shorter by the ratio and the right longer, so the rates' quotient is ratio^2.
		for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate }) {
			const DriveSettings d = keyed(mode, 0.5f, 0.0f, false, static_cast<float>(spread));
			std::vector<float> l;
			std::vector<float> r;
			runNoise(d, 96000, l, r);
			const double quotient = changesPerSecond(l) / changesPerSecond(r);
			CHECK(quotient == doctest::Approx(ratioOf(spread) * ratioOf(spread)).epsilon(0.005));
			// AND AROUND THE CENTRE: the geometric mean of the two rates is the unspread rate (symmetric in log frequency).
			const DriveSettings none = keyed(mode, 0.5f, 0.0f, false, 0.0f);
			std::vector<float> nl;
			std::vector<float> nr;
			runNoise(none, 96000, nl, nr);
			CHECK(std::sqrt(changesPerSecond(l) * changesPerSecond(r)) == doctest::Approx(changesPerSecond(nl)).epsilon(0.005));
		}
		// WITH THE KEY ON, from the edge times.
		{
			const DriveSettings d = keyed(DriveMode::Decimate, 0.5f, 0.0f, true, static_cast<float>(spread));
			std::vector<float> l;
			std::vector<float> r;
			runNoise(d, 96000, l, r);
			const double mult = 32.0 * std::pow(0.25 / 32.0, 0.5);
			CHECK(kRate / meanSpacing(edgeTimes(l)) == doctest::Approx(kNoteHz * mult * ratioOf(spread)).epsilon(0.003));
			CHECK(kRate / meanSpacing(edgeTimes(r)) == doctest::Approx(kNoteHz * mult / ratioOf(spread)).epsilon(0.003));
		}
		// SLEW: the hard limits are in the ratio; a sine's level is the same on both sides within a small bound.
		{
			const DriveSettings d = keyed(DriveMode::Slew, 0.8f, 0.0f, false, static_cast<float>(spread));
			CHECK(static_cast<double>(d.slewHz) / static_cast<double>(d.slewHzRight) == doctest::Approx(ratioOf(spread) * ratioOf(spread)).epsilon(1.0e-5));
			std::vector<float> l;
			std::vector<float> r;
			const std::vector<float> yl = runLeft(d, 48000, [](size_t i, float& a, float& b) { a = b = sineAt(i, 500.0, 0.5); }, &r);
			const auto rms = [](const std::vector<float>& x) {
				double sq = 0.0;
				for (size_t i = 24000; i < x.size(); ++i) {
					sq += static_cast<double>(x[i]) * x[i];
				}
				return std::sqrt(sq / static_cast<double>(x.size() - 24000));
			};
			const double db = 20.0 * std::log10(rms(yl) / rms(r));
			MESSAGE("Slew at Spread " << spread << ": left over right " << db << " dB");
			CHECK(std::abs(db) < 1.0);
		}
	}
	// SPREAD 0 IS ONE RATE: both sides count alike (L = R for L = R input, to the bit).
	for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew, DriveMode::Lopside }) {
		std::vector<float> r;
		const std::vector<float> l = runLeft(keyed(mode, 0.4f, 0.3f, true, 0.0f), 4800, [](size_t i, float& a, float& b) { a = b = noiseAt(i, 7); }, &r);
		CHECK(l == r);
	}
}

TEST_CASE("The tuned edge: with Key on at x4 a harmonic input has a lower inharmonic floor than with the whole-sample switch") {
	// 220 Hz at x4: a hold of 48000 / 880 = 54.5454 frames, so the whole-sample switch jitters by a sample. 4800 frames are 22 cycles; the
	// output of an ideal tuned hold is periodic in the input's period, so every bin that is not a multiple of 22 is NOT harmonic.
	const double knob = std::log(4.0 / 32.0) / std::log(0.25 / 32.0); // multiple 4
	CHECK(keyMultiple(knob) == doctest::Approx(4.0));
	const auto floorDb = [&](DriveMode mode, bool tuned) {
		DriveSettings d = keyed(mode, static_cast<float>(knob), 0.0f, true, 0.0f);
		d.keyed = tuned;
		const auto gen = [](size_t i, float& l, float& r) {
			l = r = sineAt(i, 220.0, 0.4) + sineAt(i, 440.0, 0.2) + sineAt(i, 660.0, 0.1);
		};
		const std::vector<float> y = runLeft(d, 9600 + 4800, gen);
		constexpr size_t kN = 4800;
		const size_t from = 9600;
		double harmonic = 0.0;
		double other = 0.0;
		for (size_t bin = 1; bin < kN / 2; ++bin) {
			const double hz = static_cast<double>(bin) * kRate / static_cast<double>(kN);
			if (hz > 20000.0) {
				break;
			}
			double re = 0.0;
			double im = 0.0;
			for (size_t i = 0; i < kN; ++i) {
				const double w = 2.0 * kPi * static_cast<double>(bin) * static_cast<double>(i) / static_cast<double>(kN);
				re += y[from + i] * std::cos(w);
				im += y[from + i] * std::sin(w);
			}
			const double power = re * re + im * im;
			(bin % 22 == 0 ? harmonic : other) += power;
		}
		return 10.0 * std::log10(other / harmonic);
	};
	for (const DriveMode mode : { DriveMode::Decimate, DriveMode::Crush }) {
		const double naive = floorDb(mode, false);
		const double tuned = floorDb(mode, true);
		MESSAGE((mode == DriveMode::Crush ? "Crush" : "Decimate") << " x4 at 220 Hz: inharmonic floor naive " << naive << " dB, tuned edge " << tuned << " dB");
		CHECK(tuned < naive - 10.0);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE: PARTITIONS, TAILS, AND THE LOOPS AT THE EXTREMES.                                      //
//--------------------------------------------------------------------------------------------------//

namespace {

Settings etchWith() {
	Settings s = stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings;
	s.mode = Mode::Etch;
	s.mix = 1.0f;
	s.freq = 10.0f;
	return s;
}
struct Stereo {
	std::vector<float> l;
	std::vector<float> r;
	friend bool operator==(const Stereo&, const Stereo&) = default;
};
/// Renders `in` followed by `tail` zeros. The note is changed at multiples of 480 (a whole number of control grids) and the blocks are cut at `block` inside
/// each such segment, so the only thing that varies between two partitions is where the process calls end.
Stereo renderEtch(const Settings& s, size_t signal, size_t tail, size_t block, bool moveNote) {
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	Stereo b;
	const size_t total = signal + tail;
	b.l.assign(total, 0.0f);
	for (size_t i = 0; i < signal; ++i) {
		b.l[i] = noiseAt(i, 3) * 1.6f;
	}
	b.r.assign(total, 0.0f);
	for (size_t i = 0; i < signal; ++i) {
		b.r[i] = noiseAt(i, 4) * 1.6f;
	}
	for (size_t seg = 0; seg < total; seg += 480) {
		if (moveNote) {
			e.setNoteContext({ 48.0f + 12.0f * std::sin(static_cast<float>(seg) / 9000.0f), true });
		}
		const size_t end = std::min(total, seg + 480);
		for (size_t done = seg; done < end;) {
			const size_t take = std::min(block, end - done);
			e.processStereo(b.l.data() + done, b.r.data() + done, static_cast<int>(take));
			done += take;
		}
	}
	return b;
}
float peakOf(const Stereo& b) {
	float peak = 0.0f;
	for (size_t i = 0; i < b.l.size(); ++i) {
		REQUIRE(std::isfinite(b.l[i]));
		REQUIRE(std::isfinite(b.r[i]));
		peak = std::max({ peak, std::abs(b.l[i]), std::abs(b.r[i]) });
	}
	return peak;
}
bool silentTail(const Stereo& b, size_t n) {
	for (size_t i = b.l.size() - n; i < b.l.size(); ++i) {
		if (b.l[i] != 0.0f || b.r[i] != 0.0f) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("Key and Spread in Etch: any block size gives the same output with the note moving, and the tail ends in exact zero") {
	struct Pair {
		DriveMode a;
		DriveMode b;
	};
	for (const Pair pair : { Pair { DriveMode::Crush, DriveMode::Slew }, Pair { DriveMode::Decimate, DriveMode::Lopside } }) {
		for (const bool parallel : { false, true }) {
			CAPTURE(static_cast<int>(pair.a));
			CAPTURE(parallel);
			Settings s = etchWith();
			s.slotA = { 0.5f, pair.a, 0.3f, 1.0f };
			s.slotA.key = true;
			s.slotA.spread = 0.7f;
			s.slotB = { 0.6f, pair.b, 0.5f, 1.0f };
			s.slotB.key = true;
			s.slotB.spread = 1.0f;
			s.slotParallel = parallel;
			s.drive = 0.7f;
			s.driveMode = DriveMode::Decimate;
			s.driveKey = true;
			s.driveSpread = 0.4f;
			const Stereo whole = renderEtch(s, 24000, 96000, 480, true);
			CHECK(renderEtch(s, 24000, 96000, 1, true) == whole);
			CHECK(renderEtch(s, 24000, 96000, 37, true) == whole);
			CHECK(peakOf(whole) > 0.05f);
			CHECK(peakOf(whole) < 5.0f);
			CHECK(silentTail(whole, 24000));
			// THE NOTE MATTERS (non-vacuous): a still note gives other samples.
			CHECK_FALSE(renderEtch(s, 24000, 96000, 480, false) == whole);
		}
	}
}

TEST_CASE("The third row at its extremes in three chained stages with the loops on: finite, under the wet ceiling, and silent after") {
	struct Setup {
		DriveMode drive;
		DriveMode a;
		DriveMode b;
	};
	for (const Setup setup : { Setup { DriveMode::Soft, DriveMode::Fold, DriveMode::Wreck }, Setup { DriveMode::Tube, DriveMode::Jag, DriveMode::Lean },
	                           Setup { DriveMode::Crush, DriveMode::Slew, DriveMode::Decimate }, Setup { DriveMode::Lopside, DriveMode::Soft, DriveMode::Jag } }) {
		for (const bool tapDriven : { false, true }) {
			CAPTURE(static_cast<int>(setup.drive));
			CAPTURE(tapDriven);
			Settings s = etchWith();
			s.drive = 0.95f;
			s.driveMode = setup.drive;
			s.driveHard = 1.0f;
			s.driveSharp = 1.0f;
			s.driveBias = 1.0f;
			s.driveKey = true;
			s.driveSpread = 1.0f;
			s.driveRate = 4.0f;
			s.slotA = { 0.95f, setup.a, 1.0f, 1.0f };
			s.slotB = { 0.95f, setup.b, 1.0f, 1.0f };
			for (SlotSettings* slot : { &s.slotA, &s.slotB }) {
				slot->hard = 1.0f;
				slot->sharp = 1.0f;
				slot->bias = 1.0f;
				slot->key = true;
				slot->spread = 1.0f;
				slot->rate = 4.0f;
			}
			s.resonance = 0.9f;
			s.loop = 0.5f;
			s.smear = 0.4f;
			s.tapDriven = tapDriven;
			s.emphAmount = 12.0f;
			const Stereo whole = renderEtch(s, 24000, 240000, 480, true);
			const float peak = peakOf(whole);
			MESSAGE("peak " << peak);
			CHECK(peak <= kWetCeiling * 1.001f);
			CHECK(peak > 0.05f);
			{
				float tailPeak = 0.0f;
				for (size_t i = whole.l.size() - 24000; i < whole.l.size(); ++i) {
					tailPeak = std::max({ tailPeak, std::abs(whole.l[i]), std::abs(whole.r[i]) });
				}
				MESSAGE("tail peak " << tailPeak);
			}
			CHECK(silentTail(whole, 24000));
			CHECK(renderEtch(s, 24000, 240000, 37, true) == whole);
		}
	}
}

TEST_CASE("Every third-row control is wired in all three places: it changes the sound on its own Types, in the Drive and in each slot, and is read on no other") {
	enum class Place { Drive, SlotA, SlotB };
	const auto field = [](Settings& s, Place place, float SlotSettings::*slotField, float Settings::*driveField) -> float& {
		return place == Place::Drive ? s.*driveField : (place == Place::SlotA ? s.slotA : s.slotB).*slotField;
	};
	struct Case {
		const char* name;
		DriveMode type;
		DriveMode elsewhere;
		float SlotSettings::*slotField;
		float Settings::*driveField;
	};
	const Case cases[] = {
		{ "Hard on Soft", DriveMode::Soft, DriveMode::Chonk, &SlotSettings::hard, &Settings::driveHard },
		{ "Hard on Tube", DriveMode::Tube, DriveMode::Chonk, &SlotSettings::hard, &Settings::driveHard },
		{ "Sharp on Fold", DriveMode::Fold, DriveMode::Chonk, &SlotSettings::sharp, &Settings::driveSharp },
		{ "Bias on Wreck", DriveMode::Wreck, DriveMode::Chonk, &SlotSettings::bias, &Settings::driveBias },
		{ "Bias on Jag", DriveMode::Jag, DriveMode::Chonk, &SlotSettings::bias, &Settings::driveBias },
		{ "Spread on Crush", DriveMode::Crush, DriveMode::Chonk, &SlotSettings::spread, &Settings::driveSpread },
		{ "Spread on Decimate", DriveMode::Decimate, DriveMode::Chonk, &SlotSettings::spread, &Settings::driveSpread },
		{ "Spread on Slew", DriveMode::Slew, DriveMode::Chonk, &SlotSettings::spread, &Settings::driveSpread },
		{ "Spread on Lopside", DriveMode::Lopside, DriveMode::Chonk, &SlotSettings::spread, &Settings::driveSpread },
	};
	for (const Case& c : cases) {
		for (const Place place : { Place::Drive, Place::SlotA, Place::SlotB }) {
			CAPTURE(c.name);
			CAPTURE(static_cast<int>(place));
			const auto render = [&](DriveMode type, float value) {
				Settings s = etchWith();
				if (place == Place::Drive) {
					s.drive = 0.8f;
					s.driveMode = type;
				}
				else {
					SlotSettings& slot = place == Place::SlotA ? s.slotA : s.slotB;
					slot.amount = 0.8f;
					slot.type = type;
				}
				field(s, place, c.slotField, c.driveField) = value;
				return renderEtch(s, 24000, 4800, 480, true);
			};
			CHECK_FALSE(render(c.type, 0.0f) == render(c.type, 1.0f)); // it is heard on its own Type
			CHECK(render(c.elsewhere, 0.0f) == render(c.elsewhere, 1.0f)); // and read on no other
		}
	}
	// KEY: the switch, in each place, on the four Types that have one (the note moves in this render, so the rate it makes is heard).
	for (const DriveMode type : { DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew, DriveMode::Lopside }) {
		for (const Place place : { Place::Drive, Place::SlotA, Place::SlotB }) {
			CAPTURE(static_cast<int>(type));
			CAPTURE(static_cast<int>(place));
			const auto render = [&](DriveMode t, bool key) {
				Settings s = etchWith();
				if (place == Place::Drive) {
					s.drive = 0.8f;
					s.driveMode = t;
					s.driveKey = key;
				}
				else {
					SlotSettings& slot = place == Place::SlotA ? s.slotA : s.slotB;
					slot.amount = 0.8f;
					slot.type = t;
					slot.key = key;
				}
				return renderEtch(s, 24000, 4800, 480, true);
			};
			CHECK_FALSE(render(type, false) == render(type, true));
			CHECK(render(DriveMode::Chonk, false) == render(DriveMode::Chonk, true));
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE DRIVE'S AMOUNT GLIDES (`kDriveAmountGlideSeconds`).                                             //
//--------------------------------------------------------------------------------------------------//

namespace {

constexpr double kGlideTau = 0.02; // the time constant, written here from the description and not read from the code

/// What a one-pole of time constant `kGlideTau` has reached `k` frames after a step from `from` to `to`, at `rate`.
double refGlide(double from, double to, double k, double rate) { return to + (from - to) * std::exp(-k / (kGlideTau * rate)); }

/// A stage at `from`, run on a steady input long enough to have been settled, then stepped to `to`.
struct Stepped {
	DriveStage stage;
	DriveSettings from;
	DriveSettings to;
	Stepped(DriveMode mode, float knobFrom, float knobTo, double rate, float shape = 0.3f) {
		stage.prepare(rate);
		from = deriveDrive(knobFrom, mode, shape, rate);
		to = deriveDrive(knobTo, mode, shape, rate);
		stage.set(from);
		advance(64);
		stage.set(to);
	}
	void advance(size_t frames) {
		for (size_t i = 0; i < frames; ++i) {
			float l = 0.1f + 0.05f * sineAt(done_, 220.0, 1.0);
			float r = l;
			stage.process(l, r);
			++done_;
		}
	}
	size_t done_ = 0;
};

Stereo driveAutomated(DriveMode mode, size_t block, bool inSlot) {
	Eroder e;
	e.prepare(kRate);
	Settings s = etchWith();
	s.mix = 1.0f;
	const size_t total = 48000;
	Stereo b;
	b.l.resize(total);
	b.r.resize(total);
	for (size_t i = 0; i < total; ++i) {
		b.l[i] = noiseAt(i, 5) * 1.2f;
		b.r[i] = noiseAt(i, 6) * 1.2f;
	}
	for (size_t seg = 0; seg < total; seg += 480) {
		const float amount = 0.15f + 0.8f * (0.5f + 0.5f * std::sin(static_cast<float>(seg) / 4000.0f));
		if (inSlot) {
			s.slotA = { amount, mode, 0.4f, 1.0f };
		}
		else {
			s.drive = amount;
			s.driveMode = mode;
			s.driveShape = 0.4f;
		}
		e.setTarget(s);
		const size_t end = std::min(total, seg + 480);
		for (size_t done = seg; done < end;) {
			const size_t take = std::min(block, end - done);
			e.processStereo(b.l.data() + done, b.r.data() + done, static_cast<int>(take));
			done += take;
		}
	}
	return b;
}

} // namespace

TEST_CASE("Drive amount glide: the gain into the curve and its make-up follow one pole of 20 ms toward the new Amount, at either rate") {
	for (const double rate : { 48000.0, 96000.0 }) {
		for (const DriveMode mode : { DriveMode::Tube, DriveMode::Soft, DriveMode::PaunchS, DriveMode::Chonk }) {
			CAPTURE(rate);
			CAPTURE(static_cast<int>(mode));
			Stepped t(mode, 0.2f, 0.8f, rate);
			REQUIRE(t.from.gain != t.to.gain);
			const double g0 = t.from.gain, g1 = t.to.gain, n0 = t.from.norm, n1 = t.to.norm;
			CHECK(t.stage.gainNowForTest() == doctest::Approx(g0).epsilon(1.0e-12));
			size_t at = 0;
			// Offsets in REAL time: 0.5 ms, 5 ms, 20 ms, 50 ms (frames follow the rate).
			for (const double seconds : { 0.0005, 0.005, 0.02, 0.05 }) {
				const auto k = static_cast<size_t>(seconds * rate);
				t.advance(k - at);
				at = k;
				CAPTURE(seconds);
				const double wantGain = refGlide(g0, g1, static_cast<double>(k), rate);
				const double wantNorm = refGlide(n0, n1, static_cast<double>(k), rate);
				CHECK(std::abs(t.stage.gainNowForTest() - wantGain) < 1.0e-6 * std::max(1.0, std::abs(g1 - g0)));
				CHECK(std::abs(t.stage.normNowForTest() - wantNorm) < 1.0e-6 * std::max(1.0, std::abs(n1 - n0)));
			}
			// TWENTY-FIVE TIME CONSTANTS ON (the snap is within a millionth of the target), IT IS THE TARGET'S OWN BITS.
			t.advance(static_cast<size_t>(0.5 * rate) - at + 1);
			CHECK(t.stage.gainNowForTest() == g1);
			CHECK(t.stage.normNowForTest() == n1);
		}
	}
}

TEST_CASE("Drive amount glide: Crush's and Decimate's hold, and Slew's and Lopside's rate, glide the same way and land on the target") {
	for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate }) {
		CAPTURE(static_cast<int>(mode));
		Stepped t(mode, 0.2f, 0.8f, kRate);
		const double h0 = t.from.crushHoldFrames, h1 = t.to.crushHoldFrames;
		REQUIRE(h1 > h0);
		t.advance(100);
		const double soon = t.stage.holdFramesNowForTest(0);
		CHECK(soon > h0);
		CHECK(soon < h1);
		CHECK(std::abs(soon - refGlide(h0, h1, 100.0, kRate)) < 1.0e-6 * (h1 - h0));
		CHECK(std::abs(t.stage.holdFramesNowForTest(1) - soon) < 1.0e-12); // Spread 0: both sides alike
		t.advance(static_cast<size_t>(0.5 * kRate));
		CHECK(t.stage.holdFramesNowForTest(0) == h1);
		CHECK(t.stage.holdFramesNowForTest(1) == h1);
	}
	for (const DriveMode mode : { DriveMode::Slew, DriveMode::Lopside }) {
		CAPTURE(static_cast<int>(mode));
		Stepped t(mode, 0.2f, 0.8f, kRate);
		const double z0 = t.from.slewHz, z1 = t.to.slewHz;
		t.advance(480);
		const double want = refGlide(z0, z1, 480.0, kRate);
		CHECK(std::abs(t.stage.slewHzNowForTest(0) - want) < 1.0e-4 * std::abs(z1 - z0));
		// The Slew itself runs the gliding rate: its hard limit is 2 hz / rate a frame.
		CHECK(std::abs(static_cast<double>(t.stage.slewForTest().limit()) - 2.0 * want / kRate) < 1.0e-6);
		t.advance(static_cast<size_t>(0.5 * kRate));
		CHECK(t.stage.slewHzNowForTest(0) == static_cast<double>(t.to.slewHz));
		CHECK(static_cast<double>(t.stage.slewForTest().limit()) == doctest::Approx(2.0 * z1 / kRate).epsilon(1.0e-6));
	}
}

TEST_CASE("Drive amount glide: a fresh stage, and a changed Type, start at the target and do not glide") {
	DriveStage stage;
	stage.prepare(kRate);
	stage.set(deriveDrive(0.7f, DriveMode::Tube, 0.3f, kRate));
	CHECK(stage.gainNowForTest() == static_cast<double>(deriveDrive(0.7f, DriveMode::Tube, 0.3f, kRate).gain));
	stage.set(deriveDrive(0.9f, DriveMode::Crush, 0.3f, kRate));
	CHECK(stage.holdFramesNowForTest(0) == deriveDrive(0.9f, DriveMode::Crush, 0.3f, kRate).crushHoldFrames);
	stage.set(deriveDrive(0.0f, DriveMode::Crush, 0.3f, kRate)); // rests
	stage.set(deriveDrive(0.5f, DriveMode::Crush, 0.3f, kRate));
	CHECK(stage.holdFramesNowForTest(0) == deriveDrive(0.5f, DriveMode::Crush, 0.3f, kRate).crushHoldFrames);
}

TEST_CASE("Drive amount glide: once settled the stage is a stage built at the target, to the bit") {
	for (const DriveMode mode : { DriveMode::Soft, DriveMode::Tube, DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew }) {
		CAPTURE(static_cast<int>(mode));
		Stepped moved(mode, 0.2f, 0.8f, kRate);
		DriveStage direct;
		direct.prepare(kRate);
		direct.set(deriveDrive(0.8f, mode, 0.3f, kRate));
		// Both see the same input from here, for a second; the moved one has had 0.5 s to settle first.
		moved.advance(24000);
		size_t same = 0;
		for (size_t i = 0; i < 48000; ++i) {
			const float in = 0.1f + 0.4f * sineAt(i, 330.0, 1.0);
			float a = in, b = in, c = in, d = in;
			moved.stage.process(a, b);
			direct.process(c, d);
			if (i >= 40000) {
				same += (a == c && b == d) ? 1u : 0u;
			}
		}
		// Crush, Decimate and Slew carry phase or ramp state that a long run does not forget; the others are filters with a finite memory.
		if (mode == DriveMode::Soft || mode == DriveMode::Tube) {
			CHECK(same == 8000);
		}
		CHECK(moved.stage.gainNowForTest() == direct.gainNowForTest());
		CHECK(moved.stage.normNowForTest() == direct.normNowForTest());
		CHECK(moved.stage.holdFramesNowForTest(0) == direct.holdFramesNowForTest(0));
		CHECK(moved.stage.slewHzNowForTest(0) == direct.slewHzNowForTest(0));
	}
}

TEST_CASE("Drive amount glide: a moving Amount gives the same output at any block size, in the Drive and in a slot, in every kind of Type") {
	for (const bool inSlot : { false, true }) {
		for (const DriveMode mode : { DriveMode::Soft, DriveMode::Tube, DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew, DriveMode::Lopside,
		                              DriveMode::Chonk, DriveMode::PaunchS }) {
			CAPTURE(inSlot);
			CAPTURE(static_cast<int>(mode));
			const Stereo whole = driveAutomated(mode, 480, inSlot);
			CHECK(driveAutomated(mode, 32, inSlot) == whole);
			CHECK(driveAutomated(mode, 64, inSlot) == whole);
			CHECK(driveAutomated(mode, 37, inSlot) == whole);
			CHECK(driveAutomated(mode, 1, inSlot) == whole);
			CHECK(peakOf(whole) > 0.01f);
		}
	}
}
