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

#include "fx/busutil/busutil_types.h"
#include "fx/clip/clip_types.h"
#include "fx/comp/comp_lanes.h"
#include "fx/comp/comp_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace comp = feathers::fx::comp;
namespace fx = feathers::fx;

namespace {

//------------------------------------------------------------------------------------------------//
// THE STATIC CURVE AND THE ROW (Task 4)                                                          //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the static curve is the formula") {
	// T = -20, R = 4, W = 10. The expected values are computed by hand, not by calling the curve.
	constexpr float T = -20.0f, R = 4.0f, W = 10.0f;
	// BELOW the knee: untouched.
	CHECK(std::fabs(comp::staticCurveDb(-40.0f, T, R, W) - (-40.0f)) < 1e-4f);
	// ABOVE: T + (x - T)/R = -20 + 15/4.
	CHECK(std::fabs(comp::staticCurveDb(-5.0f, T, R, W) - (-16.25f)) < 1e-4f);
	// THE KNEE'S CENTRE: x + (1/R - 1) * (W/2)^2 / (2W) = -20 + (0.25 - 1) * 25 / 20.
	CHECK(std::fabs(comp::staticCurveDb(-20.0f, T, R, W) - (-20.9375f)) < 1e-4f);
}

TEST_CASE("the knee is continuous at both edges") {
	constexpr float T = -20.0f, R = 4.0f, W = 10.0f;
	for (const float edge : { T - W / 2.0f, T + W / 2.0f }) {
		const float at = comp::staticCurveDb(edge, T, R, W);
		for (const float d : { -1e-3f, 1e-3f }) {
			CAPTURE(edge);
			CAPTURE(d);
			CHECK(std::fabs(comp::staticCurveDb(edge + d, T, R, W) - at) < 1e-2f);
		}
	}
}

TEST_CASE("knee zero is the hard curve") {
	constexpr float T = -12.0f, R = 3.0f;
	for (float x = -40.0f; x <= 6.0f; x += 0.5f) {
		const float hard = x <= T ? x : T + (x - T) / R;
		CAPTURE(x);
		CHECK(std::fabs(comp::staticCurveDb(x, T, R, 0.0f) - hard) < 1e-5f);
	}
}

TEST_CASE("the top of the Ratio travel is infinite — a brickwall at the threshold") {
	REQUIRE(comp::kParamDescs[static_cast<size_t>(comp::Param::Ratio)].max
	        == static_cast<double>(comp::kRatioInfinite));
	constexpr float T = -24.0f;
	for (float x = -20.0f; x <= 12.0f; x += 2.0f) {
		CAPTURE(x);
		// ABOVE THE KNEE (W = 6, so from T + 3), the output is the threshold, exactly.
		CHECK(std::fabs(comp::staticCurveDb(x, T, comp::kRatioInfinite, 6.0f) - T) < 1e-4f);
	}
	// AND JUST BELOW THE TOP IT IS STILL A FINITE RATIO — the brickwall is the top, not a region.
	CHECK(std::fabs(comp::staticCurveDb(0.0f, T, 19.0f, 0.0f) - (T + 24.0f / 19.0f)) < 1e-4f);
}

TEST_CASE("a row round-trips and a short row takes the defaults") {
	comp::Settings s;
	s.enabled = false;
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.thresholdDb = -31.5f;
	s.ratio = 7.25f;
	s.kneeDb = 11.0f;
	s.attack = -9.0f;
	s.release = -1.5f;
	s.detect = static_cast<int>(comp::Detect::Rms);
	s.scHpf = 6;
	s.makeupDb = 4.5f;
	s.mix = 0.35f;
	s.driveDb = 9.0f;
	s.ceilingDb = -3.0f;
	s.stereoLink = 0.3f;
	s.stereoMode = static_cast<int>(comp::StereoMode::MidSide);
	const comp::Row row = comp::rowOf(s);
	CHECK(comp::stateOf(row, row.size()) == s);

	// A SHORT ROW: only Enabled and Mode present. Everything else must be the DECLARED default — a
	// Ratio read as zero would divide by it.
	const comp::Settings partial = comp::stateOf(row, 2);
	CHECK(partial.enabled == false);
	CHECK(partial.mode == static_cast<int>(comp::Mode::Limit));
	comp::Settings expected;
	expected.enabled = false;
	expected.mode = static_cast<int>(comp::Mode::Limit);
	CHECK(partial == expected);
	CHECK(partial.ratio == 4.0f);
	CHECK(partial.thresholdDb == -18.0f);

	// AN EMPTY ROW IS THE DEFAULTS.
	CHECK(comp::stateOf(comp::Row {}, 0) == comp::Settings {});
}

//------------------------------------------------------------------------------------------------//
// THE DSP (Task 5)                                                                               //
//------------------------------------------------------------------------------------------------//

constexpr double kSampleRate = 48000.0;
constexpr int kSecond = 48000;

/// AN OPS INSTANCE, CONSTRUCTED THE WAY A HOST CONSTRUCTS ONE — into storage the test owns
/// (`test_fx_busutil.cpp`'s `Instance`), with the readings captured through the host's sink road.
class Instance {
public:
	Instance() {
		REQUIRE(comp::kAudioOps.stateBytes <= storage_.size());
		REQUIRE(comp::kAudioOps.stateAlign <= alignof(std::max_align_t));
		comp::kAudioOps.construct(storage_.data());
		comp::kAudioOps.prepare(storage_.data(), kSampleRate, 4096);
		comp::kAudioOps.setReadingsSink(
		    storage_.data(), feathers::device::ReadingsSink {
		                         this, [](void* context, const float* values, size_t count) {
			                         auto* self = static_cast<Instance*>(context);
			                         self->readingsCount_ = count;
			                         for (size_t i = 0; i < std::min(count, self->readings_.size()); ++i) {
				                         self->readings_[i] = values[i];
			                         }
			                         ++self->publishes_;
		                         } });
	}
	~Instance() { comp::kAudioOps.destruct(storage_.data()); }

	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const comp::Settings& s) {
		row_ = comp::rowOf(s);
		comp::kAudioOps.setFields(storage_.data(), row_.data(), row_.size());
	}

	void process(float* left, float* right, int frames) {
		comp::kAudioOps.processStereo(storage_.data(), left, right, frames);
	}
	/// THE SAME DC LEVEL ON BOTH CHANNELS FOR `frames` SAMPLES, in blocks of `block`; returns the last
	/// output sample of the left channel.
	float runDc(float amp, int frames, int block = 256) {
		std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
		float last = 0.0f;
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			std::fill(l.begin(), l.end(), amp);
			std::fill(r.begin(), r.end(), amp);
			process(l.data(), r.data(), n);
			last = l[static_cast<size_t>(n - 1)];
		}
		return last;
	}

	[[nodiscard]] float gainDb() const { return comp::gainDbForTest(storage_.data()); }
	[[nodiscard]] float gainDb(size_t channel) const { return comp::gainDbForTest(storage_.data(), channel); }
	[[nodiscard]] float ceilingStageDb() const { return comp::ceilingStageDbForTest(storage_.data()); }
	[[nodiscard]] float peakOverCeiling() const { return comp::peakOverCeilingForTest(storage_.data()); }
	/// DC ON EACH CHANNEL AT ITS OWN LEVEL; returns the last output pair.
	std::pair<float, float> runDc(float ampL, float ampR, int frames, int block = 256) {
		std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
		std::pair<float, float> last {};
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			std::fill(l.begin(), l.end(), ampL);
			std::fill(r.begin(), r.end(), ampR);
			process(l.data(), r.data(), n);
			last = { l[static_cast<size_t>(n - 1)], r[static_cast<size_t>(n - 1)] };
		}
		return last;
	}
	[[nodiscard]] float detectorDb() const { return comp::detectorDbForTest(storage_.data()); }
	[[nodiscard]] float reading(comp::Reading r) const { return readings_[static_cast<size_t>(r)]; }
	[[nodiscard]] size_t readingsCount() const { return readingsCount_; }
	[[nodiscard]] size_t publishes() const { return publishes_; }
	[[nodiscard]] const void* storageForTest() const { return storage_.data(); }
	/// HOW LATE THE OUTPUT IS — the device's own answer, from the row it was last given (backlog LE).
	[[nodiscard]] size_t latency() const {
		return comp::kAudioOps.latencySamples(storage_.data(), row_.data(), row_.size(), kSampleRate);
	}

	/// BACKLOG JO — THE HOST HANDS THE SIDECHAIN, as the bus road does: just before the block, valid for that call.
	void setSidechain(const float* l, const float* r, int frames, bool sounding) {
		comp::kAudioOps.setSidechain(storage_.data(), l, r, frames, sounding);
	}
	/// INPUT DC `ampIn` AND SIDECHAIN DC `ampSide` (null buffers when `connected` is false) for `frames`, in blocks of 256, the
	/// sidechain handed before every block; returns the last output sample of the left channel.
	float runDcSide(float ampIn, float ampSide, int frames, bool connected = true) {
		constexpr int block = 256;
		std::vector<float> l(block), r(block), sl(block, ampSide), sr(block, ampSide);
		float last = 0.0f;
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			std::fill(l.begin(), l.end(), ampIn);
			std::fill(r.begin(), r.end(), ampIn);
			setSidechain(connected ? sl.data() : nullptr, connected ? sr.data() : nullptr, n, connected && ampSide != 0.0f);
			process(l.data(), r.data(), n);
			last = l[static_cast<size_t>(n - 1)];
		}
		return last;
	}
private:
	alignas(std::max_align_t) std::array<std::byte, 32768> storage_ {}; // backlog QG: the lane holder and the chunk scratch
	comp::Row row_ = comp::rowOf(comp::Settings {});
	std::array<float, 8> readings_ {};
	size_t readingsCount_ = 0;
	size_t publishes_ = 0;
};

[[nodiscard]] float dbToLin(double db) {
	return static_cast<float>(std::pow(10.0, db / 20.0));
}
[[nodiscard]] double linToDb(double lin) {
	return 20.0 * std::log10(lin);
}

/// A SEEDED xorshift32 IN [-1, 1] — the test's own, so a library change cannot move its numbers.
struct Noise {
	uint32_t s = 0x9e3779b9u;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	}
};

TEST_CASE("steady-state gain reduction is the static curve") {
	comp::Settings s;
	s.detect = static_cast<int>(comp::Detect::Peak);
	s.attack = static_cast<float>(std::log2(0.0001));
	s.makeupDb = 3.0f; // so the makeup is part of the claim, not assumed to be zero
	for (int level = -40; level <= 0; level += 5) {
		Instance dev;
		dev.set(s);
		const float amp = dbToLin(level);
		const float y = dev.runDc(amp, kSecond);
		const double measured = linToDb(static_cast<double>(y) / static_cast<double>(amp));
		const double expected = static_cast<double>(comp::staticCurveDb(static_cast<float>(level), s.thresholdDb,
		                                                                 s.ratio, s.kneeDb))
		                        - level + static_cast<double>(s.makeupDb);
		CAPTURE(level);
		CAPTURE(measured);
		CAPTURE(expected);
		CHECK(std::fabs(measured - expected) < 0.01);
	}
}

TEST_CASE("at infinite ratio the steady output sits exactly on the threshold") {
	comp::Settings s;
	s.ratio = comp::kRatioInfinite;
	s.thresholdDb = -24.0f;
	s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	for (const double level : { -18.0, -9.0, 0.0 }) {
		Instance dev;
		dev.set(s);
		const float y = dev.runDc(dbToLin(level), kSecond);
		CAPTURE(level);
		CHECK(std::fabs(linToDb(static_cast<double>(y)) - (-24.0)) < 1e-4);
	}
}

TEST_CASE("the bottom of the Release travel is an instant release") {
	comp::Settings s;
	s.thresholdDb = -20.0f;
	s.release = static_cast<float>(std::log2(comp::kReleaseMinSeconds));
	const double releaseMin = comp::kParamDescs[static_cast<size_t>(comp::Param::Release)].min;
	REQUIRE(std::fabs(static_cast<double>(s.release) - releaseMin) < 1e-5);
	Instance dev;
	dev.set(s);
	dev.runDc(1.0f, kSecond / 4);
	REQUIRE(dev.gainDb() < -10.0f);
	// ONE SAMPLE OF SILENCE AND THE REDUCTION IS GONE — "off" on the card, and off in the DSP.
	dev.runDc(0.0f, 1, 1);
	CHECK(dev.gainDb() == 0.0f);
	// NOT THE CONVENTION BY ACCIDENT: one step above the bottom, it takes time.
	s.release = static_cast<float>(std::log2(0.005));
	Instance slow;
	slow.set(s);
	slow.runDc(1.0f, kSecond / 4);
	slow.runDc(0.0f, 1, 1);
	CHECK(slow.gainDb() < -10.0f);
}

/// THE 1-BASED SAMPLE AT WHICH THE GAIN FIRST COVERS 63.2% OF A STEP, read from the OUTPUT of a DC
/// input: at DC, y/x IS the applied gain (makeup 0, mix 1), so no seam is needed.
int crossing(Instance& dev, float amp, double fromDb, double toDb, int limit) {
	// THE FIRST `latency` SAMPLES OUT ENTERED BEFORE THE STEP (backlog LE), so the count starts after them:
	// the time constant is the device's, not its delay's.
	const int late = static_cast<int>(dev.latency());
	for (int k = 1; k <= limit + late; ++k) {
		float l = amp, r = amp;
		dev.process(&l, &r, 1);
		if (k <= late) {
			continue;
		}
		const double g = linToDb(static_cast<double>(l) / static_cast<double>(amp));
		if (std::fabs(g - fromDb) >= 0.632 * std::fabs(toDb - fromDb)) {
			return k - late;
		}
	}
	return -1;
}

TEST_CASE("attack reaches 63% of its step in one time constant") {
	comp::Settings s;
	s.thresholdDb = -20.0f;
	s.ratio = 20.0f;
	s.kneeDb = 0.0f;
	s.attack = static_cast<float>(std::log2(0.010)); // 480 samples
	s.release = static_cast<float>(std::log2(0.100)); // 4800 samples — DIFFERENT, so a swap shows
	Instance dev;
	dev.set(s);
	dev.runDc(dbToLin(-40.0), kSecond / 2);
	REQUIRE(std::fabs(dev.gainDb()) < 1e-6f);
	// 0 dB in, T = -20, R = 20 = ∞: the curve says -20 dB out, a reduction of 20 dB.
	REQUIRE(s.ratio == comp::kRatioInfinite);
	const double finalDb = -20.0;
	const int attack = crossing(dev, 1.0f, 0.0, finalDb, kSecond);
	MESSAGE("attack crossing sample: " << attack);
	CHECK(std::abs(attack - 480) <= 10);
	dev.runDc(1.0f, kSecond);
	REQUIRE(std::fabs(dev.gainDb() - static_cast<float>(finalDb)) < 1e-3f);
	const int release = crossing(dev, dbToLin(-40.0), finalDb, 0.0, 2 * kSecond);
	MESSAGE("release crossing sample: " << release);
	CHECK(std::abs(release - 4800) <= 10);
}

TEST_CASE("Limit never lets a sample past the ceiling") {
	for (const int hpf : { 0, 8 }) {
		comp::Settings s;
		s.mode = static_cast<int>(comp::Mode::Limit);
		s.driveDb = 24.0f;
		s.ceilingDb = -6.0f;
		s.scHpf = hpf;
		CAPTURE(hpf);
		REQUIRE((hpf == 0 || comp::scHpfHz(hpf) == 500.0f));
		Instance dev;
		dev.set(s);
		const float bound = dbToLin(-6.0) * (1.0f + 1e-6f);
		Noise noise;
		float worst = 0.0f;
		size_t checked = 0;
		for (int signal = 0; signal < 3; ++signal) {
			CAPTURE(signal);
			std::vector<float> l(240), r(240);
			for (int done = 0; done < kSecond; done += 240) {
				for (int i = 0; i < 240; ++i) {
					const double t = static_cast<double>(done + i) / kSampleRate;
					float v = 0.0f;
					if (signal == 0) {
						v = std::fmod(t * 50.0, 1.0) < 0.5 ? 1.0f : -1.0f; // full-scale square, 50 Hz
					}
					else if (signal == 1) {
						v = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * t));
					}
					else {
						// BURSTS: 5 ms of full-scale random samples, 20 ms of silence.
						v = ((done + i) % 1200) < 240 ? noise.next() : 0.0f;
					}
					l[static_cast<size_t>(i)] = v;
					r[static_cast<size_t>(i)] = signal == 2 ? noise.next() * (v != 0.0f ? 1.0f : 0.0f) : -v;
				}
				dev.process(l.data(), r.data(), 240);
				for (int i = 0; i < 240; ++i) {
					worst = std::max({ worst, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]) });
					++checked;
				}
			}
		}
		MESSAGE("worst |y| " << worst << " against the bound " << bound);
		CHECK(checked == 3u * static_cast<size_t>(kSecond));
		CHECK(worst <= bound);
		// NOT VACUOUS: +24 dB of drive puts the input far over; the output must sit AT the ceiling.
		CHECK(worst > dbToLin(-6.0) * 0.99f);
	}
}

TEST_CASE("Mix at zero is the dry signal to the bit") {
	comp::Settings s;
	s.mix = 0.0f; // set before the first process, so the ramp snaps to it
	s.thresholdDb = -60.0f;
	s.ratio = 20.0f;
	s.makeupDb = 12.0f;
	Instance dev;
	dev.set(s);
	const size_t late = dev.latency();
	REQUIRE(late > 0u);
	Noise noise;
	size_t differ = 0;
	size_t compared = 0;
	// THE DRY, `latency` SAMPLES LATE (backlog LE): every input kept, and output `t` compared with input
	// `t - latency`; the first `latency` out are the silence the device started with.
	std::vector<float> inL, inR;
	std::vector<float> l(97), r(97);
	for (int done = 0; done < kSecond / 2; done += 97) {
		for (size_t i = 0; i < 97; ++i) {
			l[i] = noise.next();
			r[i] = noise.next();
			inL.push_back(l[i]);
			inR.push_back(r[i]);
		}
		dev.process(l.data(), r.data(), 97);
		for (size_t i = 0; i < 97; ++i) {
			const size_t t = static_cast<size_t>(done) + i;
			const float wantL = t >= late ? inL[t - late] : 0.0f;
			const float wantR = t >= late ? inR[t - late] : 0.0f;
			differ += (l[i] != wantL) + (r[i] != wantR);
			++compared;
		}
	}
	CHECK(differ == 0u);
	CHECK(compared == static_cast<size_t>(kSecond / 2 / 97 + 1) * 97u);
	// NOT VACUOUS: the detector was working hard the whole time.
	CHECK(dev.gainDb() < -20.0f);
}

TEST_CASE("Mix at one is the wet signal to the bit") {
	// `(1-m)*x + m*(x*g)` at m == 1 is 0*x + x*g, which is x*g EXACTLY. The lerp form `x + m*(x*g - x)`
	// rounds `x + (x*g - x)` and misses on some samples — this case is what tells the two apart.
	comp::Settings s;
	s.thresholdDb = -40.0f;
	s.ratio = 6.0f;
	s.makeupDb = 5.0f;
	Instance dev;
	dev.set(s);
	const size_t late = dev.latency();
	Noise noise;
	size_t differ = 0;
	// EACH SAMPLE'S `x * g` AS IT ENTERED, compared with what leaves `latency` samples later (backlog LE).
	std::vector<std::pair<float, float>> want;
	for (int k = 0; k < kSecond / 4; ++k) {
		const float xL = noise.next(), xR = noise.next();
		float l = xL, r = xR;
		dev.process(&l, &r, 1);
		const float g = comp::lastGainForTest(dev.storageForTest());
		want.emplace_back(xL * g, xR * g);
		const size_t t = static_cast<size_t>(k);
		const auto [wl, wr] = t >= late ? want[t - late] : std::pair { 0.0f, 0.0f };
		differ += (l != wl) + (r != wr);
	}
	CHECK(differ == 0u);
	CHECK(dev.gainDb() < -10.0f);
}

/// THE DEEPEST REDUCTION OVER THE LAST QUARTER SECOND OF A SECOND OF 40 Hz AT -6 dB.
float bassReduction(int scHpf) {
	comp::Settings s;
	s.thresholdDb = -20.0f;
	s.ratio = 20.0f;
	s.kneeDb = 0.0f;
	// INSTANT ATTACK, so the deepest reduction IS the curve at the sine's peak — a 10 ms attack only
	// part-reaches it inside a 25 ms cycle (measured 12.06 dB against 13.3 at a literal 20:1), and the
	// claim should be a value.
	s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	s.scHpf = scHpf;
	Instance dev;
	dev.set(s);
	const float amp = dbToLin(-6.0);
	float deepest = 0.0f;
	for (int k = 0; k < kSecond; k += 16) {
		std::array<float, 16> l {}, r {};
		for (int i = 0; i < 16; ++i) {
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] =
			    amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 40.0 * (k + i) / kSampleRate));
		}
		dev.process(l.data(), r.data(), 16);
		if (k >= kSecond * 3 / 4) {
			deepest = std::max(deepest, -dev.gainDb());
		}
	}
	return deepest;
}

TEST_CASE("the sidechain high-pass spares the bass") {
	REQUIRE(comp::scHpfHz(6) == 200.0f);
	const float off = bassReduction(0);
	const float hpf = bassReduction(6);
	MESSAGE("GR off " << off << " dB, with the 200 Hz SC HPF " << hpf << " dB");
	// THE OFF CASE IS THE CURVE: -6 dB peaks, T -20, R ∞ -> the whole 14 dB over the threshold.
	CHECK(std::fabs(off - 14.0f) < 0.2f);
	CHECK(off - hpf >= 10.0f);
}

TEST_CASE("RMS detects the mean square") {
	comp::Settings s;
	s.detect = static_cast<int>(comp::Detect::Rms);
	Instance dev;
	dev.set(s);
	float lo = 1e9f, hi = -1e9f;
	for (int k = 0; k < kSecond; ++k) {
		float l = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * k / kSampleRate)), r = l;
		dev.process(&l, &r, 1);
		if (k >= kSecond - 480) {
			lo = std::min(lo, dev.detectorDb());
			hi = std::max(hi, dev.detectorDb());
		}
	}
	// A SINE OF PEAK 1 HAS MEAN SQUARE 1/2: 10*log10(0.5) = -3.0103 dB, and the whole last 10 ms sits
	// within 0.1 dB of it (the 2 kHz ripple through a 10 ms pole is a few hundredths).
	MESSAGE("RMS detector over the last 10 ms: " << lo << " .. " << hi << " dB");
	CHECK(std::fabs(lo - (-3.0103f)) < 0.1f);
	CHECK(std::fabs(hi - (-3.0103f)) < 0.1f);
}

TEST_CASE("readings are the declared count, held, and truthful") {
	comp::Settings s;
	Instance dev;
	dev.set(s);
	dev.runDc(0.0f, 1000, 64);
	CHECK(dev.readingsCount() == static_cast<size_t>(comp::Reading::kCount));
	CHECK(dev.publishes() > 0u);

	// A SINGLE-SAMPLE SPIKE OF 0.8 IN SILENCE READS AS 0.8 FOR THE NEXT 50 ms OF BLOCKS.
	std::array<float, 64> l {}, r {};
	l[37] = 0.8f;
	dev.process(l.data(), r.data(), 64);
	size_t low = 0;
	for (int k = 0; k < 2400; k += 64) {
		l.fill(0.0f);
		r.fill(0.0f);
		dev.process(l.data(), r.data(), 64);
		low += dev.reading(comp::Reading::InPeak) < 0.8f;
	}
	CHECK(low == 0u);

	// THE GR READING IS -gainDb AT A STEADY STATE, and it HOLDS after the gain has let go.
	dev.runDc(1.0f, kSecond / 2, 64);
	const float steady = -dev.gainDb();
	REQUIRE(steady > 10.0f);
	CHECK(std::fabs(dev.reading(comp::Reading::GainReduction) - steady) < 1e-4f);
	size_t dropped = 0;
	for (int k = 0; k < 2400; k += 64) {
		dev.runDc(0.0f, 64, 64);
		dropped += dev.reading(comp::Reading::GainReduction) < steady - 1e-4f;
	}
	CHECK(dropped == 0u);
	// NOT VACUOUS: the gain itself has let go by several dB inside that window.
	CHECK(-dev.gainDb() < steady - 3.0f);
}

TEST_CASE("disabled is a pass-through once the bypass ramp lands") {
	comp::Settings s;
	s.thresholdDb = -40.0f;
	s.ratio = 10.0f;
	s.makeupDb = 6.0f;
	Instance dev;
	dev.set(s);
	const size_t late = dev.latency();
	Noise noise;
	// THE INPUT `latency` SAMPLES AGO IS WHAT A PASS-THROUGH HANDS BACK (backlog LE): the device is late by
	// it disabled too, because the host was told one latency and pads everything else by it.
	std::vector<float> inL, inR;
	std::vector<float> l(240), r(240);
	const auto block = [&] {
		const size_t at = inL.size();
		for (size_t i = 0; i < 240; ++i) {
			l[i] = noise.next();
			r[i] = noise.next();
			inL.push_back(l[i]);
			inR.push_back(r[i]);
		}
		dev.process(l.data(), r.data(), 240);
		size_t differ = 0;
		for (size_t i = 0; i < 240; ++i) {
			const size_t t = at + i;
			differ += (l[i] != (t >= late ? inL[t - late] : 0.0f)) + (r[i] != (t >= late ? inR[t - late] : 0.0f));
		}
		return differ;
	};
	for (int k = 0; k < 20; ++k) {
		block();
	}
	// NOT VACUOUS: enabled, it changes every sample.
	CHECK(block() > 400u);
	s.enabled = false;
	dev.set(s);
	block(); // the 5 ms bypass ramp, still entering
	block(); // ...and leaving, `latency` later
	for (int k = 0; k < 20; ++k) {
		CHECK(block() == 0u);
	}
	CHECK(dev.reading(comp::Reading::GainReduction) == 0.0f);
}

TEST_CASE("a disable/re-enable cycle does not care how the host cuts the block") {
	// THE SAME AUDIO AND THE SAME SETTINGS CHANGES AT THE SAME SAMPLES, rendered in different block
	// sizes, must be the same samples out. The case that broke this: the pass-through (and the detector
	// clear that goes with it) was chosen per BLOCK, so whether the detector was still tracking after the
	// bypass landed depended on where the next block happened to start.
	comp::Settings on;
	on.thresholdDb = -40.0f;
	on.ratio = 10.0f;
	on.makeupDb = 6.0f;
	on.detect = static_cast<int>(comp::Detect::Rms);
	on.scHpf = 4;
	on.release = static_cast<float>(std::log2(0.5)); // slow, so stale detector state shows on re-enable
	comp::Settings off = on;
	off.enabled = false;
	// SEGMENTS: enabled, then disabled well past the 5 ms ramp, then enabled again.
	const std::array<std::pair<const comp::Settings*, int>, 3> segments {
		{ { &on, 4800 }, { &off, 1000 }, { &on, 4800 } }
	};
	Noise noise;
	std::vector<float> inL, inR;
	for (int k = 0; k < 4800 + 1000 + 4800; ++k) {
		inL.push_back(noise.next());
		inR.push_back(noise.next());
	}
	const auto render = [&](int cut) {
		Instance dev;
		std::vector<float> l = inL, r = inR;
		size_t at = 0;
		for (const auto& [settings, length] : segments) {
			dev.set(*settings);
			const int block = cut > 0 ? cut : length; // 0: each segment as ONE block
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
	for (const int cut : { 1, 7, 240, 257 }) {
		const std::vector<float> other = render(cut);
		size_t differ = 0;
		for (size_t i = 0; i < reference.size(); ++i) {
			differ += reference[i] != other[i];
		}
		CAPTURE(cut);
		CHECK(differ == 0u);
	}
}

TEST_CASE("switching off crossfades to the dry signal over the 5 ms ramp rather than stepping") {
	// AT A STEADY DC THE WET SIGNAL IS KNOWN — x * dB2lin(gainDb + makeup) — so each sample of the
	// ramp has a VALUE: x + b*(wet - x), b falling in a straight line from 1 to 0 over 240 samples.
	comp::Settings s;
	s.thresholdDb = -30.0f;
	s.makeupDb = 2.0f;
	Instance dev;
	dev.set(s);
	dev.runDc(1.0f, kSecond / 2);
	const float wet = dbToLin(static_cast<double>(dev.gainDb()) + 2.0);
	REQUIRE(wet < 0.5f);
	s.enabled = false;
	dev.set(s);
	// THE RAMP LEAVES `latency` SAMPLES AFTER IT ENTERS (backlog LE): until then the wet already in flight.
	const int late = static_cast<int>(dev.latency());
	for (int k = 1; k <= 240 + late; ++k) {
		float l = 1.0f, r = 1.0f;
		dev.process(&l, &r, 1);
		const float b = k <= late ? 1.0f : 1.0f - static_cast<float>(k - late) / 240.0f;
		CAPTURE(k);
		CHECK(std::fabs(l - (1.0f + b * (wet - 1.0f))) < 1e-4f);
	}
}

TEST_CASE("one NaN or inf sample does not latch: 1 ms later the device is back on its trajectory") {
	// THE REFERENCE IS A TWIN fed the same audio with the bad sample replaced by 0 — the device's
	// declared treatment of a non-finite input — so "recovered" is a value, not "is finite".
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	struct Bad {
		bool right;
		float value;
	};
	for (const bool rms : { false, true }) {
		for (const Bad bad : { Bad { false, nan }, Bad { true, nan }, Bad { false, inf } }) {
			CAPTURE(rms);
			CAPTURE(bad.right);
			CAPTURE(bad.value);
			comp::Settings s;
			s.thresholdDb = -30.0f;
			s.ratio = 6.0f;
			s.detect = static_cast<int>(rms ? comp::Detect::Rms : comp::Detect::Peak);
			s.scHpf = rms ? 3 : 0; // the RMS pass also runs the SC HPF, whose z1/z2 a NaN would poison
			Instance dev, twin;
			dev.set(s);
			twin.set(s);
			Noise noise;
			constexpr int kAt = 2400, kTotal = 4800;
			size_t nonFinite = 0, offTrack = 0;
			for (int k = 0; k < kTotal; ++k) {
				// R LOUDER THAN L, so a detector deaf to R (a poisoned R filter) is off the trajectory.
				const float xl = 0.3f * noise.next(), xr = noise.next();
				float l = xl, r = xr, tl = xl, tr = xr;
				if (k == kAt) {
					(bad.right ? r : l) = bad.value;
					(bad.right ? tr : tl) = 0.0f;
				}
				dev.process(&l, &r, 1);
				twin.process(&tl, &tr, 1);
				nonFinite += !std::isfinite(l) + !std::isfinite(r);
				if (k >= kAt + 48) {
					offTrack += (std::fabs(l - tl) > 1e-6f) + (std::fabs(r - tr) > 1e-6f);
				}
			}
			CHECK(nonFinite == 0u);
			CHECK(offTrack == 0u);
			CHECK(std::isfinite(dev.gainDb()));
			CHECK(std::fabs(dev.gainDb() - twin.gainDb()) < 1e-4f);
			// NOT VACUOUS: the twin is compressing, so a gain stuck at 0 would be off the trajectory.
			CHECK(twin.gainDb() < -3.0f);
		}
	}
}

TEST_CASE("a huge finite sample overflows nothing for good") {
	// 1e30 IS FINITE, SO THE INPUT GUARD PASSES IT — and squared it is inf in float. This is the case the
	// STATE guards exist for: an inf mean square (or a NaN out of the curve at inf dB) that stayed in the
	// recursion would latch exactly as a NaN input did.
	// WITH THE SC HPF TOO: unclamped, a 1e30 impulse rang the biquad at ~1e19..1e30 for ~100 ms and fed a
	// mean square of up to 3e38 — measured 22.6 dB off the twin a SECOND later. The detector's copy is now
	// clamped to +-1e4 before it is filtered or squared, so the disturbance is bounded by what +80 dBFS
	// can do.
	constexpr int kAt = 2400;
	const int releaseSamples = static_cast<int>(std::lround(std::exp2(comp::Settings {}.release) * kSampleRate));
	for (const bool rms : { false, true }) {
		for (const int hpf : { 0, 3 }) {
			CAPTURE(rms);
			CAPTURE(hpf);
			comp::Settings s;
			s.thresholdDb = -30.0f;
			s.ratio = 6.0f;
			s.detect = static_cast<int>(rms ? comp::Detect::Rms : comp::Detect::Peak);
			s.scHpf = hpf;
			Instance dev, twin;
			dev.set(s);
			twin.set(s);
			Noise noise;
			size_t nonFinite = 0;
			int lastOff = -1; // the last sample at which the gain was more than 0.1 dB off the twin's
			for (int k = 0; k < kSecond + kAt; ++k) {
				const float x = noise.next();
				float l = x, r = x, tl = x, tr = x;
				if (k == kAt) {
					l = 1e30f;
					tl = 0.0f;
				}
				dev.process(&l, &r, 1);
				twin.process(&tl, &tr, 1);
				nonFinite += !std::isfinite(l) + !std::isfinite(r);
				if (!(std::fabs(dev.gainDb() - twin.gainDb()) <= 0.1f)) {
					lastOff = k;
				}
			}
			const int recovery = lastOff < 0 ? 0 : lastOff - kAt + 1;
			MESSAGE("rms " << rms << " hpf " << hpf << ": back within 0.1 dB of the twin after " << recovery
			               << " samples (release " << releaseSamples << ")");
			CHECK(nonFinite == 0u);
			CHECK(std::isfinite(dev.gainDb()));
			// BACK ON THE TWIN'S TRACK WITHIN EIGHT RELEASE TIME CONSTANTS, and staying there. Not ONE: the
			// clamp lets the detector read up to +80 dBFS, which is ~100 dB of reduction at this threshold,
			// and a one-pole release takes tau * ln(100 / 0.1) ~ 6.9 tau to come back within 0.1 dB of anything.
			// Measured: Peak 345 / 9760 samples (SC HPF off / on), RMS 30132 / 30121, against tau = 4800.
			// Unclamped, the RMS + HPF pass was still 22.6 dB off a whole second later.
			CHECK(recovery <= 8 * releaseSamples);
			CHECK(twin.gainDb() < -3.0f);
		}
	}
	// AND THE OUTPUT GUARD: at ratio 1 nothing is reduced, so FLT_MAX-scale input times +24 dB of makeup
	// overflows to inf in the multiply. It leaves as 0, never as inf.
	comp::Settings s;
	s.ratio = 1.0f;
	s.makeupDb = 24.0f;
	Instance dev;
	dev.set(s);
	float l = 3e38f, r = -3e38f;
	dev.process(&l, &r, 1);
	CHECK(std::isfinite(l));
	CHECK(std::isfinite(r));
}

TEST_CASE("the stereo link: one channel's level sets one gain for both") {
	// THE DETECTOR IS THE LOUDER CHANNEL (max of the peaks, or of the two mean squares), and the gain it
	// computes is applied to BOTH channels — so a signal on one side ducks the other by the same amount,
	// and silence stays silence.
	for (const bool rms : { false, true }) {
		for (const bool loudRight : { true, false }) {
			CAPTURE(rms);
			CAPTURE(loudRight);
			comp::Settings s;
			s.thresholdDb = -30.0f;
			s.ratio = 4.0f;
			s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
			s.detect = static_cast<int>(rms ? comp::Detect::Rms : comp::Detect::Peak);
			// ONE SIDE SILENT: the loud side alone must engage the detector.
			{
				Instance dev;
				dev.set(s);
				std::vector<float> l(240), r(240);
				for (int k = 0; k < kSecond / 4; k += 240) {
					for (size_t i = 0; i < 240; ++i) {
						const float v = 0.7f * static_cast<float>(std::sin(0.05 * static_cast<double>(k + static_cast<int>(i))));
						(loudRight ? r : l)[i] = v;
						(loudRight ? l : r)[i] = 0.0f;
					}
					dev.process(l.data(), r.data(), 240);
				}
				CHECK(dev.gainDb() < -10.0f);
				const auto& quiet = loudRight ? l : r;
				CHECK(std::all_of(quiet.begin(), quiet.end(), [](float v) { return v == 0.0f; }));
			}
			// BOTH SIDES AT DC, one loud and one quiet: both get the SAME gain.
			{
				Instance dev;
				dev.set(s);
				const float loud = 0.9f, quiet = 0.001f;
				float l = 0.0f, r = 0.0f;
				for (int k = 0; k < kSecond / 4; ++k) {
					l = loudRight ? quiet : loud;
					r = loudRight ? loud : quiet;
					dev.process(&l, &r, 1);
				}
				const float gl = l / (loudRight ? quiet : loud);
				const float gr = r / (loudRight ? loud : quiet);
				CHECK(gr < 0.5f);
				CHECK(std::fabs(gl - gr) < 1e-5f * gr);
			}
		}
	}
}

} // namespace

//------------------------------------------------------------------------------------------------//
// BACKLOG LQ — DUAL MONO, STEREO LINK AND M/S                                                    //
//------------------------------------------------------------------------------------------------//

namespace {

/// FNV-1a over the BITS of both channels of a render: decorrelated stereo noise under a burst
/// envelope, in uneven blocks, so every smoother and ramp is exercised across partitions.
uint64_t lqRenderHash(const comp::Settings& s) {
	Instance dev;
	dev.set(s);
	Noise nl { 0x12345678u }, nr { 0x9e3779b9u };
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](float v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int b = 0; b < 4; ++b) {
			h ^= (bits >> (8 * b)) & 0xffu;
			h *= 1099511628211ull;
		}
	};
	constexpr int kBlocks[] = { 32, 17, 256, 5, 128 };
	// THE SAMPLES HASHED ARE THE ONES THE PRE-LQ BUILD HASHED — the blocks it ran to reach 24000 — read
	// `latency` samples later (backlog LE), so a Compress render is compared with its zero-latency self;
	// the input runs on past them only to push them out.
	int total = 0;
	for (int n = 0; total < 24000; ++n) {
		total += kBlocks[n % 5];
	}
	const int late = static_cast<int>(dev.latency());
	int t = 0;
	for (int n = 0; t < total + late; ++n) {
		const int frames = kBlocks[n % 5];
		const int at = t;
		std::array<float, 256> l {}, r {};
		for (int i = 0; i < frames; ++i, ++t) {
			const float env = (t / 4000) % 3 == 1 ? 0.9f : 0.05f;
			l[static_cast<size_t>(i)] = env * nl.next();
			r[static_cast<size_t>(i)] = env * (0.5f * nl.next() + 0.5f * nr.next());
		}
		dev.process(l.data(), r.data(), frames);
		for (int i = 0; i < frames; ++i) {
			if (at + i >= late && at + i < total + late) {
				mix(l[static_cast<size_t>(i)]);
				mix(r[static_cast<size_t>(i)]);
			}
		}
	}
	return h;
}

} // namespace

TEST_CASE("LQ: at the default stereo settings COMP renders bit-identically to the linked build before LQ") {
	// THE HASHES WERE TAKEN FROM THE BUILD BEFORE LQ (v1-implementation 4aad7e83): the claim is "today's
	// behaviour, bit for bit", made against the pre-change output itself. Stereo Link and M/S stay at their
	// declared defaults (100%, L/R) in every variant; the variants cover both modes, both detectors, the
	// sidechain high-pass, makeup, parallel mix and an instant release.
	// **BACKLOG LE**: the three COMPRESS hashes are still the pre-LQ build's, unchanged — each render is read
	// `latency` samples late (`lqRenderHash`), so they now say "the zero-latency Compress, delayed to the bit".
	// The two LIMIT hashes are NEW, taken from the LE build (comp-lookahead) when Limit gained its lookahead:
	// a pin, so a later change to the limiter is seen, not a claim about any earlier build.
	struct Variant {
		const char* name;
		comp::Settings s;
		uint64_t hash;
	};
	comp::Settings base;
	comp::Settings rms = base;
	rms.detect = static_cast<int>(comp::Detect::Rms);
	rms.scHpf = 4;
	rms.thresholdDb = -30.0f;
	comp::Settings parallel = base;
	parallel.mix = 0.5f;
	parallel.makeupDb = 6.0f;
	parallel.ratio = comp::kRatioInfinite;
	parallel.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	comp::Settings limit = base;
	limit.mode = static_cast<int>(comp::Mode::Limit);
	limit.driveDb = 12.0f;
	limit.ceilingDb = -3.0f;
	comp::Settings limitHpf = limit;
	limitHpf.scHpf = 7;
	limitHpf.release = static_cast<float>(std::log2(comp::kReleaseMinSeconds));
	const Variant variants[] = {
		{ "default", base, 7600463036316032116ull },
		{ "RMS + SC HPF", rms, 6946840227691340500ull },
		{ "parallel brickwall", parallel, 2247224198612346726ull },
		{ "Limit", limit, 16594780914212203330ull },
		{ "Limit + SC HPF, instant release", limitHpf, 6615134331524989098ull },
	};
	for (const Variant& v : variants) {
		CAPTURE(v.name);
		const uint64_t h = lqRenderHash(v.s);
		const std::string seen = std::string(v.name) + " hash " + std::to_string(h);
		MESSAGE(seen);
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
		CHECK(h == v.hash);
#else
		(void)h; // computed on every platform, compared only where it was pinned
#endif
	}
}

namespace {

/// THE SETTINGS THE LQ CASES SHARE: a hard knee so the static curve is two straight lines a case can
/// restate by hand, the fastest attack, Peak detection.
comp::Settings lqCompress(float link, comp::StereoMode stereo) {
	comp::Settings s;
	s.thresholdDb = -30.0f;
	s.ratio = 4.0f;
	s.kneeDb = 0.0f;
	s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	s.detect = static_cast<int>(comp::Detect::Peak);
	s.stereoLink = link;
	s.stereoMode = static_cast<int>(stereo);
	return s;
}

/// THE HARD-KNEE GAIN COMPUTER, restated: the reduction in dB for a detector level `x`.
double hardKneeGainDb(double x, double threshold, double ratio) {
	return x <= threshold ? 0.0 : (threshold + (x - threshold) / ratio) - x;
}

} // namespace

TEST_CASE("LQ: Link 0 — a hot left channel leaves R's gain untouched, in Compress and in Limit") {
	// L at -6 dBFS is 24 dB over the threshold; R at -46 is 16 dB under it. Unlinked, R's detector
	// hears only R, so R's gain must stay EXACTLY at rest.
	{
		Instance dev;
		dev.set(lqCompress(0.0f, comp::StereoMode::LeftRight));
		const auto [yl, yr] = dev.runDc(dbToLin(-6.0), dbToLin(-46.0), kSecond / 2);
		CHECK(dev.gainDb(1) == 0.0f);
		CHECK(dev.gainDb(0) == doctest::Approx(hardKneeGainDb(-6.0, -30.0, 4.0)).epsilon(1e-4)); // -18 dB
		CHECK(yr == dbToLin(-46.0)); // R passes bit for bit: gain 1, Mix 1
		CHECK(dev.reading(comp::Reading::GainReductionB) == 0.0f);
		CHECK(dev.reading(comp::Reading::GainReductionA) == doctest::Approx(18.0).epsilon(1e-3));
		(void)yl;
	}
	{
		comp::Settings s = lqCompress(0.0f, comp::StereoMode::LeftRight);
		s.mode = static_cast<int>(comp::Mode::Limit);
		s.driveDb = 6.0f;
		s.ceilingDb = -3.0f;
		Instance dev;
		dev.set(s);
		const auto [yl, yr] = dev.runDc(0.9f, 0.1f, kSecond / 2);
		CHECK(dev.gainDb(1) == 0.0f); // 0.1 * 2 = -14 dBFS, under the -3 dB ceiling
		CHECK(dev.gainDb(0) < -5.0f);
		CHECK(std::fabs(yl) <= dbToLin(-3.0));
		CHECK(yr == doctest::Approx(0.1 * dbToLin(6.0)).epsilon(1e-6));
	}
}

TEST_CASE("LQ: Link 100 — R ducks with L by exactly the linked gain") {
	Instance dev;
	dev.set(lqCompress(1.0f, comp::StereoMode::LeftRight));
	dev.runDc(dbToLin(-6.0), dbToLin(-46.0), kSecond / 2);
	CHECK(dev.gainDb(1) == dev.gainDb(0));
	CHECK(dev.gainDb(1) == doctest::Approx(hardKneeGainDb(-6.0, -30.0, 4.0)).epsilon(1e-4));
}

TEST_CASE("LQ: Link 50% puts each channel's detector halfway, in dB, to the louder channel's") {
	// THE LAW: effective = (1 - link) * own + link * max(own, other), in dB. R's own is -46 and the shared
	// level is L's -6, so at 50% R's detector reads -26: 4 dB over a -30 threshold at 4:1 is -3 dB of
	// gain. L is the louder channel, so its own level IS the shared one and it reads -18 at any Link.
	Instance dev;
	dev.set(lqCompress(0.5f, comp::StereoMode::LeftRight));
	dev.runDc(dbToLin(-6.0), dbToLin(-46.0), kSecond / 2);
	const double effectiveR = 0.5 * -46.0 + 0.5 * -6.0;
	CHECK(dev.gainDb(1) == doctest::Approx(hardKneeGainDb(effectiveR, -30.0, 4.0)).epsilon(1e-3)); // -3 dB
	CHECK(dev.gainDb(0) == doctest::Approx(hardKneeGainDb(-6.0, -30.0, 4.0)).epsilon(1e-4));       // -18 dB
	// AND IT IS BETWEEN THE TWO ENDS, strictly — the gain at 0 (none) and at 100% (-18).
	CHECK(dev.gainDb(1) < -1.0f);
	CHECK(dev.gainDb(1) > -17.0f);
}

TEST_CASE("LQ: M/S — a mono signal leaves Side's gain at rest; a Side-only peak ducks only Side") {
	// LINK 0, so each of Mid and Side listens only to itself. (At Link 100 Side follows Mid's level by
	// design: the two then share one gain, which on a mono input touches nothing, since Side is zero.)
	SUBCASE("mono") {
		Instance dev;
		dev.set(lqCompress(0.0f, comp::StereoMode::MidSide));
		const auto [yl, yr] = dev.runDc(dbToLin(-6.0), dbToLin(-6.0), kSecond / 2);
		CHECK(dev.gainDb(1) == 0.0f);
		CHECK(dev.gainDb(0) == doctest::Approx(hardKneeGainDb(-6.0, -30.0, 4.0)).epsilon(1e-4));
		CHECK(yl == yr); // still mono after decoding
		CHECK(linToDb(static_cast<double>(yl)) == doctest::Approx(-6.0 - 18.0).epsilon(1e-3));
	}
	SUBCASE("side only") {
		// L = +x, R = -x: Mid is 0 and Side is x.
		Instance dev;
		dev.set(lqCompress(0.0f, comp::StereoMode::MidSide));
		const float x = dbToLin(-6.0);
		const auto [yl, yr] = dev.runDc(x, -x, kSecond / 2);
		CHECK(dev.gainDb(0) == 0.0f);
		const double sideGain = hardKneeGainDb(-6.0, -30.0, 4.0);
		CHECK(dev.gainDb(1) == doctest::Approx(sideGain).epsilon(1e-4));
		// DECODED: L = M + S*g = x*g, R = M - S*g = -x*g.
		CHECK(static_cast<double>(yl) == doctest::Approx(static_cast<double>(x) * std::pow(10.0, sideGain / 20.0)).epsilon(1e-3));
		CHECK(yr == -yl);
	}
}

TEST_CASE("LQ: M/S Limit holds the L/R ceiling after decoding with a gain stage, not the clamp") {
	// HARD LEFT, DRIVEN: L = 1 x 12 dB, R = 0, so Mid and Side are each 6.02 dB under L — each is limited
	// to the ceiling on its own, and decoded they would sum to TWICE it. The L/R stage takes the 6.02 dB
	// back as a gain (which the clamp would otherwise take as clipping), and R stays exactly silent.
	comp::Settings s = lqCompress(0.0f, comp::StereoMode::MidSide);
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.driveDb = 12.0f;
	s.ceilingDb = -1.0f;
	Instance dev;
	dev.set(s);
	const auto [yl, yr] = dev.runDc(1.0f, 0.0f, kSecond / 2);
	CHECK(dev.ceilingStageDb() == doctest::Approx(-20.0 * std::log10(2.0)).epsilon(1e-3));
	CHECK(static_cast<double>(yl) == doctest::Approx(dbToLin(-1.0)).epsilon(1e-4));
	CHECK(yr == 0.0f);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG LE — LIMIT LOOKS AHEAD, AND THE DEVICE IS LATE BY IT IN BOTH MODES                     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("LE: the latency is Limit's 1.5 ms lookahead, in every mode, in L/R and M/S, enabled or not") {
	// THE NUMBER IS THE CONSTANT'S, restated here: round(0.0015 s * rate). A Mode, Stereo Mode or Enabled
	// flip that moved it would ask the host to restart (the Flanger's TZ lesson).
	for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
		const auto want = static_cast<uint32_t>(std::lround(0.0015 * rate));
		CAPTURE(rate);
		for (const comp::Mode mode : { comp::Mode::Compress, comp::Mode::Limit, comp::Mode::Compander }) {
			for (const comp::StereoMode stereo : { comp::StereoMode::LeftRight, comp::StereoMode::MidSide }) {
				for (const bool on : { true, false }) {
					comp::Settings s;
					s.mode = static_cast<int>(mode);
					s.stereoMode = static_cast<int>(stereo);
					s.enabled = on;
					const comp::Row row = comp::rowOf(s);
					CHECK(comp::kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate) == want);
				}
			}
		}
	}
	CHECK(comp::latencySamplesFor(48000.0) == 72u);
}

TEST_CASE("LE: the device delays by exactly the latency it reports, in every mode") {
	// AN IMPULSE UNDER EVERY THRESHOLD AND CEILING, so nothing but the delay acts on it: it must leave at
	// sample `latency` exactly and nowhere else — the count the host pads every other signal by.
	for (const comp::Mode mode : { comp::Mode::Compress, comp::Mode::Limit }) {
		for (const comp::StereoMode stereo : { comp::StereoMode::LeftRight, comp::StereoMode::MidSide }) {
			for (const bool on : { true, false }) {
				comp::Settings s;
				s.mode = static_cast<int>(mode);
				s.stereoMode = static_cast<int>(stereo);
				s.enabled = on;
				CAPTURE(static_cast<int>(mode));
				CAPTURE(static_cast<int>(stereo));
				CAPTURE(on);
				Instance dev;
				dev.set(s);
				const size_t late = dev.latency();
				REQUIRE(late == 72u);
				std::vector<float> l(400, 0.0f), r(400, 0.0f);
				l[10] = 0.01f;
				r[10] = -0.005f;
				dev.process(l.data(), r.data(), 400);
				size_t elsewhere = 0;
				for (size_t i = 0; i < l.size(); ++i) {
					if (i != 10 + late) {
						elsewhere += (l[i] != 0.0f) + (r[i] != 0.0f);
					}
				}
				CHECK(elsewhere == 0u);
				CHECK(l[10 + late] == doctest::Approx(0.01f).epsilon(1e-6));
				CHECK(r[10 + late] == doctest::Approx(-0.005f).epsilon(1e-6));
			}
		}
	}
}

namespace {

/// A DC STEP INTO LIMIT: `before` for a while, then `after`, both channels (or `after` on the left only,
/// `wide`, which in M/S puts half of it in Mid and half in Side), on a device settled on `before`.
/// out[k] is the output for the sample that ENTERED at k (the device's latency taken off); the step
/// entered at `kStepAt`.
constexpr int kStepAt = 400;

struct StepRender {
	std::vector<float> l, r;
	size_t late = 0;
	/// M/S Limit's L/R stage at the end of the render, in dB.
	float stageDb = 0.0f;
};

StepRender stepInto(comp::StereoMode stereo, float ceilingDb, float before, float after, bool wide) {
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.ceilingDb = ceilingDb;
	s.stereoMode = static_cast<int>(stereo);
	Instance dev;
	dev.set(s);
	StepRender out;
	out.late = dev.latency();
	dev.runDc(before, before, kSecond / 4);
	// `latency` MORE SAMPLES THAN THE WINDOW, so the window's last sample has left too.
	const size_t frames = 2 * static_cast<size_t>(kStepAt) + out.late;
	std::vector<float> l(frames), r(frames);
	for (size_t i = 0; i < frames; ++i) {
		const bool stepped = i >= static_cast<size_t>(kStepAt);
		l[i] = stepped ? after : before;
		r[i] = stepped ? (wide ? 0.0f : after) : before;
	}
	// IN UNEVEN BLOCKS: the stages run per sample, so the host's cut must not show.
	for (size_t done = 0; done < frames;) {
		const size_t n = std::min<size_t>(37, frames - done);
		dev.process(l.data() + done, r.data() + done, static_cast<int>(n));
		done += n;
	}
	// ELEMENT k LEFT AT k + latency, so it is the sample that ENTERED at k.
	out.l.assign(l.begin() + static_cast<std::ptrdiff_t>(out.late), l.end());
	out.r.assign(r.begin() + static_cast<std::ptrdiff_t>(out.late), r.end());
	out.stageDb = dev.ceilingStageDb();
	return out;
}

} // namespace

TEST_CASE("LE: a step 12 dB over the ceiling leaves AT the ceiling, and the gain was already ramping down") {
	// THE CLAIM AS VALUES, ON DC so y / x is the gain applied to each sample. -20 dBFS sits 19 dB under a
	// -1 dB ceiling; the step is to 12 dB over it.
	//  - NO SAMPLE PASSES THE CEILING (the lookahead's promise; the clamp is there for the SC HPF);
	//  - THE STEP'S FIRST SAMPLE LEAVES AT THE CEILING ITSELF: every gain the boxes average for it was
	//    computed with the step inside the hold, so the gain is ceiling / peak to the rounding;
	//  - THE GAIN IS ALREADY DOWN BEFORE THE STEP ARRIVES, and starts falling no earlier than the
	//    lookahead: the sample `latency + 1` before the step is untouched, the one just before is turned
	//    down. The zero-latency Limit had the first and not the third — its step left at the ceiling by
	//    the clamp, every sample before it untouched;
	//  - THE RAMP NEVER STEPS by more than the block's `1 / boxA` per sample.
	for (const comp::StereoMode stereo : { comp::StereoMode::LeftRight, comp::StereoMode::MidSide }) {
		CAPTURE(static_cast<int>(stereo));
		const float ceiling = dbToLin(-1.0);
		const float before = dbToLin(-20.0);
		const float after = dbToLin(11.0);
		const StepRender out = stepInto(stereo, -1.0f, before, after, false);
		const size_t late = out.late;
		// M/S HAS TWO STAGES OF HALF THE LOOKAHEAD EACH; a mono step lives in Mid and the first one.
		const size_t ramp = stereo == comp::StereoMode::MidSide ? comp::midSideLookaheadFor(static_cast<uint32_t>(late))
		                                                        : late;
		const auto step = static_cast<size_t>(kStepAt);
		float worst = 0.0f;
		for (size_t k = 0; k < out.l.size(); ++k) {
			worst = std::max({ worst, std::fabs(out.l[k]), std::fabs(out.r[k]) });
		}
		MESSAGE("worst " << worst << " ceiling " << ceiling);
		CHECK(worst <= ceiling * (1.0f + 1e-6f));
		CHECK(out.l[step] == doctest::Approx(ceiling).epsilon(1e-5));
		CHECK(out.r[step] == doctest::Approx(ceiling).epsilon(1e-5));
		// UNTOUCHED BEFORE THE LOOKAHEAD — in L/R to the bit (a gain of exactly 1); M/S encodes and decodes.
		const float untouched = out.l[step - ramp - 1];
		if (stereo == comp::StereoMode::LeftRight) {
			CHECK(untouched == before);
		}
		else {
			CHECK(untouched == doctest::Approx(before).epsilon(1e-6));
		}
		// ALREADY DOWN THE SAMPLE BEFORE: the ramp toward ceiling / peak is under way.
		const double gJustBefore = static_cast<double>(out.l[step - 1]) / static_cast<double>(before);
		MESSAGE("gain the sample before the step " << gJustBefore << ", at the step " << out.l[step] / after);
		CHECK(gJustBefore < 0.9);
		CHECK(gJustBefore > static_cast<double>(ceiling / after));
		// A RAMP, NOT A STEP: the box pair's bound on the gain's step per sample (`1 / boxA`, boxA the
		// longer half of `ramp + 2`).
		const double stepBound = 1.0 / static_cast<double>((ramp + 2) / 2);
		double steepest = 0.0;
		for (size_t k = step - ramp - 1; k < step; ++k) {
			const double g0 = static_cast<double>(out.l[k]) / static_cast<double>(before);
			const double g1 = static_cast<double>(out.l[k + 1]) / static_cast<double>(k + 1 < step ? before : after);
			steepest = std::max(steepest, g0 - g1);
		}
		MESSAGE("steepest fall per sample " << steepest << " bound " << stepBound);
		CHECK(steepest <= stepBound + 1e-6);
		CHECK(steepest > 0.0);
	}
}

TEST_CASE("LE: M/S Limit's L/R stage looks ahead too — a wide step never passes the ceiling") {
	// L ALONE, 12 dB over: in M/S that is Mid and Side each 6 dB over, each limited to the ceiling, and
	// decoded to twice it on the left — the L/R stage after decoding must take that back, AHEAD of the
	// sample, so the left leaves at the ceiling and is turned down before it arrives.
	const float ceiling = dbToLin(-1.0);
	const float before = dbToLin(-20.0);
	const float after = dbToLin(11.0);
	const StepRender out = stepInto(comp::StereoMode::MidSide, -1.0f, before, after, true);
	const auto step = static_cast<size_t>(kStepAt);
	float worst = 0.0f;
	for (size_t k = 0; k < out.l.size(); ++k) {
		worst = std::max({ worst, std::fabs(out.l[k]), std::fabs(out.r[k]) });
	}
	MESSAGE("worst " << worst << " ceiling " << ceiling);
	CHECK(worst <= ceiling * (1.0f + 1e-6f));
	// NOT VACUOUS: the left leaves at the ceiling, which only the L/R stage can have put it at (Mid and
	// Side each at the ceiling decode to twice it).
	CHECK(out.l[step] == doctest::Approx(ceiling).epsilon(1e-4));
	CHECK(out.l[step - 1] < before * 0.9f);
	// AND IT IS THE L/R STAGE THAT HOLDS IT, AS A GAIN: Mid and Side each at the ceiling decode to twice it,
	// so the stage sits at -6.02 dB once the step has settled — read from the seam, not the clamp.
	MESSAGE("L/R stage " << out.stageDb << " dB");
	CHECK(out.stageDb == doctest::Approx(-6.0206).epsilon(1e-3));
}

TEST_CASE("LE: Limit's release is the zero-latency Limit's — a one-pole on the reduction in dB") {
	// AFTER A STEP DOWN TO SILENCE-UNDER-THE-CEILING, the reduction must decay as G * exp(-t / tau) — the
	// branching smoother's release the device had before LE, which the Release control names. The
	// block's own release (a one-pole on the LEVEL) recovers linearly in dB, far sooner: from 12 dB at
	// 100 ms it is back to 0 dB in ~140 ms, where this is still at 4.4 dB after 100 ms.
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.ceilingDb = -1.0f;
	s.release = static_cast<float>(std::log2(0.1)); // tau = 4800 samples
	Instance dev;
	dev.set(s);
	const float loud = dbToLin(11.0);
	const float quiet = dbToLin(-30.0);
	dev.runDc(loud, loud, kSecond / 4);
	REQUIRE(dev.gainDb() == doctest::Approx(-12.0).epsilon(1e-3));
	// SAMPLE BY SAMPLE: once the loud samples have left, y / x of a quiet one is the gain it was given.
	const size_t late = dev.latency();
	std::vector<double> reduction;
	for (int k = 0; k < 2 * 4800; ++k) {
		float l = quiet, r = quiet;
		dev.process(&l, &r, 1);
		reduction.push_back(-linToDb(static_cast<double>(l) / static_cast<double>(quiet)));
	}
	// THE FIRST QUIET SAMPLE LEAVES AT `latency` CARRYING THE LOUD ONES' REDUCTION. The release proper
	// starts once the loud peak has left the hold and the boxes, a lookahead or two later; from then on
	// it must be a pure exponential, so ONE TIME CONSTANT apart the reduction falls by exactly 1/e.
	const size_t from = late + 2 * late;
	const double ratio = reduction[from + 4800] / reduction[from];
	MESSAGE("reduction leaving " << reduction[late] << " dB; over one tau from " << reduction[from] << " dB: x"
	                             << ratio);
	CHECK(reduction[late] == doctest::Approx(12.0).epsilon(2e-3));
	CHECK(ratio == doctest::Approx(std::exp(-1.0)).epsilon(2e-3));
	// NOT VACUOUS: the release is well under way and well short of done.
	CHECK(reduction[from] > 9.0);
}

TEST_CASE("LE: a Mode or Stereo Mode flip mid-stream drops nothing and replays nothing") {
	// UNDER EVERY THRESHOLD AND CEILING, Compress and L/R Limit are each exactly the delay, so a flip
	// between them must leave the output exactly the input `latency` late, straight through the flip:
	// every sample leaves by the road it entered on. A flip that switched where samples are READ rather
	// than where they ENTER would hand back `latency` samples of the other road's delay — stale or silent.
	comp::Settings s;
	s.thresholdDb = 0.0f;
	s.kneeDb = 0.0f;
	Instance dev;
	dev.set(s);
	const size_t late = dev.latency();
	Noise noise;
	std::vector<float> inL, inR, outL, outR;
	const auto run = [&](int frames) {
		std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.3f * noise.next();
			r[i] = 0.3f * noise.next();
		}
		inL.insert(inL.end(), l.begin(), l.end());
		inR.insert(inR.end(), r.begin(), r.end());
		dev.process(l.data(), r.data(), frames);
		outL.insert(outL.end(), l.begin(), l.end());
		outR.insert(outR.end(), r.begin(), r.end());
	};
	run(500);
	s.mode = static_cast<int>(comp::Mode::Limit);
	dev.set(s);
	run(30); // shorter than the latency: flipped back while the Limit samples are still in flight
	s.mode = static_cast<int>(comp::Mode::Compress);
	dev.set(s);
	run(50);
	s.mode = static_cast<int>(comp::Mode::Limit);
	dev.set(s);
	run(500);
	s.mode = static_cast<int>(comp::Mode::Compress);
	dev.set(s);
	run(500);
	REQUIRE(outL.size() == inL.size());
	size_t differ = 0;
	for (size_t t = late; t < outL.size(); ++t) {
		differ += (outL[t] != inL[t - late]) + (outR[t] != inR[t - late]);
	}
	CHECK(differ == 0u);
	// AND M/S LIMIT, whose encode and decode round, to within it.
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.stereoMode = static_cast<int>(comp::StereoMode::MidSide);
	dev.set(s);
	const size_t from = outL.size();
	run(300);
	s.stereoMode = static_cast<int>(comp::StereoMode::LeftRight);
	dev.set(s);
	run(300);
	double worst = 0.0;
	for (size_t t = from; t < outL.size(); ++t) {
		worst = std::max({ worst, std::fabs(static_cast<double>(outL[t] - inL[t - late])),
		                   std::fabs(static_cast<double>(outR[t] - inR[t - late])) });
	}
	MESSAGE("M/S flip: worst |out - in late| " << worst);
	CHECK(worst < 1e-6);
}

TEST_CASE("LE: correlated stereo at the ceiling never overshoots — held by the stages' gain, not the clamp") {
	// THE DECISION ON M/S (a): Mid/Side and the L/R stage after decoding each get half the lookahead. The
	// material is CORRELATED stereo — one burst shared by both sides at different levels, plus a little of each
	// side's own — driven 12 dB into a -1 dB ceiling, so Mid and Side are both limited and decode past the ceiling,
	// and the L/R stage has to work. The claim is a VALUE read before the final clamp (`peakOverCeilingForTest`):
	// the loudest sample the stages let through, over its own ceiling, is at most 1. The output's peak is asserted
	// too, and it is the clamp's promise; the seam is what says the clamp had nothing to do.
	for (const comp::StereoMode stereo : { comp::StereoMode::MidSide, comp::StereoMode::LeftRight }) {
		CAPTURE(static_cast<int>(stereo));
		comp::Settings s;
		s.mode = static_cast<int>(comp::Mode::Limit);
		s.driveDb = 12.0f;
		s.ceilingDb = -1.0f;
		s.stereoMode = static_cast<int>(stereo);
		Instance dev;
		dev.set(s);
		const float ceiling = dbToLin(-1.0);
		Noise shared { 0x1234567u }, ownL { 0x2468aceu }, ownR { 0x13579bdu };
		float worst = 0.0f;
		float deepestStage = 0.0f;
		std::vector<float> l(97), r(97);
		for (int done = 0; done < kSecond; done += 97) {
			for (size_t i = 0; i < l.size(); ++i) {
				const int t = done + static_cast<int>(i);
				// 4 ms ATTACKS EVERY 100 ms, a 40 ms decay: transients, each a new peak.
				const int ph = t % 4800;
				const float env = ph < 192 ? static_cast<float>(ph) / 192.0f : std::exp(-static_cast<float>(ph - 192) / 1920.0f);
				const float c = env * shared.next();
				l[i] = 0.8f * c + 0.2f * env * ownL.next();
				r[i] = 0.5f * c + 0.2f * env * ownR.next();
			}
			dev.process(l.data(), r.data(), static_cast<int>(l.size()));
			for (size_t i = 0; i < l.size(); ++i) {
				worst = std::max({ worst, std::fabs(l[i]), std::fabs(r[i]) });
			}
			deepestStage = std::min(deepestStage, dev.ceilingStageDb());
		}
		MESSAGE("peak before the clamp " << dev.peakOverCeiling() << " of the ceiling; output peak " << worst
		                                 << " against " << ceiling << "; deepest L/R stage " << deepestStage << " dB");
		CHECK(dev.peakOverCeiling() <= 1.0f + 1e-6f);
		CHECK(worst <= ceiling * (1.0f + 1e-6f));
		// NOT VACUOUS: the output reaches the ceiling, and in M/S the L/R stage did real work on the way.
		CHECK(worst >= ceiling * 0.98f);
		if (stereo == comp::StereoMode::MidSide) {
			CHECK(deepestStage < -0.5f);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// BACKLOG MI: DRIVE IN COMPRESS                                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("MI: Compress's Drive is a gain in front of the gain computer — x driven D dB is x·10^(D/20) undriven, to the bit in L/R") {
	// THE INDEPENDENT RESTATEMENT: Drive multiplies the input before the detector and before the gain, so driving
	// x by D renders exactly what an undriven device renders from x already scaled by D's gain (at Mix 100%,
	// where nothing of the undriven dry is heard). The gain is the device's own float formula, `exp(D · ln10/20)`.
	constexpr float kDriveDb = 9.0f;
	const float d = std::exp(kDriveDb * 0.11512925464970229f);
	for (const comp::StereoMode stereo : { comp::StereoMode::LeftRight, comp::StereoMode::MidSide }) {
		CAPTURE(static_cast<int>(stereo));
		comp::Settings undriven = lqCompress(0.5f, stereo);
		comp::Settings driven = undriven;
		driven.driveDb = kDriveDb;
		Instance a;
		Instance b;
		a.set(driven);
		b.set(undriven);
		Noise noise;
		size_t differ = 0;
		float worst = 0.0f;
		float deepestA = 0.0f;
		float deepestRef = 0.0f;
		for (int k = 0; k < kSecond / 2; k += 64) {
			std::array<float, 64> l {}, r {}, sl {}, sr {};
			for (size_t i = 0; i < 64; ++i) {
				l[i] = 0.2f * noise.next();
				r[i] = 0.2f * noise.next();
				sl[i] = l[i] * d;
				sr[i] = r[i] * d;
			}
			a.process(l.data(), r.data(), 64);
			b.process(sl.data(), sr.data(), 64);
			for (size_t i = 0; i < 64; ++i) {
				differ += (l[i] != sl[i]) + (r[i] != sr[i]);
				worst = std::max(worst, std::max(std::fabs(l[i] - sl[i]), std::fabs(r[i] - sr[i])));
			}
			deepestA = std::min(deepestA, a.gainDb());
			deepestRef = std::min(deepestRef, b.gainDb());
		}
		CAPTURE(worst);
		// NOT VACUOUS: the detector heard the drive and reduced.
		CHECK(deepestA < -3.0f);
		if (stereo == comp::StereoMode::LeftRight) {
			CHECK(differ == 0u);
			CHECK(deepestA == deepestRef);
		}
		else {
			// M/S DRIVES THE DETECTOR'S COPY AFTER ENCODING IT (Limit's order since LE, kept so Limit stays bit-identical),
			// so `encode(x)·d` against the restatement's `encode(x·d)` differs by rounding, and only by rounding.
			CHECK(worst < 1e-5f);
			CHECK(std::fabs(deepestA - deepestRef) < 1e-3f);
		}
	}
}

TEST_CASE("MI: Compress's Drive by value — DC at -30 dBFS driven +24 dB is reduced 9 dB at 4:1 over -18, and Mix 0 is still the dry") {
	comp::Settings s;
	s.thresholdDb = -18.0f;
	s.ratio = 4.0f;
	s.kneeDb = 0.0f;
	s.detect = static_cast<int>(comp::Detect::Peak);
	s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	s.driveDb = 24.0f;
	{
		Instance dev;
		dev.set(s);
		const float amp = dbToLin(-30.0);
		const float y = dev.runDc(amp, kSecond);
		// Driven to -6 dBFS: 12 dB over a -18 threshold at 4:1 is a 9 dB reduction, leaving -15 dBFS.
		CHECK(dev.gainDb() == doctest::Approx(-9.0f).epsilon(1e-3));
		CHECK(linToDb(static_cast<double>(y)) == doctest::Approx(-15.0).epsilon(1e-3));
	}
	{
		// THE DRY IS THE UNDRIVEN INPUT: Mix 0 hands back x, not x pushed by the drive.
		s.mix = 0.0f;
		Instance dev;
		dev.set(s);
		const float amp = dbToLin(-30.0);
		CHECK(dev.runDc(amp, kSecond) == amp);
		CHECK(dev.gainDb() < -8.0f); // not vacuous: the drive still reached the detector
	}
}

namespace {

//------------------------------------------------------------------------------------------------//
// JO — DETECTOR: SELF OR SIDE                                                                     //
//------------------------------------------------------------------------------------------------//

/// A HARD, INSTANT COMPRESSOR, so a steady state is a closed form: threshold -18 dB, 4:1, no knee, instant attack.
[[nodiscard]] comp::Settings hardComp() {
	comp::Settings s;
	s.thresholdDb = -18.0f;
	s.ratio = 4.0f;
	s.kneeDb = 0.0f;
	s.attack = static_cast<float>(std::log2(comp::kAttackMinSeconds));
	return s;
}

TEST_CASE("JO: Side ducks a quiet steady input by exactly what the loud sidechain needs — and Self does not") {
	// INPUT -30 dBFS (under the threshold: Self leaves it alone). SIDECHAIN -6 dBFS: 12 dB over -18 at 4:1 is 9 dB of reduction.
	comp::Settings s = hardComp();
	const float in = dbToLin(-30.0), side = dbToLin(-6.0);
	{
		s.detector = static_cast<int>(comp::Detector::Side);
		Instance dev;
		dev.set(s);
		const float y = dev.runDcSide(in, side, kSecond);
		CHECK(dev.gainDb() == doctest::Approx(-9.0f).epsilon(1e-3));
		CHECK(linToDb(static_cast<double>(y)) == doctest::Approx(-39.0).epsilon(1e-3)); // the INPUT, 9 dB down
		CHECK(dev.detectorDb() == doctest::Approx(-6.0f).epsilon(1e-3));
	}
	{
		s.detector = static_cast<int>(comp::Detector::Self);
		Instance dev;
		dev.set(s);
		const float y = dev.runDcSide(in, side, kSecond); // the same sidechain is handed, and ignored
		CHECK(dev.gainDb() == 0.0f);
		CHECK(y == in);
	}
}

TEST_CASE("JO: Side listens to the sidechain only — a loud input over a silent sidechain compresses nothing") {
	comp::Settings s = hardComp();
	s.detector = static_cast<int>(comp::Detector::Side);
	const float loud = dbToLin(-6.0);
	for (const bool connected : { false, true }) {
		CAPTURE(connected);
		Instance dev;
		dev.set(s);
		// Not connected hands null buffers; connected-but-silent hands zeros. Neither reduces anything.
		const float y = dev.runDcSide(loud, 0.0f, kSecond, connected);
		CHECK(dev.gainDb() == 0.0f);
		CHECK(y == loud);
		CHECK(dev.reading(comp::Reading::SidechainSounding) == 0.0f);
	}
	// AND THE SOUNDING FLAG REACHES THE READING, so the card can say "sidechain silent".
	Instance dev;
	dev.set(s);
	dev.runDcSide(loud, dbToLin(-6.0), 2048);
	CHECK(dev.reading(comp::Reading::SidechainSounding) == 1.0f);
}

TEST_CASE("JO: Self is the device it was — Detector Self with a sidechain handed renders bit-identical to no sidechain at all") {
	Noise n;
	std::vector<float> inL(8192), inR(8192), sc(8192);
	for (size_t i = 0; i < inL.size(); ++i) {
		inL[i] = 0.5f * n.next();
		inR[i] = 0.5f * n.next();
		sc[i] = n.next();
	}
	comp::Settings s;
	s.detect = static_cast<int>(comp::Detect::Rms);
	s.scHpf = 3;
	std::vector<float> aL = inL, aR = inR, bL = inL, bR = inR;
	{
		Instance dev;
		dev.set(s);
		for (size_t at = 0; at < aL.size(); at += 512) {
			dev.process(aL.data() + at, aR.data() + at, 512);
		}
	}
	{
		Instance dev;
		dev.set(s);
		for (size_t at = 0; at < bL.size(); at += 512) {
			dev.setSidechain(sc.data() + at, sc.data() + at, 512, true);
			dev.process(bL.data() + at, bR.data() + at, 512);
		}
	}
	CHECK(std::memcmp(aL.data(), bL.data(), aL.size() * sizeof(float)) == 0);
	CHECK(std::memcmp(aR.data(), bR.data(), aR.size() * sizeof(float)) == 0);
}

TEST_CASE("JO: Limit never reads the sidechain — Detector Side in Limit holds the ceiling on its own audio") {
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Limit);
	s.ceilingDb = -6.0f;
	s.detector = static_cast<int>(comp::Detector::Side);
	Instance dev;
	dev.set(s);
	const float y = dev.runDcSide(dbToLin(-1.0), 0.0f, kSecond); // silent sidechain; the audio is over the ceiling
	CHECK(dev.gainDb() < -4.0f);
	CHECK(y <= dbToLin(-6.0) * 1.0001f);
}

TEST_CASE("JO: the Detector control is live in Compress and Compander, never Limit, and its choices are Self and Side") {
	const auto& d = comp::kParamDescs[static_cast<size_t>(comp::Param::Detector)];
	CHECK(d.gateControl == static_cast<int16_t>(comp::Param::Mode));
	CHECK(d.gateMask == comp::kCompressOrCompander);
	CHECK(std::string(d.names[0]) == "Self");
	CHECK(std::string(d.names[1]) == "Side");
	CHECK(d.def == 0.0);
	comp::Settings s;
	s.detector = static_cast<int>(comp::Detector::Side);
	CHECK(comp::stateOf(comp::rowOf(s), comp::Row {}.size()) == s);
}

//------------------------------------------------------------------------------------------------//
// THE COMPANDER (backlog QG)                                                                      //
//------------------------------------------------------------------------------------------------//

/// THE COMPANDER'S STEADY-STATE GAIN IN dB, RESTATED BY HAND: the line of slope 1/R through Ref is a gain of
/// `(1/R - 1)(L - Ref)` dB, clamped to +-range. Not `companderCurveDb`, which is the code under test's own.
[[nodiscard]] double companderGainDb(double levelDb, double refDb, double ratio, double rangeDb) {
	return std::clamp((1.0 / ratio - 1.0) * (levelDb - refDb), -rangeDb, rangeDb);
}

/// COMPANDER SETTINGS WITH AN INSTANT ATTACK AND RELEASE, so a DC level is the envelope after one sample.
[[nodiscard]] comp::Settings companderInstant(float refDb, float ratio, float rangeDb) {
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Compander);
	s.thresholdDb = refDb;
	s.cmpRatio = ratio;
	s.cmpRangeDb = rangeDb;
	s.attack = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Attack)].min);
	s.release = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Release)].min);
	return s;
}

TEST_CASE("QG compander: the Mode has a third choice, Compander, and its own Ratio and Range are gated to it") {
	const auto& mode = comp::kParamDescs[static_cast<size_t>(comp::Param::Mode)];
	REQUIRE(mode.max == 2.0);
	CHECK(std::string(mode.names[2]) == "Compander");
	for (const comp::Param p : { comp::Param::CmpRatio, comp::Param::CmpRange }) {
		const auto& d = comp::kParamDescs[static_cast<size_t>(p)];
		CHECK(d.gateMask == comp::kCompanderOnly);
		CHECK(feathers::device::relevantFor(d, 2.0));
		CHECK_FALSE(feathers::device::relevantFor(d, 0.0));
		CHECK_FALSE(feathers::device::relevantFor(d, 1.0));
	}
	// THE HONESTY RULE: Compress's curve and Limit's ceiling are hidden in Compander; Ref, the times, Detect, Makeup,
	// Mix, Detector are live.
	for (const comp::Param p : { comp::Param::Ratio, comp::Param::Knee, comp::Param::Ceiling }) {
		CHECK_FALSE(feathers::device::relevantFor(comp::kParamDescs[static_cast<size_t>(p)], 2.0));
	}
	for (const comp::Param p : { comp::Param::Threshold, comp::Param::Attack, comp::Param::Release, comp::Param::Detect,
	                             comp::Param::Makeup, comp::Param::Mix, comp::Param::Detector, comp::Param::Drive,
	                             comp::Param::ScHpf, comp::Param::StereoLink, comp::Param::StereoMode }) {
		CAPTURE(static_cast<int>(p));
		CHECK(feathers::device::relevantFor(comp::kParamDescs[static_cast<size_t>(p)], 2.0));
	}
	// THE DEFAULTS ARE THE CHIP'S: 2:1, and 12 dB, Buckets' 0.25..4.
	CHECK(comp::Settings {}.cmpRatio == 2.0f);
	CHECK(comp::Settings {}.cmpRangeDb == 12.0f);
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Compander);
	s.cmpRatio = 3.25f;
	s.cmpRangeDb = 17.0f;
	CHECK(comp::stateOf(comp::rowOf(s), comp::Row {}.size()) == s);
}

TEST_CASE("QG compander: the static transfer is R:1 about Ref, both ways — loud down, quiet up — at 1.5, 2 and 4") {
	for (const float ratio : { 2.0f, 1.5f, 4.0f }) {
		for (const float refDb : { -18.0f, -30.0f }) {
			for (int level = -42; level <= 0; level += 6) {
				CAPTURE(ratio);
				CAPTURE(refDb);
				CAPTURE(level);
				Instance dev;
				dev.set(companderInstant(refDb, ratio, 24.0f));
				const float amp = dbToLin(level);
				const float y = dev.runDc(amp, kSecond / 4);
				const double want = companderGainDb(level, refDb, ratio, 24.0);
				// THE GAIN THE DSP APPLIES, BY VALUE, and the output it makes (makeup 0, mix 1).
				CHECK(std::fabs(dev.gainDb(0) - want) < 0.01);
				CHECK(std::fabs(linToDb(y) - (level + want)) < 0.01);
			}
		}
	}
	// THE CHIP'S OWN NUMBERS, SPELLED: at 2:1 about -18, -6 in leaves at -12 and -30 leaves at -24; Ref is left alone.
	Instance dev;
	dev.set(companderInstant(-18.0f, 2.0f, 12.0f));
	CHECK(std::fabs(linToDb(dev.runDc(dbToLin(-6.0), kSecond / 4)) - (-12.0)) < 0.01);
	CHECK(std::fabs(linToDb(dev.runDc(dbToLin(-30.0), kSecond / 4)) - (-24.0)) < 0.01);
	CHECK(std::fabs(linToDb(dev.runDc(dbToLin(-18.0), kSecond / 4)) - (-18.0)) < 0.01);
}

TEST_CASE("QG compander: Range is the most it cuts or boosts") {
	for (const float range : { 6.0f, 12.0f, 24.0f }) {
		CAPTURE(range);
		// FAR UNDER REF: the boost stops at +range.
		{
			Instance dev;
			dev.set(companderInstant(-12.0f, 4.0f, range));
			const float y = dev.runDc(dbToLin(-72.0), kSecond / 4);
			CHECK(std::fabs(dev.gainDb(0) - range) < 0.01f);
			CHECK(std::fabs(linToDb(y) - (-72.0 + range)) < 0.01);
		}
		// FAR OVER IT (Drive pushes the detector and the audio alike): the cut stops at -range.
		{
			comp::Settings s = companderInstant(-48.0f, 4.0f, range);
			s.driveDb = 24.0f;
			Instance dev;
			dev.set(s);
			const float y = dev.runDc(dbToLin(-6.0), kSecond / 4);
			CHECK(std::fabs(dev.gainDb(0) + range) < 0.01f);
			CHECK(std::fabs(linToDb(y) - (-6.0 + 24.0 - range)) < 0.01);
		}
	}
}

TEST_CASE("QG compander: the follower's attack and release are one-poles with the controls' time constants") {
	// A LINEAR FOLLOWER: after one time constant of a step it has covered 1 - 1/e of the way, attack up and release
	// down. `(1 - a)^(t sr) = e^-1` exactly when `a = 1 - e^(-1/(t sr))`, the chip's coefficient.
	for (const double attackS : { 0.003, 0.010, 0.050 }) {
		for (const double releaseS : { 0.060, 0.300 }) {
			CAPTURE(attackS);
			CAPTURE(releaseS);
			comp::Settings s;
			s.mode = static_cast<int>(comp::Mode::Compander);
			s.attack = static_cast<float>(std::log2(attackS));
			s.release = static_cast<float>(std::log2(releaseS));
			Instance dev;
			dev.set(s);
			const int na = static_cast<int>(std::lround(attackS * kSampleRate));
			dev.runDc(0.5f, na, 1); // one sample a block, so the count is exact whatever the partition
			const float e = std::exp(-1.0f);
			CHECK(std::fabs(comp::envelopeForTest(dev.storageForTest(), 0) - 0.5f * (1.0f - e)) < 2e-3f);
			// SETTLE AT 0.5, THEN FALL TO 0.05 FOR ONE RELEASE TIME CONSTANT.
			dev.runDc(0.5f, kSecond);
			REQUIRE(std::fabs(comp::envelopeForTest(dev.storageForTest(), 0) - 0.5f) < 1e-4f);
			const int nr = static_cast<int>(std::lround(releaseS * kSampleRate));
			dev.runDc(0.05f, nr, 1);
			CHECK(std::fabs(comp::envelopeForTest(dev.storageForTest(), 0) - (0.05f + 0.45f * e)) < 2e-3f);
		}
	}
}

TEST_CASE("QG compander: Link 0 gives each channel its own gain; Link 100% follows the louder, as Buckets does") {
	comp::Settings s = companderInstant(-18.0f, 2.0f, 24.0f);
	s.stereoLink = 0.0f;
	{
		Instance dev;
		dev.set(s);
		dev.runDc(dbToLin(-6.0), dbToLin(-30.0), kSecond / 4);
		CHECK(std::fabs(dev.gainDb(0) - companderGainDb(-6.0, -18.0, 2.0, 24.0)) < 0.01f);
		CHECK(std::fabs(dev.gainDb(1) - companderGainDb(-30.0, -18.0, 2.0, 24.0)) < 0.01f);
	}
	s.stereoLink = 1.0f;
	{
		Instance dev;
		dev.set(s);
		dev.runDc(dbToLin(-6.0), dbToLin(-30.0), kSecond / 4);
		CHECK(std::fabs(dev.gainDb(0) - companderGainDb(-6.0, -18.0, 2.0, 24.0)) < 0.01f);
		CHECK(dev.gainDb(1) == dev.gainDb(0));
	}
}

TEST_CASE("QG compander: makeup and mix act on its gain as they do on Compress's") {
	comp::Settings s = companderInstant(-18.0f, 2.0f, 12.0f);
	s.makeupDb = 3.0f;
	s.mix = 0.5f;
	Instance dev;
	dev.set(s);
	const float x = dbToLin(-6.0);
	const float y = dev.runDc(x, kSecond / 4);
	const double wet = x * std::pow(10.0, (companderGainDb(-6.0, -18.0, 2.0, 12.0) + 3.0) / 20.0);
	CHECK(std::fabs(y - (0.5 * x + 0.5 * wet)) < 1e-4);
}

TEST_CASE("QG compander: its tail reaches exact zero — at the widest boost — and the follower with it") {
	// THE HARDEST ROW FOR A TAIL: Range 24, so the gain as the envelope falls is the most it ever lifts anything.
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Compander);
	s.cmpRangeDb = 24.0f;
	s.cmpRatio = 4.0f;
	s.release = static_cast<float>(std::log2(1.0));
	Instance dev;
	dev.set(s);
	Noise noise;
	std::vector<float> l(512), r(512);
	for (int b = 0; b < 40; ++b) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.5f * noise.next();
			r[i] = 0.5f * noise.next();
		}
		dev.process(l.data(), r.data(), 512);
	}
	REQUIRE(std::fabs(l[100]) > 0.0f);
	int lastNonZero = -1;
	for (int b = 0; b < 2500; ++b) { // 26 s: a 1 s release falls from 0.5 under 1e-7 in ~15.4
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		dev.process(l.data(), r.data(), 512);
		for (size_t i = 0; i < l.size(); ++i) {
			if (l[i] != 0.0f || r[i] != 0.0f) {
				lastNonZero = b;
			}
		}
	}
	// THE LATENCY'S WORTH OF SOUND LEAVES IN THE FIRST SILENT BLOCK; NOTHING AFTER IT.
	CHECK(lastNonZero == 0);
	// THE FOLLOWER IS EXACTLY 0 TOO (it flushes under 1e-7), so nothing it holds is a denormal parked for ever.
	CHECK(comp::envelopeForTest(dev.storageForTest(), 0) == 0.0f);
	CHECK(comp::envelopeForTest(dev.storageForTest(), 1) == 0.0f);
}

//------------------------------------------------------------------------------------------------//
// THE TOPOLOGIES, LINK AND THE LANE (backlog QG stage 2)                                          //
//------------------------------------------------------------------------------------------------//

/// A COMP AS A HOST HOLDS ONE — the container adopted with the host's catalogue, so its lane can hold devices
/// (`test_delay_lane.cpp`'s `Device`).
struct Held {
	const feathers::device::AudioOps& audio = *comp::kDescriptor.audio;
	const feathers::device::ContainerOps& container = *comp::kDescriptor.container;
	void* mem = nullptr;
	comp::Row row {};

	explicit Held(const comp::Settings& s) {
		mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(mem);
		container.adopt(mem, feathers::hostCatalogue());
		audio.prepare(mem, kSampleRate, 4096);
		set(s);
	}
	~Held() {
		audio.destruct(mem);
		::operator delete(mem, std::align_val_t { audio.stateAlign });
	}
	Held(const Held&) = delete;
	Held& operator=(const Held&) = delete;

	void set(const comp::Settings& s) {
		row = comp::rowOf(s);
		audio.setFields(mem, row.data(), row.size());
	}
	feathers::rack::RackBridge& lane() const {
		feathers::rack::InnerChain* chain = comp::ensureCompanderLane(mem);
		REQUIRE(chain != nullptr);
		REQUIRE(chain->bridge() != nullptr);
		return *chain->bridge();
	}
	feathers::DeviceInstanceId add(const char* tag) const {
		const feathers::DeviceInstanceId id = lane().addDevice(tag);
		REQUIRE(id != feathers::kNoInstance);
		return id;
	}
	/// THE HOST'S RESTART AFTER A LATENCY CHANGE: prepare again, which sizes the pads for the lane's latency.
	void restart() { audio.prepare(mem, kSampleRate, 4096); }
	[[nodiscard]] uint32_t latency() const { return audio.latencySamples(mem, row.data(), row.size(), kSampleRate); }
	std::vector<float> render(const std::vector<float>& in, int block = 256) const {
		std::vector<float> l = in;
		std::vector<float> r = in;
		for (size_t at = 0; at < l.size(); at += static_cast<size_t>(block)) {
			const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - at));
			audio.processStereo(mem, l.data() + at, r.data() + at, n);
		}
		return l;
	}
	/// DC ON BOTH CHANNELS; the last output sample.
	float runDc(float amp, int frames, int block = 256) const {
		std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
		float last = 0.0f;
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			std::fill(l.begin(), l.end(), amp);
			std::fill(r.begin(), r.end(), amp);
			audio.processStereo(mem, l.data(), r.data(), n);
			last = l[static_cast<size_t>(n - 1)];
		}
		return last;
	}
	[[nodiscard]] float first(size_t ch = 0) const { return comp::firstHalfDbForTest(mem, ch); }
	[[nodiscard]] float second(size_t ch = 0) const { return comp::secondHalfDbForTest(mem, ch); }
};

size_t controlNamed(const feathers::device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	FAIL("no control " << name);
	return 0;
}

comp::Settings companderTopology(comp::Topology t, float refDb = -18.0f, float ratio = 2.0f, float rangeDb = 12.0f) {
	comp::Settings s;
	s.mode = static_cast<int>(comp::Mode::Compander);
	s.topology = static_cast<int>(t);
	s.thresholdDb = refDb;
	s.cmpRatio = ratio;
	s.cmpRangeDb = rangeDb;
	return s;
}

TEST_CASE("QG topology: the choice is four on the face, stored by label, gated to Compander; Link and the second half "
          "nest under it") {
	const auto& t = comp::kParamDescs[static_cast<size_t>(comp::Param::Topology)];
	REQUIRE(t.max == 3.0);
	CHECK(std::string(t.names[0]) == "Compress");
	CHECK(std::string(t.names[1]) == "Expand");
	CHECK(std::string(t.names[2]) == "Compander");
	CHECK(std::string(t.names[3]) == "Excomper");
	CHECK(t.gateMask == comp::kCompanderOnly);
	// THE NESTED GATE, asked of the one function the card and the window ask (`liveIn`).
	comp::Row row = comp::rowOf(comp::Settings {});
	const auto field = [&](size_t i) { return static_cast<double>(row[i]); };
	const auto live = [&](comp::Param p) { return comp::liveIn(p, field); };
	CHECK_FALSE(live(comp::Param::Topology)); // Compress mode
	row[static_cast<size_t>(comp::Param::CmpLink)] = 0.0f;
	CHECK_FALSE(live(comp::Param::CmpRatio2)); // Link off is not enough: Mode is Compress
	row[static_cast<size_t>(comp::Param::Mode)] = 2.0f;
	CHECK(live(comp::Param::Topology));
	CHECK_FALSE(live(comp::Param::CmpLink)); // topology Compress: single-ended
	CHECK_FALSE(live(comp::Param::CmpRatio2));
	for (const comp::Topology paired : { comp::Topology::Compander, comp::Topology::Excomper }) {
		row[static_cast<size_t>(comp::Param::Topology)] = static_cast<float>(paired);
		CHECK(live(comp::Param::CmpLink));
		CHECK(live(comp::Param::CmpRatio2));
		CHECK(live(comp::Param::CmpAttack2));
		CHECK(live(comp::Param::CmpRelease2));
		row[static_cast<size_t>(comp::Param::CmpLink)] = 1.0f;
		CHECK_FALSE(live(comp::Param::CmpRatio2));
		row[static_cast<size_t>(comp::Param::CmpLink)] = 0.0f;
	}
	// DEFAULTS: single-ended Compress (stage 1's sound), Link on, the second half's three at the first half's defaults.
	CHECK(comp::Settings {}.topology == static_cast<int>(comp::Topology::Compress));
	CHECK(comp::Settings {}.cmpLink);
	comp::Settings s;
	s.topology = static_cast<int>(comp::Topology::Excomper);
	s.cmpLink = false;
	s.cmpRatio2 = 3.5f;
	s.cmpAttack2 = -8.0f;
	s.cmpRelease2 = -2.0f;
	CHECK(comp::stateOf(comp::rowOf(s), comp::Row {}.size()) == s);
}

TEST_CASE("QG topology: Expand is 1:R about Ref — its gain is (R - 1)(L - Ref), clamped to +-Range") {
	for (const float ratio : { 1.5f, 2.0f, 4.0f }) {
		for (int level = -36; level <= 0; level += 6) {
			CAPTURE(ratio);
			CAPTURE(level);
			comp::Settings s = companderTopology(comp::Topology::Expand, -18.0f, ratio, 24.0f);
			s.attack = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Attack)].min);
			s.release = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Release)].min);
			Held dev(s);
			const float y = dev.runDc(dbToLin(level), kSecond / 4);
			const double want = std::clamp((ratio - 1.0) * (level + 18.0), -24.0, 24.0);
			CHECK(std::fabs(dev.first() - want) < 0.01);
			CHECK(dev.second() == 0.0f); // single-ended: no second half
			CHECK(std::fabs(linToDb(std::fabs(y)) - (level + want)) < 0.01);
		}
	}
}

TEST_CASE("QG topology: with an empty lane, Compander and Excomper give back what went in — the halves cancel to "
          "within 0.01 dB at steady state, across the range and its clamp") {
	for (const comp::Topology t : { comp::Topology::Compander, comp::Topology::Excomper }) {
		for (const float ratio : { 1.5f, 2.0f, 4.0f }) {
			for (const float range : { 12.0f, 24.0f }) {
				for (int level = -60; level <= 0; level += 6) {
					CAPTURE(static_cast<int>(t));
					CAPTURE(ratio);
					CAPTURE(range);
					CAPTURE(level);
					Held dev(companderTopology(t, -18.0f, ratio, range));
					const float x = dbToLin(level);
					const float y = dev.runDc(x, kSecond);
					// EACH HALF DID SOMETHING (unless at Ref) — not vacuous — and the two cancel.
					if (level != -18) {
						CHECK(std::fabs(dev.first()) > 0.5f);
					}
					CHECK(std::fabs(dev.first() + dev.second()) < 0.01f);
					CHECK(std::fabs(linToDb(y / x)) < 0.01);
				}
			}
		}
	}
}

/// THE WORST NET GAIN (dB, either sign) A PAIRED TOPOLOGY APPLIES TO A LEVEL-STEPPED NOISE WITH AN EMPTY LANE, sample by
/// sample, read from the seams — the DETECTOR ASYMMETRY: the second half's follower hears the first half's output, not
/// the input, so while either follower is moving the two gains do not cancel exactly.
float worstTransientDb(comp::Topology t, float ratio, float attackS, float releaseS) {
	comp::Settings s = companderTopology(t, -18.0f, ratio, 12.0f);
	s.attack = static_cast<float>(std::log2(attackS));
	s.release = static_cast<float>(std::log2(releaseS));
	Held dev(s);
	Noise noise;
	std::vector<float> l(1), r(1);
	float worst = 0.0f;
	for (int i = 0; i < 4 * kSecond; ++i) {
		const int phase = (i / (kSecond / 4)) % 4;
		const float amp = phase == 0 ? 0.5f : phase == 1 ? 0.02f : phase == 2 ? 0.2f : 0.005f;
		l[0] = r[0] = amp * noise.next();
		dev.audio.processStereo(dev.mem, l.data(), r.data(), 1);
		if (i > kSecond / 4) {
			worst = std::max(worst, std::fabs(dev.first() + dev.second()));
		}
	}
	return worst;
}

TEST_CASE("QG topology: the detector asymmetry is designed out — an empty-lane pair cancels SAMPLE BY SAMPLE on level-stepped "
          "noise, in every detector setting, within 1e-4 dB") {
	// THE FIRST DRAFT (both halves feed-forward) MEASURED a worst of 7.5 dB (Compander) and 8.9 dB (Excomper) here, a mean
	// of 2.4 dB: the second follower heard `g1 * x`. With the second half following its own output (comp.cpp's banner)
	// both followers are fed the same samples, and what is left is float rounding — measured 3e-6 dB.
	for (const comp::Topology t : { comp::Topology::Compander, comp::Topology::Excomper }) {
		for (const float ratio : { 1.5f, 4.0f }) {
			const float worst = worstTransientDb(t, ratio, 0.010f, 0.100f);
			MESSAGE("topology " << static_cast<int>(t) << " ratio " << ratio << ": worst net gain " << worst << " dB");
			CHECK(worst < 1e-4f);
		}
		// "INSTANT" AT RATIO 4: Excomper's second half is a feedback compressor, stable only below a 1/R step — the cap keeps
		// it there (without it this oscillates and the pair stops cancelling).
		const float instant = worstTransientDb(t, 4.0f, 0.0001f, 0.001f);
		MESSAGE("topology " << static_cast<int>(t) << " instant times: worst net gain " << instant << " dB");
		CHECK(instant < 1e-4f);
	}
	// AND THE OUTPUT IS THE INPUT, late by the device's latency, through RMS, the SC HPF, M/S and a partial Link.
	for (const comp::Topology t : { comp::Topology::Compander, comp::Topology::Excomper }) {
		comp::Settings s = companderTopology(t, -24.0f, 3.0f, 18.0f);
		s.detect = static_cast<int>(comp::Detect::Rms);
		s.scHpf = 4;
		s.stereoMode = static_cast<int>(comp::StereoMode::MidSide);
		s.stereoLink = 0.4f;
		s.attack = static_cast<float>(std::log2(0.002));
		Held dev(s);
		Noise noise;
		const size_t frames = static_cast<size_t>(kSecond);
		std::vector<float> l(frames), r(frames);
		for (size_t i = 0; i < frames; ++i) {
			const float amp = (i / 6000) % 2 == 0 ? 0.6f : 0.01f;
			l[i] = amp * noise.next();
			r[i] = 0.5f * amp * noise.next();
		}
		const std::vector<float> inL = l, inR = r;
		for (size_t at = 0; at < frames; at += 300) {
			const int n = static_cast<int>(std::min<size_t>(300, frames - at));
			dev.audio.processStereo(dev.mem, l.data() + at, r.data() + at, n);
		}
		const size_t late = dev.latency();
		float worst = 0.0f;
		for (size_t i = late; i < frames; ++i) {
			worst = std::max({ worst, std::fabs(l[i] - inL[i - late]), std::fabs(r[i] - inR[i - late]) });
		}
		MESSAGE("topology " << static_cast<int>(t) << ": worst |out - in| " << worst);
		CHECK(worst < 1e-5f);
	}
}

TEST_CASE("QG Link: unlinked, the second half runs on its own ratio — and at the first half's values it is the linked "
          "device to the bit") {
	// NET STEADY GAIN WITH Ratio 2 for the compressor and Ratio2 4 for the expander: the compressor leaves L at
	// Ref + (L - Ref)/2, and the expander at ratio 4 lifts that by 3 x its distance from Ref.
	comp::Settings s = companderTopology(comp::Topology::Compander, -18.0f, 2.0f, 24.0f);
	s.cmpLink = false;
	s.cmpRatio2 = 4.0f;
	Held dev(s);
	dev.runDc(dbToLin(-6.0), kSecond);
	CHECK(std::fabs(dev.first() - (-6.0f)) < 0.01f);
	CHECK(std::fabs(dev.second() - 3.0f * 6.0f) < 0.01f);
	// AT THE FIRST HALF'S OWN VALUES, UNLINKED IS LINKED, sample for sample (what unlinking from the UI starts at).
	comp::Settings linked = companderTopology(comp::Topology::Excomper, -24.0f, 3.0f, 18.0f);
	linked.attack = static_cast<float>(std::log2(0.004));
	linked.release = static_cast<float>(std::log2(0.25));
	comp::Settings unlinked = linked;
	unlinked.cmpLink = false;
	unlinked.cmpRatio2 = linked.cmpRatio;
	unlinked.cmpAttack2 = linked.attack;
	unlinked.cmpRelease2 = linked.release;
	Noise n1;
	std::vector<float> in(kSecond);
	for (float& v : in) {
		v = 0.3f * n1.next();
	}
	Held a(linked);
	Held b(unlinked);
	const std::vector<float> ya = a.render(in, 300);
	const std::vector<float> yb = b.render(in, 300);
	CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
	// AND THE UNLINKED TIMES REACH THE SECOND FOLLOWER: after a 30 dB drop, a 1.5 s release2 holds its envelope far above an
	// instant one's.
	const auto envAfterDrop = [](float release2) {
		comp::Settings u = companderTopology(comp::Topology::Compander, -18.0f, 2.0f, 24.0f);
		u.cmpLink = false;
		u.cmpRelease2 = release2;
		Held d(u);
		d.runDc(0.5f, kSecond);
		d.runDc(0.0158f, kSecond / 20);
		return comp::secondEnvelopeForTest(d.mem, 0);
	};
	const float slow = envAfterDrop(static_cast<float>(std::log2(1.5)));
	const float fast = envAfterDrop(static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::CmpRelease2)].min));
	MESSAGE("second envelope 50 ms after the drop: slow release2 " << slow << ", instant " << fast);
	CHECK(slow > 2.0f * fast);
}

TEST_CASE("QG lane: unbuilt and built-but-empty are the same bits, and the lane's devices are heard only in Compander") {
	comp::Settings s = companderTopology(comp::Topology::Compress);
	Noise noise;
	std::vector<float> in(kSecond / 2);
	for (float& v : in) {
		v = 0.4f * noise.next();
	}
	Held bare(s);
	const std::vector<float> want = bare.render(in);
	Held built(s);
	(void)built.lane();
	CHECK(comp::companderLaneOf(built.mem) != nullptr);
	const std::vector<float> empty = built.render(in);
	CHECK(std::memcmp(empty.data(), want.data(), want.size() * sizeof(float)) == 0);

	// A BUS UTILITY AT -6 dB IN THE LANE: heard after the compress half, by value, at steady DC.
	Held dev(s);
	const auto util = dev.add(fx::busutil::kDescriptor.traits.tag);
	dev.lane().setField(util, controlNamed(fx::busutil::kDescriptor, "Bus Gain"), 0.3f);
	const float g = fx::busutil::faderGain(0.3f);
	Held ref(s);
	const float yRef = ref.runDc(0.25f, kSecond);
	const float y = dev.runDc(0.25f, kSecond);
	CHECK(std::fabs(y - yRef * g) < 1e-3f * std::fabs(yRef));
	// SWITCHED AWAY TO COMPRESS MODE: the device is KEPT, and NOT RUN — the output is plain Compress.
	comp::Settings away = s;
	away.mode = static_cast<int>(comp::Mode::Compress);
	dev.set(away);
	Held plain(away);
	const float yPlain = plain.runDc(0.25f, kSecond);
	CHECK(dev.runDc(0.25f, kSecond) == doctest::Approx(yPlain).epsilon(1e-6));
	CHECK(comp::companderLaneOf(dev.mem)->engine().chainLength(0) == 1);
	// AND BACK: heard again.
	dev.set(s);
	CHECK(std::fabs(dev.runDc(0.25f, kSecond) - yRef * g) < 1e-3f * std::fabs(yRef));
}

TEST_CASE("QG lane: a LATENT device in the lane is the device's latency in EVERY mode, and the output arrives exactly then") {
	for (const comp::Mode mode : { comp::Mode::Compress, comp::Mode::Limit, comp::Mode::Compander }) {
		CAPTURE(static_cast<int>(mode));
		comp::Settings s = companderTopology(comp::Topology::Compress);
		s.mode = static_cast<int>(mode);
		s.mix = 0.0f; // the dry alone in Compress and Compander: what arrives is the alignment, not a gain
		Held dev(s);
		const auto clip = dev.add(fx::clip::kDescriptor.traits.tag);
		dev.lane().setField(clip, controlNamed(fx::clip::kDescriptor, "Clip Oversample"), 1.0f);
		(void)dev.render(std::vector<float>(600, 0.0f));
		const uint32_t laneLate = comp::companderLaneOf(dev.mem)->latencySamples(kSampleRate);
		REQUIRE(laneLate == fx::clip::kOversampleLatency);
		CHECK(dev.latency() == comp::latencySamplesFor(kSampleRate) + laneLate);
		dev.restart(); // the host's answer to a latency change: the pads are sized now
		std::vector<float> in(1024, 0.0f);
		in[10] = 0.01f;
		const std::vector<float> out = dev.render(in, 300);
		size_t at = 0;
		for (size_t i = 0; i < out.size(); ++i) {
			if (std::fabs(out[i]) > std::fabs(out[at])) {
				at = i;
			}
		}
		CHECK(at == 10 + dev.latency());
	}
	// AND THE WET THROUGH THE LANE lands there too: Compander at Mix 100%, an impulse under Ref (lifted, not moved).
	comp::Settings s = companderTopology(comp::Topology::Compress);
	Held dev(s);
	const auto clip = dev.add(fx::clip::kDescriptor.traits.tag);
	dev.lane().setField(clip, controlNamed(fx::clip::kDescriptor, "Clip Oversample"), 1.0f);
	(void)dev.render(std::vector<float>(600, 0.0f));
	dev.restart();
	std::vector<float> in(1024, 0.0f);
	in[10] = 0.01f;
	const std::vector<float> out = dev.render(in, 300);
	size_t at = 0;
	for (size_t i = 0; i < out.size(); ++i) {
		if (std::fabs(out[i]) > std::fabs(out[at])) {
			at = i;
		}
	}
	CHECK(at == 10 + dev.latency());
}

TEST_CASE("QG lane: the body carries the lane — a fresh instance loading it builds the lane with the device") {
	const comp::Settings s = companderTopology(comp::Topology::Compress);
	std::vector<uint8_t> body;
	{
		Held dev(s);
		(void)dev.add(fx::busutil::kDescriptor.traits.tag);
		(void)dev.render(std::vector<float>(600, 0.0f));
		const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
			                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
			                                       out->insert(out->end(), d, d + n);
		                                       } };
		REQUIRE(dev.container.saveState(dev.mem, sink));
	}
	Held fresh(s);
	CHECK(comp::companderLaneOf(fresh.mem) == nullptr);
	REQUIRE(fresh.container.loadState(fresh.mem, body.data(), body.size()));
	REQUIRE(comp::companderLaneOf(fresh.mem) != nullptr);
	(void)fresh.render(std::vector<float>(600, 0.0f));
	CHECK(comp::companderLaneOf(fresh.mem)->engine().chainLength(0) == 1);
}

TEST_CASE("QG lane: a reverb in the lane keeps COMP awake in Compander until the OUTPUT is exactly silent; in Compress "
          "it is not run and holds nothing") {
	Held dev(companderTopology(comp::Topology::Compander));
	(void)dev.add("FVERB");
	(void)dev.render(std::vector<float>(600, 0.0f));
	dev.restart();
	std::vector<float> l(256), r(256);
	bool fell = false;
	bool sounded = false;
	for (int block = 0; block < 48000 * 90 / 256; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		if (block < 20) {
			l[0] = r[0] = 0.5f;
		}
		dev.audio.processStereo(dev.mem, l.data(), r.data(), 256);
		for (size_t i = 0; i < 256; ++i) {
			sounded = sounded || (block > 40 && l[i] != 0.0f); // the reverb's tail, past the input
			if (fell) {
				REQUIRE(l[i] == 0.0f);
				REQUIRE(r[i] == 0.0f);
			}
		}
		if (!fell && block > 40 && !dev.audio.holdsTail(dev.mem)) {
			fell = true;
		}
	}
	CHECK(sounded);
	CHECK(fell);
	// IN COMPRESS MODE THE LANE IS NOT RUN, so it is not what keeps the device awake.
	comp::Settings away = companderTopology(comp::Topology::Compander);
	away.mode = static_cast<int>(comp::Mode::Compress);
	dev.set(away);
	CHECK_FALSE(dev.audio.holdsTail(dev.mem));
}


//------------------------------------------------------------------------------------------------//
// NEGATIVE DRIVE: the compressor's Drive goes negative as well                                 //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Drive negative: the range is -24..+24 dB around a 0 default, centred, and the ends clamp") {
	const auto& d = comp::kParamDescs[static_cast<size_t>(comp::Param::Drive)];
	CHECK(std::string(d.key) == "comp_drive");
	CHECK(d.min == -24.0);
	CHECK(d.max == 24.0);
	CHECK(d.def == 0.0);
	CHECK(d.centred);
	comp::Row row = comp::rowOf(comp::Settings {});
	row[static_cast<size_t>(comp::Param::Drive)] = -40.0f;
	CHECK(comp::stateOf(row, row.size()).driveDb == -24.0f);
	row[static_cast<size_t>(comp::Param::Drive)] = 40.0f;
	CHECK(comp::stateOf(row, row.size()).driveDb == 24.0f);
	row[static_cast<size_t>(comp::Param::Drive)] = -17.5f;
	CHECK(comp::stateOf(row, row.size()).driveDb == -17.5f);
}

/// THE STEADY OUTPUT (dB) FOR DC AT `levelDb` — the last sample after a second, instant attack and release.
double steadyOutDb(comp::Settings s, double levelDb) {
	s.attack = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Attack)].min);
	s.release = static_cast<float>(comp::kParamDescs[static_cast<size_t>(comp::Param::Release)].min);
	Instance dev;
	dev.set(s);
	return linToDb(std::fabs(static_cast<double>(dev.runDc(dbToLin(levelDb), kSecond))));
}

TEST_CASE("Drive negative: -12 dB of drive moves every mode's static transfer by -12 dB of input — Compress, Limit, Compander") {
	// THE CLAIM, restated per mode: out(x, drive -12) is out(x - 12, drive 0), because drive scales the detector and the
	// audio alike and the dry is not heard at Mix 100%. Read against an undriven device fed 12 dB less, and against the
	// curve by hand, so neither half can pass alone.
	comp::Settings compress;
	compress.thresholdDb = -18.0f;
	compress.ratio = 4.0f;
	compress.kneeDb = 0.0f;
	comp::Settings limit;
	limit.mode = static_cast<int>(comp::Mode::Limit);
	limit.ceilingDb = -6.0f;
	comp::Settings compander;
	compander.mode = static_cast<int>(comp::Mode::Compander);
	compander.thresholdDb = -18.0f;
	compander.cmpRatio = 2.0f;
	compander.cmpRangeDb = 24.0f;
	struct Mode {
		const char* name;
		comp::Settings s;
		double (*byHand)(double drivenDb);
	};
	const Mode modes[] = {
		{ "Compress", compress, [](double x) { return x <= -18.0 ? x : -18.0 + (x + 18.0) / 4.0; } },
		{ "Limit", limit, [](double x) { return std::min(x, -6.0); } },
		{ "Compander", compander, [](double x) { return x + std::clamp(-0.5 * (x + 18.0), -24.0, 24.0); } },
	};
	for (const Mode& m : modes) {
		for (const double level : { 6.0, 0.0, -6.0, -24.0 }) {
			CAPTURE(m.name);
			CAPTURE(level);
			comp::Settings driven = m.s;
			driven.driveDb = -12.0f;
			const double y = steadyOutDb(driven, level);
			const double reference = steadyOutDb(m.s, level - 12.0);
			CHECK(std::fabs(y - reference) < 0.01);
			CHECK(std::fabs(y - m.byHand(level - 12.0)) < 0.02);
		}
	}
	// NOT VACUOUS: in Limit, +6 dBFS undriven sits at the ceiling, and backed off 12 dB it passes untouched at -6.
	comp::Settings undrivenLimit = limit;
	CHECK(std::fabs(steadyOutDb(undrivenLimit, 6.0) - (-6.0)) < 0.02);
	comp::Settings backedOff = limit;
	backedOff.driveDb = -12.0f;
	CHECK(std::fabs(steadyOutDb(backedOff, 6.0) - (-6.0)) < 0.02);
	CHECK(std::fabs(steadyOutDb(backedOff, 0.0) - (-12.0)) < 0.02);
}

TEST_CASE("Drive negative: the dry is never driven — Mix 0 hands back x — and the meter's IN is the undriven input") {
	for (const comp::Mode mode : { comp::Mode::Compress, comp::Mode::Compander }) {
		CAPTURE(static_cast<int>(mode));
		comp::Settings s;
		s.mode = static_cast<int>(mode);
		s.driveDb = -24.0f;
		s.mix = 0.0f;
		Instance dev;
		dev.set(s);
		const float amp = dbToLin(-6.0);
		CHECK(dev.runDc(amp, kSecond) == amp);
		CHECK(dev.reading(comp::Reading::InPeak) == doctest::Approx(amp).epsilon(1e-6));
	}
	// AT THE BOTTOM OF THE RANGE, Compress below its threshold is a plain -24 dB: the audio was backed off with the detector.
	comp::Settings s;
	s.driveDb = -24.0f;
	Instance dev;
	dev.set(s);
	const float amp = dbToLin(-6.0);
	CHECK(std::fabs(linToDb(dev.runDc(amp, kSecond)) - (-30.0)) < 0.01);
	CHECK(dev.gainDb() == 0.0f); // -30 dBFS is under the -18 threshold: no reduction
	CHECK(dev.reading(comp::Reading::OutPeak) == doctest::Approx(dbToLin(-30.0)).epsilon(1e-4));
}

} // namespace
