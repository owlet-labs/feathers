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

// THE EQ'S DESIGN LIBRARY, step 1 of the EQ.
//
// EVERY REFERENCE HERE IS AN INDEPENDENT RESTATEMENT, never a call into eq_design.h: the analog
// prototypes are re-derived with std::complex from RBJ's s-domain transfer functions, the Butterworth
// magnitude from its closed form, the tilt from the straight line it approximates, and the filter
// realisations are checked against a direct-form recursion in double. A test that asked the library
// what the answer should be could only ever agree with it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/eq/eq_design.h"
#include "fx/eq/eq_readout.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace feathers::fx::eq;

namespace {

using cd = std::complex<double>;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

/// RBJ's analog prototypes, written out again from the cookbook: A = 10^(dB/40), s normalised to the
/// corner. Deliberately NOT eq::analogPrototype.
double referenceDb(BandType type, double gainDb, double q, double fOverF0) {
	const double a = std::pow(10.0, gainDb / 40.0);
	const cd s(0.0, fOverF0);
	cd h;
	switch (type) {
	case BandType::LowShelf:
		h = a * (s * s + std::sqrt(a) / q * s + a) / (a * s * s + std::sqrt(a) / q * s + 1.0);
		break;
	case BandType::HighShelf:
		h = a * (a * s * s + std::sqrt(a) / q * s + 1.0) / (s * s + std::sqrt(a) / q * s + a);
		break;
	default: h = (s * s + (a / q) * s + 1.0) / (s * s + s / (a * q) + 1.0); break;
	}
	return 20.0 * std::log10(std::abs(h));
}

/// |H| of a designed band, in dB, by evaluating its sections' z-transforms directly.
double designedDb(const BandDesign& d, double hz, double fs) {
	const cd z1 = std::polar(1.0, -kTwoPi * hz / fs);
	cd h = 1.0;
	if (!d.bypass) {
		for (int i = 0; i < d.count; ++i) {
			const Biquad& q = d.sections[static_cast<size_t>(i)].z;
			h *= (q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1);
		}
	}
	return 20.0 * std::log10(std::abs(h));
}

/// TWO TIERS, because the matched design's error has two sources. Where the band AND the frequency being
/// read are both at or below fs/8, the design has no excuse — the analog prototype has nothing past
/// Nyquist for it to miss. Anywhere else, a wide or tall band's analog skirt reaches past Nyquist and no
/// host-rate design can follow it (a Q 0.3 bell at 16 kHz is off by dB as far down as 2 kHz).
struct GridResult {
	double worstAll = 0.0;
	double worstWellBelow = 0.0;
	int designs = 0;
	int unsolved = 0;
};

/// The grid of the doc's §2.2, widened to ±48 dB because Amount reaches ±200% (R-list, Q5).
GridResult sweep(BandType type, const std::vector<double>& qs) {
	GridResult r;
	for (double fs : {44100.0, 48000.0, 96000.0}) {
		const double fmax = std::min(20000.0, 0.45 * fs);
		for (double f0 = 20.0; f0 <= 20000.0; f0 *= 1.25) {
			for (double q : qs) {
				for (double g : {-48.0, -24.0, -12.0, -3.0, 3.0, 12.0, 24.0, 48.0}) {
					const BandDesign d = type == BandType::Bell        ? designBell(f0, fs, g, q)
					                     : type == BandType::LowShelf ? designLowShelf(f0, fs, g, q)
					                                                  : designHighShelf(f0, fs, g, q);
					++r.designs;
					if (!d.solved) {
						++r.unsolved;
					}
					for (int j = 0; j <= 400; ++j) {
						const double f = 20.0 * std::pow(fmax / 20.0, j / 400.0);
						const double e = std::fabs(designedDb(d, f, fs) - referenceDb(type, g, q, f / f0));
						r.worstAll = std::max(r.worstAll, e);
						if (f0 <= fs / 8.0 && f <= fs / 8.0) {
							r.worstWellBelow = std::max(r.worstWellBelow, e);
						}
					}
				}
			}
		}
	}
	return r;
}

/// A direct-form-I recursion in double: the reference any realisation is checked against.
std::vector<double> directImpulse(const Biquad& q, int n) {
	std::vector<double> y(static_cast<size_t>(n));
	double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
	for (int i = 0; i < n; ++i) {
		const double x = i == 0 ? 1.0 : 0.0;
		const double out = q.b0 * x + q.b1 * x1 + q.b2 * x2 - q.a1 * y1 - q.a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = out;
		y[static_cast<size_t>(i)] = out;
	}
	return y;
}

} // namespace

TEST_CASE("EQ design: matched bells and shelves track the analog prototype over the whole grid") {
	// MEASURED 2026-09-25 over this grid (±48 dB, because Amount reaches ±200%): bells 0.168 dB well
	// below Nyquist and 2.60 dB anywhere (a Q 0.3 bell at 16 kHz and -48 dB); shelves 0.478 and 1.396
	// (Q 1.5 at ±48 dB). The bounds sit just above those. A bilinear design is 4.5-7.9 dB off at 12-16 kHz
	// and fails the "anywhere" bound; a wrong numerator fails the "well below" one.
	const GridResult bell = sweep(BandType::Bell, {0.3, 0.5, 0.71, 1.0, 1.5, 3.0, 6.0, 12.0, 18.0});
	const GridResult low = sweep(BandType::LowShelf, {0.3, 0.5, 0.71, 1.0, 1.5});
	const GridResult high = sweep(BandType::HighShelf, {0.3, 0.5, 0.71, 1.0, 1.5});
	MESSAGE("bell worst: well below " << bell.worstWellBelow << " dB, anywhere " << bell.worstAll << " dB");
	MESSAGE("low shelf worst: well below " << low.worstWellBelow << " dB, anywhere " << low.worstAll << " dB");
	MESSAGE("high shelf worst: well below " << high.worstWellBelow << " dB, anywhere " << high.worstAll << " dB");
	CHECK(bell.worstWellBelow < 0.25);
	CHECK(bell.worstAll < 3.0);
	CHECK(low.worstWellBelow < 0.6);
	CHECK(low.worstAll < 1.6);
	CHECK(high.worstWellBelow < 0.6);
	CHECK(high.worstAll < 1.6);
}

TEST_CASE("EQ design: the reciprocal rule leaves no bell or shelf unsolved") {
	int designs = 0, unsolved = 0;
	for (BandType t : {BandType::Bell, BandType::LowShelf, BandType::HighShelf}) {
		const GridResult r = sweep(t, {0.3, 0.71, 1.5});
		designs += r.designs;
		unsolved += r.unsolved;
	}
	CHECK(designs > 5000);
	CHECK(unsolved == 0);

	// AND AT THE DESIGN LIMIT ITSELF, 0.45 fs, where Offset parks a band that has run out of room (R7):
	// every type, the full Q and gain ranges, every rate — solvable, finite, and stable (|a2| < 1).
	int limitDesigns = 0, limitBad = 0;
	for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
		const double f0 = designLimitHz(fs);
		for (double q : {0.1, 0.3, 0.71, 1.5, 4.0, 18.0}) {
			for (double g : {-48.0, -24.0, -3.0, 3.0, 24.0, 48.0}) {
				for (const BandDesign& d : {designBell(f0, fs, g, q), designLowShelf(f0, fs, g, q),
				                            designHighShelf(f0, fs, g, q)}) {
					++limitDesigns;
					const Biquad& z = d.sections[0].z;
					const bool finite = std::isfinite(z.b0) && std::isfinite(z.b1) && std::isfinite(z.b2)
					                    && std::isfinite(z.a1) && std::isfinite(z.a2);
					if (!d.solved || !finite || std::fabs(z.a2) >= 1.0) {
						++limitBad;
					}
				}
			}
		}
	}
	CHECK(limitDesigns == 432);
	CHECK(limitBad == 0);

	// AND THE RULE IS WHAT DOES IT: designed directly, without the inversion, a high-shelf BOOST near
	// Nyquist is unsolvable (the doc measured 48-73 dB errors once clamped). If this stops being true the
	// check above has stopped proving anything.
	const Matched naive = matchedFromAnalog(analogPrototype(BandType::HighShelf, 12.0, 0.71), 18000.0, 48000.0);
	CHECK_FALSE(naive.solved);
}

TEST_CASE("EQ design: Amount -100% is the exact inverse of +100% through the whole path") {
	// H(-g) = 1/H(+g) for every gain band and the tilt, so the two responses sum to 0 dB everywhere.
	const double fs = 48000.0;
	const Band bands[] = {
	    {BandType::LowShelf, true, 90.0, 7.0, 0.71, 3},  {BandType::Bell, true, 300.0, -9.0, 2.0, 3},
	    {BandType::Bell, true, 3100.0, 14.0, 0.8, 3},    {BandType::HighShelf, true, 12000.0, -6.0, 1.0, 3},
	    {BandType::Bell, true, 17500.0, 20.0, 4.0, 3},
	};
	auto response = [&](double amount, double hz) {
		const Globals g {0.0, amount, 2.5, 800.0};
		BandDesign designs[std::size(bands)];
		for (size_t i = 0; i < std::size(bands); ++i) {
			designs[i] = design(resolve(bands[i], g, fs), fs);
		}
		return responseDb(designs, designTilt(g, fs), hz, fs);
	};
	double worst = 0.0, loudest = 0.0;
	for (double hz = 20.0; hz < 21000.0; hz *= 1.05) {
		worst = std::max(worst, std::fabs(response(1.0, hz) + response(-1.0, hz)));
		loudest = std::max(loudest, std::fabs(response(1.0, hz)));
	}
	CHECK(worst < 1e-9);
	CHECK(loudest > 10.0); // the curve under test is not flat, or the first check proves nothing
}

TEST_CASE("EQ design: Butterworth cuts are the analog Butterworth at the warped frequency") {
	const double fs = 48000.0;
	for (int slope = 0; slope < kSlopeCount; ++slope) {
		const int order = kSlopeDbPerOctave[slope] / 6;
		for (double fc : {40.0, 250.0, 1500.0}) {
			const BandDesign low = designCut(false, fc, fs, slope, kButterworthQ);
			const BandDesign high = designCut(true, fc, fs, slope, kButterworthQ);
			CHECK(low.count == (order + 1) / 2);
			// -3.01 dB at the corner: the bilinear design is prewarped there, so this is exact.
			CHECK(std::fabs(designedDb(high, fc, fs) + 3.0103) < 1e-3);
			CHECK(std::fabs(designedDb(low, fc, fs) + 3.0103) < 1e-3);
			// THE REFERENCE IS EXACT, not approximate: a bilinear design prewarped at fc IS the analog
			// Butterworth read at the warped frequency tan(pi f / fs) / tan(pi fc / fs), so the two agree to
			// rounding at every frequency, Nyquist included.
			double worst = 0.0;
			for (double f = fc / 8.0; f <= std::min(8.0 * fc, 0.45 * fs); f *= 1.1) {
				const double warped = std::tan(kTwoPi * 0.5 * f / fs) / std::tan(kTwoPi * 0.5 * fc / fs);
				const double ratio = std::pow(warped, 2.0 * order);
				const double refHigh = -10.0 * std::log10(1.0 + 1.0 / ratio);
				const double refLow = -10.0 * std::log10(1.0 + ratio);
				// compare only where the reference is above -90 dB, where a dB error still means something
				if (refLow > -90.0) {
					worst = std::max(worst, std::fabs(designedDb(high, f, fs) - refLow));
				}
				if (refHigh > -90.0) {
					worst = std::max(worst, std::fabs(designedDb(low, f, fs) - refHigh));
				}
			}
			CHECK(worst < 1e-6);
		}
	}
}

TEST_CASE("EQ design: Q raises a cut's corner and a 6 dB cut has no Q") {
	const double fs = 48000.0;
	const BandDesign flat = designCut(false, 200.0, fs, 3, kButterworthQ);
	const BandDesign peaked = designCut(false, 200.0, fs, 3, 4.0);
	double flatMax = -200.0, peakMax = -200.0;
	for (double f = 100.0; f < 1000.0; f *= 1.01) {
		flatMax = std::max(flatMax, designedDb(flat, f, fs));
		peakMax = std::max(peakMax, designedDb(peaked, f, fs));
	}
	CHECK(flatMax < 0.01);
	CHECK(peakMax > 6.0);

	const BandDesign a = designCut(true, 5000.0, fs, 0, kButterworthQ);
	const BandDesign b = designCut(true, 5000.0, fs, 0, 8.0);
	REQUIRE(a.count == 1);
	CHECK(a.sections[0].firstOrder);
	CHECK(designedDb(a, 7000.0, fs) == designedDb(b, 7000.0, fs));
}

TEST_CASE("EQ design: a notch nulls at its frequency") {
	const BandDesign d = designNotch(1000.0, 48000.0, 2.0);
	CHECK(designedDb(d, 1000.0, 48000.0) < -120.0);
	CHECK(std::fabs(designedDb(d, 100.0, 48000.0)) < 0.1);
}

TEST_CASE("EQ design: the tilt stays within its measured line error and passes the pivot at 0 dB") {
	// MEASURED in the doc (§2.6): 0.67-0.69 dB off a straight line at ±3 dB/oct and 1.23-1.28 at +6, at
	// 44.1 and 48 kHz. Bilinear sections were 9-13 dB off at +6 near 20 kHz.
	for (double fs : {44100.0, 48000.0}) {
		for (double slope : {-6.0, -3.0, -1.5, 1.5, 3.0, 6.0}) {
			for (double pivot : {200.0, 1000.0, 5000.0}) {
				const TiltDesign t = designTilt(slope, pivot, fs);
				auto db = [&](double hz) { return 10.0 * std::log10(tiltMagnitudeSquared(t, kTwoPi * hz / fs)); };
				CHECK(std::fabs(db(pivot)) < 1e-9);
				double worst = 0.0;
				for (double f = 20.0; f <= std::min(20000.0, 0.45 * fs); f *= 1.02) {
					worst = std::max(worst, std::fabs(db(f) - slope * std::log2(f / pivot)));
				}
				// The line error is measured relative to a line through the pivot, so it grows by the
				// approximation's own error at the pivot; the doc's figures are through 1 kHz.
				CHECK(worst < (std::fabs(slope) > 4.0 ? 2.0 : 1.2));
			}
		}
	}
	CHECK(designTilt(0.0, 1000.0, 48000.0).bypass);
}

TEST_CASE("EQ design: the SVF and the one-pole realise their biquads") {
	// The doc measured 3.8e-7 worst sample difference for bells, float SVF against double TDF-II.
	const double fs = 48000.0;
	std::vector<Biquad> second;
	for (double f0 : {20.0, 200.0, 2000.0, 15000.0}) {
		for (double q : {0.5, 1.0, 8.0}) {
			for (double g : {-18.0, 6.0, 18.0}) {
				second.push_back(designBell(f0, fs, g, q).sections[0].z);
			}
			second.push_back(designHighShelf(f0, fs, 9.0, std::min(q, 1.5)).sections[0].z);
			second.push_back(designLowShelf(f0, fs, -9.0, std::min(q, 1.5)).sections[0].z);
		}
		for (int s : {1, 3, 7}) {
			const BandDesign cut = designCut(true, std::max(f0, 40.0), fs, s, kButterworthQ);
			for (int i = 0; i < cut.count; ++i) {
				if (!cut.sections[static_cast<size_t>(i)].firstOrder) {
					second.push_back(cut.sections[static_cast<size_t>(i)].z);
				}
			}
		}
	}
	double worst = 0.0;
	for (const Biquad& q : second) {
		const std::vector<double> ref = directImpulse(q, 48000);
		SvfState s;
		const SvfCoefs c = svfFromBiquad(q);
		for (size_t i = 0; i < ref.size(); ++i) {
			worst = std::max(worst, std::fabs(ref[i] - static_cast<double>(s.step(c, i == 0 ? 1.0f : 0.0f))));
		}
	}
	MESSAGE("SVF worst sample difference: " << worst);
	CHECK(worst < 2e-6);

	std::vector<Biquad> first;
	for (const Biquad& q : designTilt(4.0, 1000.0, fs).sections) {
		first.push_back(q);
	}
	first.push_back(designCut(false, 300.0, fs, 0, kButterworthQ).sections[0].z);
	first.push_back(designCut(true, 9000.0, fs, 2, kButterworthQ).sections[1].z);
	double worstOne = 0.0;
	for (const Biquad& q : first) {
		const std::vector<double> ref = directImpulse(q, 48000);
		OnePoleState s;
		const OnePoleCoefs c = onePoleFromBiquad(q);
		for (size_t i = 0; i < ref.size(); ++i) {
			worstOne = std::max(worstOne, std::fabs(ref[i] - static_cast<double>(s.step(c, i == 0 ? 1.0f : 0.0f))));
		}
	}
	MESSAGE("one-pole worst sample difference: " << worstOne);
	CHECK(worstOne < 2e-6);
}

TEST_CASE("EQ design: a band at exactly 0 dB is bypassed and one at 1e-6 dB is not") {
	for (BandType t : {BandType::Bell, BandType::LowShelf, BandType::HighShelf}) {
		const Band zero {t, true, 1000.0, 0.0, 1.0, 3};
		const Band tiny {t, true, 1000.0, 1e-6, 1.0, 3};
		CHECK(design(resolve(zero, Globals {}, 48000.0), 48000.0).bypass);
		CHECK_FALSE(design(resolve(tiny, Globals {}, 48000.0), 48000.0).bypass);
		// Amount 0 flattens every gain band to exactly 0 dB, so it bypasses too.
		CHECK(design(resolve(tiny, Globals {0.0, 0.0, 0.0, 1000.0}, 48000.0), 48000.0).bypass);
	}
	// An off band is bypassed whatever it says.
	CHECK(design(resolve({BandType::LowCut, false, 100.0, 0.0, 0.71, 3}, Globals {}, 48000.0), 48000.0).bypass);
}

TEST_CASE("EQ design: Offset follows freely below Nyquist and a bell fades out as it crosses Nyquist") {
	// R7: the fade means the band going over Nyquist. At 48 kHz the design limit is
	// 0.45 fs = 21.6 kHz and Nyquist is 24 kHz; the fade runs in octaves between them (assumption A1).
	const double fs = 48000.0;
	const double limit = 21600.0, nyquist = 24000.0;
	const Globals up2 {2.0, 1.0, 0.0, 1000.0};

	// Two octaves up: 1 kHz -> 4 kHz, gain untouched.
	const EffectiveBand moved = resolve({BandType::Bell, true, 1000.0, 6.0, 1.0, 3}, up2, fs);
	CHECK(moved.freqHz == doctest::Approx(4000.0));
	CHECK(moved.gainDb == doctest::Approx(6.0));

	// NO 20 kHz CEILING: 5.2 kHz x 4 = 20.8 kHz is below the limit and is designed there, full gain.
	const EffectiveBand high = resolve({BandType::Bell, true, 5200.0, 6.0, 1.0, 3}, up2, fs);
	CHECK(high.freqHz == doctest::Approx(20800.0));
	CHECK(high.gainDb == doctest::Approx(6.0));
	// And at 96 kHz a band follows Offset to 40 kHz.
	CHECK(resolve({BandType::Bell, true, 10000.0, 6.0, 1.0, 3}, up2, 96000.0).freqHz == doctest::Approx(40000.0));

	// Halfway (in octaves) from the limit to Nyquist: designed AT the limit, with half its gain.
	const double halfway = limit * std::sqrt(nyquist / limit);
	const EffectiveBand fading = resolve({BandType::Bell, true, halfway / 4.0, 12.0, 1.0, 3}, up2, fs);
	CHECK(fading.freqHz == doctest::Approx(limit));
	CHECK(fading.gainDb == doctest::Approx(6.0));
	// At and past Nyquist, nothing: the band is bypassed.
	const EffectiveBand atNyquist = resolve({BandType::Bell, true, nyquist / 4.0, 12.0, 1.0, 3}, up2, fs);
	CHECK(std::fabs(atNyquist.gainDb) < 1e-12);
	const EffectiveBand gone = resolve({BandType::Bell, true, 10000.0, 12.0, 1.0, 3}, up2, fs);
	CHECK(gone.gainDb == 0.0);
	CHECK(design(gone, fs).bypass);

	// A high shelf fades the same way; a low shelf past Nyquist has become a broadband gain and keeps it
	// (assumption A2).
	CHECK(resolve({BandType::HighShelf, true, 10000.0, 12.0, 0.71, 3}, up2, fs).gainDb == 0.0);
	const EffectiveBand broad = resolve({BandType::LowShelf, true, 10000.0, 12.0, 0.71, 3}, up2, fs);
	CHECK(broad.gainDb == doctest::Approx(12.0));
	CHECK(broad.freqHz == doctest::Approx(limit));

	// THE FLOOR IS A PLAIN CLAMP FOR EVERY TYPE: no fade at the bottom (R7).
	const Globals down3 {-3.0, 1.0, 0.0, 1000.0};
	for (BandType t : {BandType::Bell, BandType::LowShelf, BandType::HighShelf}) {
		const EffectiveBand low = resolve({t, true, 40.0, 12.0, 0.71, 3}, down3, fs);
		CHECK(low.freqHz == doctest::Approx(kFloorHz));
		CHECK(low.gainDb == doctest::Approx(12.0));
	}

	// Cuts clamp and keep working: a Low Cut pushed past Nyquist is still a Low Cut at the limit.
	const EffectiveBand cut = resolve({BandType::LowCut, true, 10000.0, 0.0, 0.71, 3}, up2, fs);
	CHECK(cut.freqHz == doctest::Approx(limit));
	CHECK(designedDb(design(cut, fs), 1000.0, fs) < -100.0);
	// A notch clamps too.
	CHECK(resolve({BandType::Notch, true, 10000.0, 0.0, 2.0, 3}, up2, fs).freqHz == doctest::Approx(limit));
}

TEST_CASE("EQ readout: the house note naming with cents and Hz") {
	// The reference's naming, MIDI 60 = C3 (value_box.cpp's Unit::Note): 220 Hz is MIDI 57, A2.
	CHECK(frequencyReadout(220.0) == "A2 +0c / 220.0 Hz");
	CHECK(frequencyReadout(440.0) == "A3 +0c / 440.0 Hz");
	// Middle C, 261.6256 Hz, is MIDI 60: C3 in this naming.
	CHECK(frequencyReadout(261.6256) == "C3 +0c / 261.6 Hz");
	// 1000 Hz: 69 + 12 log2(1000/440) = 83.2131, so B4 (83 -> octave 83/12 - 2 = 4) and +21 cents.
	CHECK(frequencyReadout(1000.0) == "B4 +21c / 1.00 kHz");
	// 50 Hz: 69 + 12 log2(50/440) = 31.3483 -> G0 (31 = G, octave 2 - 2 = 0), +35 cents.
	CHECK(frequencyReadout(50.0) == "G0 +35c / 50.0 Hz");
	// 212 Hz: 56.3587 -> 56 = G#, octave 56/12 - 2 = 2, +36 cents.
	CHECK(frequencyReadout(212.0) == "G#2 +36c / 212.0 Hz");
	// Below the note, the sign says so: 435 Hz = 68.8021 -> A3 -20c.
	CHECK(frequencyReadout(435.0) == "A3 -20c / 435.0 Hz");
	// 12.4 kHz: 126.8003 -> 127 = G, octave 127/12 - 2 = 8, -20 cents.
	CHECK(frequencyReadout(12400.0) == "G8 -20c / 12.4 kHz");
}
