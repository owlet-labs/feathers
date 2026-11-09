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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/format_value.h"
#include "fx/phaser/phaser.h"
#include "fx/phaser/phaser_response.h"
#include "fx/phaser/phaser_types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace ph = feathers::fx::phaser;

namespace {

/// A ROW WITH EVERY FIELD OFF ITS DEFAULT — so a field the codec drops or swaps cannot hide behind a
/// default that happens to equal what it should have been. Continuous controls sit 0.2-0.73 of the way up
/// their range (inside every range, off every default); stepped ones take a value other than their
/// default.
ph::Row offDefaultRow() {
	ph::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		const auto& d = ph::kParamDescs[i];
		if (d.stepped) {
			const double hi = d.max;
			row[i] = static_cast<float>(d.def < hi ? hi : d.min);
		}
		else {
			// A FRACTION OF ITS OWN PER FIELD, so two fields with equal ranges never share a value — spread over 0.2..0.73
			// by the row's width, so the table can grow (round 2 added twenty) without walking a field off its range.
			row[i] = static_cast<float>(d.min + (d.max - d.min) * (0.2 + 0.53 * static_cast<double>(i) / static_cast<double>(row.size())));
		}
		CAPTURE(d.key);
		REQUIRE(row[i] != static_cast<float>(d.def));
	}
	return row;
}

} // namespace

TEST_CASE("phaser: stored row round-trips through the codec") {
	SUBCASE("defaults: an empty row decodes to the declared defaults and encodes back to them") {
		const ph::State fresh = ph::stateOf(ph::Row {}, 0);
		CHECK(fresh == ph::State {});
		const ph::Row row = ph::rowOf(fresh);
		for (size_t i = 0; i < row.size(); ++i) {
			CAPTURE(ph::kParamDescs[i].key);
			CHECK(row[i] == static_cast<float>(ph::kParamDescs[i].def));
		}
	}
	SUBCASE("every field off its default comes back bit for bit") {
		const ph::Row in = offDefaultRow();
		const ph::Row out = ph::rowOf(ph::stateOf(in, in.size()));
		for (size_t i = 0; i < in.size(); ++i) {
			CAPTURE(ph::kParamDescs[i].key);
			CHECK(out[i] == in[i]);
		}
	}
	SUBCASE("a short row puts the absent fields at their DECLARED defaults, not at zero") {
		const ph::Row in = offDefaultRow();
		const size_t present = static_cast<size_t>(ph::Param::Mix);
		const ph::Row out = ph::rowOf(ph::stateOf(in, present));
		for (size_t i = 0; i < in.size(); ++i) {
			CAPTURE(ph::kParamDescs[i].key);
			CHECK(out[i] == (i < present ? in[i] : static_cast<float>(ph::kParamDescs[i].def)));
		}
	}
	SUBCASE("valueOf and setValueOf address the same field the row does") {
		ph::State s {};
		const ph::Row in = offDefaultRow();
		for (size_t i = 0; i < in.size(); ++i) {
			ph::setValueOf(&s, static_cast<ph::Param>(i), in[i]);
		}
		for (size_t i = 0; i < in.size(); ++i) {
			CAPTURE(ph::kParamDescs[i].key);
			CHECK(ph::valueOf(s, static_cast<ph::Param>(i)) == in[i]);
		}
	}
	SUBCASE("the stored Choice index maps to the stage count it names") {
		for (size_t i = 0; i < std::size(ph::kStageNames); ++i) {
			ph::Row row = ph::rowOf(ph::State {});
			row[static_cast<size_t>(ph::Param::Stages)] = static_cast<float>(i);
			const ph::State s = ph::stateOf(row, row.size());
			CHECK(s.settings.stages == std::stoi(ph::kStageNames[i]));
		}
	}
	SUBCASE("each row field lands in the Settings field it NAMES") {
		// THE ROUND TRIP CANNOT SEE A SYMMETRIC SWAP — rowOf and stateOf trading the same two fields encode
		// back to the same row. So every field is asserted by name. The equal-default groups are where a
		// swap would hide: Mix/LfoDepth/EnvAmount (0.5), Drive/Position/EnvSens/Rotate (0),
		// EnvOn/ThroughZero/KeyTrack (off). `offDefaultRow` gives each its own value.
		const ph::Row in = offDefaultRow();
		const ph::Settings s = ph::stateOf(in, in.size()).settings;
		const auto at = [&in](ph::Param p) { return in[static_cast<size_t>(p)]; };
		const auto flag = [&at](ph::Param p) { return at(p) >= 0.5f; };
		CHECK(s.enabled == flag(ph::Param::Enabled));
		CHECK(static_cast<float>(s.mode) == at(ph::Param::Mode));
		CHECK(s.stages == ph::kStageCounts[static_cast<size_t>(at(ph::Param::Stages))]);
		CHECK(s.centerLog2Hz == at(ph::Param::CenterHz));
		CHECK(s.delayLog2s == at(ph::Param::DelayMs));
		CHECK(s.feedback == at(ph::Param::Feedback));
		CHECK(s.dampingLog2Hz == at(ph::Param::Damping));
		CHECK(s.drive == at(ph::Param::Drive));
		CHECK(s.mix == at(ph::Param::Mix));
		CHECK(s.position == at(ph::Param::Position));
		CHECK(s.lfoOn == flag(ph::Param::LfoOn));
		CHECK(s.lfoDepth == at(ph::Param::LfoDepth));
		CHECK(static_cast<float>(s.sync) == at(ph::Param::Sync));
		CHECK(s.rateLog2Hz == at(ph::Param::RateHz));
		CHECK(s.stereoDegrees == at(ph::Param::Stereo));
		CHECK(s.envOn == flag(ph::Param::EnvOn));
		CHECK(s.envAmount == at(ph::Param::EnvAmount));
		CHECK(s.envAttackLog2s == at(ph::Param::EnvAttack));
		CHECK(s.envReleaseLog2s == at(ph::Param::EnvRelease));
		CHECK(s.envSensDb == at(ph::Param::EnvSens));
		CHECK(static_cast<float>(s.sweep) == at(ph::Param::Sweep));
		CHECK(s.throughZero == flag(ph::Param::ThroughZero));
		CHECK(s.keyTrack == flag(ph::Param::KeyTrack));
		CHECK(s.rotate == at(ph::Param::Rotate));
		// THE GROUPS REALLY ARE DISTINCT IN THIS ROW, or the named checks above prove nothing about a swap.
		CHECK(at(ph::Param::Mix) != at(ph::Param::LfoDepth));
		CHECK(at(ph::Param::Mix) != at(ph::Param::EnvAmount));
		CHECK(at(ph::Param::LfoDepth) != at(ph::Param::EnvAmount));
		CHECK(at(ph::Param::Drive) != at(ph::Param::Position));
		CHECK(at(ph::Param::Drive) != at(ph::Param::EnvSens));
		CHECK(at(ph::Param::Position) != at(ph::Param::Rotate));
		// The three off-by-default switches all read On in this row, so a swap among them is invisible —
		// flip one and re-check each against its own field.
		ph::Row flipped = in;
		flipped[static_cast<size_t>(ph::Param::ThroughZero)] = 0.0f;
		const ph::Settings f = ph::stateOf(flipped, flipped.size()).settings;
		CHECK(f.envOn);
		CHECK_FALSE(f.throughZero);
		CHECK(f.keyTrack);
	}
}

TEST_CASE("phaser: the log2 range constants are the hertz and seconds the table names") {
	// AN INDEPENDENT RESTATEMENT: each constant against std::log2 of the physical value in the param table,
	// to an ABSOLUTE tolerance (doctest's Approx adds a relative scale that would pass a wrong digit here).
	constexpr double kTol = 1e-12;
	CHECK(std::fabs(ph::kCenterLog2Min - std::log2(40.0)) < kTol);
	CHECK(std::fabs(ph::kCenterLog2Max - std::log2(12000.0)) < kTol);
	CHECK(std::fabs(ph::kCenterLog2Default - std::log2(800.0)) < kTol);
	CHECK(std::fabs(ph::kDelayLog2Min - std::log2(0.0002)) < kTol);
	CHECK(std::fabs(ph::kDelayLog2Max - std::log2(0.015)) < kTol);
	CHECK(std::fabs(ph::kDelayLog2Default - std::log2(0.0025)) < kTol);
	CHECK(std::fabs(ph::kDampingLog2Min - std::log2(500.0)) < kTol);
	CHECK(std::fabs(ph::kDampingLog2Max - std::log2(20000.0)) < kTol);
	CHECK(std::fabs(ph::kRateLog2Min - std::log2(0.01)) < kTol);
	CHECK(std::fabs(ph::kRateLog2Max - std::log2(20.0)) < kTol);
	CHECK(std::fabs(ph::kRateLog2Default - std::log2(0.3)) < kTol);
	CHECK(std::fabs(ph::kEnvAttackLog2Min - std::log2(0.0005)) < kTol);
	CHECK(std::fabs(ph::kEnvAttackLog2Max - std::log2(5.0)) < kTol);
	CHECK(std::fabs(ph::kEnvAttackLog2Default - std::log2(0.01)) < kTol);
	CHECK(std::fabs(ph::kEnvReleaseLog2Min - std::log2(0.01)) < kTol);
	CHECK(std::fabs(ph::kEnvReleaseLog2Max - std::log2(20.0)) < kTol);
	CHECK(std::fabs(ph::kEnvReleaseLog2Default - std::log2(0.2)) < kTol);
}

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kRate = 48000.0;
/// THE LINEAR LOOP'S PEAK at fb ±0.995 under 10 s of 0 dBFS noise. The loop's power gain averaged over a
/// turn of phase is 1/(1 − 0.995²) ≈ 100, so the wet's RMS runs ~10× the input's and its peaks a few times
/// that. MEASURED 12.9-15.4 across every stage count and both signs (2026-10-03); twice that is the bound —
/// bounded and finite is the claim, the 0.995 itself is asserted by value through `feedbackForTest`.
constexpr float kLinearPeakBound = 32.0f;
/// THE TAIL'S BUDGET, in 512-frame blocks at 48 kHz: half the silence sweep's window. `kSnapLevel` is 1e-15,
/// 300 dB under the burst. MEASURED 2026-10-03, twelve stages after 1 s of noise: the default loop 11 blocks,
/// the hot ones (±0.9, damped or driven) 115-131.
constexpr int kTailBudgetBlocks = 200;
/// THE FLANGER'S, longer by its loop: through-zero at Position 0.4 reads 432 samples back, and at |fb| 0.9 the
/// loop loses 0.92 dB a pass, so 300 dB to `kSnapLevel` is ~327 passes = 141k samples = 276 blocks. MEASURED
/// 2026-10-03: 11 (default), 137 (hot, 209-sample loop), 248 and 259 (through-zero, damped and driven).
constexpr int kFlangerTailBudgetBlocks = 400;

std::vector<float> noise(int frames, uint32_t seed, float amplitude = 1.0f) {
	std::vector<float> out(static_cast<size_t>(frames));
	for (float& v : out) {
		seed = seed * 1664525u + 1013904223u;
		v = amplitude * static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
	}
	return out;
}

/// THE PLAIN PHASER: no LFO, no envelope, Position at rest, the loop open and linear.
ph::Settings plainPhaser(int stages, float feedback = 0.0f) {
	ph::Settings s;
	s.mode = ph::Mode::Phaser;
	s.stages = stages;
	s.lfoOn = false;
	s.envOn = false;
	s.position = 0.0f;
	s.feedback = feedback;
	s.drive = 0.0f;
	s.mix = 0.5f;
	return s;
}

std::unique_ptr<ph::Phaser> deviceWith(const ph::Settings& s, double rate = kRate) {
	auto dev = std::make_unique<ph::Phaser>();
	dev->setTarget(s);
	dev->prepare(rate);
	return dev;
}

/// Run a sine through, mono, in 512-frame blocks; after `settleSeconds`, return the steady state's
/// complex gain at `hz` over `windowSeconds` — a correlation with e^{−jωn} of the output over the
/// input's. Integer hertz over whole seconds is a whole number of cycles, so the image term cancels.
std::complex<double> measuredGain(ph::Phaser& dev, double hz, double settleSeconds = 0.5,
                                  double windowSeconds = 1.0, double rate = kRate) {
	const int settle = static_cast<int>(settleSeconds * rate);
	const int total = settle + static_cast<int>(windowSeconds * rate);
	const double w = 2.0 * kPi * hz / rate;
	std::complex<double> in {};
	std::complex<double> out {};
	std::vector<float> block(512);
	for (int n0 = 0; n0 < total; n0 += 512) {
		const int frames = std::min(512, total - n0);
		for (int j = 0; j < frames; ++j) {
			block[static_cast<size_t>(j)] = static_cast<float>(0.5 * std::sin(w * static_cast<double>(n0 + j)));
		}
		std::vector<float> x(block.begin(), block.begin() + frames);
		dev.processMono(block.data(), frames);
		for (int j = 0; j < frames; ++j) {
			const int n = n0 + j;
			if (n < settle) {
				continue;
			}
			const std::complex<double> e = std::polar(1.0, -w * static_cast<double>(n));
			in += static_cast<double>(x[static_cast<size_t>(j)]) * e;
			out += static_cast<double>(block[static_cast<size_t>(j)]) * e;
		}
	}
	return out / in;
}

/// The output's RMS over the input's, after `settleSeconds`, for a sine at `hz` (any frequency).
double measuredRmsRatio(ph::Phaser& dev, double hz, double rate = kRate) {
	const int settle = static_cast<int>(0.5 * rate);
	const int total = settle + static_cast<int>(rate);
	const double w = 2.0 * kPi * hz / rate;
	double in2 = 0.0;
	double out2 = 0.0;
	std::vector<float> block(512);
	for (int n0 = 0; n0 < total; n0 += 512) {
		const int frames = std::min(512, total - n0);
		for (int j = 0; j < frames; ++j) {
			block[static_cast<size_t>(j)] = static_cast<float>(0.5 * std::sin(w * static_cast<double>(n0 + j)));
		}
		const std::vector<float> x(block.begin(), block.begin() + frames);
		dev.processMono(block.data(), frames);
		for (int j = 0; j < frames; ++j) {
			if (n0 + j >= settle) {
				in2 += static_cast<double>(x[static_cast<size_t>(j)]) * x[static_cast<size_t>(j)];
				out2 += static_cast<double>(block[static_cast<size_t>(j)]) * block[static_cast<size_t>(j)];
			}
		}
	}
	return std::sqrt(out2 / in2);
}

double dB(double ratio) { return 20.0 * std::log10(ratio); }

/// THE CASCADE'S RESPONSE M from the closed form, every stage at g.
std::complex<double> cascadeOf(double g, int stages, double hz, double rate = kRate) {
	std::complex<double> m { 1.0, 0.0 };
	for (int i = 0; i < stages; ++i) {
		m *= ph::stageResponseOfG(g, hz / rate);
	}
	return m;
}

/// THE WHOLE PHASER IN CLOSED FORM at drive 0: wet = M / (1 − fb·z⁻¹·H_d·M), out = x + mix(wet − x), where
/// H_d is the damping lowpass — 1 when open, else the TPT one-pole H_lp(z) = g(z + 1) / ((1 + g)z + (g − 1))
/// at gDamp = tan(π·fd/sr) (`gDamp` ≤ 0 means open).
std::complex<double> phaserOf(double g, int stages, double fb, double mix, double hz, double rate = kRate,
                              double gDamp = 0.0) {
	const std::complex<double> m = cascadeOf(g, stages, hz, rate);
	const std::complex<double> z = std::polar(1.0, 2.0 * kPi * hz / rate);
	const std::complex<double> damp = gDamp > 0.0 ? gDamp * (z + 1.0) / ((1.0 + gDamp) * z + (gDamp - 1.0))
	                                              : std::complex<double> { 1.0, 0.0 };
	const std::complex<double> wet = m / (1.0 - fb / z * damp * m);
	return 1.0 + mix * (wet - 1.0);
}

} // namespace

TEST_CASE("phaser: Enabled off is a wire, bit for bit, on both roads") {
	const std::vector<float> l0 = noise(512, 0x9e3779b9u);
	const std::vector<float> r0 = noise(512, 0x7f4a7c15u);
	ph::Settings s = plainPhaser(6, 0.4f);
	s.enabled = false;
	auto dev = deviceWith(s);
	std::vector<float> l = l0;
	std::vector<float> r = r0;
	dev->processStereo(l.data(), r.data(), 512);
	CHECK(l == l0);
	CHECK(r == r0);
	std::vector<float> m = l0;
	dev->processMono(m.data(), 512);
	CHECK(m == l0);
}

TEST_CASE("phaser: an N-stage equal-corner cascade's first notch is where the closed form says") {
	// THE INDEPENDENT RESTATEMENT. A bilinear first-order allpass at prewarped corner g turns a frequency f
	// through φ(f) = −2·atan(tan(πf/sr) / g) — the analog −2·atan(ω/ωc) with ω = tan(πf/sr), ωc = g. N equal
	// stages turn N times that, and the dry-plus-wet sum 0.5(1 + e^{jNφ}) is zero where Nφ = −π: each stage
	// contributes π/N, so atan(tan(πf/sr)/g) = π/(2N), and
	//
	//     f_notch = (sr/π) · atan(g · tan(π/(2N))).
	//
	// g itself is restated too: tan(π · 800 / 48000), exact tan against the engine's `fasttan`.
	const double gRef = std::tan(kPi * 800.0 / kRate);
	for (const int n : { 2, 4, 6, 12 }) {
		CAPTURE(n);
		auto dev = deviceWith(plainPhaser(n));
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		REQUIRE(dev->stagesForTest() == n);
		// THE ENGINE'S OWN COEFFICIENTS, every stage, both channels.
		for (int ch = 0; ch < 2; ++ch) {
			for (int i = 0; i < n; ++i) {
				CHECK(std::fabs(static_cast<double>(dev->stageGForTest(ch, i)) - gRef) / gRef < 1.0e-5);
			}
		}
		const double fNotch = kRate / kPi * std::atan(gRef * std::tan(kPi / (2.0 * n)));
		CAPTURE(fNotch);
		// THE RESPONSE OF THE ENGINE'S COEFFICIENTS at the restated notch.
		std::complex<double> m { 1.0, 0.0 };
		for (int i = 0; i < n; ++i) {
			m *= ph::stageResponseOfG(static_cast<double>(dev->stageGForTest(0, i)), fNotch / kRate);
		}
		CHECK(dB(std::abs(0.5 * (1.0 + m))) < -40.0);
		// AND THE ENGINE ITSELF: a sine at the notch comes out 40 dB down. This is what the dry sum makes —
		// the cascade alone is an allpass and passes it at 0 dB.
		auto running = deviceWith(plainPhaser(n));
		const double rms = measuredRmsRatio(*running, fNotch);
		CAPTURE(dB(rms));
		CHECK(dB(rms) < -40.0);
	}
}

TEST_CASE("phaser: Position moves every corner to Center·2^(3s), clamped under 0.45·sr") {
	for (const float position : { -1.0f, -0.4f, 0.0f, 0.5f, 1.0f }) {
		CAPTURE(position);
		ph::Settings s = plainPhaser(6);
		s.position = position;
		auto dev = deviceWith(s);
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		const double fc = 800.0 * std::exp2(3.0 * position);
		const double gRef = std::tan(kPi * fc / kRate);
		CHECK(std::fabs(static_cast<double>(dev->stageGForTest(0, 3)) - gRef) / gRef < 1.0e-5);
	}
	// THE TOP CLAMP: 12 kHz centre, swept up three octaves, asks for 96 kHz; it gets 0.45·sr.
	ph::Settings s = plainPhaser(6);
	s.centerLog2Hz = static_cast<float>(ph::kCenterLog2Max);
	s.position = 1.0f;
	auto dev = deviceWith(s);
	std::vector<float> one(16, 0.0f);
	dev->processMono(one.data(), 16);
	const double gTop = std::tan(kPi * 0.45);
	CHECK(std::fabs(static_cast<double>(dev->stageGForTest(0, 0)) - gTop) / gTop < 1.0e-5);
}

TEST_CASE("phaser and flanger: the engine's output still follows Mix — out − in is Mix times (wet − in) (backlog LT)") {
	// THE TERRAIN NO LONGER READS MIX (backlog LT: drawn at a fixed 50%); THE SOUND STILL DOES. Out = dry + Mix·(wet − dry),
	// so the steady-state complex gain G(Mix) satisfies G(m) − G(0) = m·(G(1) − G(0)) at every frequency — G(0) is the
	// dry road, a wire in the Phaser and the latency's delay in the Flanger. Measured at Mix 0, 0.2 and 1.0.
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = plainPhaser(6, 0.3f);
		s.mode = mode;
		const auto gainAt = [&](float m, double hz) {
			ph::Settings t = s;
			t.mix = m;
			auto dev = deviceWith(t);
			return measuredGain(*dev, hz);
		};
		for (const double hz : { 300.0, 1000.0, 3000.0 }) {
			CAPTURE(hz);
			const std::complex<double> dry = gainAt(0.0f, hz);
			const std::complex<double> full = gainAt(1.0f, hz) - dry;
			const std::complex<double> fifth = gainAt(0.2f, hz) - dry;
			// Non-vacuous: the wet differs from the dry by a real amount at these frequencies.
			REQUIRE(std::abs(full) > 0.05);
			CHECK(std::abs(fifth - 0.2 * full) < 1e-3);
		}
	}
}

TEST_CASE("phaser: Feedback sign moves the peaks between notches") {
	constexpr int kStages = 6;
	const double gRef = std::tan(kPi * 800.0 / kRate);
	// THE MIDPOINT BETWEEN THE FIRST TWO NOTCHES, in phase: the cascade has turned a whole cycle (Nφ = −2π),
	// so the wet is in phase with the dry and the plain sum peaks — f = (sr/π)·atan(g·tan(π/N)).
	const double fPeak = kRate / kPi * std::atan(gRef * std::tan(kPi / kStages));
	CAPTURE(fPeak);
	const double plain = std::abs(phaserOf(gRef, kStages, 0.0, 0.5, fPeak));
	const double positive = std::abs(phaserOf(gRef, kStages, 0.5, 0.5, fPeak));
	const double negative = std::abs(phaserOf(gRef, kStages, -0.5, 0.5, fPeak));
	CHECK(positive > plain * 1.2);
	CHECK(negative < plain);

	// THE ENGINE AGAINST THE CLOSED FORM, to 0.1 dB, at five frequencies clear of the notches (214, 800 and
	// 2952 Hz for six stages at 800 Hz) — magnitude in dB and phase both.
	for (const float fb : { 0.0f, 0.5f, -0.5f, 0.9f }) {
		CAPTURE(fb);
		auto probe = deviceWith(plainPhaser(kStages, fb));
		std::vector<float> one(16, 0.0f);
		probe->processMono(one.data(), 16);
		CHECK(probe->feedbackForTest() == fb);
		CHECK(probe->dampingOpenForTest());
		for (const double hz : { 100.0, 400.0, 1500.0, 5000.0, 12000.0 }) {
			CAPTURE(hz);
			auto dev = deviceWith(plainPhaser(kStages, fb));
			const std::complex<double> measured = measuredGain(*dev, hz);
			const std::complex<double> closed = phaserOf(gRef, kStages, fb, 0.5, hz);
			CAPTURE(dB(std::abs(measured)));
			CAPTURE(dB(std::abs(closed)));
			CHECK(std::fabs(dB(std::abs(measured)) - dB(std::abs(closed))) < 0.1);
			CHECK(std::abs(std::arg(measured / closed)) < 0.0115); // 0.1 dB's worth of phase, in radians
		}
	}
	// AND THE PEAK ITSELF, measured: the nearest whole hertz, positive feedback louder than none.
	auto none = deviceWith(plainPhaser(kStages, 0.0f));
	auto pos = deviceWith(plainPhaser(kStages, 0.5f));
	const double hz = std::round(fPeak);
	CHECK(std::abs(measuredGain(*pos, hz)) > 1.2 * std::abs(measuredGain(*none, hz)));

	// DAMPING, BY VALUE: at 2 kHz the loop is wet = M / (1 − fb·z⁻¹·H_lp·M), and the engine matches that to
	// 0.1 dB at the same five frequencies — for both signs of a strong loop, where the lowpass matters most.
	const double gDamp = std::tan(kPi * 2000.0 / kRate);
	double dampingMoves = 0.0; // the largest damped-vs-undamped difference: the case is not vacuous
	for (const float fb : { 0.8f, -0.8f }) {
		CAPTURE(fb);
		ph::Settings s = plainPhaser(kStages, fb);
		s.dampingLog2Hz = static_cast<float>(std::log2(2000.0));
		for (const double f : { 100.0, 400.0, 1500.0, 5000.0, 12000.0 }) {
			CAPTURE(f);
			auto dev = deviceWith(s);
			const std::complex<double> measured = measuredGain(*dev, f);
			REQUIRE_FALSE(dev->dampingOpenForTest());
			const std::complex<double> closed = phaserOf(gRef, kStages, fb, 0.5, f, kRate, gDamp);
			const std::complex<double> undamped = phaserOf(gRef, kStages, fb, 0.5, f);
			CAPTURE(dB(std::abs(measured)));
			CAPTURE(dB(std::abs(closed)));
			CAPTURE(dB(std::abs(undamped)));
			dampingMoves = std::max(dampingMoves, std::fabs(dB(std::abs(closed)) - dB(std::abs(undamped))));
			CHECK(std::fabs(dB(std::abs(measured)) - dB(std::abs(closed))) < 0.1);
			CHECK(std::abs(std::arg(measured / closed)) < 0.0115);
		}
	}
	CHECK(dampingMoves > 1.0);
}

TEST_CASE("phaser: Damping is out of the loop exactly at its top, and held under 0.45·sr below it") {
	// AT THE STORED TOP — the float a row holds, which rounds BELOW the double constant.
	ph::Settings s = plainPhaser(6, 0.5f);
	s.dampingLog2Hz = static_cast<float>(ph::kDampingLog2Max);
	auto open = deviceWith(s, 44100.0);
	std::vector<float> one(16, 0.0f);
	open->processMono(one.data(), 16);
	CHECK(open->dampingOpenForTest());
	// ONE FLOAT STEP BELOW: in the loop, and 20 kHz is past 0.45·44.1k, so the corner is that.
	s.dampingLog2Hz = std::nextafter(static_cast<float>(ph::kDampingLog2Max), 0.0f);
	auto shut = deviceWith(s, 44100.0);
	shut->processMono(one.data(), 16);
	CHECK_FALSE(shut->dampingOpenForTest());
	CHECK(shut->dampingHzForTest() == doctest::Approx(0.45 * 44100.0).epsilon(1e-12));
	// AN ORDINARY CORNER is its own hertz.
	s.dampingLog2Hz = static_cast<float>(std::log2(2000.0));
	auto dark = deviceWith(s, 44100.0);
	dark->processMono(one.data(), 16);
	CHECK(std::fabs(dark->dampingHzForTest() - 2000.0) < 1.0e-3);
	// AND THE READOUT AGREES WITH THE ENGINE on the float-rounded top.
	char out[32] = {};
	REQUIRE(ph::kTextOps.format(static_cast<size_t>(ph::Param::Damping),
	                            static_cast<double>(static_cast<float>(ph::kDampingLog2Max)),
	                            feathers::device::TextView {}, out, sizeof(out)));
	CHECK(std::string(out) == "open");
}

TEST_CASE("phaser: full feedback, full drive, every stage count, both polarities stays bounded") {
	// 10 s OF WHITE NOISE AT 0 dBFS. With Drive up the saturator bounds what the loop adds (|sat| < 1/k),
	// so the output stays within a few times the input whatever the stage count.
	const int frames = static_cast<int>(10.0 * kRate);
	const std::vector<float> l0 = noise(frames, 0x1234567u);
	const std::vector<float> r0 = noise(frames, 0x7654321u);
	for (const float drive : { 1.0f, 0.0f }) {
		for (const int n : ph::kStageCounts) {
			for (const float fb : { 1.0f, -1.0f }) {
				CAPTURE(drive);
				CAPTURE(n);
				CAPTURE(fb);
				ph::Settings s = plainPhaser(n, fb);
				s.drive = drive;
				auto dev = deviceWith(s);
				std::vector<float> l = l0;
				std::vector<float> r = r0;
				float peak = 0.0f;
				bool finite = true;
				for (int n0 = 0; n0 < frames; n0 += 512) {
					const int run = std::min(512, frames - n0);
					dev->processStereo(l.data() + n0, r.data() + n0, run);
					for (int j = n0; j < n0 + run; ++j) {
						finite = finite && std::isfinite(l[static_cast<size_t>(j)]) && std::isfinite(r[static_cast<size_t>(j)]);
						peak = std::max({ peak, std::fabs(l[static_cast<size_t>(j)]), std::fabs(r[static_cast<size_t>(j)]) });
					}
				}
				CAPTURE(peak);
				CHECK(finite);
				if (drive > 0.0f) {
					// THE SATURATED LOOP RUNS THE STORED ±1, unclamped.
					CHECK(dev->feedbackForTest() == fb);
					CHECK(peak < 8.0f);
				}
				else {
					// THE LINEAR LOOP IS HELD AT ±0.995 — at ±1 it would be lossless and grow without bound.
					CHECK(dev->feedbackForTest() == static_cast<float>(fb * ph::kMaxLinearFeedback));
					CHECK(peak < kLinearPeakBound);
				}
			}
		}
	}
}

TEST_CASE("phaser: the tail reaches exact zero") {
	// 1 s OF NOISE, THEN SILENCE, in 512-frame blocks: after the budget every sample is exactly 0.0f and
	// stays so. The budget is MEASURED, with a margin, per arrangement (see kTailBudgetBlocks).
	const std::vector<float> burst = noise(static_cast<int>(kRate), 0xfeedfaceu, 0.8f);
	struct Arrangement {
		const char* what;
		float feedback;
		float drive;
		float dampingLog2Hz;
	};
	const Arrangement arrangements[] = {
		{ "default loop", 0.3f, 0.0f, static_cast<float>(ph::kDampingLog2Max) },
		{ "hot linear loop", 0.9f, 0.0f, static_cast<float>(ph::kDampingLog2Max) },
		{ "negative, damped", -0.9f, 0.0f, static_cast<float>(std::log2(2000.0)) },
		{ "driven", 0.9f, 0.6f, static_cast<float>(ph::kDampingLog2Max) },
	};
	for (const Arrangement& a : arrangements) {
		CAPTURE(a.what);
		ph::Settings s = plainPhaser(12, a.feedback);
		s.drive = a.drive;
		s.dampingLog2Hz = a.dampingLog2Hz;
		auto dev = deviceWith(s);
		std::vector<float> l = burst;
		std::vector<float> r = burst;
		for (size_t n0 = 0; n0 < burst.size(); n0 += 512) {
			const int run = static_cast<int>(std::min<size_t>(512, burst.size() - n0));
			dev->processStereo(l.data() + n0, r.data() + n0, run);
		}
		int lastNonZero = -1;
		std::vector<float> bl(512);
		std::vector<float> br(512);
		for (int b = 0; b < kTailBudgetBlocks * 2; ++b) {
			std::fill(bl.begin(), bl.end(), 0.0f);
			std::fill(br.begin(), br.end(), 0.0f);
			dev->processStereo(bl.data(), br.data(), 512);
			for (int j = 0; j < 512; ++j) {
				if (bl[static_cast<size_t>(j)] != 0.0f || br[static_cast<size_t>(j)] != 0.0f) {
					lastNonZero = b;
				}
			}
		}
		INFO(std::string(a.what) << ": last non-zero block " << lastNonZero);
		CHECK(lastNonZero >= 0); // there WAS a tail, or the claim is vacuous
		CHECK(lastNonZero < kTailBudgetBlocks);
	}
}

TEST_CASE("phaser: setFields carries the travel past the params, and a non-finite one reads as home") {
	// THE WIDE ROW: the declared params, then `kPhaseCount` floats of folded travel (device_row.h's blob).
	std::vector<float> wide(ph::kTravelFirst + ph::phase_space::kPhaseCount, 0.0f);
	const ph::Row row = ph::rowOf(ph::State {});
	std::copy(row.begin(), row.end(), wide.begin());
	for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
		wide[ph::kTravelFirst + i] = 0.125f * static_cast<float>(i + 1);
	}
	constexpr size_t kPoisoned = 7;
	wide[ph::kTravelFirst + kPoisoned] = std::numeric_limits<float>::quiet_NaN();

	alignas(ph::Phaser) unsigned char storage[sizeof(ph::Phaser)];
	ph::kAudioOps.construct(storage);
	ph::kAudioOps.prepare(storage, 48000.0, 4096);
	ph::kAudioOps.setFields(storage, wide.data(), wide.size());
	const auto& dev = *reinterpret_cast<const ph::Phaser*>(storage);
	for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(dev.target().travel[i] == (i == kPoisoned ? 0.0f : 0.125f * static_cast<float>(i + 1)));
	}
	// A ROW WITH NO TRAVEL AT ALL (one written before the pad existed) is home.
	ph::kAudioOps.setFields(storage, wide.data(), ph::kTravelFirst);
	for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
		CHECK(dev.target().travel[i] == 0.0f);
	}
	ph::kAudioOps.destruct(storage);
}

TEST_CASE("phaser: the readouts never say \"off\" for a real time, and Damping's top reads \"open\"") {
	REQUIRE(ph::kDescriptor.text != nullptr);
	REQUIRE(ph::kDescriptor.text->format != nullptr);
	const auto print = [](ph::Param p, double value) {
		char out[32] = {};
		const bool mine = ph::kDescriptor.text->format(static_cast<size_t>(p), value, feathers::device::TextView {},
		                                                out, sizeof(out));
		return mine ? std::string(out) : std::string("<declared unit>");
	};
	// AT EACH BOTTOM: the time itself, not the LogSeconds "off" (none of the three skips its stage there).
	CHECK(print(ph::Param::DelayMs, ph::kDelayLog2Min) == "0.2ms");
	CHECK(print(ph::Param::EnvAttack, ph::kEnvAttackLog2Min) == "0.5ms");
	CHECK(print(ph::Param::EnvRelease, ph::kEnvReleaseLog2Min) == "10ms");
	// AND AT THE LONG ENDS, SECONDS (`printSeconds`, the house reading: "2s", "1.5s").
	CHECK(print(ph::Param::EnvAttack, ph::kEnvAttackLog2Max) == "5s");
	CHECK(print(ph::Param::EnvRelease, ph::kEnvReleaseLog2Max) == "20s");
	CHECK(print(ph::Param::EnvRelease, std::log2(1.5)) == "1.5s");
	CHECK(print(ph::Param::EnvRelease, std::log2(12.0)) == "12s");
	// AND THE DECLARED UNIT WOULD HAVE SAID "off" — so the hook is what changed the reading.
	CHECK(feathers::device::formatValue(ph::kParamDescs[static_cast<size_t>(ph::Param::DelayMs)], ph::kDelayLog2Min)
	      == "off");
	// THE LOOP'S LOWPASS IS BYPASSED AT ITS TOP: "open". Below it the declared hertz reading stands.
	CHECK(print(ph::Param::Damping, ph::kDampingLog2Max) == "open");
	CHECK(print(ph::Param::Damping, ph::kDampingLog2Max - 1.0) == "<declared unit>");
	// EVERY OTHER CONTROL PRINTS BY ITS UNIT.
	CHECK(print(ph::Param::Mix, 0.5) == "<declared unit>");
	// STEREO READS WHOLE DEGREES.
	CHECK(feathers::device::formatValue(ph::kParamDescs[static_cast<size_t>(ph::Param::Stereo)], 90.0) == "90");
}

//==================================================================================================
// THE FLANGER (the plan's Task 3).
//==================================================================================================

namespace {

ph::Settings plainFlanger(float position = 0.0f, float feedback = 0.0f, bool throughZero = false) {
	ph::Settings s = plainPhaser(6, feedback);
	s.mode = ph::Mode::Flanger;
	s.position = position;
	s.throughZero = throughZero;
	return s;
}

/// The centroid Σ n·y[n] / Σ y[n] of the response to a unit impulse — the delay of a linear-phase path,
/// in samples. Mono, one 4096-frame block.
double impulseCentroid(ph::Phaser& dev) {
	std::vector<float> x(4096, 0.0f);
	x[0] = 1.0f;
	dev.processMono(x.data(), 4096);
	double moment = 0.0;
	double mass = 0.0;
	for (size_t n = 0; n < x.size(); ++n) {
		moment += static_cast<double>(n) * x[n];
		mass += x[n];
	}
	return moment / mass;
}

} // namespace

TEST_CASE("flanger: the read delay is DelayMs·2^(2s)") {
	// THE INDEPENDENT RESTATEMENT: 2.5 ms (the default) is 120 samples at 48 kHz, and a full sweep is two
	// octaves either way.
	for (const float s : { -1.0f, -0.5f, 0.0f, 0.7f, 1.0f }) {
		CAPTURE(s);
		auto dev = deviceWith(plainFlanger(s));
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		const double expected = 0.0025 * kRate * std::exp2(2.0 * s);
		CHECK(std::fabs(static_cast<double>(dev->delaySamplesForTest(0)) - expected) < 1.0e-3);
		CHECK(dev->delaySamplesForTest(1) == dev->delaySamplesForTest(0));
		CHECK(dev->dryDelaySamplesForTest() == 0);
	}
	// THE CLAMPS: 15 ms swept up two octaves asks for 60 ms and gets 20; 0.2 ms swept down asks for 2.4
	// samples and gets the line's minimum read, 6.
	{
		ph::Settings s = plainFlanger(1.0f);
		s.delayLog2s = static_cast<float>(ph::kDelayLog2Max);
		auto dev = deviceWith(s);
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		CHECK(std::fabs(static_cast<double>(dev->delaySamplesForTest(0)) - 0.020 * kRate) < 1.0e-3);
	}
	{
		ph::Settings s = plainFlanger(-1.0f);
		s.delayLog2s = static_cast<float>(ph::kDelayLog2Min);
		auto dev = deviceWith(s);
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		CHECK(dev->delaySamplesForTest(0) == 6.0f);
	}
	// AND THE LINE IS READ THERE: wet only, an impulse comes out centred on the delay (the kernel is linear
	// phase, so its centroid is the delay itself).
	// AND THE WHOLE PLAIN FLANGER IS THE REPORTED LATENCY LATE (`latencySamplesFor`), Through Zero off too.
	ph::Settings wetOnly = plainFlanger(0.3f);
	wetOnly.mix = 1.0f;
	auto dev = deviceWith(wetOnly);
	const double expected = 0.0025 * kRate * std::exp2(2.0 * 0.3) + static_cast<double>(ph::tzDrySamples(kRate));
	CHECK(std::fabs(impulseCentroid(*dev) - expected) < 1.0e-2);
}

TEST_CASE("flanger: through-zero cancels at the crossing") {
	// TZ ON, Mix 0.5, no feedback, Position 0: the wet reads at exactly the dry's delay, through the same
	// kernel, and is inverted — out = 0.5·(x_d − x_d).
	{
		auto dev = deviceWith(plainFlanger(0.0f, 0.0f, true));
		const double rms = measuredRmsRatio(*dev, 1000.0);
		CAPTURE(dB(rms));
		CHECK(dB(rms) < -60.0);
	}
	// NOT VACUOUS: off the crossing it does not cancel. Position 0.5 is 60 samples (1.25 ms) apart, which at
	// 1 kHz is |sin(π · 1000 · 0.00125)| = 0.707 of the input.
	{
		auto dev = deviceWith(plainFlanger(0.5f, 0.0f, true));
		const double rms = measuredRmsRatio(*dev, 1000.0);
		CHECK(std::fabs(rms - std::fabs(std::sin(kPi * 1000.0 * 0.00125))) < 0.01);
	}
	// THE RELATIVE DELAY, read from the engine: wet − dry = s · Delay (120 samples at the default 2.5 ms).
	for (const float s : { -1.0f, 0.0f, 1.0f }) {
		CAPTURE(s);
		auto dev = deviceWith(plainFlanger(s, 0.0f, true));
		std::vector<float> one(16, 0.0f);
		dev->processMono(one.data(), 16);
		CHECK(dev->dryDelaySamplesForTest() == 384);
		const double relative = static_cast<double>(dev->delaySamplesForTest(0)) - dev->dryDelaySamplesForTest();
		CHECK(std::fabs(relative - s * 0.0025 * kRate) < 1.0e-3);
	}
	// AND A LONG DELAY'S EXCURSION IS HELD TO THE DRY LESS THE LINE'S MINIMUM, so s = −1 still reads legally.
	ph::Settings deep = plainFlanger(-1.0f, 0.0f, true);
	deep.delayLog2s = static_cast<float>(ph::kDelayLog2Max);
	auto dev = deviceWith(deep);
	std::vector<float> one(16, 0.0f);
	dev->processMono(one.data(), 16);
	CHECK(dev->delaySamplesForTest(0) == 6.0f);
}

TEST_CASE("flanger: reported latency equals the dry delay in the Flanger, Through Zero on or off; the Phaser has none") {
	for (const double rate : { 44100.0, 96000.0 }) {
		const uint32_t dry = static_cast<uint32_t>(std::lround(0.008 * rate)); // 353 and 768
		for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
			for (const bool tz : { false, true }) {
				CAPTURE(rate);
				CAPTURE(static_cast<int>(mode));
				CAPTURE(tz);
				ph::State state;
				state.settings.mode = mode;
				state.settings.throughZero = tz;
				const ph::Row row = ph::rowOf(state);
				const uint32_t latency = ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate);
				// THROUGH ZERO DOES NOT MOVE IT (a TZ flip used to restart the host and kill every
				// held note): the plain flanger's output is held back by the same delay.
				const bool expected = mode == ph::Mode::Flanger;
				CHECK(latency == (expected ? dry : 0u));
				// THE ENGINE DELAYS THE DRY BY EXACTLY THAT, and with Mix at 0 (the dry alone) an impulse
				// comes out centred there — on, and switched off.
				if (expected) {
					ph::Settings s = plainFlanger(0.0f, 0.0f, tz);
					s.mix = 0.0f;
					auto dev = deviceWith(s, rate);
					CHECK(impulseCentroid(*dev) == doctest::Approx(static_cast<double>(dry)).epsilon(1e-6));
					if (tz) {
						CHECK(static_cast<uint32_t>(dev->dryDelaySamplesForTest()) == latency);
					}
					s.enabled = false;
					auto off = deviceWith(s, rate);
					CHECK(impulseCentroid(*off) == doctest::Approx(static_cast<double>(dry)).epsilon(1e-6));
				}
			}
		}
	}
	// AN EMPTY ROW READS AS THE DEFAULTS: the Phaser, no latency.
	CHECK(ph::kAudioOps.latencySamples(nullptr, nullptr, 0, 48000.0) == 0u);
}

TEST_CASE("flanger: a mode switch clears the engine being entered, so nothing stale replays") {
	const std::vector<float> loud = noise(4096, 0xabcdef01u);
	for (const ph::Mode first : { ph::Mode::Flanger, ph::Mode::Phaser }) {
		CAPTURE(static_cast<int>(first));
		const ph::Mode other = first == ph::Mode::Flanger ? ph::Mode::Phaser : ph::Mode::Flanger;
		ph::Settings s = plainFlanger(0.0f, 0.9f);
		s.mode = first;
		auto dev = deviceWith(s);
		std::vector<float> x = loud;
		dev->processMono(x.data(), 4096); // the engine fills with noise
		s.mode = other;
		dev->setTarget(s);
		std::vector<float> quiet(64, 0.0f);
		dev->processMono(quiet.data(), 64); // one short visit to the other engine
		s.mode = first;
		dev->setTarget(s);
		// BACK, ON SILENCE: the first engine was cleared on re-entry, so nothing it held replays. The Phaser's silence
		// in is silence out at once; the Flanger is its latency late, and what comes out first is the INPUT that latency
		// ago (the dry, through the cleared ring) — never the old flanger's own output — then silence.
		std::vector<float> back(4096, 0.0f);
		dev->processMono(back.data(), 4096);
		const size_t latency = first == ph::Mode::Flanger ? static_cast<size_t>(ph::tzDrySamples(kRate)) : 0u;
		size_t wrong = 0;
		for (size_t n = 0; n < back.size(); ++n) {
			// The input stream was loud (4096), then 64 zeros, then these zeros.
			const long at = static_cast<long>(4096 + 64 + n) - static_cast<long>(latency);
			const float input = at < 4096 ? loud[static_cast<size_t>(at)] : 0.0f;
			wrong += back[n] != input ? 1u : 0u;
		}
		CHECK(wrong == 0u);
		CHECK(std::all_of(back.begin() + static_cast<long>(latency), back.end(), [](float v) { return v == 0.0f; }));
	}
}

TEST_CASE("flanger: full feedback, full drive, both polarities, with and without through-zero stays bounded") {
	const int frames = static_cast<int>(10.0 * kRate);
	const std::vector<float> l0 = noise(frames, 0x2468aceu);
	const std::vector<float> r0 = noise(frames, 0x13579bdu);
	for (const float drive : { 1.0f, 0.0f }) {
		for (const bool tz : { false, true }) {
			for (const float fb : { 1.0f, -1.0f }) {
				CAPTURE(drive);
				CAPTURE(tz);
				CAPTURE(fb);
				ph::Settings s = plainFlanger(0.4f, fb, tz);
				s.drive = drive;
				auto dev = deviceWith(s);
				std::vector<float> l = l0;
				std::vector<float> r = r0;
				float peak = 0.0f;
				bool finite = true;
				for (int n0 = 0; n0 < frames; n0 += 512) {
					const int run = std::min(512, frames - n0);
					dev->processStereo(l.data() + n0, r.data() + n0, run);
					for (int j = n0; j < n0 + run; ++j) {
						finite = finite && std::isfinite(l[static_cast<size_t>(j)]) && std::isfinite(r[static_cast<size_t>(j)]);
						peak = std::max({ peak, std::fabs(l[static_cast<size_t>(j)]), std::fabs(r[static_cast<size_t>(j)]) });
					}
				}
				CAPTURE(peak);
				CHECK(finite);
				if (drive > 0.0f) {
					CHECK(dev->feedbackForTest() == fb);
					CHECK(peak < 8.0f);
				}
				else {
					CHECK(dev->feedbackForTest() == static_cast<float>(fb * ph::kMaxLinearFeedback));
					CHECK(peak < kLinearPeakBound);
				}
			}
		}
	}
}

TEST_CASE("flanger: the tail reaches exact zero") {
	const std::vector<float> burst = noise(static_cast<int>(kRate), 0xc0ffee11u, 0.8f);
	struct Arrangement {
		const char* what;
		float feedback;
		float drive;
		float dampingLog2Hz;
		bool throughZero;
	};
	const Arrangement arrangements[] = {
		{ "default loop", 0.3f, 0.0f, static_cast<float>(ph::kDampingLog2Max), false },
		{ "hot linear loop", 0.9f, 0.0f, static_cast<float>(ph::kDampingLog2Max), false },
		{ "through-zero, negative, damped", -0.9f, 0.0f, static_cast<float>(std::log2(2000.0)), true },
		{ "driven", 0.9f, 0.6f, static_cast<float>(ph::kDampingLog2Max), true },
	};
	for (const Arrangement& a : arrangements) {
		CAPTURE(std::string(a.what));
		ph::Settings s = plainFlanger(0.4f, a.feedback, a.throughZero);
		s.drive = a.drive;
		s.dampingLog2Hz = a.dampingLog2Hz;
		auto dev = deviceWith(s);
		std::vector<float> l = burst;
		std::vector<float> r = burst;
		for (size_t n0 = 0; n0 < burst.size(); n0 += 512) {
			const int run = static_cast<int>(std::min<size_t>(512, burst.size() - n0));
			dev->processStereo(l.data() + n0, r.data() + n0, run);
		}
		int lastNonZero = -1;
		std::vector<float> bl(512);
		std::vector<float> br(512);
		for (int b = 0; b < kFlangerTailBudgetBlocks * 2; ++b) {
			std::fill(bl.begin(), bl.end(), 0.0f);
			std::fill(br.begin(), br.end(), 0.0f);
			dev->processStereo(bl.data(), br.data(), 512);
			for (int j = 0; j < 512; ++j) {
				if (bl[static_cast<size_t>(j)] != 0.0f || br[static_cast<size_t>(j)] != 0.0f) {
					lastNonZero = b;
				}
			}
		}
		INFO(std::string(a.what) << ": last non-zero block " << lastNonZero);
		CHECK(lastNonZero >= 0);
		CHECK(lastNonZero < kFlangerTailBudgetBlocks);
	}
}

namespace {

/// Run `frames` of `x` (or of silence) through, stereo, in blocks of `block`.
void runStereo(ph::Phaser& dev, int frames, float x = 0.0f, int block = 512) {
	std::vector<float> l(static_cast<size_t>(block));
	std::vector<float> r(static_cast<size_t>(block));
	for (int n0 = 0; n0 < frames; n0 += block) {
		const int run = std::min(block, frames - n0);
		std::fill(l.begin(), l.end(), x);
		std::fill(r.begin(), r.end(), x);
		dev.processStereo(l.data(), r.data(), run);
	}
}

/// The Sync choice that NAMES `division` — found by its label, so the test does not do the index arithmetic.
uint32_t syncChoice(const char* division) {
	for (size_t i = 0; i < ph::kSyncNames.size(); ++i) {
		if (std::string(ph::kSyncNames[i]) == division) {
			return static_cast<uint32_t>(i);
		}
	}
	FAIL("no such division: " << division);
	return 0;
}

/// The forward distance from phase a to phase b, 0..1.
double phaseAhead(double a, double b) {
	const double d = b - a;
	return d - std::floor(d);
}

/// A READINGS SINK that keeps the last publish.
struct Captured {
	std::vector<float> values;
	int publishes = 0;
	static void publish(void* context, const float* values, size_t count) {
		auto* self = static_cast<Captured*>(context);
		self->values.assign(values, values + count);
		++self->publishes;
	}
};

} // namespace

//==================================================================================================
// THE REVIEW'S FIXES (2026-10-03): the saturator, the glides, the guards, the kernels, the clamps.
//==================================================================================================

TEST_CASE("phaser: the loop's saturator is tanh(v·k)/k with k = 7·Drive, continuous into Drive 0") {
	// BY VALUE against std::tanh, wherever the argument is inside fasttanh's clamp (|v·k| ≤ 4.5), where its
	// stated absolute error is 4.3e-5 — so 5e-5/k on the quotient.
	for (const float drive : { 0.1f, 0.37f, 1.0f }) {
		const double k = 7.0 * drive;
		for (int i = -40; i <= 40; ++i) {
			const float v = 0.05f * static_cast<float>(i);
			if (std::fabs(v * k) > 4.5) {
				continue;
			}
			CAPTURE(drive);
			CAPTURE(v);
			const double expected = std::tanh(static_cast<double>(v) * k) / k;
			CHECK(std::fabs(static_cast<double>(ph::saturate(v, drive)) - expected) < 5.0e-5 / k);
		}
		// AND BOUNDED BY 1/k however hard it is driven: what holds a ±1 loop.
		CHECK(std::fabs(ph::saturate(1000.0f, drive)) <= static_cast<float>(1.0 / k));
	}
	// DRIVE 0 IS THE IDENTITY, AND A WHISPER OF DRIVE IS A WHISPER OF CURVE: within 1e-6 of linear.
	for (int i = -20; i <= 20; ++i) {
		const float v = 0.05f * static_cast<float>(i);
		CHECK(ph::saturate(v, 0.0f) == v);
		CHECK(std::fabs(ph::saturate(v, 1.0e-4f) - v) < 1.0e-6f);
	}
	// THE CEILING OPENS LINEARLY: 0.995 at Drive 0, 0.9975 at 0.05, 1 from 0.1 up.
	CHECK(ph::effectiveFeedback(1.0, 0.0) == ph::kMaxLinearFeedback);
	CHECK(std::fabs(ph::effectiveFeedback(1.0, 0.05) - 0.9975) < 1.0e-12);
	CHECK(std::fabs(ph::effectiveFeedback(-1.0, 0.1) + 1.0) < 1.0e-12);
	CHECK(ph::effectiveFeedback(0.7, 0.0) == 0.7);
}

TEST_CASE("phaser: the glides land on their targets, and damping leaves the loop when the glide does") {
	// POSITION, 5 ms: a step from 0 to 1 is within e^-5 after 25 ms, and exactly 1 once within 1e-7.
	ph::Settings s = plainPhaser(6, 0.5f);
	s.dampingLog2Hz = static_cast<float>(std::log2(2000.0));
	auto dev = deviceWith(s);
	std::vector<float> block(1200, 0.0f); // 25 ms
	dev->processMono(block.data(), 16);
	REQUIRE(dev->positionForTest() == 0.0);
	REQUIRE_FALSE(dev->dampingOpenForTest());
	s.position = 1.0f;
	s.dampingLog2Hz = static_cast<float>(ph::kDampingLog2Max);
	dev->setTarget(s);
	dev->processMono(block.data(), 1200);
	CHECK(dev->positionForTest() > 1.0 - std::exp(-5.0) - 0.01);
	CHECK(dev->positionForTest() < 1.0);
	// The 20 ms glide on Damping is still on its way up after 25 ms: still in the loop.
	CHECK_FALSE(dev->dampingOpenForTest());
	std::vector<float> long_(48000, 0.0f);
	dev->processMono(long_.data(), 48000);
	CHECK(dev->positionForTest() == 1.0);
	// LANDED EXACTLY ON THE STORED TOP, so it is out of the loop.
	CHECK(dev->dampingLog2ForTest() == static_cast<double>(static_cast<float>(ph::kDampingLog2Max)));
	CHECK(dev->dampingOpenForTest());
}

TEST_CASE("phaser: one non-finite input sample is read as silence, and the device still reaches exact zero") {
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = plainFlanger(0.2f, 0.9f, mode == ph::Mode::Flanger);
		s.mode = mode;
		auto dev = deviceWith(s);
		std::vector<float> x = noise(512, 0x5eed5eedu, 0.5f);
		x[200] = std::numeric_limits<float>::quiet_NaN();
		x[201] = std::numeric_limits<float>::infinity();
		dev->processMono(x.data(), 512);
		CHECK(std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); }));
		std::vector<float> next = noise(512, 0x5eed0001u, 0.5f);
		dev->processMono(next.data(), 512);
		CHECK(std::all_of(next.begin(), next.end(), [](float v) { return std::isfinite(v); }));
		// AND THE TAIL ENDS: silence in, exact zero out within the flanger's budget.
		int lastNonZero = -1;
		std::vector<float> quiet(512);
		for (int b = 0; b < kFlangerTailBudgetBlocks; ++b) {
			std::fill(quiet.begin(), quiet.end(), 0.0f);
			dev->processMono(quiet.data(), 512);
			if (std::any_of(quiet.begin(), quiet.end(), [](float v) { return v != 0.0f; })) {
				lastNonZero = b;
			}
		}
		CHECK(lastNonZero < kFlangerTailBudgetBlocks - 1);
	}
	// A FAULT THE GUARD CATCHES: a finite input near FLT_MAX overflows the loop to inf − inf. The guard resets the
	// LOOP ONLY — the sweep's sources live on: the LFO's phase is where its normal advance puts it, and the corner
	// the readings report is a real one.
	{
		ph::Settings s = plainPhaser(6, 0.9f);
		s.lfoOn = true;
		s.rateLog2Hz = 1.0f; // 2 Hz
		auto dev = deviceWith(s);
		Captured got;
		dev->setReadingsSink({ &got, &Captured::publish });
		runStereo(*dev, 512, 0.3f);
		const double before = dev->lfoPhaseForTest();
		runStereo(*dev, 512, 3.0e38f); // 32 ticks
		REQUIRE(dev->loopResetsForTest() > 0);
		const double expected = before + 32.0 * 16.0 * 2.0 / kRate;
		CHECK(std::fabs(dev->lfoPhaseForTest() - (expected - std::floor(expected))) < 1.0e-9);
		const float corner = got.values[static_cast<size_t>(ph::Reading::CornerHzL)];
		CHECK(std::isfinite(corner));
		CHECK(corner > 0.0f);
		// AND AFTER IT, ordinary input comes out finite.
		std::vector<float> l(512, 0.3f);
		std::vector<float> r(512, 0.3f);
		dev->processStereo(l.data(), r.data(), 512);
		CHECK(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v); }));
	}
}

TEST_CASE("flanger: switched off, Through Zero on or off, the output is the input exactly the latency late") {
	for (const double rate : { 44100.0, 48000.0, 96000.0 })
	for (const bool tz : { true, false }) {
		CAPTURE(rate);
		CAPTURE(tz);
		ph::State state;
		state.settings = plainFlanger(0.3f, 0.6f, tz);
		state.settings.enabled = false;
		const ph::Row row = ph::rowOf(state);
		const uint32_t latency = ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate);
		REQUIRE(latency > 0u);
		auto dev = deviceWith(state.settings, rate);
		const std::vector<float> in = noise(4096, 0x0ff0ff0fu);
		std::vector<float> out = in;
		for (int n0 = 0; n0 < 4096; n0 += 300) { // an awkward block size, on purpose
			dev->processMono(out.data() + n0, std::min(300, 4096 - n0));
		}
		// SAMPLE FOR SAMPLE: silence while the ring fills, then the input itself.
		bool exact = true;
		for (size_t n = 0; n < in.size(); ++n) {
			const float expected = n < latency ? 0.0f : in[n - latency];
			exact = exact && out[n] == expected;
		}
		CHECK(exact);
	}
}

TEST_CASE("flanger: with Through Zero off the output is the plain flanger's own output, exactly the reported latency late") {
	// THE PRICE OF A LATENCY THAT THROUGH ZERO CANNOT MOVE (a TZ flip used to restart the host and kill
	// every held note): the plain flanger is now as late as through-zero. Asserted as a PURE DELAY, sample for sample,
	// of the output it made before it reported one (`setPlainUndelayedForTest`), with the LFO running, feedback,
	// Drive and stereo — so nothing but the delay changed, and the delay is exactly the latency the host is told.
	for (const double rate : { 44100.0, 48000.0, 96000.0 })
	for (const bool barber : { false, true }) {
		CAPTURE(rate);
		CAPTURE(barber);
		ph::State state;
		state.settings = plainFlanger(0.2f, 0.7f, false);
		state.settings.lfoOn = true;
		state.settings.lfoDepth = 0.8f;
		state.settings.drive = 0.4f;
		state.settings.mix = 0.6f;
		state.settings.sweep = barber ? ph::SweepShape(1) : ph::SweepShape::Normal;
		const ph::Row row = ph::rowOf(state);
		const uint32_t latency = ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate);
		REQUIRE(latency == static_cast<uint32_t>(std::lround(0.008 * rate)));
		auto late = deviceWith(state.settings, rate);
		auto undelayed = deviceWith(state.settings, rate);
		undelayed->setPlainUndelayedForTest(true);
		const std::vector<float> inL = noise(16384, 0x5eed1234u);
		const std::vector<float> inR = noise(16384, 0x0badf00du);
		std::vector<float> aL = inL, aR = inR, bL = inL, bR = inR;
		for (int n0 = 0; n0 < 16384; n0 += 300) { // an awkward block size, on purpose
			const int n = std::min(300, 16384 - n0);
			late->processStereo(aL.data() + n0, aR.data() + n0, n);
			undelayed->processStereo(bL.data() + n0, bR.data() + n0, n);
		}
		size_t mismatches = 0;
		double energy = 0.0;
		for (size_t n = 0; n < aL.size(); ++n) {
			const float wantL = n < latency ? 0.0f : bL[n - latency];
			const float wantR = n < latency ? 0.0f : bR[n - latency];
			mismatches += (aL[n] != wantL ? 1u : 0u) + (aR[n] != wantR ? 1u : 0u);
			energy += static_cast<double>(bL[n]) * bL[n];
		}
		CHECK(mismatches == 0u);
		// NON-VACUITY: the undelayed output is the flanger, not the input or silence.
		CHECK(energy > 1.0);
		size_t differsFromInput = 0;
		for (size_t n = 0; n < bL.size(); ++n) {
			differsFromInput += bL[n] != inL[n] ? 1u : 0u;
		}
		CHECK(differsFromInput > bL.size() / 2);
	}
}

TEST_CASE("flanger: the dry delay and the reported latency clamp together, and a rate that is no rate has none") {
	// 384 kHz: 8 ms is 3072 samples, inside the 4096-sample ring — a true 8 ms, and the report AGREES.
	ph::State state;
	state.settings = plainFlanger(0.0f, 0.0f, true);
	const ph::Row row = ph::rowOf(state);
	const uint32_t latency = ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), 384000.0);
	CHECK(latency == 3072u);
	CHECK(static_cast<double>(latency) / 384.0 == 8.0); // ms
	// PAST THE RING (768 kHz asks 6144) both clamp to it, together.
	CHECK(ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), 768000.0)
	      == static_cast<uint32_t>(ph::kDryLineSize));
	ph::Settings s = state.settings;
	s.mix = 0.0f; // the dry alone
	auto dev = deviceWith(s, 384000.0);
	std::vector<float> x(8192, 0.0f);
	x[0] = 1.0f;
	dev->processMono(x.data(), 8192);
	CHECK(dev->dryDelaySamplesForTest() == static_cast<int>(latency));
	CHECK(x[latency] == 1.0f);
	CHECK(std::count_if(x.begin(), x.end(), [](float v) { return v != 0.0f; }) == 1);
	// THE WET'S READ STAYS INSIDE ITS LINE at 384 kHz: Delay 15 ms swept up asks 20 ms = 7680 samples.
	ph::Settings top = plainFlanger(1.0f);
	top.delayLog2s = static_cast<float>(ph::kDelayLog2Max);
	auto high = deviceWith(top, 384000.0);
	std::vector<float> one(16, 0.0f);
	high->processMono(one.data(), 16);
	CHECK(high->delaySamplesForTest(0) <= static_cast<float>(ph::kLineSize - ph::kMinDelaySamples));
	CHECK(ph::flangerDelaySamples(ph::kDelayLog2Max, 1.0, 768000.0) == ph::kMaxWetReadSamples);
	// NO RATE, NO LATENCY.
	CHECK(ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), 0.0) == 0u);
	CHECK(ph::kAudioOps.latencySamples(nullptr, row.data(), row.size(), -48000.0) == 0u);
}

TEST_CASE("sinc tables: the default is the comb's 0.455 kernel unchanged, and B = 1 is a delta at a whole sample") {
	// THE DEFAULT, AGAINST THE KERNEL AS IT WAS WRITTEN BEFORE THE BANDWIDTH BECAME A PARAMETER — restated
	// here line for line, so the comb and the automodulator are provably bit-identical.
	const feathers::dsp::SincTable table {};
	using T = feathers::dsp::SincTable;
	const auto oldTap = [](double t) {
		const double u = t / static_cast<double>(T::kHalfTaps);
		if (!(std::abs(u) < 1.0)) {
			return 0.0;
		}
		const double x = 0.455 * t;
		const double sinc = (x == 0.0) ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
		const double window = 0.42 + 0.5 * std::cos(std::numbers::pi * u) + 0.08 * std::cos(2.0 * std::numbers::pi * u);
		return 0.455 * sinc * window;
	};
	bool same = true;
	for (int row = 0; row < T::kPhases; ++row) {
		float taps[T::kTaps];
		table.kernel(static_cast<float>(row) / static_cast<float>(T::kPhases), taps);
		for (int j = 0; j < T::kTaps; ++j) {
			const double t = static_cast<double>(T::kHalfTaps - j) - static_cast<double>(row) / T::kPhases;
			same = same && taps[j] == static_cast<float>(oldTap(t));
		}
	}
	CHECK(same);
	CHECK(table.bandwidth() == T::kBandwidth);
	// B = 1 AT frac 0: one tap of exactly 1, the rest exactly 0 — a whole-sample read is the sample.
	const T wide { 1.0 };
	float taps[T::kTaps];
	wide.kernel(0.0f, taps);
	for (int j = 0; j < T::kTaps; ++j) {
		CAPTURE(j);
		CHECK(taps[j] == (j == T::kHalfTaps ? 1.0f : 0.0f));
	}
}

//==================================================================================================
// THE MODULATION (the plan's Task 4): the LFO, the stereo offset, the follower, Position as the target.
//==================================================================================================


TEST_CASE("phaser: with both sources off the sweep sits exactly on Position") {
	// BOTH SOURCES OFF WITH THEIR AMOUNTS UP, and a loud input — so the follower HAS a level and the LFO HAS a
	// phase; were either term still in the sum, it would show.
	for (const float position : { -1.0f, -0.3f, 0.0f, 0.8f }) {
		CAPTURE(position);
		ph::Settings s = plainPhaser(6);
		s.lfoOn = false;
		s.lfoDepth = 0.9f;
		s.envOn = false;
		s.envAmount = 0.9f;
		auto dev = deviceWith(s);
		runStereo(*dev, 4800, 0.7f);
		REQUIRE(dev->envLevelForTest() > 0.5); // the follower is hearing the input
		// THE GLIDE FROM 0, then settled: 5 ms, landing exactly once within 1e-7 — half a second is far past it.
		s.position = position;
		dev->setTarget(s);
		runStereo(*dev, 24000, 0.7f);
		REQUIRE(dev->positionForTest() == static_cast<double>(position));
		for (int ch = 0; ch < 2; ++ch) {
			CAPTURE(ch);
			CHECK(dev->sweepForTest(ch) == static_cast<double>(position));
			// AND THE CORNER IS POSITION'S: 800 · 2^(3·Position), restated.
			const double fc = 800.0 * std::exp2(3.0 * static_cast<double>(position));
			CHECK(std::fabs(dev->cornerHzForTest(ch) - fc) / fc < 1.0e-5);
		}
	}
}

TEST_CASE("phaser: modulating Position moves the sweep when the sources are off") {
	// **A MODULATION ARRIVES AS A CHANGED FIELD VALUE THROUGH `setFields`.** The rack resolves each field's base
	// plus its routes' offsets into the slot's resolved row once a block (`RackEngine`'s `resolveInto`) and hands
	// that row to the device's `setFields` (rack_engine.cpp); this device declares no `windowedControls`, so
	// that is its ONLY road. So the external-modulation road is driven here exactly as the rack drives it: the
	// stored row with Position moved by the route's offset, through the ops table.
	ph::State state;
	state.settings = plainPhaser(6);
	const ph::Row base = ph::rowOf(state);
	REQUIRE(base[static_cast<size_t>(ph::Param::Position)] == 0.0f);
	alignas(ph::Phaser) unsigned char storage[sizeof(ph::Phaser)];
	ph::kAudioOps.construct(storage);
	ph::kAudioOps.prepare(storage, kRate, 4096);
	const auto& dev = *reinterpret_cast<const ph::Phaser*>(storage);
	std::vector<float> l(512, 0.25f);
	std::vector<float> r(512, 0.25f);
	for (const float offset : { 0.45f, -0.7f, 0.1f }) {
		CAPTURE(offset);
		ph::Row resolved = base;
		resolved[static_cast<size_t>(ph::Param::Position)] += offset;
		// A BLOCK AT A TIME, as the rack does: the resolved row, then the audio.
		for (int b = 0; b < 50; ++b) {
			ph::kAudioOps.setFields(storage, resolved.data(), resolved.size());
			ph::kAudioOps.processStereo(storage, l.data(), r.data(), 512);
		}
		for (int ch = 0; ch < 2; ++ch) {
			CHECK(dev.sweepForTest(ch) == static_cast<double>(offset));
			const double fc = 800.0 * std::exp2(3.0 * static_cast<double>(offset));
			CHECK(std::fabs(dev.cornerHzForTest(ch) - fc) / fc < 1.0e-5);
		}
	}
	ph::kAudioOps.destruct(storage);
}

TEST_CASE("phaser: synced LFO period is one division at the host tempo") {
	// THE INDEPENDENT RESTATEMENT: a division of `beats` at `spb` seconds a beat is one cycle every beats·spb
	// seconds, so the phase advances 1 / (beats · spb · sr) a sample. Measured over 4800 samples from the
	// engine's own phase.
	const auto advancePerSample = [](ph::Phaser& dev) {
		runStereo(dev, 16);
		const double before = dev.lfoPhaseForTest();
		runStereo(dev, 4800);
		return phaseAhead(before, dev.lfoPhaseForTest()) / 4800.0;
	};
	struct Case {
		const char* division;
		double beats;
		double bpm;
	};
	for (const Case c : { Case { "1/4", 1.0, 120.0 }, Case { "1/4", 1.0, 90.0 }, Case { "1/8", 0.5, 120.0 },
	                      Case { "1/8", 0.5, 90.0 }, Case { "1/8T", 0.5 * 2.0 / 3.0, 120.0 },
	                      Case { "1/4.", 1.5, 90.0 } }) {
		CAPTURE(c.division);
		CAPTURE(c.bpm);
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.sync = syncChoice(c.division);
		auto dev = deviceWith(s);
		const double spb = 60.0 / c.bpm;
		dev->setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(spb));
		const double expected = 1.0 / (c.beats * spb * kRate);
		CHECK(std::fabs(advancePerSample(*dev) - expected) < 1.0e-12);
	}
	SUBCASE("free runs at Rate, and a synced division with no tempo at one second a beat") {
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.rateLog2Hz = 1.0f; // 2 Hz
		auto free = deviceWith(s);
		CHECK(std::fabs(advancePerSample(*free) - 2.0 / kRate) < 1.0e-12);
		s.sync = syncChoice("1/4");
		auto noTempo = deviceWith(s);
		noTempo->setTransportClock(feathers::device::TransportClock::unknown());
		CHECK(std::fabs(advancePerSample(*noTempo) - 1.0 / kRate) < 1.0e-12);
	}
	SUBCASE("on a playing transport the phase is the song's: beat b of a division d is frac(b/d)") {
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.sync = syncChoice("1/8");
		const double period = 0.5;
		const double spb = 0.5;
		auto dev = deviceWith(s);
		feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(spb);
		clock.playing = true;
		clock.songPositionBeats = 37.3;
		dev->setTransportClock(clock);
		runStereo(*dev, 16);
		const auto frac = [](double v) { return v - std::floor(v); };
		CHECK(std::fabs(dev->lfoPhaseForTest() - frac(37.3 / period)) < 1.0e-9);
		// THE NEXT BLOCK, its clock where the song now is; its second tick is 16 samples further on.
		const double tickBeats = 16.0 / kRate / spb;
		clock.songPositionBeats = 37.3 + tickBeats;
		dev->setTransportClock(clock);
		runStereo(*dev, 32);
		CHECK(std::fabs(dev->lfoPhaseForTest() - frac((37.3 + 2.0 * tickBeats) / period)) < 1.0e-9);
		// A SEEK: the song jumps back, and the phase is the new position's.
		clock.songPositionBeats = 3.1;
		dev->setTransportClock(clock);
		runStereo(*dev, 16);
		CHECK(std::fabs(dev->lfoPhaseForTest() - frac(3.1 / period)) < 1.0e-9);
		// STOPPED, the same position is no position: it free-runs from where it was, one tick's worth on.
		clock.playing = false;
		dev->setTransportClock(clock);
		runStereo(*dev, 16);
		CHECK(std::fabs(dev->lfoPhaseForTest() - frac(3.1 / period + 16.0 / (period * spb * kRate))) < 1.0e-9);
	}
}

TEST_CASE("phaser: Stereo puts R's LFO phase Stereo/360 ahead of L's") {
	for (const float stereo : { 0.0f, 37.0f, 90.0f, 180.0f }) {
		CAPTURE(stereo);
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.lfoDepth = 0.5f;
		s.stereoDegrees = stereo;
		auto dev = deviceWith(s);
		runStereo(*dev, 4800, 0.3f);
		const double phiL = dev->lfoPhaseForTest(0);
		const double phiR = dev->lfoPhaseForTest(1);
		CHECK(std::fabs(phaseAhead(phiL, phiR) - std::fmod(stereo / 360.0, 1.0)) < 1.0e-12);
		// AND EACH CHANNEL'S SWEEP AND CORNER ARE ITS OWN PHASE'S: Position 0 + 0.5·sin(2πφ), 800·2^(3s).
		for (int ch = 0; ch < 2; ++ch) {
			CAPTURE(ch);
			const double phi = ch == 0 ? phiL : phiR;
			const double sweep = 0.5 * std::sin(2.0 * kPi * phi);
			CHECK(std::fabs(dev->sweepForTest(ch) - sweep) < 1.0e-12);
			const double fc = 800.0 * std::exp2(3.0 * sweep);
			CHECK(std::fabs(dev->cornerHzForTest(ch) - fc) / fc < 1.0e-5);
		}
		// NOT VACUOUS: at 90° (0.1 s in, φL ≈ 0.03) the two corners are far apart.
		if (stereo == 90.0f) {
			CHECK(std::fabs(dev->sweepForTest(1) - dev->sweepForTest(0)) > 0.3);
		}
	}
}

TEST_CASE("phaser: the envelope follower reaches the level of a held tone and falls with Release") {
	// A HELD DC "TONE" OF 0.5, so every tick's input peak is exactly 0.5 and the one-pole's closed form is exact:
	// k ticks of a = 1 − e^(−T/τ) close (1 − a)^k = e^(−t/τ) of the gap. Attack 10 ms, Release 200 ms, Sens 0.
	//
	// **THE LOOP IS HOT (fb 0.9), which is what makes this the input-only guard too**: at DC the cascade passes
	// 1 and the loop multiplies by 1/(1 − 0.9), so the OUTPUT is 5.5× the input — a follower reading it would
	// sit near 2.7, not 0.5.
	ph::Settings s = plainPhaser(6, 0.9f);
	s.envOn = true;
	s.envAmount = 1.0f;
	auto dev = deviceWith(s);
	constexpr double kA = 0.5;
	const double attack = 0.01;
	const double release = 0.2;
	runStereo(*dev, static_cast<int>(5.0 * attack * kRate), static_cast<float>(kA)); // 2400 samples, 150 ticks
	const double held = kA * (1.0 - std::exp(-5.0));
	CAPTURE(dev->envLevelForTest());
	CHECK(std::fabs(dev->envLevelForTest() - held) < 1.0e-6);
	// ITS READING IS THAT LEVEL IN dB OVER [−48, 0]: restated.
	CHECK(std::fabs(dev->envForTest() - (20.0 * std::log10(held) + 48.0) / 48.0) < 1.0e-6);
	// AND THE SWEEP READS IT: the next tick, Position 0 and the LFO off, is EnvAmount · env exactly.
	const double env = dev->envForTest();
	runStereo(*dev, 1, static_cast<float>(kA));
	CHECK(dev->sweepForTest(0) == env);
	CHECK(dev->sweepForTest(1) == env);
	// THE TONE STOPS (the one sample above was the 2401st, so 15 more close its tick).
	runStereo(*dev, 15, static_cast<float>(kA));
	const double stopped = dev->envLevelForTest();
	CHECK(std::fabs(stopped - kA * (1.0 - std::exp(-2416.0 / (attack * kRate)))) < 1.0e-6);
	runStereo(*dev, static_cast<int>(release * kRate)); // 9600 samples of silence, 600 ticks
	CHECK(std::fabs(dev->envLevelForTest() - stopped * std::exp(-1.0)) < 1.0e-6);
}

TEST_CASE("phaser: the follower hears the input only — silent input, ringing output, an envelope of exactly 0") {
	// A HOT FLANGER rings for over a second after its input stops (MEASURED: 137 blocks, see the tail budget).
	// With Release at its 10 ms bottom the follower reaches exact zero within ~0.35 s of silence — while the
	// output is still ringing. A follower that read the output would still be above zero.
	ph::Settings s = plainFlanger(0.4f, 0.9f);
	s.envOn = true;
	s.envReleaseLog2s = static_cast<float>(ph::kEnvReleaseLog2Min);
	auto dev = deviceWith(s);
	const std::vector<float> burst = noise(48000, 0x51e7ce5u, 0.8f);
	std::vector<float> l = burst;
	std::vector<float> r = burst;
	for (int n0 = 0; n0 < 48000; n0 += 512) {
		const int run = std::min(512, 48000 - n0);
		dev->processStereo(l.data() + n0, r.data() + n0, run);
	}
	REQUIRE(dev->envLevelForTest() > 0.1);
	runStereo(*dev, 28800); // 0.6 s of silence
	std::vector<float> ql(512, 0.0f);
	std::vector<float> qr(512, 0.0f);
	dev->processStereo(ql.data(), qr.data(), 512);
	// THE OUTPUT IS STILL RINGING — or the claim is vacuous.
	const float ring = *std::max_element(ql.begin(), ql.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); });
	CAPTURE(ring);
	REQUIRE(ring != 0.0f); // MEASURED ~6e-7 here: far above the follower's 1e-15 snap, so a reader of it is not 0
	CHECK(dev->envLevelForTest() == 0.0);
	CHECK(dev->envForTest() == 0.0);
}

TEST_CASE("phaser: the follower's Attack, Release and Sens move the sweep with Env on") {
	// THE HONESTY GUARD'S VALUE ASSERTION for the three controls gated on Env: each moves the follower's level
	// to its own closed form, and the sweep reads that level. DC of 0.25, 480 samples (30 ticks).
	constexpr double kA = 0.25;
	const auto levelAfter = [](const ph::Settings& s, int attackSamples, int silentSamples) {
		auto dev = deviceWith(s);
		runStereo(*dev, attackSamples, static_cast<float>(kA));
		runStereo(*dev, silentSamples);
		const double level = dev->envLevelForTest();
		// THE SWEEP READS IT, the next tick (Position 0, LFO off, amount 1).
		const double env = dev->envForTest();
		runStereo(*dev, 1);
		CHECK(dev->sweepForTest(0) == env);
		return level;
	};
	ph::Settings s = plainPhaser(6);
	s.envOn = true;
	s.envAmount = 1.0f;
	const double t = 480.0 / kRate;
	SUBCASE("Attack") {
		for (const double tau : { 0.0005, 0.2, 5.0 }) {
			CAPTURE(tau);
			s.envAttackLog2s = static_cast<float>(std::log2(tau));
			CHECK(std::fabs(levelAfter(s, 480, 0) - kA * (1.0 - std::exp(-t / tau))) < 1.0e-6);
		}
	}
	SUBCASE("Release") {
		s.envAttackLog2s = static_cast<float>(ph::kEnvAttackLog2Min);
		const double held = kA * (1.0 - std::exp(-t / 0.0005));
		for (const double tau : { 0.01, 2.0, 20.0 }) {
			CAPTURE(tau);
			s.envReleaseLog2s = static_cast<float>(std::log2(tau));
			CHECK(std::fabs(levelAfter(s, 480, 480) - held * std::exp(-t / tau)) < 1.0e-6);
		}
	}
	SUBCASE("a long Release: at t = tau the level is e^-1 of where it started (tau = 15 s)") {
		// THE LONG END: 15 s is 720 000 samples, 45 000 ticks at 48 kHz, and the one-pole closes
		// e^(−16/(τ·sr)) a tick — so at t = τ exactly e^(−1) is left. τ restated as the float the row stores.
		s.envAttackLog2s = static_cast<float>(ph::kEnvAttackLog2Min);
		s.envReleaseLog2s = static_cast<float>(std::log2(15.0));
		const double tau = std::exp2(static_cast<double>(s.envReleaseLog2s));
		const double held = kA * (1.0 - std::exp(-t / 0.0005));
		const double level = levelAfter(s, 480, 720000);
		const double expected = held * std::exp(-720000.0 / kRate / tau);
		CAPTURE(level);
		CAPTURE(expected);
		CHECK(std::fabs(level - expected) < 1.0e-9);
		CHECK(std::fabs(level / held - std::exp(-1.0)) < 1.0e-6);
	}
	SUBCASE("Sens") {
		for (const double db : { -24.0, 12.0 }) {
			CAPTURE(db);
			s.envSensDb = static_cast<float>(db);
			const double peak = kA * std::pow(10.0, db / 20.0);
			CHECK(std::fabs(levelAfter(s, 480, 0) - peak * (1.0 - std::exp(-t / 0.01))) < 1.0e-6);
		}
	}
}

TEST_CASE("phaser: LFO swings the corner by LfoDepth·3 octaves") {
	// OVER ONE LFO PERIOD (4 Hz, 750 ticks), the extremes of the corner the engine computes are
	// Center · 2^(±3·Depth). Ticks sample the sine every 1/750 of a cycle, so a peak is caught to within
	// 1 − cos(π/750) ≈ 9e-6 of the sweep — 2e-5 relative in Hz at full depth.
	for (const float depth : { 0.5f, 1.0f }) {
		CAPTURE(depth);
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.lfoDepth = depth;
		s.rateLog2Hz = 2.0f;
		auto dev = deviceWith(s);
		double lo = 1.0e9;
		double hi = 0.0;
		for (int tick = 0; tick < 750; ++tick) {
			runStereo(*dev, 16);
			lo = std::min(lo, dev->cornerHzForTest(0));
			hi = std::max(hi, dev->cornerHzForTest(0));
		}
		const double top = 800.0 * std::exp2(3.0 * depth);
		const double bottom = 800.0 * std::exp2(-3.0 * depth);
		CAPTURE(lo);
		CAPTURE(hi);
		CHECK(std::fabs(hi - top) / top < 1.0e-4);
		CHECK(std::fabs(lo - bottom) / bottom < 1.0e-4);
		// AND THE CORNER REPORTED IS THE ONE THE STAGES RUN: a tick's 16 samples ramp every stage's G onto
		// g/(1+g), g = tan(π·fc/sr) for the fc that tick computed.
		runStereo(*dev, 16);
		const double g = std::tan(kPi * dev->cornerHzForTest(0) / kRate);
		for (int i = 0; i < 6; ++i) {
			CHECK(std::fabs(static_cast<double>(dev->stageGForTest(0, i)) - g) / g < 1.0e-4);
		}
	}
}

TEST_CASE("phaser: readings report the engine's own values") {
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = plainFlanger(0.1f, 0.3f, mode == ph::Mode::Flanger);
		s.mode = mode;
		s.lfoOn = true;
		s.lfoDepth = 0.6f;
		s.envOn = true;
		s.envAmount = -0.4f;
		auto dev = deviceWith(s);
		Captured got;
		dev->setReadingsSink({ &got, &Captured::publish });
		const std::vector<float> in = noise(4800, 0xbeadu, 0.4f);
		std::vector<float> l = in;
		std::vector<float> r = in;
		for (int n0 = 0; n0 < 4800; n0 += 480) {
			dev->processStereo(l.data() + n0, r.data() + n0, 480);
		}
		REQUIRE(got.publishes == 10); // once per process call
		REQUIRE(got.values.size() == ph::kReadingCount);
		const auto at = [&got](ph::Reading rd) { return got.values[static_cast<size_t>(rd)]; };
		CHECK(at(ph::Reading::SweepL) == static_cast<float>(dev->sweepForTest(0)));
		CHECK(at(ph::Reading::SweepR) == static_cast<float>(dev->sweepForTest(1)));
		CHECK(at(ph::Reading::LfoPhase) == static_cast<float>(dev->lfoPhaseForTest()));
		// ENV IS THE READING THE SWEEP USED, so the three are one tick's facts: SweepL restated from it.
		CHECK(at(ph::Reading::Env) == static_cast<float>(dev->envUsedForTest()));
		{
			const double lfo = 0.6 * std::sin(2.0 * kPi * dev->lfoPhaseForTest(0));
			const double sweep = std::clamp(0.1 + lfo + -0.4 * dev->envUsedForTest(), -1.0, 1.0);
			CHECK(std::fabs(static_cast<double>(at(ph::Reading::SweepL)) - sweep) < 1.0e-6);
		}
		CHECK(at(ph::Reading::CornerHzL) == static_cast<float>(dev->cornerHzForTest(0)));
		CHECK(at(ph::Reading::CornerHzR) == static_cast<float>(dev->cornerHzForTest(1)));
		// THE DELAY AS THE COMB HEARS IT: the wet's read, less the through-zero dry's — restated from the
		// engine's two delays, in ms.
		for (int ch = 0; ch < 2; ++ch) {
			const double relative = (dev->delayTargetSamplesForTest(ch) - dev->dryDelaySamplesForTest()) / kRate * 1000.0;
			CHECK(at(ch == 0 ? ph::Reading::DelayMsL : ph::Reading::DelayMsR) == static_cast<float>(relative));
			// AND IN THE FLANGER THAT IS THE DELAY THE LINE WAS READ AT on the tick's last sample (the block ends on a
			// tick boundary). The Phaser does not run the flanger's per-sample delay at all.
			if (mode == ph::Mode::Flanger) {
				CHECK(std::fabs(dev->delayTargetSamplesForTest(ch) - dev->delaySamplesForTest(ch)) < 1.0e-3);
			}
		}
		CHECK(at(ph::Reading::NoteHz) == 0.0f);
		CHECK(at(ph::Reading::BarberPhase) == 0.0f);
		// NOT VACUOUS: the two channels' sweeps differ (Stereo 90°), and the follower is reading something.
		CHECK(at(ph::Reading::SweepL) != at(ph::Reading::SweepR));
		CHECK(at(ph::Reading::Env) > 0.0f);
		// AND THROUGH THE OPS TABLE, the road the rack installs.
		CHECK(ph::kAudioOps.setReadingsSink != nullptr);
		CHECK(ph::kAudioOps.setTransportClock != nullptr);
	}
}

namespace {

/// **THE FLANGER'S ZIPPER, MEASURED** — the per-tick delay against a per-sample one, through the SAME line and
/// kernel, so the only difference between the two is the delay trajectory. Returns, in dB re the reference's
/// whole power, the error's power in the bands ±`halfBand` Hz around carrier ± `offsetHz` (the lower one folded
/// to positive frequency), and the error's whole power. Wet only, no feedback, LFO at `rateHz`, Depth 1.
struct Zipper {
	double bandDb;
	double totalDb;
};
Zipper flangerZipper(double carrierHz, double rateHz, double delayMs, double offsetHz, double halfBand) {
	ph::Settings s = plainFlanger(0.0f, 0.0f, false);
	s.mix = 1.0f;
	s.lfoOn = true;
	s.lfoDepth = 1.0f;
	s.stereoDegrees = 0.0f;
	s.rateLog2Hz = static_cast<float>(std::log2(rateHz));
	s.delayLog2s = static_cast<float>(std::log2(delayMs * 0.001));
	auto dev = deviceWith(s);
	dev->setPlainUndelayedForTest(true); // the sweep's own read, aligned with the reference's (no reported latency)
	const feathers::dsp::SincTable table { ph::kFlangerBandwidth };
	auto line = std::make_unique<feathers::dsp::SincDelayLine<ph::kLineSize>>(table);
	// THE ENGINE'S LFO, PER SAMPLE: tick k uses phase k·16·f/sr, and its ramp LANDS on that tick's delay at the
	// tick's last sample (16k + 15) — so the per-sample reference reads phase (n − 15)·f/sr at sample n. The
	// engine's ramp is the straight line through those landing points; the reference is the curve.
	const double cps = std::exp2(static_cast<double>(s.rateLog2Hz));
	const double delayLog2 = static_cast<double>(s.delayLog2s);
	const int settle = static_cast<int>(0.25 * kRate);
	const int n = static_cast<int>(kRate); // one second: 1 Hz bins
	const int total = settle + n;
	std::vector<double> err(static_cast<size_t>(n));
	std::vector<double> ref(static_cast<size_t>(n));
	std::vector<float> block(512);
	for (int n0 = 0; n0 < total; n0 += 512) {
		const int frames = std::min(512, total - n0);
		for (int j = 0; j < frames; ++j) {
			block[static_cast<size_t>(j)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * carrierHz * (n0 + j) / kRate));
		}
		const std::vector<float> x(block.begin(), block.begin() + frames);
		dev->processMono(block.data(), frames);
		for (int j = 0; j < frames; ++j) {
			const int at = n0 + j;
			const double phase = static_cast<double>(std::max(at - 15, 0)) * cps / kRate;
			const double sweep = std::sin(2.0 * kPi * (phase - std::floor(phase)));
			const double d = ph::flangerDelaySamples(delayLog2, sweep, kRate);
			const float r = line->read(static_cast<float>(d));
			line->write(x[static_cast<size_t>(j)]);
			if (at >= settle) {
				ref[static_cast<size_t>(at - settle)] = r;
				err[static_cast<size_t>(at - settle)] = static_cast<double>(block[static_cast<size_t>(j)]) - r;
			}
		}
	}
	// HANN-WINDOWED, Parseval for the reference's whole power; a direct DFT at whole hertz in the bands.
	double refPower = 0.0;
	double errPower = 0.0;
	std::vector<double> w(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
		refPower += std::pow(w[static_cast<size_t>(i)] * ref[static_cast<size_t>(i)], 2.0);
		errPower += std::pow(w[static_cast<size_t>(i)] * err[static_cast<size_t>(i)], 2.0);
	}
	double band = 0.0;
	for (const double centre : { std::fabs(carrierHz + offsetHz), std::fabs(carrierHz - offsetHz) }) {
		for (int f = static_cast<int>(centre - halfBand); f <= static_cast<int>(centre + halfBand); ++f) {
			if (f <= 0 || f >= static_cast<int>(kRate / 2)) {
				continue;
			}
			std::complex<double> acc {};
			const std::complex<double> step = std::polar(1.0, -2.0 * kPi * f / kRate);
			std::complex<double> rot { 1.0, 0.0 };
			for (int i = 0; i < n; ++i) {
				acc += w[static_cast<size_t>(i)] * err[static_cast<size_t>(i)] * rot;
				rot *= step;
				if ((i & 4095) == 4095) {
					rot /= std::abs(rot);
				}
			}
			band += 2.0 * std::norm(acc) / n; // one-sided bins against Parseval's two-sided whole
		}
	}
	return { 10.0 * std::log10(band / refPower), 10.0 * std::log10(errPower / refPower) };
}

} // namespace

TEST_CASE("flanger: the delay sweeps per sample, so there is no zipper at the tick rate") {
	// MEASURED 2026-10-03 with the delay ramped linearly between ticks (the engine before the per-sample delay):
	// the ±3 kHz bands at −25.6 dB (1 kHz carrier) and −24.1 dB (10 kHz), the whole error −21.2 and −14.5 dB.
	// With the delay evaluated per sample: −160.9 and −161.5 dB in the bands, −150.1 and −150.7 dB whole — float
	// rounding. The bar is the review's −60 dB.
	for (const double carrier : { 1000.0, 10000.0 }) {
		const Zipper z = flangerZipper(carrier, 20.0, 10.0, 3000.0, 500.0);
		INFO("carrier " << carrier << " Hz: ±3 kHz bands " << z.bandDb << " dB, whole error " << z.totalDb
		                 << " dB re the carrier");
		CHECK(z.bandDb < -60.0);
	}
}

TEST_CASE("phaser: Free on a playing transport follows the song's seconds, and a Rate change is FM, not a jump") {
	// AUTOMOD'S RULE (Free always locks), its seconds road: the phase at song second
	// t is frac(t · Rate) — an independent restatement — wherever the playhead was dropped.
	ph::Settings s = plainPhaser(6);
	s.lfoOn = true;
	s.rateLog2Hz = -1.0f; // 0.5 Hz
	auto dev = deviceWith(s);
	const auto frac = [](double v) { return v - std::floor(v); };
	feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
	clock.playing = true;
	double t = 100.3; // far into the song, so a re-derived phase would jump by a lot
	const double tick = 16.0 / kRate;
	const auto block = [&](int ticks) {
		clock.songPositionSeconds = t;
		dev->setTransportClock(clock);
		runStereo(*dev, 16 * ticks);
		t += tick * ticks;
	};
	block(1);
	CHECK(std::fabs(dev->lfoPhaseForTest() - frac(100.3 * 0.5)) < 1.0e-9);
	block(30); // it started a tick on from the first, so its last tick stood at 100.3 + 30 ticks
	CHECK(std::fabs(dev->lfoPhaseForTest() - frac((100.3 + 30.0 * tick) * 0.5)) < 1.0e-9);
	// RATE JUMPS TO 2 Hz (gliding there over 20 ms): every tick's phase moves on by no more than one tick's
	// advance at the faster rate — followed, so the shape is frequency-modulated. A phase re-derived as
	// position / window would leap by 100 s × (2 − 0.5) Hz of turns' worth here.
	s.rateLog2Hz = 1.0f;
	dev->setTarget(s);
	double before = dev->lfoPhaseForTest();
	double largest = 0.0;
	for (int k = 0; k < 600; ++k) {
		block(1);
		double step = dev->lfoPhaseForTest() - before;
		step -= std::floor(step + 0.5); // to [−0.5, 0.5)
		largest = std::max(largest, std::fabs(step));
		before = dev->lfoPhaseForTest();
	}
	CAPTURE(largest);
	CHECK(largest <= 2.0 * tick + 1.0e-12);
	CHECK(largest > 1.9 * tick); // and it did reach the new rate: not vacuous
}

TEST_CASE("phaser: a transport with a nonsense tempo cannot put a NaN in the sweep") {
	// 1e-300 SECONDS A BEAT: the beats position of a later tick in the block is infinite, and so is every
	// lock arithmetic built on it. The phase starts over, the sweep falls back, the output stays finite.
	for (const bool synced : { true, false }) {
		CAPTURE(synced);
		ph::Settings s = plainPhaser(6, 0.5f);
		s.lfoOn = true;
		s.lfoDepth = 1.0f;
		if (synced) {
			s.sync = syncChoice("1/8");
		}
		auto dev = deviceWith(s);
		feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(1.0e-300);
		clock.playing = true;
		clock.songPositionBeats = 10.0;
		clock.songPositionSeconds = 1.0e308;
		bool finite = true;
		for (int b = 0; b < 20; ++b) {
			dev->setTransportClock(clock);
			std::vector<float> l = noise(512, 0x1000u + static_cast<uint32_t>(b), 0.5f);
			std::vector<float> r = l;
			dev->processStereo(l.data(), r.data(), 512);
			for (size_t j = 0; j < l.size(); ++j) {
				finite = finite && std::isfinite(l[j]) && std::isfinite(r[j]);
			}
			for (int ch = 0; ch < 2; ++ch) {
				finite = finite && std::isfinite(dev->sweepForTest(ch)) && std::isfinite(dev->lfoPhaseForTest(ch))
				         && std::isfinite(dev->cornerHzForTest(ch));
			}
		}
		CHECK(finite);
		CHECK(dev->lfoPhaseForTest() >= 0.0);
		CHECK(dev->lfoPhaseForTest() < 1.0);
	}
	// AND sweepOf ITSELF: a non-finite term falls back to Position.
	CHECK(ph::sweepOf(0.25, std::numeric_limits<double>::quiet_NaN()) == 0.25);
	CHECK(ph::sweepOf(-0.5, 0.1, std::numeric_limits<double>::infinity()) == -0.5);
}

TEST_CASE("phaser: a source switched off leaves the sweep exactly on Position within 50 ms") {
	for (const bool lfo : { true, false }) {
		CAPTURE(lfo);
		ph::Settings s = plainPhaser(6);
		s.position = 0.3f;
		s.lfoOn = lfo;
		s.lfoDepth = 1.0f;
		s.envOn = !lfo;
		s.envAmount = -1.0f;
		auto dev = deviceWith(s);
		runStereo(*dev, 9600, 0.8f);
		REQUIRE(std::fabs(dev->sweepForTest(0) - 0.3) > 0.05); // the source WAS moving it
		s.lfoOn = false;
		s.envOn = false;
		dev->setTarget(s);
		runStereo(*dev, 2400, 0.8f); // 50 ms
		CHECK(dev->sweepForTest(0) == static_cast<double>(0.3f));
		CHECK(dev->sweepForTest(1) == static_cast<double>(0.3f));
	}
}

TEST_CASE("phaser: readings are published while switched off too") {
	ph::Settings s = plainPhaser(6);
	s.lfoOn = true;
	auto dev = deviceWith(s);
	Captured got;
	dev->setReadingsSink({ &got, &Captured::publish });
	runStereo(*dev, 512);
	REQUIRE(got.publishes == 1);
	const double sweep = dev->sweepForTest(0);
	s.enabled = false;
	dev->setTarget(s);
	runStereo(*dev, 1024); // two calls
	CHECK(got.publishes == 3);
	REQUIRE(got.values.size() == ph::kReadingCount);
	// WHERE IT STANDS: the last sweep it ran.
	CHECK(got.values[static_cast<size_t>(ph::Reading::SweepL)] == static_cast<float>(sweep));
}

//==================================================================================================
// THE BARBERPOLE (the plan's Task 5): Sweep = Up | Down, three voices at staggered ramp phases.
//==================================================================================================

namespace {

/// THE RAMP PHASES, RESTATED: voice k leads voice 0 by k/3 of a cycle.
double voicePhaseOf(double psi, int k) {
	const double p = psi + static_cast<double>(k) / 3.0;
	return p - std::floor(p);
}

/// THE WINDOW, RESTATED FROM ITS DEFINITION: (2/3)·sin²(π ψ_k).
double windowOf(double psiK) {
	const double s = std::sin(kPi * psiK);
	return 2.0 / 3.0 * s * s;
}

/// ψ FROM THE LFO's PHASE φ, restated: Up rides φ, Down rides 1 − φ.
double rampPhaseOf(double phi, ph::SweepShape sweep) {
	const double p = sweep == ph::SweepShape::Down ? 1.0 - phi : phi;
	return p - std::floor(p);
}

/// A barberpole at Position 0 and a running LFO; Env off, the loop light.
/// **THE BARBERPOLE'S OWN PHASE ρ AFTER `ticks` ADVANCES**, free-running from 0, restated from the rule
/// (2026-10-03): ψ turns at Rate·(0.5 / max(Depth, 1/64)), capped at 8 Hz, so the climb is Depth's to leave alone. Depth
/// as the row stores it (a float).
double barberRateOf(double rateHz, float depth) {
	return std::min(rateHz * 0.5 / std::max(static_cast<double>(depth), 1.0 / 64.0), 8.0);
}
double rhoAfter(int ticks, double rateHz, float depth) {
	const double turns = static_cast<double>(ticks) * 16.0 * barberRateOf(rateHz, depth) / kRate;
	return turns - std::floor(turns);
}

ph::Settings barberpole(ph::Mode mode, ph::SweepShape sweep, float depth = 0.5f, float rateLog2Hz = 1.0f) {
	ph::Settings s = plainPhaser(6, 0.3f);
	s.mode = mode;
	s.sweep = sweep;
	s.lfoOn = true;
	s.lfoDepth = depth;
	s.rateLog2Hz = rateLog2Hz;
	return s;
}

} // namespace

TEST_CASE("barberpole: the three window gains sum to one at every phase") {
	// THE IDENTITY sin²x + sin²(x + π/3) + sin²(x + 2π/3) = 3/2, so (2/3)·Σ = 1. Each gain against the
	// definition restated, then the sum.
	double worst = 0.0;
	for (int i = 0; i < 10000; ++i) {
		const double psi = static_cast<double>(i) / 10000.0;
		double sum = 0.0;
		for (int k = 0; k < ph::kBarberVoices; ++k) {
			const double psiK = ph::barberVoicePhase(psi, k);
			CHECK(std::fabs(psiK - voicePhaseOf(psi, k)) < 1.0e-15);
			const double w = ph::barberGain(psiK);
			CHECK(std::fabs(w - windowOf(voicePhaseOf(psi, k))) < 1.0e-12);
			sum += w;
		}
		worst = std::max(worst, std::fabs(sum - 1.0));
	}
	CAPTURE(worst);
	CHECK(worst < 1.0e-6);
	// AND THE WINDOW IS ZERO EXACTLY WHERE A VOICE WRAPS, and whole at the middle of its travel.
	CHECK(ph::barberGain(0.0) == 0.0);
	CHECK(std::fabs(ph::barberGain(0.5) - 2.0 / 3.0) < 1.0e-15);
	// ψ FROM THE BARBERPOLE'S PHASE ρ: Up is ρ, Down is 1 − ρ, and both stay in [0, 1).
	for (const double phi : { 0.0, 0.25, 0.9 }) {
		CHECK(ph::barberPhaseOf(phi, ph::SweepShape::Up) == phi);
		CHECK(std::fabs(ph::barberPhaseOf(phi, ph::SweepShape::Down) - rampPhaseOf(phi, ph::SweepShape::Down)) < 1.0e-15);
	}
	CHECK(ph::barberPhaseOf(0.0, ph::SweepShape::Down) == 0.0);
}

TEST_CASE("barberpole: the sweep rises monotonically for ten cycles") {
	// **WHAT RISES, AND WHAT DOES NOT.** Every VOICE's sweep rises by exactly one tick's travel a tick
	// (2·Depth·Δψ, Δψ = Rate·16/sr), for ever, and steps back down by 2·Depth only at its own wrap — where its
	// window is (2/3)·sin²(π·Δψ) or less, i.e. silent. The W-WEIGHTED CENTRE does NOT rise: the set of voice
	// phases is the same set every third of a cycle, so Σ w_k·s_k is PERIODIC with period 1/3 — it ripples
	// about Position and never drifts. That stationarity is the barberpole: the motion is endless because the
	// centre stays put. So the monotonic claim is made of each voice (and of ψ, unwrapped), and the centre is
	// asserted stationary — inside the ripple the window algebra allows, restated below.
	constexpr double kDepth = 0.5;
	constexpr double kRateHz = 2.0;
	const double travel = kRateHz * 16.0 / kRate; // Δψ a tick
	// THE CENTRE'S RIPPLE, restated: max over ψ of |Σ w_k·(2ψ_k − 1)|.
	double ripple = 0.0;
	for (int i = 0; i < 3000; ++i) {
		const double psi = static_cast<double>(i) / 3000.0;
		double c = 0.0;
		for (int k = 0; k < 3; ++k) {
			c += windowOf(voicePhaseOf(psi, k)) * (2.0 * voicePhaseOf(psi, k) - 1.0);
		}
		ripple = std::max(ripple, std::fabs(c));
	}
	// Each voice's sweep is Depth·(2ψ_k − 1), so the centre is Depth·c(ψ) and stays within ±Depth·ripple.
	MESSAGE("the w-weighted centre's ripple: ±" << ripple << "·Depth");
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(static_cast<int>(sweep));
			const double dir = sweep == ph::SweepShape::Up ? 1.0 : -1.0;
			auto dev = deviceWith(barberpole(mode, sweep, static_cast<float>(kDepth), static_cast<float>(std::log2(kRateHz))));
			runStereo(*dev, 16, 0.1f);
			REQUIRE(dev->voicesRunningForTest() == 3);
			double prevS[3][2];
			double prevPsi[2];
			for (int k = 0; k < 3; ++k) {
				for (int ch = 0; ch < 2; ++ch) {
					prevS[k][ch] = dev->voiceSweepForTest(k, ch);
				}
			}
			for (int ch = 0; ch < 2; ++ch) {
				prevPsi[ch] = dev->barberPhaseForTest(ch);
			}
			double unwrapped = 0.0;
			int wraps[3][2] = {};
			int badSteps = 0;
			int loudWraps = 0;
			double centreLo = 1.0e9;
			double centreHi = -1.0e9;
			double prevCentre[2] = { dev->barberSweepForTest(0), dev->barberSweepForTest(1) };
			int bigCentreSteps = 0;
			const int ticks = static_cast<int>(std::ceil(10.0 / travel)) + 1; // ten cycles
			for (int tick = 0; tick < ticks; ++tick) {
				runStereo(*dev, 16, 0.1f);
				for (int ch = 0; ch < 2; ++ch) {
					// ψ MOVES ONE TICK'S TRAVEL, in the sweep's direction.
					double dpsi = dev->barberPhaseForTest(ch) - prevPsi[ch];
					dpsi -= std::floor(dpsi + 0.5);
					if (std::fabs(dpsi - dir * travel) > 1.0e-9) {
						++badSteps;
					}
					if (ch == 0) {
						unwrapped += dpsi;
					}
					prevPsi[ch] = dev->barberPhaseForTest(ch);
					double centre = 0.0;
					for (int k = 0; k < 3; ++k) {
						const double s = dev->voiceSweepForTest(k, ch);
						const double ds = s - prevS[k][ch];
						prevS[k][ch] = s;
						centre += dev->voiceGainForTest(k, ch) * s;
						if (ds * dir < 0.0) {
							// THE WRAP: back the whole span, less the tick's travel — and silent.
							++wraps[k][ch];
							if (std::fabs(std::fabs(ds) - 2.0 * kDepth * (1.0 - travel)) > 1.0e-9) {
								++badSteps;
							}
							const double bound = 2.0 / 3.0 * std::pow(std::sin(kPi * travel), 2.0) + 1.0e-12;
							if (dev->voiceGainForTest(k, ch) > bound) {
								++loudWraps;
							}
						}
						else if (std::fabs(ds - dir * 2.0 * kDepth * travel) > 1.0e-9) {
							++badSteps; // never backwards, never more than one tick's travel
						}
					}
					// THE CENTRE IS THE ENGINE'S `barberSweepForTest`, and it is stationary.
					CHECK(std::fabs(dev->barberSweepForTest(ch) - centre) < 1.0e-9);
					centreLo = std::min(centreLo, centre);
					centreHi = std::max(centreHi, centre);
					// THE PLAN'S LITERAL CLAIM: the centre never jumps by more than one tick's travel, 2·Depth·Δψ.
					if (std::fabs(centre - prevCentre[ch]) > 2.0 * kDepth * travel + 1.0e-9) {
						++bigCentreSteps;
					}
					prevCentre[ch] = centre;
				}
			}
			CHECK(bigCentreSteps == 0);
			CHECK(badSteps == 0);
			CHECK(loudWraps == 0);
			// TEN CYCLES, in the sweep's direction.
			CHECK(unwrapped * dir >= 10.0);
			CHECK(std::fabs(unwrapped - dir * travel * ticks) < 1.0e-6);
			for (int k = 0; k < 3; ++k) {
				for (int ch = 0; ch < 2; ++ch) {
					CHECK(wraps[k][ch] >= 10);
					CHECK(wraps[k][ch] <= 11);
				}
			}
			CAPTURE(centreLo);
			CAPTURE(centreHi);
			CHECK(centreLo >= -kDepth * ripple - 1.0e-6);
			CHECK(centreHi <= kDepth * ripple + 1.0e-6);
			// NOT VACUOUS: the centre does ripple, nearly across the whole band the algebra allows.
			CHECK(centreHi - centreLo > 1.5 * kDepth * ripple);
		}
	}
}

TEST_CASE("barberpole: no gain step at a voice's wrap") {
	SUBCASE("the phaser, a tick at a time: the voice that wraps has w < 1e-6 at that tick") {
		// 1 Hz: Δψ = 16/48000 a tick, so the window at the first tick past a wrap is at most
		// (2/3)·sin²(π/3000) = 7.3e-7.
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(sweep));
			auto dev = deviceWith(barberpole(ph::Mode::Phaser, sweep, 0.5f, 0.0f));
			runStereo(*dev, 16, 0.1f);
			double prev[3];
			for (int k = 0; k < 3; ++k) {
				prev[k] = dev->voicePhaseForTest(k, 0);
			}
			int wrapsSeen = 0;
			for (int tick = 0; tick < 3200; ++tick) {
				runStereo(*dev, 16, 0.1f);
				for (int k = 0; k < 3; ++k) {
					const double psi = dev->voicePhaseForTest(k, 0);
					const bool wrapped = sweep == ph::SweepShape::Up ? psi < prev[k] : psi > prev[k];
					prev[k] = psi;
					if (wrapped) {
						++wrapsSeen;
						CAPTURE(k);
						CAPTURE(tick);
						CHECK(dev->voiceGainForTest(k, 0) < 1.0e-6);
						// THE GAIN THE CASCADE RAN its tick's last sample with: the ramp has landed there.
						CHECK(std::fabs(dev->voiceGainRampForTest(k, 0)) < 1.0e-6);
						// NOT VACUOUS: the voice's corner did jump the whole sweep at this tick.
						CHECK(std::fabs(dev->voiceSweepForTest(k, 0) - (sweep == ph::SweepShape::Up ? -0.5 : 0.5)) < 1.0e-3);
					}
				}
			}
			CHECK(wrapsSeen >= 3); // a cycle and a bit: every voice wraps at least once
		}
	}
	SUBCASE("the flanger, a sample at a time: the tap that wraps has w < 1e-6 at that sample") {
		// RATE 4 Hz AT DEPTH 1: ψ turns at 4·0.5/1 = 2 Hz (the climb is Rate's, not the crossings), so Δψ = 2/48000 a
		// sample and the window at the first sample past a wrap is at most (2/3)·sin²(π·2/48000) = 1.1e-8.
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(sweep));
			auto dev = deviceWith(barberpole(ph::Mode::Flanger, sweep, 1.0f, 2.0f));
			std::vector<float> l(1, 0.1f);
			std::vector<float> r(1, 0.1f);
			dev->processStereo(l.data(), r.data(), 1);
			double prev[3];
			double prevDelay[3];
			for (int k = 0; k < 3; ++k) {
				prev[k] = voicePhaseOf(dev->barberRunForTest(0), k);
				prevDelay[k] = dev->tapDelaySamplesForTest(k, 0);
			}
			int wrapsSeen = 0;
			for (int n = 0; n < 24000; ++n) {
				l[0] = 0.1f;
				r[0] = 0.1f;
				dev->processStereo(l.data(), r.data(), 1);
				for (int k = 0; k < 3; ++k) {
					const double psi = voicePhaseOf(dev->barberRunForTest(0), k);
					const bool wrapped = sweep == ph::SweepShape::Up ? psi < prev[k] : psi > prev[k];
					prev[k] = psi;
					const double delay = dev->tapDelaySamplesForTest(k, 0);
					if (wrapped) {
						++wrapsSeen;
						CAPTURE(k);
						CAPTURE(n);
						CHECK(dev->tapGainForTest(k, 0) < 1.0e-6);
						// NOT VACUOUS: the tap's read jumped from one end of the sweep to the other (2.5 ms·2^(±2)).
						CHECK(std::fabs(delay - prevDelay[k]) > 400.0);
					}
					prevDelay[k] = delay;
				}
			}
			CHECK(wrapsSeen >= 3); // half a second, a cycle of ψ: every tap wraps
		}
	}
}

TEST_CASE("barberpole: Rate sets the climb in octaves a second, and Depth does not change it") {
	// Under the barberpole, Depth used to widen the span AND speed the motion, because Rate set ψ's
	// crossings a second. They are decoupled by design. THE INDEPENDENT RESTATEMENT of the climb: a voice's corner is
	// Center·2^(3·s_k) and s_k = Position + Depth·(2ψ_k − 1), so a voice climbs 6·Depth octaves per turn of ψ; ψ turns
	// at Rate·(0.5/Depth); so the climb is 3·Rate octaves a second whatever the Depth. MEASURED here off the engine's
	// own voice corners, tick to tick, on a voice away from its wrap.
	constexpr double kRateHz = 0.5;
	const auto climbOf = [](float depth, ph::SweepShape sweep) {
		ph::Settings s = barberpole(ph::Mode::Phaser, sweep, depth, static_cast<float>(std::log2(kRateHz)));
		s.position = 0.0f;
		auto dev = deviceWith(s);
		runStereo(*dev, 16 * 7);
		// THE VOICE NEAREST THE MIDDLE OF ITS TRAVEL, so neither this tick nor the next is its wrap.
		int k = 0;
		for (int v = 1; v < 3; ++v) {
			if (std::fabs(dev->voicePhaseForTest(v, 0) - 0.5) < std::fabs(dev->voicePhaseForTest(k, 0) - 0.5)) {
				k = v;
			}
		}
		const double before = std::log2(dev->voiceCornerHzForTest(k, 0));
		runStereo(*dev, 16);
		const double after = std::log2(dev->voiceCornerHzForTest(k, 0));
		return (after - before) / (16.0 / kRate); // octaves a second
	};
	for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
		CAPTURE(static_cast<int>(sweep));
		const double dir = sweep == ph::SweepShape::Up ? 1.0 : -1.0;
		for (const float depth : { 0.25f, 0.5f, 1.0f }) {
			CAPTURE(depth);
			const double climb = climbOf(depth, sweep);
			CAPTURE(climb);
			CHECK(std::fabs(climb - dir * 3.0 * kRateHz) < 1.0e-6);
		}
	}
	SUBCASE("at Depth 0.5, ψ turns at Rate exactly and IS the LFO's φ, as before the change") {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			auto dev = deviceWith(barberpole(ph::Mode::Phaser, sweep, 0.5f, 1.0f)); // 2 Hz
			runStereo(*dev, 4800);
			CHECK(dev->barberRateHzForTest() == doctest::Approx(2.0).epsilon(1e-12));
			CHECK(dev->barberPhaseForTest(0) == rampPhaseOf(dev->lfoPhaseForTest(0), sweep));
		}
	}
	SUBCASE("the cap: ψ never turns faster than 8 Hz, at a tiny Depth or a fast Rate") {
		struct Case {
			float depth;
			double rateHz;
			double want;
		};
		for (const Case c : { Case { 0.01f, 2.0, 8.0 }, Case { 0.0f, 0.3, 8.0 }, Case { 0.5f, 20.0, 8.0 },
		                      Case { 0.1f, 1.0, 5.0 }, Case { 0.05f, 1.0, 8.0 } }) {
			CAPTURE(c.depth);
			CAPTURE(c.rateHz);
			auto dev = deviceWith(barberpole(ph::Mode::Phaser, ph::SweepShape::Up, c.depth, static_cast<float>(std::log2(c.rateHz))));
			runStereo(*dev, 160);
			CHECK(dev->barberRateHzForTest() == doctest::Approx(c.want).epsilon(1e-6));
			CHECK(dev->barberRateHzForTest() <= 8.0);
		}
	}
	SUBCASE("song-locked: on a playing transport ψ is the song's position over the SCALED period") {
		const auto frac = [](double v) { return v - std::floor(v); };
		// FREE RATE, the seconds road: ρ = frac(t · Rate·0.5/Depth) at song time t.
		{
			auto dev = deviceWith(barberpole(ph::Mode::Phaser, ph::SweepShape::Up, 0.8f, 0.0f)); // 1 Hz, Depth 0.8
			feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
			clock.playing = true;
			clock.songPositionSeconds = 13.37;
			dev->setTransportClock(clock);
			runStereo(*dev, 16);
			const double cps = 1.0 * 0.5 / static_cast<double>(0.8f);
			CHECK(std::fabs(dev->barberPhaseForTest(0) - frac(13.37 * cps)) < 1.0e-9);
			// AND NOT THE LFO'S: φ is frac(13.37 · 1).
			CHECK(std::fabs(dev->barberPhaseForTest(0) - dev->lfoPhaseForTest(0)) > 1.0e-3);
		}
		// A SYNCED DIVISION, the beats road: a 1/4 at Depth 0.25 is two crossings a beat — N·(Rate/ψ) = 0.5 beats.
		{
			ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Down, 0.25f, 0.0f);
			s.sync = syncChoice("1/4");
			auto dev = deviceWith(s);
			feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
			clock.playing = true;
			clock.songPositionBeats = 37.3;
			clock.songPositionSeconds = 37.3 * 0.5;
			dev->setTransportClock(clock);
			runStereo(*dev, 16);
			CHECK(std::fabs(dev->barberPhaseForTest(0) - rampPhaseOf(frac(37.3 / 0.5), ph::SweepShape::Down)) < 1.0e-9);
		}
	}
	SUBCASE("Depth moved mid-run: ψ never jumps further than a tick's advance at the faster rate") {
		ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Up, 1.0f, 1.0f); // 2 Hz, ψ at 1 Hz
		auto dev = deviceWith(s);
		runStereo(*dev, 1600);
		double prev = dev->barberPhaseForTest(0);
		double worst = 0.0;
		for (int tick = 0; tick < 400; ++tick) {
			if (tick == 100) {
				s.lfoDepth = 0.25f; // ψ to 4 Hz, through the Depth glide
				dev->setTarget(s);
			}
			runStereo(*dev, 16);
			double d = dev->barberPhaseForTest(0) - prev;
			d -= std::floor(d + 0.5);
			worst = std::max(worst, d);
			CHECK(d >= 0.0); // never backwards
			CHECK(d <= 4.0 * 16.0 / kRate + 1.0e-12);
			prev = dev->barberPhaseForTest(0);
		}
		// AND IT DID SPEED UP: the last step is the new rate's.
		CHECK(dev->barberRateHzForTest() == doctest::Approx(4.0).epsilon(1e-6));
		CHECK(worst > 3.9 * 16.0 / kRate);
	}
}

TEST_CASE("barberpole: the voices' corners are what s_k says") {
	// THE INDEPENDENT RESTATEMENT: ψ from the barberpole's own phase ρ — free-running from 0 at Rate·(0.5/Depth)
	// (`rhoAfter`), Up ρ, Down 1 − ρ — the right Stereo/360 on; ψ_k =
	// frac(ψ + k/3); s_k = Position + Depth·(2ψ_k − 1) + EnvAmount·env, clamped; fc_k = Center·2^(3·s_k). The
	// follower is ON with a negative amount, so a voice sweep that dropped the Env term would be off by
	// 0.4·env (> 0.1 here).
	for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
		CAPTURE(static_cast<int>(sweep));
		ph::Settings s = barberpole(ph::Mode::Phaser, sweep, 0.6f, 0.5f);
		s.position = 0.1f;
		s.envOn = true;
		s.envAmount = -0.4f;
		s.stereoDegrees = 90.0f;
		auto dev = deviceWith(s);
		Captured got;
		dev->setReadingsSink({ &got, &Captured::publish });
		const std::vector<float> in = noise(4800, 0x5eedu, 0.5f);
		std::vector<float> l = in;
		std::vector<float> r = in;
		for (int n0 = 0; n0 < 4800; n0 += 480) {
			dev->processStereo(l.data() + n0, r.data() + n0, 480);
		}
		const double env = dev->envUsedForTest();
		REQUIRE(0.4 * env > 0.1);
		// 4800 samples are 300 ticks; the last used ρ after 299 advances. Depth 0.6 is off the reference, so ψ is NOT φ.
		const double psiL = rampPhaseOf(rhoAfter(299, std::exp2(0.5), 0.6f), sweep);
		CHECK(std::fabs(psiL - rampPhaseOf(dev->lfoPhaseForTest(0), sweep)) > 1.0e-3);
		double logCorner[2] = {};
		for (int ch = 0; ch < 2; ++ch) {
			CAPTURE(ch);
			const double psi = ch == 0 ? psiL : (psiL + 0.25) - std::floor(psiL + 0.25);
			CHECK(std::fabs(dev->barberPhaseForTest(ch) - psi) < 1.0e-10);
			for (int k = 0; k < 3; ++k) {
				CAPTURE(k);
				const double psiK = voicePhaseOf(psi, k);
				const double sk = std::clamp(0.1 + 0.6 * (2.0 * psiK - 1.0) + -0.4 * env, -1.0, 1.0);
				CHECK(std::fabs(dev->voiceSweepForTest(k, ch) - sk) < 1.0e-6);
				const double fc = std::clamp(800.0 * std::exp2(3.0 * sk), 20.0, 0.45 * kRate);
				CHECK(std::fabs(dev->voiceCornerHzForTest(k, ch) - fc) / fc < 1.0e-5);
				const double w = windowOf(psiK);
				CHECK(std::fabs(dev->voiceGainForTest(k, ch) - w) < 1.0e-9);
				// AND THE STAGES RUN THAT CORNER: the block ended on a tick, so every stage's G has landed on it.
				const double g = std::tan(kPi * fc / kRate);
				for (int i = 0; i < 6; ++i) {
					CHECK(std::fabs(static_cast<double>(dev->voiceStageGForTest(k, ch, i)) - g) / g < 1.0e-4);
				}
				logCorner[ch] += w * std::log2(fc);
			}
		}
		// THE READINGS: CornerHz is the w-weighted log corner, BarberPhase is ψ.
		REQUIRE(got.values.size() == ph::kReadingCount);
		const auto at = [&got](ph::Reading rd) { return static_cast<double>(got.values[static_cast<size_t>(rd)]); };
		CHECK(std::fabs(at(ph::Reading::CornerHzL) - std::exp2(logCorner[0])) / std::exp2(logCorner[0]) < 1.0e-5);
		CHECK(std::fabs(at(ph::Reading::CornerHzR) - std::exp2(logCorner[1])) / std::exp2(logCorner[1]) < 1.0e-5);
		CHECK(std::fabs(at(ph::Reading::BarberPhase) - psiL) < 1.0e-6);
	}
}

TEST_CASE("barberpole under through-zero: each tap's relative delay is (2ψ_k − 1)·Delay") {
	// THE DECISION (2026-10-03): under through-zero the barberpole is the point — each tap's RELATIVE delay
	// ramps (2ψ_k − 1)·Delay through zero, endlessly, and its window is zero where it jumps from +Delay to
	// −Delay. Position 0, Depth 1, the default 2.5 ms = 120 samples. Restated from ψ's own phase (`rhoAfter`) at each
	// tick's last sample (the per-sample ψ passes through the tick's there), then per sample from ψ itself.
	for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
		CAPTURE(static_cast<int>(sweep));
		const double dir = sweep == ph::SweepShape::Up ? 1.0 : -1.0;
		ph::Settings s = barberpole(ph::Mode::Flanger, sweep, 1.0f, 0.0f); // 1 Hz
		s.throughZero = true;
		auto dev = deviceWith(s);
		Captured got;
		dev->setReadingsSink({ &got, &Captured::publish });
		runStereo(*dev, 16, 0.2f);
		REQUIRE(dev->dryDelaySamplesForTest() == 384);
		int crossings[3] = {};
		double prevRel[3] = {};
		for (int k = 0; k < 3; ++k) {
			prevRel[k] = dev->tapDelaySamplesForTest(k, 0) - 384.0;
		}
		// A CYCLE OF ψ: at Depth 1 it turns at Rate·0.5 (the climb kept at Depth 0.5's speed), so 6000 ticks at 1 Hz.
		for (int tick = 0; tick < 6000; ++tick) {
			runStereo(*dev, 16, 0.2f);
			// ψ RESTATED: ρ after tick + 1 advances (the first block ran one), free from 0.
			const double psiL = rampPhaseOf(rhoAfter(tick + 1, 1.0, 1.0f), sweep);
			for (int ch = 0; ch < 2; ++ch) {
				const double psi = ch == 0 ? psiL : psiL + 0.25;
				double weighted = 0.0;
				for (int k = 0; k < 3; ++k) {
					const double psiK = voicePhaseOf(psi, k);
					const double rel = dev->tapDelaySamplesForTest(k, ch) - dev->dryDelaySamplesForTest();
					// AT A WRAP the tick's own ψ_k and the sample's can sit either side of it; skip that one sample.
					if (psiK > 1.0e-6 && psiK < 1.0 - 1.0e-6) {
						CHECK(std::fabs(rel - (2.0 * psiK - 1.0) * 120.0) < 2.0e-3);
						CHECK(std::fabs(dev->tapGainForTest(k, ch) - windowOf(psiK)) < 1.0e-6);
					}
					weighted += windowOf(psiK) * (2.0 * psiK - 1.0) * 120.0;
					// (A tick can land EXACTLY on the crossing — ψ_k = 1/2 is on the tick grid at 1 Hz — so the sign is
					// compared with the last NON-zero reading.)
					if (ch == 0 && rel != 0.0) {
						if (rel * prevRel[k] < 0.0 && std::fabs(rel - prevRel[k]) < 120.0) {
							++crossings[k]; // a crossing THROUGH zero, not the wrap's jump
						}
						prevRel[k] = rel;
					}
				}
				// THE FLANGER'S READING OF ITS DELAY: the w-weighted relative delay.
				CHECK(std::fabs(dev->delaySamplesForTest(ch) - dev->dryDelaySamplesForTest() - weighted) < 2.0e-3);
			}
		}
		// EVERY TAP PASSES THROUGH ZERO, once a cycle.
		for (int k = 0; k < 3; ++k) {
			CHECK(crossings[k] == 1);
		}
		// THE READINGS: DelayMs is the w-weighted RELATIVE delay at the tick, in ms.
		REQUIRE(got.values.size() == ph::kReadingCount);
		double weightedMs = 0.0;
		const double psiL = rampPhaseOf(rhoAfter(6000, 1.0, 1.0f), sweep);
		for (int k = 0; k < 3; ++k) {
			weightedMs += windowOf(voicePhaseOf(psiL, k)) * (2.0 * voicePhaseOf(psiL, k) - 1.0) * 2.5;
		}
		CHECK(std::fabs(static_cast<double>(got.values[static_cast<size_t>(ph::Reading::DelayMsL)]) - weightedMs) < 1.0e-4);
		// PER SAMPLE, ψ moves Rate/sr a sample in the sweep's direction and the taps follow it.
		for (int n = 0; n < 40; ++n) {
			const double before = dev->barberRunForTest(0);
			runStereo(*dev, 1, 0.2f);
			double step = dev->barberRunForTest(0) - before;
			step -= std::floor(step + 0.5);
			CHECK(std::fabs(step - dir * 0.5 / kRate) < 1.0e-12); // ψ's rate: 1 Hz · 0.5 / Depth 1
			for (int k = 0; k < 3; ++k) {
				const double psiK = voicePhaseOf(dev->barberRunForTest(0), k);
				CHECK(std::fabs(dev->tapDelaySamplesForTest(k, 0) - 384.0 - (2.0 * psiK - 1.0) * 120.0) < 2.0e-3);
			}
		}
	}
}

TEST_CASE("flanger: a Sweep switch crossfades the read, so the wet never steps") {
	// **THE BOUND, DERIVED FROM THE STEADY STATE.** Wet only, no feedback, a sine of amplitude A at f. Away from a
	// switch the wet is Σ g_k·x(n − d_k(n)) with Σ g_k = 1 (one read at g = 1 in Normal, three windows under the
	// barberpole), so from one sample to the next it moves by at most
	//
	//     steady = 2A·sin(π·f·(1 + δ)/sr),
	//
	// a sine's largest one-sample step with its frequency raised by the reads' Doppler and the windows' motion;
	// δ = 0.1 covers both here (the delay moves under 0.06 samples a sample at 1 Hz, Depth 0.8, 2.5 ms, and the
	// windows by under 1e-4 a sample). ACROSS A SWITCH the crossfade's weight steps by 1/64 a sample between two reads
	// each at most A, and the convex blend of two reads that each move by at most `steady` moves by at most `steady`,
	// so the bound is steady + 2A/64. A STEPPED switch moves the wet by 2A·|sin(π·f·Δd/sr)| for the delay it jumps,
	// Δd — tens of samples here, so most of 2A.
	constexpr double kA = 0.5;
	constexpr double kHz = 1000.0;
	const double steady = 2.0 * kA * std::sin(kPi * kHz * 1.1 / kRate);
	const double bound = steady + 2.0 * kA / 64.0;
	for (const bool tz : { false, true }) {
		CAPTURE(tz);
		ph::Settings s = plainFlanger(0.0f, 0.0f, tz);
		s.mix = 1.0f;
		s.lfoOn = true;
		s.lfoDepth = 0.8f;
		s.rateLog2Hz = 0.0f; // 1 Hz
		s.stereoDegrees = 0.0f;
		auto dev = deviceWith(s);
		// EVERY SWITCH THE CONTROL OFFERS, at block edges off the tick grid (blocks of 100).
		const ph::SweepShape order[] = { ph::SweepShape::Up,     ph::SweepShape::Down, ph::SweepShape::Normal,
		                                 ph::SweepShape::Down,   ph::SweepShape::Up,   ph::SweepShape::Normal,
		                                 ph::SweepShape::Up,     ph::SweepShape::Normal };
		std::vector<float> l(100);
		std::vector<float> r(100);
		long n = 0;
		float prev = 0.0f;
		double steadyMax = 0.0;
		double switchMax = 0.0;
		int sinceSwitch = 1 << 20;
		const auto run = [&](int frames) {
			for (int j = 0; j < frames; ++j) {
				l[static_cast<size_t>(j)] = static_cast<float>(kA * std::sin(2.0 * kPi * kHz * static_cast<double>(n + j) / kRate));
				r[static_cast<size_t>(j)] = l[static_cast<size_t>(j)];
			}
			dev->processStereo(l.data(), r.data(), frames);
			for (int j = 0; j < frames; ++j) {
				const float y = l[static_cast<size_t>(j)];
				if (n + j > 4800) { // the line full
					// THE 200 SAMPLES THE SWITCH'S TICK AND ITS FADE LAND IN, against all the rest.
					double& worst = sinceSwitch < 200 ? switchMax : steadyMax;
					worst = std::max(worst, std::fabs(static_cast<double>(y - prev)));
				}
				prev = y;
				++sinceSwitch;
			}
			n += frames;
		};
		int fadesSeen = 0;
		for (int b = 0; b < 117; ++b) {
			run(100);
		}
		for (const ph::SweepShape sweep : order) {
			s.sweep = sweep;
			dev->setTarget(s);
			sinceSwitch = 0;
			// 20 SAMPLES hold the next tick, and the fade it starts runs 64: in flight when they end.
			run(20);
			if (dev->sweepFadeLeftForTest() > 0) {
				++fadesSeen;
			}
			for (int b = 0; b < 116; ++b) {
				run(100); // 11620 more: off any period of the LFO or the sine
			}
		}
		INFO("through-zero " << tz << ": steady max step " << steadyMax << " (bound " << steady << "), across switches "
		                      << switchMax << " (bound " << bound << ")");
		CHECK(fadesSeen == static_cast<int>(std::size(order))); // every switch started a fade
		CHECK(steadyMax <= steady); // the derivation's premise, measured
		CHECK(switchMax <= bound);
	}
}

TEST_CASE("barberpole: Sweep = Normal runs voice 0 alone, and the other voices' states stay exactly zero") {
	// NORMAL IS THE ENGINE BEFORE THE BARBERPOLE: one cascade a channel, its window (1, 0, 0) by construction —
	// the other two voices are not run at all, so a hot loop, a damped one and a driven one leave them at zero.
	ph::Settings s = plainPhaser(12, 0.9f);
	s.lfoOn = true;
	s.lfoDepth = 0.8f;
	s.drive = 0.3f;
	s.dampingLog2Hz = static_cast<float>(std::log2(2000.0));
	auto dev = deviceWith(s);
	const std::vector<float> in = noise(48000, 0xa11ce5u, 0.7f);
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (int n0 = 0; n0 < 48000; n0 += 512) {
		dev->processStereo(l.data() + n0, r.data() + n0, std::min(512, 48000 - n0));
	}
	CHECK(dev->voicesRunningForTest() == 1);
	for (int ch = 0; ch < 2; ++ch) {
		CAPTURE(ch);
		CHECK(dev->voiceStateMagnitudeForTest(0, ch) > 0.0); // not vacuous: voice 0 is ringing
		CHECK(dev->voiceStateMagnitudeForTest(1, ch) == 0.0);
		CHECK(dev->voiceStateMagnitudeForTest(2, ch) == 0.0);
		CHECK(dev->voiceGainForTest(0, ch) == 1.0);
		CHECK(dev->voiceGainForTest(1, ch) == 0.0);
		CHECK(dev->voiceGainForTest(2, ch) == 0.0);
		// AND VOICE 0 IS THE CHANNEL: its sweep and corner are the ones the Normal readings report.
		CHECK(dev->voiceSweepForTest(0, ch) == dev->sweepForTest(ch));
		CHECK(dev->voiceCornerHzForTest(0, ch) == dev->cornerHzForTest(ch));
		CHECK(dev->voiceStageGForTest(0, ch, 3) == dev->stageGForTest(ch, 3));
	}
	SUBCASE("entering the barberpole, the two voices join EMPTY — nothing from a past visit replays") {
		// A VISIT: Up on noise fills voices 1 and 2; back to Normal, they stop where they were.
		s.sweep = ph::SweepShape::Up;
		dev->setTarget(s);
		runStereo(*dev, 4800, 0.5f);
		REQUIRE(dev->voiceStateMagnitudeForTest(1, 0) > 0.0);
		s.sweep = ph::SweepShape::Normal;
		dev->setTarget(s);
		runStereo(*dev, 4800, 0.5f);
		CHECK(dev->voicesRunningForTest() == 1);
		REQUIRE(dev->voiceStateMagnitudeForTest(1, 0) > 0.0); // the stale state is there to replay
		// BACK TO Up, on SILENCE: the joining voices hear nothing and were cleared, so they stay exactly zero
		// (voice 0's own loop rings on, but it is its own).
		s.sweep = ph::SweepShape::Up;
		dev->setTarget(s);
		runStereo(*dev, 512);
		CHECK(dev->voicesRunningForTest() == 3);
		for (int ch = 0; ch < 2; ++ch) {
			CHECK(dev->voiceStateMagnitudeForTest(1, ch) == 0.0);
			CHECK(dev->voiceStateMagnitudeForTest(2, ch) == 0.0);
		}
		CHECK(dev->voiceStateMagnitudeForTest(0, 0) > 0.0);
	}
	SUBCASE("leaving it, the windows fade to (1, 0, 0) across one tick, and then voice 0 runs alone") {
		s.sweep = ph::SweepShape::Down;
		dev->setTarget(s);
		runStereo(*dev, 4800, 0.5f);
		s.sweep = ph::SweepShape::Normal;
		dev->setTarget(s);
		runStereo(*dev, 16, 0.5f); // the fading tick: three voices, their ramps landing on (1, 0, 0)
		CHECK(dev->voicesRunningForTest() == 3);
		CHECK(std::fabs(dev->voiceGainRampForTest(0, 0) - 1.0f) < 1.0e-6f);
		CHECK(std::fabs(dev->voiceGainRampForTest(1, 0)) < 1.0e-6f);
		CHECK(std::fabs(dev->voiceGainRampForTest(2, 0)) < 1.0e-6f);
		runStereo(*dev, 16, 0.5f);
		CHECK(dev->voicesRunningForTest() == 1);
	}
}

TEST_CASE("barberpole: with the LFO off it is the plain engine — every voice on Position, the windows summing to one") {
	// **THE DECISION**: the ramp IS the motion, so with no LFO amount (off, or Depth 0) there is no motion — each
	// voice sits on Position (plus the follower's term), and Σ w = 1 makes the sum the plain engine's, at three
	// times its cost. Nothing is switched: ψ keeps running, so turning the LFO back on finds the pole turning.
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		for (const bool tz : { false, true }) {
			if (mode == ph::Mode::Phaser && tz) {
				continue;
			}
			CAPTURE(static_cast<int>(mode));
			CAPTURE(tz);
			ph::Settings normal = plainPhaser(8, 0.6f);
			normal.mode = mode;
			normal.throughZero = tz;
			normal.position = 0.35f;
			normal.lfoOn = false;
			ph::Settings up = normal;
			up.sweep = ph::SweepShape::Up;
			auto a = deviceWith(normal);
			auto b = deviceWith(up);
			const std::vector<float> in = noise(24000, 0x0ddba11u, 0.5f);
			std::vector<float> la = in;
			std::vector<float> ra = in;
			std::vector<float> lb = in;
			std::vector<float> rb = in;
			for (int n0 = 0; n0 < 24000; n0 += 512) {
				const int run = std::min(512, 24000 - n0);
				a->processStereo(la.data() + n0, ra.data() + n0, run);
				b->processStereo(lb.data() + n0, rb.data() + n0, run);
			}
			CHECK(b->voicesRunningForTest() == 3);
			for (int k = 0; k < 3; ++k) {
				CHECK(b->voiceSweepForTest(k, 0) == static_cast<double>(0.35f));
			}
			float worst = 0.0f;
			for (size_t j = 0; j < in.size(); ++j) {
				worst = std::max({ worst, std::fabs(la[j] - lb[j]), std::fabs(ra[j] - rb[j]) });
			}
			CAPTURE(worst);
			CHECK(worst < 1.0e-5f);
		}
	}
}

TEST_CASE("barberpole: full feedback, full drive, twelve stages, both directions, both engines stays bounded") {
	const int frames = static_cast<int>(3.0 * kRate);
	const std::vector<float> l0 = noise(frames, 0x3141592u);
	const std::vector<float> r0 = noise(frames, 0x2718281u);
	struct Engine {
		ph::Mode mode;
		bool tz;
	};
	for (const Engine e : { Engine { ph::Mode::Phaser, false }, Engine { ph::Mode::Flanger, false },
	                        Engine { ph::Mode::Flanger, true } }) {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			for (const float drive : { 1.0f, 0.0f }) {
				for (const float fb : { 1.0f, -1.0f }) {
					CAPTURE(static_cast<int>(e.mode));
					CAPTURE(e.tz);
					CAPTURE(static_cast<int>(sweep));
					CAPTURE(drive);
					CAPTURE(fb);
					ph::Settings s = barberpole(e.mode, sweep, 1.0f, 1.0f);
					s.stages = 12;
					s.feedback = fb;
					s.drive = drive;
					s.throughZero = e.tz;
					s.position = 0.4f;
					auto dev = deviceWith(s);
					std::vector<float> l = l0;
					std::vector<float> r = r0;
					float peak = 0.0f;
					bool finite = true;
					for (int n0 = 0; n0 < frames; n0 += 512) {
						const int run = std::min(512, frames - n0);
						dev->processStereo(l.data() + n0, r.data() + n0, run);
						for (int j = n0; j < n0 + run; ++j) {
							finite = finite && std::isfinite(l[static_cast<size_t>(j)]) && std::isfinite(r[static_cast<size_t>(j)]);
							peak = std::max({ peak, std::fabs(l[static_cast<size_t>(j)]), std::fabs(r[static_cast<size_t>(j)]) });
						}
					}
					CAPTURE(peak);
					CHECK(finite);
					CHECK(dev->loopResetsForTest() == 0);
					CHECK(peak < (drive > 0.0f ? 8.0f : kLinearPeakBound));
				}
			}
		}
	}
}

TEST_CASE("barberpole: the tail reaches exact zero in both engines") {
	const std::vector<float> burst = noise(static_cast<int>(kRate), 0xbabe5u, 0.8f);
	struct Engine {
		ph::Mode mode;
		bool tz;
		int budget;
	};
	for (const Engine e : { Engine { ph::Mode::Phaser, false, kTailBudgetBlocks },
	                        Engine { ph::Mode::Flanger, false, kFlangerTailBudgetBlocks },
	                        Engine { ph::Mode::Flanger, true, kFlangerTailBudgetBlocks } }) {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(e.mode));
			CAPTURE(e.tz);
			CAPTURE(static_cast<int>(sweep));
			ph::Settings s = barberpole(e.mode, sweep, 1.0f, 1.0f);
			s.stages = 12;
			s.feedback = 0.9f;
			s.drive = 0.6f;
			s.throughZero = e.tz;
			auto dev = deviceWith(s);
			std::vector<float> l = burst;
			std::vector<float> r = burst;
			for (size_t n0 = 0; n0 < burst.size(); n0 += 512) {
				const int run = static_cast<int>(std::min<size_t>(512, burst.size() - n0));
				dev->processStereo(l.data() + n0, r.data() + n0, run);
			}
			int lastNonZero = -1;
			std::vector<float> bl(512);
			std::vector<float> br(512);
			for (int b = 0; b < e.budget * 2; ++b) {
				std::fill(bl.begin(), bl.end(), 0.0f);
				std::fill(br.begin(), br.end(), 0.0f);
				dev->processStereo(bl.data(), br.data(), 512);
				for (int j = 0; j < 512; ++j) {
					if (bl[static_cast<size_t>(j)] != 0.0f || br[static_cast<size_t>(j)] != 0.0f) {
						lastNonZero = b;
					}
				}
			}
			INFO("barberpole tail, mode " << static_cast<int>(e.mode) << " tz " << e.tz << " sweep "
			                               << static_cast<int>(sweep) << ": last non-zero block " << lastNonZero);
			CHECK(lastNonZero >= 0);
			CHECK(lastNonZero < e.budget);
		}
	}
}

//==================================================================================================
// KEY TRACKING (Task 6).
//==================================================================================================

namespace {

feathers::device::NoteContext noteOf(float pitch, bool gate) {
	feathers::device::NoteContext n;
	n.pitchSemitones = pitch;
	n.gate = gate;
	return n;
}

/// THE INDEPENDENT RESTATEMENT of the tracked frequency: equal temperament, A4 = 69 = 440 Hz, through `pow`
/// rather than the engine's `exp2`.
double trackedHz(double pitch, double offset = 0.0) {
	return 440.0 * std::pow(2.0, (pitch + offset - 69.0) / 12.0);
}

/// THE DEFAULT KNOBS AS A ROW STORES THEM — 2.5 ms and 800 Hz through a float log2, which is what the engine reads
/// (the float rounding is ~3e-7 of the value, far above the 1e-9 these cases hold the centre to).
double knobDelaySamples() { return std::exp2(static_cast<double>(static_cast<float>(std::log2(0.0025)))) * kRate; }
double knobCornerHz() { return std::exp2(static_cast<double>(static_cast<float>(std::log2(800.0)))); }

/// THE KNOBS AS OFFSETS FROM THE NOTE, restated from their definition: the stored float's distance
/// from the float its default stores as, in semitones — the phaser's 12·log2(Center/800 Hz), the flanger's
/// −12·log2(Delay/2.5 ms) — through `log2` of the hertz and seconds rather than the header's helper.
double storedCenterOffset(const ph::Settings& s) {
	const double ref = std::exp2(static_cast<double>(static_cast<float>(std::log2(800.0))));
	return 12.0 * std::log2(std::exp2(static_cast<double>(s.centerLog2Hz)) / ref);
}
double storedDelayOffset(const ph::Settings& s) {
	const double ref = std::exp2(static_cast<double>(static_cast<float>(std::log2(0.0025))));
	return -12.0 * std::log2(std::exp2(static_cast<double>(s.delayLog2s)) / ref);
}

/// One control tick, so the device computes its centre (the first tick PLACES every glide).
void oneTick(ph::Phaser& dev) {
	std::vector<float> one(16, 0.0f);
	dev.processMono(one.data(), 16);
}

/// THE DRY-PLUS-WET RESPONSE AT `hz` of the cascade the engine is running on `channel` — every stage at its OWN
/// coefficient, read back through `stageGForTest`, against `stageResponseOfG` (the closed form the earlier notch
/// case restates). Feedback 0 and Mix 0.5, so it is 0.5·(1 + M).
double notchDepthDb(const ph::Phaser& dev, int stages, double hz, int channel = 0) {
	std::complex<double> m { 1.0, 0.0 };
	for (int i = 0; i < stages; ++i) {
		m *= ph::stageResponseOfG(static_cast<double>(dev.stageGForTest(channel, i)), hz / kRate);
	}
	return dB(std::abs(0.5 * (1.0 + m)));
}

} // namespace

TEST_CASE("key track: flanger delay is 1000/noteHz") {
	// ONE PERIOD OF THE NOTE: 1000/f ms is kRate/f samples. The knob's 2.5 ms (120 samples) is nowhere near any of
	// these, so the knob cannot pass for the note. Note 36 is C2, 65.4 Hz, 15.29 ms — PAST the Delay knob's 15 ms
	// top and inside the engine's 20 ms, and in tune (`keyDelayLog2s`).
	struct Case {
		float note;
		float offset;
	};
	for (const Case c : { Case { 36.0f, 0.0f }, Case { 60.0f, 0.0f }, Case { 84.0f, 0.0f }, Case { 60.0f, 7.0f } }) {
		CAPTURE(c.note);
		CAPTURE(c.offset);
		ph::Settings s = plainFlanger();
		s.keyTrack = true;
		// THE OFFSET IS THE DELAY KNOB'S distance from its default, a longer delay lower (`kDelayKeyRefLog2`).
		s.delayLog2s = static_cast<float>(ph::kDelayKeyRefLog2 - static_cast<double>(c.offset) / 12.0);
		const double offset = storedDelayOffset(s);
		auto dev = deviceWith(s);
		dev->setNoteContext(noteOf(c.note, true));
		oneTick(*dev);
		const double hz = trackedHz(c.note, offset);
		const double expected = kRate / hz;
		CAPTURE(expected);
		// NoteHz IS THE NOTE'S OWN — the offset is the knob's, applied after.
		CHECK(std::fabs(dev->noteHzForTest() - trackedHz(c.note)) < 1.0e-9 * trackedHz(c.note));
		for (int ch = 0; ch < 2; ++ch) {
			// THE TICK'S TARGET, in double, and the delay the line was READ at on the tick's last sample (float).
			CHECK(std::fabs(dev->delayTargetSamplesForTest(ch) - expected) < 1.0e-9 * expected);
			CHECK(std::fabs(static_cast<double>(dev->delaySamplesForTest(ch)) - expected) < 1.0e-3);
		}
		CHECK(std::fabs(expected - 0.0025 * kRate) > 1.0);
	}
	SUBCASE("the sweep still moves around the note: Position 0.5 is one octave longer") {
		ph::Settings s = plainFlanger(0.5f);
		s.keyTrack = true;
		auto dev = deviceWith(s);
		dev->setNoteContext(noteOf(72.0f, true));
		oneTick(*dev);
		const double expected = kRate / trackedHz(72.0) * 2.0;
		CHECK(std::fabs(dev->delayTargetSamplesForTest(0) - expected) < 1.0e-9 * expected);
	}
	SUBCASE("through-zero: the RELATIVE delay at the sweep's extremes is one period, held under the dry") {
		// THE DECISION: under through-zero the note sets the SPAN, so the comb at s = ±1 is the note's and the crossing
		// at s = 0 is still a null. The span is held to the dry less kMinDelay — 384 − 6 = 378 samples at 48 kHz, so
		// note 84 (45.9 samples) is in tune and note 36 (733.9) clamps.
		const double dry = 0.008 * kRate;
		for (const float note : { 84.0f, 36.0f }) {
			for (const float position : { 1.0f, -0.6f, 0.0f }) {
				CAPTURE(note);
				CAPTURE(position);
				ph::Settings s = plainFlanger(position, 0.0f, true);
				s.keyTrack = true;
				auto dev = deviceWith(s);
				dev->setNoteContext(noteOf(note, true));
				oneTick(*dev);
				REQUIRE(dev->dryDelaySamplesForTest() == static_cast<int>(dry));
				const double span = std::min(kRate / trackedHz(note), dry - 6.0);
				const double relative = dev->delayTargetSamplesForTest(0) - dry;
				CHECK(std::fabs(relative - position * span) < 1.0e-9 * span);
			}
		}
	}
}

TEST_CASE("key track: the phaser's first notch lands on the note") {
	// **EXACT IN THE DIGITAL DOMAIN, NOT THE ANALOG PROTOTYPE'S.** `keyCornerLog2Hz` solves for the corner whose
	// BILINEAR stage turns the note through exactly π/N, so the notch is on the note to the engine's own rounding
	// (fasttan, a float G): the response read off the engine's coefficients at the note is far below the −40 dB the
	// plan asks for, and −80 dB is asserted as the tight bound.
	//
	// THE TOLERANCE THE ANALOG FORM WOULD HAVE NEEDED is computed below, not assumed: Center = f/tan(π/(2N)) is exact
	// at N = 2 (tan(π/4) = 1, so the prewarp's atan(tan(·)) is the identity) and drifts with N, because the prewarp
	// bends ω = tan(πf/sr) differently at the note and at the N-dependent corner.
	for (const int n : { 2, 6, 12 }) {
		for (const float note : { 48.0f, 72.0f }) {
			CAPTURE(n);
			CAPTURE(note);
			ph::Settings s = plainPhaser(n); // feedback 0, Mix 0.5, the LFO and follower off
			s.keyTrack = true;
			auto dev = deviceWith(s);
			dev->setNoteContext(noteOf(note, true));
			oneTick(*dev);
			REQUIRE(dev->stagesForTest() == n);
			const double hz = trackedHz(note);
			for (int ch = 0; ch < 2; ++ch) {
				const double depth = notchDepthDb(*dev, n, hz, ch);
				CAPTURE(depth);
				CHECK(depth < -40.0);
				CHECK(depth < -80.0);
			}
			// THE ANALOG PROTOTYPE, restated: its corner, prewarped as the engine prewarps one.
			const double gAnalog = std::tan(kPi * (hz / std::tan(kPi / (2.0 * n))) / kRate);
			const double analogDb = dB(std::abs(0.5 * (1.0 + cascadeOf(gAnalog, n, hz))));
			INFO("N " << n << " note " << note << ": the analog prototype's notch at the note is " << analogDb
			           << " dB; the engine's " << notchDepthDb(*dev, n, hz) << " dB");
			if (n == 12 && note == 72.0f) {
				// WHY THE EXACT FORM: here the analog one misses the plan's own −40 dB.
				CHECK(analogDb > -40.0);
			}
		}
	}
	SUBCASE("and the running engine nulls a sine at the note") {
		ph::Settings s = plainPhaser(6);
		s.keyTrack = true;
		auto dev = deviceWith(s);
		dev->setNoteContext(noteOf(72.0f, true));
		const double rms = measuredRmsRatio(*dev, trackedHz(72.0));
		CAPTURE(dB(rms));
		CHECK(dB(rms) < -40.0);
	}
}

TEST_CASE("key track holds the last note after release, and the knob rules before any note") {
	for (const ph::Mode mode : { ph::Mode::Flanger, ph::Mode::Phaser }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = mode == ph::Mode::Flanger ? plainFlanger() : plainPhaser(6);
		s.keyTrack = true;
		auto dev = deviceWith(s);
		// THE CENTRE, restated per mode: the flanger's read delay in samples, the phaser's corner in Hz.
		const auto centre = [&dev, mode]() {
			return mode == ph::Mode::Flanger ? dev->delayTargetSamplesForTest(0) : dev->cornerHzForTest(0);
		};
		const double knob = mode == ph::Mode::Flanger ? knobDelaySamples() : knobCornerHz();
		const auto ofNote = [mode](double hz) {
			return mode == ph::Mode::Flanger ? kRate / hz
			                                 : kRate / kPi * std::atan(std::tan(kPi * hz / kRate) / std::tan(kPi / 12.0));
		};

		// BEFORE ANY NOTE: the knob.
		oneTick(*dev);
		CHECK_FALSE(dev->heardNoteForTest());
		CHECK(dev->noteHzForTest() == 0.0);
		CHECK(std::fabs(centre() - knob) < 1.0e-9 * knob);
		// A RELEASED NOTE IS NOT A NOTE: the gate never rose.
		dev->setNoteContext(noteOf(72.0f, false));
		runStereo(*dev, 4800);
		CHECK_FALSE(dev->heardNoteForTest());
		CHECK(std::fabs(centre() - knob) < 1.0e-9 * knob);

		// A HELD NOTE — and the centre GLIDES there, it does not step: one tick of a 5 ms one-pole IN LOG2
		// (`kKeySmoothSeconds` — a note's own glide, so a trill lands; a knob keeps 20 ms). RESTATED: the knob's log2,
		// moved by 1 − e^(−16/(0.005·48000)) of the way to the note's, then out of log2.
		dev->setNoteContext(noteOf(60.0f, true));
		oneTick(*dev);
		const double target = ofNote(trackedHz(60.0));
		const double first = centre();
		CHECK(std::fabs(first - knob) > 1.0e-6 * knob);
		CHECK(std::fabs(first - target) > 1.0e-6 * target);
		CHECK((first - knob) * (target - first) > 0.0); // strictly between
		const auto stepOf = [&](double seconds) {
			const double a = 1.0 - std::exp(-16.0 / (seconds * kRate));
			const double from = std::log2(knob);
			return std::exp2(from + (std::log2(target) - from) * a);
		};
		const double expected = stepOf(0.005);
		CAPTURE(first);
		CAPTURE(expected);
		CHECK(std::fabs(first - expected) < 1.0e-9 * expected);
		// NOT THE KNOBS' 20 ms (the value it had before the review): a quarter of the way as far.
		CHECK(std::fabs(first - stepOf(0.02)) > 1.0e-3 * expected);
		runStereo(*dev, 24000); // 0.5 s, 25 time constants: landed
		CHECK(std::fabs(centre() - target) < 1.0e-6 * target);
		const double held = centre();

		// RELEASED, with the host handing over a DIFFERENT pitch: the centre holds the last held note, bit for bit.
		dev->setNoteContext(noteOf(79.0f, false));
		runStereo(*dev, 24000);
		CHECK(dev->heardNoteForTest());
		CHECK(centre() == held);
		CHECK(std::fabs(dev->noteHzForTest() - trackedHz(60.0)) < 1.0e-9 * trackedHz(60.0));

		// BYPASSED AND BACK: still the last note — the enable road is not a reset.
		ph::Settings off = s;
		off.enabled = false;
		dev->setTarget(off);
		runStereo(*dev, 512);
		dev->setTarget(s);
		oneTick(*dev);
		CHECK(std::fabs(centre() - target) < 1.0e-6 * target);

		// RESET FORGETS IT: as if just switched on, so the knob again until a note.
		dev->reset();
		oneTick(*dev);
		CHECK_FALSE(dev->heardNoteForTest());
		CHECK(std::fabs(centre() - knob) < 1.0e-9 * knob);
	}
}

TEST_CASE("key track off: the note is ignored") {
	for (const ph::Mode mode : { ph::Mode::Flanger, ph::Mode::Phaser }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = mode == ph::Mode::Flanger ? plainFlanger() : plainPhaser(6);
		s.keyTrack = false;
		const auto run = [&s](bool keyTrack, Captured& got) {
			ph::Settings t = s;
			t.keyTrack = keyTrack;
			auto dev = deviceWith(t);
			dev->setReadingsSink({ &got, &Captured::publish });
			dev->setNoteContext(noteOf(84.0f, true));
			runStereo(*dev, 512);
			return dev;
		};
		Captured got;
		const auto dev = run(false, got);
		CHECK(dev->noteHzForTest() == 0.0);
		REQUIRE(got.values.size() == ph::kReadingCount);
		CHECK(got.values[static_cast<size_t>(ph::Reading::NoteHz)] == 0.0f);
		if (mode == ph::Mode::Flanger) {
			CHECK(std::fabs(dev->delayTargetSamplesForTest(0) - knobDelaySamples()) < 1.0e-9);
		}
		else {
			CHECK(std::fabs(dev->cornerHzForTest(0) - knobCornerHz()) < 1.0e-9);
		}
		// NOT VACUOUS: the same note with Key Track on moves the centre, and reports the frequency.
		Captured gotOn;
		const auto on = run(true, gotOn);
		CHECK(gotOn.values[static_cast<size_t>(ph::Reading::NoteHz)] == static_cast<float>(on->noteHzForTest()));
		CHECK(std::fabs(on->noteHzForTest() - trackedHz(84.0)) < 1.0e-9 * trackedHz(84.0));
		if (mode == ph::Mode::Flanger) {
			CHECK(std::fabs(on->delayTargetSamplesForTest(0) - dev->delayTargetSamplesForTest(0)) > 1.0);
		}
		else {
			CHECK(std::fabs(on->cornerHzForTest(0) - dev->cornerHzForTest(0)) > 1.0);
		}
	}
}

TEST_CASE("key track: an absurd pitch gives a finite NoteHz and an in-range centre") {
	// THE PITCH IS CLAMPED TO [−60, 200] semitones in `keyHzOf`, so 1e4 is 200: 440·2^(131/12), restated here, and
	// finite — without the clamp it is 440·2^(9931/12), an infinity. And the low end, −1e4, is −60: 440·2^(−129/12),
	// never 0. The knob's offset applies after, and the corner and delay clamps take it from there.
	CHECK(std::fabs(ph::keyHzOf(1.0e4) - 440.0 * std::pow(2.0, 131.0 / 12.0)) < 1.0e-6);
	CHECK(std::fabs(ph::keyHzOf(-1.0e4) - 440.0 * std::pow(2.0, -129.0 / 12.0)) < 1.0e-12);
	for (const ph::Mode mode : { ph::Mode::Flanger, ph::Mode::Phaser }) {
		for (const float pitch : { 1.0e4f, -1.0e4f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(pitch);
			ph::Settings s = mode == ph::Mode::Flanger ? plainFlanger() : plainPhaser(6);
			s.keyTrack = true;
			// AND THE KNOB'S OFFSET PUSHING THE SAME WAY, at its end: still finite and in range.
			s.centerLog2Hz = static_cast<float>(pitch > 0.0f ? ph::kCenterLog2Max : ph::kCenterLog2Min);
			s.delayLog2s = static_cast<float>(pitch > 0.0f ? ph::kDelayLog2Min : ph::kDelayLog2Max);
			auto dev = deviceWith(s);
			Captured got;
			dev->setReadingsSink({ &got, &Captured::publish });
			dev->setNoteContext(noteOf(pitch, true));
			runStereo(*dev, 512);
			const double hz = dev->noteHzForTest();
			CHECK(std::isfinite(hz));
			CHECK(hz > 0.0);
			REQUIRE(got.values.size() == ph::kReadingCount);
			CHECK(std::isfinite(got.values[static_cast<size_t>(ph::Reading::NoteHz)]));
			for (int ch = 0; ch < 2; ++ch) {
				if (mode == ph::Mode::Flanger) {
					const double d = dev->delayTargetSamplesForTest(ch);
					CHECK(d >= static_cast<double>(ph::kMinDelaySamples));
					CHECK(d <= ph::kMaxDelayMs * 0.001 * kRate);
					CHECK(std::isfinite(static_cast<double>(dev->delaySamplesForTest(ch))));
				}
				else {
					const double fc = dev->cornerHzForTest(ch);
					CHECK(fc >= ph::kCornerMinHz);
					CHECK(fc <= ph::kCornerMaxFraction * kRate);
					const float g = dev->stageGForTest(ch, 0);
					CHECK(std::isfinite(g));
					CHECK(g > 0.0f);
				}
			}
		}
	}
}

TEST_CASE("key track: Center and Delay are the offset from the tracked note, +0 at their defaults") {
	// ONE KNOB (a Center and a separate key offset were confusing): while a note is tracked,
	// Center is 12·log2(Center/800 Hz) semitones from it and Delay −12·log2(Delay/2.5 ms) — restated here through the
	// hertz and the seconds (`storedCenterOffset`, `storedDelayOffset`), not the header's helper.
	SUBCASE("the references ARE the defaults, so Key on at the default lands on the note") {
		CHECK(ph::kCenterKeyRefLog2 == ph::kParamDescs[static_cast<size_t>(ph::Param::CenterHz)].def);
		CHECK(ph::kDelayKeyRefLog2 == ph::kParamDescs[static_cast<size_t>(ph::Param::DelayMs)].def);
		CHECK(std::fabs(std::exp2(ph::kCenterKeyRefLog2) - 800.0) < 1.0e-9);
		CHECK(std::fabs(std::exp2(ph::kDelayKeyRefLog2) - 0.0025) < 1.0e-15);
		// AT THE DEFAULTS THE OFFSET IS EXACTLY ZERO, as the row stores them (floats).
		CHECK(storedCenterOffset(plainPhaser(6)) == 0.0);
		CHECK(storedDelayOffset(plainFlanger()) == 0.0);
		CHECK(ph::keyOffsetSemitonesOf(ph::Mode::Phaser, static_cast<double>(plainPhaser(6).centerLog2Hz)) == 0.0);
		CHECK(ph::keyOffsetSemitonesOf(ph::Mode::Flanger, static_cast<double>(plainFlanger().delayLog2s)) == 0.0);
	}
	SUBCASE("Center at 1600 Hz (+12 st): the first notch is an octave above the note, and not on it") {
		for (const int n : { 2, 6, 12 }) {
			CAPTURE(n);
			ph::Settings s = plainPhaser(n);
			s.keyTrack = true;
			s.centerLog2Hz = static_cast<float>(std::log2(1600.0));
			REQUIRE(std::fabs(storedCenterOffset(s) - 12.0) < 1.0e-5);
			auto dev = deviceWith(s);
			dev->setNoteContext(noteOf(60.0f, true));
			oneTick(*dev);
			// NoteHz is the NOTE; the centre is the note moved by the knob.
			CHECK(std::fabs(dev->noteHzForTest() - trackedHz(60.0)) < 1.0e-9 * trackedHz(60.0));
			CHECK(notchDepthDb(*dev, n, trackedHz(60.0, storedCenterOffset(s))) < -80.0);
			CHECK(notchDepthDb(*dev, n, trackedHz(60.0)) > -40.0);
		}
	}
	SUBCASE("Flanger Delay 1.25 ms (+12 st): the comb's period is an octave above the note") {
		ph::Settings s = plainFlanger();
		s.keyTrack = true;
		s.delayLog2s = static_cast<float>(std::log2(0.00125));
		REQUIRE(std::fabs(storedDelayOffset(s) - 12.0) < 1.0e-5);
		auto dev = deviceWith(s);
		dev->setNoteContext(noteOf(60.0f, true));
		oneTick(*dev);
		const double expected = kRate / trackedHz(60.0, storedDelayOffset(s));
		CHECK(std::fabs(dev->delayTargetSamplesForTest(0) - expected) < 1.0e-9 * expected);
		// HALF THE NOTE'S OWN PERIOD, to the float the knob is stored as.
		CHECK(std::fabs(dev->delayTargetSamplesForTest(0) - 0.5 * kRate / trackedHz(60.0)) < 1.0e-5 * expected);
	}
	SUBCASE("any offset, either mode: the exact ratio 2^(st/12)") {
		const auto deviceAt = [](ph::Mode mode, double st) {
			ph::Settings s = mode == ph::Mode::Flanger ? plainFlanger() : plainPhaser(6);
			s.keyTrack = true;
			s.centerLog2Hz = static_cast<float>(ph::kCenterKeyRefLog2 + st / 12.0);
			s.delayLog2s = static_cast<float>(ph::kDelayKeyRefLog2 - st / 12.0);
			auto dev = deviceWith(s);
			dev->setNoteContext(noteOf(60.0f, true));
			oneTick(*dev);
			return std::pair { std::move(dev), mode == ph::Mode::Flanger ? storedDelayOffset(s) : storedCenterOffset(s) };
		};
		const auto [flatF, zeroF] = deviceAt(ph::Mode::Flanger, 0.0);
		REQUIRE(zeroF == 0.0);
		for (const double st : { -24.0, -12.0, -5.0, 7.0, 19.3, 24.0 }) {
			CAPTURE(st);
			const auto [f, offF] = deviceAt(ph::Mode::Flanger, st);
			const double ratio = std::pow(2.0, offF / 12.0);
			CHECK(std::fabs(flatF->delayTargetSamplesForTest(0) / f->delayTargetSamplesForTest(0) - ratio) < 1.0e-9 * ratio);
			const auto [p, offP] = deviceAt(ph::Mode::Phaser, st);
			CHECK(notchDepthDb(*p, 6, trackedHz(60.0, offP)) < -80.0);
			CHECK(notchDepthDb(*p, 6, trackedHz(60.0)) > -40.0);
		}
	}
	SUBCASE("not tracked, the knob is its own Hz or ms, whatever it is set to") {
		ph::Settings s = plainPhaser(6);
		s.keyTrack = true; // on, and no note yet
		s.centerLog2Hz = static_cast<float>(std::log2(1600.0));
		auto dev = deviceWith(s);
		oneTick(*dev);
		CHECK(std::fabs(dev->cornerHzForTest(0) - std::exp2(static_cast<double>(s.centerLog2Hz))) < 1.0e-9 * 1600.0);
	}
}

//==================================================================================================
// THE EXPLORE LAYER (Task 7) — `phaser_phase_space.h`.
//==================================================================================================

namespace {

namespace ps = ph::phase_space;

/// THE INDEPENDENT RESTATEMENTS: the homes from their definition (φ^((i+1)/4) + frac(i/φ), through `pow` and
/// `fmod` rather than the header's constexpr ladder), the unipolar triangle written out, and the offset.
double homeOf(size_t i) {
	const double phi = std::numbers::phi;
	return std::pow(phi, static_cast<double>(i + 1) / 4.0) + std::fmod(static_cast<double>(i) * (phi - 1.0), 1.0);
}
double triOf(double p) {
	const double x = p - std::floor(p);
	return x < 0.5 ? 2.0 * x : 2.0 - 2.0 * x;
}
double offsetOf(size_t i, float travel, double scale) {
	return scale * (triOf(homeOf(i) + static_cast<double>(travel)) - triOf(homeOf(i)));
}

/// THE PAD'S ROAD, restated: the plane at `rotate` anchored at the homes' mean, each phase `phaseFor(i, home, tx, ty)`
/// through `SliceBasis`, its travel term folded modulo one for storage (the shaper's `wrapForStorage`).
ps::Travel padTravel(double rotate, double tx, double ty) {
	std::array<double, ps::kPhaseCount> home {};
	double mean = 0.0;
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		home[i] = homeOf(i);
		mean += home[i];
	}
	mean /= static_cast<double>(ps::kPhaseCount);
	const auto basis = feathers::sliceBasisFor<ps::kPhaseCount>(rotate, mean);
	ps::Travel out {};
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		const double v = basis.phaseFor(i, home[i], tx, ty) - home[i];
		out[i] = static_cast<float>(v - std::floor(v));
	}
	return out;
}

/// ROUND 2'S FOUR FIELDS (indices 30..33) BACK AT HOME, for a Task 7 case whose restatement is of the first thirty's
/// roads alone (the four have their own cases, "explore round 2: …").
ps::Travel round2AtHome(ps::Travel travel) {
	for (size_t i = ps::kRateSkewPhase; i < ps::kPhaseCount; ++i) {
		travel[i] = 0.0f;
	}
	return travel;
}

/// THE EXPLORE VALUES, flattened in INDEX order — the test's own reading of the table in the header.
std::array<double, ps::kPhaseCount> flatten(const ps::Explore& e) {
	std::array<double, ps::kPhaseCount> out {};
	for (size_t i = 0; i < ps::kSpreadCount; ++i) {
		out[i] = e.spreadOctaves[i];
		out[12 + i] = e.lfoPhase[i];
	}
	out[24] = e.bend.skew;
	out[25] = e.bend.square;
	out[26] = e.bend.triangle;
	out[27] = e.dampingOctaves;
	out[28] = e.driveBias;
	out[29] = e.stereoOctaves;
	out[30] = e.rateSkewOctaves;
	out[31] = e.feedbackTilt;
	out[32] = e.pairingOctaves;
	out[33] = e.sweepCurve;
	return out;
}

/// Run two devices over the same noise, stereo, in 512-frame blocks, and return whether every sample of both
/// channels is bit-identical.
bool renderIdentical(ph::Phaser& a, ph::Phaser& b, int frames, uint32_t seed) {
	const std::vector<float> l0 = noise(frames, seed, 0.5f);
	const std::vector<float> r0 = noise(frames, seed ^ 0x5bd1e995u, 0.5f);
	std::vector<float> la = l0, ra = r0, lb = l0, rb = r0;
	for (int n0 = 0; n0 < frames; n0 += 512) {
		const int run = std::min(512, frames - n0);
		a.processStereo(la.data() + n0, ra.data() + n0, run);
		b.processStereo(lb.data() + n0, rb.data() + n0, run);
	}
	bool same = true;
	for (size_t j = 0; j < la.size(); ++j) {
		same = same && std::bit_cast<uint32_t>(la[j]) == std::bit_cast<uint32_t>(lb[j])
		       && std::bit_cast<uint32_t>(ra[j]) == std::bit_cast<uint32_t>(rb[j]);
	}
	return same;
}

/// A hot arrangement for the identity cases: the LFO running, the loop fed back, driven and damped, stereo spread.
ph::Settings hotOf(ph::Mode mode, ph::SweepShape sweep, bool tz) {
	ph::Settings s = barberpole(mode, sweep, 0.6f, 1.0f);
	s.stages = 12;
	s.feedback = 0.7f;
	s.drive = 0.6f;
	s.dampingLog2Hz = static_cast<float>(std::log2(3000.0));
	s.stereoDegrees = 90.0f;
	s.position = 0.1f;
	s.throughZero = tz;
	return s;
}

struct Arrangement {
	const char* what;
	ph::Mode mode;
	ph::SweepShape sweep;
	bool tz;
};
constexpr Arrangement kArrangements[] = {
	{ "phaser, Normal", ph::Mode::Phaser, ph::SweepShape::Normal, false },
	{ "phaser, Up", ph::Mode::Phaser, ph::SweepShape::Up, false },
	{ "flanger, Normal", ph::Mode::Flanger, ph::SweepShape::Normal, false },
	{ "flanger, Down, through-zero", ph::Mode::Flanger, ph::SweepShape::Down, true },
};

} // namespace

TEST_CASE("explore: zero travel gives the plain parameter set bit for bit under any Rotate") {
	// THE INVARIANT (design doc): zero travel ⇒ the derived set IS the plain set, bit for bit, whatever the plane's
	// turn. Two halves. (1) The VALUES: every derived offset is exactly 0.0, and every engine quantity the layer can
	// touch — the per-stage sweeps and corners, the damping corner, the saturator's bias, the right channel's centre,
	// the flanger's tap offsets and delays — equals its PLAIN restatement through the pure functions the engine had
	// before the layer existed. (2) THE OUTPUT: bit-identical to a device that never had travel, across the three
	// turns (an identical-path comparison is the claim here: identity).
	for (const Arrangement& a : kArrangements) {
		for (const double rotate : { 0.0, 1.0e3, -7.0e5 }) {
			CAPTURE(a.what);
			CAPTURE(rotate);
			const ph::Settings plain = hotOf(a.mode, a.sweep, a.tz);
			// THE PAD'S ROAD AT ZERO GESTURE under this turn: every stored float exactly 0.
			const ps::Travel travel = padTravel(rotate, 0.0, 0.0);
			for (size_t i = 0; i < ps::kPhaseCount; ++i) {
				CHECK(std::bit_cast<uint32_t>(travel[i]) == 0u);
			}
			ph::Settings at = plain;
			at.rotate = static_cast<float>(rotate);
			at.travel = travel;
			auto ref = deviceWith(plain);
			auto dev = deviceWith(at);
			CHECK(renderIdentical(*ref, *dev, 12000 + 37, 0xc0ffeeu));

			CHECK(dev->exploreForTest() == ps::Explore {});
			for (const double v : flatten(dev->exploreForTest())) {
				CHECK(std::bit_cast<uint64_t>(v) == 0u); // +0.0 exactly, not merely zero
			}
			CHECK(dev->driveBiasForTest() == 0.0f);
			CHECK(dev->dampingHzForTest() == ph::dampingCornerHz(dev->dampingLog2ForTest(), kRate));
			for (int ch = 0; ch < 2; ++ch) {
				CAPTURE(ch);
				if (a.sweep == ph::SweepShape::Normal) {
					// THE CHANNEL'S CORNER, the right's included (no skew): the plain `phaserCornerHz`.
					CHECK(dev->cornerHzForTest(ch)
					      == ph::phaserCornerHz(dev->centerLog2ForTest(), dev->sweepForTest(ch), kRate));
					if (a.mode == ph::Mode::Flanger) {
						const double d = a.tz ? ph::tzWetSamples(dev->delayLog2ForTest(), dev->sweepForTest(ch), kRate)
						                      : ph::flangerDelaySamples(dev->delayLog2ForTest(), dev->sweepForTest(ch), kRate);
						CHECK(dev->delayTargetSamplesForTest(ch) == d);
					}
				}
				const int voices = a.sweep == ph::SweepShape::Normal ? 1 : ph::kBarberVoices;
				for (int v = 0; v < voices; ++v) {
					for (int i = 0; i < ph::kMaxStages; ++i) {
						// EVERY STAGE ON ITS VOICE'S SWEEP, and every stage's coefficient its voice's first stage's.
						CHECK(dev->voiceStageSweepForTest(v, ch, i) == dev->voiceSweepForTest(v, ch));
						CHECK(dev->voiceStageCoefficientForTest(v, ch, i) == dev->voiceStageCoefficientForTest(v, ch, 0));
					}
					CHECK(dev->tapLog2OffsetForTest(v, ch) == 0.0f);
					CHECK(dev->tapPhaseOffsetForTest(v, ch) == 0.0f);
				}
			}
		}
	}
}

TEST_CASE("explore: Rotate alone leaves the sound standing, with travel too") {
	// THE SHAPER'S FACT (`Shaper :: Shaper Rotate` on test_fx_devices' dead list): Rotate turns the plane the NEXT
	// gesture moves in and never reaches the derive. So the same stored travel under two turns is the same sound, to
	// the bit — and that is why `Phaser Rotate` stays on the dead list after this task.
	const ps::Travel travel = padTravel(1.0e3, 0.41, -0.27);
	for (const Arrangement& a : kArrangements) {
		CAPTURE(a.what);
		ph::Settings s = hotOf(a.mode, a.sweep, a.tz);
		s.travel = travel;
		s.rotate = 0.0f;
		auto one = deviceWith(s);
		s.rotate = -7.0e5f;
		auto two = deviceWith(s);
		CHECK(renderIdentical(*one, *two, 9000, 0xabcdefu));
		CHECK(one->exploreForTest() == two->exploreForTest());
		CHECK_FALSE(one->exploreForTest() == ps::Explore {}); // not vacuous: the travel is live
	}
}

TEST_CASE("explore: travel moves the per-stage corners") {
	// THE VALUE of each stage's prewarped corner g = tan(π·fc_i/sr), against an independent restatement: the travel
	// built through `SliceBasis` by this test, each spread `1.5·(tri(phase_i) − tri(home_i))`, the corner
	// `Center·2^(3s + spread_i ± p)` (and the right's centre moved by the stereo skew, index 29), clamped — round 2's notch
	// pairing p (index 32: + on even stages, − on odd) and sweep curve (index 33: s = 0.2^(2^c)) restated with it.
	constexpr double kRotate = 1234.5;
	ps::Travel travel = padTravel(kRotate, 0.37, -0.61);
	// ROUND 2'S CURVE AND PAIRING SET WHERE THEY ARE LOUD (the plane happens to leave the curve near home here).
	travel[ps::kCurvePhase] = 0.3f;
	travel[ps::kPairingPhase] = 0.6f;
	ph::Settings s = plainPhaser(12);
	s.position = 0.2f;
	s.rotate = static_cast<float>(kRotate);
	s.travel = travel;
	auto dev = deviceWith(s);
	runStereo(*dev, 24000); // 0.5 s: the 20 ms glides have landed
	const double centre = static_cast<double>(static_cast<float>(std::log2(800.0)));
	const double c = offsetOf(ps::kCurvePhase, travel[ps::kCurvePhase], 1.0);
	const double pair = offsetOf(ps::kPairingPhase, travel[ps::kPairingPhase], 1.0);
	REQUIRE(std::fabs(c) > 0.05); // not vacuous: the curve and the pairing both travel here
	REQUIRE(std::fabs(pair) > 0.05);
	const double sweep = std::pow(static_cast<double>(0.2f), std::exp2(c));
	double widest = 0.0;
	for (int ch = 0; ch < 2; ++ch) {
		const double skew = ch == 1 ? offsetOf(29, travel[29], 0.5) : 0.0;
		CHECK(std::fabs(dev->exploreForTest().stereoOctaves - offsetOf(29, travel[29], 0.5)) < 1.0e-6);
		for (size_t i = 0; i < 12; ++i) {
			CAPTURE(ch);
			CAPTURE(i);
			const double spread = offsetOf(i, travel[i], 1.5);
			CHECK(std::fabs(dev->exploreForTest().spreadOctaves[i] - spread) < 1.0e-6);
			CHECK(std::fabs(spread) <= 1.5);
			widest = std::max(widest, std::fabs(spread));
			const double fc = std::clamp(std::exp2(centre + skew + 3.0 * sweep + spread + (i % 2 == 0 ? pair : -pair)), 20.0, 0.45 * kRate);
			const double g = std::tan(kPi * fc / kRate);
			const double got = static_cast<double>(dev->stageGForTest(ch, static_cast<int>(i)));
			CAPTURE(g);
			CAPTURE(got);
			CHECK(std::fabs(got - g) < 2.0e-5 * g);
		}
	}
	// NOT VACUOUS: the stages really are spread, by a good fraction of the reach.
	CAPTURE(widest);
	CHECK(widest > 0.3);
	CHECK(std::fabs(dev->stageGForTest(0, 0) - dev->stageGForTest(0, 5)) > 1.0e-3f * dev->stageGForTest(0, 0));
}

TEST_CASE("explore: Normal-mode per-stage LFO offsets act on φ; barberpole ones act on the sweep value and never on ψ") {
	// THE DECISION (`stageSweepsOf`). Only the LFO-phase group travels here (indices 12..23), so nothing else moves.
	ps::Travel travel {};
	for (size_t i = 0; i < 12; ++i) {
		travel[12 + i] = 0.05f + 0.06f * static_cast<float>(i);
	}
	const auto offOf = [&travel](size_t i) { return offsetOf(12 + i, travel[12 + i], 0.5); };
	constexpr double kDepth = static_cast<double>(0.6f);
	const double position = static_cast<double>(0.1f);

	SUBCASE("Normal: stage i sweeps Position + Depth·sin(2π(φ + off_i))") {
		ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Normal, 0.6f, 0.0f);
		s.stages = 12;
		s.position = 0.1f;
		s.travel = travel;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000 + 37 * 16);
		double moved = 0.0;
		for (int ch = 0; ch < 2; ++ch) {
			const double phi = dev->lfoPhaseForTest(ch);
			for (size_t i = 0; i < 12; ++i) {
				CAPTURE(ch);
				CAPTURE(i);
				CHECK(std::fabs(dev->exploreForTest().lfoPhase[i] - offOf(i)) < 1.0e-6);
				const double expected = std::clamp(position + kDepth * std::sin(2.0 * kPi * (phi + offOf(i))), -1.0, 1.0);
				CHECK(std::fabs(dev->stageSweepForTest(ch, static_cast<int>(i)) - expected) < 1.0e-6);
				moved = std::max(moved, std::fabs(dev->stageSweepForTest(ch, static_cast<int>(i)) - dev->sweepForTest(ch)));
			}
		}
		CAPTURE(moved);
		CHECK(moved > 0.05);
	}
	SUBCASE("barberpole: ψ_k unchanged; stage i sweeps s_k + Depth·(sin(2π(φ + off_i)) − sin(2πφ))") {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(sweep));
			ph::Settings s = barberpole(ph::Mode::Phaser, sweep, 0.6f, 0.0f);
			s.stages = 12;
			s.position = 0.1f;
			auto plain = deviceWith(s);
			s.travel = travel;
			auto dev = deviceWith(s);
			runStereo(*plain, 24000 + 53 * 16);
			runStereo(*dev, 24000 + 53 * 16);
			double moved = 0.0;
			for (int ch = 0; ch < 2; ++ch) {
				const double phi = dev->lfoPhaseForTest(ch);
				CHECK(phi == plain->lfoPhaseForTest(ch));
				for (int k = 0; k < ph::kBarberVoices; ++k) {
					CAPTURE(ch);
					CAPTURE(k);
					// ψ IS THE TRAVEL-FREE ψ, TO THE BIT, and so is the voice's sweep.
					CHECK(dev->voicePhaseForTest(k, ch) == plain->voicePhaseForTest(k, ch));
					CHECK(dev->voiceSweepForTest(k, ch) == plain->voiceSweepForTest(k, ch));
					if (ch == 0) {
						// ψ'S OWN PHASE, restated (Depth 0.6 at 1 Hz; 24000 + 53·16 samples is 1553 ticks).
						CHECK(std::fabs(dev->voicePhaseForTest(k, 0)
						                - voicePhaseOf(rampPhaseOf(rhoAfter(1552, 1.0, 0.6f), sweep), k))
						      < 1.0e-10);
					}
					const double sk = dev->voiceSweepForTest(k, ch);
					for (size_t i = 0; i < 12; ++i) {
						CAPTURE(i);
						const double expected = std::clamp(
						    sk + kDepth * (std::sin(2.0 * kPi * (phi + offOf(i))) - std::sin(2.0 * kPi * phi)), -1.0, 1.0);
						const double got = dev->voiceStageSweepForTest(k, ch, static_cast<int>(i));
						CHECK(std::fabs(got - expected) < 1.0e-6);
						moved = std::max(moved, std::fabs(got - sk));
					}
				}
			}
			CAPTURE(moved);
			CHECK(moved > 0.05);
		}
	}
}

TEST_CASE("explore: the flanger reads spread 0/1 and LFO offsets 12/13 per channel in Normal, and index k on tap k under the barberpole") {
	ps::Travel travel = round2AtHome(padTravel(-321.0, -0.53, 0.29)); // the first thirty's roads only
	// THE SHAPE GROUP AT HOME, so the restatement below can read the LFO as the sine (the bent shape has its own case).
	for (size_t i = ps::kLfoShapeFirst; i < ps::kLfoShapeFirst + ps::kLfoShapeCount; ++i) {
		travel[i] = 0.0f;
	}
	const double skew = offsetOf(29, travel[29], 0.5);
	SUBCASE("Normal") {
		ph::Settings s = barberpole(ph::Mode::Flanger, ph::SweepShape::Normal, 0.6f, 0.0f);
		s.position = 0.1f;
		s.travel = travel;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000 + 41 * 16);
		for (int ch = 0; ch < 2; ++ch) {
			CAPTURE(ch);
			const size_t idx = static_cast<size_t>(ch);
			const double delayOff = offsetOf(idx, travel[idx], 1.5) + (ch == 1 ? skew : 0.0);
			const double phaseOff = offsetOf(12 + idx, travel[12 + idx], 0.5);
			CHECK(std::fabs(static_cast<double>(dev->tapLog2OffsetForTest(0, ch)) - delayOff) < 1.0e-6);
			CHECK(std::fabs(static_cast<double>(dev->tapPhaseOffsetForTest(0, ch)) - phaseOff) < 1.0e-6);
			const double phi = dev->lfoPhaseForTest(ch);
			const double sw = std::clamp(static_cast<double>(0.1f) + 0.6 * std::sin(2.0 * kPi * (phi + phaseOff)), -1.0, 1.0);
			CHECK(std::fabs(dev->sweepForTest(ch) - sw) < 1.0e-6);
			const double d = std::clamp(0.0025 * std::exp2(delayOff + 2.0 * sw) * kRate, static_cast<double>(ph::kMinDelaySamples), 0.02 * kRate);
			CHECK(std::fabs(dev->delayTargetSamplesForTest(ch) - d) < 1.0e-5 * d);
			// AND THE PER-SAMPLE READ LANDS ON IT at the tick's last sample (the bent road, per sample).
			CHECK(std::fabs(static_cast<double>(dev->delaySamplesForTest(ch)) - d) < 1.0e-3 * d);
		}
		// INDICES 2 AND 14 ARE INERT IN NORMAL: taps 1 and 2 carry nothing.
		for (int ch = 0; ch < 2; ++ch) {
			for (int k = 1; k < ph::kBarberVoices; ++k) {
				CHECK(dev->tapLog2OffsetForTest(k, ch) == 0.0f);
				CHECK(dev->tapPhaseOffsetForTest(k, ch) == 0.0f);
			}
		}
	}
	SUBCASE("barberpole: tap k reads spread k and LFO offset 12 + k, on both channels") {
		ph::Settings s = barberpole(ph::Mode::Flanger, ph::SweepShape::Up, 0.6f, 0.0f);
		s.position = 0.1f;
		s.travel = travel;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000 + 41 * 16);
		for (int ch = 0; ch < 2; ++ch) {
			const double phi = dev->lfoPhaseForTest(ch);
			for (int k = 0; k < ph::kBarberVoices; ++k) {
				CAPTURE(ch);
				CAPTURE(k);
				const size_t idx = static_cast<size_t>(k);
				const double delayOff = offsetOf(idx, travel[idx], 1.5) + (ch == 1 ? skew : 0.0);
				const double phaseOff = offsetOf(12 + idx, travel[12 + idx], 0.5);
				CHECK(std::fabs(static_cast<double>(dev->tapLog2OffsetForTest(k, ch)) - delayOff) < 1.0e-6);
				CHECK(std::fabs(static_cast<double>(dev->tapPhaseOffsetForTest(k, ch)) - phaseOff) < 1.0e-6);
				// THE TAP'S SWEEP: its voice's, moved by its offset as a sweep VALUE; its delay by its spread.
				const double sk = dev->voiceSweepForTest(k, ch);
				const double st = std::clamp(sk + 0.6 * (std::sin(2.0 * kPi * (phi + phaseOff)) - std::sin(2.0 * kPi * phi)),
				                             -1.0, 1.0);
				const double d = std::clamp(0.0025 * std::exp2(delayOff + 2.0 * st) * kRate, static_cast<double>(ph::kMinDelaySamples), 0.02 * kRate);
				CHECK(std::fabs(dev->voiceDelayTargetSamplesForTest(k, ch) - d) < 1.0e-5 * d);
			}
		}
	}
}

TEST_CASE("explore: the LFO shape bends continuously, stays in [−1, 1], and is the sine exactly at home") {
	const ps::LfoBend home {};
	for (int n = -50; n < 150; ++n) {
		const double p = 0.0137 * n;
		CHECK(ph::lfoShape(p, home) == ph::lfoShape(p)); // the home road is the old expression
	}
	// THE THREE ENDS, restated: full skew puts the peak at 0.5·0.1 = 0.05 of the cycle; full triangle blend IS the
	// triangle; full squareness is tanh(6·sin)/tanh(6).
	CHECK(std::fabs(ph::lfoShape(0.05, { 1.0, 0.0, 0.0 }) - 1.0) < 1.0e-12);
	CHECK(std::fabs(ph::lfoShape(0.125, { 0.0, 0.0, 1.0 }) - 0.5) < 1.0e-12);
	CHECK(std::fabs(ph::lfoShape(0.6, { 0.0, 0.0, 1.0 }) - (2.0 - 4.0 * 0.6)) < 1.0e-12);
	CHECK(std::fabs(ph::lfoShape(0.1, { 0.0, 1.0, 0.0 }) - std::tanh(6.0 * std::sin(0.2 * kPi)) / std::tanh(6.0))
	      < 1.0e-12);
	// IN RANGE AND CONTINUOUS: no step bigger than the sine's own slope allows (2π·dp, with the skew's 1/(2·0.1) on
	// its steep side and the squareness's gain on top), anywhere, across the wrap included, for a grid of bends.
	for (const double a : { 0.0, 0.3, 1.0 }) {
		for (const double b : { 0.0, 0.5, 1.0 }) {
			for (const double c : { 0.0, 0.7, 1.0 }) {
				const ps::LfoBend bend { a, b, c };
				double last = ph::lfoShape(-0.0001, bend);
				double worst = 0.0;
				for (int n = 0; n <= 20000; ++n) {
					const double y = ph::lfoShape(n / 10000.0, bend);
					CHECK(std::isfinite(y));
					CHECK(std::fabs(y) <= 1.0 + 1.0e-12);
					worst = std::max(worst, std::fabs(y - last));
					last = y;
				}
				CAPTURE(a);
				CAPTURE(b);
				CAPTURE(c);
				CHECK(worst < 0.03);
			}
		}
	}
}

TEST_CASE("explore: the asymmetric saturator is zero at zero, non-expansive, bounded, and the plain one at bias 0") {
	// RESTATED as the DEFINITION, (tanh(v·k + b) − tanh(b))/k in double, k = 7·Drive, the bias full from Drive 0.1 up.
	for (const float drive : { 0.2f, 0.6f, 1.0f }) {
		for (const float bias : { -0.5f, -0.13f, 0.21f, 0.5f }) {
			CAPTURE(drive);
			CAPTURE(bias);
			const double k = 7.0 * static_cast<double>(drive);
			const double b = static_cast<double>(bias);
			CHECK(ph::saturate(0.0f, drive, bias) == 0.0f);
			CHECK(ph::saturate(-0.0f, drive, bias) == 0.0f);
			for (const float v : { -2.0f, -0.6f, -0.1f, -1.0e-3f, 1.0e-6f, 0.05f, 0.3f, 1.5f }) {
				CAPTURE(v);
				const double defined = (std::tanh(static_cast<double>(v) * k + b) - std::tanh(b)) / k;
				const double got = static_cast<double>(ph::saturate(v, drive, bias));
				// fasttanh is a Padé approximant: within 2e-3 of tanh's value over the clamp, relative to the scale 1/k.
				CHECK(std::fabs(got - defined) < 3.0e-3 / k + 1.0e-6 * std::fabs(defined));
				// NON-EXPANSIVE: never more than what went in (the loop is never hotter than the linear one).
				CHECK(std::fabs(got) <= std::fabs(static_cast<double>(v)) * (1.0 + 1.0e-3));
			}
			// THE SLOPE AT 0 IS sech²(b), under one.
			const double slope = (static_cast<double>(ph::saturate(1.0e-4f, drive, bias))
			                      - static_cast<double>(ph::saturate(-1.0e-4f, drive, bias))) / 2.0e-4;
			CHECK(std::fabs(slope - (1.0 - std::tanh(b) * std::tanh(b))) < 0.01);
			const double bound = (1.0 + std::tanh(0.5)) / k * 1.01;
			for (const float v : { -100.0f, -1.0f, 1.0f, 100.0f }) {
				CHECK(std::fabs(static_cast<double>(ph::saturate(v, drive, bias))) <= bound);
			}
			// ASYMMETRIC: a positive and a negative swing of one size come back different.
			CHECK(std::fabs(ph::saturate(0.8f, drive, bias) + ph::saturate(-0.8f, drive, bias)) > 1.0e-3f);
		}
		CHECK(ph::saturate(0.37f, drive, 0.0f) == ph::saturate(0.37f, drive));
	}
	// WITH NO SATURATOR (Drive 0) THERE IS NOTHING TO BIAS: the identity. And the bias RAMPS IN with Drive below 0.1,
	// so the curve is continuous into it: at Drive 0.01 the bias is a tenth of itself.
	CHECK(ph::saturate(0.37f, 0.0f, 0.4f) == 0.37f);
	const double k = 0.07;
	const double defined = (std::tanh(0.37 * k + 0.04) - std::tanh(0.04)) / k;
	CHECK(std::fabs(static_cast<double>(ph::saturate(0.37f, 0.01f, 0.4f)) - defined) < 1.0e-3);
}

TEST_CASE("explore: the damping colour moves a running lowpass, and an open loop stays open") {
	ps::Travel travel {};
	travel[ps::kDampingPhase] = 0.31f;
	const double colour = offsetOf(ps::kDampingPhase, travel[ps::kDampingPhase], 2.0);
	REQUIRE(std::fabs(colour) > 0.2);
	SUBCASE("below the top: the corner is the knob's moved by the colour") {
		ph::Settings s = plainPhaser(6, 0.5f);
		s.dampingLog2Hz = static_cast<float>(std::log2(2000.0));
		s.travel = travel;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000);
		CHECK_FALSE(dev->dampingOpenForTest());
		const double expected = std::min(std::max(std::exp2(static_cast<double>(s.dampingLog2Hz) + colour), 20.0), 0.45 * kRate);
		CHECK(std::fabs(dev->dampingHzForTest() - expected) < 1.0e-5 * expected);
	}
	SUBCASE("at the top: open, whatever the colour") {
		for (const float t : { 0.31f, 0.77f, 0.05f }) {
			ph::Settings s = plainPhaser(6, 0.5f);
			s.travel[ps::kDampingPhase] = t;
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			CHECK(dev->exploreForTest().dampingOctaves != 0.0);
			CHECK(dev->dampingOpenForTest());
		}
	}
}

TEST_CASE("explore: the damping colour fades out over the knob's top octave, so the lowpass leaves the loop seamlessly") {
	const double top = static_cast<double>(static_cast<float>(ph::kDampingLog2Max));
	SUBCASE("the pure function at colour −2 oct: the plain corner at the top, the full colour an octave below, no step") {
		CHECK(ph::colouredDampingCornerHz(top, -2.0, kRate) == ph::dampingCornerHz(top, kRate));
		const double low = top - 1.5;
		CHECK(std::fabs(ph::colouredDampingCornerHz(low, -2.0, kRate) - std::exp2(low - 2.0)) < 1.0e-9 * std::exp2(low));
		// IN THE FADE the coloured log2 is x + C·(top − x), slope 1 − C = 3 at C = −2; outside it, 1. So a knob step of
		// h octaves moves the corner by at most 3h octaves, and never jumps.
		constexpr double kStep = 1.0e-3;
		double last = std::log2(ph::colouredDampingCornerHz(top - 2.0, -2.0, kRate));
		for (double x = top - 2.0 + kStep; x <= top; x += kStep) {
			const double now = std::log2(ph::colouredDampingCornerHz(x, -2.0, kRate));
			CHECK(std::fabs(now - last) <= 3.0 * kStep * (1.0 + 1.0e-6));
			last = now;
		}
		// AT ZERO COLOUR, EXACTLY THE PLAIN CORNER, at every knob.
		for (double x = top - 3.0; x <= top; x += 0.0137) {
			CHECK(ph::colouredDampingCornerHz(x, 0.0, kRate) == ph::dampingCornerHz(x, kRate));
		}
	}
	SUBCASE("the engine: the knob glided to its top under the most negative colour the pad reaches") {
		float t = 0.0f;
		double colour = 0.0;
		for (int n = 0; n < 400; ++n) {
			const float c = 0.0025f * static_cast<float>(n);
			if (offsetOf(ps::kDampingPhase, c, 2.0) < colour) {
				colour = offsetOf(ps::kDampingPhase, c, 2.0);
				t = c;
			}
		}
		CAPTURE(colour);
		REQUIRE(colour < -1.0);
		ph::Settings s = plainPhaser(6, 0.5f);
		s.dampingLog2Hz = static_cast<float>(std::log2(10000.0));
		s.travel[ps::kDampingPhase] = t;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000); // settled: the colour landed, the knob at 10 kHz
		REQUIRE_FALSE(dev->dampingOpenForTest());
		const double gap = top - dev->dampingLog2ForTest();
		CAPTURE(gap);
		// THE BOUND, DERIVED: the knob's 20 ms glide moves it by at most α·gap a tick (α = 1 − e^(−16/960)), and the
		// coloured log2 moves by at most (1 + |C|) times the knob's (the fade's slope above), so a tick moves the corner
		// by at most (1 + |C|)·α·gap octaves.
		const double alpha = 1.0 - std::exp(-16.0 / (0.02 * kRate));
		const double bound = (1.0 + std::fabs(colour)) * alpha * gap * (1.0 + 1.0e-4);
		CAPTURE(bound);
		s.dampingLog2Hz = static_cast<float>(ph::kDampingLog2Max);
		dev->setTarget(s);
		double last = std::log2(dev->dampingHzForTest());
		double worst = 0.0;
		int ticks = 0;
		while (!dev->dampingOpenForTest() && ticks < 3000) {
			last = std::log2(dev->dampingHzForTest());
			oneTick(*dev);
			++ticks;
			if (!dev->dampingOpenForTest()) {
				worst = std::max(worst, std::fabs(std::log2(dev->dampingHzForTest()) - last));
			}
		}
		CAPTURE(worst);
		CAPTURE(ticks);
		REQUIRE(dev->dampingOpenForTest());
		CHECK(worst <= bound);
		// NO STEP AT THE EXIT: the last lowpass before the loop opened sat where the plain knob's would — at the top, to
		// within what the opening test leaves. `dampingOpen` compares IN FLOAT, so the loop opens once the glided knob
		// rounds to the stored top: within half a float ULP of it (4.8e-7 octave at 14.29). The tick before was at most
		// 1/(1 − α) of that away, and the coloured corner (1 + |C|) times further: ~1e-6 octave. Unfaded it would be
		// |C| = 1.11 octaves below — the step this fade removes.
		const float topF = static_cast<float>(ph::kDampingLog2Max);
		const double halfUlp = 0.5 * (static_cast<double>(std::nextafter(topF, 100.0f)) - static_cast<double>(topF));
		const double exitBound = (1.0 + std::fabs(colour)) * halfUlp / (1.0 - alpha) * 1.01;
		CAPTURE(exitBound);
		CHECK(std::fabs(last - top) < exitBound);
	}
}

TEST_CASE("explore: the loop DC blocker holds the output's mean at zero under loud asymmetric drive") {
	// AN ASYMMETRIC SATURATOR RECTIFIES, and the loop's DC gain (every allpass is +1 at DC) would recirculate what it
	// makes. THE BOUND, DERIVED: with the blocker the loop carries no DC in the steady state (a highpass is zero at DC),
	// the cascade passes the loop's DC at unit gain so the wet carries none, and the dry sine — 1 kHz over a window of
	// exactly one second — has a zero mean. The blocker's start-up transient is e^(−0.5 s / 15.9 ms) = e^(−31) by the
	// window's start. What remains is float rounding, ~1e-7 a sample, so the mean is held to 1e-5.
	float t = 0.0f;
	double bias = 0.0;
	for (int n = 0; n < 400; ++n) {
		const float c = 0.0025f * static_cast<float>(n);
		if (std::fabs(offsetOf(ps::kDrivePhase, c, 0.5)) > std::fabs(bias)) {
			bias = offsetOf(ps::kDrivePhase, c, 0.5);
			t = c;
		}
	}
	CAPTURE(bias);
	REQUIRE(std::fabs(bias) > 0.4);
	ph::Settings s = plainPhaser(6, 0.9f);
	s.drive = 1.0f;
	s.travel[ps::kDrivePhase] = t;
	auto dev = deviceWith(s);
	const int settle = static_cast<int>(0.5 * kRate);
	const int total = settle + static_cast<int>(kRate);
	const double w = 2.0 * kPi * 1000.0 / kRate;
	double sumL = 0.0;
	double sumR = 0.0;
	double peak = 0.0;
	std::vector<float> l(480);
	std::vector<float> r(480);
	for (int n0 = 0; n0 < total; n0 += 480) {
		for (int j = 0; j < 480; ++j) {
			l[static_cast<size_t>(j)] = r[static_cast<size_t>(j)] = static_cast<float>(std::sin(w * (n0 + j)));
		}
		dev->processStereo(l.data(), r.data(), 480);
		for (int j = 0; j < 480; ++j) {
			if (n0 + j >= settle) {
				sumL += l[static_cast<size_t>(j)];
				sumR += r[static_cast<size_t>(j)];
				peak = std::max(peak, static_cast<double>(std::fabs(l[static_cast<size_t>(j)])));
			}
		}
	}
	const double meanL = sumL / kRate;
	const double meanR = sumR / kRate;
	MESSAGE("bias " << bias << ": output mean L " << meanL << ", R " << meanR << ", peak " << peak);
	CHECK(std::fabs(static_cast<double>(dev->driveBiasForTest()) - bias) < 1.0e-6);
	CHECK(peak > 0.1); // the loop is loud, or the claim is vacuous
	CHECK(std::fabs(meanL) < 1.0e-5);
	CHECK(std::fabs(meanR) < 1.0e-5);
}

TEST_CASE("explore: travel survives a state round trip") {
	// THE DEVICE HALF: the wide row a patch stores (the controls, then the thirty-four folded floats) into a device through
	// the ops table's `setFields` — the travel bit for bit, and the layer it derives the restated one. THE DOCUMENT HALF
	// (the row to a saved rack and back) is `test_rack_state.cpp`'s "a device's row state past its parameters is held,
	// saved and restored — the row rule", which walks every device in `kDevices` that declares extra state; PHFL is
	// there (`device_registry.h`) with its thirty-four floats.
	const ps::Travel travel = padTravel(777.0, 1.7, -2.3);
	std::vector<float> wide(ph::kTravelFirst + ps::kPhaseCount, 0.0f);
	const ph::Row row = ph::rowOf(ph::State {});
	std::copy(row.begin(), row.end(), wide.begin());
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		wide[ph::kTravelFirst + i] = travel[i];
	}
	alignas(ph::Phaser) unsigned char storage[sizeof(ph::Phaser)];
	ph::kAudioOps.construct(storage);
	ph::kAudioOps.prepare(storage, kRate, 4096);
	ph::kAudioOps.setFields(storage, wide.data(), wide.size());
	const auto& dev = *reinterpret_cast<const ph::Phaser*>(storage);
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(dev.target().travel[i]) == std::bit_cast<uint32_t>(travel[i]));
	}
	// ONE BLOCK: the first tick PLACES every glide, so the layer is the derive of the stored travel, exactly.
	std::vector<float> l(512, 0.0f);
	std::vector<float> r(512, 0.0f);
	ph::kAudioOps.processStereo(storage, l.data(), r.data(), 512);
	const auto got = flatten(dev.exploreForTest());
	constexpr double kScales[] = { 1.5, 0.5, 1.0, 2.0, 0.5, 0.5, 0.25, 0.3, 1.0, 1.0}; // … colour, bias, skew, rate skew, tilt, pairing, curve
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		const double scale = i < 12 ? kScales[0] : i < 24 ? kScales[1] : i < 27 ? kScales[2] : kScales[3 + (i - 27)];
		const double off = offsetOf(i, travel[i], scale);
		const double expected = (i >= 24 && i < 27) ? std::fabs(off) : off;
		CHECK(std::fabs(got[i] - expected) < 1.0e-6);
	}
	CHECK_FALSE(dev.exploreForTest() == ps::Explore {});
	ph::kAudioOps.destruct(storage);
}

TEST_CASE("explore: every phase index is read by the derive exactly once") {
	// MEASURED, NOT ASSERTED (automod's first count missed a fifth of its space): the derive counts its own reads.
	std::array<int, ps::kPhaseCount> reads {};
	(void)ps::exploreOf(ps::Travel {}, &reads);
	int total = 0;
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(reads[i] == 1);
		total += reads[i];
	}
	CHECK(total == static_cast<int>(ps::kPhaseCount));
	CHECK(ph::kTraits.extraState == ps::kPhaseCount);
	// AND EACH INDEX MOVES THE ONE VALUE THE TABLE NAMES FOR IT, and no other: travel on index i alone moves entry i
	// of the flattened derive (the test's own reading of the table) and leaves every other exactly 0.
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		ps::Travel one {};
		one[i] = 0.3f;
		const auto flat = flatten(ps::exploreOf(one));
		for (size_t j = 0; j < ps::kPhaseCount; ++j) {
			CAPTURE(j);
			if (j == i) {
				CHECK(flat[j] != 0.0);
			}
			else {
				CHECK(std::bit_cast<uint64_t>(flat[j]) == 0u);
			}
		}
	}
}

TEST_CASE("explore: tails still reach exact zero with drive asymmetry and travel") {
	// THE DC THE BIAS MAKES IS REMOVED BY SUBTRACTION (sat_b(0) is exactly 0), so a silent loop is exactly silent and
	// the device can join the silence sweep with travel as without it. The bias is set where it is large.
	ps::Travel travel = padTravel(55.0, 0.83, 0.44);
	float best = 0.0f;
	for (int n = 0; n < 40; ++n) {
		const float t = 0.025f * static_cast<float>(n);
		if (std::fabs(offsetOf(ps::kDrivePhase, t, 0.5)) > std::fabs(offsetOf(ps::kDrivePhase, best, 0.5))) {
			best = t;
		}
	}
	travel[ps::kDrivePhase] = best;
	const double bias = offsetOf(ps::kDrivePhase, best, 0.5);
	CAPTURE(bias);
	REQUIRE(std::fabs(bias) > 0.25);
	const std::vector<float> burst = noise(static_cast<int>(kRate), 0x7a11u, 0.8f);
	for (const Arrangement& a : kArrangements) {
		CAPTURE(a.what);
		ph::Settings s = hotOf(a.mode, a.sweep, a.tz);
		s.feedback = 0.9f;
		s.travel = travel;
		auto dev = deviceWith(s);
		std::vector<float> l = burst;
		std::vector<float> r = burst;
		for (size_t n0 = 0; n0 < burst.size(); n0 += 512) {
			const int run = static_cast<int>(std::min<size_t>(512, burst.size() - n0));
			dev->processStereo(l.data() + n0, r.data() + n0, run);
		}
		CHECK(std::fabs(static_cast<double>(dev->driveBiasForTest()) - bias) < 1.0e-6);
		const int budget = a.mode == ph::Mode::Phaser ? kTailBudgetBlocks : kFlangerTailBudgetBlocks;
		int lastNonZero = -1;
		std::vector<float> bl(512);
		std::vector<float> br(512);
		for (int b = 0; b < budget * 2; ++b) {
			std::fill(bl.begin(), bl.end(), 0.0f);
			std::fill(br.begin(), br.end(), 0.0f);
			dev->processStereo(bl.data(), br.data(), 512);
			for (int j = 0; j < 512; ++j) {
				if (bl[static_cast<size_t>(j)] != 0.0f || br[static_cast<size_t>(j)] != 0.0f) {
					lastNonZero = b;
				}
			}
		}
		INFO(std::string(a.what) << " with travel and bias " << bias << ": last non-zero block " << lastNonZero);
		CHECK(lastNonZero >= 0);
		CHECK(lastNonZero < budget);
	}
}

TEST_CASE("explore: travel at extreme values stays bounded and finite") {
	// UNBOUNDED BY DESIGN: the pad's road folds any travel into [0, 1) — and a hand-built patch carrying RAW huge floats
	// (never folded) must be as safe. Full feedback, both drives, every arrangement, 2 s of 0 dBFS noise.
	const ps::Travel folded = padTravel(3.0e5, 1.0e6, -3.0e6);
	ps::Travel raw {};
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		raw[i] = (i % 2 == 0 ? 1.0f : -1.0f) * 1.0e6f * static_cast<float>(i + 1);
	}
	const int frames = static_cast<int>(2.0 * kRate);
	const std::vector<float> l0 = noise(frames, 0x31337u);
	const std::vector<float> r0 = noise(frames, 0x1ee7u);
	for (const ps::Travel& travel : { folded, raw }) {
		for (const Arrangement& a : kArrangements) {
			for (const float drive : { 1.0f, 0.0f }) {
				for (const float fb : { 1.0f, -1.0f }) {
					CAPTURE(a.what);
					CAPTURE(drive);
					CAPTURE(fb);
					ph::Settings s = hotOf(a.mode, a.sweep, a.tz);
					s.feedback = fb;
					s.drive = drive;
					s.lfoDepth = 1.0f;
					s.travel = travel;
					auto dev = deviceWith(s);
					std::vector<float> l = l0;
					std::vector<float> r = r0;
					float peak = 0.0f;
					bool finite = true;
					for (int n0 = 0; n0 < frames; n0 += 512) {
						const int run = std::min(512, frames - n0);
						dev->processStereo(l.data() + n0, r.data() + n0, run);
						for (int j = n0; j < n0 + run; ++j) {
							const float x = l[static_cast<size_t>(j)];
							const float y = r[static_cast<size_t>(j)];
							finite = finite && std::isfinite(x) && std::isfinite(y);
							peak = std::max({ peak, std::fabs(x), std::fabs(y) });
						}
					}
					CAPTURE(peak);
					CHECK(finite);
					CHECK(peak < (drive > 0.0f ? 8.0f : kLinearPeakBound));
					CHECK(dev->loopResetsForTest() == 0);
					for (const double v : flatten(dev->exploreForTest())) {
						CHECK(std::isfinite(v));
					}
				}
			}
		}
	}
}


//==================================================================================================
// ROUND 2 (Task 11): four more fields, and a field-scaler pill per group — `phaser_phase_space.h`'s table.
//==================================================================================================

namespace {

/// EACH ROUND-2 INDEX'S SCALE, by the header's table (restated here, not read from it).
double round2Scale(size_t i) {
	switch (i) {
	case 30: return 0.25; // rate skew, octaves
	case 31: return 0.3;  // feedback tilt
	case 32: return 1.0;  // notch pairing, octaves
	default: return 1.0;  // sweep curve, c
	}
}

/// THE STORED TRAVEL ON INDEX `i` (alone) THAT MOVES ITS OFFSET FURTHEST, signed by `positive`.
float loudest(size_t i, bool positive = true) {
	float best = 0.0f;
	double reach = 0.0;
	for (int n = 0; n < 200; ++n) {
		const float t = 0.005f * static_cast<float>(n);
		const double off = offsetOf(i, t, 1.0);
		if ((positive ? off : -off) > reach) {
			reach = positive ? off : -off;
			best = t;
		}
	}
	return best;
}

/// THE GROUP OF EACH INDEX, the header's grouping restated: 12, 12, 3, then one each.
size_t groupOfIndex(size_t i) {
	return i < 12 ? 0 : i < 24 ? 1 : i < 27 ? 2 : 3 + (i - 27);
}

/// The travel with group `g`'s indices at zero.
ps::Travel withoutGroup(ps::Travel travel, size_t g) {
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		if (groupOfIndex(i) == g) {
			travel[i] = 0.0f;
		}
	}
	return travel;
}

bool flatBitsEqual(const ps::Explore& a, const ps::Explore& b) {
	const auto fa = flatten(a);
	const auto fb = flatten(b);
	for (size_t i = 0; i < fa.size(); ++i) {
		if (std::bit_cast<uint64_t>(fa[i]) != std::bit_cast<uint64_t>(fb[i])) {
			return false;
		}
	}
	return true;
}

/// THE SWEEP CURVE, restated from the header's words: sign(s)·|s|^e, and for e < 1 under |s| < 0.05 the straight line
/// to (0.05, 0.05^e).
double curveRestated(double s, double e) {
	const double a = std::fabs(s);
	if (e < 1.0 && a < 0.05) {
		return s * std::pow(0.05, e) / 0.05;
	}
	return (s < 0.0 ? -1.0 : 1.0) * std::pow(a, e);
}

/// THE CEILING AT DRIVE 0, restated: ±0.995.
double linearCeiling(double fb) { return std::clamp(fb, -0.995, 0.995); }

} // namespace

TEST_CASE("explore round 2: the derive gives each new field its own restated offset, and the groups tile the space") {
	CHECK(ps::kPhaseCount == 34);
	CHECK(ph::kTraits.extraState == 34);
	for (size_t i = 30; i < 34; ++i) {
		for (const float t : { 0.13f, 0.41f, 0.77f }) {
			CAPTURE(i);
			CAPTURE(t);
			ps::Travel one {};
			one[i] = t;
			const auto flat = flatten(ps::exploreOf(one));
			const double want = offsetOf(i, t, round2Scale(i));
			CHECK(std::fabs(flat[i] - want) < 1.0e-6);
			CHECK(flat[i] != 0.0);
		}
	}
	// THE GROUPING, against the test's own reading of the table.
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(static_cast<size_t>(ps::groupOf(i)) == groupOfIndex(i));
	}
	CHECK(ps::kGroupCount == 10);
}

TEST_CASE("explore round 2: rate skew runs R's LFO at L's rate times 2^skew, each channel song-locked at its own rate") {
	const float t = loudest(ps::kRateSkewPhase);
	const double skew = offsetOf(ps::kRateSkewPhase, t, 0.25);
	CAPTURE(skew);
	REQUIRE(std::fabs(skew) > 0.15);
	const auto frac = [](double v) { return v - std::floor(v); };
	SUBCASE("Normal: R's rate, and R's phase on a playing transport is frac(t · Rate · 2^skew)") {
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.rateLog2Hz = -1.0f; // 0.5 Hz
		s.stereoDegrees = 90.0f;
		s.travel[ps::kRateSkewPhase] = t;
		auto dev = deviceWith(s);
		feathers::device::TransportClock clock = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
		clock.playing = true;
		double song = 100.3;
		const double tick = 16.0 / kRate;
		const auto block = [&](int ticks) {
			clock.songPositionSeconds = song;
			dev->setTransportClock(clock);
			runStereo(*dev, 16 * ticks);
			song += tick * ticks;
		};
		block(1);
		block(30);
		const double last = song - tick; // the last tick stood one tick before where the next block would start
		const double rateR = 0.5 * std::exp2(skew);
		CHECK(std::fabs(dev->lfoRateHzForTest(0) - 0.5) < 1.0e-9);
		CHECK(std::fabs(dev->lfoRateHzForTest(1) - rateR) < 1.0e-9 * rateR);
		CHECK(std::fabs(dev->lfoPhaseForTest(0) - frac(last * 0.5)) < 1.0e-9);
		// R READS ITS OWN CLOCK, Stereo/360 on.
		const double rhoR = frac(dev->lfoPhaseForTest(1) - 0.25);
		CAPTURE(rhoR);
		CHECK(std::fabs(rhoR - frac(last * rateR)) < 1.0e-9);
		// NOT VACUOUS: at this skew R is far from where L's clock would have put it.
		CHECK(std::fabs(rhoR - frac(last * 0.5)) > 0.01);
	}
	SUBCASE("barberpole: ψ_R turns at barberCyclesPerSecond of R's skewed LFO rate") {
		ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Up, 0.5f, -1.0f);
		s.travel[ps::kRateSkewPhase] = t;
		auto dev = deviceWith(s);
		runStereo(*dev, 24000);
		const double rateR = 0.5 * std::exp2(skew);
		const double wantL = std::min(0.5 * (0.5 / 0.5), 8.0);
		const double wantR = std::min(rateR * (0.5 / 0.5), 8.0);
		CHECK(std::fabs(dev->barberRateHzForTest(0) - wantL) < 1.0e-9);
		CHECK(std::fabs(dev->barberRateHzForTest(1) - wantR) < 1.0e-9);
	}
	SUBCASE("at home R's clock is L's, to the bit") {
		ph::Settings s = plainPhaser(6);
		s.lfoOn = true;
		s.stereoDegrees = 0.0f;
		auto dev = deviceWith(s);
		runStereo(*dev, 9600);
		CHECK(std::bit_cast<uint64_t>(dev->lfoRateHzForTest(0)) == std::bit_cast<uint64_t>(dev->lfoRateHzForTest(1)));
		CHECK(std::bit_cast<uint64_t>(dev->lfoPhaseForTest(0)) == std::bit_cast<uint64_t>(dev->lfoPhaseForTest(1)));
	}
}

TEST_CASE("explore round 2: feedback tilt splits the channels, or steps the barberpole phaser's voices, before the ceiling") {
	const float t = loudest(ps::kFbTiltPhase);
	const double tilt = offsetOf(ps::kFbTiltPhase, t, 0.3);
	CAPTURE(tilt);
	REQUIRE(tilt > 0.2);
	for (const float fb : { 0.4f, 0.9f }) { // 0.9 + tilt/2 passes the 0.995 ceiling: the ceiling is AFTER the tilt
		CAPTURE(fb);
		{ // phaser, Normal: L + tilt/2, R − tilt/2
			ph::Settings s = plainPhaser(6, fb);
			s.travel[ps::kFbTiltPhase] = t;
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			CHECK(std::fabs(dev->loopFeedbackForTest(0, 0) - linearCeiling(fb + 0.5 * tilt)) < 1.0e-6);
			CHECK(std::fabs(dev->loopFeedbackForTest(0, 1) - linearCeiling(fb - 0.5 * tilt)) < 1.0e-6);
		}
		{ // phaser, barberpole: voice k runs fb + tilt·(k − 1) on both channels
			ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Up, 0.5f, 1.0f);
			s.feedback = fb;
			s.travel[ps::kFbTiltPhase] = t;
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			for (int v = 0; v < 3; ++v) {
				for (int ch = 0; ch < 2; ++ch) {
					CAPTURE(v);
					CAPTURE(ch);
					CHECK(std::fabs(dev->loopFeedbackForTest(v, ch) - linearCeiling(fb + tilt * (v - 1))) < 1.0e-6);
				}
			}
		}
		{ // flanger, barberpole: one loop a channel, so the channels split it
			ph::Settings s = barberpole(ph::Mode::Flanger, ph::SweepShape::Down, 0.5f, 1.0f);
			s.feedback = fb;
			s.travel[ps::kFbTiltPhase] = t;
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			CHECK(std::fabs(dev->loopFeedbackForTest(0, 0) - linearCeiling(fb + 0.5 * tilt)) < 1.0e-6);
			CHECK(std::fabs(dev->loopFeedbackForTest(0, 1) - linearCeiling(fb - 0.5 * tilt)) < 1.0e-6);
		}
	}
}

TEST_CASE("explore round 2: notch pairing parts each pair of stages in the Phaser, and is inert in the Flanger") {
	const float t = loudest(ps::kPairingPhase, false);
	const double pair = offsetOf(ps::kPairingPhase, t, 1.0);
	CAPTURE(pair);
	REQUIRE(std::fabs(pair) > 0.3);
	ph::Settings s = plainPhaser(12);
	s.position = 0.1f;
	s.travel[ps::kPairingPhase] = t;
	auto dev = deviceWith(s);
	runStereo(*dev, 24000);
	const double centre = static_cast<double>(static_cast<float>(std::log2(800.0)));
	for (int ch = 0; ch < 2; ++ch) {
		for (int i = 0; i < 12; ++i) {
			CAPTURE(ch);
			CAPTURE(i);
			const double fc = std::clamp(std::exp2(centre + 3.0 * static_cast<double>(0.1f) + (i % 2 == 0 ? pair : -pair)),
			                             20.0, 0.45 * kRate);
			const double g = std::tan(kPi * fc / kRate);
			CHECK(std::fabs(static_cast<double>(dev->stageGForTest(ch, i)) - g) < 2.0e-5 * g);
		}
	}
	// THE FLANGER HAS NO STAGES: the same travel is the same sound as none, to the bit.
	for (const ph::SweepShape sweep : { ph::SweepShape::Normal, ph::SweepShape::Up }) {
		ph::Settings f = barberpole(ph::Mode::Flanger, sweep, 0.5f, 1.0f);
		auto plain = deviceWith(f);
		f.travel[ps::kPairingPhase] = t;
		auto paired = deviceWith(f);
		CHECK(renderIdentical(*plain, *paired, 9000, 0x5eedu));
	}
}

TEST_CASE("explore round 2: the sweep curve bends the summed sweep, sign kept, before the clamp") {
	for (const bool positive : { true, false }) {
		const float t = loudest(ps::kCurvePhase, positive);
		const double c = offsetOf(ps::kCurvePhase, t, 1.0);
		CAPTURE(c);
		REQUIRE(std::fabs(c) > 0.3);
		const double e = std::exp2(c);
		for (const float pos : { 0.5f, -0.35f }) {
			CAPTURE(pos);
			ph::Settings s = plainPhaser(6);
			s.position = pos;
			s.travel[ps::kCurvePhase] = t;
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			const double want = curveRestated(static_cast<double>(pos), e);
			CHECK(std::fabs(dev->sweepForTest(0) - want) < 1.0e-9);
			CHECK(std::fabs(dev->sweepForTest(0) - static_cast<double>(pos)) > 0.02); // not vacuous
			// THE FLANGER'S READ AT THE CURVED SWEEP.
			ph::Settings f = plainFlanger(pos);
			f.travel[ps::kCurvePhase] = t;
			auto fl = deviceWith(f);
			runStereo(*fl, 24000);
			const double d = std::exp2(fl->delayLog2ForTest() + 2.0 * want) * kRate;
			CHECK(std::fabs(fl->delayTargetSamplesForTest(0) - d) < 1.0e-6 * d);
			CHECK(std::fabs(static_cast<double>(fl->delaySamplesForTest(0)) - d) < 1.0e-3 * d);
		}
		// WITH THE LFO: the curve acts on the whole sum, Position + Depth·sin(2πφ), and only then the clamp.
		ph::Settings s = plainPhaser(6);
		s.position = 0.3f;
		s.lfoOn = true;
		s.lfoDepth = 0.9f;
		s.travel[ps::kCurvePhase] = t;
		auto dev = deviceWith(s);
		for (int k = 0; k < 20; ++k) {
			runStereo(*dev, 4800);
			const double sum = static_cast<double>(0.3f) + static_cast<double>(0.9f) * std::sin(2.0 * kPi * dev->lfoPhaseForTest(0));
			const double want = std::clamp(curveRestated(sum, e), -1.0, 1.0);
			CAPTURE(k);
			CHECK(std::fabs(dev->sweepForTest(0) - want) < 1.0e-9);
		}
	}
}

//==================================================================================================
// THE EXPLORE PILLS — THE HOUSE OVERRIDE LAW: `fieldWithOverride(f_i, knob_g)` in front of each
// phase's field, 0.5 neutral, 0 and 1 forcing the field; power off = no effect.
//==================================================================================================

namespace {

/// EACH INDEX'S SCALE, the header's table restated (the "travel survives a state round trip" case's own list).
double scaleOfIndex(size_t i) {
	constexpr double kScales[] = { 1.5, 0.5, 1.0, 2.0, 0.5, 0.5, 0.25, 0.3, 1.0, 1.0 };
	return i < 12 ? kScales[0] : i < 24 ? kScales[1] : i < 27 ? kScales[2] : kScales[3 + (i - 27)];
}
bool isShapeIndex(size_t i) { return i >= 24 && i < 27; }

/// THE LAW, RESTATED from its definition (not `fieldWithOverride`): a multiply below the neutral, a lift above it.
double overrideRestated(double f, double k) { return k <= 0.5 ? f * (2.0 * k) : f + (1.0 - f) * (2.0 * k - 1.0); }

/// THE OFFSET THE LAW GIVES, restated end to end in double: the triangle written out, the homes from their definition.
double pillOffsetRestated(size_t i, float travel, double knob) {
	const double f = triOf(homeOf(i) + static_cast<double>(travel));
	const double v = scaleOfIndex(i) * (overrideRestated(f, knob) - triOf(homeOf(i)));
	return isShapeIndex(i) ? std::fabs(v) : v;
}

/// Pills with every knob at `knob` and every power `on`.
ps::Pills allPills(float knob, bool on) {
	ps::Pills p;
	p.knob.fill(knob);
	p.on.fill(on);
	return p;
}

/// THE TRAVELS the forcing cases sweep: none, and three distinct pad positions.
const std::array<ps::Travel, 4>& forcingTravels() {
	static const std::array<ps::Travel, 4> kTravels = { ps::Travel {}, padTravel(777.0, 0.6, -0.4),
		                                                padTravel(-55.0, -0.8, 0.45), padTravel(3.0, 1.7, 2.2) };
	return kTravels;
}

} // namespace

TEST_CASE("explore pills: at knob 0.5 the derive is the pill-free one bit for bit, and zero travel is exactly zero") {
	// THE LAW'S FIXED POINT, IN FLOAT: `f · (0.5f · 2.0f)` is f · 1.0f, which is f exactly — for every home field and for
	// a dense sweep of floats in [0, 1].
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(feathers::fieldWithOverride(ps::kHomeField[i], 0.5f))
		      == std::bit_cast<uint32_t>(ps::kHomeField[i]));
	}
	for (int n = 0; n <= 4096; ++n) {
		const float f = static_cast<float>(n) / 4096.0f + (n % 7 == 0 ? 0.0f : 1.0e-5f * static_cast<float>(n % 13));
		const float fc = std::min(f, 1.0f);
		CHECK(std::bit_cast<uint32_t>(feathers::fieldWithOverride(fc, 0.5f)) == std::bit_cast<uint32_t>(fc));
	}
	// ZERO TRAVEL'S FIELDS ARE THE HOME FIELDS, bit for bit, and every group's offset is +0.0 at 0.5 — on or off.
	const ps::Fields atHome = ps::fieldsOf(ps::Travel {});
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(atHome[i]) == std::bit_cast<uint32_t>(ps::kHomeField[i]));
	}
	for (const bool on : { true, false }) {
		for (const double v : flatten(ps::exploreOf(ps::Travel {}, allPills(0.5f, on)))) {
			CHECK(std::bit_cast<uint64_t>(v) == 0u);
		}
	}
	// WITH TRAVEL, AT 0.5, EVERY GROUP IS THE PILL-FREE DERIVE: `scale · (tri(home + t) − tri(home))`, the round-2
	// expression through the same triangle, to the bit.
	for (const ps::Travel& travel : forcingTravels()) {
		const auto got = flatten(ps::exploreOf(travel, allPills(0.5f, true)));
		for (size_t i = 0; i < ps::kPhaseCount; ++i) {
			CAPTURE(i);
			const double raw
			    = scaleOfIndex(i) * (ps::triangleAt(ps::kHome[i] + static_cast<double>(travel[i])) - ps::triangleAt(ps::kHome[i]));
			const double want = isShapeIndex(i) ? std::fabs(raw) : raw;
			CHECK(std::bit_cast<uint64_t>(got[i]) == std::bit_cast<uint64_t>(want));
		}
	}
	// THROUGH THE ENGINE, every arrangement: pills explicitly at 0.5 (on, and off) render the plain device to the bit.
	for (const Arrangement& a : kArrangements) {
		for (const bool on : { true, false }) {
			CAPTURE(a.what);
			CAPTURE(on);
			const ph::Settings plain = hotOf(a.mode, a.sweep, a.tz);
			ph::Settings pills = plain;
			pills.pillKnob.fill(0.5f);
			pills.pillOn.fill(on);
			auto ref = deviceWith(plain);
			auto dev = deviceWith(pills);
			CHECK(renderIdentical(*ref, *dev, 12000 + 37, 0xfeedu));
			for (const double v : flatten(dev->exploreForTest())) {
				CHECK(std::bit_cast<uint64_t>(v) == 0u);
			}
		}
	}
}

TEST_CASE("explore pills: knob 1 forces every field in its group to 1, and knob 0 to 0, whatever the travel") {
	for (const float knob : { 1.0f, 0.0f }) {
		const double forced = knob; // the field the law drives to
		for (size_t t = 0; t < forcingTravels().size(); ++t) {
			const ps::Travel& travel = forcingTravels()[t];
			const ps::Fields fields = ps::fieldsOf(travel);
			const auto neutral = flatten(ps::exploreOf(travel));
			for (size_t g = 0; g < ps::kGroupCount; ++g) {
				CAPTURE(knob);
				CAPTURE(t);
				CAPTURE(g);
				ps::Pills p;
				p.knob[g] = knob;
				const auto got = flatten(ps::exploreOf(travel, p));
				for (size_t i = 0; i < ps::kPhaseCount; ++i) {
					CAPTURE(i);
					if (groupOfIndex(i) != g) {
						// EVERY OTHER GROUP IS UNTOUCHED, to the bit.
						CHECK(std::bit_cast<uint64_t>(got[i]) == std::bit_cast<uint64_t>(neutral[i]));
						continue;
					}
					// THE FIELD IS FORCED EXACTLY: the law returns 1.0f (or 0.0f) for this phase's own field.
					CHECK(feathers::fieldWithOverride(fields[i], knob) == knob);
					// THE OFFSET, RESTATED INDEPENDENTLY: scale · (forced − tri(home)), the triangle written out.
					const double v = scaleOfIndex(i) * (forced - triOf(homeOf(i)));
					const double want = isShapeIndex(i) ? std::fabs(v) : v;
					CHECK(std::fabs(got[i] - want) < 1.0e-6);
					// AND EXACTLY THE FORCED FIELD against the stored home field, whatever the travel.
					const double exact = scaleOfIndex(i) * (forced - static_cast<double>(ps::kHomeField[i]));
					CHECK(std::bit_cast<uint64_t>(got[i])
					      == std::bit_cast<uint64_t>(isShapeIndex(i) ? std::fabs(exact) : exact));
				}
			}
		}
	}
	// NOT VACUOUS AT ZERO TRAVEL: a forced group moves the sound at home (no home field is 0 or 1 exactly).
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(ps::kHomeField[i] > 0.0f);
		CHECK(ps::kHomeField[i] < 1.0f);
	}
	// THROUGH THE ENGINE, from the stored pills at ZERO TRAVEL: its glides land on the forced set.
	for (const float knob : { 1.0f, 0.0f }) {
		ph::Settings s = hotOf(ph::Mode::Phaser, ph::SweepShape::Normal, false);
		s.pillKnob.fill(knob);
		auto dev = deviceWith(s);
		runStereo(*dev, 24000);
		const auto got = flatten(dev->exploreForTest());
		for (size_t i = 0; i < ps::kPhaseCount; ++i) {
			CAPTURE(knob);
			CAPTURE(i);
			CHECK(std::fabs(got[i] - pillOffsetRestated(i, 0.0f, knob)) < 1.0e-6);
			CHECK(got[i] != 0.0);
		}
	}
}

TEST_CASE("explore pills: a knob between the ends is fieldWithOverride by value, below and above the neutral") {
	for (const float knob : { 0.25f, 0.75f, 0.1f, 0.9f }) {
		for (size_t t = 1; t < forcingTravels().size(); ++t) {
			const ps::Travel& travel = forcingTravels()[t];
			const ps::Fields fields = ps::fieldsOf(travel);
			const auto got = flatten(ps::exploreOf(travel, allPills(knob, true)));
			const auto neutral = flatten(ps::exploreOf(travel));
			for (size_t i = 0; i < ps::kPhaseCount; ++i) {
				CAPTURE(knob);
				CAPTURE(t);
				CAPTURE(i);
				// THE ONE LAW: exactly `fieldWithOverride` on this phase's field ...
				const double v = scaleOfIndex(i)
				                 * (static_cast<double>(feathers::fieldWithOverride(fields[i], knob))
				                    - static_cast<double>(ps::kHomeField[i]));
				CHECK(std::bit_cast<uint64_t>(got[i]) == std::bit_cast<uint64_t>(isShapeIndex(i) ? std::fabs(v) : v));
				// ... and that law is the restated one, by value.
				CHECK(std::fabs(got[i] - pillOffsetRestated(i, travel[i], knob)) < 1.0e-6);
				// NOT VACUOUS: away from the neutral the value differs from the field's own.
				CHECK(got[i] != neutral[i]);
			}
		}
	}
}

TEST_CASE("explore pills: power off is no effect at any knob — the group's offsets exactly 0, its travel at 0 to the bit") {
	const ps::Travel travel = padTravel(777.0, 0.6, -0.4);
	for (const float knob : { 0.0f, 0.25f, 0.5f, 1.0f }) {
		for (size_t g = 0; g < ps::kGroupCount; ++g) {
			CAPTURE(knob);
			CAPTURE(g);
			ps::Pills p;
			p.knob[g] = knob;
			p.on[g] = false;
			const auto got = flatten(ps::exploreOf(travel, p));
			const auto neutral = flatten(ps::exploreOf(travel));
			for (size_t i = 0; i < ps::kPhaseCount; ++i) {
				CAPTURE(i);
				CHECK(std::bit_cast<uint64_t>(got[i])
				      == (groupOfIndex(i) == g ? 0u : std::bit_cast<uint64_t>(neutral[i])));
			}
		}
	}
	// THROUGH THE ENGINE: off (at knob 1, the forcing end) renders the device whose group has no travel, to the bit.
	for (const Arrangement& a : { kArrangements[1], kArrangements[2] }) {
		for (size_t g = 0; g < ps::kGroupCount; ++g) {
			CAPTURE(a.what);
			CAPTURE(g);
			ph::Settings base = hotOf(a.mode, a.sweep, a.tz);
			base.travel = travel;
			ph::Settings off = base;
			off.pillOn[g] = false;
			off.pillKnob[g] = 1.0f; // the knob is kept while off, and heard as nothing
			ph::Settings without = base;
			without.travel = withoutGroup(travel, g);
			auto ref = deviceWith(without);
			auto dark = deviceWith(off);
			CHECK(renderIdentical(*ref, *dark, 6000, 0x1234u + static_cast<uint32_t>(g)));
			CHECK(flatBitsEqual(ref->exploreForTest(), dark->exploreForTest()));
			// NOT VACUOUS: the group really travels in `base`.
			auto full = deviceWith(base);
			runStereo(*full, 512);
			CHECK_FALSE(flatBitsEqual(full->exploreForTest(), dark->exploreForTest()));
		}
	}
	// AND A PILL'S MOVE REACHES A RUNNING DEVICE (no new travel): the glide carries it there.
	ph::Settings s = hotOf(ph::Mode::Phaser, ph::SweepShape::Normal, false);
	s.travel = travel;
	auto dev = deviceWith(s);
	runStereo(*dev, 512);
	s.pillKnob[static_cast<size_t>(ps::Group::Skew)] = 1.0f;
	dev->setTarget(s);
	runStereo(*dev, 24000);
	CHECK(std::fabs(dev->exploreForTest().stereoOctaves - pillOffsetRestated(ps::kStereoPhase, travel[29], 1.0)) < 1.0e-6);
}

TEST_CASE("explore pills: the descriptor gates what it can, and each gated pill is inert where its gate is shut") {
	using feathers::device::relevantFor;
	const auto desc = [](ph::Param p) -> const feathers::fx::ParamDesc& { return ph::kParamDescs[static_cast<size_t>(p)]; };
	for (const ps::Group g : { ps::Group::Pairing }) {
		for (const ph::Param p : { ph::pillKnobParam(g), ph::pillPowerParam(g) }) {
			CHECK(desc(p).gateControl == static_cast<int16_t>(ph::Param::Mode));
			CHECK(relevantFor(desc(p), static_cast<float>(ph::Mode::Phaser)));
			CHECK_FALSE(relevantFor(desc(p), static_cast<float>(ph::Mode::Flanger)));
		}
	}
	for (const ps::Group g : { ps::Group::Stagger, ps::Group::Shape }) { // Rate skew: its own case below
		for (const ph::Param p : { ph::pillKnobParam(g), ph::pillPowerParam(g) }) {
			CHECK(desc(p).gateControl == static_cast<int16_t>(ph::Param::LfoOn));
			CHECK(relevantFor(desc(p), 1.0f));
			CHECK_FALSE(relevantFor(desc(p), 0.0f));
		}
	}
	for (const ps::Group g : { ps::Group::Spread, ps::Group::Colour, ps::Group::Bias, ps::Group::Skew, ps::Group::RateSkew, ps::Group::FbTilt,
	                           ps::Group::Curve }) {
		CHECK(desc(ph::pillKnobParam(g)).gateControl == -1);
	}
	// THE KEYS, as listed in the report.
	CHECK(std::string(desc(ph::Param::XRateSkew).key) == "phaser_x_rate_skew_field");
	CHECK(std::string(desc(ph::Param::XFbTiltOn).key) == "phaser_x_fb_tilt_on");
	// INERT WITH THE LFO OFF, Normal sweep, both engines: the Stagger, Shape and Rate-skew groups' travel is the same
	// sound as none, to the bit (each reaches the sweep only through LfoDepth · lfo, which is 0).
	const ps::Travel travel = padTravel(42.0, 0.7, 0.3);
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		ph::Settings s = hotOf(mode, ph::SweepShape::Normal, false);
		s.lfoOn = false;
		ph::Settings lfoOnly = s;
		for (size_t i = 0; i < ps::kPhaseCount; ++i) {
			const size_t g = groupOfIndex(i);
			lfoOnly.travel[i] = (g == 1 || g == 2 || g == 6) ? travel[i] : 0.0f;
		}
		auto plain = deviceWith(s);
		auto moved = deviceWith(lfoOnly);
		CAPTURE(static_cast<int>(mode));
		CHECK(renderIdentical(*plain, *moved, 9000, 0xabcu));
	}
}


TEST_CASE("explore review: after a rate-skew excursion R re-converges on L, and the device is the never-skewed one again") {
	// THE DECISION: zero travel is the textbook sound, even after a played excursion. A skewed R drifts from L; back
	// home it glides onto L (≈200 ms) and then IS L's clock — so its phase is L's + Stereo/360 by value, and once the
	// loop's memory of the excursion has decayed the output equals a device that was never skewed, to the bit.
	for (const ph::SweepShape sweep : { ph::SweepShape::Normal, ph::SweepShape::Up }) {
		CAPTURE(static_cast<int>(sweep));
		ph::Settings s = barberpole(ph::Mode::Phaser, sweep, 0.5f, 0.0f); // 1 Hz
		s.lfoOn = true;
		s.stereoDegrees = 0.0f; // Stereo 0: any drift left at home is audible as R moving apart from L
		ph::Settings skewed = s;
		skewed.travel[ps::kRateSkewPhase] = loudest(ps::kRateSkewPhase);
		auto a = deviceWith(skewed);
		auto b = deviceWith(s);
		const auto both = [&](int frames, float level, uint32_t seed) {
			const std::vector<float> l0 = noise(frames, seed, level);
			const std::vector<float> r0 = noise(frames, seed ^ 0x77u, level);
			std::vector<float> la = l0, ra = r0, lb = l0, rb = r0;
			for (int n0 = 0; n0 < frames; n0 += 512) {
				const int run = std::min(512, frames - n0);
				a->processStereo(la.data() + n0, ra.data() + n0, run);
				b->processStereo(lb.data() + n0, rb.data() + n0, run);
			}
		};
		both(3 * 48000, 0.5f, 0x1111u);
		const auto around = [](double d) { return d - std::floor(d + 0.5); };
		// NOT VACUOUS: three seconds of skew have taken R well away from L.
		CHECK(std::fabs(around(a->lfoPhaseForTest(1) - a->lfoPhaseForTest(0))) > 0.05);
		CHECK_FALSE(a->rConvergedForTest(0));
		a->setTarget(s); // HOME
		both(48000 / 20, 0.5f, 0x2222u); // 50 ms: the skew's 20 ms glide has landed, R is still gliding
		const double early = std::fabs(around(a->lfoPhaseForTest(1) - a->lfoPhaseForTest(0)));
		CHECK_FALSE(a->rConvergedForTest(0));
		both(48000 / 4, 0.5f, 0x3333u);
		CHECK(std::fabs(around(a->lfoPhaseForTest(1) - a->lfoPhaseForTest(0))) < early); // gliding, not jumping
		both(5 * 48000, 0.5f, 0x4444u);
		CHECK(a->rConvergedForTest(0));
		CHECK(a->rConvergedForTest(1));
		// BY VALUE: R's φ is L's + Stereo/360, to the bit, and both are the never-skewed device's.
		CHECK(std::bit_cast<uint64_t>(a->lfoPhaseForTest(1))
		      == std::bit_cast<uint64_t>(ph::channelLfoPhase(a->lfoPhaseForTest(0), 0.0, 1)));
		CHECK(std::bit_cast<uint64_t>(a->lfoPhaseForTest(1)) == std::bit_cast<uint64_t>(b->lfoPhaseForTest(1)));
		CHECK(std::bit_cast<uint64_t>(a->barberPhaseForTest(1)) == std::bit_cast<uint64_t>(b->barberPhaseForTest(1)));
		CHECK(a->lfoRateHzForTest(1) == b->lfoRateHzForTest(1));
		// AND THE SOUND: silence until both loops are exactly empty, then the same noise through both — bit-identical.
		both(3 * 48000, 0.0f, 0x5555u);
		CHECK(renderIdentical(*a, *b, 24000, 0x6666u));
	}
}

TEST_CASE("explore review: the sweep curve has a finite slope through zero, so the flanger's read never steps at a crossing") {
	// e < 1 MADE |s|^e VERTICAL AT 0 and the per-sample read jumped ~22 samples at every crossing. Now a straight line
	// under |s| < ε = 0.05 joins the power curve C0. THE BOUND, DERIVED: the read is d = D·2^(2·f(s)) for the curve f, and
	// s = Depth·sin(2πφ) moves at most Depth·2π·Rate/sr a sample, so a sample moves the read by at most
	//     D·2ln2 · max(2^(2f)·f′) · Depth·2π·Rate/sr,
	// and 2^(2f)·f′ is largest just under ε: 2^(2ε^e) · ε^(e−1) (the line's slope; the power curve's beyond ε is e times
	// it, and 2^(2f) grows only to 4 while f′ falls as |s|^(e−1)). A 10% margin covers the tick's ramps.
	ph::Settings s = plainFlanger(0.0f);
	s.lfoOn = true;
	s.lfoDepth = 1.0f;
	s.rateLog2Hz = 1.0f; // 2 Hz: eight crossings over the second measured
	s.travel[ps::kCurvePhase] = loudest(ps::kCurvePhase, false);
	s.pillKnob[static_cast<size_t>(ps::Group::Curve)] = 0.0f; // the field forced to 0: the most negative c the layer has
	auto dev = deviceWith(s);
	runStereo(*dev, 9600);
	const double e = std::exp2(dev->exploreForTest().sweepCurve);
	CAPTURE(e);
	// THE LAYER REACHES e ≈ 0.81 AT MOST (index 33's home sits at tri 0.30, so c ≥ −0.30 with the field forced to 0 — the
	// round-2 multiplier's 200% reached −0.6, which the house law has no road to); e = 0.25 is checked on the pure curve
	// below.
	REQUIRE(e < 0.85);
	const double eps = 0.05;
	const double D = std::exp2(dev->delayLog2ForTest()) * kRate;
	const double bound = 1.1 * D * 2.0 * std::numbers::ln2 * std::exp2(2.0 * std::pow(eps, e)) * std::pow(eps, e - 1.0)
	                     * 1.0 * 2.0 * kPi * 2.0 / kRate;
	std::vector<float> l(1, 0.0f);
	std::vector<float> r(1, 0.0f);
	double last = static_cast<double>(dev->delaySamplesForTest(0));
	double worst = 0.0;
	for (int n = 0; n < 48000; ++n) {
		l[0] = r[0] = 0.0f;
		dev->processStereo(l.data(), r.data(), 1);
		const double now = static_cast<double>(dev->delaySamplesForTest(0));
		worst = std::max(worst, std::fabs(now - last));
		last = now;
	}
	MESSAGE("sweep curve e = " << e << ": the read's largest step a sample " << worst << " (bound " << bound << ")");
	CHECK(worst < bound);
	CHECK(worst > 0.2 * bound); // not vacuous: the read really moves near the bound
	// AND THE CURVE ITSELF AT e = 0.25 (c = −2): through zero in steps of 1e-6, no step of f exceeds the line's slope
	// ε^(e−1) times the step (9.46e-6), where the old |s|^0.25 stepped 0.0316 on the first step off zero.
	const double e4 = 0.25;
	double worstStep = 0.0;
	double prev = ph::curvedSweepOf(-0.2, e4);
	for (int k = 1; k <= 400000; ++k) {
		const double v = ph::curvedSweepOf(-0.2 + 1.0e-6 * k, e4);
		worstStep = std::max(worstStep, std::fabs(v - prev));
		prev = v;
	}
	CAPTURE(worstStep);
	CHECK(worstStep <= std::pow(eps, e4 - 1.0) * 1.0e-6 * (1.0 + 1.0e-9));
	CHECK(ph::curvedSweepOf(1.0, e4) == 1.0);
	CHECK(ph::curvedSweepOf(-1.0, e4) == -1.0);
	CHECK(std::fabs(ph::curvedSweepOf(eps, e4) - std::pow(eps, e4)) < 1.0e-15); // C0 where the line meets the curve
}

TEST_CASE("explore review: at Drive 0 the drive bias is no bias at all — bit-identical, no DC blocker, the model agreeing") {
	// THE BIAS RAMPS IN WITH DRIVE (`biasTanhOf`), so at Drive 0 the curve has nothing to bias — and now nothing runs for
	// it either: the DC blocker is keyed on the EFFECTIVE bias. So the window's "Bias grey at Drive 0" is true.
	const float t = loudest(ps::kDrivePhase);
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		CAPTURE(static_cast<int>(mode));
		ph::Settings s = mode == ph::Mode::Phaser ? plainPhaser(6, 0.8f) : plainFlanger(0.3f, 0.8f, false);
		s.drive = 0.0f;
		ph::Settings biased = s;
		biased.travel[ps::kDrivePhase] = t;
		auto plain = deviceWith(s);
		auto dev = deviceWith(biased);
		CHECK(renderIdentical(*plain, *dev, 24000, 0x0d0du));
		CHECK(dev->exploreForTest().driveBias != 0.0); // not vacuous: the derived bias is there
		const ph::ResponseModel m = ph::responseModelOf(biased, {}, kRate);
		const ph::ResponseModel h = ph::responseModelOf(s, {}, kRate);
		CHECK_FALSE(m.dcBlock);
		for (const double hz : { 20.0, 200.0, 2000.0 }) {
			CHECK(ph::magnitudeDb(m, hz) == ph::magnitudeDb(h, hz));
		}
	}
}

TEST_CASE("explore review: rate skew is heard under the barberpole with the LFO off, so LFO On is not its gate") {
	// ψ_R's RATE moves R's three windows, and with the LFO off every voice sits on Position — so the skew is inert only
	// while the voices are all alike. They are not when the Fb tilt gives each phaser voice its own loop, nor when the
	// spread gives each flanger tap its own delay: then which voice the windows favour is heard, and the skew moves it.
	// So the pill is ungated in the descriptor and `PhaserExpanded` shows it live with the LFO on OR under the
	// barberpole. In Normal with the LFO off it is inert (the descriptor-gates case above renders that).
	ps::Travel travel {};
	travel[ps::kRateSkewPhase] = loudest(ps::kRateSkewPhase);
	travel[ps::kFbTiltPhase] = loudest(ps::kFbTiltPhase);
	for (size_t i = 0; i < 3; ++i) {
		travel[ps::kSpreadFirst + i] = 0.2f + 0.15f * static_cast<float>(i);
	}
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		CAPTURE(static_cast<int>(mode));
		// A SLOW RATE: with the LFO off ψ turns at Rate·0.5/(1/64) (Depth taken at its floor), capped at 8 Hz — at 1 Hz both
		// channels sit on the cap and the skew is inert; under 0.25 Hz they part.
		ph::Settings s = barberpole(mode, ph::SweepShape::Up, 0.5f, -4.0f); // 0.0625 Hz
		s.lfoOn = false;
		s.feedback = 0.6f;
		s.position = 0.2f;
		ph::Settings without = s;
		without.travel = travel;
		without.travel[ps::kRateSkewPhase] = 0.0f;
		ph::Settings with = s;
		with.travel = travel;
		auto a = deviceWith(without);
		auto b = deviceWith(with);
		CHECK_FALSE(renderIdentical(*a, *b, 48000, 0xbeefu));
		CHECK(b->barberRateHzForTest(1) != b->barberRateHzForTest(0)); // the mechanism: ψ_R's own rate
	}
	CHECK(ph::kParamDescs[static_cast<size_t>(ph::Param::XRateSkew)].gateControl == -1);
	CHECK(ph::kParamDescs[static_cast<size_t>(ph::Param::XRateSkewOn)].gateControl == -1);
}

TEST_CASE("explore review: Sweep is heard with the LFO off once the Fb tilt makes the barberpole's voices differ, so it has no gate") {
	// THE DECISION: with the LFO off every voice sits on Position, and Normal is one loop at Feedback while Up is
	// three loops at Feedback − tilt, Feedback and Feedback + tilt, weighted by windows that keep turning. BY VALUE: the
	// loops' feedback (the per-voice accessor) differs from Normal's, and so does the output.
	const float t = loudest(ps::kFbTiltPhase);
	const double tilt = offsetOf(ps::kFbTiltPhase, t, 0.3);
	REQUIRE(tilt > 0.2);
	ph::Settings normal = plainPhaser(6, 0.5f);
	normal.lfoOn = false;
	normal.position = 0.2f;
	normal.travel[ps::kFbTiltPhase] = t;
	ph::Settings up = normal;
	up.sweep = ph::SweepShape::Up;
	auto n = deviceWith(normal);
	auto u = deviceWith(up);
	CHECK_FALSE(renderIdentical(*n, *u, 48000, 0x5eedu));
	CHECK(u->voicesRunningForTest() == 3);
	CHECK(std::fabs(u->loopFeedbackForTest(0, 0) - linearCeiling(0.5 - tilt)) < 1.0e-6);
	CHECK(std::fabs(u->loopFeedbackForTest(2, 0) - linearCeiling(0.5 + tilt)) < 1.0e-6);
	CHECK(std::fabs(n->loopFeedbackForTest(0, 0) - linearCeiling(0.5 + 0.5 * tilt)) < 1.0e-6);
	CHECK(u->loopFeedbackForTest(0, 0) != n->loopFeedbackForTest(0, 0));
	// AND THE DESCRIPTOR SAYS SO: no gate.
	CHECK(ph::kParamDescs[static_cast<size_t>(ph::Param::Sweep)].gateControl == -1);
}
//==================================================================================================
// THE RESPONSE MODEL (Task 8): the plot's picture, built through the engine's own pure functions.
//==================================================================================================

namespace {

/// A SINE'S STEADY-STATE GAIN THROUGH A FRESH ENGINE (mono: the left channel's road), in dB — the output's RMS over
/// the input's across `windowSeconds` after `settleSeconds` — and the readings published by the block that ends
/// nearest the window's middle. Blocks of 512, a whole number of ticks, so every publish is a tick's end.
struct SineRun {
	double gainDb = 0.0;
	std::vector<float> readings;
};
SineRun sineThrough(const ph::Settings& s, double hz, double settleSeconds = 0.5, double windowSeconds = 0.5,
                    double amplitude = 0.5) {
	auto dev = deviceWith(s);
	Captured got;
	dev->setReadingsSink({ &got, &Captured::publish });
	const int settle = static_cast<int>(settleSeconds * kRate);
	const int total = settle + static_cast<int>(windowSeconds * kRate);
	const int middle = settle + (total - settle) / 2;
	const double w = 2.0 * kPi * hz / kRate;
	double in2 = 0.0;
	double out2 = 0.0;
	SineRun run;
	std::vector<float> block(512);
	for (int n0 = 0; n0 < total; n0 += 512) {
		const int frames = std::min(512, total - n0);
		for (int j = 0; j < frames; ++j) {
			block[static_cast<size_t>(j)] = static_cast<float>(amplitude * std::sin(w * static_cast<double>(n0 + j)));
		}
		const std::vector<float> x(block.begin(), block.begin() + frames);
		dev->processMono(block.data(), frames);
		for (int j = 0; j < frames; ++j) {
			if (n0 + j >= settle) {
				in2 += static_cast<double>(x[static_cast<size_t>(j)]) * x[static_cast<size_t>(j)];
				out2 += static_cast<double>(block[static_cast<size_t>(j)]) * block[static_cast<size_t>(j)];
			}
		}
		if (run.readings.empty() && n0 + frames >= middle) {
			run.readings = got.values;
		}
	}
	run.gainDb = 10.0 * std::log10(out2 / in2);
	return run;
}

/// FIVE TEST FREQUENCIES AWAY FROM THE NOTCHES: from a log grid of integer hertz, 150 Hz to `topFraction` of the rate
/// (the corner's own ceiling for the phaser; the model's trusted band for the flanger), those the model puts above
/// −15 dB, five of them spread across the qualifiers. The model needs no reading to place them: the LFO is off (or,
/// under the barberpole, at 0.01 Hz, where ψ hardly moves from 0).
std::vector<double> awayFromNotches(const ph::Settings& s) {
	const ph::ResponseModel m = ph::responseModelOf(s, ph::ResponseInputs {}, kRate);
	std::vector<double> ok;
	const double topFraction = s.mode == ph::Mode::Flanger ? ph::kFlangerTrustedFraction : ph::kCornerMaxFraction;
	const double top = topFraction * kRate;
	for (int i = 0; i < 48; ++i) {
		const double hz = std::round(150.0 * std::pow(top / 150.0, static_cast<double>(i) / 47.0));
		if (ph::magnitudeDb(m, hz) > -15.0f) {
			ok.push_back(hz);
		}
	}
	std::vector<double> out;
	if (ok.size() < 5) {
		return ok;
	}
	for (size_t k = 0; k < 5; ++k) {
		out.push_back(ok[k * (ok.size() - 1) / 4]);
	}
	return out;
}

/// THE MODEL AGAINST THE ENGINE at each frequency: |model − measured| within `tolDb`, the model built from the run's
/// own readings. Returns the largest disagreement.
double agreement(const ph::Settings& s, const std::vector<double>& freqs, double tolDb, double amplitude = 0.5) {
	double worst = 0.0;
	for (const double hz : freqs) {
		const SineRun run = sineThrough(s, hz, 0.5, 0.5, amplitude);
		REQUIRE(run.readings.size() == ph::kReadingCount);
		const ph::ResponseInputs in = ph::responseInputsOf(run.readings.data(), run.readings.size(), 0);
		const double model = static_cast<double>(ph::magnitudeDb(ph::responseModelOf(s, in, kRate), hz));
		CAPTURE(hz);
		CAPTURE(model);
		CAPTURE(run.gainDb);
		CHECK(std::fabs(model - run.gainDb) < tolDb);
		worst = std::max(worst, std::fabs(model - run.gainDb));
	}
	return worst;
}

} // namespace

TEST_CASE("response model agrees with the engine — phaser") {
	// THE ONE PLACE A RENDERED BUFFER IS THE RIGHT INSTRUMENT: the claim is about GAIN. LFO off, so the sweep is static
	// and the engine is a linear time-invariant system (Drive 0) whose steady-state gain at a sine is |H|.
	double caseWorst = 0.0; // the case's one summary line, below
	for (const int stages : { 2, 6, 12 }) {
		for (const float fb : { 0.0f, 0.7f, -0.7f }) {
			for (const bool damped : { false, true }) {
				CAPTURE(stages);
				CAPTURE(fb);
				CAPTURE(damped);
				ph::Settings s = plainPhaser(stages, fb);
				s.position = 0.1f;
				s.dampingLog2Hz = damped ? static_cast<float>(std::log2(3000.0)) : static_cast<float>(ph::kDampingLog2Max);
				const std::vector<double> freqs = awayFromNotches(s);
				REQUIRE(freqs.size() == 5);
				const double worst = agreement(s, freqs, 0.5);
				INFO("phaser " << stages << " stages, fb " << fb << std::string(damped ? ", damped" : ", open") << ": worst "
				               << worst << " dB");
				caseWorst = std::max(caseWorst, worst);
				if (damped && fb != 0.0f) {
					// NOT VACUOUS: at these frequencies the damping moves the response by more than the tolerance, so a
					// model that dropped it from the loop could not pass.
					ph::Settings open = s;
					open.dampingLog2Hz = static_cast<float>(ph::kDampingLog2Max);
					const ph::ResponseModel a = ph::responseModelOf(s, {}, kRate);
					const ph::ResponseModel b = ph::responseModelOf(open, {}, kRate);
					double moved = 0.0;
					for (const double hz : freqs) {
						moved = std::max(moved, std::fabs(static_cast<double>(ph::magnitudeDb(a, hz) - ph::magnitudeDb(b, hz))));
					}
					CHECK(moved > 1.0);
				}
			}
		}
	}
	MESSAGE("phaser: the model's worst disagreement over every arrangement " << caseWorst << " dB");
}

TEST_CASE("response model agrees with the engine — flanger") {
	// Position 0.33 so every read is FRACTIONAL (asserted: the B = 1 kernel is a delta at a whole sample, so a whole read
	// would test nothing of the kernel — at 0.3 the through-zero 5 ms read was 456.000) and so through-zero is off its
	// crossing (at s = 0 it cancels everything at Mix 0.5).
	double caseWorst = 0.0; // the case's one summary line, below
	for (const double delayMs : { 1.0, 5.0 }) {
		for (const float fb : { 0.0f, 0.7f, -0.7f }) {
			for (const bool tz : { false, true }) {
				for (const bool damped : { false, true }) {
					CAPTURE(delayMs);
					CAPTURE(fb);
					CAPTURE(tz);
					CAPTURE(damped);
					ph::Settings s = plainFlanger(0.33f, fb, tz);
					s.delayLog2s = static_cast<float>(std::log2(delayMs * 0.001));
					s.dampingLog2Hz
					    = damped ? static_cast<float>(std::log2(3000.0)) : static_cast<float>(ph::kDampingLog2Max);
					const ph::ResponseModel m = ph::responseModelOf(s, {}, kRate);
					const double d = m.delaySamples[0];
					CAPTURE(d);
					REQUIRE(std::fabs(d - std::round(d)) > 0.05);
					const std::vector<double> freqs = awayFromNotches(s);
					REQUIRE(freqs.size() == 5);
					const double worst = agreement(s, freqs, 0.5);
					INFO("flanger " << delayMs << " ms, fb " << fb << std::string(tz ? ", through-zero" : "")
					                << std::string(damped ? ", damped" : "") << ": worst " << worst << " dB");
					caseWorst = std::max(caseWorst, worst);
					if (damped && fb != 0.0f) {
						// NOT VACUOUS: the damping moves the response here by more than the tolerance.
						ph::Settings open = s;
						open.dampingLog2Hz = static_cast<float>(ph::kDampingLog2Max);
						const ph::ResponseModel b = ph::responseModelOf(open, {}, kRate);
						double moved = 0.0;
						for (const double hz : freqs) {
							moved = std::max(moved, std::fabs(static_cast<double>(ph::magnitudeDb(m, hz) - ph::magnitudeDb(b, hz))));
						}
						CHECK(moved > 1.0);
					}
				}
			}
		}
	}
	MESSAGE("flanger: the model's worst disagreement over every arrangement " << caseWorst << " dB");
}

TEST_CASE("response model agrees with the engine — drive bias and the DC blocker") {
	// THE MODEL'S LINEARISED LOOP: with Drive 0.6 and the explore layer's drive bias, the saturator's slope at zero is
	// 1 − tanh²(b) and the 10 Hz DC blocker is in the loop. A 1e-3 sine keeps the loop in its linear region (the
	// saturator's argument stays ~1e-2), so the steady-state gain is the linearised one.
	ph::Settings s = plainPhaser(6, 0.9f);
	s.drive = 0.6f;
	// THE BIAS AT ITS LARGEST: the stored travel on index 28 that moves the derive furthest.
	float best = 0.0f;
	double biggest = 0.0;
	for (int k = 0; k < 100; ++k) {
		ps::Travel t {};
		t[ps::kDrivePhase] = 0.01f * static_cast<float>(k);
		const double b = std::fabs(ps::exploreOf(t).driveBias);
		if (b > biggest) {
			biggest = b;
			best = t[ps::kDrivePhase];
		}
	}
	s.travel[ps::kDrivePhase] = best;
	const ph::ResponseModel m = ph::responseModelOf(s, {}, kRate);
	REQUIRE(m.dcBlock);
	REQUIRE(m.loopGain < m.feedback * 0.95);
	std::vector<double> freqs = awayFromNotches(s);
	REQUIRE(freqs.size() == 5);
	freqs.push_back(20.0);
	// NOT VACUOUS, each term on its own: the slope moves |H| by more than the tolerance somewhere in the set, and the
	// DC blocker does at 20 Hz.
	ph::ResponseModel noSlope = m;
	noSlope.loopGain = m.feedback;
	noSlope.loopGains = m.feedbacks; // `responseAt` reads the per-loop gains (the tilt makes them one a loop)
	ph::ResponseModel noBlocker = m;
	noBlocker.dcBlock = false;
	double slopeMoves = 0.0;
	for (const double hz : freqs) {
		slopeMoves = std::max(slopeMoves, std::fabs(static_cast<double>(ph::magnitudeDb(m, hz) - ph::magnitudeDb(noSlope, hz))));
	}
	const double blockerMoves = std::fabs(static_cast<double>(ph::magnitudeDb(m, 20.0) - ph::magnitudeDb(noBlocker, 20.0)));
	CAPTURE(slopeMoves);
	CAPTURE(blockerMoves);
	CHECK(slopeMoves > 0.5);
	CHECK(blockerMoves > 0.5);
	const double worst = agreement(s, freqs, 0.5, 1.0e-3);
	MESSAGE("drive bias: worst " << worst << " dB; the slope moves |H| by " << slopeMoves << " dB, the blocker by "
	                             << blockerMoves << " dB at 20 Hz");
}

TEST_CASE("response model agrees with the engine — with travel") {
	// THE PER-STAGE CORNERS: the explore spread moves every stage on its own, and the colour moves the damping. The model
	// sees them through `stageCornerHz` and `colouredDampingCornerHz`, as the engine's tick does.
	ph::Settings s = plainPhaser(12, 0.5f);
	s.position = -0.1f;
	s.dampingLog2Hz = static_cast<float>(std::log2(3000.0));
	for (size_t i = 0; i < ph::phase_space::kSpreadCount; ++i) {
		s.travel[ph::phase_space::kSpreadFirst + i] = 0.11f + 0.07f * static_cast<float>(i);
	}
	s.travel[ph::phase_space::kDampingPhase] = 0.3f;
	// NOT VACUOUS: the travel spreads the stages and colours the damping.
	const ph::phase_space::Explore e = ph::phase_space::exploreOf(s.travel);
	REQUIRE(e.spreadOctaves[0] != e.spreadOctaves[1]);
	REQUIRE(e.dampingOctaves != 0.0);
	const ph::ResponseModel m = ph::responseModelOf(s, {}, kRate);
	REQUIRE(m.G[0][0] != m.G[0][1]);
	const std::vector<double> freqs = awayFromNotches(s);
	REQUIRE(freqs.size() == 5);
	MESSAGE("phaser with travel: worst " << agreement(s, freqs, 0.5) << " dB");
}

TEST_CASE("response model agrees with the engine — pills forcing their fields at 0 and 1") {
	// THE MODEL DERIVES WITH THE SAME PILLS (`exploreOf(travel, pillsOf(s))`): Spread forced up and Colour forced down,
	// then the reverse, at zero travel (where the pills alone move the sound) and with travel.
	for (const float knob : { 1.0f, 0.0f }) {
		for (const bool travelled : { false, true }) {
			CAPTURE(knob);
			CAPTURE(travelled);
			ph::Settings s = plainPhaser(12, 0.5f);
			s.position = -0.1f;
			s.dampingLog2Hz = static_cast<float>(std::log2(3000.0));
			if (travelled) {
				s.travel = padTravel(-55.0, -0.8, 0.45);
				s.travel = withoutGroup(withoutGroup(s.travel, 6), 1); // rate skew and stagger: not an instant's
			}
			s.pillKnob[static_cast<size_t>(ps::Group::Spread)] = knob;
			s.pillKnob[static_cast<size_t>(ps::Group::Colour)] = 1.0f - knob;
			// THE ENGINE'S LANDED LAYER IS THE MODEL'S.
			auto dev = deviceWith(s);
			runStereo(*dev, 24000);
			const auto engine = flatten(dev->exploreForTest());
			const auto model = flatten(ps::exploreOf(s.travel, ph::pillsOf(s)));
			for (size_t i = 0; i < ps::kPhaseCount; ++i) {
				CAPTURE(i);
				CHECK(std::fabs(engine[i] - model[i]) < 1.0e-12);
			}
			// NOT VACUOUS: the forced pills move the response from the neutral one by more than the tolerance.
			ph::Settings neutral = s;
			neutral.pillKnob.fill(0.5f);
			const std::vector<double> freqs = awayFromNotches(s);
			REQUIRE(freqs.size() == 5);
			const ph::ResponseModel a = ph::responseModelOf(s, {}, kRate);
			const ph::ResponseModel b = ph::responseModelOf(neutral, {}, kRate);
			double moved = 0.0;
			for (const double hz : freqs) {
				moved = std::max(moved, std::fabs(static_cast<double>(ph::magnitudeDb(a, hz) - ph::magnitudeDb(b, hz))));
			}
			CHECK(moved > 1.0);
			MESSAGE("pills at " << knob << std::string(travelled ? ", travelled" : ", home") << ": worst " << agreement(s, freqs, 0.5)
			                    << " dB, the pills moved |H| " << moved << " dB");
		}
	}
}

TEST_CASE("response model agrees with the engine — barberpole") {
	// THE LFO AT ITS SLOWEST (0.01 Hz), so the three voices are quasi-static across the measurement — and the window
	// short (0.2 s) and the Depth modest (0.2), because the motion that remains is what limits the agreement, not the
	// model: ψ moves 0.002 over the window, so a flanger tap at 2.5 ms moves ~0.13 samples, ±7° of phase at 14.4 kHz.
	// (At Depth 0.5 over 0.5 s it moved ~0.8 samples and the 14.4 kHz row disagreed by 0.8 dB, MEASURED 2026-10-03,
	// while every row under 5 kHz agreed within 0.1 dB.) The model reads ψ from the readings at the window's middle.
	double caseWorst = 0.0; // the case's one summary line, below
	for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
		for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(static_cast<int>(sweep));
			ph::Settings s = barberpole(mode, sweep, 0.2f, static_cast<float>(ph::kRateLog2Min));
			s.feedback = 0.5f;
			const std::vector<double> freqs = awayFromNotches(s);
			REQUIRE(freqs.size() == 5);
			double worst = 0.0;
			for (const double hz : freqs) {
				const SineRun run = sineThrough(s, hz, 0.3, 0.2);
				const ph::ResponseInputs in = ph::responseInputsOf(run.readings.data(), run.readings.size(), 0);
				// NOT VACUOUS: the barberpole is running, with all three voices heard.
				REQUIRE(in.barberPhase != 0.0);
				const ph::ResponseModel m = ph::responseModelOf(s, in, kRate);
				REQUIRE(m.voices == 3);
				const double model = static_cast<double>(ph::magnitudeDb(m, hz));
				CAPTURE(hz);
				CAPTURE(model);
				CAPTURE(run.gainDb);
				CHECK(std::fabs(model - run.gainDb) < 0.2);
				worst = std::max(worst, std::fabs(model - run.gainDb));
			}
			caseWorst = std::max(caseWorst, worst);
		}
	}
	MESSAGE("barberpole: the model's worst disagreement over both modes and directions " << caseWorst << " dB");
}

TEST_CASE("response model — barberpole: the residue is the motion, not the model") {
	{
		// HALVING THE WINDOW SHRINKS IT ABOUT FOUR TIMES. The measurement is an RMS over a window while the voices move; the model is the instant at the
		// window's middle. A motion-averaging error is second order in the window (the curvature of |H|² over it), so
		// halving the window cuts it ~4×; a BIAS in the model would not move at all. Depth 0.5, the flanger at the top
		// of its trusted band, where the motion is largest.
		ph::Settings s = barberpole(ph::Mode::Flanger, ph::SweepShape::Up, 0.5f, static_cast<float>(ph::kRateLog2Min));
		s.feedback = 0.5f;
		const std::vector<double> freqs = awayFromNotches(s);
		REQUIRE(freqs.size() == 5);
		const double hz = freqs.back();
		const auto errorOver = [&](double window) {
			const SineRun run = sineThrough(s, hz, 0.3, window);
			const ph::ResponseInputs in = ph::responseInputsOf(run.readings.data(), run.readings.size(), 0);
			return std::fabs(static_cast<double>(ph::magnitudeDb(ph::responseModelOf(s, in, kRate), hz)) - run.gainDb);
		};
		const double full = errorOver(0.4);
		const double half = errorOver(0.2);
		CAPTURE(hz);
		CAPTURE(full);
		CAPTURE(half);
		MESSAGE("barberpole discriminator at " << hz << " Hz: window 0.4 s " << full << " dB, 0.2 s " << half << " dB");
		CHECK(full > 0.2); // there is a residue to shrink
		CHECK(half < 0.4 * full);
	}
}


TEST_CASE("response model agrees with the engine — feedback tilt, notch pairing and sweep curve") {
	// ROUND 2'S FIELDS IN THE MODEL: the tilt per loop, the pairing per stage, the curve on the sweep — each at its
	// loudest, all three together, and the model built from the run's readings. Mono, so the LEFT channel (L + tilt/2).
	const auto loudestOf = [](size_t i) {
		float best = 0.0f;
		double reach = 0.0;
		for (int n = 0; n < 200; ++n) {
			const float t = 0.005f * static_cast<float>(n);
			const double off = std::fabs(offsetOf(i, t, 1.0));
			if (off > reach) {
				reach = off;
				best = t;
			}
		}
		return best;
	};
	ps::Travel travel {};
	for (const size_t i : { ps::kFbTiltPhase, ps::kPairingPhase, ps::kCurvePhase }) {
		travel[i] = loudestOf(i);
	}
	double caseWorst = 0.0;
	SUBCASE("phaser and flanger, Normal") {
		for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
			CAPTURE(static_cast<int>(mode));
			ph::Settings s = mode == ph::Mode::Phaser ? plainPhaser(12, 0.5f) : plainFlanger(0.33f, 0.5f, false);
			s.position = 0.33f;
			s.travel = travel;
			// NOT VACUOUS: the three fields move the drawn response by more than the tolerance at these frequencies.
			ph::Settings home = s;
			home.travel = {};
			const std::vector<double> freqs = awayFromNotches(s);
			REQUIRE(freqs.size() == 5);
			const ph::ResponseModel a = ph::responseModelOf(s, {}, kRate);
			const ph::ResponseModel b = ph::responseModelOf(home, {}, kRate);
			double moved = 0.0;
			for (int k = 0; k < 64; ++k) {
				const double hz = 100.0 * std::pow(140.0, k / 63.0);
				moved = std::max(moved, std::fabs(static_cast<double>(ph::magnitudeDb(a, hz) - ph::magnitudeDb(b, hz))));
			}
			CAPTURE(moved);
			CHECK(moved > 1.0);
			CHECK(a.feedbacks[0] != b.feedbacks[0]);
			caseWorst = std::max(caseWorst, agreement(s, freqs, 0.5));
		}
	}
	SUBCASE("phaser, barberpole: each voice's own loop") {
		ph::Settings s = barberpole(ph::Mode::Phaser, ph::SweepShape::Up, 0.2f, static_cast<float>(ph::kRateLog2Min));
		s.feedback = 0.5f;
		s.travel = travel;
		const std::vector<double> freqs = awayFromNotches(s);
		REQUIRE(freqs.size() == 5);
		for (const double hz : freqs) {
			const SineRun run = sineThrough(s, hz, 0.3, 0.2);
			const ph::ResponseInputs in = ph::responseInputsOf(run.readings.data(), run.readings.size(), 0);
			const ph::ResponseModel m = ph::responseModelOf(s, in, kRate);
			REQUIRE(m.voices == 3);
			REQUIRE(m.feedbacks[0] != m.feedbacks[2]); // the voices' loops really differ
			const double model = static_cast<double>(ph::magnitudeDb(m, hz));
			CAPTURE(hz);
			CAPTURE(model);
			CAPTURE(run.gainDb);
			CHECK(std::fabs(model - run.gainDb) < 0.2);
			caseWorst = std::max(caseWorst, std::fabs(model - run.gainDb));
		}
	}
	MESSAGE("round 2 fields: the model's worst disagreement " << caseWorst << " dB");
}

TEST_CASE("response model: the rate skew is the one round-2 field it leaves out") {
	// ONE INSTANT CANNOT SHOW A RATE: the model takes R's φ as L's + Stereo/360 (`responseModelOf`'s note). So with the
	// skew travelling the engine's R drifts from that, and at home it IS that, to the bit.
	for (const bool skewed : { false, true }) {
		CAPTURE(skewed);
		ph::Settings s = plainPhaser(6, 0.3f);
		s.lfoOn = true;
		s.lfoDepth = 0.7f;
		s.rateLog2Hz = 1.0f;
		s.stereoDegrees = 60.0f;
		if (skewed) {
			s.travel[ps::kRateSkewPhase] = 0.5f;
			REQUIRE(std::fabs(offsetOf(ps::kRateSkewPhase, 0.5f, 0.25)) > 0.05);
		}
		auto dev = deviceWith(s);
		Captured got;
		dev->setReadingsSink({ &got, &Captured::publish });
		runStereo(*dev, 48000);
		const ph::ResponseModel r = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), 1), kRate);
		const double modelR = ph::channelLfoPhase(dev->lfoPhaseForTest(0), static_cast<double>(s.stereoDegrees), 1);
		if (skewed) {
			CHECK(std::fabs(dev->lfoPhaseForTest(1) - modelR) > 0.01);
			CHECK(std::fabs(r.sweep.sweep - dev->sweepForTest(1)) > 1.0e-3);
		}
		else {
			CHECK(std::bit_cast<uint64_t>(dev->lfoPhaseForTest(1)) == std::bit_cast<uint64_t>(modelR));
			CHECK(std::fabs(r.sweep.sweep - dev->sweepForTest(1)) < 1.0e-6);
		}
	}
}
TEST_CASE("response summary: deepest, tilt and contrast of a hand-computed two-stage phaser") {
	// TWO EQUAL STAGES, Mix 0.5, no feedback: H = (1 + M)/2 with M = ap², and each stage turns f through
	// −2·atan(tan(πf/sr)/g) (`stageResponseOfG`). So at tan(πf/sr) = g·tan(α) the cascade turns −4α and
	// |H| = |cos 2α|: α = π/12 gives cos(π/6), π/8 gives cos(π/4), π/4 gives 0 — a null, floored at −60.
	ph::Settings s = plainPhaser(2, 0.0f);
	const ph::ResponseModel m = ph::responseModelOf(s, {}, kRate);
	REQUIRE(m.G[0][0] == m.G[0][1]);
	const double G = static_cast<double>(m.G[0][0]);
	const double g = G / (1.0 - G);
	const std::array<double, 3> alpha { kPi / 12.0, kPi / 8.0, kPi / 4.0 };
	const std::array<double, 3> wantDb { 20.0 * std::log10(std::cos(kPi / 6.0)), 20.0 * std::log10(std::cos(kPi / 4.0)),
		                                 -60.0 };
	std::array<double, 3> hz {};
	for (size_t i = 0; i < 3; ++i) {
		hz[i] = kRate / kPi * std::atan(g * std::tan(alpha[i]));
	}
	// THE STATISTICS WRITTEN OUT for three points: the mean, the slope Σ(x − x̄)(y − ȳ)/Σ(x − x̄)², the spread.
	double xm = 0.0, ym = 0.0;
	for (size_t i = 0; i < 3; ++i) {
		xm += std::log2(hz[i]) / 3.0;
		ym += wantDb[i] / 3.0;
	}
	double sxy = 0.0, sxx = 0.0, syy = 0.0;
	for (size_t i = 0; i < 3; ++i) {
		const double dx = std::log2(hz[i]) - xm;
		const double dy = wantDb[i] - ym;
		sxy += dx * dy;
		sxx += dx * dx;
		syy += dy * dy;
	}
	const ph::ResponseSummary got = ph::responseSummaryOf(m, hz);
	CAPTURE(got.deepestDb);
	CAPTURE(got.tiltDbPerOct);
	CAPTURE(got.contrastDb);
	CHECK(got.deepestDb == -60.0);
	CHECK(std::fabs(got.tiltDbPerOct - sxy / sxx) < 1.0e-4);
	CHECK(std::fabs(got.contrastDb - std::sqrt(syy / 3.0)) < 1.0e-4);
	// The values themselves, so a sign or scale error in the restatement cannot hide: the response falls into the null,
	// a steep downward tilt, and a spread of tens of dB.
	CHECK(got.tiltDbPerOct < -20.0);
	CHECK(got.contrastDb > 20.0);
	// AN EMPTY GRID IS ALL ZERO.
	const ph::ResponseSummary none = ph::responseSummaryOf(m, std::span<const double> {});
	CHECK(none.deepestDb == 0.0);
	CHECK(none.tiltDbPerOct == 0.0);
	CHECK(none.contrastDb == 0.0);
}

TEST_CASE("response model takes its inputs from the readings") {
	// THE MODEL'S CORNERS ARE THE ENGINE'S: every stage's G, built from the published readings, against the engine's own
	// float G (`voiceStageCoefficientForTest`) — so the plot draws the cascade that is running, not a restatement of it.
	ps::Travel travel {};
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		travel[i] = 0.05f + 0.031f * static_cast<float>(i);
	}
	// THE RATE SKEW (index 30) IS LEFT HOME HERE, AND ONLY HERE: the model is one instant and takes R's φ as L's +
	// Stereo/360 (`responseModelOf`'s note), so a skewed R drifts from it by design. "response model: the rate skew is the
	// one round-2 field it leaves out" pins that exclusion; every other index travels.
	travel[ps::kRateSkewPhase] = 0.0f;
	SUBCASE("EXACTLY, where every reading is exact: a tracked A4 (440 Hz is a float), the LFO and follower off") {
		for (const ph::Mode mode : { ph::Mode::Phaser, ph::Mode::Flanger }) {
			ph::Settings s = plainPhaser(12, 0.4f);
			s.mode = mode;
			s.position = 0.2f;
			s.keyTrack = true;
			s.travel = travel;
			s.dampingLog2Hz = static_cast<float>(std::log2(2500.0));
			auto dev = deviceWith(s);
			Captured got;
			dev->setReadingsSink({ &got, &Captured::publish });
			dev->setNoteContext(noteOf(69.0f, true));
			runStereo(*dev, 9600);
			REQUIRE(got.values.size() == ph::kReadingCount);
			REQUIRE(got.values[static_cast<size_t>(ph::Reading::NoteHz)] == 440.0f);
			for (int ch = 0; ch < 2; ++ch) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(ch);
				const ph::ResponseModel m
				    = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), ch), kRate);
				REQUIRE(m.voices == 1);
				CHECK(m.sweep.sweep == dev->sweepForTest(ch));
				CHECK(m.sweep.cornerHz == dev->cornerHzForTest(ch));
				CHECK(m.sweep.delay == dev->delayTargetSamplesForTest(ch));
				for (int i = 0; i < 12; ++i) {
					CAPTURE(i);
					CHECK(m.G[0][static_cast<size_t>(i)] == dev->voiceStageCoefficientForTest(0, ch, i));
				}
				CHECK(m.feedback == static_cast<double>(dev->loopFeedbackForTest(0, ch))); // the tilt makes it per channel
				CHECK(m.dampOpen == dev->dampingOpenForTest());
				CHECK(m.dampHz == dev->dampingHzForTest());
			}
			// NOT VACUOUS: the travel spreads the stages, and the skew separates the channels.
			const ph::ResponseModel l = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), 0), kRate);
			const ph::ResponseModel r = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), 1), kRate);
			CHECK(l.G[0][0] != l.G[0][1]);
			CHECK(l.G[0][0] != r.G[0][0]);
		}
	}
	SUBCASE("to the readings' float precision, with the LFO, the follower and the barberpole running") {
		for (const ph::SweepShape sweep : { ph::SweepShape::Normal, ph::SweepShape::Up, ph::SweepShape::Down }) {
			CAPTURE(static_cast<int>(sweep));
			ph::Settings s = barberpole(ph::Mode::Phaser, sweep, 0.6f, 0.5f);
			s.stages = 12;
			s.envOn = true;
			s.envAmount = -0.3f;
			s.stereoDegrees = 70.0f;
			s.travel = travel;
			auto dev = deviceWith(s);
			Captured got;
			dev->setReadingsSink({ &got, &Captured::publish });
			const std::vector<float> in = noise(24000, 0xfeedu, 0.3f);
			std::vector<float> l = in;
			std::vector<float> r = in;
			for (int n0 = 0; n0 < 24000; n0 += 480) {
				dev->processStereo(l.data() + n0, r.data() + n0, 480);
			}
			REQUIRE(got.values[static_cast<size_t>(ph::Reading::Env)] > 0.0f);
			for (int ch = 0; ch < 2; ++ch) {
				CAPTURE(ch);
				const ph::ResponseModel m
				    = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), ch), kRate);
				REQUIRE(m.voices == (sweep == ph::SweepShape::Normal ? 1 : 3));
				for (int v = 0; v < m.voices; ++v) {
					CHECK(std::fabs(m.sweep.voices[static_cast<size_t>(v)].psi - dev->voicePhaseForTest(v, ch)) < 1.0e-6);
					CHECK(std::fabs(m.sweep.voices[static_cast<size_t>(v)].s - dev->voiceSweepForTest(v, ch)) < 1.0e-5);
					for (int i = 0; i < 12; ++i) {
						CAPTURE(v);
						CAPTURE(i);
						const double want = static_cast<double>(dev->voiceStageCoefficientForTest(v, ch, i));
						CHECK(std::fabs(static_cast<double>(m.G[static_cast<size_t>(v)][static_cast<size_t>(i)]) - want)
						      < 1.0e-5 * want);
					}
				}
			}
		}
	}
	SUBCASE("the flanger barberpole's taps: each tap's read and window, at a tick's end") {
		// THE ENGINE'S TAPS ARE COMPUTED PER SAMPLE (`barberTaps`), from float ramps and a ramp phase re-seeded from ψ each
		// tick; the block ends on a tick's last sample, where they pass through the tick's targets. NOT EXACT: ψ arrives as
		// a float reading and the engine's taps are floats — so 1e-3 samples on a read of ~100-500 samples (a float's ulp
		// at 480 is 3e-5), and 1e-5 on a window. With travel, so each tap carries its own spread and LFO offset.
		for (const bool tz : { false, true }) {
			for (const ph::SweepShape sweep : { ph::SweepShape::Up, ph::SweepShape::Down }) {
				CAPTURE(tz);
				CAPTURE(static_cast<int>(sweep));
				ph::Settings s = barberpole(ph::Mode::Flanger, sweep, 0.6f, 0.5f);
				s.throughZero = tz;
				s.envOn = true;
				s.envAmount = -0.3f;
				s.stereoDegrees = 70.0f;
				s.travel = travel;
				auto dev = deviceWith(s);
				Captured got;
				dev->setReadingsSink({ &got, &Captured::publish });
				const std::vector<float> in = noise(24000, 0xfacedu, 0.3f);
				std::vector<float> l = in;
				std::vector<float> r = in;
				for (int n0 = 0; n0 < 24000; n0 += 480) {
					dev->processStereo(l.data() + n0, r.data() + n0, 480);
				}
				for (int ch = 0; ch < 2; ++ch) {
					CAPTURE(ch);
					const ph::ResponseModel m
					    = ph::responseModelOf(s, ph::responseInputsOf(got.values.data(), got.values.size(), ch), kRate);
					REQUIRE(m.voices == 3);
					for (int k = 0; k < 3; ++k) {
						CAPTURE(k);
						const auto at = static_cast<size_t>(k);
						CAPTURE(m.delaySamples[at]);
						CAPTURE(dev->tapDelaySamplesForTest(k, ch));
						CHECK(std::fabs(m.delaySamples[at] - static_cast<double>(dev->tapDelaySamplesForTest(k, ch))) < 1.0e-3);
						CHECK(std::fabs(m.w[at] - static_cast<double>(dev->tapGainForTest(k, ch))) < 1.0e-5);
					}
					// NOT VACUOUS: the three taps read three different delays.
					CHECK(m.delaySamples[0] != m.delaySamples[1]);
					CHECK(m.delaySamples[1] != m.delaySamples[2]);
				}
			}
		}
	}
}

TEST_CASE("phaser fuzz: every control jumping to its extremes, through the ops table, returns promptly and stays finite") {
	// THE 2026-10-03 LOCK-UP: the whole synth locked up, even after the modulator was disabled, while
	// modulating this device. The rack hands a device its modulated row through `setFields` once a block, so this
	// drives that road: each block a random set of the modulatable controls jumps — to its min, its max, far past
	// either, NaN, or anywhere between — the stepped ones (Mode, Sweep, Through Zero, Stages, Sync, the switches)
	// flip now and then, the pad's travel goes to ±1e30, NaN and infinity, the note and the transport carry nonsense,
	// and the blocks are 1..4096 frames at 8 kHz..384 kHz, stereo and mono. No denormal flush (the plugin flushes;
	// this is the harsher case). A WATCHDOG ends the process if one call is stuck for 2 s — a hang is a failure,
	// not a slow test. What is asserted per sample: a finite input never leaves a non-finite output.
	namespace dev = feathers::device;
	std::atomic<long long> startedNs { 0 };
	std::atomic<bool> finished { false };
	std::atomic<int> where { 0 };
	std::thread dog([&] {
		while (!finished) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			const long long s = startedNs.load();
			if (s != 0 && std::chrono::steady_clock::now().time_since_epoch().count() - s > 2'000'000'000LL) {
				std::fprintf(stderr, "PHASER FUZZ WATCHDOG: one process call stuck for 2 s (seed*100000+block %d)\n",
				             where.load());
				std::_Exit(3);
			}
		}
	});
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	long badSamples = 0;
	long calls = 0;
	for (int seed = 0; seed < 12; ++seed) {
		CAPTURE(seed);
		std::mt19937 rng(static_cast<unsigned>(seed));
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0, 384000.0, 8000.0 };
		const double sr = rates[static_cast<size_t>(seed) % std::size(rates)];
		alignas(ph::Phaser) static unsigned char storage[sizeof(ph::Phaser)];
		ph::kAudioOps.construct(storage);
		ph::kAudioOps.prepare(storage, sr, 4096);
		std::vector<float> wide(ph::kTravelFirst + ph::phase_space::kPhaseCount, 0.0f);
		const auto pick = [&](size_t i) -> float {
			const auto& d = ph::kParamDescs[i];
			const auto lo = static_cast<float>(d.min);
			const auto hi = static_cast<float>(d.max);
			switch (rng() % 10) {
			case 0: return lo;
			case 1: return hi;
			case 2: return static_cast<float>(d.def);
			case 3: return lo - (hi - lo) * 10.0f;
			case 4: return hi + (hi - lo) * 10.0f;
			case 5: return rng() % 20 == 0 ? nan : lo + (hi - lo) * u(rng);
			default: return lo + (hi - lo) * u(rng);
			}
		};
		const auto pickTravel = [&]() -> float {
			switch (rng() % 8) {
			case 0: return 0.0f;
			case 1: return 1e6f * (u(rng) - 0.5f);
			case 2: return rng() % 2 ? 1e30f : -1e30f;
			case 3: return rng() % 4 == 0 ? inf : nan;
			default: return 4.0f * (u(rng) - 0.5f);
			}
		};
		for (size_t i = 0; i < ph::kParamCount; ++i) {
			wide[i] = pick(i);
		}
		for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
			wide[ph::kTravelFirst + i] = pickTravel();
		}
		std::vector<float> L(4096), R(4096), inL(4096), inR(4096);
		double phase = 0.0;
		for (int b = 0; b < 1500; ++b) {
			const bool all = rng() % 20 == 0;
			for (size_t i = 0; i < ph::kParamCount; ++i) {
				const bool modulatable = ph::kParamDescs[i].modulatable;
				if ((modulatable && (all || rng() % 4 == 0)) || (!modulatable && rng() % 200 == 0)) {
					wide[i] = pick(i);
				}
			}
			if (rng() % 50 == 0) {
				for (size_t i = 0; i < ph::phase_space::kPhaseCount; ++i) {
					wide[ph::kTravelFirst + i] = pickTravel();
				}
			}
			ph::kAudioOps.setFields(storage, wide.data(), rng() % 100 == 0 ? rng() % wide.size() : wide.size());
			if (rng() % 30 == 0) {
				dev::NoteContext n;
				n.gate = rng() % 2 == 0;
				const float pitches[] = { 60.0f, -200.0f, 300.0f, 0.0f, 127.0f, 1e6f, -1e6f, nan, inf };
				n.pitchSemitones = rng() % 3 ? 127.0f * u(rng) : pitches[rng() % std::size(pitches)];
				ph::kAudioOps.setNoteContext(storage, n);
			}
			if (rng() % 40 == 0) {
				dev::TransportClock c;
				const double spb[] = { 0.5, 0.0, -1.0, 1e-300, 1e300, 0.001, 100.0 };
				const double pos[] = { 0.0, 10.0, -5.0, 1e18, std::numeric_limits<double>::quiet_NaN(),
					                   std::numeric_limits<double>::infinity() };
				c.secondsPerBeat = spb[rng() % std::size(spb)];
				c.playing = rng() % 2 == 0;
				c.songPositionSeconds = pos[rng() % std::size(pos)];
				c.songPositionBeats = pos[rng() % std::size(pos)];
				ph::kAudioOps.setTransportClock(storage, c);
			}
			if (rng() % 500 == 0) {
				ph::kAudioOps.reset(storage);
			}
			const int frames = 1 + static_cast<int>(rng() % (rng() % 8 == 0 ? 4096u : 512u));
			const int signal = static_cast<int>(rng() % 8);
			for (int j = 0; j < frames; ++j) {
				phase += 0.01 + (signal == 1 ? 0.2 * u(rng) : 0.0);
				float x = 0.0f;
				switch (signal) {
				case 0: x = 0.0f; break;
				case 1:
				case 2: x = static_cast<float>(std::sin(phase)); break;
				case 3: x = 100.0f * (u(rng) - 0.5f); break;
				case 4: x = rng() % 100 == 0 ? nan : u(rng) - 0.5f; break;
				case 5: x = rng() % 100 == 0 ? inf : u(rng) - 0.5f; break;
				default: x = 2.0f * (u(rng) - 0.5f); break;
				}
				L[static_cast<size_t>(j)] = inL[static_cast<size_t>(j)] = x;
				R[static_cast<size_t>(j)] = inR[static_cast<size_t>(j)] = signal == 7 ? -x : 0.7f * x;
			}
			const bool mono = rng() % 10 == 0;
			where = seed * 100000 + b;
			startedNs = std::chrono::steady_clock::now().time_since_epoch().count();
			if (mono) {
				ph::kAudioOps.processMono(storage, L.data(), frames);
			}
			else {
				ph::kAudioOps.processStereo(storage, L.data(), R.data(), frames);
			}
			startedNs = 0;
			++calls;
			for (size_t j = 0; j < static_cast<size_t>(frames); ++j) {
				if ((std::isfinite(inL[j]) && !std::isfinite(L[j]))
				    || (!mono && std::isfinite(inR[j]) && !std::isfinite(R[j]))) {
					++badSamples;
				}
			}
		}
		ph::kAudioOps.destruct(storage);
	}
	finished = true;
	dog.join();
	CHECK(calls == 12 * 1500);
	CHECK(badSamples == 0);
}
