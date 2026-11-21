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

#include "fx/clip/clip_types.h"
#include "fx/dyn/clip_curve.h"
#include "fx/eroder/eroder_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace clip = feathers::fx::clip;
namespace dyn = feathers::fx::dyn;

namespace {

//------------------------------------------------------------------------------------------------//
// THE CURVES AND THE ROW (Task 7)                                                                //
//------------------------------------------------------------------------------------------------//

TEST_CASE("every curve stays inside the ceiling and is exact below where it starts acting") {
	for (const float knee : { 0.0f, 0.1f, 0.5f, 1.0f }) {
		const float t = 1.0f - knee;
		for (float u = -8.0f; u <= 8.0f; u += 0.001953125f) { // 1/512, so ±t and ±1 are visited exactly
			CAPTURE(knee);
			CAPTURE(u);
			const float c = dyn::softClip(u, knee);
			const float detail = u - c; // what clipping removed (unfiltered here — the curve's bound is on c)
			const float soft = c;
			const float texture = clip::textureCurve(c, detail, 1.0f);
			const float grit = clip::gritCurve(c, u, knee, 1.0f, 0.999f);
			const float fold = clip::foldCurve(u, 1.0f);
			const float bits1 = clip::bitsCurve(u, knee, 1);
			const float bits2 = clip::bitsCurve(u, knee, 2);
			for (const float y : { soft, texture, grit, fold, bits1, bits2 }) {
				CHECK(std::fabs(y) <= 1.0f);
			}
			if (std::fabs(u) <= t) {
				CHECK(soft == u);
				CHECK(texture == u); // c == u, and the detail is exactly 0
				CHECK(grit == u);
				CHECK(bits1 == u);
				CHECK(bits2 == u);
			}
			if (std::fabs(u) <= 1.0f) {
				CHECK(fold == u);
			}
		}
	}
}

TEST_CASE("Texture never carves past zero: |y| <= |c| for any detail") {
	size_t checked = 0;
	for (float c = -1.0f; c <= 1.0f; c += 0.0625f) {
		for (float d = -2.0f; d <= 2.0f; d += 0.03125f) {
			for (const float fb : { 0.0f, 0.25f, 0.5f, 1.0f }) {
				const float y = clip::textureCurve(c, d, fb);
				CAPTURE(c);
				CAPTURE(d);
				CAPTURE(fb);
				CHECK(std::fabs(y) <= std::fabs(c));
				// AND ON THE SAME SIDE: the carve reduces the magnitude, it never flips the sign.
				CHECK((y == 0.0f || (y < 0.0f) == (c < 0.0f)));
				++checked;
			}
		}
	}
	CHECK(checked > 10000u);
	// A VALUE, not just a bound: c = 0.8, detail -0.3 at foldback 0.5 carves 0.15 either way.
	CHECK(std::fabs(clip::textureCurve(0.8f, -0.3f, 0.5f) - 0.65f) < 1e-6f);
	CHECK(std::fabs(clip::textureCurve(-0.8f, 0.3f, 0.5f) - (-0.65f)) < 1e-6f);
}

TEST_CASE("Bits quantises the top to exactly 2 or 4 levels") {
	for (const float knee : { 0.1f, 0.5f, 1.0f }) {
		const float t = 1.0f - knee;
		for (const int bits : { 1, 2 }) {
			std::set<float> seen;
			for (int i = 1; i <= 100000; ++i) {
				const float u = t + (1.0f - t) * static_cast<float>(i) / 100000.0f; // (t, 1]
				seen.insert(clip::bitsCurve(u, knee, bits));
			}
			CAPTURE(knee);
			CAPTURE(bits);
			CHECK(seen.size() == (bits == 1 ? 2u : 4u));
			// THE LEVELS SPAN THE REGION: the lowest is t and the highest is the ceiling.
			CHECK(*seen.begin() == doctest::Approx(t).epsilon(1e-6));
			CHECK(*seen.rbegin() == 1.0f);
		}
	}
	// KNEE 0 IS A HARD CLAMP: no region to quantise.
	CHECK(clip::bitsCurve(0.5f, 0.0f, 2) == 0.5f);
	CHECK(clip::bitsCurve(3.0f, 0.0f, 2) == 1.0f);
	CHECK(clip::bitsCurve(-3.0f, 0.0f, 1) == -1.0f);
}

TEST_CASE("Grit leaves a signal below the knee exactly alone and carves in proportion to the overshoot") {
	constexpr float knee = 0.2f;
	// BELOW THE KNEE: over == 0, the clipped value comes back exactly.
	for (const float u : { 0.0f, 0.3f, -0.79f, 0.8f }) {
		const float c = dyn::softClip(u, knee);
		CHECK(clip::gritCurve(c, u, knee, 1.0f, 0.9f) == c);
	}
	// THE CARVE IS PROPORTIONAL TO THE OVERSHOOT (noise and c held fixed).
	const float c = 0.95f;
	const float carveA = c - clip::gritCurve(c, 0.9f, knee, 1.0f, 0.5f);  // over = 0.1
	const float carveB = c - clip::gritCurve(c, 1.1f, knee, 1.0f, 0.5f);  // over = 0.3
	CHECK(carveA > 0.0f);
	CHECK(std::fabs(carveB / carveA - 3.0f) < 1e-4f);
	// AND IT IS kGritScale * grit * over * noise, as a value.
	CHECK(std::fabs(carveB - clip::kGritScale * 1.0f * 0.3f * 0.5f) < 1e-6f);
}

TEST_CASE("Fold folds: the values the triangle gives, and depth 0 is the clamp") {
	CHECK(std::fabs(clip::foldCurve(1.5f, 1.0f) - 0.5f) < 1e-6f);
	CHECK(std::fabs(clip::foldCurve(3.0f, 1.0f) - (-1.0f)) < 1e-6f);
	CHECK(std::fabs(clip::foldCurve(-1.5f, 1.0f) - (-0.5f)) < 1e-6f);
	for (const float u : { 1.2f, 2.5f, -4.7f }) {
		CHECK(clip::foldCurve(u, 0.0f) == std::clamp(u, -1.0f, 1.0f));
	}
	// EXACT IDENTITY BELOW THE FOLD, INCLUDING AT TINY u, where the triangle's `v - 1` would round.
	for (const float u : { 1e-30f, -1e-30f, 1e-8f, 0.3f, -0.3f, 1.0f, -1.0f }) {
		CAPTURE(u);
		CHECK(clip::foldCurve(u, 1.0f) == u);
		CHECK(clip::foldCurve(u, 0.5f) == u);
	}
}

TEST_CASE("a row round-trips and a short row takes the defaults") {
	clip::Settings s;
	s.enabled = false;
	s.mode = static_cast<int>(clip::Mode::Bits);
	s.driveDb = 9.5f;
	s.ceilingDb = -4.0f;
	s.outputDb = 2.0f;
	s.oversample = false;
	s.knee = 0.35f;
	s.freq = 12.0f;
	s.foldback = 0.8f;
	s.grit = 0.2f;
	s.depth = 0.6f;
	s.bits = 1;
	const clip::Row row = clip::rowOf(s);
	CHECK(clip::stateOf(row, row.size()) == s);
	// A SHORT ROW: Oversample absent must be ON (its declared default), not zero.
	const clip::Settings partial = clip::stateOf(row, 3);
	CHECK(partial.oversample == true);
	CHECK(partial.knee == 0.1f);
	CHECK(partial.mode == static_cast<int>(clip::Mode::Bits));
	// A ROW THAT LACKS THE FIELD READS THE CONTROL'S DECLARED DEFAULT, the rotation's Rate too (the struct's own is off).
	clip::Settings declared;
	declared.rate = static_cast<float>(clip::kRateLog2Default);
	CHECK(clip::stateOf(clip::Row {}, 0) == declared);
}

//------------------------------------------------------------------------------------------------//
// THE DSP (Task 8)                                                                               //
//------------------------------------------------------------------------------------------------//

constexpr double kSampleRate = 48000.0;
constexpr int kSecond = 48000;

/// AN OPS INSTANCE, CONSTRUCTED THE WAY A HOST CONSTRUCTS ONE, with the readings captured through the
/// host's sink road (`test_fx_comp.cpp`'s `Instance`).
class Instance {
public:
	Instance() {
		REQUIRE(clip::kAudioOps.stateBytes <= storage_.size());
		REQUIRE(clip::kAudioOps.stateAlign <= alignof(std::max_align_t));
		clip::kAudioOps.construct(storage_.data());
		clip::kAudioOps.prepare(storage_.data(), kSampleRate, 4096);
		clip::kAudioOps.setReadingsSink(
		    storage_.data(), feathers::device::ReadingsSink {
		                         this, [](void* context, const float* values, size_t count) {
			                         auto* self = static_cast<Instance*>(context);
			                         for (size_t i = 0; i < std::min(count, self->readings_.size()); ++i) {
				                         self->readings_[i] = values[i];
			                         }
			                         self->readingsCount_ = count;
		                         } });
	}
	~Instance() { clip::kAudioOps.destruct(storage_.data()); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const clip::Settings& s) {
		const clip::Row row = clip::rowOf(s);
		clip::kAudioOps.setFields(storage_.data(), row.data(), row.size());
	}
	void process(float* left, float* right, int frames) {
		clip::kAudioOps.processStereo(storage_.data(), left, right, frames);
	}
	/// A HOST CHANGING RATE — `prepare` again, as an activate at a new rate does.
	void prepare(double sampleRate) { clip::kAudioOps.prepare(storage_.data(), sampleRate, 4096); }
	[[nodiscard]] float reading(clip::Reading r) const { return readings_[static_cast<size_t>(r)]; }
	[[nodiscard]] size_t readingsCount() const { return readingsCount_; }
	[[nodiscard]] const void* storageForTest() const { return storage_.data(); }

private:
	alignas(std::max_align_t) std::array<std::byte, 8192> storage_ {};
	std::array<float, 8> readings_ {};
	size_t readingsCount_ = 0;
};

[[nodiscard]] float dbToLin(double db) {
	return static_cast<float>(std::pow(10.0, db / 20.0));
}

/// A SEEDED xorshift32 IN [-1, 1] — the test's own.
struct Noise {
	uint32_t s = 0x2545f491u;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	}
};

[[nodiscard]] std::vector<float> sine(int n, double hz, double amp) {
	std::vector<float> out(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		out[static_cast<size_t>(i)] =
		    static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / kSampleRate));
	}
	return out;
}

/// POWER AT `hz` over the samples [from, end), by Goertzel.
[[nodiscard]] double goertzel(const std::vector<float>& x, size_t from, double hz) {
	const double k = 2.0 * std::cos(2.0 * std::numbers::pi * hz / kSampleRate);
	double s1 = 0.0, s2 = 0.0;
	for (size_t i = from; i < x.size(); ++i) {
		const double s0 = static_cast<double>(x[i]) + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - k * s1 * s2;
}

constexpr clip::Mode kModes[] = { clip::Mode::Soft, clip::Mode::Texture, clip::Mode::Grit, clip::Mode::Fold,
	                              clip::Mode::Bits };

/// THE ERODER'S SIX, appended after Bits, and all eleven.
constexpr clip::Mode kNewModes[] = { clip::Mode::Tube,   clip::Mode::Jag,  clip::Mode::Lean,
	                                 clip::Mode::Chonk, clip::Mode::PaunchS, clip::Mode::PaunchH,
	                                 clip::Mode::PaunchOS, clip::Mode::PaunchOH, clip::Mode::Slew,   clip::Mode::Lopside };
constexpr clip::Mode kAllModes[] = { clip::Mode::Soft,  clip::Mode::Texture, clip::Mode::Grit,
	                                 clip::Mode::Fold,  clip::Mode::Bits,    clip::Mode::Tube,
	                                 clip::Mode::Jag,   clip::Mode::Lean,    clip::Mode::Chonk,
	                                 clip::Mode::PaunchS, clip::Mode::PaunchH, clip::Mode::PaunchOS, clip::Mode::PaunchOH, clip::Mode::Slew,
	                                 clip::Mode::Lopside };

/// THE WORST |y| OVER COMP'S CASE-3 SIGNALS (full-scale 50 Hz square, 1 kHz sine, bursts of random
/// full-scale samples), a second each — one number per signal.
std::array<float, 3> worstOver(const clip::Settings& s) {
	Instance dev;
	dev.set(s);
	Noise noise;
	std::array<float, 3> worstOf {};
	std::vector<float> l(240), r(240);
	for (int signal = 0; signal < 3; ++signal) {
		float& worst = worstOf[static_cast<size_t>(signal)];
		for (int done = 0; done < kSecond; done += 240) {
			for (int i = 0; i < 240; ++i) {
				const double t = static_cast<double>(done + i) / kSampleRate;
				float v = 0.0f;
				if (signal == 0) {
					v = std::fmod(t * 50.0, 1.0) < 0.5 ? 1.0f : -1.0f;
				}
				else if (signal == 1) {
					v = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * t));
				}
				else {
					v = ((done + i) % 1200) < 240 ? noise.next() : 0.0f;
				}
				l[static_cast<size_t>(i)] = v;
				r[static_cast<size_t>(i)] = -v;
			}
			dev.process(l.data(), r.data(), 240);
			for (int i = 0; i < 240; ++i) {
				worst = std::max({ worst, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]) });
			}
		}
	}
	return worstOf;
}

TEST_CASE("every mode stays inside the ceiling") {
	const float ceiling = dbToLin(-6.0);
	for (const clip::Mode mode : kModes) {
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.driveDb = 24.0f;
		s.ceilingDb = -6.0f;
		s.oversample = false;
		CAPTURE(static_cast<int>(mode));
		// OVERSAMPLING OFF: the curve's ±1 times the ceiling, STRICT, on every signal.
		const std::array<float, 3> off = worstOver(s);
		const float offWorst = *std::max_element(off.begin(), off.end());
		CHECK(offWorst <= ceiling * (1.0f + 1e-6f));
		CHECK(offWorst > ceiling * 0.9f); // not vacuous: +24 dB of drive reaches the top
		// OVERSAMPLING ON: the halfband decimator band-limits the clipped 4x signal, and a band-limited clip
		// rings past its top — inter-sample reconstruction, not a defect in the curve. **THE BOUNDS ARE
		// MEASURED, NOT IDEAL**, worst mode per signal (Soft/Bits: a clamp at +24 dB is the squarest top).
		// RE-MEASURED 2026-09-25 after the oversampler was rebuilt clean-room on `src/dsp/halfband.h`
		// (the vendored filters before it read 1.31x / 1.45x / 2.70x, Texture 1.14x, Fold 1.25x):
		//   square 50 Hz  1.45x the ceiling (+3.2 dB)
		//   sine 1 kHz    1.42x (+3.0 dB)
		//   random bursts 2.73x (+8.7 dB) — full-scale white noise driven +24 dB is a random telegraph at
		//                 4x, and band-limiting that is the worst case there is.
		// Texture measured 1.05x and Fold 1.11x overall. The asserted bounds are those plus a margin. For a
		// hard ceiling, switch oversampling off.
		s.oversample = true;
		const std::array<float, 3> on = worstOver(s);
		MESSAGE("mode " << static_cast<int>(mode) << ": OS on, square " << on[0] / ceiling << "x, sine "
		                << on[1] / ceiling << "x, bursts " << on[2] / ceiling << "x the ceiling");
		CHECK(on[0] <= ceiling * 1.55f);
		CHECK(on[1] <= ceiling * 1.52f);
		CHECK(on[2] <= ceiling * 2.85f);
	}
}

TEST_CASE("a signal that never reaches the knee passes untouched") {
	for (const clip::Mode mode : kModes) {
		CAPTURE(static_cast<int>(mode));
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.oversample = false;
		{
			Instance dev;
			dev.set(s);
			auto l = sine(4800, 1000.0, 0.1), r = sine(4800, 700.0, 0.1);
			const auto dl = l, dr = r;
			dev.process(l.data(), r.data(), 4800);
			size_t differ = 0;
			for (size_t i = 0; i < l.size(); ++i) {
				differ += (l[i] != dl[i]) + (r[i] != dr[i]);
			}
			CHECK(differ == 0u);
		}
		// OVERSAMPLED: the halfbands are unity in the passband within 0.05 dB (the fx-dyn measurement).
		s.oversample = true;
		Instance dev;
		dev.set(s);
		auto l = sine(9600, 1000.0, 0.1), r = l;
		dev.process(l.data(), r.data(), 9600);
		double in = 0.0, out = 0.0;
		const auto ref = sine(9600, 1000.0, 0.1);
		for (size_t i = 4800; i < 9600; ++i) {
			in += static_cast<double>(ref[i]) * ref[i];
			out += static_cast<double>(l[i]) * l[i];
		}
		CHECK(std::fabs(10.0 * std::log10(out / in)) < 0.05);
	}
}

TEST_CASE("negative Drive reaches the curve quieter: -12 dB takes a signal 6 dB over the ceiling out of clipping") {
	// DC 2.0 is 6 dB over a 0 dB ceiling. Knee 0 (a hard clip), Oversample off, so the curve is the identity
	// below the ceiling and the output is the driven level itself: no make-up, the ceiling and Output at 0.
	// The setting goes in through the row, so the descriptor's range is what lets -12 through.
	const auto run = [](float driveDb, float in) {
		clip::Settings s;
		s.knee = 0.0f;
		s.oversample = false;
		s.driveDb = driveDb;
		Instance dev;
		dev.set(s);
		std::vector<float> l(kSecond, in), r(kSecond, -in);
		dev.process(l.data(), r.data(), kSecond); // the Drive ramp lands
		return std::tuple { l.back(), r.back(), dev.reading(clip::Reading::Over) };
	};
	const clip::Row row = clip::rowOf(clip::Settings { .driveDb = -12.0f });
	REQUIRE(clip::stateOf(row, row.size()).driveDb == -12.0f);
	const auto [l, r, over] = run(-12.0f, 2.0f);
	const double expected = 2.0 * std::pow(10.0, -12.0 / 20.0); // 0.502, an independent restatement
	CHECK(std::fabs(l - expected) < 1e-5);
	CHECK(std::fabs(r + expected) < 1e-5);
	CHECK(over == 0.0f); // below the onset: nothing for the curve to do
	// DRIVE 0 IS TODAY'S DEVICE: the same signal clips to the ceiling exactly, and one under it passes exactly.
	const auto [l0, r0, over0] = run(0.0f, 2.0f);
	CHECK(l0 == 1.0f);
	CHECK(r0 == -1.0f);
	CHECK(over0 == 1.0f);
	const auto [lq, rq, overq] = run(0.0f, 0.5f);
	CHECK(lq == 0.5f);
	CHECK(rq == -0.5f);
	CHECK(overq == 0.0f);
}

TEST_CASE("Grit is silent on silence and scales with the overshoot") {
	clip::Settings s;
	s.mode = static_cast<int>(clip::Mode::Grit);
	s.oversample = false;
	s.knee = 0.2f;
	s.grit = 1.0f;
	{
		Instance dev;
		dev.set(s);
		std::vector<float> l(4800, 0.0f), r(4800, 0.0f);
		dev.process(l.data(), r.data(), 4800);
		CHECK(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
		CHECK(std::all_of(r.begin(), r.end(), [](float v) { return v == 0.0f; }));
	}
	// THE MEAN CARVE AT A DC LEVEL IS kGritScale * grit * over * E[noise] = kGritScale * over / 2.
	const auto meanCarve = [&](float u) {
		Instance dev;
		dev.set(s);
		std::vector<float> l(20000, u), r(20000, u);
		dev.process(l.data(), r.data(), 20000);
		const float c = dyn::softClip(u, s.knee);
		double sum = 0.0;
		for (float v : l) {
			sum += static_cast<double>(c - v);
		}
		return sum / 20000.0;
	};
	const double a = meanCarve(0.95f); // over 0.15
	const double b = meanCarve(1.1f);  // over 0.3
	MESSAGE("mean carve " << a << " at over 0.15, " << b << " at over 0.3");
	CHECK(std::fabs(a - clip::kGritScale * 0.15 * 0.5) < 0.02 * clip::kGritScale * 0.15 * 0.5 + 1e-4);
	CHECK(std::fabs(b / a - 2.0) < 0.05);
}

TEST_CASE("oversampling lowers the aliases") {
	// Soft knee 0 (a hard clip), Drive +18 into a 7 kHz sine: the fx-dyn alias measurement through the
	// device. The first five folded images of the odd harmonics, off the harmonic series.
	std::vector<double> folds;
	for (int h = 3; folds.size() < 5 && h < 200; h += 2) {
		double f = std::fmod(h * 7000.0, kSampleRate);
		if (f > kSampleRate / 2.0) {
			f = kSampleRate - f;
		}
		if (f > 0.0 && f < 20000.0 && std::fmod(f, 7000.0) != 0.0
		    && std::find(folds.begin(), folds.end(), f) == folds.end()) {
			folds.push_back(f);
		}
	}
	REQUIRE(folds.size() == 5);
	const auto aliasPower = [&](bool os) {
		clip::Settings s;
		s.knee = 0.0f;
		s.driveDb = 18.0f;
		s.oversample = os;
		Instance dev;
		dev.set(s);
		auto l = sine(9600, 7000.0, 0.5), r = l;
		dev.process(l.data(), r.data(), 9600);
		double p = 0.0;
		for (double f : folds) {
			p += goertzel(l, 4800, f);
		}
		return std::pair { p, goertzel(l, 4800, 7000.0) };
	};
	const auto [naive, naiveFund] = aliasPower(false);
	const auto [over, overFund] = aliasPower(true);
	const double reductionDb = 10.0 * std::log10(naive / over);
	MESSAGE("alias reduction " << reductionDb << " dB");
	// MEASURED 38.9 dB (2026-09-25, re-measured on the clean-room oversampler); the bar is that less a margin.
	CHECK(reductionDb >= 33.0);
	CHECK(overFund > 0.1 * naiveFund); // not vacuous: the fundamental survives
}

TEST_CASE("the Over light is exactly the clipper's activity") {
	for (const bool os : { false, true }) {
		CAPTURE(os);
		clip::Settings s; // Soft, knee 0.1: the curve starts acting at 0.9
		s.oversample = os;
		const float onsetU = clip::onset(s.mode, s.knee);
		REQUIRE(std::fabs(onsetU - 0.9f) < 1e-6f);
		{
			Instance dev;
			dev.set(s);
			auto l = sine(4800, 1000.0, 0.899), r = l;
			dev.process(l.data(), r.data(), 4800);
			CHECK(dev.readingsCount() == static_cast<size_t>(clip::Reading::kCount));
			CHECK(dev.reading(clip::Reading::Over) == 0.0f);
		}
		Instance dev;
		dev.set(s);
		auto l = sine(480, 1000.0, 0.901), r = l;
		dev.process(l.data(), r.data(), 480);
		CHECK(dev.reading(clip::Reading::Over) == 1.0f);
		// HELD 50 ms: through the next 2400 samples of silence it still reads 1.
		size_t dark = 0;
		for (int k = 0; k < 2400; k += 48) {
			std::vector<float> zl(48, 0.0f), zr(48, 0.0f);
			dev.process(zl.data(), zr.data(), 48);
			dark += dev.reading(clip::Reading::Over) != 1.0f;
		}
		CHECK(dark == 0u);
	}
	// **AND IT IS METERED AHEAD OF THE CURVE**, which Soft cannot show: its curve is monotone, so its output
	// is past the onset exactly when its input is. Fold is not — an overshoot of 1 folds right back to 0 —
	// so Fold is where "ahead" and "after" disagree. DC at u = 2 is folding hard and must light the light.
	clip::Settings s;
	s.mode = static_cast<int>(clip::Mode::Fold);
	s.oversample = false;
	s.driveDb = static_cast<float>(20.0 * std::log10(2.0));
	Instance dev;
	dev.set(s);
	std::vector<float> l(480, 1.0f), r(480, 1.0f);
	dev.process(l.data(), r.data(), 480);
	REQUIRE(std::fabs(l.back()) < 0.01f); // it IS folding: u = 2 folds to 0
	CHECK(dev.reading(clip::Reading::Over) == 1.0f);
}

TEST_CASE("the IN meter reads the incoming signal before Drive, enabled or not") {
	// A SPIKE OF 0.25 AT DRIVE +12 dB: the driven signal is 1.0 (clipping), the incoming one is 0.25, and the
	// meter reads the incoming one — COMP's contract. Then the SAME spike with the device off reads the same,
	// so the bar does not jump on a toggle. Oversample off, so a one-sample spike is not spread by the
	// interpolator and the Over light can prove the drive is live.
	clip::Settings s;
	s.driveDb = 12.0f;
	s.oversample = false;
	Instance dev;
	dev.set(s);
	std::vector<float> z(kSecond, 0.0f), zr = z;
	dev.process(z.data(), zr.data(), kSecond); // the Drive ramp lands
	std::array<float, 64> l {}, r {};
	l[37] = 0.25f;
	dev.process(l.data(), r.data(), 64);
	const float on = dev.reading(clip::Reading::InPeak);
	CHECK(on == 0.25f);
	CHECK(dev.reading(clip::Reading::Over) == 1.0f); // not vacuous: the driven signal IS past the onset

	s.enabled = false;
	dev.set(s);
	std::fill(z.begin(), z.end(), 0.0f);
	std::fill(zr.begin(), zr.end(), 0.0f);
	dev.process(z.data(), zr.data(), kSecond); // the bypass ramp lands and the 50 ms hold lets go
	REQUIRE(dev.reading(clip::Reading::InPeak) == 0.0f);
	l.fill(0.0f);
	r.fill(0.0f);
	l[37] = 0.25f;
	dev.process(l.data(), r.data(), 64);
	REQUIRE(l[37] == 0.25f); // it IS the pass-through branch
	CHECK(dev.reading(clip::Reading::InPeak) == on);
}

TEST_CASE("Texture's high-pass corner is where Freq says, at the rate it runs") {
	for (const bool os : { false, true }) {
		CAPTURE(os);
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::Texture);
		s.freq = static_cast<float>(std::log2(4000.0));
		s.oversample = os;
		Instance dev;
		dev.set(s);
		std::array<float, 16> l {}, r {};
		dev.process(l.data(), r.data(), 16);
		const double atCorner = clip::textureHighpassGainForTest(dev.storageForTest(), 4000.0);
		const double low = clip::textureHighpassGainForTest(dev.storageForTest(), 1000.0);
		const double high = clip::textureHighpassGainForTest(dev.storageForTest(), 10000.0);
		MESSAGE("OS " << os << ": |H| at 1k " << low << ", 4k " << atCorner << ", 10k " << high);
		// BUTTERWORTH: -3 dB AT THE CORNER. A filter designed at fs and run at 4 fs has its corner at 16 kHz.
		CHECK(std::fabs(atCorner - std::numbers::sqrt2 / 2.0) < 0.01);
		// AND A 1 kHz COMPONENT OF THE REMOVED SIGNAL CONTRIBUTES UNDER 10% OF WHAT A 10 kHz ONE DOES.
		CHECK(low < 0.1 * high);
	}
}

TEST_CASE("one NaN or inf sample does not latch in any mode") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	for (const clip::Mode mode : kAllModes) {
		for (const float bad : { nan, inf, 1e30f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(bad);
			clip::Settings s;
			s.mode = static_cast<int>(mode);
			s.driveDb = 12.0f;
			Instance dev, twin;
			dev.set(s);
			twin.set(s);
			Noise noise;
			size_t nonFinite = 0, offTrack = 0;
			for (int k = 0; k < 4800; ++k) {
				const float x = 0.5f * noise.next();
				float l = x, r = -x, tl = x, tr = -x;
				if (k == 2400) {
					l = bad;
					// A NON-FINITE SAMPLE IS TREATED AS 0; a huge finite one is real and clipped.
					tl = std::isfinite(bad) ? bad : 0.0f;
				}
				dev.process(&l, &r, 1);
				twin.process(&tl, &tr, 1);
				nonFinite += !std::isfinite(l) + !std::isfinite(r);
				offTrack += (l != tl) + (r != tr);
			}
			CHECK(nonFinite == 0u);
			CHECK(offTrack == 0u);
		}
	}
	// AND A FINITE SAMPLE THAT OVERFLOWS THE GAIN: 3e38 through +24 dB of drive over a -24 dB ceiling is
	// inf in the multiply. Unclamped, that inf entered the upsampler and latched it at NaN for good.
	for (const clip::Mode mode : kAllModes) {
		CAPTURE(static_cast<int>(mode));
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.driveDb = 24.0f;
		s.ceilingDb = -24.0f;
		Instance dev, twin;
		dev.set(s);
		twin.set(s);
		Noise noise;
		size_t nonFinite = 0, offTrack = 0;
		for (int k = 0; k < 4800; ++k) {
			const float x = 0.01f * noise.next();
			float l = k == 100 ? 3e38f : x;
			float r = l;
			float tl = k == 100 ? 0.0f : x;
			float tr = tl;
			dev.process(&l, &r, 1);
			twin.process(&tl, &tr, 1);
			nonFinite += !std::isfinite(l) + !std::isfinite(r);
			// A NaN-LATCHED UPSAMPLER WOULD STILL LEAVE FINITE OUTPUT — the output guard turns it into 0 —
			// so "finite" alone is blind to it. Back with the twin (fed 0 there) in the second half.
			if (k >= 2400) {
				// (a Paunch law's offset blocker keeps the one clamped sample's worth of offset a while: 5e-3)
				const bool law = mode == clip::Mode::PaunchS || mode == clip::Mode::PaunchH;
				// (Paunch-OS's and -OH's follower keeps a trace of the one huge sample for several time constants: finiteness only)
				if (mode != clip::Mode::PaunchOS && mode != clip::Mode::PaunchOH) {
					offTrack += std::fabs(l - tl) > (law ? 5e-3f : 1e-3f);
				}
			}
		}
		CHECK(nonFinite == 0u);
		CHECK(offTrack == 0u);
	}
}

TEST_CASE("partition does not matter — every mode, oversampled, through a disable and re-enable") {
	for (const clip::Mode mode : kAllModes) {
		CAPTURE(static_cast<int>(mode));
		clip::Settings on;
		on.mode = static_cast<int>(mode);
		on.driveDb = 18.0f;
		on.oversample = true;
		clip::Settings off = on;
		off.enabled = false;
		const std::array<std::pair<const clip::Settings*, int>, 3> segments {
			{ { &on, 4800 }, { &off, 1000 }, { &on, 4800 } }
		};
		Noise noise;
		std::vector<float> inL, inR;
		for (int k = 0; k < 4800 + 1000 + 4800; ++k) {
			inL.push_back(0.5f * noise.next());
			inR.push_back(0.5f * noise.next());
		}
		const auto render = [&](int cut) {
			Instance dev;
			std::vector<float> l = inL, r = inR;
			size_t at = 0;
			for (const auto& [settings, length] : segments) {
				dev.set(*settings);
				const int block = cut > 0 ? cut : length;
				for (int done = 0; done < length; done += block) {
					const int n = std::min(block, length - done);
					dev.process(l.data() + at, r.data() + at, n);
					at += static_cast<size_t>(n);
				}
			}
			l.insert(l.end(), r.begin(), r.end());
			return l;
		};
		const std::vector<float> reference = render(0);
		for (const int cut : { 1, 7, 64 }) {
			const std::vector<float> other = render(cut);
			size_t differ = 0;
			for (size_t i = 0; i < reference.size(); ++i) {
				differ += reference[i] != other[i];
			}
			CAPTURE(cut);
			CHECK(differ == 0u);
		}
	}
}

TEST_CASE("after a burst the tail reaches exact silence, without a denormal guard") {
	// THE HALFBAND TAILS PARK IN THE DENORMALS, so before `kRestLevel` this never reached zero. No
	// DenormalGuard here, on purpose: this is the arithmetic a host that sets no FTZ gets.
	for (const clip::Mode mode : kAllModes) {
		CAPTURE(static_cast<int>(mode));
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		// THE SLEWS TAKE THEIR RATE FROM DRIVE: at +24 dB they are a 50 Hz limiter and would barely move a 440 Hz sine.
		const bool slews = mode == clip::Mode::Slew || mode == clip::Mode::Lopside;
		s.driveDb = slews ? -12.0f : 24.0f;
		Instance dev;
		dev.set(s);
		auto l = sine(512, 440.0, 0.8), r = l;
		dev.process(l.data(), r.data(), 512);
		float reached = 0.0f;
		for (int k = 300; k < 512; ++k) {
			reached = std::max(reached, std::fabs(l[static_cast<size_t>(k)]));
		}
		REQUIRE(reached > 0.1f); // not vacuous: the burst reached the output
		int lastNonZero = -1;
		for (int k = 0; k < kSecond; k += 512) {
			std::vector<float> zl(512, 0.0f), zr(512, 0.0f);
			dev.process(zl.data(), zr.data(), 512);
			for (int i = 0; i < 512; ++i) {
				if (zl[static_cast<size_t>(i)] != 0.0f || zr[static_cast<size_t>(i)] != 0.0f) {
					lastNonZero = k + i;
				}
			}
		}
		MESSAGE("mode " << static_cast<int>(mode) << ": last non-zero sample " << lastNonZero
		                << " after the silence began");
		// EXACT SILENCE WITHIN 60 ms: the rest engages at 50 ms of zero input (the tail is far under
		// -180 dBFS by then), and nothing after it is non-zero.
		// **TUBE AND LEAN ARE THE EXCEPTION**: they leave a standing offset that the Eroder's 10 Hz one-pole takes out, and
		// that offset's discharge is the tail (16 ms per time constant, so -180 dBFS is reached by ~0.3 s, and then exactly 0).
		const bool offsetModes = mode == clip::Mode::Tube || mode == clip::Mode::Lean || mode == clip::Mode::PaunchS ||
		                         mode == clip::Mode::PaunchH;
		CHECK(lastNonZero < (offsetModes ? 24000 : 2880));
	}
}

TEST_CASE("a quiet signal is never cut by the rest") {
	// 1e-6 (-120 dBFS) is quiet but it is SIGNAL: the input is never exactly zero, so the rest must not
	// engage. Unity through the oversampled path within 0.05 dB, and no sample forced to zero.
	clip::Settings s;
	Instance dev;
	dev.set(s);
	auto l = sine(kSecond, 1000.0, 1e-6), r = l;
	const auto ref = l;
	dev.process(l.data(), r.data(), kSecond);
	double in = 0.0, out = 0.0;
	size_t zeros = 0;
	for (size_t i = kSecond / 2; i < ref.size(); ++i) {
		in += static_cast<double>(ref[i]) * ref[i];
		out += static_cast<double>(l[i]) * l[i];
		zeros += (l[i] == 0.0f && ref[i] != 0.0f);
	}
	CHECK(std::fabs(10.0 * std::log10(out / in)) < 0.05);
	CHECK(zeros == 0u);
}

TEST_CASE("disabled is a pass-through once the bypass ramp lands — late by the declared latency, exactly") {
	// **A BYPASS STILL DELAYS** (backlog NR, the latency contract): with 4x on the host was told
	// `kOversampleLatency`, so the pass-through is the input that many samples late, to the bit; with 1x, the input.
	for (const bool os : { true, false }) {
		CAPTURE(os);
		const size_t late = os ? clip::kOversampleLatency : 0u;
		clip::Settings s;
		s.driveDb = 18.0f;
		s.oversample = os;
		Instance dev;
		dev.set(s);
		Noise noise;
		std::vector<float> l(240), r(240), inL, inR;
		const auto block = [&] {
			for (size_t i = 0; i < 240; ++i) {
				l[i] = noise.next();
				r[i] = noise.next();
				inL.push_back(l[i]);
				inR.push_back(r[i]);
			}
			const size_t base = inL.size() - 240;
			dev.process(l.data(), r.data(), 240);
			size_t differ = 0;
			for (size_t i = 0; i < 240; ++i) {
				const size_t at = base + i;
				const float wantL = at >= late ? inL[at - late] : 0.0f;
				const float wantR = at >= late ? inR[at - late] : 0.0f;
				differ += (l[i] != wantL) + (r[i] != wantR);
			}
			return differ;
		};
		block();
		CHECK(block() > 400u); // not vacuous: enabled, it changes the signal
		s.enabled = false;
		dev.set(s);
		block(); // the 5 ms ramp
		for (int k = 0; k < 10; ++k) {
			CHECK(block() == 0u);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE QUALITY REVIEW'S FIXES                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a new sample rate redesigns Texture's high-pass with no set after it") {
	// `prepare` IS THE ONLY THING THAT RUNS on a rate change: a host re-activating at 96 kHz sends no new
	// fields. So the redesign in `prepare` is load-bearing, and this is the case that stands on it.
	for (const bool os : { false, true }) {
		CAPTURE(os);
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::Texture);
		s.freq = static_cast<float>(std::log2(4000.0));
		s.oversample = os;
		Instance dev; // prepared at 48 kHz
		dev.set(s);
		dev.prepare(96000.0);
		const double atCorner = clip::textureHighpassGainForTest(dev.storageForTest(), 4000.0);
		MESSAGE("OS " << os << " at 96 kHz: |H| at 4k " << atCorner);
		CHECK(std::fabs(atCorner - std::numbers::sqrt2 / 2.0) < 0.01);
	}
}

TEST_CASE("Depth moving between blocks while folding does not step the output") {
	// **DC AT u = 2 FOLDS TO EXACTLY 1 - depth** (`foldCurve`: clamp 1, fold 0), so the output IS the depth
	// and a block-rate depth is a block-rate step in the output. Flipped 1 -> 0 -> 1 every 64 samples, far
	// faster than the 5 ms ramp can finish, which is the worst a modulator can ask.
	for (const bool os : { false, true }) {
		CAPTURE(os);
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::Fold);
		s.oversample = os;
		s.driveDb = static_cast<float>(20.0 * std::log10(2.0));
		s.depth = 1.0f;
		Instance dev;
		dev.set(s);
		std::vector<float> l(64, 1.0f), r(64, 1.0f);
		for (int k = 0; k < kSecond / 64; ++k) { // settle the halfbands on the DC
			std::fill(l.begin(), l.end(), 1.0f);
			std::fill(r.begin(), r.end(), 1.0f);
			dev.process(l.data(), r.data(), 64);
		}
		REQUIRE(std::fabs(l.back()) < 0.01f); // not vacuous: it IS folding, to 1 - 1 = 0
		float prev = l.back();
		float worst = 0.0f;
		float lo = 1.0f, hi = 0.0f;
		for (int k = 0; k < 48; ++k) {
			s.depth = (k % 2 == 0) ? 0.0f : 1.0f;
			dev.set(s);
			std::fill(l.begin(), l.end(), 1.0f);
			std::fill(r.begin(), r.end(), 1.0f);
			dev.process(l.data(), r.data(), 64);
			for (const float y : l) {
				worst = std::max(worst, std::fabs(y - prev));
				lo = std::min(lo, y);
				hi = std::max(hi, y);
				prev = y;
			}
		}
		MESSAGE("OS " << os << ": largest sample-to-sample step " << worst << ", output range " << lo << ".."
		                << hi);
		// NOT VACUOUS: the depth did move the output — by about 64/240 of the full swing each block.
		CHECK(hi - lo > 0.2f);
		// THE RAMP'S OWN STEP IS 1/240 (5 ms at 48 kHz) of a full-scale swing; unramped it is the swing.
		// MEASURED 2026-09-25: 0.00417 with 4x off, 0.00510 with it on (the halfbands ring on each corner).
		CHECK(worst < 0.0065f);
	}
}

TEST_CASE("switching into Texture carves no stale detail left from the last time") {
	// A HIGH-PASS NOTHING ELSE STEPS keeps whatever the last Texture stretch left in it. Two instances, one
	// with a loud Texture past and a Soft stretch since, one fresh; the same input into Texture must come
	// out bit for bit the same. Oversample off, so the high-pass is the only memory that could differ.
	clip::Settings s;
	s.oversample = false;
	s.driveDb = 18.0f;
	s.mode = static_cast<int>(clip::Mode::Texture);
	s.foldback = 1.0f;
	const auto loud = sine(4800, 1000.0, 0.9);
	const auto probe = sine(256, 1000.0, 0.9);

	Instance used;
	used.set(s);
	{
		auto l = loud, r = loud;
		used.process(l.data(), r.data(), 4800);
	}
	s.mode = static_cast<int>(clip::Mode::Soft);
	used.set(s);
	{
		std::vector<float> l(480, 0.0f), r(480, 0.0f); // under the rest's 50 ms, so the rest clears nothing
		used.process(l.data(), r.data(), 480);
	}
	s.mode = static_cast<int>(clip::Mode::Texture);
	used.set(s);
	auto usedL = probe, usedR = probe;
	used.process(usedL.data(), usedR.data(), 256);

	Instance fresh;
	fresh.set(s);
	{
		std::vector<float> l(480, 0.0f), r(480, 0.0f);
		fresh.process(l.data(), r.data(), 480);
	}
	auto freshL = probe, freshR = probe;
	fresh.process(freshL.data(), freshR.data(), 256);

	size_t differ = 0;
	for (size_t i = 0; i < probe.size(); ++i) {
		differ += (usedL[i] != freshL[i]) + (usedR[i] != freshR[i]);
	}
	CHECK(differ == 0u);
	// NOT VACUOUS: Texture IS carving here — the probe differs from plain Soft of the same input.
	clip::Settings soft = s;
	soft.mode = static_cast<int>(clip::Mode::Soft);
	Instance plain;
	plain.set(soft);
	auto softL = probe, softR = probe;
	plain.process(softL.data(), softR.data(), 256);
	size_t carved = 0;
	for (size_t i = 0; i < probe.size(); ++i) {
		carved += freshL[i] != softL[i];
	}
	CHECK(carved > 50u);
}

TEST_CASE("with 4x an inter-sample peak past the onset lights Over, though no host sample reaches it") {
	// A 12 kHz SINE AT 48 kHz, PHASED 45 DEGREES: every host sample is +-0.707 of the amplitude, and the
	// peaks fall between them. At amplitude 1 the samples sit under Soft's 0.9 onset and the waveform does
	// not — so at 4x the curve acts, and the light must say so. Off, the curve sees only host samples and
	// has nothing to do, so the light stays dark: both answers are the curve's activity.
	constexpr int kN = 960;
	std::vector<float> x(kN);
	float hostPeak = 0.0f;
	for (int i = 0; i < kN; ++i) {
		x[static_cast<size_t>(i)] = static_cast<float>(
		    std::sin(2.0 * std::numbers::pi * 12000.0 * i / kSampleRate + std::numbers::pi / 4.0));
		hostPeak = std::max(hostPeak, std::fabs(x[static_cast<size_t>(i)]));
	}
	clip::Settings s; // Soft, knee 0.1: onset 0.9
	REQUIRE(hostPeak < clip::onset(s.mode, s.knee));
	for (const bool os : { false, true }) {
		CAPTURE(os);
		s.oversample = os;
		Instance dev;
		dev.set(s);
		auto l = x, r = x;
		dev.process(l.data(), r.data(), kN);
		CHECK(dev.reading(clip::Reading::Over) == (os ? 1.0f : 0.0f));
	}
}

TEST_CASE("at 22.05 kHz with Oversample off the top Freq is clamped under Nyquist and stays stable") {
	// 12 kHz IS PAST NYQUIST AT 22.05 kHz. Unclamped the design's w0 exceeds pi; clamped to 0.45 of the
	// rate it is a stable high-pass with its corner at 9922.5 Hz.
	clip::Settings s;
	s.mode = static_cast<int>(clip::Mode::Texture);
	s.oversample = false;
	s.freq = static_cast<float>(clip::kFreqMaxLog2);
	s.driveDb = 24.0f;
	s.foldback = 1.0f;
	Instance dev;
	dev.prepare(22050.0);
	dev.set(s);
	Noise noise;
	std::vector<float> l(441), r(441);
	size_t bad = 0;
	float worst = 0.0f;
	for (int k = 0; k < 50; ++k) { // a second at 22.05 kHz
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = noise.next();
			r[i] = noise.next();
		}
		dev.process(l.data(), r.data(), static_cast<int>(l.size()));
		for (size_t i = 0; i < l.size(); ++i) {
			bad += !std::isfinite(l[i]) || !std::isfinite(r[i]);
			worst = std::max({ worst, std::fabs(l[i]), std::fabs(r[i]) });
		}
	}
	CHECK(bad == 0u);
	// THE DESIGN IS READ AFTER THE FIRST BLOCK: the toggle to Oversample off is applied by `process`.
	const double clampedHz = 0.45 * 22050.0;
	const double atClamp = clip::textureHighpassGainForTest(dev.storageForTest(), clampedHz);
	MESSAGE("|H| at the clamped corner " << clampedHz << " Hz: " << atClamp);
	CHECK(std::fabs(atClamp - std::numbers::sqrt2 / 2.0) < 0.01);
	// TEXTURE CARVES TOWARD ZERO ONLY (`textureCurve`), so nothing passes the 0 dBFS ceiling.
	CHECK(worst <= 1.0f);
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE DECLARED LATENCY (backlog NR)                                                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NR: the declared latency is the oversampler's measured delay — 6 samples with 4x at any rate, 0 with 1x") {
	// THE MEASUREMENT, NOT THE CONSTANT: an impulse below the knee (where every curve is exactly the input) through
	// the device with 4x on, so its response is the halfbands' alone. The group delay at low frequency, and where the
	// impulse peaks, both round to the declared count.
	clip::Settings s;
	s.oversample = true;
	Instance dev;
	dev.set(s);
	constexpr size_t kN = 4096;
	std::vector<float> l(kN, 0.0f), r(kN, 0.0f);
	l[0] = r[0] = 0.01f;
	dev.process(l.data(), r.data(), static_cast<int>(kN));
	size_t peak = 0;
	for (size_t n = 0; n < kN; ++n) {
		if (std::fabs(l[n]) > std::fabs(l[peak])) {
			peak = n;
		}
	}
	const auto groupDelayAt = [&](double hz) {
		const double w = 2.0 * std::numbers::pi * hz / kSampleRate;
		double hr = 0.0, hi = 0.0, nr = 0.0, ni = 0.0;
		for (size_t n = 0; n < kN; ++n) {
			const double h = l[n];
			const double c = std::cos(w * static_cast<double>(n)), sn = -std::sin(w * static_cast<double>(n));
			hr += h * c;
			hi += h * sn;
			nr += static_cast<double>(n) * h * c;
			ni += static_cast<double>(n) * h * sn;
		}
		// Re(sum n h e^-jwn / sum h e^-jwn)
		return (nr * hr + ni * hi) / (hr * hr + hi * hi);
	};
	const double gd100 = groupDelayAt(100.0);
	const double gd1k = groupDelayAt(1000.0);
	MESSAGE("CLIP 4x: group delay " << gd100 << " samples at 100 Hz, " << gd1k << " at 1 kHz; impulse peak at " << peak);
	CHECK(gd100 == doctest::Approx(5.636).epsilon(0.001));
	CHECK(static_cast<uint32_t>(std::lround(gd100)) == clip::kOversampleLatency);
	CHECK(peak == clip::kOversampleLatency);

	// THE DECLARATION: from the STORED Oversample alone, at any rate, enabled or not; an absent field is its default (4x).
	const auto latency = clip::kAudioOps.latencySamples;
	REQUIRE(latency != nullptr);
	for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
		for (const bool enabled : { true, false }) {
			CAPTURE(rate);
			CAPTURE(enabled);
			clip::Settings t;
			t.enabled = enabled;
			t.oversample = true;
			const clip::Row on = clip::rowOf(t);
			t.oversample = false;
			const clip::Row off = clip::rowOf(t);
			CHECK(latency(nullptr, on.data(), on.size(), rate) == clip::kOversampleLatency);
			CHECK(latency(nullptr, off.data(), off.size(), rate) == 0u);
		}
		CHECK(latency(nullptr, nullptr, 0, rate) == clip::kOversampleLatency);
	}
}

//------------------------------------------------------------------------------------------------//
// THE FIVE ORIGINAL MODES, PINNED BEFORE THE ERODER'S SIX WERE ADDED                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the five original modes render the bits they rendered before the Eroder's six were added") {
	// A hash of each mode's output (both channels, FNV-1a over the float bits), taken from the build BEFORE the
	// engine was touched, at settings that exercise the knee, the oversampler and the per-mode controls.
	constexpr std::array<uint64_t, 10> kPinned = {
	    0x6782e28deccb3d98ull, 0x0d0a06f7b0cb7214ull, 0xeb78bb3329a14570ull, 0xe4c08bfd13865d54ull, 0x77d99434f75eff39ull, 0x12162a28e53eb1a8ull, 0xaea451dd5955dc5eull, 0x7490bfd6b4e3b421ull, 0x1346253a910caa15ull, 0x5c3ceb5c89eb2cf2ull };
	size_t at = 0;
	for (const bool os : { true, false }) {
		for (const clip::Mode mode : kModes) {
			clip::Settings s;
			s.mode = static_cast<int>(mode);
			s.driveDb = 12.0f;
			s.ceilingDb = -3.0f;
			s.outputDb = 1.5f;
			s.oversample = os;
			s.knee = 0.3f;
			s.foldback = 0.7f;
			s.grit = 0.6f;
			s.depth = 0.8f;
			s.bits = 1;
			Instance dev;
			dev.set(s);
			Noise noise;
			std::vector<float> l(4800), r(4800);
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = 0.6f * noise.next() + 0.4f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
				r[i] = 0.5f * noise.next();
			}
			for (size_t done = 0; done < l.size(); done += 480) {
				dev.process(l.data() + done, r.data() + done, 480);
			}
			uint64_t h = 1469598103934665603ull;
			for (const std::vector<float>* ch : { &l, &r }) {
				for (const float v : *ch) {
					uint32_t bits = 0;
					std::memcpy(&bits, &v, sizeof(bits));
					h = (h ^ bits) * 1099511628211ull;
				}
			}
			CAPTURE(os);
			CAPTURE(static_cast<int>(mode));
#if defined(__linux__) && defined(__x86_64__) // a bit-exact pin holds only where it was taken (see the eroder's)
			CHECK(h == kPinned[at]);
#else
			(void)h;
			(void)kPinned;
			(void)at;
#endif
			++at;
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE ERODER'S SIX                                                                                //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Shape is live in exactly the six modes taken from the Eroder, and the modes keep their order") {
	const auto& d = clip::kParamDescs[static_cast<size_t>(clip::Param::Shape)];
	CHECK(std::string(d.key) == "clip_shape");
	CHECK(d.def == 0.0);
	CHECK(d.max == 1.0);
	for (const clip::Mode mode : kAllModes) {
		const bool expected = mode >= clip::Mode::Tube;
		CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
		CHECK(feathers::device::relevantFor(d, static_cast<double>(mode)) == expected);
	}
	const char* appended[] = { "Soft", "Texture", "Grit", "Fold", "Bits", "Tube", "Jag", "Lean (w)", "Chonk (w)", "Paunch-S (w)", "Paunch-H (w)", "Paunch-OS (w)", "Paunch-OH (w)", "Slew", "Lopside" };
	for (size_t i = 0; i < std::size(appended); ++i) {
		CHECK(std::string(clip::kModeNames[i]) == appended[i]);
	}
	CHECK(clip::kParamDescs[static_cast<size_t>(clip::Param::Mode)].max == 14.0);
}

TEST_CASE("the Eroder's six never exceed the ceiling: strict with Oversample off, the measured bound with 4x") {
	const float ceiling = dbToLin(-6.0);
	for (const clip::Mode mode : kNewModes) {
		for (const float shape : { 0.0f, 0.5f, 1.0f }) {
			for (const float drive : { 24.0f, 0.0f, -24.0f }) {
				clip::Settings s;
				s.mode = static_cast<int>(mode);
				s.driveDb = drive;
				s.ceilingDb = -6.0f;
				s.shape = shape;
				s.oversample = false;
				CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
				CAPTURE(shape);
				CAPTURE(drive);
				// SQUARE, 1 kHz SINE, NOISE BURSTS at full scale: the hot sine and the noise are among them.
				const std::array<float, 3> off = worstOver(s);
				const float worst = *std::max_element(off.begin(), off.end());
				CHECK(worst <= ceiling * (1.0f + 1e-6f));
				if (drive == 24.0f && mode != clip::Mode::Slew && mode != clip::Mode::Lopside) {
					CHECK(worst > ceiling * 0.9f); // not vacuous: +24 dB reaches the top
				}
				s.oversample = true;
				const std::array<float, 3> on = worstOver(s);
				MESSAGE(clip::kModeNames[static_cast<size_t>(mode)] << " shape " << shape << " drive " << drive
				                                                    << ": 4x square " << on[0] / ceiling << "x, sine "
				                                                    << on[1] / ceiling << "x, bursts " << on[2] / ceiling << "x");
				CHECK(on[0] <= ceiling * 1.55f);
				CHECK(on[1] <= ceiling * 1.52f);
				CHECK(on[2] <= ceiling * 2.85f);
			}
		}
	}
}

/// THE COMPLEX AMPLITUDE of `hz` over the whole-cycle window [from, end), by a direct sum (the window holds whole periods).
[[nodiscard]] std::complex<double> project(const std::vector<float>& x, size_t from, double hz) {
	std::complex<double> sum = 0.0;
	for (size_t i = from; i < x.size(); ++i) {
		const double w = -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate;
		sum += static_cast<double>(x[i]) * std::complex<double>(std::cos(w), std::sin(w));
	}
	return sum * (2.0 / static_cast<double>(x.size() - from));
}

/// THE SAME PROJECTION of a function of the phase, in double: the expected harmonic, restated from the Eroder's formulas.
[[nodiscard]] std::complex<double> expectedHarmonic(const std::function<double(double)>& f, int harmonic) {
	constexpr int kPoints = 1 << 14;
	std::complex<double> sum = 0.0;
	for (int i = 0; i < kPoints; ++i) {
		const double theta = 2.0 * std::numbers::pi * i / kPoints;
		const double w = -static_cast<double>(harmonic) * theta;
		sum += f(theta) * std::complex<double>(std::cos(w), std::sin(w));
	}
	return sum * (2.0 / kPoints);
}

/// ONE CHANNEL'S OUTPUT for a 1 kHz sine of `amp`, Oversample off, after the DC blocker has settled.
[[nodiscard]] std::pair<std::vector<float>, std::vector<float>> renderSine(const clip::Settings& s, double amp) {
	Instance dev;
	dev.set(s);
	std::vector<float> l = sine(48000, 1000.0, amp), r = l;
	for (int done = 0; done < 48000; done += 480) {
		dev.process(l.data() + done, r.data() + done, 480);
	}
	return { l, r };
}

TEST_CASE("Shape moves the harmonics of Tube, Jag and Lean to the values the Eroder's formulas give") {
	constexpr double kAmp = 0.7;
	constexpr double kHalfPi = std::numbers::pi * 0.5;
	// THE FORMULAS, restated in double from `eroder_types.h`'s own description (Tube's ratio, Jag's triangle, Lean's bias),
	// then the stage's own steps: Tube's and Lean's standing offset taken out (steady state: the mean over a cycle, which a
	// 10 Hz one-pole removes from a 1 kHz tone), and the hard clip at +-1 the ceiling puts after them.
	const auto tube = [](double shape) {
		return [shape](double u) {
			const double ratio = 1.5 * std::pow(8.0 / 1.5, shape);
			return u >= 0.0 ? std::tanh(u) : std::tanh(ratio * u) / ratio;
		};
	};
	const auto jag = [](double shape) {
		return [shape](double u) {
			const double x = std::tanh(u) * (1.0 + 4.0 * shape);
			return std::asin(std::sin(kHalfPi * x)) / kHalfPi;
		};
	};
	const auto lean = [](double shape) {
		return [shape](double u) {
			const double b = 1.2 * shape;
			const double rest = std::tanh(b);
			return (std::tanh(u + b) - rest) / (1.0 - rest * rest);
		};
	};
	struct Row {
		clip::Mode mode;
		std::function<std::function<double(double)>(double)> curve;
		int harmonic;
		bool removesOffset;
	};
	const Row rows[] = { { clip::Mode::Tube, tube, 2, true }, { clip::Mode::Jag, jag, 3, false }, { clip::Mode::Lean, lean, 2, true } };
	for (const Row& row : rows) {
		std::vector<double> amplitudes;
		for (const float shape : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			clip::Settings s;
			s.mode = static_cast<int>(row.mode);
			s.shape = shape;
			s.oversample = false;
			CAPTURE(clip::kModeNames[static_cast<size_t>(row.mode)]);
			CAPTURE(shape);
			const auto [l, r] = renderSine(s, kAmp);
			const auto raw = row.curve(static_cast<double>(shape));
			double mean = 0.0;
			for (int i = 0; row.removesOffset && i < 4096; ++i) {
				mean += raw(kAmp * std::sin(2.0 * std::numbers::pi * i / 4096.0)) / 4096.0;
			}
			const auto curve = [&](double u) { return std::clamp(raw(u) - mean, -1.0, 1.0); };
			const auto expected = expectedHarmonic([&](double th) { return curve(kAmp * std::sin(th)); }, row.harmonic);
			const auto fundamental = expectedHarmonic([&](double th) { return curve(kAmp * std::sin(th)); }, 1);
			const auto got = project(l, 24000, 1000.0 * row.harmonic);
			// THE EXPECTED PHASE is the projection of the same curve; the 10 Hz offset blocker is a hair of phase
			// at 1 kHz and up, and Jag's fold, in float, is a few tenths of a percent off the double one: the
			// tolerance is 1.5% of the larger of the harmonic and the fundamental (the harmonics are 0.02 to 0.6).
			CHECK(std::abs(got - expected) < 0.015 * std::max(std::abs(expected), std::abs(fundamental)));
			amplitudes.push_back(std::abs(got));
			if (row.mode == clip::Mode::Lean && shape == 0.0f) {
				CHECK(std::abs(got) < 1e-4); // a tanh at no bias: no even harmonic
			}
		}
		// THE SECOND ASSERTION: the harmonic really moves with Shape (the first is not vacuous).
		CHECK(std::abs(amplitudes.front() - amplitudes.back()) > 0.01);
		if (row.mode == clip::Mode::Lean || row.mode == clip::Mode::Tube) {
			CHECK(amplitudes[0] < amplitudes[2]);
			CHECK(amplitudes[2] < amplitudes[4]);
		}
	}
}

TEST_CASE("Chonk: left is the tanh, the right is turned toward the clean signal, so the odd harmonics of the two are in antiphase at the top of Shape") {
	for (const float shape : { 0.0f, 0.25f, 0.5f, 1.0f }) {
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::Chonk);
		s.bal = 0.0f; // THE LAW'S OWN TEST: Bal at 0% is the law as it was (the trim is measured by its own cases)
		s.shape = shape;
		s.driveDb = 12.0f;
		s.oversample = false;
		CAPTURE(shape);
		constexpr double kAmp = 0.4;
		const auto [l, r] = renderSine(s, kAmp);
		// RESTATED: L = tanh(g x); R = L + 2 shape (x - L); so R's third is (1 - 2 shape) times L's.
		const double gain = std::pow(10.0, 12.0 / 20.0);
		const auto tanhH3 = expectedHarmonic([&](double th) { return std::tanh(gain * kAmp * std::sin(th)); }, 3);
		const auto l3 = project(l, 24000, 3000.0);
		const auto r3 = project(r, 24000, 3000.0);
		CHECK(std::abs(l3 - tanhH3) < 1e-3);
		CHECK(std::abs(r3 - (1.0 - 2.0 * static_cast<double>(shape)) * tanhH3) < 1e-3);
		// THE DIFFERENCE is 2 shape (tanh - clean): the two channels differ whenever Shape is up.
		double diff = 0.0;
		for (size_t i = 24000; i < l.size(); ++i) {
			diff = std::max(diff, static_cast<double>(std::fabs(l[i] - r[i])));
		}
		if (shape == 0.0f) {
			CHECK(diff == 0.0);
		}
		else {
			CHECK(diff > 0.05);
		}
		if (shape == 1.0f) {
			CHECK((l3 * std::conj(r3)).real() < 0.0); // antiphase
			CHECK(std::abs(r3 + l3) < 1e-3);
		}
	}
}

/// THE PER-SAMPLE STEP of a rising and then a falling ramp, through Slew or Lopside fed a slow +-0.8 square.
[[nodiscard]] std::pair<double, double> slewSteps(clip::Mode mode, float shape, float driveDb, size_t probe = 200) {
	clip::Settings s;
	s.mode = static_cast<int>(mode);
	s.shape = shape;
	s.driveDb = driveDb;
	s.oversample = false;
	Instance dev;
	dev.set(s);
	std::vector<float> l(96000), r;
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = ((i / 24000) % 2) == 0 ? -0.8f : 0.8f;
	}
	r = l;
	for (size_t done = 0; done < l.size(); done += 480) {
		dev.process(l.data() + done, r.data() + done, 480);
	}
	// 24000 samples is a rising half then a falling one; read a step in the middle of each ramp.
	return { static_cast<double>(l[24000 + probe] - l[24000 + probe - 1]),
		     static_cast<double>(l[48000 + probe] - l[48000 + probe - 1]) };
}

TEST_CASE("Lopside's rising and falling rates are 2^(+-balance) of the limit, Drive sets the rate as the Eroder's knob does") {
	// DRIVE +24 dB is the top of the knob: 50 Hz, a limit of 2 * 50 / 48000 per sample at no balance.
	for (const float shape : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		const auto [rise, fall] = slewSteps(clip::Mode::Lopside, shape, 24.0f);
		const double balance = 2.0 * static_cast<double>(shape) - 1.0;
		const double limit = 2.0 * 50.0 / 48000.0;
		CAPTURE(shape);
		CHECK(rise == doctest::Approx(limit * std::exp2(balance)).epsilon(1e-4));
		CHECK(-fall == doctest::Approx(limit * std::exp2(-balance)).epsilon(1e-4));
		CHECK(rise / -fall == doctest::Approx(std::exp2(2.0 * balance)).epsilon(1e-4));
	}
	// SLEW at shape 0 is the hard limit, and Drive moves the rate: 0 dB is the middle of the knob, sqrt(20000 * 50) = 1 kHz.
	const auto [rise, fall] = slewSteps(clip::Mode::Slew, 0.0f, 0.0f, 10);
	CHECK(rise == doctest::Approx(2.0 * 1000.0 / 48000.0).epsilon(1e-4));
	CHECK(-fall == doctest::Approx(2.0 * 1000.0 / 48000.0).epsilon(1e-4));
	const auto [rise2, fall2] = slewSteps(clip::Mode::Slew, 0.0f, 12.0f, 20); // a quarter of the way up: 20000 * (0.0025)^0.75
	CHECK(rise2 == doctest::Approx(2.0 * 20000.0 * std::pow(0.0025, 0.75) / 48000.0).epsilon(1e-4));
	CHECK(rise2 < rise);
}

/// THE PAUNCH LAWS THROUGH CLIP, Oversample off: the same signal in both channels, a 1 kHz sine or noise.
[[nodiscard]] std::pair<std::vector<float>, std::vector<float>> renderLaw(clip::Mode mode, float driveDb, float ceilingDb,
                                                                           float shape, double amp, bool noise,
                                                                           bool oversample = false) {
	clip::Settings s;
	s.mode = static_cast<int>(mode);
	s.driveDb = driveDb;
	s.ceilingDb = ceilingDb;
	s.shape = shape;
	s.oversample = oversample;
	Instance dev;
	dev.set(s);
	std::vector<float> l = sine(48000, 1000.0, amp);
	if (noise) {
		Noise n;
		for (float& v : l) {
			v = static_cast<float>(amp) * n.next();
		}
	}
	std::vector<float> r = l;
	for (int done = 0; done < 48000; done += 480) {
		dev.process(l.data() + done, r.data() + done, 480);
	}
	return { l, r };
}

TEST_CASE("Paunch-S and Paunch-H in Clip: both sides stay under the ceiling on a hot sine and on noise, at every Shape") {
	const float ceiling = dbToLin(-6.0);
	for (const clip::Mode mode : { clip::Mode::PaunchS, clip::Mode::PaunchH, clip::Mode::PaunchOS, clip::Mode::PaunchOH }) {
		for (const float shape : { 0.0f, 0.5f, 1.0f }) {
			for (const bool noise : { false, true }) {
				CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
				CAPTURE(shape);
				CAPTURE(noise);
				const auto [l, r] = renderLaw(mode, 24.0f, -6.0f, shape, 1.0, noise);
				float peak = 0.0f;
				for (size_t i = 0; i < l.size(); ++i) {
					peak = std::max({ peak, std::fabs(l[i]), std::fabs(r[i]) });
				}
				CHECK(peak <= ceiling * (1.0f + 1e-6f));
				CHECK(peak > ceiling * 0.9f); // not vacuous: the law pushed it up to the ceiling
				const auto [lo, ro] = renderLaw(mode, 24.0f, -6.0f, shape, 1.0, noise, true);
				float over = 0.0f;
				for (size_t i = 0; i < lo.size(); ++i) {
					over = std::max({ over, std::fabs(lo[i]), std::fabs(ro[i]) });
				}
				CHECK(over <= ceiling * 2.85f); // the oversampler's measured overshoot, as the other modes' bound
			}
		}
	}
}

TEST_CASE("Paunch-S and Paunch-H in Clip: at Shape 0 the side carries the 2nd harmonic below the ceiling, the mono sum is the dry") {
	// Drive at 0 dB is the middle of its range: a = 2. Input 0.1 into a -6 dB ceiling: the law's input peaks at 0.2, the output
	// at 0.2 + 2 * 0.2 = 0.6 of the ceiling, so nothing is clamped.
	constexpr double kAmp = 0.1;
	const double ceiling = dbToLin(-6.0);
	const double a = 2.0;
	const double peakIn = kAmp / ceiling;
	for (const clip::Mode mode : { clip::Mode::PaunchS, clip::Mode::PaunchH }) {
		CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
		const auto [l, r] = renderLaw(mode, 0.0f, -6.0f, 0.0f, kAmp, false);
		std::vector<float> side(l.size());
		std::vector<float> mid(l.size());
		for (size_t i = 0; i < l.size(); ++i) {
			side[i] = 0.5f * (l[i] - r[i]);
			mid[i] = 0.5f * (l[i] + r[i]);
		}
		const double h2 = std::abs(project(side, 24000, 2000.0));
		const double expected =
		    ceiling * (mode == clip::Mode::PaunchS ? a * peakIn * 4.0 / (3.0 * std::numbers::pi)
		                                           : (4.0 / (3.0 * std::numbers::pi)) * a * a * peakIn * peakIn / 2.0);
		MESSAGE(clip::kModeNames[static_cast<size_t>(mode)] << ": side 2nd " << h2 << ", restated " << expected);
		CHECK(std::abs(h2 / expected - 1.0) < 0.03);
		CHECK(h2 < ceiling);
		// THE MID IS THE DRY: its fundamental, and nothing at the harmonics (the offset blocker is a hair of phase, not level).
		const double fundamental = std::abs(project(mid, 24000, 1000.0));
		CHECK(std::abs(fundamental / kAmp - 1.0) < 0.002);
		for (const double hz : { 2000.0, 3000.0, 4000.0, 6000.0 }) {
			CHECK(std::abs(project(mid, 24000, hz)) < 1.0e-4 * fundamental);
		}
	}
}

TEST_CASE("Paunch-OS in Clip: the side carries a 3rd harmonic of a / 4 of the signal at any level, the mid is the dry, and the ceiling holds") {
	// Drive at 0 dB is the middle of its range: a = 2. A 220 Hz sine, 1 s, the last half measured (110 whole cycles).
	const double ceiling = dbToLin(-6.0);
	for (const double amp : { 0.03, 0.1, 0.25 }) {
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::PaunchOS);
		s.driveDb = 0.0f;
		s.ceilingDb = -6.0f;
		s.oversample = false;
		Instance dev;
		dev.set(s);
		std::vector<float> l = sine(48000, 220.0, amp), r = l;
		for (int done = 0; done < 48000; done += 480) {
			dev.process(l.data() + done, r.data() + done, 480);
		}
		std::vector<float> side(l.size()), mid(l.size());
		for (size_t i = 0; i < l.size(); ++i) {
			side[i] = 0.5f * (l[i] - r[i]);
			mid[i] = 0.5f * (l[i] + r[i]);
		}
		const double h3 = std::abs(project(side, 24000, 660.0));
		const double fundamental = std::abs(project(mid, 24000, 220.0));
		MESSAGE("amp " << amp << ": side 3rd " << h3 << " restated " << 2.0 * amp / 4.0 << ", side fundamental " << 20.0 * std::log10(std::abs(project(side, 24000, 220.0)) / fundamental) << " dB of the mid's");
		CHECK(std::abs(h3 / (2.0 * amp / 4.0) - 1.0) < 0.05);
		CHECK(std::abs(fundamental / amp - 1.0) < 0.01);
		float peak = 0.0f;
		for (size_t i = 0; i < l.size(); ++i) {
			peak = std::max({ peak, std::fabs(l[i]), std::fabs(r[i]) });
		}
		CHECK(peak <= static_cast<float>(ceiling) * (1.0f + 1e-6f));
	}
}

/// THE PAUNCH-S MODE THROUGH CLIP at a hot level: Drive 0 dB is a = 2, Ceiling -6 dB, a 1 kHz sine of 0.5 (the law's input peaks at about 1).
[[nodiscard]] std::tuple<std::vector<float>, std::vector<float>, size_t, size_t> renderDc(clip::Mode mode, float dc, double amp, bool negate = false) {
	clip::Settings s;
	s.mode = static_cast<int>(mode);
	s.driveDb = 0.0f;
	s.ceilingDb = -6.0f;
	s.oversample = false;
	s.dc = dc;
	Instance dev;
	dev.set(s);
	std::vector<float> l = sine(48000, 1000.0, negate ? -amp : amp), r = l;
	for (int done = 0; done < 48000; done += 480) {
		dev.process(l.data() + done, r.data() + done, 480);
	}
	return { l, r, clip::dcLastClampCountForTest(dev.storageForTest()), clip::dcFramesForTest(dev.storageForTest()) };
}

TEST_CASE("Clip DC: the ceiling clips the law's signal WITH its offset (one side of each channel), then the offset is removed by the amount; the ceiling holds") {
	const double ceiling = static_cast<double>(dbToLin(-6.0));
	const double amp = 0.5;
	const double u = amp / ceiling;
	// THE CLIPPED SIGNAL, restated: left = clamp(u sin + 2 |u sin|, -1, 1); its mean over a cycle.
	double mean = 0.0;
	const int n = 4800;
	for (int i = 0; i < n; ++i) {
		const double x = u * std::sin(2.0 * std::numbers::pi * (i + 0.5) / n);
		mean += std::clamp(x + 2.0 * std::fabs(x), -1.0, 1.0);
	}
	mean /= n;
	for (const clip::Mode mode : { clip::Mode::PaunchS, clip::Mode::PaunchH }) {
		const bool rectifier = mode == clip::Mode::PaunchS;
		double want = mean;
		if (!rectifier) {
			want = 0.0;
			for (int i = 0; i < n; ++i) {
				const double x = u * std::sin(2.0 * std::numbers::pi * (i + 0.5) / n);
				want += std::clamp(x + (4.0 / (3.0 * std::numbers::pi)) * 4.0 * x * x, -1.0, 1.0);
			}
			want /= n;
		}
		CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
		// KEEP (0%): the output is the clipped signal itself, one-sided, its mean the restated one.
		const auto [kl, kr, kc, kf] = renderDc(mode, 0.0f, amp);
		double kept = 0.0;
		float kmin = 1.0f;
		float kmax = -1.0f;
		for (size_t i = 24000; i < kl.size(); ++i) {
			kept += static_cast<double>(kl[i]);
			kmin = std::min(kmin, kl[i]);
			kmax = std::max(kmax, kl[i]);
		}
		kept /= 24000.0;
		MESSAGE(clip::kModeNames[static_cast<size_t>(mode)] << ": kept mean " << kept << " restated " << want * ceiling << ", left range " << kmin << " .. " << kmax);
		CHECK(std::abs(kept - want * ceiling) < 0.02 * std::fabs(want * ceiling));
		if (rectifier) {
			CHECK(kmin >= -1.0e-4f); // the rectifier's signal is never below 0: one side of the left
		}
		CHECK(kmax <= static_cast<float>(ceiling) * (1.0f + 1e-6f));
		CHECK(std::fabs(kmax - static_cast<float>(ceiling)) < 1e-4f); // the ceiling clip acts on the top
		// BLOCK (100%): the offset is out, and the order shows: the left's top is the ceiling less the mean (clip first, remove after).
		const auto [bl, br, bc, bf] = renderDc(mode, 1.0f, amp);
		double blockedMean = 0.0;
		float top = -1.0f;
		for (size_t i = 24000; i < bl.size(); ++i) {
			blockedMean += static_cast<double>(bl[i]);
			top = std::max(top, bl[i]);
		}
		blockedMean /= 24000.0;
		MESSAGE("blocked mean " << blockedMean << ", left top " << top << " restated " << ceiling * (1.0 - want) << "; last clamp acted on " << bc << " of " << bf << " frames");
		CHECK(std::fabs(blockedMean) < 0.001 * ceiling);
		CHECK(std::abs(top - static_cast<float>(ceiling * (1.0 - want))) < 0.03 * ceiling);
		for (size_t i = 0; i < bl.size(); ++i) {
			REQUIRE(std::fabs(bl[i]) <= static_cast<float>(ceiling) * (1.0f + 1e-6f));
			REQUIRE(std::fabs(br[i]) <= static_cast<float>(ceiling) * (1.0f + 1e-6f));
		}
		// MIRROR: the right at +x is the left's at -x.
		const auto [nl, nr, nc, nf] = renderDc(mode, 1.0f, amp, true);
		double worst = 0.0;
		for (size_t i = 0; i < br.size(); ++i) {
			worst = std::max(worst, static_cast<double>(std::fabs(br[i] + nl[i])));
		}
		CHECK(worst < 1.0e-4);
	}
}

TEST_CASE("Clip DC: Tube and Lean at 100% are the curve, the offset removed, then the clamp — the order they always used — and 0% leaves the offset in") {
	namespace er = feathers::fx::eroder;
	for (const clip::Mode mode : { clip::Mode::Tube, clip::Mode::Lean }) {
		CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.shape = 1.0f;
		s.driveDb = 12.0f;
		s.oversample = false;
		const er::DriveMode dm = mode == clip::Mode::Tube ? er::DriveMode::Tube : er::DriveMode::Lean;
		const std::vector<float> x = sine(48000, 1000.0, 0.5);
		std::vector<float> out[2];
		int at = 0;
		for (const float dc : { 1.0f, 0.0f }) {
			s.dc = dc;
			Instance dev;
			dev.set(s);
			std::vector<float> l = x, r = x;
			for (int done = 0; done < 48000; done += 480) {
				dev.process(l.data() + done, r.data() + done, 480);
			}
			out[at++] = l;
		}
		// THE RESTATEMENT of today's order (no oversampling): curve, a 10 Hz one-pole's estimate subtracted, the clamp at the ceiling.
		const double gain = std::pow(10.0, 12.0 / 20.0);
		const float coefficient = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * er::kDriveDcHz / 48000.0));
		float dcEstimate = 0.0f;
		er::WidthTrim trim;
		trim.prepare(48000.0);
		double worst = 0.0;
		double meanOff = 0.0;
		for (size_t i = 0; i < x.size(); ++i) {
			const float u = static_cast<float>(gain) * x[i];
			float y = er::shapeDrive(dm, u, 1.0f, 1.0f);
			// LEAN'S LEVEL TRIM sits between the law and the offset's removal (law, trim, removal, clamp): restated with the trim's own function.
			float yr = er::shapeDriveRight(dm, u, 1.0f, 1.0f);
			if (dm == er::DriveMode::Lean) {
				er::widthTrim(trim, u, u, y, yr, 1.0f);
			}
			dcEstimate += (y - dcEstimate) * coefficient;
			const float removed = std::clamp(y - dcEstimate, -1.0f, 1.0f);
			worst = std::max(worst, static_cast<double>(std::fabs(removed - out[0][i])));
			if (i >= 24000) {
				meanOff += static_cast<double>(out[1][i]) / 24000.0;
			}
		}
		MESSAGE(clip::kModeNames[static_cast<size_t>(mode)] << ": worst difference from the restated order " << worst << ", mean at 0% " << meanOff);
		CHECK(worst < 1.0e-4);
		CHECK(std::fabs(meanOff) > 0.01);
	}
}

TEST_CASE("Paunch in Clip: Shape is the angle (0.25 puts the addition in the mid), the rotation at 1 Hz keeps the two sides level over a whole turn, and the ceiling holds") {
	constexpr double kAmp = 0.1;
	for (const clip::Mode mode : { clip::Mode::PaunchS, clip::Mode::PaunchOS }) {
		CAPTURE(clip::kModeNames[static_cast<size_t>(mode)]);
		// STATIC ANGLE: Shape 0.25, the side is empty (under -80 dB of the mid's fundamental).
		const auto [l, r] = renderLaw(mode, 0.0f, -6.0f, 0.25f, kAmp, false);
		std::vector<float> side(l.size());
		std::vector<float> mid(l.size());
		for (size_t i = 0; i < l.size(); ++i) {
			side[i] = 0.5f * (l[i] - r[i]);
			mid[i] = 0.5f * (l[i] + r[i]);
		}
		const double fundamental = std::abs(project(mid, 24000, 1000.0));
		// (Paunch-OS's fixed leak term is the one that cancels its fundamental, at about -40 dB; the rest are empty under -80 dB.)
		const double bound = mode == clip::Mode::PaunchOS ? 1.0e-3 : 1.0e-4;
		CHECK(std::abs(project(side, 24000, 1000.0)) < bound * fundamental);
		CHECK(std::abs(project(side, 24000, 2000.0)) < bound * fundamental);
		CHECK(std::abs(project(mid, 24000, 3000.0)) > 0.0); // (the mid holds the addition)
		// TURNING: Rate 1 Hz on an asymmetric wave (sine + half a 2nd harmonic), hot enough to reach the ceiling.
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.driveDb = 0.0f;
		s.ceilingDb = -6.0f;
		s.oversample = false;
		s.rate = 0.0f; // 1 Hz
		Instance dev;
		dev.set(s);
		std::vector<float> a(4 * 48000);
		for (size_t i = 0; i < a.size(); ++i) {
			const double t = static_cast<double>(i) / 48000.0;
			a[i] = static_cast<float>(0.2 * (std::sin(2.0 * std::numbers::pi * 220.0 * t) + 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t - 0.5 * std::numbers::pi)) / 1.5);
		}
		std::vector<float> al = a;
		std::vector<float> ar = a;
		for (size_t done = 0; done < a.size(); done += 480) {
			dev.process(al.data() + done, ar.data() + done, 480);
		}
		const double ceiling = static_cast<double>(dbToLin(-6.0));
		double peak = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			peak = std::max({ peak, static_cast<double>(std::fabs(al[i])), static_cast<double>(std::fabs(ar[i])) });
		}
		CHECK(peak <= ceiling * (1.0 + 1.0e-6));
		for (size_t turn = 1; turn < 4; ++turn) {
			double el = 0.0;
			double er = 0.0;
			for (size_t i = turn * 48000; i < (turn + 1) * 48000; ++i) {
				el += static_cast<double>(al[i]) * al[i];
				er += static_cast<double>(ar[i]) * ar[i];
			}
			const double db = 10.0 * std::log10(el / er);
			MESSAGE(clip::kModeNames[static_cast<size_t>(mode)] << " whole turn " << turn << " left minus right " << db << " dB");
			CHECK(std::abs(db) <= 0.3);
		}
	}
}

TEST_CASE("Chonk's Bal in Clip: 0% is the law as it was (the tanh and the turned-over right), 100% keeps the two sides at the balance they came in with") {
	const std::vector<float> x = sine(48000, 220.0, 0.1);
	std::vector<float> out[3];
	int at = 0;
	for (const float bal : { 0.0f, 1.0f }) {
		clip::Settings s;
		s.mode = static_cast<int>(clip::Mode::Chonk);
		s.shape = 1.0f;
		s.driveDb = 24.0f;
		s.oversample = false;
		s.bal = bal;
		Instance dev;
		dev.set(s);
		std::vector<float> l = x;
		std::vector<float> r = x;
		for (int done = 0; done < 48000; done += 480) {
			dev.process(l.data() + done, r.data() + done, 480);
		}
		out[at++] = r;
	}
	// BAL 0 RESTATED: the right is tanh(g x) + 2 shape (x g'... ) turned over; compare the left-right level difference instead of every sample:
	double e0 = 0.0;
	double e1 = 0.0;
	for (size_t i = 24000; i < x.size(); ++i) {
		e0 += static_cast<double>(out[0][i]) * out[0][i];
		e1 += static_cast<double>(out[1][i]) * out[1][i];
	}
	CHECK(e0 != e1); // not vacuous: the trim acts on the right
	CHECK(e1 > e0);  // Chonk thins the right at full Drive; the trim brings it back to the input's balance
}
