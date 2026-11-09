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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
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
	CHECK(clip::stateOf(clip::Row {}, 0) == clip::Settings {});
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
	for (const clip::Mode mode : kModes) {
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
	for (const clip::Mode mode : kModes) {
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
				offTrack += std::fabs(l - tl) > 1e-3f;
			}
		}
		CHECK(nonFinite == 0u);
		CHECK(offTrack == 0u);
	}
}

TEST_CASE("partition does not matter — every mode, oversampled, through a disable and re-enable") {
	for (const clip::Mode mode : kModes) {
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
	for (const clip::Mode mode : kModes) {
		CAPTURE(static_cast<int>(mode));
		clip::Settings s;
		s.mode = static_cast<int>(mode);
		s.driveDb = 24.0f;
		Instance dev;
		dev.set(s);
		auto l = sine(512, 440.0, 0.8), r = l;
		dev.process(l.data(), r.data(), 512);
		REQUIRE(std::fabs(l[500]) > 0.1f); // not vacuous: the burst reached the output
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
		CHECK(lastNonZero < 2880);
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
