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
	case DriveMode::Wreck:
		return std::sin(0.5 * kPi * std::tanh(u + x.bias) * (1.0 + 4.0 * shape));
	case DriveMode::Jag:
		return refTriangle(std::tanh(u + x.bias) * (1.0 + 4.0 * shape));
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
