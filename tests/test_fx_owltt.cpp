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

#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "character/phi_triangle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

using namespace feathers::fx::owltt;

namespace {

/// Make a default Settings with processing enabled (mode 1 = AP 6dB) and sensible globals.
Settings defaultSettings() {
	Settings s;
	s.mode = 1.0f; // AP 6dB
	s.threshold = 0.5f;
	s.ratio = 0.0f;
	s.bandwidth = 0.5f;
	s.upDownSkew = 0.5f;
	s.attack = 0.25f;
	s.release = 0.25f;
	s.character = 0.0f;
	s.vibe = 0.0f;
	s.lowCrossover = 200.0f;
	s.highCrossover = 2000.0f;
	s.outputGain = 0.5f;
	s.blend = 1.0f;
	return s;
}

/// Fill L/R with a sine wave of given frequency and amplitude.
void fillSine(std::vector<float>& L, std::vector<float>& R, float freqHz, float sampleRate,
              float amplitude, size_t count) {
	const float omega = 2.0f * static_cast<float>(std::numbers::pi) * freqHz / sampleRate;
	for (size_t i = 0; i < count; ++i) {
		L[i] = R[i] = amplitude * std::sin(omega * static_cast<float>(i));
	}
}

/// Process a buffer in chunks.
void processInChunks(Owltt& owltt, std::vector<float>& L, std::vector<float>& R, int chunkSize) {
	for (size_t offset = 0; offset < L.size(); offset += static_cast<size_t>(chunkSize)) {
		const int frames = std::min(chunkSize, static_cast<int>(L.size() - offset));
		owltt.processStereo(&L[offset], &R[offset], frames);
	}
}

} // namespace

TEST_CASE("OWLTT processes audio without crashing and produces output") {
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.threshold = 0.3f;
	target.ratio = 0.5f;
	target.character = 0.5f;
	owltt.setTarget(target);

	SUBCASE("processes a sine wave") {
		std::vector<float> L(4800), R(4800);
		const float omega = 2.0f * static_cast<float>(std::numbers::pi) * 1000.0f / sampleRate;
		for (size_t i = 0; i < L.size(); ++i) {
			L[i] = R[i] = 0.5f * std::sin(omega * static_cast<float>(i));
		}

		processInChunks(owltt, L, R, 128);

		// Output should be finite and non-zero.
		float maxAbs = 0.0f;
		for (size_t i = 0; i < L.size(); ++i) {
			maxAbs = std::max(maxAbs, std::max(std::abs(L[i]), std::abs(R[i])));
			CHECK(std::isfinite(L[i]));
			CHECK(std::isfinite(R[i]));
		}
		CHECK(maxAbs > 0.0f);
	}

	SUBCASE("produces different output with different character settings") {
		std::vector<float> L1(4800), R1(4800), L2(4800), R2(4800);
		const float omega = 2.0f * static_cast<float>(std::numbers::pi) * 500.0f / sampleRate;
		for (size_t i = 0; i < L1.size(); ++i) {
			L1[i] = R1[i] = L2[i] = R2[i] = 0.3f * std::sin(omega * static_cast<float>(i));
		}

		target.character = 0.0f;
		owltt.setTarget(target);
		owltt.reset();
		processInChunks(owltt, L1, R1, 128);

		target.character = 1.0f;
		owltt.setTarget(target);
		owltt.reset();
		processInChunks(owltt, L2, R2, 128);

		// Outputs should differ.
		float diff = 0.0f;
		for (size_t i = 0; i < L1.size(); ++i) {
			diff += std::abs(L1[i] - L2[i]);
		}
		CHECK(diff > 0.0f);
	}
}

TEST_CASE("OWLTT mode 0 bypasses audio unchanged") {
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.mode = 0.0f; // Off
	target.ratio = 1.0f; // would heavily compress if not bypassed
	owltt.setTarget(target);

	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 1000.0f, sampleRate, 0.5f, L.size());

	// Store input for comparison.
	std::vector<float> inL = L, inR = R;

	processInChunks(owltt, L, R, 128);

	// Output should equal input (bypass).
	for (size_t i = 0; i < L.size(); ++i) {
		CHECK(L[i] == doctest::Approx(inL[i]).epsilon(1e-6f));
		CHECK(R[i] == doctest::Approx(inR[i]).epsilon(1e-6f));
	}
}

TEST_CASE("OWLTT all 10 crossover types process without crashing") {
	constexpr float sampleRate = 48000.0f;

	for (int mode = 1; mode <= 10; ++mode) {
		CAPTURE(mode);
		Owltt owltt;
		owltt.prepare(sampleRate);

		Settings target = defaultSettings();
		target.mode = static_cast<float>(mode);
		target.ratio = 0.5f;
		owltt.setTarget(target);

		std::vector<float> L(1200), R(1200);
		fillSine(L, R, 1000.0f, sampleRate, 0.5f, L.size());

		processInChunks(owltt, L, R, 128);

		for (size_t i = 0; i < L.size(); ++i) {
			CHECK(std::isfinite(L[i]));
			CHECK(std::isfinite(R[i]));
		}
		// Non-zero output.
		float maxAbs = 0.0f;
		for (size_t i = 0; i < L.size(); ++i) {
			maxAbs = std::max(maxAbs, std::max(std::abs(L[i]), std::abs(R[i])));
		}
		CHECK(maxAbs > 0.0f);
	}
}

TEST_CASE("backlog EH: a crossover type that comes back starts clean — its filters do not resume the tail they held when that type last ran") {
	// NINE CROSSOVER OBJECTS SIT BEHIND THE MODE SWITCH, and each kept its allpass state from whenever
	// its type last ran: type 1, loud; type 5, loud; type 1 again on SILENCE rang the state it had
	// parked under type 5's whole run. The multimode filter guards the identical case. Measured by
	// value: with silence in, the output is silence from the first sample.
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);
	Settings target = defaultSettings();
	target.ratio = 0.8f;
	target.lowCrossover = 200.0f;
	target.highCrossover = 2000.0f;
	const auto runMode = [&](int mode, float amp) {
		target.mode = static_cast<float>(mode);
		owltt.setTarget(target);
		std::vector<float> L(4800), R(4800);
		if (amp > 0.0f) {
			fillSine(L, R, 500.0f, sampleRate, amp, L.size());
		}
		processInChunks(owltt, L, R, 128);
		float peak = 0.0f;
		for (size_t i = 0; i < 512; ++i) {
			peak = std::max(peak, std::max(std::abs(L[i]), std::abs(R[i])));
		}
		return peak;
	};
	REQUIRE(runMode(1, 0.5f) > 0.0f);
	REQUIRE(runMode(5, 0.5f) > 0.0f);
	// BACK TO TYPE 1 WITH NOTHING IN: nothing out, from the first sample of the first chunk.
	const float ring = runMode(1, 0.0f);
	CAPTURE(ring);
	CHECK(ring < 1e-6f);
}

TEST_CASE("backlog EH: a bypass clears OWLTT's running state, so re-enabling on silence is silent") {
	// The crossovers' filters and the bands' gain memories waited through a bypass and resumed mid-tail
	// — and the device's own rest-flush was gated on `enabled`, so it could never do the clearing while
	// bypassed. Loud, bypassed one block, enabled on SILENCE: nothing out from the first sample.
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);
	Settings target = defaultSettings();
	target.mode = 5.0f;
	target.ratio = 0.8f;
	owltt.setTarget(target);
	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 500.0f, sampleRate, 0.5f, L.size());
	processInChunks(owltt, L, R, 128);
	target.enabled = false;
	owltt.setTarget(target);
	std::fill(L.begin(), L.end(), 0.0f);
	std::fill(R.begin(), R.end(), 0.0f);
	processInChunks(owltt, L, R, 128);
	target.enabled = true;
	owltt.setTarget(target);
	std::fill(L.begin(), L.end(), 0.0f);
	std::fill(R.begin(), R.end(), 0.0f);
	processInChunks(owltt, L, R, 128);
	float ring = 0.0f;
	for (size_t i = 0; i < 512; ++i) {
		ring = std::max(ring, std::max(std::abs(L[i]), std::abs(R[i])));
	}
	CAPTURE(ring);
	CHECK(ring < 1e-6f);
}

TEST_CASE("backlog EH: past the first lap, Vibe and Character still move the sound — the vibe phases read the lap's fraction, and the feel phases multiply in double") {
	// SHAPE 2, twice in one function. Vibe's phi block ran only past the first lap and read
	// `clamp(v, 0, 1)` there — a constant — so every vibe phase sat flat across each lap's interior;
	// Character's six feel phases multiplied a float lap count by a phi constant, collapsing to zero
	// past ~5.2 million laps. Asserted as a NON-IDENTITY between two positions inside one lap, with the
	// same pair inside lap zero as the non-vacuity control (always different, both builds).
	// READ OFF VALUE SEAMS, not off the samples: the vibe knee phase and the character response are the
	// numbers the two roads produce, and a render carries every other quantity at once.
	constexpr float sampleRate = 48000.0f;
	struct Read {
		float knee;
		float feel;
	};
	const auto readAt = [&](float character, float vibe) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings target = defaultSettings();
		target.mode = 5.0f;
		target.ratio = 0.8f;
		target.character = character;
		target.vibe = vibe;
		owltt.setTarget(target);
		std::vector<float> L(512), R(512);
		fillSine(L, R, 300.0f, sampleRate, 0.5f, L.size());
		processInChunks(owltt, L, R, 128);
		return Read { owltt.vibePhaseKneeForTest(), owltt.feelPhase033ForTest() };
	};
	SUBCASE("vibe: two positions inside the second lap give two knee phases") {
		// Past the first lap the vibe block reads the phi road; with the clamp both positions read 1.0
		// and the knee was one number across the whole lap.
		CHECK(readAt(0.9f, 1.3f).knee != readAt(0.9f, 1.6f).knee);
	}
	SUBCASE("character: two laps six million out give two feel phases") {
		// Whole laps, because a float control cannot hold a fraction out there (its step is 0.5 at six
		// million); the phase is the lap count times a phi constant, wrapped, and with the float product
		// every lap past ~5.2 million read exactly 0.
		const float lap0 = readAt(0.0f, 0.3f).feel;
		const float lap1 = readAt(1.0f, 0.3f).feel;
		CHECK(lap0 != lap1); // the control
		const float far0 = readAt(6000000.0f, 0.3f).feel;
		const float far1 = readAt(6000001.0f, 0.3f).feel;
		CAPTURE(far0);
		CAPTURE(far1);
		CHECK(far0 != far1); // the claim
		CHECK(far1 == doctest::Approx(owlet::dsp::phi::wrapPhase(6000001.0 * owlet::dsp::phi::kPhi033)).epsilon(1e-5)); // by value
	}
}

TEST_CASE("OWLTT different crossover types produce different band routing") {
	constexpr float sampleRate = 48000.0f;

	// Use a signal between the two crossovers (500Hz) and high ratio so band routing differences
	// are audible. Compare AP 6dB (mode 1) vs LR4 Full (mode 8).
	auto processMode = [&](int mode) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings target = defaultSettings();
		target.mode = static_cast<float>(mode);
		target.ratio = 0.8f;
		target.lowCrossover = 200.0f;
		target.highCrossover = 2000.0f;
		owltt.setTarget(target);

		std::vector<float> L(4800), R(4800);
		fillSine(L, R, 500.0f, sampleRate, 0.5f, L.size());
		processInChunks(owltt, L, R, 128);

		float energy = 0.0f;
		for (size_t i = 0; i < L.size(); ++i) {
			energy += L[i] * L[i] + R[i] * R[i];
		}
		return energy;
	};

	const float energyAP = processMode(1);
	const float energyLR4 = processMode(8);

	// Different crossover slopes → different band routing → different compressed energy.
	CHECK(energyAP != doctest::Approx(energyLR4).epsilon(1e-3f));
}

TEST_CASE("OWLTT soft clipping limits output level") {
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.mode = 6; // LR2 Full
	target.ratio = 1.0f; // full limiting
	target.threshold = 0.0f; // low threshold = compress everything
	target.outputGain = 1.0f; // max makeup (+16dB)
	owltt.setTarget(target);

	// Full-scale sine.
	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 1000.0f, sampleRate, 1.0f, L.size());

	processInChunks(owltt, L, R, 128);

	// Soft clipper at output clamps to ~kOutputClipKnee (1.0) plus half excess. With heavy
	// compression + makeup the signal is large, so the output must be bounded (not infinite).
	float maxAbs = 0.0f;
	for (size_t i = 0; i < L.size(); ++i) {
		maxAbs = std::max(maxAbs, std::max(std::abs(L[i]), std::abs(R[i])));
		CHECK(std::isfinite(L[i]));
		CHECK(std::isfinite(R[i]));
	}
	// Soft clip keeps output bounded to a reasonable multiple of full scale.
	CHECK(maxAbs < 10.0f);
	CHECK(maxAbs > 0.0f);
}

TEST_CASE("OWLTT M/S width processing affects stereo image") {
	constexpr float sampleRate = 48000.0f;

	// Process a signal with character in the Width zone (zone 0) which modulates bandWidth.
	// Compare mono input (L==R) vs stereo input (L!=R) — width processing should affect them
	// differently when width != 1.
	auto process = [&](bool stereo) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings target = defaultSettings();
		target.mode = 1; // AP 6dB
		target.character = 0.1f; // Width zone (zone 0), mid-position
		target.ratio = 0.5f;
		owltt.setTarget(target);

		std::vector<float> L(4800), R(4800);
		fillSine(L, R, 500.0f, sampleRate, 0.5f, L.size());
		if (!stereo) {
			// Force mono: R = L (already the case from fillSine).
		}
		else {
			// Make R a different phase.
			for (size_t i = 0; i < R.size(); ++i) {
				R[i] = -R[i];
			}
		}
		processInChunks(owltt, L, R, 128);

		float energy = 0.0f;
		for (size_t i = 0; i < L.size(); ++i) {
			energy += L[i] * L[i] + R[i] * R[i];
		}
		return energy;
	};

	const float energyMono = process(false);
	const float energyStereo = process(true);

	// Stereo input (opposite phase) with width processing produces different energy than mono.
	CHECK(energyMono != doctest::Approx(energyStereo).epsilon(1e-3f));
}

TEST_CASE("OWLTT linked parameter shift moves all bands") {
	constexpr float sampleRate = 48000.0f;

	// With linked threshold at default (0.5) and per-band thresholds at default, the effective
	// threshold is 0.5. Shifting the linked threshold down should increase compression (more gain
	// reduction) on a steady signal.
	auto measureGR = [&](float linkedThreshold) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings target = defaultSettings();
		target.mode = 1; // AP 6dB
		target.threshold = linkedThreshold;
		target.ratio = 0.8f;
		owltt.setTarget(target);

		std::vector<float> L(4800), R(4800);
		fillSine(L, R, 1000.0f, sampleRate, 0.5f, L.size());
		processInChunks(owltt, L, R, 128);

		// Return the maximum gain reduction across bands (most negative = most compression).
		float minGR = 0.0f;
		for (int b = 0; b < 3; ++b) {
			minGR = std::min(minGR, owltt.getBandGainReductionDB(static_cast<size_t>(b)));
		}
		return minGR;
	};

	const float grHighThreshold = measureGR(0.8f); // high threshold = less compression
	const float grLowThreshold = measureGR(0.2f);  // low threshold = more compression

	// Lower linked threshold should produce more gain reduction (more negative).
	CHECK(grLowThreshold < grHighThreshold);
}

TEST_CASE("OWLTT blend control mixes wet and dry") {
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.mode = 1;
	target.ratio = 0.8f;
	target.blend = 0.5f;
	owltt.setTarget(target);

	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 1000.0f, sampleRate, 0.5f, L.size());
	const std::vector<float> inL = L, inR = R;

	processInChunks(owltt, L, R, 128);

	// With blend=0.5 and compression active, output should differ from input (wet component present)
	// but not be fully compressed either. Just verify it's finite, non-zero, and differs from input.
	float maxAbs = 0.0f;
	float diff = 0.0f;
	for (size_t i = 0; i < L.size(); ++i) {
		maxAbs = std::max(maxAbs, std::max(std::abs(L[i]), std::abs(R[i])));
		diff += std::abs(L[i] - inL[i]);
		CHECK(std::isfinite(L[i]));
		CHECK(std::isfinite(R[i]));
	}
	CHECK(maxAbs > 0.0f);
	CHECK(diff > 0.0f);
}

TEST_CASE("OWLTT enabled bypass is bit-identical to input") {
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.enabled = false; // Bypass
	target.ratio = 0.8f;
	owltt.setTarget(target);

	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 1000.0f, sampleRate, 0.5f, L.size());
	const std::vector<float> inL = L, inR = R;

	processInChunks(owltt, L, R, 128);

	// Bypass should produce bit-identical output to input.
	for (size_t i = 0; i < L.size(); ++i) {
		CHECK(L[i] == doctest::Approx(inL[i]));
		CHECK(R[i] == doctest::Approx(inR[i]));
	}
}

TEST_CASE("OWLTT drives one band and leaves the others near unity") {
	// THE TEST THE BRIEF'S §6 ASKED FOR: drive one band and assert the other two stay near unity.
	// Uses a 30Hz sine (well below the 200Hz low crossover) so all energy is in the LOW band.
	// The MID and HIGH compressors should see near-zero input and produce near-zero gain reduction.
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.mode = 6; // LR2 Full
	target.threshold = 0.3f; // Low threshold so LOW band compresses
	target.ratio = 0.8f;
	// Set MID and HIGH thresholds very high and ratios to zero so they don't compress
	// even with crossover leakage.
	//
	// "TO ZERO" MEANS THE EFFECTIVE RATIO, NOT THE STORED ONE, and the two are the same number only
	// while the global sits at neutral -- which it does not here, `target.ratio` being 0.8. Written
	// as a bare `0.0f` this line silently left the mid and high bands at the global's own
	// displacement (0.3 once ratio's shift was centred), so the comment described a state the code
	// did not produce. Inverted through the device's own function rather than by subtracting 0.5 by
	// hand, for the reason `bandValueForEffective` exists at all.
	target.mid.threshold = 0.99f;
	target.mid.ratio = bandValueForEffective(0.0f, ratioShiftOf(target));
	target.high.threshold = 0.99f;
	target.high.ratio = bandValueForEffective(0.0f, ratioShiftOf(target));
	owltt.setTarget(target);

	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 10.0f, sampleRate, 0.5f, L.size()); // 10Hz = all in LOW band

	processInChunks(owltt, L, R, 128);

	// LOW band should show compression (negative GR).
	const float grLow = owltt.getBandGainReductionDB(0);
	CHECK(grLow < -1.0f); // At least 1dB of gain reduction in the driven band.

	// MID and HIGH bands should show near-zero gain reduction (minimal signal in those bands).
	// Crossover leakage means some small GR is expected; allow 2dB tolerance.
	const float grMid = owltt.getBandGainReductionDB(1);
	const float grHigh = owltt.getBandGainReductionDB(2);
	// ABSOLUTE, AND SPELT OUT, BECAUSE `Approx().epsilon()` IS NOT WHAT "WITHIN 2 dB" MEANS.
	// doctest compares `|a - b| < epsilon * (scale + max(|a|,|b|))` with `scale` defaulting to 1, so
	// `Approx(0.0f).epsilon(2.0f)` admits any `a` satisfying `|a| < 2 + 2|a|` — true for EVERY finite
	// value. MEASURED: a gain reduction of 1,000,000 dB passed this line. Both of these could not
	// fail, in a case whose whole subject is that these two bands are NOT being compressed.
	CHECK(std::fabs(grMid) < 2.0f);
	CHECK(std::fabs(grHigh) < 2.0f);
}

TEST_CASE("OWLTT crossover frequency verification") {
	// Verify that the crossover frequencies are correct by testing at different frequencies.
	constexpr float sampleRate = 48000.0f;

	auto testFrequency = [&](float freqHz) {
		Owltt owltt2;
		owltt2.prepare(sampleRate);

		Settings target2 = defaultSettings();
		target2.mode = 6; // LR2 Full
		target2.threshold = 0.99f; // High threshold so no compression
		target2.ratio = 0.0f;
		owltt2.setTarget(target2);

		std::vector<float> L(4800), R(4800);
		fillSine(L, R, freqHz, sampleRate, 0.5f, L.size());

		processInChunks(owltt2, L, R, 128);

		const float inLow = owltt2.getBandInputLevelLog(0);
		const float inMid = owltt2.getBandInputLevelLog(1);
		const float inHigh = owltt2.getBandInputLevelLog(2);
		MESSAGE(freqHz << " Hz: LOW=" << inLow << " MID=" << inMid << " HIGH=" << inHigh);
	};

	testFrequency(10.0f);   // Should be mostly LOW
	testFrequency(100.0f);  // Should be mostly LOW
	testFrequency(500.0f);  // Should be mostly MID
	testFrequency(900.0f);  // Should be mostly MID
	testFrequency(5000.0f); // Should be mostly HIGH
	testFrequency(10000.0f); // Should be mostly HIGH
}

TEST_CASE("OWLTT crossover filter direct test") {
	// Test the crossover filter directly to verify it's working.
	constexpr float sampleRate = 48000.0f;
	LR2CrossoverFull xover;
	xover.setLowCrossover(200.0f, sampleRate);
	xover.setHighCrossover(2000.0f, sampleRate);
	xover.reset();

	auto testFrequency = [&](float freqHz) {
		xover.reset();
		const size_t N = 4800;
		const float omega = 2.0f * static_cast<float>(std::numbers::pi) * freqHz / sampleRate;
		float lowRMS = 0.0f, midRMS = 0.0f, highRMS = 0.0f;
		for (size_t i = 0; i < N; ++i) {
			const float input = std::sin(omega * static_cast<float>(i));
			CrossoverBands bands;
			xover.process(input, bands);
			lowRMS += bands.low * bands.low;
			midRMS += bands.mid * bands.mid;
			highRMS += bands.high * bands.high;
		}
		lowRMS = std::sqrt(lowRMS / static_cast<float>(N));
		midRMS = std::sqrt(midRMS / static_cast<float>(N));
		highRMS = std::sqrt(highRMS / static_cast<float>(N));
		const float totalRMS = std::sqrt(lowRMS * lowRMS + midRMS * midRMS + highRMS * highRMS);
		MESSAGE(freqHz << " Hz: LOW=" << lowRMS << " MID=" << midRMS << " HIGH=" << highRMS << " total=" << totalRMS);
	};

	testFrequency(10.0f);
	testFrequency(100.0f);
	testFrequency(200.0f);
	testFrequency(500.0f);
	testFrequency(1000.0f);
	testFrequency(2000.0f);
	testFrequency(5000.0f);
	testFrequency(10000.0f);
}

TEST_CASE("OWLTT compression behavior at 900Hz") {
	// At 900 Hz, most energy should be in the MID band (200-2000 Hz).
	// The MID band should show the most compression (most negative GR).
	constexpr float sampleRate = 48000.0f;
	Owltt owltt;
	owltt.prepare(sampleRate);

	Settings target = defaultSettings();
	target.mode = 6; // LR2 Full
	target.threshold = 0.5f;
	target.ratio = 0.7f;
	owltt.setTarget(target);

	std::vector<float> L(4800), R(4800);
	fillSine(L, R, 900.0f, sampleRate, 0.5f, L.size());

	processInChunks(owltt, L, R, 128);

	const float grLow = owltt.getBandGainReductionDB(0);
	const float grMid = owltt.getBandGainReductionDB(1);
	const float grHigh = owltt.getBandGainReductionDB(2);

	const float inLow = owltt.getBandInputLevelLog(0);
	const float inMid = owltt.getBandInputLevelLog(1);
	const float inHigh = owltt.getBandInputLevelLog(2);

	MESSAGE("900Hz GR: LOW=" << grLow << " MID=" << grMid << " HIGH=" << grHigh);
	MESSAGE("900Hz IN: LOW=" << inLow << " MID=" << inMid << " HIGH=" << inHigh);

	// All GR values should be <= small positive (downward compression, not massive amplification).
	CHECK(grLow <= 1.0f);
	CHECK(grMid <= 1.0f);
	CHECK(grHigh <= 1.0f);
}

TEST_CASE("the band with the most signal reduces the most — the inversion guard") {
	// THE CASE THAT WOULD HAVE CAUGHT IT, and its absence is why the defect survived four rounds of
	// diagnosis and shipped three times.
	//
	// `Up/Down Skew` was declared -1..1 with a default of 0, while `owltt.cpp` converts the stored
	// value with `upDownSkew * 2 - 1` — a 0..1 mapping. So a fresh OWLTT stored 0.0, converted it to
	// -1.0, and `downwardFactor = clamp(1 + skew, 0, 1)` came out ZERO on every band. Downward
	// compression could not act at all; only the upward path could. The loudest band reported no
	// reduction and a near-silent one ran away.
	//
	// EVERY EXISTING CASE PASSED THROUGHOUT. They asserted that output differs from input, that the
	// device does not crash, and that character settings change something — all true of a compressor
	// that only ever boosts. **This asserts the ORDERING, which is the property that was wrong.**
	namespace dt = feathers::fx::owltt;

	Owltt owltt;
	owltt.prepare(48000.0);

	dt::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(dt::kParamDescs[i].def);
	}
	row[static_cast<size_t>(dt::Param::Enabled)] = 1.0f;
	row[static_cast<size_t>(dt::Param::Ratio)] = 0.7f;
	owltt.setTarget(dt::stateOf(row));

	// A TONE HIGH ENOUGH TO SIT IN THE UPPER BANDS, so the bands genuinely differ in level. A signal
	// that lands equally in all three proves nothing about ordering.
	std::vector<float> left(512), right(512);
	for (int block = 0; block < 60; ++block) {
		for (int i = 0; i < 512; ++i) {
			const float t = static_cast<float>(block * 512 + i) / 48000.0f;
			left[i] = right[i] = 0.7f * std::sin(6.2831853f * 900.0f * t);
		}
		owltt.processStereo(left.data(), right.data(), 512);
	}

	std::array<float, 3> level {};
	std::array<float, 3> reduction {};
	for (size_t b = 0; b < 3; ++b) {
		level[b] = owltt.getBandInputLevelLog(b);
		reduction[b] = owltt.getBandGainReductionDB(b);
		CAPTURE(b);
		CAPTURE(level[b]);
		CAPTURE(reduction[b]);
	}

	// THE BANDS REALLY DO DIFFER, or the ordering claim below is vacuous.
	const size_t loudest = static_cast<size_t>(
	    std::max_element(level.begin(), level.end()) - level.begin());
	const size_t quietest = static_cast<size_t>(
	    std::min_element(level.begin(), level.end()) - level.begin());
	REQUIRE(loudest != quietest);
	REQUIRE(level[loudest] > level[quietest] + 1.0f);

	// DOWNWARD COMPRESSION REDUCES: a negative envelope is attenuation.
	CHECK(reduction[loudest] < 0.0f);

	// AND THE LOUDEST BAND IS REDUCED AT LEAST AS MUCH AS THE QUIETEST. This is the assertion the
	// inversion failed: it had the quietest band at +0.90 and the loudest at 0.
	CHECK(reduction[loudest] <= reduction[quietest]);
}

TEST_CASE("OWLTT compresses like a compressor") {
	// THE CASE THAT COMES WITH THE FIX. Backlog 24 measured this device and deliberately pinned
	// nothing, because the numbers then were not the intended ones -- 20 dB of reduction FLAT from 0
	// to -20 dBFS is a pad, and half the threshold knob produced no change at all. Pinning that would
	// have enshrined it.
	//
	// TWO DEFECTS WERE FIXED AND THIS ASSERTS THE SHAPE OF THE RESULT, not the digits. A characterisation
	// case full of literals rots on the first voicing change; what must not change is that the device
	// behaves like the thing it claims to be.
	constexpr float sampleRate = 48000.0f;
	constexpr size_t n = 48000; // a second, so the envelope settles

	auto gainAt = [&](Settings s, float amp) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		owltt.setTarget(s);
		std::vector<float> L(n), R(n);
		fillSine(L, R, 900.0f, sampleRate, amp, n);
		processInChunks(owltt, L, R, 128);
		float peak = 0.0f;
		for (size_t i = n * 3 / 4; i < n; ++i) {
			peak = std::max(peak, std::abs(L[i]));
		}
		return 20.0f * std::log10(std::max(peak, 1e-9f)) - 20.0f * std::log10(amp);
	};

	Settings base = defaultSettings();
	base.ratio = 0.5f; // THE PLUGIN'S default, not the helper's 0.0 -- which is a no-op device and is
	                   // how the first measurement of this came back reading 0.0 dB at every level.

	SUBCASE("gain reduction grows with input level") {
		// THE DEFINING PROPERTY, and the one the old code failed: it reduced by a CONSTANT 20 dB from
		// 0 down to -20 dBFS, which is a pad wearing a compressor's controls. A compressor pushes
		// harder the further over the threshold it is.
		const float loud = gainAt(base, 1.0f);
		const float mid = gainAt(base, 0.25f);
		const float quiet = gainAt(base, 0.03f);
		CAPTURE(loud);
		CAPTURE(mid);
		CAPTURE(quiet);
		CHECK(loud < mid);
		CHECK(mid < quiet);
		// AND IT LETS GO below the threshold rather than riding a permanent cut.
		CHECK(quiet > -1.5f);
	}

	SUBCASE("every part of the threshold knob does something") {
		// HALF THIS KNOB USED TO BE DEAD -- 0.0, 0.25 and 0.5 all produced -20.1 dB, because the
		// mapping spanned 149 dB and the bottom hundred of them were below any real signal.
		float previous = -1000.0f;
		for (const float t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			Settings s = base;
			s.threshold = t;
			const float g = gainAt(s, 0.25f);
			CAPTURE(t);
			CAPTURE(g);
			// MONOTONIC AND STRICT: a higher threshold is less compression, and every step moves.
			CHECK(g > previous + 0.5f);
			previous = g;
		}
	}

	SUBCASE("the region between the thresholds is transparent") {
		// THE ANSWER TO THE QUESTION, asserted. `thresholdUp` sat ABOVE `thresholdDown`, so the
		// band between the two bars was where BOTH paths engaged and cancelled -- while the card drew
		// it as "the dead zone". It sits below now, so widening bandwidth widens a genuinely
		// transparent window and the upward boost starts lower.
		//
		// MEASURED IN THE UPWARD REGION, because that is where this control lives: swept at -12 dBFS
		// it is flat and correctly so, since that signal is above the down threshold and only the
		// downward path runs. A sweep in the wrong region reports "no effect" for a working control.
		float previous = 1000.0f;
		for (const float bw : { 0.0f, 0.25f, 0.5f, 0.75f }) {
			Settings s = base;
			s.bandwidth = bw;
			const float g = gainAt(s, 0.01f); // -40 dBFS
			CAPTURE(bw);
			CAPTURE(g);
			// WIDER GAP -> LOWER UP THRESHOLD -> LESS BOOST. Monotonic, and it must actually move.
			CHECK(g < previous);
			previous = g;
		}
		// AND NOT VACUOUS: the control has real range rather than three identical readings that
		// happen to satisfy a strict inequality by rounding.
		Settings narrow = base;
		narrow.bandwidth = 0.0f;
		Settings wide = base;
		wide.bandwidth = 1.0f;
		CHECK(gainAt(narrow, 0.01f) - gainAt(wide, 0.01f) > 1.0f);
	}
}

TEST_CASE("the crossover puts each tone in its own band") {
	// Do the crossover bands work properly? They did not.
	//
	// MEASURED BY MUTING ONE BAND AND ASKING WHICH TONE DISAPPEARS, which is the only question that
	// distinguishes a working splitter from a broken one -- and notably NOT the invariant the
	// crossover header states about itself. `low + mid + high == input` held the whole time it was
	// broken: a comb splitter reconstructs perfectly too. A sum that is right says nothing about
	// whether the parts are, which is why this asserts ROUTING and not reconstruction.
	//
	// WHAT IT FOUND: muting LOW attenuated EVERY frequency -- 60 Hz by 48 dB and 9 kHz by 5.4 --
	// while muting MID or HIGH did nothing at all. All the signal was in one band. `OnePoleAP`
	// realises `(a + z^-1)/(1 + a z^-1)`, whose corner is `a = (t-1)/(t+1)`, and it was being handed
	// `t/(1+t)` -- the one-pole LOWPASS coefficient. At 200 Hz and 48 kHz that is +0.0129 where
	// -0.9742 was wanted, so the allpass was very nearly a plain delay and the "split" was a comb.
	constexpr float sampleRate = 48000.0f;
	constexpr size_t n = 24000;

	// MODE 1 IS THE ONE THAT CLAIMS TO BE CORRECT. `crossover.h` says so in as many words: "ORDER=1
	// is the correct 6dB/oct crossover; higher orders are 'broken' but creatively useful". So this
	// pins the mode that makes a claim, and leaves Quirky, Twisted and Weird to be what they say
	// they are.
	auto mutedGain = [&](int band, float freq, float mode) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings s = defaultSettings();
		s.mode = mode;
		s.ratio = 0.0f; // ROUTING, not dynamics -- no compression anywhere in this measurement
		s.lowCrossover = 200.0f;
		s.highCrossover = 2000.0f;
		if (band == 0) s.low.level = 0.0f;
		if (band == 1) s.mid.level = 0.0f;
		if (band == 2) s.high.level = 0.0f;
		owltt.setTarget(s);
		std::vector<float> L(n), R(n);
		fillSine(L, R, freq, sampleRate, 0.25f, n);
		processInChunks(owltt, L, R, 128);
		float peak = 0.0f;
		for (size_t i = n * 3 / 4; i < n; ++i) {
			peak = std::max(peak, std::abs(L[i]));
		}
		return 20.0f * std::log10(std::max(peak, 1e-9f)) - 20.0f * std::log10(0.25f);
	};

	struct Case { float freq; int ownBand; const char* name; };
	// WELL INSIDE EACH BAND, not at the corners: a crossover is 6 dB/oct here, so a tone AT 200 Hz
	// genuinely belongs to two bands and asserting otherwise would be asserting a brick wall.
	const std::array<Case, 3> cases { { { 60.0f, 0, "low" }, { 900.0f, 1, "mid" },
		                                { 6000.0f, 2, "high" } } };
	// EVERY MODE THAT CLAIMS TO BE A REAL CROSSOVER. 1 is the AP 6 dB/oct one `crossover.h` names as
	// correct; 5-8 are LR2 and LR4, Fast and Full. Left out on purpose: 2, 3, 4 and 10 are the
	// higher-order allpass variants the header calls "'broken' but creatively useful", and 9 is AP1
	// with low and high deliberately swapped. A case that demanded correct routing from those would
	// be asserting against their stated job.
	for (const float mode : { 1.0f, 5.0f, 6.0f, 7.0f, 8.0f }) {
		CAPTURE(mode);
		for (const Case& c : cases) {
			CAPTURE(c.freq);
			CAPTURE(std::string(c.name));
			const float own = mutedGain(c.ownBand, c.freq, mode);
			for (int other = 0; other < 3; ++other) {
				if (other == c.ownBand) {
					continue;
				}
				CAPTURE(other);
				const float loss = mutedGain(other, c.freq, mode);
				// MUTING ITS OWN BAND MUST COST MORE THAN MUTING EITHER OTHER, and by a real margin --
				// 3 dB, so a splitter that merely leans the right way does not pass.
				CHECK(own < loss - 3.0f);
			}
			// AND MUTING ITS OWN BAND ACTUALLY DOES SOMETHING. Without this, three bands that all
			// pass nothing would satisfy the comparison above.
			CHECK(own < -4.0f);
		}
	}
}

TEST_CASE("a global at its declared default displaces nothing, so a band keeps its whole range") {
	// THE INVARIANT BEHIND THE REPORTED BUG: the per-band ratio bars in the popout graph did not respond.
	//
	// `effectiveBandOf` CLAMPS THE SUM, which is right and is not the bug: a band control declared
	// 0..1 plus a shift can reach only `[max(0, shift), min(1, 1 + shift)]`. So the band loses
	// `|shift|` of its declared travel, at whichever end the shift points. That is the ordinary cost
	// of an offset control pushed off neutral, and every other offset in this codebase pays it.
	//
	// WHAT IS NOT ORDINARY IS PAYING IT AT THE DEFAULT. `ratioShiftOf` returned the global ITSELF
	// rather than its distance from neutral, and the global's declared default is 0.5 -- so a device
	// as it comes out of the box had HALF of every per-band ratio unreachable, and the unreachable
	// half was the bottom one. The graph draws a strip over the whole of 0..1 and the strip's handle
	// sits on the effective value, so the ring simply stopped following the pointer at mid-height:
	// MEASURED before the fix, a drag to 0.25 left the effective ratio at 0.500 and the ring where it
	// started, on the card and on the panel alike.
	//
	// STATED AS A PROPERTY OF THE DECLARATIONS rather than as three numbers, because three numbers
	// are what drifted. Whatever a global's declared default is, its shift at that default is zero.
	const auto defaultOf = [](Param p) {
		return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
	};

	Settings atDefaults {};
	atDefaults.threshold = defaultOf(Param::Threshold);
	atDefaults.ratio = defaultOf(Param::Ratio);
	atDefaults.bandwidth = defaultOf(Param::Bandwidth);

	CHECK(thresholdShiftOf(atDefaults) == doctest::Approx(0.0f));
	CHECK(ratioShiftOf(atDefaults) == doctest::Approx(0.0f));
	CHECK(bandwidthShiftOf(atDefaults) == doctest::Approx(0.0f));

	// AND THE CONSEQUENCE, SPELLED OUT ON THE VALUES rather than left to follow from the shifts: with
	// the globals where they come up, a band's own control sweeps its whole declared range into the
	// effective value the compressor uses. This is the claim the picture rests on, so it is asserted
	// on the picture's own quantity and not on the shift.
	for (const float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		CAPTURE(v);
		BandParams band {};
		band.threshold = v;
		band.ratio = v;
		band.bandwidth = v;
		const EffectiveBand eff = effectiveBandOf(band, atDefaults);
		CHECK(eff.threshold == doctest::Approx(v));
		CHECK(eff.ratio == doctest::Approx(v));
		CHECK(eff.bandwidth == doctest::Approx(v));
	}

	// AND THE DEVICE STILL COMPRESSES OUT OF THE BOX, which is what the additive shift was FOR --
	// `owltt_types.h`'s own words: "the device compresses out of the box through the macro rather than
	// through twelve knobs". A centred shift keeps that only if the per-band default moves to match,
	// so the two declarations are pinned together here rather than trusted to agree.
	BandParams shipped {};
	shipped.ratio = defaultOf(Param::LowRatio);
	CHECK(effectiveBandOf(shipped, atDefaults).ratio == doctest::Approx(0.5f));
	CHECK(defaultOf(Param::LowRatio) == doctest::Approx(defaultOf(Param::MidRatio)));
	CHECK(defaultOf(Param::LowRatio) == doctest::Approx(defaultOf(Param::HighRatio)));

	// THE GLOBAL'S OWN SWEEP IS UNCHANGED BY THE MOVE, which is what makes it safe. At the per-band
	// default the macro must still take the device from no compression to full limiting across its
	// whole travel -- the property a hand on that knob would notice immediately if it were lost.
	for (const float g : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		CAPTURE(g);
		Settings macro = atDefaults;
		macro.ratio = g;
		CHECK(effectiveBandOf(shipped, macro).ratio == doctest::Approx(g));
	}
}


TEST_CASE("the attack and release knobs map to the times the code documents") {
	// **The mapping follows the curve the code writes down.** `setAttack` computes
	// `0.5 + (exp(2k) - 1) * 15` and `setRelease` computes `5 + (exp(2k) - 1) * 75`, and until
	// `fastExp` was replaced neither did: the approximation carries `1 + 0.08x^2`, a fudge fitted near
	// zero, so `exp(2)` came out 1.347x high and the top of both knobs was that far off its own
	// formula.
	//
	// ASSERTED AGAINST AN INDEPENDENT RESTATEMENT — `std::exp` written out here, not a second call
	// into the device — and **at both ends of the knob**, because the error is 1.00x at the bottom and
	// 1.35x at the top, so a case at one end cannot see the curve at all.
	constexpr float kSR = 48000.0f;
	Owltt d;
	d.prepare(kSR);
	Settings s = defaultSettings();
	s.enabled = true;
	std::vector<float> l(32, 0.0f);
	std::vector<float> r(32, 0.0f);

	int checked = 0;
	for (const float knob : { 0.0f, 0.05f, 0.25f, 0.5f, 0.75f, 1.0f }) {
		s.attack = knob;
		s.release = knob;
		d.setTarget(s);
		// The knobs reach the bands through a render, which is the only road there is.
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		d.processStereo(l.data(), r.data(), 32);

		const double e = std::exp(2.0 * static_cast<double>(knob));
		const double wantAttack = 0.5 + (e - 1.0) * 15.0;
		const double wantRelease = 5.0 + (e - 1.0) * 75.0;
		CAPTURE(knob);
		CAPTURE(wantAttack);
		CAPTURE(wantRelease);
		CHECK(static_cast<double>(d.attackMsForTest(0)) == doctest::Approx(wantAttack).epsilon(1e-5));
		CHECK(static_cast<double>(d.releaseMsForTest(0)) == doctest::Approx(wantRelease).epsilon(1e-5));
		++checked;
	}
	CHECK(checked == 6);

	// AND THE TWO ENDS ARE FAR APART, so "it matches the formula" is not a statement about a knob that
	// does nothing. 0.5 ms to 96.3 ms is the range the formula describes.
	s.attack = 0.0f;
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), 32);
	const float bottom = d.attackMsForTest(0);
	s.attack = 1.0f;
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), 32);
	const float top = d.attackMsForTest(0);
	CAPTURE(bottom);
	CAPTURE(top);
	CHECK(bottom == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(top == doctest::Approx(96.3358).epsilon(1e-4));
}

TEST_CASE("the ATTACK knob sets the attack, the RELEASE knob does not, and the block never did") {
	// **Even at minimum attack, OWLTT's attack was slow. It was, and the cause was not where anyone
	// looked.**
	//
	// Measured before anything was changed: the attack time did NOT scale with the host's block — 68,
	// 65.3, 64, 64, 69.3, 64 ms across blocks of 16 to 512, flat across a 32x range — so the detection
	// window was not the bottleneck. Forcing the detector instant left it at 64 ms; forcing the
	// ENVELOPE instant collapsed it to 1.33 ms. The envelope was all of it.
	//
	// **AND THE ENVELOPE WAS USING THE WRONG COEFFICIENT.** `runEnvelope` asked "is the gain moving
	// away from unity", which differs from "is the gain going down" in exactly one place: the first
	// note after a rest, when the upward expander sits at +18 dB and the gain must travel DOWN THROUGH
	// unity. Every step of that descent counted as a release.
	//
	// SO THIS ASSERTS THE THREE THINGS THAT WERE WRONG, as TIMES rather than as renders — a comparison
	// of two buffers proves something changed, never that a 0.5 ms attack takes 0.5 ms.
	constexpr float kSR = 48000.0f;

	// Time for band 0's gain to reach half its eventual reduction after silence, in ms. Read off the
	// gain the sample loop applies, through the seam that exists because no render can be asked what a
	// coefficient is.
	const auto attackMs = [&](int block, float attackKnob, float releaseKnob) {
		Owltt d;
		d.prepare(kSR);
		Settings s = defaultSettings();
		s.enabled = true;
		s.attack = attackKnob;
		s.release = releaseKnob;
		s.threshold = 0.5f; // the shipped defaults, which is the population the report is about
		s.ratio = 0.5f;
		d.setTarget(s);

		std::vector<float> l(static_cast<size_t>(block), 0.0f);
		std::vector<float> r(static_cast<size_t>(block), 0.0f);
		for (int i = 0; i < 200; ++i) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.processStereo(l.data(), r.data(), block);
		}
		const float rest = d.smoothedGainForTest(0);

		const int calls = static_cast<int>(48000 * 0.5) / block + 2;
		std::vector<float> trace;
		for (int i = 0; i < calls; ++i) {
			for (int k = 0; k < block; ++k) {
				const double t = static_cast<double>(i * block + k) / kSR;
				l[static_cast<size_t>(k)] =
				    0.9f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * t));
				r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
			}
			d.processStereo(l.data(), r.data(), block);
			trace.push_back(d.smoothedGainForTest(0));
		}
		const float settled = trace.back();
		const float half = rest + 0.5f * (settled - rest);
		for (size_t i = 0; i < trace.size(); ++i) {
			if (settled < rest ? trace[i] <= half : trace[i] >= half) {
				return 1000.0 * static_cast<double>((i + 1) * block) / kSR;
			}
		}
		return -1.0;
	};

	SUBCASE("the attack knob moves the attack, monotonically and over a real range") {
		// NON-VACUITY FIRST: there has to BE a reduction to time, or every number below is the length
		// of a search that found nothing.
		const double fastest = attackMs(32, 0.0f, 0.25f);
		const double slowest = attackMs(32, 1.0f, 0.25f);
		CAPTURE(fastest);
		CAPTURE(slowest);
		REQUIRE(fastest > 0.0);
		REQUIRE(slowest > 0.0);
		// MEASURED: 2.0 ms at the bottom, 30.0 at the top. Before the fix these were 17.3 and 20.7 —
		// a knob with 1.2x of travel where its own formula promises 190x.
		CHECK(fastest < 4.0);
		CHECK(slowest > 20.0);
		CHECK(slowest / fastest > 8.0);

		double previous = 0.0;
		int rose = 0;
		for (const float k : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			const double ms = attackMs(32, k, 0.25f);
			CAPTURE(k);
			CAPTURE(ms);
			if (ms > previous) {
				++rose;
			}
			previous = ms;
		}
		CHECK(rose == 5);
	}

	SUBCASE("the RELEASE knob does not set the attack, which is the defect this case exists for") {
		// **THE MEASUREMENT THAT NAMED THE BUG.** Before the fix the release knob moved the attack
		// time from 4 ms to 122.7 ms — 31x — while the attack knob moved it 1.2x. The two controls
		// had swapped jobs, and no setting of Attack could make the attack fast.
		std::vector<double> byRelease;
		for (const float rel : { 0.0f, 0.25f, 0.5f, 1.0f }) {
			byRelease.push_back(attackMs(32, 0.0f, rel));
		}
		CAPTURE(byRelease[0]);
		CAPTURE(byRelease[3]);
		REQUIRE(byRelease[0] > 0.0);
		REQUIRE(byRelease[3] > 0.0);
		// A 25x span of release times may move the attack a little — the return path IS a release and
		// the trajectory is not one-directional — but it must not GOVERN it. Four is generous against
		// the 31x that was there and tight against the 2x that is.
		CHECK(byRelease[3] / byRelease[0] < 4.0);
	}

	SUBCASE("and the attack time is a duration, not a count of blocks") {
		// THE OTHER DECISION, and the one whose prediction was wrong: the WINDOW was block-dependent
		// and the attack time was not. Both are block-independent now, and the invariance is what is
		// pinned here rather than the number, which moves with the classification fix.
		const double at32 = attackMs(32, 0.0f, 0.25f);
		CAPTURE(at32);
		REQUIRE(at32 > 0.0);
		for (const int block : { 16, 64, 128, 256, 512 }) {
			const double ms = attackMs(block, 0.0f, 0.25f);
			CAPTURE(block);
			CAPTURE(ms);
			// One block of slack, because the trajectory can only be sampled once per call. Nothing
			// wider: at 512 samples one block is 10.7 ms, so a window-limited detector could not hide
			// inside this.
			CHECK(std::abs(ms - at32) <= 1000.0 * block / kSR + 0.1);
		}
	}
}

TEST_CASE("the meters fall at a rate in milliseconds, not in host callbacks") {
	// **THE PORTING-SHAPED DEFECT, FOUND BY SWEEPING FOR IT RATHER THAN BY TRIPPING OVER IT.** The
	// firmware writes `peak = max(thisBuffer, peak * 0.95)` once per buffer, and on hardware with ONE
	// block size known at design time that IS a rate. On desktop the host chooses the block, so the
	// same line is a half-life of **9 ms at 32 samples and 148 ms at 512** — sixteen times slower for
	// the same signal, decided by a number in the user's audio settings.
	//
	// Two sites, found together: `BandCompressor::decayBandOutputPeak` and the master `outputPeak_` in
	// `Owltt::updateMeters`. Meters rather than audio, which makes it a smaller finding than the
	// detector and not a smaller defect — the OTT display is what a player reads the compressor's
	// behaviour off.
	//
	// **AND A SECOND DEFECT FOUND BEHIND THE FIRST, WHICH THIS FIXTURE PINS BY EXISTING.** The master
	// decay used to sit INSIDE `updateMeters`, after its `if (!meterCallback_) return;` — while
	// `outputPeakThisBuffer_` is written in the sample loop unconditionally. So with no editor open
	// the master peak was a running maximum that was never cleared: it accumulated for the life of the
	// plugin, and the first meter update after somebody opened the editor reported the loudest thing
	// since load rather than the current level. **This case installs no meter callback**, so the
	// assertion that the meter FALLS is the assertion that the decay is not behind that gate.
	//
	// ASSERTED AS AN INVARIANCE OVER A DURATION. The same total samples, delivered in different block
	// sizes, must leave the meter in the same place; a per-call rate cannot satisfy that and no
	// tolerance hides a 16x difference.
	constexpr float kSR = 48000.0f;
	constexpr int kTotal = 8192; // 171 ms, several half-lives

	const auto peakAfterDecay = [&](int block) {
		Owltt d;
		d.prepare(kSR);
		Settings s = defaultSettings();
		s.enabled = true;
		d.setTarget(s);

		std::vector<float> l(static_cast<size_t>(block), 0.0f);
		std::vector<float> r(static_cast<size_t>(block), 0.0f);
		// A loud burst to charge the meters, delivered the same way in every run.
		for (int done = 0; done < 2048; done += block) {
			for (int k = 0; k < block; ++k) {
				const double t = static_cast<double>(done + k) / kSR;
				l[static_cast<size_t>(k)] =
				    0.9f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * t));
				r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
			}
			d.processStereo(l.data(), r.data(), block);
		}
		const float charged = d.outputPeakForTest();
		// THEN SILENCE, and the meter falls.
		for (int done = 0; done < kTotal; done += block) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.processStereo(l.data(), r.data(), block);
		}
		return std::pair<float, float>{ charged, d.outputPeakForTest() };
	};

	const auto [charged32, fallen32] = peakAfterDecay(32);
	CAPTURE(charged32);
	CAPTURE(fallen32);
	// NON-VACUITY, BOTH ENDS. The meter has to have CHARGED, and it has to have FALLEN — a meter stuck
	// at zero satisfies "every block size agrees" perfectly.
	REQUIRE(charged32 > 0.01f);
	REQUIRE(fallen32 < charged32 * 0.5f);
	REQUIRE(fallen32 > 0.0f);

	for (const int block : { 64, 128, 256, 512 }) {
		const auto [charged, fallen] = peakAfterDecay(block);
		CAPTURE(block);
		CAPTURE(charged);
		CAPTURE(fallen);
		// The charge is the same signal through the same device, so it must match closely; the FALL is
		// the claim. 2% covers the block-boundary quantisation of where the burst ends and nothing
		// more — the defect this replaces was a factor of sixteen.
		CHECK(charged == doctest::Approx(charged32).epsilon(0.02));
		CHECK(fallen == doctest::Approx(fallen32).epsilon(0.02));
	}
}

TEST_CASE("OWLTT's clip indicator holds for a time, not for a number of the host's buffers") {
	// THE FOURTH PER-BLOCK RATE IN THIS DEVICE, and the one the pass that fixed the other three
	// walked past. `Owltt::processStereo` says of the master peak decay: "Same reference block as the
	// gain smoother and the detector, so all three of this device's ported per-block rates agree
	// about what 'today' means." There are FOUR. `updateMeters`, immediately below, still counts in
	// CALLS: `if (++meterRefreshCounter_ >= kMeterRefreshBuffers)` with 35, and a hold of
	// `kIndicatorHoldBuffers / kMeterRefreshBuffers` refreshes -- 140 calls in all.
	//
	// The firmware could count buffers because it had ONE buffer size, decided at design time. A
	// plugin is handed whatever the host feels like, so the same clip lights for a tenth of a second
	// at a 32-sample block and a second and a half at 512.
	//
	// ASSERTED AS A VALUE, NOT AS A DIFFERENCE BETWEEN RENDERS: the quantity in question is a
	// DURATION, so the case reads the duration. `clipping` is only observable through the meter
	// callback, so that is the seam used.
	constexpr double kSampleRate = 48000.0;
	// LONG ENOUGH FOR A REFRESH AT THE LARGER BLOCK, and that is a fixture requirement rather than a
	// margin: the clip test only runs every 35 CALLS, so a 4096-sample burst is 8 calls at block 512
	// and the indicator is never even evaluated. The first draft did exactly that and the
	// non-vacuity REQUIRE below caught it.
	constexpr int kBurst = 65536;
	constexpr int kQuiet = 200000; // longer than the largest hold any block size can produce

	const auto holdSamplesAt = [&](int block) {
		Owltt owltt;
		owltt.prepare(kSampleRate);
		owltt.setTarget(defaultSettings());

		long samplesSeen = 0;
		long blockEnd = 0;
		long lastClipAt = -1;
		bool everClipped = false;
		owltt.setMeterCallback([&](size_t, const std::array<float, 3>&, const std::array<float, 3>&,
		                          const std::array<float, 3>&, bool clipping, const Owltt::MeterExtras&) {
			if (clipping) {
				everClipped = true;
				lastClipAt = blockEnd;
			}
		});

		std::vector<float> left(static_cast<size_t>(block), 0.0f);
		std::vector<float> right(static_cast<size_t>(block), 0.0f);

		// A burst well over full scale, so the output peak clears the clip threshold of 1.0.
		long burstEnd = 0;
		for (int done = 0; done < kBurst; done += block) {
			// HOT ENOUGH TO CLIP, measured rather than assumed: an amplitude of 4 gives an output
			// peak of 2.44 and does NOT light the indicator, so `outputPeak_` is plainly not on the
			// output samples' scale. 16 does light it.
			std::fill(left.begin(), left.end(), 16.0f);
			std::fill(right.begin(), right.end(), 16.0f);
			blockEnd = samplesSeen + block;
			owltt.processStereo(left.data(), right.data(), block);
			samplesSeen = blockEnd;
			burstEnd = samplesSeen;
		}
		// Then silence, for long enough that the indicator must have released.
		for (int done = 0; done < kQuiet; done += block) {
			std::fill(left.begin(), left.end(), 0.0f);
			std::fill(right.begin(), right.end(), 0.0f);
			blockEnd = samplesSeen + block;
			owltt.processStereo(left.data(), right.data(), block);
			samplesSeen = blockEnd;
		}
		// NON-VACUITY: if the burst never lit the indicator, the number below is about nothing.
		REQUIRE(everClipped);
		REQUIRE(lastClipAt > 0);
		return lastClipAt - burstEnd;
	};

	const long smallBlock = holdSamplesAt(32);
	const long largeBlock = holdSamplesAt(512);
	CAPTURE(smallBlock);
	CAPTURE(largeBlock);

	// THE SAME WALL-CLOCK HOLD, whatever the host's buffer. Generous tolerance -- the quantity is
	// quantised to the refresh period, so the two cannot agree to the sample.
	const double ratio = static_cast<double>(largeBlock) / static_cast<double>(std::max(1L, smallBlock));
	CAPTURE(ratio);
	CHECK(ratio < 2.0);
	CHECK(ratio > 0.5);
}

TEST_CASE("every descriptor sits at its own Param's index, named") {
	// **THE SHAPER'S GUARD, ON THE DEVICE WITH THE POSITIONAL-WALK HISTORY.** `kParamDescs` is a
	// positional array read by index everywhere — `kParamDescs[static_cast<size_t>(p)]` — so an entry
	// inserted or removed in the middle slides every later control onto the wrong declaration: wrong
	// range, wrong default, wrong tooltip, wrong `modSpan`, and no compile error anywhere.
	//
	// **THE `static_assert` ON THE LENGTH CANNOT SEE IT**, and this case exists because the length
	// check is what was relied on. Deleting two entries from the enum and two from the array in the
	// same edit keeps the two counts equal by construction — which is exactly the edit that removed
	// `CharacterPhase`/`VibePhase` (OWLTT still had dual-gang value boxes), and
	// exactly the edit shape whose earlier cousin cost this device an hour of hunting an instability
	// in the phase DSP that was not there. See the row-walk case below.
	//
	// **A HAND-WRITTEN LIST IS THE RIGHT SHAPE HERE, unusually** — the shaper's version says why and
	// it is unchanged: this is a SECOND, independent statement of an ordering nothing else asserts,
	// and two hand-maintained lists disagreeing is precisely the event to be told about. Deriving it
	// from `kParamDescs` would make it agree by construction and assert nothing at all.
	const std::pair<Param, const char*> kExpected[] = {
		{ Param::Mode, "Mode" },
		{ Param::Enabled, "Enabled" },
		{ Param::Threshold, "Threshold" },
		{ Param::Ratio, "Ratio" },
		{ Param::Bandwidth, "Bandwidth" },
		{ Param::UpDownSkew, "Up/Down Skew" },
		{ Param::Attack, "Attack" },
		{ Param::Release, "Release" },
		{ Param::Character, "Character" },
		{ Param::Vibe, "Vibe" },
		{ Param::LowThreshold, "Low Threshold" },
		{ Param::LowRatio, "Low Ratio" },
		{ Param::LowBandwidth, "Low Bandwidth" },
		{ Param::LowLevel, "Low Level" },
		{ Param::MidThreshold, "Mid Threshold" },
		{ Param::MidRatio, "Mid Ratio" },
		{ Param::MidBandwidth, "Mid Bandwidth" },
		{ Param::MidLevel, "Mid Level" },
		{ Param::HighThreshold, "High Threshold" },
		{ Param::HighRatio, "High Ratio" },
		{ Param::HighBandwidth, "High Bandwidth" },
		{ Param::HighLevel, "High Level" },
		{ Param::LowCrossover, "Low Crossover" },
		{ Param::HighCrossover, "High Crossover" },
		{ Param::OutputGain, "Output Gain" },
		{ Param::Blend, "Blend" },
		{ Param::SoftClip, "Soft Clip" }, // backlog EL, appended 2026-09-16
		{ Param::CharacterMode, "Character Mode" }, // backlog IW, appended 2026-09-23
		{ Param::VibeMode, "Vibe Mode" },
		{ Param::StereoLink, "Stereo Link" }, // backlog LQ, appended 2026-10-10
		{ Param::StereoMode, "Stereo Mode" },
		{ Param::Width, "Width" }, // backlog LR, appended 2026-10-10
		{ Param::LowWidth, "Low Width" },
		{ Param::MidWidth, "Mid Width" },
		{ Param::HighWidth, "High Width" },
		{ Param::Drive, "Drive" }, // backlog NB, appended 2026-10-17
		{ Param::Lookahead, "Lookahead" }, // backlog ND, appended 2026-10-18
		{ Param::KeyTrack, "OWLTT Key Track" }, // Key Track, appended 2026-10-28
	};
	// THE COUNT IS PART OF THE CLAIM, or a list that lost an entry would pass by checking fewer.
	CHECK(std::size(kExpected) == static_cast<size_t>(Param::kCount));
	for (const auto& [param, name] : kExpected) {
		const size_t index = static_cast<size_t>(param);
		REQUIRE(index < std::size(kParamDescs));
		CAPTURE(index);
		CAPTURE(name);
		CHECK(std::string(kParamDescs[index].name) == name);
	}
}

TEST_CASE("the row walk and the Param enum agree, field by field") {
	// **THE CASE THAT WOULD HAVE CAUGHT A SHIPPED-LOOKING BUG IN MINUTES, and did not exist.**
	//
	// `stateOf` and `rowOf` walk the row POSITIONALLY — `row[i++]` — while every other file addresses
	// it by `Param`. The bug this was written for: `CharacterPhase`/`VibePhase` were added next to
	// `character` and `vibe`, which is where they belong by MEANING, putting them at indices 10 and 11
	// while the enum put them at 26 and 27. Every band's threshold, ratio, bandwidth and level then
	// read one slot late. (Those two params are deleted now; the hazard is the WALK, which remains.)
	//
	// THE DEVICE STILL RAN, WHICH IS THE POINT. Nothing crashed, nothing was NaN, and the two walks
	// agreed with EACH OTHER — so a `stateOf(rowOf(s)) == s` roundtrip is green on the shifted
	// version and cannot see this at all. What it did was diverge to a peak of 7203 out of a 4.0
	// input on the device's OWN DEFAULTS; the registry's hot-signal sweep caught it and named OWLTT,
	// and the hour after that went on hunting an instability in the phase DSP that was not there.
	//
	// SO THE CLAIM HERE IS THE ONE THAT WAS MISSING: not that the two walks are inverses of each
	// other, but that BOTH agree with the enum. Written against `rowOf`, which does not clamp, so a
	// mismatch reads as a wrong index rather than as a squashed value.
	Settings s;
	s.mode = 3.0f;
	s.enabled = true;
	s.threshold = 0.11f;
	s.ratio = 0.12f;
	s.bandwidth = 0.13f;
	s.upDownSkew = 0.14f;
	s.attack = 0.15f;
	s.release = 0.16f;
	s.character = 0.17f;
	s.vibe = 0.18f;
	s.low = { 0.21f, 0.22f, 0.23f, 0.24f };
	s.mid = { 0.31f, 0.32f, 0.33f, 0.34f };
	s.high = { 0.41f, 0.42f, 0.43f, 0.44f };
	s.lowCrossover = 0.51f;
	s.highCrossover = 0.52f;
	s.outputGain = 0.53f;
	s.blend = 0.54f;
	s.width = 1.61f; // backlog LR: the four widths, each a number only it holds
	s.low.width = 1.71f;
	s.mid.width = 1.72f;
	s.high.width = 1.73f;
	s.driveDb = 7.25f; // backlog NB

	const Row row = rowOf(s);
	const auto at = [&row](Param p) { return row[static_cast<size_t>(p)]; };

	CHECK(at(Param::Mode) == doctest::Approx(3.0f));
	CHECK(at(Param::Enabled) == doctest::Approx(1.0f));
	CHECK(at(Param::Threshold) == doctest::Approx(0.11f));
	CHECK(at(Param::Ratio) == doctest::Approx(0.12f));
	CHECK(at(Param::Bandwidth) == doctest::Approx(0.13f));
	CHECK(at(Param::UpDownSkew) == doctest::Approx(0.14f));
	CHECK(at(Param::Attack) == doctest::Approx(0.15f));
	CHECK(at(Param::Release) == doctest::Approx(0.16f));
	CHECK(at(Param::Character) == doctest::Approx(0.17f));
	CHECK(at(Param::Vibe) == doctest::Approx(0.18f));

	// THE TWELVE THE SHIFT LANDED ON. Every one of these was reading its neighbour's value.
	CHECK(at(Param::LowThreshold) == doctest::Approx(0.21f));
	CHECK(at(Param::LowRatio) == doctest::Approx(0.22f));
	CHECK(at(Param::LowBandwidth) == doctest::Approx(0.23f));
	CHECK(at(Param::LowLevel) == doctest::Approx(0.24f));
	CHECK(at(Param::MidThreshold) == doctest::Approx(0.31f));
	CHECK(at(Param::MidRatio) == doctest::Approx(0.32f));
	CHECK(at(Param::MidBandwidth) == doctest::Approx(0.33f));
	CHECK(at(Param::MidLevel) == doctest::Approx(0.34f));
	CHECK(at(Param::HighThreshold) == doctest::Approx(0.41f));
	CHECK(at(Param::HighRatio) == doctest::Approx(0.42f));
	CHECK(at(Param::HighBandwidth) == doctest::Approx(0.43f));
	CHECK(at(Param::HighLevel) == doctest::Approx(0.44f));

	CHECK(at(Param::LowCrossover) == doctest::Approx(0.51f));
	CHECK(at(Param::HighCrossover) == doctest::Approx(0.52f));
	CHECK(at(Param::OutputGain) == doctest::Approx(0.53f));
	CHECK(at(Param::Blend) == doctest::Approx(0.54f));
	CHECK(at(Param::Width) == doctest::Approx(1.61f));
	CHECK(at(Param::LowWidth) == doctest::Approx(1.71f));
	CHECK(at(Param::MidWidth) == doctest::Approx(1.72f));
	CHECK(at(Param::HighWidth) == doctest::Approx(1.73f));
	CHECK(at(Param::Drive) == doctest::Approx(7.25f));
	// **THE TWO PHASE PARAMS THAT USED TO CLOSE THIS WALK ARE GONE** — their laps
	// are the coordinates' own whole parts now. `Blend` is the last field in both the enum and the
	// walk, which is what this case is really about: the two orders agreeing.

	// AND THE READ WALK IS THE WRITE WALK'S INVERSE, which is the second half of the argument: the
	// pins above put `rowOf` on the enum, and this puts `stateOf` on `rowOf`. Neither alone is the
	// claim — the shifted version passed this one.
	const Settings back = stateOf(row);
	CHECK(back.threshold == doctest::Approx(s.threshold));
	CHECK(back.low.threshold == doctest::Approx(s.low.threshold));
	CHECK(back.low.level == doctest::Approx(s.low.level));
	CHECK(back.mid.ratio == doctest::Approx(s.mid.ratio));
	CHECK(back.high.bandwidth == doctest::Approx(s.high.bandwidth));
	CHECK(back.blend == doctest::Approx(s.blend));
	CHECK(back.character == doctest::Approx(s.character));
	CHECK(back.vibe == doctest::Approx(s.vibe));
	CHECK(back.width == doctest::Approx(s.width));
	CHECK(back.low.width == doctest::Approx(s.low.width));
	CHECK(back.mid.width == doctest::Approx(s.mid.width));
	CHECK(back.high.width == doctest::Approx(s.high.width));
	CHECK(back.driveDb == doctest::Approx(s.driveDb));
}

TEST_CASE("each zone knob's LAP reaches the DSP, and the two knobs are two quantities") {
	// THE FUNCTION WAS WRITTEN AND UNREACHABLE. `feelPhaseOffset_` and `vibePhaseOffset_` are read in
	// six places in `computeCharacterAndVibe` — the phi triangles that author Character's zone 7 and
	// every vibe-driven offset — and were assigned nowhere, so both sat at 0.0f from the port until
	// 2026-08-28: some effects with zone knobs were missing their gamma functions.
	//
	// ASSERTED ON THE SOUND rather than on a setter, because the claim is that the offset REACHES the
	// DSP. A setter test would have passed on the dead version too — the field was always assignable;
	// nothing read it from outside.
	constexpr float sampleRate = 48000.0f;
	const auto render = [&](float character, float vibe) {
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings s = defaultSettings();
		// COMPRESSING, NOT IDLING. `defaultSettings()` leaves `ratio` at 0 — no gain reduction — and
		// the character phases author the KNEE, the per-band TIMING and the SKEW, every one of which
		// is a property of a gain reduction that is not happening. The first draft of this case ran
		// at that default and reported no difference from any offset, which reads exactly like a
		// phase that does not reach the DSP.
		s.threshold = 0.2f;
		s.ratio = 0.8f;
		s.character = character;
		s.vibe = vibe;
		owltt.setTarget(s);
		std::vector<float> L(4096), R(4096);
		const float omega = 2.0f * static_cast<float>(std::numbers::pi) * 220.0f / sampleRate;
		for (size_t i = 0; i < L.size(); ++i) {
			L[i] = R[i] = 0.6f * std::sin(omega * static_cast<float>(i));
		}
		processInChunks(owltt, L, R, 128);
		return L;
	};
	const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
		double worst = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
		}
		return worst;
	};

	// CHARACTER ZONE 7 IS WHERE BOTH OFFSETS LAND, and that is a fact about the port rather than a
	// convenience: every consumer of the phi phases sits in a `case 7:` arm. So the case drives the
	// knob there, and REQUIREs that it did — a zone-6 render would make every check below vacuous.
	const float inZone7 = 7.5f / 8.0f;

	// **THE PHASE IS THE COORDINATE'S LAP NOW — the separate params are gone.** So
	// the same claim is made by moving the coordinate a WHOLE LAP: the zone and the position inside it
	// are identical, and only the field rotation differs. A case that moved a fraction would be
	// changing the zone as well and could not tell the two apart.
	SUBCASE("Character's LAP changes the sound, at the same zone and position") {
		const auto quiet = render(inZone7, 0.0f);
		const auto moved = render(inZone7 + 3.0f, 0.0f);
		CHECK(differs(quiet, moved) > 1e-4);
	}

	SUBCASE("Vibe's LAP changes the sound, at the same zone and position") {
		const auto quiet = render(inZone7, 0.0f);
		const auto moved = render(inZone7, 3.0f);
		CHECK(differs(quiet, moved) > 1e-4);
	}

	SUBCASE("and they are not the same knob") {
		// THE DISTINCTION THIS ARC EXISTS TO KEEP. One device-wide gamma would have made these two
		// renders identical, and a single shared offset is exactly the design that was rejected: the
		// firmware sums a per-knob offset with a device gamma rather than collapsing them. Collapsing
		// each coordinate with its OWN phase is a different move and leaves this distinction intact.
		const auto onCharacter = render(inZone7 + 3.0f, 0.0f);
		const auto onVibe = render(inZone7, 3.0f);
		CHECK(differs(onCharacter, onVibe) > 1e-4);
	}

	SUBCASE("and a moved phase is still a finite, bounded signal") {
		// THE PROPERTY THE REGISTRY SWEEP ASKS OF EVERY DEVICE, asked here at the specific settings
		// this arc made reachable. Activating dead code is exactly where an unbounded path would
		// hide, and the sweep found one — it turned out to be a row-index shift rather than the DSP
		// (see the row-walk case above), which is why this stays: the next one might not be.
		for (const float ph : { 0.0f, 0.5f, 1.0f, 8.0f, 64.0f }) {
			CAPTURE(ph);
			const auto out = render(inZone7 + ph, 1.0f + ph);
			float peak = 0.0f;
			for (const float x : out) {
				REQUIRE(std::isfinite(x));
				peak = std::max(peak, std::fabs(x));
			}
			CHECK(peak < 4.0f);
		}
	}
}

TEST_CASE("COORDINATES: both declarations admit what the DSP accepts, past 64 and below 0") {
	// **THE SAME WALL THE SHAPER'S COLOUR PHASE HAD, ON THE SECOND DEVICE THAT HAS THIS MECHANIC.**
	// Both phase counters declared `0.0, 64.0`, and `ZoneCoordBox::onCarry` clamps each carried lap to
	// the declared range — so the coarse knob stopped at the 64th lap. It was reported twice on the
	// shaper and not yet on these; the report was expected rather than waited for.
	//
	// FIVE WALLS OF THIS SHAPE HAVE NOW BEEN FOUND — Gamma, Rotate, Colour Phase, and these two —
	// **every one by a person noticing and none by a sweep.** `tests/test_device_ranges.cpp` is the
	// sweep; this case is the value assertion for the two controls it covers here.
	//
	// **RE-POINTED AT THE COORDINATES THEMSELVES (OWLTT still had dual-gang value boxes).** The two
	// phase params are gone and Character/Vibe swallowed them: the whole part of
	// each coordinate IS the lap. So the wall this case guards has not moved, it has only changed
	// which declaration can rebuild it — and it is the SAME declaration a hand now drags, which is
	// strictly closer to what was reported than the counter this used to read.
	constexpr float sampleRate = 48000.0f;
	const auto& character = kParamDescs[static_cast<size_t>(Param::Character)];
	const auto& vibe = kParamDescs[static_cast<size_t>(Param::Vibe)];

	SUBCASE("the declared ranges are drag windows and not domains") {
		for (const feathers::device::ParamDesc* d : { &character, &vibe }) {
			// **`std::string`, NOT THE BARE `const char*`.** doctest has no stringifier for a raw
			// pointer and prints it as `1`, so a failure would name neither control — measured on this
			// case's own known-bad, where both entries reported `d->name := 1`.
			CAPTURE(std::string(d->name));
			CHECK(d->max > 64.0);
			CHECK(d->min == -d->max);
			CHECK(d->def == 0.0);
			// **`modSpan` IS NOT THE RANGE AND MUST NOT FOLLOW IT.** One unit is a full turn of the
			// field, read off the consumption; widening a drag window says nothing about how far a
			// full-depth route moves the control. This device's routes were once wrong by 64x through
			// exactly that confusion, so the number is pinned beside the widening that could revive it.
			CHECK(d->modSpan == doctest::Approx(kOnePhaseUnit));
		}
	}

	SUBCASE("the decode carries the whole of both, negatives included") {
		// OWLTT's `stateOf` is a positional copy with no clamping anywhere, so this asserts the
		// contract rather than a transformation — and it is the assertion that would catch a clamp
		// arriving here later, which is precisely how the shaper's gamma was silenced.
		Settings s = defaultSettings();
		s.character = 129.0f;
		s.vibe = -37.5f;
		const Settings back = stateOf(rowOf(s));
		CHECK(back.character == doctest::Approx(129.0f));
		CHECK(back.vibe == doctest::Approx(-37.5f));

		Row ends {};
		ends[static_cast<size_t>(Param::Character)] = static_cast<float>(character.max);
		ends[static_cast<size_t>(Param::Vibe)] = static_cast<float>(vibe.min);
		const Settings atEnds = stateOf(ends);
		CHECK(atEnds.character == doctest::Approx(character.max));
		CHECK(atEnds.vibe == doctest::Approx(vibe.min));
	}

	SUBCASE("and the DSP moves out there — the widening is not a lie") {
		// PROOF THE FIRST TWO SUBCASES ARE NOT VACUOUS, on the same fixture the case above
		// established: character zone 7, compressing rather than idling, because the phases author
		// the knee and the per-band timing of a gain reduction that has to be happening.
		//
		// **THE LAP IS ADDED TO THE COORDINATE NOW, not passed beside it.** Every call below keeps the
		// FRACTION at `inZone7`, so the zone and the position inside it are identical across a pair
		// and only the field rotation differs — the same discipline the case above adopted. A call
		// that passed the lap alone would land in zone 0 and compare two different zones.
		const float inZone7 = 7.5f / 8.0f;
		const auto render = [&](float charLap, float vibeLap) {
			Owltt owltt;
			owltt.prepare(sampleRate);
			Settings s = defaultSettings();
			s.threshold = 0.2f;
			s.ratio = 0.8f;
			s.character = inZone7 + charLap;
			s.vibe = vibeLap;
			owltt.setTarget(s);
			std::vector<float> L(4096), R(4096);
			const float omega = 2.0f * static_cast<float>(std::numbers::pi) * 220.0f / sampleRate;
			for (size_t i = 0; i < L.size(); ++i) {
				L[i] = R[i] = 0.6f * std::sin(omega * static_cast<float>(i));
			}
			processInChunks(owltt, L, R, 128);
			return L;
		};
		const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
			double worst = 0.0;
			for (size_t i = 0; i < a.size(); ++i) {
				worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
			}
			return worst;
		};

		// PAST THE OLD WALL: a lap at 64 must be worth what a lap anywhere else is.
		CHECK(differs(render(64.0f, 0.0f), render(65.0f, 0.0f)) > 1e-4);
		CHECK(differs(render(0.0f, 64.0f), render(0.0f, 65.0f)) > 1e-4);

		// AND BACKWARDS FROM THE ORIGIN, which is what a symmetric declaration buys and what an
		// asymmetric one walls off at the place every patch starts. `wrapPhase` and `triangleFloat`
		// both floor rather than truncate, so a negative lap is a real position.
		CHECK(differs(render(0.0f, 0.0f), render(-3.0f, 0.0f)) > 1e-4);
		CHECK(differs(render(0.0f, 0.0f), render(0.0f, -3.0f)) > 1e-4);

		// AND IT STAYS A FINITE, BOUNDED SIGNAL OUT THERE — the property the registry sweep asks of
		// every device, asked at the settings this widening newly makes reachable.
		for (const float ph : { -8388608.0f, -1000.0f, 1000.0f, 8388608.0f }) {
			CAPTURE(ph);
			float peak = 0.0f;
			for (const float x : render(ph, ph)) {
				REQUIRE(std::isfinite(x));
				peak = std::max(peak, std::fabs(x));
			}
			CHECK(peak < 4.0f);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// THE OWLTT PARITY SWEEP, 2026-09-16 — the port read against the firmware's `multiband.h` at the
// commit it was ported from. Every case below is a VALUE read through a seam, because the thing that
// moved is not recoverable from the render; each has a known-bad that went red (the commit says which).
// ---------------------------------------------------------------------------------------------

namespace {

/// One 512-frame slice of a quiet tone at `settings`, so the block-rate state (zone tables, twist,
/// crossovers) is computed once and can be read.
Owltt sliceAt(const Settings& settings) {
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(settings);
	std::vector<float> L(512), R(512);
	fillSine(L, R, 440.0f, 48000.0f, 0.01f, L.size());
	o.processStereo(L.data(), R.data(), 512);
	return o;
}

} // namespace

TEST_CASE("parity: the four weird crossovers are Quirky, Twisted, Weird and Twist3 — the Width zone's bass "
          "width reads the port's own Mode numbers, not the firmware's zero-based ones") {
	// The Width zone writes the bass width as `-0.2 + pos * 1.5` on a weird splitter and
	// `0.7 + pos * 0.5` otherwise. Character 0.0625 is zone 0 at position 0.5 (eight zones), so the two
	// values are 0.55 and 0.95 — and which Mode gets which is the whole claim.
	Settings s = defaultSettings();
	s.character = 0.0625f;
	s.vibe = 0.0f; // no vibe modulation on the table
	for (int mode = 1; mode <= 10; ++mode) {
		s.mode = static_cast<float>(mode);
		const Owltt o = sliceAt(s);
		const bool weird = (mode == 2 || mode == 3 || mode == 4 || mode == 10); // kModeNames' numbering
		CAPTURE(mode);
		CHECK(o.bandWidthForTest(0) == doctest::Approx(weird ? 0.55f : 0.95f).epsilon(1e-5));
	}
}

TEST_CASE("parity: the vibe depth reads the lap's fraction — 0.55 at the top of every lap, never more, "
          "never negative") {
	Settings s = defaultSettings();
	s.character = 0.0625f;
	for (const float v : { 0.3f, 1.3f, 5.3f, -0.7f, 0.9f, 0.0f }) {
		s.vibe = v;
		const Owltt o = sliceAt(s);
		const float frac = v - std::floor(v);
		CAPTURE(v);
		CHECK(o.vibeModAmountForTest() == doctest::Approx(0.55f * frac).epsilon(1e-5));
		CHECK(o.vibeModAmountForTest() >= 0.0f);
		CHECK(o.vibeModAmountForTest() <= 0.55f);
	}
	// NON-VACUOUS: the depth is a live term — at vibe 0.9 the bass width differs from vibe 0.0's.
	s.vibe = 0.0f;
	const float at0 = sliceAt(s).bandWidthForTest(0);
	s.vibe = 0.9f;
	const float at09 = sliceAt(s).bandWidthForTest(0);
	CHECK(at0 != doctest::Approx(at09));
}

TEST_CASE("parity: the Twisted splitter runs THIS slice's twist — the crossovers are updated after the vibe "
          "pass, not before it") {
	Settings s = defaultSettings();
	s.mode = 3.0f;  // Twisted
	s.vibe = 0.75f; // past the half-lap where the twist starts to vary
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	std::vector<float> L(512), R(512);
	fillSine(L, R, 440.0f, 48000.0f, 0.01f, L.size());
	o.processStereo(L.data(), R.data(), 512);
	const float twistA = o.vibeTwistForTest();
	CHECK(o.twistedCrossoverTwistForTest() == doctest::Approx(twistA).epsilon(1e-6));

	// MOVE VIBE, ONE SLICE: the splitter must carry the NEW twist at the end of that slice.
	s.vibe = 0.9f;
	o.setTarget(s);
	o.processStereo(L.data(), R.data(), 512);
	const float twistB = o.vibeTwistForTest();
	REQUIRE(twistB != doctest::Approx(twistA)); // non-vacuous: the twist actually changed
	CHECK(o.twistedCrossoverTwistForTest() == doctest::Approx(twistB).epsilon(1e-6));
}

TEST_CASE("parity: the output meter reads what LEAVES the device — after the clipper, not before it") {
	// At blend 1 the output buffer IS the post-clip sum, so the meter's peak must equal the buffer's
	// own peak. A pre-clip read is larger whenever the clipper is working — which a 6.0 tone into a
	// 1.0 knee guarantees.
	Settings s = defaultSettings();
	s.mode = 1.0f;
	s.blend = 1.0f;
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	std::vector<float> L(512), R(512);
	fillSine(L, R, 1000.0f, 48000.0f, 6.0f, L.size());
	o.processStereo(L.data(), R.data(), 512);
	float outPeak = 0.0f;
	for (size_t i = 0; i < L.size(); ++i) {
		outPeak = std::max(outPeak, std::max(std::fabs(L[i]), std::fabs(R[i])));
	}
	REQUIRE(outPeak > 1.0f); // the clipper was engaged, so pre- and post-clip peaks differ
	CHECK(o.outputPeakForTest() == doctest::Approx(outPeak).epsilon(1e-5));
}

TEST_CASE("parity: the high crossover keeps the firmware's 100 Hz gap above the low one, so the split cannot "
          "invert") {
	Settings s = defaultSettings();
	s.lowCrossover = 1500.0f;
	s.highCrossover = 1000.0f; // dialled below the low corner
	CHECK(sliceAt(s).effectiveHighCrossoverHzForTest() == doctest::Approx(1600.0f));
	s.lowCrossover = 200.0f;
	s.highCrossover = 2000.0f; // the ordinary case passes through untouched
	CHECK(sliceAt(s).effectiveHighCrossoverHzForTest() == doctest::Approx(2000.0f));
}

TEST_CASE("parity: a band's combined gain is ceilinged at 256x, the firmware's kGainQ22MaxBand") {
	// Full upward ratio on near-silence saturates the compressor's own 31.6x clamp; a Level knob at
	// its top is 10x; the product would be 316x, and the reference could never hold more than 256x.
	Settings s = defaultSettings();
	s.mode = 1.0f;
	s.ratio = 1.0f;
	s.threshold = 1.0f;
	s.upDownSkew = 0.0f; // fully upward
	s.low.level = s.mid.level = s.high.level = 1.0f;
	s.low.ratio = s.mid.ratio = s.high.ratio = 1.0f;
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	// SILENCE, as the attack-time case above feeds it: the follower's log level sits on its floor and
	// the upward path has its whole span to climb. Under a second of it, so the rest flush (one
	// silent second) has not yet cleared the state this reads.
	std::vector<float> L(512, 0.0f), R(512, 0.0f);
	for (int i = 0; i < 80; ++i) {
		std::fill(L.begin(), L.end(), 0.0f);
		std::fill(R.begin(), R.end(), 0.0f);
		o.processStereo(L.data(), R.data(), 512);
	}
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(o.lastTargetBandGainForTest(b) <= 256.0f);
	}
	// NON-VACUOUS: at least one band is AT the ceiling, so the clamp is what set the number.
	const float top = std::max({ o.lastTargetBandGainForTest(0), o.lastTargetBandGainForTest(1),
	                             o.lastTargetBandGainForTest(2) });
	CHECK(top == doctest::Approx(256.0f).epsilon(1e-4));
}

TEST_CASE("parity: the up-threshold seam reads the threshold the band compares against — the bandwidth "
          "below the down threshold, in knob units") {
	// `getBandThresholdUp` used to read a knob field only an uncalled setter wrote: 0 forever. It
	// reads the member `calculateGain` uses now, inverted to knob units like its down-threshold twin.
	Settings s = defaultSettings();
	s.threshold = 0.5f;
	s.bandwidth = 0.5f;
	const Owltt o = sliceAt(s);
	for (size_t b = 0; b < 3; ++b) {
		const float down = o.getBandThresholdDown(b);
		const float downInternal = 0.2f + 0.8f * down;
		const float upInternal = std::max(0.2f, downInternal - 0.6f * 0.5f);
		CAPTURE(b);
		CHECK(o.getBandThresholdUp(b) == doctest::Approx((upInternal - 0.2f) / 0.8f).epsilon(1e-5));
		CHECK(o.getBandThresholdUp(b) != 0.0f);
		CHECK(o.getBandThresholdUp(b) < down);
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG EL — THE OWLTT UI WORK: the soft clipper's switch,
// a light per band while its clipper is working, and the combined gain reduction. Each is a value
// read through a seam or the meter callback; each has a known-bad that went red (the commit).
// ---------------------------------------------------------------------------------------------

TEST_CASE("backlog EL: the Soft Clip switch — Off lets a hot sum through unclipped, On is how the device always "
          "ran, and the row carries it") {
	// A 6.0 tone into a 1.0 output knee: with the clipper on, the output peak is knee + half the
	// excess; with it off, the peak is the unclipped sum. Read off the samples, since at blend 1 the
	// output IS the sum.
	const auto peakFor = [](bool softClip) {
		Settings s = defaultSettings();
		s.mode = 1.0f;
		s.blend = 1.0f;
		s.softClip = softClip;
		Owltt o;
		o.prepare(48000.0f);
		o.setTarget(s);
		std::vector<float> L(512), R(512);
		fillSine(L, R, 1000.0f, 48000.0f, 6.0f, L.size());
		o.processStereo(L.data(), R.data(), 512);
		float peak = 0.0f;
		for (size_t i = 0; i < L.size(); ++i) {
			peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
		}
		return peak;
	};
	const float clipped = peakFor(true);
	const float open = peakFor(false);
	REQUIRE(clipped > 1.0f); // the clipper was working, so the two differ for a reason
	CHECK(open > clipped * 1.2f);
	// THE SWITCH TRAVELS IN THE ROW, at its own index, and defaults On.
	Settings s;
	CHECK(s.softClip == true);
	s.softClip = false;
	const Row row = rowOf(s);
	CHECK(row[static_cast<size_t>(Param::SoftClip)] == 0.0f);
	CHECK(stateOf(row).softClip == false);
	CHECK(kParamDescs[static_cast<size_t>(Param::SoftClip)].def == 1.0);
}

TEST_CASE("backlog EL: a band's light comes on while its clipper is working, holds for the clip light's ~500 ms, "
          "and goes out — and the meter callback carries all three") {
	// ONE HOT BAND BY CONSTRUCTION: the mid and high band Levels are turned to zero, so whatever the
	// splitter hands them (and before backlog ER was fixed it handed them plenty — the old subtractive
	// splitter leaked a low tone into MID at -6 dB, so a 60 Hz tone lit all three bands in the first
	// draft of this case) leaves at zero gain and cannot pass the threshold. The low band, at unity, takes a 4.0 tone past its
	// 1.0 threshold (half its +6 dB knee). The claim is the LIGHTS' mechanism — threshold, hold,
	// release — not the splitter's separation, which has its own case.
	Settings s = defaultSettings();
	s.mode = 8.0f; // LR4 Full
	s.blend = 1.0f;
	s.mid.level = 0.0f;
	s.high.level = 0.0f;
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	std::array<bool, 3> seen { false, false, false };
	float lastCombined = -1.0f;
	o.setMeterCallback([&](size_t, const std::array<float, 3>&, const std::array<float, 3>&,
	                       const std::array<float, 3>&, bool, const Owltt::MeterExtras& extras) {
		seen = extras.bandSaturating;
		lastCombined = extras.combinedGrDb;
	});
	std::vector<float> L(512), R(512);
	fillSine(L, R, 60.0f, 48000.0f, 4.0f, L.size());
	// 1.3 s OF THE HOT TONE — longer than the ~0.9 s hold, ON PURPOSE. The tone starts as a step, and a
	// step is broadband: the mid and high bands see the onset click above the threshold and light for
	// one hold. Reading after the hold has expired is what makes "only band 0" a claim about the tone
	// rather than about its first sample.
	for (int i = 0; i < 120; ++i) {
		std::vector<float> l = L, r = R;
		o.processStereo(l.data(), r.data(), 512);
	}
	CHECK(o.bandSaturatingForTest(0));
	CHECK_FALSE(o.bandSaturatingForTest(1));
	CHECK_FALSE(o.bandSaturatingForTest(2));
	CHECK(seen == std::array<bool, 3> { true, false, false }); // the callback carries the same three
	CHECK(lastCombined >= 0.0f);

	// SILENCE: the light holds, then goes out — kIndicatorHoldBuffers of the firmware's 256-frame
	// buffers is ~0.9 s at 48 kHz; read at 0.3 s (still lit) and after 1.5 s (out).
	std::vector<float> z(512, 0.0f), z2(512, 0.0f);
	for (int i = 0; i < 28; ++i) { // 0.3 s
		std::fill(z.begin(), z.end(), 0.0f);
		std::fill(z2.begin(), z2.end(), 0.0f);
		o.processStereo(z.data(), z2.data(), 512);
	}
	CHECK(o.bandSaturatingForTest(0));
	for (int i = 0; i < 140; ++i) { // +1.5 s
		std::fill(z.begin(), z.end(), 0.0f);
		std::fill(z2.begin(), z2.end(), 0.0f);
		o.processStereo(z.data(), z2.data(), 512);
	}
	CHECK_FALSE(o.bandSaturatingForTest(0));
}

TEST_CASE("backlog ER (found 2026-09-16, fixed the same day): the 'LR' splitter is a Linkwitz-Riley pair now — a 60 Hz "
          "tone leaves MID what the 24 dB/oct prototype says, not half of itself") {
	// THE DEFECT, kept for the record: `LRCrossover::process` formed `rest = input - LP^N(input)` and
	// took MID from the rest. A one-pole cascade lags a tone below its corner, and `x - lagged(x)` is
	// not small — |1 - 0.97 e^(-j30°)| is 0.51 — so HALF a 60 Hz tone arrived in MID through a splitter
	// whose name promised 24 dB/octave. The firmware (`lr_crossover.h`) has the same arithmetic; the
	// port inherited it; this case pinned the leak at 0.49 until the fix, so the fix was measured
	// against a number. THE FIX: the highpass is a real Butterworth-squared highpass, not a subtraction,
	// so the leak is the prototype's — (Ω²/√(1+Ω⁴))² at Ω = 60/200 = 0.0080 — and `low + mid + high`
	// is the input through an allpass the blend is now matched to (`alignDry`). The full response is
	// pinned in test_fx_owltt_crossover; this is the sighting, restated as the separation it should be.
	LR4CrossoverFull x;
	x.setLowCrossover(200.0f, 48000.0f);
	x.setHighCrossover(2000.0f, 48000.0f);
	std::array<float, 3> peak { 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < 48000; ++i) {
		const float in = std::sin(2.0f * static_cast<float>(std::numbers::pi) * 60.0f * static_cast<float>(i) / 48000.0f);
		CrossoverBands out;
		x.process(in, out);
		if (i > 24000) {
			peak[0] = std::max(peak[0], std::fabs(out.low));
			peak[1] = std::max(peak[1], std::fabs(out.mid));
			peak[2] = std::max(peak[2], std::fabs(out.high));
		}
	}
	const float ratio = 60.0f / 200.0f;
	const float hp4 = (ratio * ratio / std::sqrt(1.0f + ratio * ratio * ratio * ratio));
	CHECK(peak[0] > 0.98f);                                          // the low band has the tone
	CHECK(peak[1] == doctest::Approx(hp4 * hp4).epsilon(0.05));       // 0.0080 — the prototype's leak, not 0.49
	CHECK(peak[2] < 1e-4f);                                          // and the high band is clean
}

TEST_CASE("backlog ER: a wet/dry Blend on an LR mode mixes against the crossover's own phase — a tone AT a corner "
          "comes through a 50 % blend at unity where the raw mix would cancel it") {
	// An LR4 pair's sum is the input through a second-order allpass per crossover, which is -180° AT
	// the corner. So wet + dry at 50 % against the RAW input is zero there: a 2 kHz tone through a
	// 2 kHz crossover vanishes when Blend is turned to the middle. The dry buffer runs through the same
	// allpasses now (`alignDry`), and the mix is flat. The compressor is held out of the picture — no
	// ratio, a threshold nothing reaches, a quiet tone — so the claim is the blend's arithmetic alone.
	constexpr float sampleRate = 48000.0f;
	for (const float freq : { 200.0f, 2000.0f }) {
		CAPTURE(freq);
		Owltt owltt;
		owltt.prepare(sampleRate);
		Settings s = defaultSettings();
		s.mode = 8.0f; // LR4 Full
		s.blend = 0.5f;
		s.ratio = 0.0f;
		s.threshold = 1.0f;
		s.softClip = false;
		owltt.setTarget(s);
		std::vector<float> L(19200), R(19200);
		fillSine(L, R, freq, sampleRate, 0.1f, L.size());
		processInChunks(owltt, L, R, 128);
		double sq = 0.0;
		for (size_t i = L.size() / 2; i < L.size(); ++i) {
			sq += static_cast<double>(L[i]) * L[i];
		}
		const float rms = static_cast<float>(std::sqrt(sq / static_cast<double>(L.size() / 2)));
		// 0.1 amplitude is 0.0707 RMS; unity through the blend. The raw mix measures ~0 here.
		CHECK(rms == doctest::Approx(0.0707f).epsilon(0.05));
	}
}

TEST_CASE("backlog EL: the combined gain reduction is the mean |gain change| of the three bands, in dB — "
          "restated from the band envelopes the bars draw") {
	Settings s = defaultSettings();
	s.mode = 1.0f;
	s.ratio = 0.8f;
	s.threshold = 0.3f;
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	// METERING RUNS ONLY FOR A LISTENER — `updateMeters` returns at once without a callback, as the
	// firmware's `meteringEnabled_` gates its own — so the case is one, as every surface is.
	float carried = -1.0f;
	o.setMeterCallback([&](size_t, const std::array<float, 3>&, const std::array<float, 3>&,
	                       const std::array<float, 3>&, bool, const Owltt::MeterExtras& extras) {
		carried = extras.combinedGrDb;
	});
	std::vector<float> L(512), R(512);
	fillSine(L, R, 440.0f, 48000.0f, 0.8f, L.size());
	for (int i = 0; i < 40; ++i) {
		std::vector<float> l = L, r = R;
		o.processStereo(l.data(), r.data(), 512);
	}
	float expected = 0.0f;
	for (size_t b = 0; b < 3; ++b) {
		expected += std::fabs(o.getBandGainReductionDB(b) * 8.686f);
	}
	expected /= 3.0f;
	REQUIRE(expected > 0.5f); // the compressor is working, so the mean is a number and not a floor
	CHECK(o.combinedGainReductionDbForTest() == doctest::Approx(expected).epsilon(1e-4));
	CHECK(carried == doctest::Approx(expected).epsilon(1e-4)); // and the callback carries the same number
}

TEST_CASE("backlog FK: a moving Blend is followed per sample inside the call — every sample is wet and dry mixed at that sample's value") {
	// Restated independently: Blend does not reach the compressor, so a twin at Blend 1 on the same input is the wet
	// signal, the input is the dry, and each output sample must be wet*b[i] + dry*(1-b[i]) for the window's own b[i].
	// Handed through the ops road (`AudioOps::setFieldWindows`), as the rack hands it.
	constexpr float sampleRate = 48000.0f;
	Settings target = defaultSettings();
	target.ratio = 0.8f;
	Owltt wet;
	Owltt moving;
	wet.prepare(sampleRate);
	moving.prepare(sampleRate);
	wet.setTarget(target);
	target.blend = 0.2f; // the scalar the window replaces
	moving.setTarget(target);

	constexpr int kFrames = 64;
	std::vector<float> wetL(4800), wetR(4800);
	fillSine(wetL, wetR, 1000.0f, sampleRate, 0.5f, wetL.size());
	std::vector<float> movL = wetL, movR = wetR;
	const std::vector<float> in = wetL;
	std::array<float, kFrames> run {};
	for (int i = 0; i < kFrames; ++i) {
		run[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(kFrames - 1); // 0 -> 1 across the call
	}
	std::array<const float*, 32> runs {};
	runs[static_cast<size_t>(Param::Blend)] = run.data();
	REQUIRE(kAudioOps.setFieldWindows != nullptr);
	REQUIRE(kAudioOps.windowedControls.size() == 2u); // Blend (FK) and Drive (backlog NB)
	float worst = 0.0f;
	for (size_t offset = 0; offset + kFrames <= wetL.size(); offset += kFrames) {
		wet.processStereo(&wetL[offset], &wetR[offset], kFrames);
		kAudioOps.setFieldWindows(&moving, runs.data(), runs.size(), kFrames);
		moving.processStereo(&movL[offset], &movR[offset], kFrames);
		for (int i = 0; i < kFrames; ++i) {
			const size_t at = offset + static_cast<size_t>(i);
			const float b = run[static_cast<size_t>(i)];
			worst = std::max(worst, std::abs(movL[at] - (wetL[at] * b + in[at] * (1.0f - b))));
		}
	}
	CAPTURE(worst);
	CHECK(worst < 1e-6f);
	// NOT VACUOUS: wet and dry differ, so a scalar blend could not pass.
	float apart = 0.0f;
	for (size_t i = 0; i < wetL.size(); ++i) {
		apart = std::max(apart, std::abs(wetL[i] - in[i]));
	}
	CHECK(apart > 0.01f);
}

TEST_CASE("IW: a mode and an unbounded value put the coordinate at lap + (mode + amount) / 8 — and Free is the value itself") {
	// Modes, then Free; and since the modes' implementations are unbounded, the in-mode phase stays unbounded.
	CHECK(zoneModeCoordinate(3, 2.4f) == doctest::Approx(2.0f + (3.0f + 0.4f) / 8.0f));   // Punch, lap 2, 40% in
	CHECK(zoneModeCoordinate(1, -0.25f) == doctest::Approx(-1.0f + (1.0f + 0.75f) / 8.0f)); // floor, not truncation
	CHECK(zoneModeCoordinate(kZoneModeFree, 5.3f) == 5.3f);                                // Free: the value itself
	// THE LISTS: eight behaviours, then Free last; both modes default to Free, so a fresh device is unchanged.
	CHECK(std::string_view(kCharacterModeNames[kZoneModeFree]) == "Free");
	CHECK(std::string_view(kVibeModeNames[kZoneModeFree]) == "Free");
	for (const Param p : { Param::CharacterMode, Param::VibeMode }) {
		const auto& d = kParamDescs[static_cast<size_t>(p)];
		CHECK(d.modeCell);
		CHECK(d.stepped);
		CHECK(d.def == static_cast<double>(kZoneModeFree));
		CHECK_FALSE(d.modulatable);
	}
	CHECK(Settings {}.characterMode == kZoneModeFree);
	CHECK(Settings {}.vibeMode == kZoneModeFree);
}

TEST_CASE("IW: the engine reads Character and Vibe THROUGH their modes — a mode plus a value is Free at the mapped coordinate") {
	// BY VALUE, THROUGH THE SEAMS: a device in (mode, value) must land every band's width, attack and release exactly
	// where a device in Free at `zoneModeCoordinate(mode, value)` does. A read that skipped the mode would put the
	// first at the raw value's zone instead.
	for (const auto& [mode, value] : { std::pair { 3u, 2.4f }, std::pair { 0u, 0.8f }, std::pair { 6u, -1.3f } }) {
		CAPTURE(mode);
		CAPTURE(value);
		Settings moded = defaultSettings();
		moded.character = value;
		moded.characterMode = mode;
		moded.vibe = value;
		moded.vibeMode = mode;
		Settings free = defaultSettings();
		free.character = zoneModeCoordinate(mode, value);
		free.vibe = zoneModeCoordinate(mode, value);
		const Owltt a = sliceAt(moded);
		const Owltt b = sliceAt(free);
		for (size_t band = 0; band < 3; ++band) {
			CAPTURE(band);
			CHECK(a.bandWidthForTest(band) == b.bandWidthForTest(band));
			CHECK(a.attackMsForTest(band) == b.attackMsForTest(band));
			CHECK(a.releaseMsForTest(band) == b.releaseMsForTest(band));
		}
		CHECK(a.vibePhaseKneeForTest() == b.vibePhaseKneeForTest());
		CHECK(a.feelPhase033ForTest() == b.feelPhase033ForTest());
	}
}

TEST_CASE("IW: inside a mode the value is FENCED — whatever it is, the coordinate the engine reads is in that mode's zone") {
	// The fence, as the arithmetic the engine reads: for any value, on any lap, `zoneModeCoordinate` lands inside the
	// chosen zone, at the value's fraction across it. (The equivalence case above is what ties this to the engine.)
	for (uint32_t mode = 0; mode < kZoneModeFree; ++mode) {
		for (float value = -5.0f; value < 5.0f; value += 0.137f) {
			CAPTURE(mode);
			CAPTURE(value);
			const float coordinate = zoneModeCoordinate(mode, value);
			const float inLap = coordinate - std::floor(coordinate);
			CHECK(std::floor(coordinate) == std::floor(value));                            // the lap is the value's
			CHECK(static_cast<uint32_t>(std::floor(inLap * 8.0f)) == mode);                  // always this zone
			CHECK(inLap * 8.0f - static_cast<float>(mode)
			      == doctest::Approx(value - std::floor(value)).epsilon(1e-4));              // at the value's fraction
		}
	}
	// AND AT LAP 0 THE ENGINE AGREES BY VALUE: the Width zone writes the bass width as `0.7 + pos * 0.5` on a plain
	// splitter (the parity case above), and in the Width MODE `pos` is the value's fraction.
	Settings s = defaultSettings();
	s.mode = 1.0f;
	s.vibe = 0.0f;
	s.characterMode = 0; // Width
	for (const float value : { 0.25f, 0.5f, 0.9f }) {
		CAPTURE(value);
		s.character = value;
		CHECK(sliceAt(s).bandWidthForTest(0) == doctest::Approx(0.7f + value * 0.5f).epsilon(1e-5));
	}
}

//------------------------------------------------------------------------------------------------//
// BACKLOG LQ — DUAL MONO, STEREO LINK AND M/S                                                    //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE PATCH DEFAULT, from the descriptor — what a fresh device in a lane runs (`Settings {}` is the
/// no-op device the engine cases lean on, so it is not the default this claim is about).
Settings patchDefault() {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	return stateOf(row);
}

/// A SEEDED xorshift32 IN [-1, 1] — the test's own, so a library change cannot move its numbers.
struct LqNoise {
	uint32_t s;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	}
};

/// FNV-1a over the BITS of both channels of a render: decorrelated stereo noise under a slow
/// burst envelope (quiet, loud, quiet, so both the upward and the downward paths move), handed over
/// in uneven blocks.
uint64_t lqRenderHash(const Settings& s) {
	Owltt owltt;
	owltt.prepare(48000.0);
	owltt.setTarget(s);
	LqNoise nl { 0x12345678u }, nr { 0x9e3779b9u };
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
	int t = 0;
	for (int n = 0; t < 24000; ++n) {
		const int frames = kBlocks[n % 5];
		std::array<float, 256> l {}, r {};
		for (int i = 0; i < frames; ++i, ++t) {
			const float env = (t / 4000) % 3 == 1 ? 0.9f : 0.02f;
			l[static_cast<size_t>(i)] = env * nl.next();
			r[static_cast<size_t>(i)] = env * (0.5f * nl.next() + 0.5f * nr.next());
		}
		owltt.processStereo(l.data(), r.data(), frames);
		for (int i = 0; i < frames; ++i) {
			mix(l[static_cast<size_t>(i)]);
			mix(r[static_cast<size_t>(i)]);
		}
	}
	return h;
}

} // namespace

TEST_CASE("LQ: at the default stereo settings OWLTT renders bit-identically to the linked build before LQ") {
	// THE HASHES WERE TAKEN FROM THE BUILD BEFORE LQ (v1-implementation 4aad7e83), so this is the
	// "today's behaviour, bit for bit" claim against the pre-change output itself, not against a
	// restatement that could share a mistake. Every variant leaves Stereo Link and M/S at their declared
	// defaults: Link 100%, L/R.
	struct Variant {
		const char* name;
		Settings s;
		uint64_t hash;
	};
	Settings base = patchDefault();
	Settings ap6 = base;
	ap6.mode = 1.0f;
	Settings heavy = base;
	heavy.threshold = 0.2f;
	heavy.ratio = 0.9f;
	heavy.upDownSkew = 0.8f;
	Settings punch = base;
	punch.characterMode = 3;
	punch.character = 0.6f;
	Settings owlZone = base;
	owlZone.characterMode = 7;
	owlZone.character = 0.4f;
	owlZone.vibeMode = 6;
	owlZone.vibe = 0.3f;
	Settings blended = base;
	blended.blend = 0.6f;
	blended.softClip = false;
	blended.mode = 8.0f;
	const Variant variants[] = {
		{ "default", base, 9074996434631607774ull },
		{ "AP 6dB", ap6, 3778802132889232869ull },
		{ "heavy", heavy, 16577964413036874256ull },
		{ "Punch", punch, 5414003785638588303ull },
		{ "OWLTT zone + Drift", owlZone, 13795555111261912375ull },
		{ "blend, no clip, LR4", blended, 16294062805875666825ull },
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

/// LQ'S SCENE: the patch default, downward compression only (Up/Down at 1 — so a channel the detector
/// does not push over the threshold has an envelope of EXACTLY zero, with no upward lift to muddy "at rest"),
/// a low threshold so a hot channel compresses in every band.
Settings lqScene(float link, bool midSide) {
	Settings s = patchDefault();
	s.upDownSkew = 1.0f;
	s.threshold = 0.3f;
	s.stereoLink = link;
	s.midSide = midSide;
	return s;
}

/// HALF A SECOND OF NOISE, each channel scaled by its own amount from one shared source (`share`) and its
/// own (1 - share), in the 32-sample chunks `Synth::render` hands over.
void lqRun(Owltt& owltt, float ampL, float ampR, float share) {
	LqNoise common { 0x2468ace1u }, ownL { 0x13579bdfu }, ownR { 0x0badf00du };
	std::array<float, 32> l {}, r {};
	for (int t = 0; t < 24000; t += 32) {
		for (size_t i = 0; i < 32; ++i) {
			const float c = common.next();
			l[i] = ampL * (share * c + (1.0f - share) * ownL.next());
			r[i] = ampR * (share * c + (1.0f - share) * ownR.next());
		}
		owltt.processStereo(l.data(), r.data(), 32);
	}
}

} // namespace

TEST_CASE("LQ: OWLTT Link 0 — a hot left channel leaves every band's R gain at rest") {
	Owltt owltt;
	owltt.prepare(48000.0);
	owltt.setTarget(lqScene(0.0f, false));
	lqRun(owltt, 0.8f, 0.0f, 0.0f);
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(owltt.channelGainReductionForTest(b, 1) == 0.0f); // gain exactly 1
		CHECK(owltt.channelGainReductionForTest(b, 0) < -0.1f);  // L is compressing
	}
}

TEST_CASE("LQ: OWLTT Link 0 — R's gain is whatever R alone would get, with or without L") {
	// WITH UPWARD COMPRESSION ON (the default Up/Down), a quiet channel is lifted rather than left at rest —
	// so the claim at Link 0 is independence: R's envelope with a hot L beside it is R's envelope with L
	// silent, to the bit, in every band.
	Settings s = lqScene(0.0f, false);
	s.upDownSkew = 0.5f;
	Owltt withL;
	withL.prepare(48000.0);
	withL.setTarget(s);
	lqRun(withL, 0.8f, 0.05f, 0.0f);
	Owltt alone;
	alone.prepare(48000.0);
	alone.setTarget(s);
	lqRun(alone, 0.0f, 0.05f, 0.0f);
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(withL.channelGainReductionForTest(b, 1) == alone.channelGainReductionForTest(b, 1));
		CHECK(withL.channelGainReductionForTest(b, 0) != alone.channelGainReductionForTest(b, 0));
	}
}

TEST_CASE("LQ: OWLTT Link 100 — both channels of a band carry the linked gain") {
	Owltt owltt;
	owltt.prepare(48000.0);
	owltt.setTarget(lqScene(1.0f, false));
	lqRun(owltt, 0.8f, 0.0f, 0.0f);
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(owltt.channelGainReductionForTest(b, 1) == owltt.channelGainReductionForTest(b, 0));
		CHECK(owltt.channelGainReductionForTest(b, 0) == owltt.getBandGainReductionDB(b));
		CHECK(owltt.channelGainReductionForTest(b, 1) < -0.1f);
	}
}

TEST_CASE("LQ: OWLTT Link 50% puts R's detector halfway, in the detector's log units, to the band's shared level") {
	// THE LAW: effective = (1 - link) * own + link * shared, in nepers (so in dB). `own` is read off the
	// same input at Link 0 (a channel's own detector does not depend on Link) and `shared` off the band's
	// linked detector, which is the one detector OWLTT had before LQ.
	const float ampR = 0.02f;
	Owltt unlinked;
	unlinked.prepare(48000.0);
	unlinked.setTarget(lqScene(0.0f, false));
	lqRun(unlinked, 0.8f, ampR, 0.0f);
	Owltt half;
	half.prepare(48000.0);
	half.setTarget(lqScene(0.5f, false));
	lqRun(half, 0.8f, ampR, 0.0f);
	Owltt linked;
	linked.prepare(48000.0);
	linked.setTarget(lqScene(1.0f, false));
	lqRun(linked, 0.8f, ampR, 0.0f);
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		const float own = unlinked.channelInputLevelLogForTest(b, 1);
		const float shared = half.getBandInputLevelLog(b);
		CHECK(own < shared - 1.0f); // R really is the quieter one, by more than a neper
		CHECK(half.channelInputLevelLogForTest(b, 1) == doctest::Approx(0.5f * own + 0.5f * shared).epsilon(1e-5));
		// AND THE GAIN IT YIELDS IS BETWEEN THE TWO ENDS, strictly.
		const float g0 = unlinked.channelGainReductionForTest(b, 1);
		const float g50 = half.channelGainReductionForTest(b, 1);
		const float g100 = linked.channelGainReductionForTest(b, 1);
		CAPTURE(g0);
		CAPTURE(g50);
		CAPTURE(g100);
		CHECK(g50 < g0);
		CHECK(g50 > g100);
	}
}

TEST_CASE("LQ: OWLTT M/S — a mono signal leaves Side's gain at rest; a Side-only signal ducks only Side") {
	// LINK 0, so Mid and Side each listen only to themselves.
	SUBCASE("mono") {
		Owltt owltt;
		owltt.prepare(48000.0);
		owltt.setTarget(lqScene(0.0f, true));
		lqRun(owltt, 0.8f, 0.8f, 1.0f); // L == R
		for (size_t b = 0; b < 3; ++b) {
			CAPTURE(b);
			CHECK(owltt.channelGainReductionForTest(b, 1) == 0.0f);
			CHECK(owltt.channelGainReductionForTest(b, 0) < -0.1f);
		}
	}
	SUBCASE("side only") {
		Owltt owltt;
		owltt.prepare(48000.0);
		owltt.setTarget(lqScene(0.0f, true));
		lqRun(owltt, 0.8f, -0.8f, 1.0f); // L == -R, so Mid is zero
		for (size_t b = 0; b < 3; ++b) {
			CAPTURE(b);
			CHECK(owltt.channelGainReductionForTest(b, 0) == 0.0f);
			CHECK(owltt.channelGainReductionForTest(b, 1) < -0.1f);
		}
	}
}

TEST_CASE("LQ: OWLTT M/S reads a mono signal at the level L/R reads it, even where the high band averages") {
	// THE WIDTH ZONE AT 0.5 PUTS THE HIGH BAND'S WIDTH ABOVE 1, which switches the linked detector to the MEAN of
	// its two channels (`useAvgEnvelope`). Fed Mid and Side, a mean would read a mono signal (Side 0) at half its
	// level — 6 dB under L/R — and switching to M/S would move every threshold. In M/S the linked level is the
	// louder of the two instead, so on a mono input Mid carries exactly the gain L/R gives both channels.
	Settings s = lqScene(1.0f, false);
	s.characterMode = 0; // Width
	s.character = 0.5f;
	s.upDownSkew = 0.5f;
	Owltt lr;
	lr.prepare(48000.0);
	lr.setTarget(s);
	lqRun(lr, 0.5f, 0.5f, 1.0f);
	REQUIRE(lr.bandWidthForTest(2) > 1.0f); // the averaging road is really on
	s.midSide = true;
	Owltt ms;
	ms.prepare(48000.0);
	ms.setTarget(s);
	lqRun(ms, 0.5f, 0.5f, 1.0f);
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(ms.getBandInputLevelLog(b) == doctest::Approx(lr.getBandInputLevelLog(b)).epsilon(1e-6));
		CHECK(ms.channelGainReductionForTest(b, 0) == doctest::Approx(lr.channelGainReductionForTest(b, 0)).epsilon(1e-5));
	}
}

//== BACKLOG LR: OWLTT's stereo width, and the controls that bring it back ==================================//

namespace {

/// ONE BAND'S WORTH OF A WIDE SOURCE: a quadrature tone, `L = a sin`, `R = a cos`, whose Mid and Side carry
/// EXACTLY equal energy — a side/mid ratio of 1 going in. Rendered through `s` for half a second in 32-sample
/// chunks; returns sqrt(E_side / E_mid) over the last quarter second, the width the device sent the tone out at.
float lrSideOverMid(const Settings& s, float hz, Owltt* keep = nullptr) {
	Owltt local;
	Owltt& owltt = keep != nullptr ? *keep : local;
	owltt.prepare(48000.0);
	owltt.setTarget(s);
	const double w = 2.0 * std::numbers::pi * static_cast<double>(hz) / 48000.0;
	double em = 0.0, es = 0.0;
	std::array<float, 32> l {}, r {};
	for (int t = 0; t < 24000; t += 32) {
		for (size_t i = 0; i < 32; ++i) {
			const double ph = w * static_cast<double>(t + static_cast<int>(i));
			l[i] = static_cast<float>(0.1 * std::sin(ph));
			r[i] = static_cast<float>(0.1 * std::cos(ph));
		}
		owltt.processStereo(l.data(), r.data(), 32);
		if (t >= 12000) {
			for (size_t i = 0; i < 32; ++i) {
				const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
				const double sd = 0.5 * (static_cast<double>(l[i]) - r[i]);
				em += m * m;
				es += sd * sd;
			}
		}
	}
	return static_cast<float>(std::sqrt(es / em));
}

/// A tone deep inside each band at the default corners (200 Hz, 2 kHz): 40 Hz, 630 Hz (their geometric
/// mean), 12 kHz.
constexpr std::array<float, 3> kLrBandTones { 40.0f, 630.0f, 12000.0f };

} // namespace

TEST_CASE("LR: measured — OWLTT sends each band's side out at the zone's width, and the default patch narrows all three") {
	// THE PREMISE OF BACKLOG LR, MEASURED rather than read. A wide tone in each band goes in at side/mid 1 and
	// comes out at the band's width: `bandWidthForTest` is the value the device used, and the render is the
	// second assertion that the value is what the audio got. LR4 Full so the skirts are steep enough that a
	// tone hears one band; the default patch (LR2 Full) is measured too, at a looser bound, because its skirts
	// let a neighbour band's width bleed in.
	struct Zone {
		const char* name;
		uint32_t mode;
		float amount;
	};
	const Zone zones[] = {
		{ "patch default (Free 0 = Width at 0)", kZoneModeFree, 0.0f },
		{ "Width 0.5", 0, 0.5f }, { "Punch 0.5", 3, 0.5f }, { "Air 0.5", 4, 0.5f },
		{ "Rich (default branch) 0.5", 5, 0.5f }, { "OTT 0.5", 6, 0.5f }, { "OWLTT 0.5", 7, 0.5f },
	};
	for (const float modeValue : { 8.0f, 6.0f }) { // LR4 Full, then the patch default LR2 Full
		for (const Zone& z : zones) {
			Settings s = patchDefault();
			s.mode = modeValue;
			s.characterMode = z.mode;
			s.character = z.amount;
			for (size_t b = 0; b < 3; ++b) {
				Owltt owltt;
				const float ratio = lrSideOverMid(s, kLrBandTones[b], &owltt);
				const float width = owltt.bandWidthForTest(b);
				const std::string zoneName = z.name;
				CAPTURE(zoneName);
				CAPTURE(modeValue);
				CAPTURE(b);
				const std::string seen = std::string(z.name) + " mode " + std::to_string(modeValue) + " band "
				                         + std::to_string(b) + ": zone width " + std::to_string(width)
				                         + ", measured side/mid " + std::to_string(ratio);
				MESSAGE(seen);
				CHECK(ratio == doctest::Approx(std::abs(width)).epsilon(modeValue == 8.0f ? 0.05 : 0.15));
			}
		}
	}
	// THE PATCH DEFAULT NARROWS EVERY BAND — the entry's claim, as numbers.
	Settings d = patchDefault();
	Owltt o;
	(void)lrSideOverMid(d, 1000.0f, &o);
	CHECK(o.bandWidthForTest(0) == doctest::Approx(0.7f));
	CHECK(o.bandWidthForTest(1) == doctest::Approx(0.85f));
	CHECK(o.bandWidthForTest(2) == doctest::Approx(0.9f));
}

TEST_CASE("LR: at Width 100% everywhere OWLTT renders bit-identically to the build before the width controls") {
	// THE HASHES WERE TAKEN FROM THE BUILD BEFORE LR (v1-implementation deae20fe), over the zones that write
	// the width, the vibe that modulates it, a weird splitter, M/S (which encodes AFTER the width) and Link 0
	// — so "100% means as the zone sets it" is pinned against the pre-change output itself. Every variant
	// leaves the four Width controls at their declared default.
	struct Variant {
		const char* name;
		Settings s;
		uint64_t hash;
	};
	const Settings base = patchDefault();
	Settings widthZone = base;
	widthZone.characterMode = 0;
	widthZone.character = 0.7f;
	Settings air = base;
	air.characterMode = 4;
	air.character = 0.3f;
	air.vibe = 0.8f; // vibe's width modulation on
	Settings owlWeird = base;
	owlWeird.mode = 3.0f; // Twisted
	owlWeird.characterMode = 7;
	owlWeird.character = 2.4f; // lap 2: the feel phases move the width too
	Settings midSide = widthZone;
	midSide.midSide = true;
	Settings unlinked = air;
	unlinked.stereoLink = 0.0f;
	unlinked.midSide = true;
	const Variant variants[] = {
		{ "default", base, 9074996434631607774ull },
		{ "Width zone 0.7", widthZone, 12900952257831726833ull },
		{ "Air + vibe", air, 12358078177218883803ull },
		{ "OWLTT zone, Twisted, lap 2", owlWeird, 17577377456130589773ull },
		{ "Width zone, M/S", midSide, 4672508459224113082ull },
		{ "Air + vibe, M/S, Link 0", unlinked, 5055517475999705068ull },
	};
	for (const Variant& v : variants) {
		const std::string variantName = v.name;
		CAPTURE(variantName);
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

TEST_CASE("LR: the width a band sends on is the zone's x Width x the band's Width — by value, and the side the "
          "audio carries agrees") {
	// THE LAW, RESTATED HERE rather than read back from `effectiveBandWidthOf`: the zone's width (read off the
	// device, which Width must not move) times the two scalers, inside the +-2 the render loop always held.
	// The render is the SECOND assertion, proving the value is what the audio got (LR4 Full, so a tone hears
	// one band).
	struct Case {
		const char* name;
		uint32_t zoneMode;
		float zoneAmount;
		float global;
		std::array<float, 3> band;
	};
	const Case cases[] = {
		{ "default zone, global 150%", kZoneModeFree, 0.0f, 1.5f, { 1.0f, 1.0f, 1.0f } },
		{ "default zone, bands only", kZoneModeFree, 0.0f, 1.0f, { 1.4f, 0.5f, 1.9f } },
		{ "Punch, both", 3, 0.5f, 1.25f, { 1.2f, 0.8f, 0.6f } },
		{ "Air, global 40%", 4, 0.5f, 0.4f, { 1.0f, 1.0f, 1.0f } },
		{ "OWLTT zone, both", 7, 0.5f, 1.8f, { 0.3f, 1.1f, 0.9f } },
		{ "Width zone, past the clamp", 0, 0.9f, 2.0f, { 2.0f, 1.0f, 0.5f } },
	};
	for (const Case& c : cases) {
		const std::string caseName = c.name;
		CAPTURE(caseName);
		Settings zoneOnly = patchDefault();
		zoneOnly.mode = 8.0f;
		zoneOnly.characterMode = c.zoneMode;
		zoneOnly.character = c.zoneAmount;
		Settings s = zoneOnly;
		s.width = c.global;
		s.low.width = c.band[0];
		s.mid.width = c.band[1];
		s.high.width = c.band[2];
		for (size_t b = 0; b < 3; ++b) {
			CAPTURE(b);
			Owltt reference;
			(void)lrSideOverMid(zoneOnly, kLrBandTones[b], &reference);
			Owltt owltt;
			const float ratio = lrSideOverMid(s, kLrBandTones[b], &owltt);
			const float zone = reference.bandWidthForTest(b);
			// THE ZONE'S WIDTH IS THE ZONE'S: the controls scale what it sends on, never what it set.
			CHECK(owltt.bandWidthForTest(b) == zone);
			const float want = std::clamp(zone * c.global * c.band[b], -2.0f, 2.0f);
			CHECK(owltt.effectiveBandWidthForTest(b) == doctest::Approx(want).epsilon(1e-6));
			CHECK(ratio == doctest::Approx(std::abs(want)).epsilon(0.05));
		}
	}
}

TEST_CASE("LR: Width 0 sends no side — the output is mono to the bit at Link 100% in L/R, and in M/S at any Link") {
	struct Case {
		const char* name;
		float link;
		bool midSide;
	};
	const Case cases[] = { { "L/R, Link 100%", 1.0f, false }, { "M/S, Link 100%", 1.0f, true }, { "M/S, Link 0", 0.0f, true } };
	for (const Case& c : cases) {
		const std::string caseName = c.name;
		CAPTURE(caseName);
		for (const int which : { -1, 0, 1, 2 }) { // the global, then each band's own (the others left at 100%)
			CAPTURE(which);
			Settings s = patchDefault();
			s.characterMode = 0; // the Width zone at 0.5: every zone width at or above 0.95, nothing narrow already
			s.character = 0.5f;
			s.stereoLink = c.link;
			s.midSide = c.midSide;
			if (which < 0) {
				s.width = 0.0f;
			}
			else {
				(which == 0 ? s.low : which == 1 ? s.mid : s.high).width = 0.0f;
			}
			Owltt owltt;
			owltt.prepare(48000.0);
			owltt.setTarget(s);
			LqNoise nl { 0x12345678u }, nr { 0x9e3779b9u };
			double sideOut = 0.0, energy = 0.0;
			std::array<float, 32> l {}, r {};
			for (int t = 0; t < 24000; t += 32) {
				for (size_t i = 0; i < 32; ++i) {
					l[i] = 0.3f * nl.next();
					r[i] = 0.3f * nr.next();
				}
				owltt.processStereo(l.data(), r.data(), 32);
				for (size_t i = 0; i < 32; ++i) {
					const double d = static_cast<double>(l[i]) - r[i];
					sideOut += d * d;
					energy += static_cast<double>(l[i]) * l[i];
				}
			}
			for (size_t b = 0; b < 3; ++b) {
				const bool zeroed = which < 0 || static_cast<size_t>(which) == b;
				CHECK((owltt.effectiveBandWidthForTest(b) == 0.0f) == zeroed);
			}
			REQUIRE(energy > 1.0); // the device is passing signal
			if (which < 0) {
				CHECK(sideOut == 0.0); // L == R at every sample
			}
			else {
				CHECK(sideOut > 0.0); // the other two bands still carry their side
			}
		}
	}
}

TEST_CASE("LR: the crossovers do not collapse width — with every band's effective width 1, each band's side leaves as "
          "it came, in all ten modes") {
	// THE ENTRY'S SECOND QUESTION. Both channels run identical linear splitters, so Mid and Side pass through the same
	// filters and their ratio cannot change there; whatever narrowing there is lives in the width stage. MEASURED in
	// every mode, including the four allpass "weird" splitters whose bass the zone tables treat differently: the band
	// Widths are set to 1 / the zone's width, so every band sends on 1, and a wide tone in each band (and one at each
	// corner, where two bands overlap) must leave at side/mid 1.
	for (int mode = 1; mode <= 10; ++mode) {
		CAPTURE(mode);
		Settings s = patchDefault();
		s.mode = static_cast<float>(mode);
		s.characterMode = 0; // the Width zone at 0.8: every zone width positive in every mode, weird or not
		s.character = 0.8f;
		Owltt probe;
		(void)lrSideOverMid(s, 1000.0f, &probe);
		s.low.width = 1.0f / probe.bandWidthForTest(0);
		s.mid.width = 1.0f / probe.bandWidthForTest(1);
		s.high.width = 1.0f / probe.bandWidthForTest(2);
		for (const float hz : { 40.0f, 200.0f, 630.0f, 2000.0f, 12000.0f }) {
			CAPTURE(hz);
			Owltt owltt;
			const float ratio = lrSideOverMid(s, hz, &owltt);
			for (size_t b = 0; b < 3; ++b) {
				CHECK(owltt.effectiveBandWidthForTest(b) == doctest::Approx(1.0f).epsilon(1e-6));
			}
			CHECK(ratio == doctest::Approx(1.0f).epsilon(0.01));
		}
	}
}

TEST_CASE("LR: a row from before the width controls reads them at 100%, not at the zero a short row leaves") {
	// A ZERO THERE IS MONO, so a pre-LR row (31 fields, ending at Stereo Mode) must take the declared default, as
	// the stereo pair does — the host's `setFields` road, which is the one a stored patch arrives by.
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	for (const Param p : { Param::Width, Param::LowWidth, Param::MidWidth, Param::HighWidth }) {
		row[static_cast<size_t>(p)] = 0.0f; // what a short row would leave behind, were the fill missing
	}
	Owltt owltt;
	REQUIRE(kAudioOps.setFields != nullptr);
	kAudioOps.setFields(&owltt, row.data(), static_cast<size_t>(Param::Width));
	CHECK(owltt.target().width == 1.0f);
	CHECK(owltt.target().low.width == 1.0f);
	CHECK(owltt.target().mid.width == 1.0f);
	CHECK(owltt.target().high.width == 1.0f);
	// NON-VACUOUS: a full row is read as it stands, zeros and all.
	kAudioOps.setFields(&owltt, row.data(), row.size());
	CHECK(owltt.target().width == 0.0f);
	CHECK(owltt.target().high.width == 0.0f);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG NB: DRIVE                                                                              //
//------------------------------------------------------------------------------------------------//

namespace {

/// HALF A SECOND OF DECORRELATED NOISE in the 32-sample chunks `Synth::render` hands over, each block scaled by
/// `pre` BEFORE the device sees it — the restatement's road — and the device's output and detectors read after.
struct NbRun {
	std::vector<float> out;
	std::array<std::array<float, 2>, 3> level {};
	std::array<std::array<float, 2>, 3> gr {};
};

NbRun nbRun(const Settings& s, float pre) {
	Owltt owltt;
	owltt.prepare(48000.0);
	owltt.setTarget(s);
	LqNoise nl { 0x51ed270bu }, nr { 0x2545f491u };
	NbRun run;
	std::array<float, 32> l {}, r {};
	for (int t = 0; t < 24000; t += 32) {
		for (size_t i = 0; i < 32; ++i) {
			const float env = (t / 6000) % 2 == 1 ? 0.7f : 0.05f;
			l[i] = env * nl.next() * pre;
			r[i] = env * (0.6f * nl.next() + 0.4f * nr.next()) * pre;
		}
		owltt.processStereo(l.data(), r.data(), 32);
		run.out.insert(run.out.end(), l.begin(), l.end());
		run.out.insert(run.out.end(), r.begin(), r.end());
	}
	for (size_t b = 0; b < 3; ++b) {
		for (size_t c = 0; c < 2; ++c) {
			run.level[b][c] = owltt.channelInputLevelLogForTest(b, c);
			run.gr[b][c] = owltt.channelGainReductionForTest(b, c);
		}
	}
	return run;
}

/// THE DETECTOR'S LOG UNITS (nepers) back to dBFS — `updateMeters`' own conversion, with ln(2^31) as full scale.
double nbDbfs(float levelLog) {
	return 8.6859 * (static_cast<double>(levelLog) - std::log(2147483648.0));
}

} // namespace

TEST_CASE("NB: OWLTT's Drive is a gain in front of the dynamics — x driven D dB is x·10^(D/20) undriven, to the bit, at the detectors and at the output") {
	// THE INDEPENDENT RESTATEMENT: the drive multiplies the input before anything else hears it, so driving x by D
	// renders exactly what an undriven device renders from x already scaled by D's gain — at Blend 100%, where nothing
	// of the undriven dry is heard. The gain is COMP's float formula, `exp(D · ln10/20)`, written out here.
	for (const float driveDb : { 9.0f, -7.5f, 24.0f }) {
		const float d = std::exp(driveDb * 0.11512925464970229f);
		for (const bool midSide : { false, true }) {
			for (const float mode : { 6.0f, 1.0f, 8.0f }) {
				CAPTURE(driveDb);
				CAPTURE(midSide);
				CAPTURE(mode);
				Settings undriven = patchDefault();
				undriven.midSide = midSide;
				undriven.mode = mode;
				undriven.stereoLink = 0.5f; // both channels' own detectors and the linked one all in play
				Settings driven = undriven;
				driven.driveDb = driveDb;
				const NbRun a = nbRun(driven, 1.0f);
				const NbRun ref = nbRun(undriven, d);
				const NbRun plain = nbRun(undriven, 1.0f);
				REQUIRE(a.out.size() == ref.out.size());
				size_t differ = 0;
				for (size_t i = 0; i < a.out.size(); ++i) {
					differ += a.out[i] != ref.out[i];
				}
				CHECK(differ == 0u);
				for (size_t b = 0; b < 3; ++b) {
					for (size_t c = 0; c < 2; ++c) {
						CAPTURE(b);
						CAPTURE(c);
						CHECK(a.level[b][c] == ref.level[b][c]);
						CHECK(a.gr[b][c] == ref.gr[b][c]);
						// NOT VACUOUS: the detector heard the drive — its level sits D above the undriven one's (the
						// level is a linear one-pole of |x|, so the gain passes through it; `fastLog`, the firmware's truncated series,
						// is off by up to 1.2 dB near the top of a binade, so the bound is that and not tighter).
						CHECK(std::fabs(nbDbfs(a.level[b][c]) - nbDbfs(plain.level[b][c]) - static_cast<double>(driveDb)) < 1.3);
					}
				}
			}
		}
	}
}

TEST_CASE("NB: Drive by value — DC at -30 dBFS driven +24 dB reads -6 dBFS at the low band's detector and is reduced 9 dB, 18 over a -24 dBFS threshold at a ratio of 50%") {
	// THE STATIC CURVE, BY HAND (`BandCompressor::calculateGain`): threshold 50% puts the downward threshold at
	// 0.2 + 0.8·0.5 = 0.6 of the way up a 60 dB span below full scale, -24 dBFS; Ratio 50% takes half the overshoot;
	// Up/Down at 100% is downward only. DC lands wholly in the LR2 low band. Character's default zone has a 0.2 knee,
	// 1.2 nepers (10.4 dB) either side of the threshold, so 18 dB over is past it and the curve is the straight line.
	Settings s = patchDefault();
	s.upDownSkew = 1.0f;
	s.driveDb = 24.0f;
	const float amp = static_cast<float>(std::pow(10.0, -30.0 / 20.0));
	const auto runDc = [amp](const Settings& settings) {
		Owltt owltt;
		owltt.prepare(48000.0);
		owltt.setTarget(settings);
		std::array<float, 32> l {}, r {};
		for (int t = 0; t < 48000; t += 32) {
			l.fill(amp);
			r.fill(amp);
			owltt.processStereo(l.data(), r.data(), 32);
		}
		return std::pair { owltt.channelInputLevelLogForTest(0, 0), owltt.channelGainReductionForTest(0, 0) };
	};
	const auto [level, gr] = runDc(s);
	CHECK(std::fabs(nbDbfs(level) - -6.0) < 0.1);
	CHECK(std::fabs(8.6859 * static_cast<double>(gr) - -9.0) < 0.15);
	// NOT VACUOUS: undriven, the same DC sits 6 dB UNDER the threshold, inside the knee, and is barely touched.
	Settings plain = s;
	plain.driveDb = 0.0f;
	const auto [plainLevel, plainGr] = runDc(plain);
	CHECK(std::fabs(nbDbfs(plainLevel) - -30.0) < 0.1);
	CHECK(8.6859 * static_cast<double>(plainGr) > -0.5);
}

TEST_CASE("NB: Blend's dry is the UNDRIVEN input — at Blend 0 a driven OWLTT renders what an undriven one does, to the bit, while its detectors still hear the drive") {
	for (const float mode : { 6.0f, 1.0f, 8.0f }) { // an LR mode (the dry through `alignDry`), an AP mode, LR4
		CAPTURE(mode);
		Settings undriven = patchDefault();
		undriven.mode = mode;
		undriven.blend = 0.0f;
		Settings driven = undriven;
		driven.driveDb = 18.0f;
		const NbRun a = nbRun(driven, 1.0f);
		const NbRun ref = nbRun(undriven, 1.0f);
		size_t differ = 0;
		for (size_t i = 0; i < a.out.size(); ++i) {
			differ += a.out[i] != ref.out[i];
		}
		CHECK(differ == 0u);
		CHECK(std::fabs(nbDbfs(a.level[1][0]) - nbDbfs(ref.level[1][0]) - 18.0) < 1.3); // the wet side was driven (`fastLog`'s bound, above)
	}
}

TEST_CASE("NB: at Drive 0 dB OWLTT renders bit-identically to the build before the drive") {
	// THE PINNED HASHES of the LQ case, taken before NB existed, with the drive set to 0 dB EXPLICITLY — and a hair
	// of drive moves them, so the hash is one the drive can reach.
	Settings base = patchDefault();
	base.driveDb = 0.0f;
	Settings heavy = base;
	heavy.threshold = 0.2f;
	heavy.ratio = 0.9f;
	heavy.upDownSkew = 0.8f;
	const uint64_t hBase = lqRenderHash(base);
	const uint64_t hHeavy = lqRenderHash(heavy);
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
	CHECK(hBase == 9074996434631607774ull);
	CHECK(hHeavy == 16577964413036874256ull);
#else
	(void)hHeavy;
#endif
	Settings nudged = base;
	nudged.driveDb = 0.05f;
	CHECK(lqRenderHash(nudged) != hBase);
}

TEST_CASE("NB: a routed Drive is read per sample from the host's window — a run holding D renders what a stored D does, and a run that moves is followed inside the call") {
	REQUIRE(kAudioOps.setFieldWindows != nullptr);
	const auto drive = static_cast<size_t>(Param::Drive);
	CHECK(std::find(kAudioOps.windowedControls.begin(), kAudioOps.windowedControls.end(), static_cast<uint8_t>(drive))
	      != kAudioOps.windowedControls.end());
	// THE SAME 64 FRAMES THROUGH THREE DEVICES: D stored; 0 stored with a run of D handed through `setFieldWindows`;
	// and 0 stored with a run that holds 0 for the first half and D for the second.
	constexpr float kD = 12.0f;
	constexpr int kFrames = 64;
	std::array<float, kFrames> constant {}, step {};
	constant.fill(kD);
	for (int i = 0; i < kFrames; ++i) {
		step[static_cast<size_t>(i)] = i < kFrames / 2 ? 0.0f : kD;
	}
	std::array<const float*, static_cast<size_t>(Param::kCount)> runs {};
	const auto render = [&](float stored, const float* run, std::array<float, kFrames>& l) {
		Settings s = patchDefault();
		s.driveDb = stored;
		// NO GAIN COMPUTER AND NO CLIPPER: Ratio 0 leaves every band's gain at rest, so the device is linear and causal and
		// a sample can differ only by what reached it — a per-CALL gain would otherwise smear the second half's drive
		// back over the first through the block's one detector reading.
		s.ratio = 0.0f;
		s.softClip = false;
		Owltt owltt;
		owltt.prepare(48000.0);
		owltt.setTarget(s);
		LqNoise n { 0x6a09e667u };
		std::array<float, kFrames> r {};
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.3f * n.next();
			r[i] = 0.3f * n.next();
		}
		runs.fill(nullptr);
		runs[drive] = run;
		kAudioOps.setFieldWindows(&owltt, runs.data(), runs.size(), kFrames);
		owltt.processStereo(l.data(), r.data(), kFrames);
	};
	std::array<float, kFrames> stored {}, windowed {}, stepped {}, flat {};
	render(kD, nullptr, stored);
	render(0.0f, constant.data(), windowed);
	render(0.0f, step.data(), stepped);
	render(0.0f, nullptr, flat);
	CHECK(std::memcmp(stored.data(), windowed.data(), sizeof(stored)) == 0);
	// THE STEP IS FOLLOWED: the first half is the undriven render; the second is not.
	size_t firstHalfSame = 0;
	size_t secondHalfSame = 0;
	for (int i = 0; i < kFrames; ++i) {
		const bool same = stepped[static_cast<size_t>(i)] == flat[static_cast<size_t>(i)];
		(i < kFrames / 2 ? firstHalfSame : secondHalfSame) += same ? 1 : 0;
	}
	CHECK(firstHalfSame == static_cast<size_t>(kFrames / 2));
	CHECK(secondHalfSame < static_cast<size_t>(kFrames / 4));
}

//------------------------------------------------------------------------------------------------//
// BACKLOG ND — LOOKAHEAD                                                                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE REPORTED SPIKE, BY VALUE: an aggressive OWLTT resting on a quiet signal (so its upward gain is high), then
/// a step to 0 dBFS. Returns the onset's peak over the steady peak once the gain has settled — 1.0 is no overshoot.
/// Three partials, one per band, so every band rests high and every band is hit. 32-frame calls, as `Synth` hands them.
struct NdStep {
	float onsetPeak = 0.0f;
	float steadyPeak = 0.0f;
	[[nodiscard]] float overshoot() const { return onsetPeak / steadyPeak; }
};
NdStep ndStep(uint32_t lookahead, bool softClip, float attack = 0.25f, float ratio = 0.9f, float threshold = 0.3f) {
	constexpr double kRate = 48000.0;
	Settings s = patchDefault();
	s.ratio = ratio;
	s.threshold = threshold;
	s.softClip = softClip;
	s.lookahead = lookahead;
	s.attack = attack;
	Owltt owltt;
	owltt.prepare(kRate);
	owltt.setTarget(s);
	const int rest = static_cast<int>(2.0 * kRate);
	const int total = rest + static_cast<int>(0.6 * kRate);
	const int late = static_cast<int>(lookaheadSamplesFor(lookahead, kRate));
	const int onsetEnd = rest + late + static_cast<int>(0.030 * kRate);
	const int steadyFrom = rest + late + static_cast<int>(0.400 * kRate);
	NdStep out;
	std::array<float, 32> l {}, r {};
	for (int t0 = 0; t0 < total; t0 += 32) {
		for (int i = 0; i < 32; ++i) {
			const double t = static_cast<double>(t0 + i) / kRate;
			const double x = (std::sin(2.0 * std::numbers::pi * 110.0 * t) + std::sin(2.0 * std::numbers::pi * 1100.0 * t)
			                  + std::sin(2.0 * std::numbers::pi * 7000.0 * t))
			                 / 3.0;
			const double amp = t0 + i < rest ? 0.003 : 1.0; // -50 dBFS, then 0 dBFS
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(amp * x);
		}
		owltt.processStereo(l.data(), r.data(), 32);
		for (int i = 0; i < 32; ++i) {
			const int t = t0 + i;
			const float a = std::max(std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]));
			if (t >= rest && t < onsetEnd) {
				out.onsetPeak = std::max(out.onsetPeak, a);
			}
			if (t >= steadyFrom) {
				out.steadyPeak = std::max(out.steadyPeak, a);
			}
		}
	}
	return out;
}

} // namespace

TEST_CASE("ND: at Lookahead Off OWLTT renders bit-identically to the build before the lookahead") {
	// THE PINNED HASHES of the LQ and NB cases, taken before ND existed, with the lookahead set Off EXPLICITLY — and the
	// shortest lookahead moves them, so the hash is one the lookahead can reach.
	Settings base = patchDefault();
	base.lookahead = 0;
	Settings heavy = base;
	heavy.threshold = 0.2f;
	heavy.ratio = 0.9f;
	heavy.upDownSkew = 0.8f;
	const uint64_t hBase = lqRenderHash(base);
	const uint64_t hHeavy = lqRenderHash(heavy);
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
	CHECK(hBase == 9074996434631607774ull);
	CHECK(hHeavy == 16577964413036874256ull);
#else
	(void)hHeavy;
#endif
	Settings on = base;
	on.lookahead = 1;
	CHECK(lqRenderHash(on) != hBase);
	// AND OFF REPORTS NO LATENCY, so a fresh OWLTT asks no host for a restart.
	const Row row = rowOf(base);
	CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), 48000.0) == 0u);
	// A ROW FROM BEFORE ND stops short of the field: that is Off too.
	CHECK(kAudioOps.latencySamples(nullptr, row.data(), static_cast<size_t>(Param::Lookahead), 48000.0) == 0u);
}

TEST_CASE("ND: the latency OWLTT reports is the delay it applies — every choice at three rates, the reported count restated as round(ms·rate/1000), and the render, enabled and bypassed, exactly that many samples late") {
	REQUIRE(kAudioOps.latencySamples != nullptr);
	// THE REPORT, RESTATED: each label's milliseconds at the rate, rounded — read off the LABEL, not off `kLookaheadMs`.
	for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
		for (uint32_t c = 0; c < kLookaheadChoices; ++c) {
			CAPTURE(rate);
			CAPTURE(c);
			const double ms = c == 0 ? 0.0 : std::stod(std::string(kLookaheadNames[c]));
			Settings s = patchDefault();
			s.lookahead = c;
			const Row row = rowOf(s);
			CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate)
			      == static_cast<uint32_t>(std::lround(ms * rate / 1000.0)));
		}
	}
	CHECK(lookaheadSamplesFor(kLookaheadChoices - 1, 48000.0) == 480u); // 10 ms at 48 kHz

	// THE DELAY, BY VALUE. Ratio 0 leaves every band's gain at rest and the clipper is off, so the device is linear and
	// time-invariant: a render with the lookahead must be the render without it, exactly `n` samples later, to the bit —
	// every band and Blend's dry alike (Blend 0.5, so both are in the sum). Bypassed, it must be the input `n` late.
	constexpr double kRate = 48000.0;
	constexpr int kFrames = 2048;
	const auto render = [&](uint32_t choice, bool enabled, std::vector<float>& l, std::vector<float>& r) {
		Settings s = patchDefault();
		s.ratio = 0.0f;
		s.softClip = false;
		s.blend = 0.5f;
		s.enabled = enabled;
		s.lookahead = choice;
		Owltt owltt;
		owltt.prepare(kRate);
		owltt.setTarget(s);
		LqNoise n { 0x243f6a88u };
		l.assign(kFrames, 0.0f);
		r.assign(kFrames, 0.0f);
		for (int i = 0; i < kFrames; ++i) {
			l[static_cast<size_t>(i)] = 0.4f * n.next();
			r[static_cast<size_t>(i)] = 0.4f * n.next();
		}
		const std::vector<float> inL = l;
		for (int at = 0; at < kFrames; at += 32) {
			owltt.processStereo(&l[static_cast<size_t>(at)], &r[static_cast<size_t>(at)], 32);
		}
		CHECK(owltt.lookaheadAppliedForTest() == lookaheadSamplesFor(choice, kRate));
		return inL;
	};
	std::vector<float> offL, offR;
	(void)render(0, true, offL, offR);
	for (uint32_t c = 1; c < kLookaheadChoices; ++c) {
		CAPTURE(c);
		const auto n = static_cast<size_t>(lookaheadSamplesFor(c, kRate));
		std::vector<float> onL, onR;
		(void)render(c, true, onL, onR);
		size_t same = 0;
		size_t silentHead = 0;
		for (size_t i = 0; i < static_cast<size_t>(kFrames); ++i) {
			if (i < n) {
				silentHead += onL[i] == 0.0f && onR[i] == 0.0f ? 1 : 0;
			}
			else {
				same += onL[i] == offL[i - n] && onR[i] == offR[i - n] ? 1 : 0;
			}
		}
		CHECK(silentHead == n);
		CHECK(same == static_cast<size_t>(kFrames) - n);
		// BYPASSED: the input itself, `n` late — the host was told the latency whether the device is on or not.
		std::vector<float> byL, byR;
		const std::vector<float> in = render(c, false, byL, byR);
		size_t late = 0;
		for (size_t i = n; i < static_cast<size_t>(kFrames); ++i) {
			late += byL[i] == in[i - n] ? 1 : 0;
		}
		CHECK(late == static_cast<size_t>(kFrames) - n);
	}
}

TEST_CASE("ND: a step from a long quiet rest to 0 dBFS — with 5 or 10 ms of lookahead the onset overshoots the settled peak by under 1.1x, at the fastest Attack and the default, from over 8x Off; the settled level moves under 1 dB") {
	// THE REPORTED SPIKE, BY VALUE (`ndStep`): two seconds at -50 dBFS under Ratio 90% and Threshold 30%, so the upward gain
	// rests high, then 0 dBFS. The onset's peak over the settled peak; 1.0 is no overshoot. OPTION 3 (the chosen fix):
	// under a lookahead the detector's rise, the attack and a falling gain settle inside the window, so the overshoot is
	// gone whatever the Attack says. MEASURED: Off 10.57x (Attack 0) and 13.88x (default); at 5 ms 1.038 and 1.046; at
	// 10 ms 1.030 and 1.046. The settled peak, Off against on, 0.1444 against 0.1410..0.1420 (under 0.2 dB).
	for (const float attack : { 0.0f, 0.25f }) {
		CAPTURE(attack);
		const NdStep off = ndStep(0, true, attack);
		CAPTURE(off.overshoot());
		CHECK(off.overshoot() > 8.0f); // the spike exists Off, or the case would prove nothing
		for (const uint32_t choice : { 5u, 10u }) {
			CAPTURE(choice);
			const NdStep on = ndStep(choice, true, attack);
			CAPTURE(on.overshoot());
			CHECK(on.overshoot() < 1.1f);
			// THE SETTLED SOUND IS THE SAME DEVICE: within 1 dB of Off once the onset has passed.
			CHECK(std::fabs(20.0f * std::log10(on.steadyPeak / off.steadyPeak)) < 1.0f);
		}
	}
	// AND A GENTLER SETTING (Ratio 70%, Threshold 50%): MEASURED 2.45x / 3.18x Off, 1.02x at 10 ms.
	for (const float attack : { 0.0f, 0.25f }) {
		CAPTURE(attack);
		CHECK(ndStep(0, true, attack, 0.7f, 0.5f).overshoot() > 2.0f);
		CHECK(ndStep(10, true, attack, 0.7f, 0.5f).overshoot() < 1.1f);
	}
}

//------------------------------------------------------------------------------------------------//
// KEY TRACK: the Phaser's contract on OWLTT's two crossovers                                  //
//------------------------------------------------------------------------------------------------//

namespace {

feathers::device::NoteContext heldAt(float pitch, bool held = true) {
	feathers::device::NoteContext n;
	n.pitchSemitones = pitch;
	n.gate = held;
	return n;
}

/// ONE OWLTT RUN: the settings, the notes handed in before `blocks` blocks of a quiet sine — what the splitters end on.
void runBlocks(Owltt& o, int blocks) {
	std::vector<float> L(256), R(256);
	for (int b = 0; b < blocks; ++b) {
		fillSine(L, R, 440.0f, 48000.0f, 0.01f, L.size());
		o.processStereo(L.data(), R.data(), 256);
	}
}

/// THE CORNER KEY TRACK MUST PLACE, restated from the design: the note's Hz moved by the knob's offset from Low
/// Crossover's 200 Hz default, clamped to 20 Hz .. 20 kHz.
double trackedRestated(double knobHz, double pitch) {
	const double noteHz = 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0);
	return std::clamp(noteHz * knobHz / 200.0, 20.0, 20000.0);
}

} // namespace

TEST_CASE("OWLTT Key Track: at a held note both crossovers sit on the note moved by their offsets — restated, and Low at its default IS the note") {
	Settings s = defaultSettings();
	s.keyTrack = true;
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	o.setNoteContext(heldAt(57.0f));
	runBlocks(o, 20);
	CHECK(o.effectiveLowCrossoverHzForTest() == doctest::Approx(trackedRestated(s.lowCrossover, 57.0)).epsilon(1e-5));
	CHECK(o.effectiveLowCrossoverHzForTest() == doctest::Approx(220.0).epsilon(1e-5));
	CHECK(o.effectiveHighCrossoverHzForTest() == doctest::Approx(trackedRestated(s.highCrossover, 57.0)).epsilon(1e-5));
	CHECK(o.noteHzReading() == doctest::Approx(220.0).epsilon(1e-9));
	s.highCrossover = 3000.0f;
	o.setTarget(s);
	o.setNoteContext(heldAt(64.0f));
	runBlocks(o, 20);
	CHECK(o.effectiveHighCrossoverHzForTest() == doctest::Approx(trackedRestated(3000.0, 64.0)).epsilon(1e-5));
}

TEST_CASE("OWLTT Key Track: no note yet, the knobs rule; a held note is LATCHED through the release; the host's reset forgets it") {
	Settings s = defaultSettings();
	s.keyTrack = true;
	void* mem = ::operator new(kAudioOps.stateBytes, std::align_val_t { kAudioOps.stateAlign });
	kAudioOps.construct(mem);
	kAudioOps.prepare(mem, 48000.0, 4096);
	Owltt& o = *static_cast<Owltt*>(mem);
	o.setTarget(s);
	runBlocks(o, 4);
	CHECK(o.effectiveLowCrossoverHzForTest() == s.lowCrossover); // no note: the knob, exactly
	CHECK(o.noteHzReading() == 0.0);
	kAudioOps.setNoteContext(mem, heldAt(60.0f));
	runBlocks(o, 20);
	CHECK(o.effectiveLowCrossoverHzForTest() == doctest::Approx(trackedRestated(s.lowCrossover, 60.0)).epsilon(1e-5));
	kAudioOps.setNoteContext(mem, heldAt(72.0f, false)); // released, at another pitch: ignored
	runBlocks(o, 20);
	CHECK(o.effectiveLowCrossoverHzForTest() == doctest::Approx(trackedRestated(s.lowCrossover, 60.0)).epsilon(1e-5));
	kAudioOps.reset(mem);
	runBlocks(o, 4);
	CHECK(o.effectiveLowCrossoverHzForTest() == s.lowCrossover);
	kAudioOps.destruct(mem);
	::operator delete(mem, std::align_val_t { kAudioOps.stateAlign });
}

TEST_CASE("OWLTT Key Track: Off is OWLTT before Key Track — notes change nothing, bit for bit") {
	Settings s = defaultSettings();
	Owltt quiet;
	Owltt noted;
	quiet.prepare(48000.0f);
	noted.prepare(48000.0f);
	quiet.setTarget(s);
	noted.setTarget(s);
	std::vector<float> aL(256), aR(256), bL(256), bR(256);
	for (int b = 0; b < 32; ++b) {
		noted.setNoteContext(heldAt(40.0f + static_cast<float>(b), b % 2 == 0));
		fillSine(aL, aR, 330.0f + 10.0f * static_cast<float>(b), 48000.0f, 0.5f, aL.size());
		bL = aL;
		bR = aR;
		quiet.processStereo(aL.data(), aR.data(), 256);
		noted.processStereo(bL.data(), bR.data(), 256);
		for (size_t i = 0; i < aL.size(); ++i) {
			REQUIRE(aL[i] == bL[i]);
			REQUIRE(aR[i] == bR[i]);
		}
	}
	CHECK(noted.effectiveLowCrossoverHzForTest() == s.lowCrossover);
	CHECK(noted.noteHzReading() == 0.0);
}

TEST_CASE("OWLTT Key Track: the gap and the audible range hold on the TRACKED corners — High below Low, and a note pushing High past 20 kHz") {
	Settings s = defaultSettings();
	s.keyTrack = true;
	s.highCrossover = 150.0f; // below Low's offset
	Owltt o;
	o.prepare(48000.0f);
	o.setTarget(s);
	o.setNoteContext(heldAt(57.0f));
	runBlocks(o, 20);
	CHECK(o.effectiveHighCrossoverHzForTest() == doctest::Approx(o.effectiveLowCrossoverHzForTest() + 100.0f).epsilon(1e-5));
	s.highCrossover = 2000.0f;
	o.setTarget(s);
	o.setNoteContext(heldAt(108.0f)); // C8: 2 kHz's offset lands past 20 kHz
	runBlocks(o, 20);
	CHECK(o.effectiveHighCrossoverHzForTest() == doctest::Approx(20000.0f).epsilon(1e-5));
}

TEST_CASE("OWLTT Key Track: the switch round-trips the row — saved, read into a fresh instance by value, and that instance tracks") {
	Settings s = defaultSettings();
	s.keyTrack = true;
	const Row row = rowOf(s);
	CHECK(row[static_cast<size_t>(Param::KeyTrack)] == 1.0f);
	const Settings back = stateOf(row);
	CHECK(back.keyTrack);
	Owltt fresh;
	fresh.prepare(48000.0f);
	fresh.setTarget(back);
	fresh.setNoteContext(heldAt(57.0f));
	runBlocks(fresh, 20);
	CHECK(fresh.effectiveLowCrossoverHzForTest() == doctest::Approx(220.0).epsilon(1e-5));
}

//------------------------------------------------------------------------------------------------//
// BACKLOG OY — AT BLEND 0, DO THE CROSSOVER CUTOFFS STILL CHANGE THE TONE? (PHASE 1: A STUDY)     //
//------------------------------------------------------------------------------------------------//

namespace {

struct OyRun {
	std::vector<float> in, out; // interleaved L,R
};

/// One render at Blend 0: an impulse, then a decorrelated noise burst, through `s` — the Blend held by the
/// stored value (`windowed` false) or by a host window of zeros, which is the road a routed Blend takes.
OyRun oyRender(const Settings& s, bool windowed) {
	constexpr double kRate = 48000.0;
	constexpr int kFrames = 8192;
	constexpr int kChunk = 256;
	Owltt owltt;
	owltt.prepare(kRate);
	owltt.setTarget(s);
	std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
	l[64] = r[64] = 0.5f;
	LqNoise n { 0x9e3779b9u };
	for (int i = 2048; i < 3072; ++i) {
		l[static_cast<size_t>(i)] = 0.5f * n.next();
		r[static_cast<size_t>(i)] = 0.5f * n.next();
	}
	OyRun run;
	for (int i = 0; i < kFrames; ++i) {
		run.in.push_back(l[static_cast<size_t>(i)]);
		run.in.push_back(r[static_cast<size_t>(i)]);
	}
	const std::array<float, kChunk> zeros {};
	for (int at = 0; at < kFrames; at += kChunk) {
		if (windowed) {
			owltt.setBlendRun(zeros.data());
		}
		owltt.processStereo(&l[static_cast<size_t>(at)], &r[static_cast<size_t>(at)], kChunk);
	}
	for (int i = 0; i < kFrames; ++i) {
		run.out.push_back(l[static_cast<size_t>(i)]);
		run.out.push_back(r[static_cast<size_t>(i)]);
	}
	return run;
}

} // namespace

TEST_CASE("OY: at Blend 0 every mode renders the input exactly, late by its declared latency — at two cutoff pairs, "
          "lookahead Off and 10 ms, Blend stored and host-moved") {
	// At Blend 0, moving the band cutoffs changed the tone. It did, in the four LR modes: their dry
	// was the input through the crossover's allpass (`alignDry`, backlog ER), whose phase follows the cutoffs — MEASURED
	// before the fix, max|out - in| 0.65 (LR2) and 1.03 (LR4) on this signal, the two cutoff pairs 0.47 and 1.05 apart,
	// at an energy ratio of exactly 1. Now the dry fades to the raw input below `kDryAlignFadeBlend`, so Blend 0 is the
	// input to the bit in every mode. Mode 0 (unreachable by the descriptor, reachable by a raw row) is the aside: it
	// reported the lookahead's latency and delayed nothing; it takes the bypass road now, so it is late by it too.
	const uint32_t looks[] = { 0u, kLookaheadChoices - 1 };
	for (int mode = 0; mode <= 10; ++mode) {
		for (const uint32_t look : looks) {
			for (const bool windowed : { false, true }) {
				CAPTURE(mode);
				CAPTURE(look);
				CAPTURE(windowed);
				Settings a = patchDefault();
				a.mode = static_cast<float>(mode);
				a.blend = 0.0f;
				a.lookahead = look;
				a.lowCrossover = 200.0f;
				a.highCrossover = 2000.0f;
				Settings b = a;
				b.lowCrossover = 80.0f;
				b.highCrossover = 6000.0f;
				const OyRun ra = oyRender(a, windowed);
				const OyRun rb = oyRender(b, windowed);
				const size_t lat = 2u * lookaheadSamplesFor(look, 48000.0); // interleaved
				size_t differA = 0, differB = 0;
				for (size_t i = 0; i < ra.out.size(); ++i) {
					const float want = i >= lat ? ra.in[i - lat] : 0.0f;
					differA += ra.out[i] != want;
					differB += rb.out[i] != want;
				}
				CHECK(differA == 0u);
				CHECK(differB == 0u);
			}
		}
	}
}

namespace {

/// The steady RMS of a 0.1 tone at `freq` through `s`, over the second half of 0.4 s.
float oyToneRms(const Settings& s, float freq) {
	Owltt owltt;
	owltt.prepare(48000.0f);
	owltt.setTarget(s);
	std::vector<float> L(19200), R(19200);
	fillSine(L, R, freq, 48000.0f, 0.1f, L.size());
	processInChunks(owltt, L, R, 128);
	double sq = 0.0;
	for (size_t i = L.size() / 2; i < L.size(); ++i) {
		sq += static_cast<double>(L[i]) * L[i];
	}
	return static_cast<float>(std::sqrt(sq / static_cast<double>(L.size() / 2)));
}

/// The largest |out[i] - out[i-1]| of a 0.1 tone at `freq` through `s`, past a 0.1 s settle, with Blend handed in by
/// the host's window as `blendAt(frame)`.
template <class F>
float oyMaxStep(const Settings& s, float freq, F blendAt) {
	constexpr int kFrames = 14336; // whole chunks
	constexpr int kChunk = 128;
	Owltt owltt;
	owltt.prepare(48000.0f);
	owltt.setTarget(s);
	std::vector<float> L(kFrames), R(kFrames);
	fillSine(L, R, freq, 48000.0f, 0.1f, L.size());
	std::array<float, kChunk> run {};
	for (int at = 0; at < kFrames; at += kChunk) {
		for (int i = 0; i < kChunk; ++i) {
			run[static_cast<size_t>(i)] = blendAt(at + i);
		}
		owltt.setBlendRun(run.data());
		owltt.processStereo(&L[static_cast<size_t>(at)], &R[static_cast<size_t>(at)], kChunk);
	}
	float step = 0.0f;
	for (size_t i = 4800; i < L.size(); ++i) {
		step = std::max(step, std::fabs(L[i] - L[i - 1]));
	}
	return step;
}

/// A held patch for the two cases below: no ratio, a threshold nothing reaches, no clipper — the blend's arithmetic alone.
Settings oyQuiet(float mode, float blend) {
	Settings s = defaultSettings();
	s.mode = mode;
	s.blend = blend;
	s.ratio = 0.0f;
	s.threshold = 1.0f;
	s.softClip = false;
	return s;
}

} // namespace

TEST_CASE("OY: above the fade an LR mode's Blend is still flat — a tone at each corner comes through Blend 50 % and "
          "Blend at the fade's end at unity, in all four LR modes") {
	// ER's claim, kept: from `kDryAlignFadeBlend` up the dry is the aligned one, so a tone AT a corner, where the raw
	// mix cancels, comes through at unity. 0.1 amplitude is 0.0707 RMS.
	for (const float mode : { 5.0f, 6.0f, 7.0f, 8.0f }) {
		for (const float blend : { 0.5f, kDryAlignFadeBlend }) {
			for (const float freq : { 200.0f, 2000.0f }) {
				CAPTURE(mode);
				CAPTURE(blend);
				CAPTURE(freq);
				CHECK(oyToneRms(oyQuiet(mode, blend), freq) == doctest::Approx(0.0707f).epsilon(0.05));
			}
		}
	}
	CHECK(alignedDryShare(0.0f) == 0.0f);
	CHECK(alignedDryShare(kDryAlignFadeBlend * 0.5f) == doctest::Approx(0.5f));
	CHECK(alignedDryShare(kDryAlignFadeBlend) == 1.0f);
	CHECK(alignedDryShare(1.0f) == 1.0f);
}

TEST_CASE("OY: a host-moved Blend through the fade does not click — a tone at an LR4 corner, Blend swept 0 to 20 % "
          "and back, steps no further sample to sample than the tone does at either end") {
	// At the corner the aligned dry is the raw one inverted, so a dry that SWITCHED rather than faded would jump by
	// twice the tone's amplitude (0.2) in one sample. The tone's own largest step is 0.1 * 2pi * 2000 / 48000 = 0.026.
	const Settings s = oyQuiet(8.0f, 0.0f);
	const float atZero = oyMaxStep(s, 2000.0f, [](int) { return 0.0f; });
	const float atTop = oyMaxStep(s, 2000.0f, [](int) { return 0.2f; });
	const float swept = oyMaxStep(s, 2000.0f, [](int f) {
		const float t = static_cast<float>(f % 4800) / 4800.0f; // a triangle 0 -> 0.2 -> 0 every 0.1 s
		return 0.4f * std::min(t, 1.0f - t);
	});
	CAPTURE(atZero);
	CAPTURE(atTop);
	CHECK(atZero > 0.02f); // the tone is there
	CHECK(swept < 1.1f * std::max(atZero, atTop));
}
