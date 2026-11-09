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

#include "dsp/halfband.h"
#include "fx/busutil/busutil_types.h"
#include "fx/dyn/clip_curve.h"
#include "fx/dyn/meter_ramp.h"
#include "fx/dyn/oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

namespace dyn = feathers::fx::dyn;

TEST_CASE("softClip is the input below the knee and never exceeds one") {
	for (float knee : { 0.0f, 0.03f, 0.25f, 1.0f }) {
		const float t = 1.0f - knee;
		for (float x = -4.0f; x <= 4.0f; x += 0.001f) {
			const float y = dyn::softClip(x, knee);
			CHECK(std::fabs(y) <= 1.0f);
			if (std::fabs(x) <= t) {
				CHECK(y == x); // exact, not approx
			}
		}
		// THE KNEE ITSELF, EXPLICITLY — the grid above steps in float and need not land on t.
		CHECK(dyn::softClip(t, knee) == t);
		CHECK(dyn::softClip(-t, knee) == -t);
	}
}

TEST_CASE("Bus Utility clips through the shared curve") {
	for (float x = -3.0f; x <= 3.0f; x += 0.01f) {
		for (float knee : { 0.0f, 0.12f, 1.0f }) {
			CHECK(feathers::fx::busutil::clipSample(x, knee) == dyn::softClip(x, knee));
		}
	}
}

TEST_CASE("HeldPeak holds a one-sample peak for at least one whole window") {
	dyn::HeldPeak p;
	p.prepare(48000.0); // 50 ms window = 2400 samples
	p.push(0.9f);
	for (int i = 0; i < 2399 + 2400; ++i) {
		CHECK(p.reading() >= 0.9f);
		p.push(0.0f);
	}
	for (int i = 0; i < 2400; ++i) {
		p.push(0.0f);
	}
	CHECK(p.reading() == 0.0f);
}

TEST_CASE("LinearRamp lands exactly on its target in the declared time") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005); // 240 samples
	r.snap(1.0f);
	r.set(0.0f);
	for (int i = 0; i < 239; ++i) {
		CHECK(r.next() > 0.0f);
	}
	CHECK(r.next() == 0.0f);
	CHECK(r.settled());
}

TEST_CASE("LinearRamp's first set is a snap, not a ramp from zero") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005);
	r.set(0.7f);
	CHECK(r.next() == 0.7f);
}

TEST_CASE("LinearRamp retargeted mid-ramp continues from where it is") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005); // 240 samples
	r.snap(0.0f);
	r.set(1.0f);
	float v = 0.0f;
	for (int i = 0; i < 120; ++i) {
		v = r.next();
	}
	REQUIRE(std::fabs(v - 0.5f) < 1e-4f);
	r.set(0.0f);
	// THE NEXT VALUE is one new step from where it was — not a jump to the old target, not a restart.
	const float expected = v + (0.0f - v) / 240.0f;
	CHECK(r.next() == expected);
	for (int i = 0; i < 238; ++i) {
		r.next();
	}
	CHECK(r.next() == 0.0f);
	CHECK(r.settled());
}

TEST_CASE("LinearRamp after reset snaps on the next set") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005);
	r.snap(1.0f);
	r.reset();
	r.set(0.3f);
	CHECK(r.next() == 0.3f);
	CHECK(r.settled());
}

TEST_CASE("LinearRamp ignores a non-finite target") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005);
	r.snap(0.4f);
	r.set(std::nanf(""));
	r.set(std::numeric_limits<float>::infinity()); // not 1.0f / 0.0f: MSVC refuses a constant division by zero (C2124)
	CHECK(r.next() == 0.4f);
	CHECK(r.settled());
}

TEST_CASE("LinearRamp prepare lands a ramp in flight") {
	dyn::LinearRamp r;
	r.prepare(48000.0, 0.005);
	r.snap(0.0f);
	r.set(1.0f);
	r.next();
	r.prepare(96000.0, 0.005);
	CHECK(r.settled());
	CHECK(r.next() == 1.0f);
}

namespace {

constexpr double kRate = 48000.0;

std::vector<float> sine(int n, double hz, double amp, double phase = 0.0) {
	std::vector<float> v(static_cast<std::size_t>(n));
	for (int i = 0; i < n; ++i) {
		v[static_cast<std::size_t>(i)] =
		    static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate + phase));
	}
	return v;
}

double rms(const std::vector<float>& v, int from) {
	double sum = 0.0;
	for (std::size_t i = static_cast<std::size_t>(from); i < v.size(); ++i) {
		sum += static_cast<double>(v[i]) * v[i];
	}
	return std::sqrt(sum / static_cast<double>(v.size() - static_cast<std::size_t>(from)));
}

/// THE POWER AT ONE FREQUENCY over [from, end) — Goertzel. The window is chosen by the caller so every
/// frequency asked about lands on a whole number of cycles, so a rectangular window leaks nothing.
double goertzel(const std::vector<float>& v, int from, double hz, double rate = kRate) {
	const double w = 2.0 * std::numbers::pi * hz / rate;
	const double c = 2.0 * std::cos(w);
	double s1 = 0.0, s2 = 0.0;
	for (std::size_t i = static_cast<std::size_t>(from); i < v.size(); ++i) {
		const double s0 = v[i] + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - c * s1 * s2;
}

void hardClip(float& l, float& r) {
	l = std::clamp(l, -0.5f, 0.5f);
	r = std::clamp(r, -0.5f, 0.5f);
}

} // namespace

// A -20 dBFS 1 kHz sine through the identity at 4x: unity gain within 0.001 dB after settling (MEASURED 6e-8). 1 kHz
// is 48 samples a cycle, so the 4800-sample window is exactly 100 cycles and the filters' group delay
// cannot bias the RMS.
TEST_CASE("Oversampler4x is unity in the passband") {
	constexpr int kN = 9600;
	auto left = sine(kN, 1000.0, 0.1);
	auto right = sine(kN, 1000.0, 0.1, 1.0);
	const auto inL = left, inR = right;
	dyn::Oversampler4x os;
	os.reset();
	os.process(left.data(), right.data(), kN, [](float&, float&) {});
	const double gainL = 20.0 * std::log10(rms(left, kN / 2) / rms(inL, kN / 2));
	const double gainR = 20.0 * std::log10(rms(right, kN / 2) / rms(inR, kN / 2));
	MESSAGE("passband gain L " << gainL << " dB, R " << gainR << " dB");
	CHECK(std::fabs(gainL) < 0.001);
	CHECK(std::fabs(gainR) < 0.001);
}

// Same input, processed as one 9600 block vs blocks of 1, 7, 13, 64 in turn: bit-identical output. The
// shape is a hard clip so the state carried between blocks includes what the nonlinearity made.
TEST_CASE("Oversampler4x does not care how the host cuts the block") {
	constexpr int kN = 9600;
	const auto srcL = sine(kN, 7000.0, 1.0);
	const auto srcR = sine(kN, 3100.0, 0.8, 0.3);

	auto oneL = srcL, oneR = srcR;
	dyn::Oversampler4x whole;
	whole.reset();
	whole.process(oneL.data(), oneR.data(), kN, hardClip);

	auto cutL = srcL, cutR = srcR;
	dyn::Oversampler4x cut;
	cut.reset();
	constexpr int kSizes[] = { 1, 7, 13, 64 };
	int at = 0;
	for (int k = 0; at < kN; ++k) {
		const int n = std::min(kSizes[k % 4], kN - at);
		cut.process(cutL.data() + at, cutR.data() + at, n, hardClip);
		at += n;
	}

	int mismatches = 0;
	for (int i = 0; i < kN; ++i) {
		if (oneL[static_cast<std::size_t>(i)] != cutL[static_cast<std::size_t>(i)] ||
		    oneR[static_cast<std::size_t>(i)] != cutR[static_cast<std::size_t>(i)]) {
			++mismatches;
		}
	}
	CHECK(mismatches == 0);
	// NOT VACUOUS: the clip did something, so the comparison was of shaped output rather than silence.
	CHECK(rms(oneL, kN / 2) > 0.1);
}

// Hard clip (clamp to ±0.5) of a 0 dBFS 7 kHz sine at 48 kHz. The odd harmonics above Nyquist fold
// back onto frequencies that are NOT harmonics of 7 kHz; at 1x they land there at full strength, and
// the oversampler should have removed them before the fold. The window (last 4800 samples) holds a
// whole number of cycles of every integer-kHz frequency, so Goertzel reads each bin without leakage.
TEST_CASE("Oversampler4x lowers the aliases of a hard clip") {
	constexpr int kN = 9600;
	constexpr double kF = 7000.0;

	// THE FOLD LIST, computed: harmonic h*f folded into [0, fs/2], kept when below 20 kHz, above DC and
	// not itself a multiple of f. The first five.
	std::vector<double> folds;
	for (int h = 3; folds.size() < 5 && h < 200; h += 2) {
		double f = std::fmod(h * kF, kRate);
		if (f > kRate / 2.0) {
			f = kRate - f;
		}
		const bool harmonic = std::fmod(f, kF) == 0.0;
		const bool fresh = std::find(folds.begin(), folds.end(), f) == folds.end();
		if (f > 0.0 && f < 20000.0 && !harmonic && fresh) {
			folds.push_back(f);
		}
	}
	std::string list;
	for (double f : folds) {
		list += std::to_string(static_cast<int>(f)) + " ";
	}
	INFO("folded images (Hz): " << list);
	REQUIRE(folds.size() == 5);

	auto naiveL = sine(kN, kF, 1.0), naiveR = naiveL;
	for (int i = 0; i < kN; ++i) {
		hardClip(naiveL[static_cast<std::size_t>(i)], naiveR[static_cast<std::size_t>(i)]);
	}

	auto osL = sine(kN, kF, 1.0), osR = osL;
	dyn::Oversampler4x os;
	os.reset();
	os.process(osL.data(), osR.data(), kN, hardClip);

	double naive = 0.0, over = 0.0;
	for (double f : folds) {
		naive += goertzel(naiveL, kN / 2, f);
		over += goertzel(osL, kN / 2, f);
	}
	const double reductionDb = 10.0 * std::log10(naive / over);
	// NOT VACUOUS: the fundamental survives the oversampled clip at a sensible level, so a quiet alias
	// band is not merely a quiet output.
	const double fundamental = goertzel(osL, kN / 2, kF);
	MESSAGE("alias reduction " << reductionDb << " dB over " << list);
	// MEASURED 35.8 dB on the clean-room halfbands (2026-09-25; the vendored filters before them read
	// 39.0); the bar is that less a margin. **WHAT THIS CAN SEE: that the clip ran oversampled and the
	// path decimates at all** (a swapped or a single interpolator branch read 3.0 and 2.1 dB: red).
	// **WHAT IT CANNOT: filter strength.** The wide set in the steep DOWN stage read 35.77, unchanged; in
	// the steep UP stage it read 38.7, HIGHER — the residue here is the clip's own harmonics folding
	// against 192 kHz, in band before any filter runs (`src/dsp/halfband.h` makes the same point), and it
	// moves with the up path's phase, not its rejection. Strength is the bit pin's and the rejection
	// bounds' job.
	CHECK(reductionDb >= 33.0);
	CHECK(fundamental > 0.1 * goertzel(naiveL, kN / 2, kF));
}

// THE INDEPENDENT REFERENCE for the down path: the voice's own decimator, `Halfband4x::decimate` (feathers-
// dsp, compiled there). The shape OVERWRITES every oversampled frame with noise, so what reaches the down
// stages is known exactly and the up stages drop out; the host rate output must then be the reference's
// to the bit. Host blocks cut at 1, 7, 13 and 64 frames against the reference's 32, so this also holds the
// per-sample down path to the block decimator across any partition. With this pin, the unity, partition
// and alias cases above are testing the voice's filter rather than something that merely resembles it.
TEST_CASE("Oversampler4x decimates exactly as Halfband4x") {
	constexpr int kN = 1600; // a multiple of Halfband4x::kMaxBlock
	std::uint32_t seed = 12345u;
	auto noise = [&seed] {
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
	};
	std::vector<float> fourL(4 * kN), fourR(4 * kN);
	for (int k = 0; k < 4 * kN; ++k) {
		fourL[static_cast<std::size_t>(k)] = noise();
		fourR[static_cast<std::size_t>(k)] = noise();
	}

	std::vector<float> left(kN, 0.25f), right(kN, -0.25f);
	dyn::Oversampler4x os;
	os.reset();
	std::size_t at4 = 0;
	auto replace = [&](float& l, float& r) {
		l = fourL[at4];
		r = fourR[at4];
		++at4;
	};
	constexpr int kSizes[] = { 1, 7, 13, 64 };
	for (int k = 0, at = 0; at < kN; ++k) {
		const int n = std::min(kSizes[k % 4], kN - at);
		os.process(left.data() + at, right.data() + at, n, replace);
		at += n;
	}
	REQUIRE(at4 == fourL.size());

	std::vector<float> refL(kN), refR(kN);
	feathers::Halfband4x hbL, hbR;
	hbL.reset();
	hbR.reset();
	constexpr int kBlock = feathers::Halfband4x::kMaxBlock;
	for (int at = 0; at < kN; at += kBlock) {
		hbL.decimate(fourL.data() + 4 * at, refL.data() + at, kBlock);
		hbR.decimate(fourR.data() + 4 * at, refR.data() + at, kBlock);
	}

	int mismatches = 0;
	double energy = 0.0;
	for (std::size_t i = 0; i < static_cast<std::size_t>(kN); ++i) {
		mismatches += (left[i] != refL[i]) + (right[i] != refR[i]);
		energy += static_cast<double>(refL[i]) * refL[i];
	}
	CHECK(mismatches == 0);
	// NOT VACUOUS: the reference said something — noise in, noise-scale out.
	CHECK(energy > 10.0);
}

namespace {

/// ONE STAGE OF THE INTERPOLATOR ALONE, a sine at 48 kHz in, the 96 kHz output captured.
template <int N>
std::vector<float> interpolate(const double (&coefs)[N], double hz, int n) {
	feathers::HalfbandInterpolator2x<N> up(coefs);
	std::vector<float> out;
	out.reserve(2 * static_cast<std::size_t>(n));
	for (float x : sine(n, hz, 1.0)) {
		float a = 0.0f, b = 0.0f;
		up.process(x, a, b);
		out.push_back(a);
		out.push_back(b);
	}
	return out;
}

} // namespace

// THE INTERPOLATOR'S GAIN AND ORDER, on the stage the oversampler runs at the host side (the steep set):
// a 0 dBFS 1 kHz sine at 48 kHz comes out at 96 kHz at full scale. A halved gain reads -6.02 dB here; a
// swapped output order is a phase error between the branches that takes the tone down with it.
TEST_CASE("HalfbandInterpolator2x is unity in the passband") {
	constexpr int kN = 9600;
	constexpr double kHigh = 2.0 * kRate;
	const auto out = interpolate(feathers::kHalfbandStage2, 1000.0, kN);
	const int from = kN; // the last 9600 of 19200 samples at 96 kHz: 100 whole cycles of 1 kHz
	const double expected = std::pow(0.5 * (2 * kN - from), 2.0);
	const double gainDb = 10.0 * std::log10(goertzel(out, from, 1000.0, kHigh) / expected);
	MESSAGE("interpolator gain at 1 kHz " << gainDb << " dB");
	// MEASURED 2026-09-25: -1.1e-7 dB. One branch alone reads -0.0047 dB, so the bar is tight.
	CHECK(std::fabs(gainDb) < 0.001);

	// **AND SAMPLE BY SAMPLE, so a dropped branch is not riding a 0.001 dB margin.** The 96 kHz output must
	// BE a 1 kHz sine at one fixed delay — every sample, the `later` ones included. Fitted by least squares
	// over whole cycles (the in-phase and quadrature sums), then the worst sample's distance from the fit.
	// One branch serving both outputs is a sample-and-hold: each `later` sample repeats `earlier`, which sits
	// ~2*pi*1k/96k/2 = 0.033 of full scale off a sine. The image, 120 dB down, is 1e-6.
	double sinSum = 0.0, cosSum = 0.0;
	const int count = 2 * kN - from;
	for (int m = from; m < 2 * kN; ++m) {
		const double ph = 2.0 * std::numbers::pi * 1000.0 * m / kHigh;
		sinSum += out[static_cast<std::size_t>(m)] * std::sin(ph);
		cosSum += out[static_cast<std::size_t>(m)] * std::cos(ph);
	}
	const double a = 2.0 * sinSum / count, b = 2.0 * cosSum / count;
	double residual = 0.0;
	for (int m = from; m < 2 * kN; ++m) {
		const double ph = 2.0 * std::numbers::pi * 1000.0 * m / kHigh;
		residual = std::max(residual, std::fabs(out[static_cast<std::size_t>(m)] - (a * std::sin(ph) + b * std::cos(ph))));
	}
	MESSAGE("worst sample off the fitted 1 kHz sine: " << residual);
	// MEASURED 2026-09-25: 4.8e-7. The bar sits two hundred times above that and 330 times under 0.033.
	CHECK(residual < 1e-4);
}

// WHAT AN INTERPOLATOR IS FOR: zero-stuffing 48 kHz to 96 kHz puts an image of f at 48k - f, and the
// filter must remove it. 7 kHz (image at 41 kHz, deep in the stopband) and 23 kHz (image at 25 kHz, just
// past the transition, where a weak or mis-ordered filter shows first).
TEST_CASE("HalfbandInterpolator2x rejects its image") {
	constexpr int kN = 9600;
	constexpr double kHigh = 2.0 * kRate;
	for (double f : { 7000.0, 23000.0 }) {
		CAPTURE(f);
		const auto out = interpolate(feathers::kHalfbandStage2, f, kN);
		const int from = kN;
		const double tone = goertzel(out, from, f, kHigh);
		const double image = goertzel(out, from, kRate - f, kHigh);
		const double rejectionDb = 10.0 * std::log10(tone / image);
		MESSAGE("tone " << f << " Hz: image at " << kRate - f << " Hz " << rejectionDb << " dB below it");
		// NOT VACUOUS: the tone itself is there at full scale.
		CHECK(std::fabs(10.0 * std::log10(tone / std::pow(0.5 * (2 * kN - from), 2.0))) < 0.2);
		// MEASURED 2026-09-25: 124.3 dB at 41 kHz, 119.4 dB at 25 kHz. The bar is the worst less ~9 dB.
		CHECK(rejectionDb >= 110.0);
	}
}

// THE CHANNELS STAY WHERE THEY WERE PUT. Every stage is one mono filter per channel, eight of them, so a
// crossed name at any one stage would move the signal across — and every other case here drives both
// channels alike, where a swap is invisible. One side sine, the other silent, both ways.
TEST_CASE("Oversampler4x keeps left and right apart") {
	constexpr int kN = 9600;
	for (bool leftLive : { true, false }) {
		CAPTURE(leftLive);
		const auto tone = sine(kN, 1000.0, 0.5);
		auto left = leftLive ? tone : std::vector<float>(kN, 0.0f);
		auto right = leftLive ? std::vector<float>(kN, 0.0f) : tone;
		dyn::Oversampler4x os;
		os.reset();
		os.process(left.data(), right.data(), kN, [](float&, float&) {});
		const auto& live = leftLive ? left : right;
		const auto& quiet = leftLive ? right : left;
		float peak = 0.0f;
		for (float v : quiet) {
			peak = std::max(peak, std::fabs(v));
		}
		CHECK(peak < 1e-6f);
		const double gain = 20.0 * std::log10(rms(live, kN / 2) / rms(tone, kN / 2));
		CHECK(std::fabs(gain) < 0.001);
	}
}

// THE UP STAGES' STRENGTH, seen where only the shape can see it: the oversampled frames themselves. A 0
// dBFS sine at f through the identity, captured at 192 kHz, must carry the fundamental and nothing at
// the images the two up stages exist to reject: 48k - f (stage one's image; at 96k, 48k + f is the
// same bin), 96k - f (stage two's; 96k + f is its mirror) and 48k + f (96k minus stage one's image:
// stage two's image of stage one's residue, so rejected twice). The alias case above cannot see this
// (its residue is the clip's own harmonics folding at 192k), so without this case the up filters could
// weaken freely.
// The window (last 19200 of 38400 oversampled samples) is a whole number of cycles of every integer-kHz
// frequency at 192k, so each bin reads without leakage.
TEST_CASE("Oversampler4x's up stages reject their images") {
	constexpr int kN = 9600;
	constexpr double kHigh = 4.0 * kRate;
	// TWO TONES, because the two ways a halfband weakens show in different places. A wide coefficient set
	// in the steep stage keeps 7 kHz's images deep (113 dB at 41 kHz) and loses them only at the band
	// edge: 23 kHz's first image, 25 kHz, read 13 dB (MEASURED 2026-09-25, the known-bad that puts
	// `kHalfbandStage1` in the steep up stage). A 7 kHz tone alone would have passed it.
	for (double f : { 7000.0, 23000.0 }) {
		CAPTURE(f);
		auto left = sine(kN, f, 1.0), right = left;
		std::vector<float> frames;
		frames.reserve(4 * kN);
		dyn::Oversampler4x os;
		os.reset();
		os.process(left.data(), right.data(), kN, [&frames](float& l, float&) { frames.push_back(l); });
		REQUIRE(frames.size() == static_cast<std::size_t>(4 * kN));
		const int from = 2 * kN;
		const double fundamental = goertzel(frames, from, f, kHigh);
		// NOT VACUOUS: the fundamental is there at full scale (a whole-cycle window: power = (A N / 2)^2).
		const double expected = std::pow(0.5 * (4 * kN - from), 2.0);
		CHECK(std::fabs(10.0 * std::log10(fundamental / expected)) < 0.1);
		for (double image : { 48000.0 - f, 48000.0 + f, 96000.0 - f }) {
			const double rejectionDb = 10.0 * std::log10(fundamental / goertzel(frames, from, image, kHigh));
			CAPTURE(image);
			MESSAGE("tone " << f << " image " << image << " Hz: " << rejectionDb << " dB below the fundamental");
			// MEASURED 2026-09-25 on the clean-room halfbands — 7 kHz: 41k 124.2 dB, 55k 160.8, 89k 113.8;
			// 23 kHz: 25k 119.5, 71k 175.6, 73k 112.7. The bar is the worst less ~8 dB.
			CHECK(rejectionDb >= 105.0);
		}
	}
}
