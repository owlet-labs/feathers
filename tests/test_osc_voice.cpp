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

// Behavioural tests for the voice path: the phase accumulator, the block plumbing around the
// decimator, and the crossfade that a character rebuild goes through. What the oversampling is FOR
// is measured next door in test_aliasing.cpp; this file is about everything else the path has to get
// right for that measurement to mean anything.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_stair.hpp"
#include "spectrum.h"
#include "voice_harness.h"

#include "dsp/halfband.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using owlet::dsp::buildPhiStairParams;
using feathers::Character;
using feathers::OscEngine;
using feathers::test::BankedVoice;
using feathers::test::magnitudeAt;
using feathers::test::noteToHz;
using feathers::test::Window;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

/// What CharacterBank makes of kCharacterFadeSeconds at this rate: 256 samples. Recomputed here
/// rather than read back off a bank so a test that disagreed with the rule would say so.
constexpr int kFadeSamples = static_cast<int>(feathers::kCharacterFadeSeconds * kSampleRate + 0.5);

float maxAdjacentDelta(const std::vector<float>& x) {
	float m = 0.0f;
	for (size_t i = 1; i < x.size(); i++) {
		m = std::max(m, std::abs(x[i] - x[i - 1]));
	}
	return m;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
	float m = 0.0f;
	for (size_t i = 0; i < a.size(); i++) {
		m = std::max(m, std::abs(a[i] - b[i]));
	}
	return m;
}

Character stair(uint16_t zone, float offset) {
	return feathers::test::characterAt(OscEngine::Stair, zone, offset);
}

Character morph(uint16_t zone, float offset) {
	return feathers::test::characterAt(OscEngine::Morph, zone, offset);
}

// The two phase offsets that straddle zone 64's step-count crossing, bisected rather than
// hard-coded: they are adjacent floats, so a decimal literal for either rounds to the same side, and
// bisecting keeps the case pointed at the crossing if a firmware bump moves it. Same construction as
// test_stair_renderer.cpp, which pins the pop this pair causes in the character layer — a params
// delta of 1.6, from a height that flips sign when the step count crosses an integer.
std::pair<float, float> stepCountCrossing() {
	float lo = 0.00585f;
	float hi = 0.00605f;
	REQUIRE(buildPhiStairParams(64, lo).widths[7] == 0.0f);
	REQUIRE(buildPhiStairParams(64, hi).widths[7] > 0.0f);
	while (true) {
		const float mid = 0.5f * (lo + hi);
		if (mid == lo || mid == hi) {
			break;
		}
		if (buildPhiStairParams(64, mid).widths[7] > 0.0f) {
			hi = mid;
		}
		else {
			lo = mid;
		}
	}
	return {lo, hi};
}

struct SwitchRun {
	std::vector<float> fading;    // The one block the crossfade occupies.
	std::vector<float> oldAtFade; // The old character over that block's phases, from a voice that never swapped.
	std::vector<float> newAtFade; // The new character over the same phases. An instant switch would land on one
	                              // of these mid-block, so their difference is the size of the jump being masked.
	std::vector<float> after;      // The block after the fade, which should be the new character alone.
	std::vector<float> reference;  // That same block from the voice that played the new character all along.
	std::vector<float> afterLater; // Two blocks later again, once the decimator has forgotten the old character.
	std::vector<float> referenceLater;
};

/// Render the switching voice alongside two that never switch — one stuck on the old character, one
/// on the new. All three line up sample for sample, because every voice starts at phase zero on the
/// same frequency and is handed the same block sizes.
SwitchRun runSwitch(const Character& from, const Character& to, double freqHz, int warmup = 4) {
	BankedVoice v;
	BankedVoice refOld;
	BankedVoice refNew;
	std::vector<float> buf(static_cast<size_t>(kBlock));
	std::vector<float> oldBuf(static_cast<size_t>(kBlock));
	std::vector<float> newBuf(static_cast<size_t>(kBlock));

	for (auto* p : {&v, &refOld, &refNew}) {
		p->setSampleRate(kSampleRate);
		p->setFrequency(freqHz);
	}
	v.setCharacter(from);
	refOld.setCharacter(from);
	refNew.setCharacter(to);

	auto step = [&] {
		v.render(buf.data(), kBlock);
		refOld.render(oldBuf.data(), kBlock);
		refNew.render(newBuf.data(), kBlock);
	};

	SwitchRun r;
	for (int i = 0; i < warmup; i++) {
		step();
	}

	v.setCharacter(to);
	step();
	r.fading = buf;
	r.oldAtFade = oldBuf;
	r.newAtFade = newBuf;

	step();
	r.after = buf;
	r.reference = newBuf;

	step();
	step();
	r.afterLater = buf;
	r.referenceLater = newBuf;
	return r;
}

struct Case {
	const char* name;
	Character from;
	Character to;
};

std::vector<Case> switchCases() {
	const auto [lo, hi] = stepCountCrossing();
	return {
	    // The pop the crossfade exists for: adjacent phase offsets, a step count crossing 7 between
	    // them, and a height that flips from -0.8 to +0.8 as it does.
	    {"stair step-count crossing", stair(64, lo), stair(64, hi)},
	    // A large ordinary move, for the case where the two waveforms simply have nothing to do with
	    // each other rather than differing by one quantised decision.
	    {"stair zone 64 to 600", stair(64, 0.0f), stair(600, 0.0f)},
	    // Engines swapped mid-flight, which is the largest discontinuity the voice can be asked for.
	    {"morph to stair", morph(17, 0.0f), stair(108, 0.25f)},
	};
}

} // namespace

TEST_CASE("a rebuild crossfades, so the character layer's pops never reach the output") {
	// The bound is derived, not fitted. Writing the fade as y = (1-t)A + tB with t advancing by 1/N
	// per sample, consecutive samples differ by
	//
	//   y[n] - y[n-1] = (1-t)(A[n] - A[n-1]) + t(B[n] - B[n-1]) + (t[n] - t[n-1])(B[n-1] - A[n-1])
	//
	// so no sample-to-sample move can exceed the rougher of the two waveforms' own moves plus one
	// ramp step's worth of the gap between them. N is the FADE's length, not the block's — a fade
	// with a length of its own is the whole point of the ~2 ms floor, and a bound written against
	// the block length would have quietly loosened every time a test used a longer one. The 5%
	// allowance is because the fade runs at the oversampled rate and this is measured after
	// decimation, so the ramp and the filter do not quite commute.
	//
	// The second check is the point of the exercise: the gap being crossed is two to eight times what
	// any single sample-to-sample move ends up being. Dropped in instantly, that gap IS the pop.
	for (double freq : {110.0, 440.0}) {
		for (const Case& c : switchCases()) {
			INFO("case: ", std::string(c.name));
			CAPTURE(freq);
			const SwitchRun r = runSwitch(c.from, c.to, freq);

			const float jump = maxDiff(r.oldAtFade, r.newAtFade);
			const float roughest = std::max(maxAdjacentDelta(r.oldAtFade), maxAdjacentDelta(r.newAtFade));
			const float bound = 1.05f * (roughest + jump / static_cast<float>(kFadeSamples));

			CHECK(maxAdjacentDelta(r.fading) <= bound);
			CHECK(maxAdjacentDelta(r.fading) < 0.7f * jump);

			// These are real jumps; a case that quietly stopped being one would be no test. The floor is
			// 0.35 rather than the 0.4 an earlier revision used because routing stair through the 128-slot
			// grid shrank the gaps — the narrow-step excursions that made them widest are exactly what the
			// grid does not carry. Smallest of the six is now 0.377.
			CHECK(jump > 0.35f);
		}
	}
}

TEST_CASE("backlog EH: an FM depth change crossfades its index as the jitter depth does — the carrier's phase offset does not step at the rebuild") {
	// The FM index was read from the fresh table alone under a comment claiming the jitter's behaviour
	// (a crossfade), so every rebuild that changed the depth stepped the modulator's contribution to
	// the carrier phase in one sample — a discontinuity in the tone. Two flagship characters differing
	// only in FM depth; the fading block's roughest step is bounded by the two pure signals' own
	// roughness plus the fade's per-sample share of the total difference, as the general case bounds it.
	Character from = morph(17, 0.0f);
	from.flagship = true;
	from.sounds[0].fmDepth = 0.0f;
	Character to = from;
	to.sounds[0].fmDepth = 0.8f;
	// THE INDEX IS READ OFF A SEAM, NOT INFERRED FROM THE SAMPLES (CLAUDE.md: a value cannot be
	// recovered from a rendered buffer). `fmIndexNowForTest` is the index the last rendered sample
	// used. One sample into the fade it must be one part in kFadeSamples of the way from the old
	// index to the new; at the fade's end, the new. With the index taken from the fresh table alone,
	// the first fading sample already used the new index.
	for (double freq : {110.0, 440.0}) {
		CAPTURE(freq);
		BankedVoice v;
		BankedVoice refOld;
		BankedVoice refNew;
		for (auto* p : {&v, &refOld, &refNew}) {
			p->setSampleRate(kSampleRate);
			p->setFrequency(freq);
		}
		v.setCharacter(from);
		refOld.setCharacter(from);
		refNew.setCharacter(to);
		// Room for the render past the fade below, which is longer than a block now the fade is the rebuild interval.
		std::vector<float> buf(static_cast<size_t>(std::max(kBlock, kFadeSamples + 8)));
		for (int i = 0; i < 4; ++i) {
			v.render(buf.data(), kBlock);
			refOld.render(buf.data(), kBlock);
			refNew.render(buf.data(), kBlock);
		}
		const float oldIndex = refOld.voice.fmIndexNowForTest();
		const float newIndex = refNew.voice.fmIndexNowForTest();
		REQUIRE(newIndex > oldIndex + 1e-3f); // the depth change reaches the index: not vacuous
		v.setCharacter(to);
		v.render(buf.data(), 1);
		REQUIRE(v.voice.isFadingForTest());
		const float first = v.voice.fmIndexNowForTest();
		CAPTURE(oldIndex);
		CAPTURE(newIndex);
		CAPTURE(first);
		const float share = (newIndex - oldIndex) / static_cast<float>(kFadeSamples);
		CHECK(first == doctest::Approx(oldIndex + share).epsilon(0.5));
		CHECK(first < oldIndex + 0.1f * (newIndex - oldIndex)); // nowhere near the new index yet
		v.render(buf.data(), kFadeSamples + 8);
		CHECK_FALSE(v.voice.isFadingForTest());
		CHECK(v.voice.fmIndexNowForTest() == doctest::Approx(newIndex).epsilon(1e-4));
	}
}

TEST_CASE("FM index: it is computed when the frequency or the depth moves, not once per sample, and the value is the one a voice with no history computes") {
	Character c = morph(17, 0.0f);
	c.flagship = true;
	c.sounds[0].fmDepth = 0.8f;
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(220.0);
	v.setCharacter(c);
	std::vector<float> buf(static_cast<size_t>(kBlock));
	for (int i = 0; i < 4; ++i) {
		v.render(buf.data(), kBlock);
	}
	REQUIRE(v.voice.fmIndexNowForTest() > 0.0f); // FM is on: the index is asked for
	const uint64_t settled = v.voice.fmIndexComputesForTest();
	for (int i = 0; i < 4; ++i) {
		v.render(buf.data(), kBlock); // thousands of samples at the same frequency and depth
	}
	CHECK(v.voice.fmIndexComputesForTest() == settled);
	// A new frequency is one new computation (per slot asked), and its value is a fresh voice's at that frequency.
	v.setFrequency(660.0);
	v.render(buf.data(), kBlock);
	BankedVoice fresh;
	fresh.setSampleRate(kSampleRate);
	fresh.setFrequency(660.0);
	fresh.setCharacter(c);
	for (int i = 0; i < 5; ++i) {
		fresh.render(buf.data(), kBlock);
	}
	CHECK(v.voice.fmIndexComputesForTest() > settled);
	CHECK(v.voice.fmIndexComputesForTest() <= settled + 4);
	CHECK(v.voice.fmIndexNowForTest() == fresh.voice.fmIndexNowForTest());
}

TEST_CASE("a moving character is INTERPOLATED: each sample between two rebuilds is on the line between the two bakes, and the next fade starts where the last ended (backlog PB (c))") {
	// THE REBUILD INTERVAL IS THE FADE (kCharacterFadeSeconds, 256 samples at 48 kHz): a request inside a fade waits for
	// it, and the build that then lands fades in over the whole of the next interval. So a moving character is a chain of
	// straight lines between consecutive bakes — never a bake held and then stepped. Asserted on VALUES, at host rate
	// (no decimator between the mix and the sample, so the arithmetic is exact): three voices that never rebuild play A,
	// B and C at the same phases, with jitter and grit cleared; the moving voice is asked for B, then at once for C
	// (which must wait), and every sample it renders is compared with the mix the fade position says it must be.
	REQUIRE(kFadeSamples == 256);
	const Character a = stair(64, 0.0f);
	const Character b = stair(600, 0.0f);
	const Character c = stair(300, 0.25f); // stair: no jitter or grit for a late build to carry
	BankedVoice v;
	BankedVoice refA;
	BankedVoice refB;
	BankedVoice refC;
	const std::pair<BankedVoice*, const Character*> all[] = { { &v, &a }, { &refA, &a }, { &refB, &b }, { &refC, &c } };
	for (const auto& [voice, character] : all) {
		voice->setSampleRate(kSampleRate);
		voice->setFrequency(110.0);
		voice->setCharacter(*character);
		voice->clearModifiersForTest();
	}
	float out = 0.0f;
	float outA = 0.0f;
	float outB = 0.0f;
	float outC = 0.0f;
	const auto step = [&] {
		v.renderWithoutOversamplingForTest(&out, 1);
		refA.renderWithoutOversamplingForTest(&outA, 1);
		refB.renderWithoutOversamplingForTest(&outB, 1);
		refC.renderWithoutOversamplingForTest(&outC, 1);
	};
	for (int i = 0; i < 4 * kFadeSamples; ++i) {
		step();
	}
	REQUIRE(out == outA);

	v.setCharacter(b); // from rest: builds at once
	v.setCharacter(c); // inside the new fade: deferred until it ends
	v.clearModifiersForTest();
	const auto between = [](float x, float lo, float hi) { return x >= std::min(lo, hi) && x <= std::max(lo, hi); };
	int checkedAB = 0;
	int checkedBC = 0;
	int plateau = 0;
	float gapAB = 0.0f;
	float gapBC = 0.0f;
	bool sawFirstFade = false;
	for (int i = 0; i < 3 * kFadeSamples; ++i) {
		step();
		// The fade position of the sample just rendered, counted here from the voice's own state: the ramp is
		// (k + 1) / N on the k-th fading sample, reaching exactly 1 on the last.
		if (checkedAB < kFadeSamples) {
			const double t = static_cast<double>(checkedAB + 1) / static_cast<double>(kFadeSamples);
			CAPTURE(checkedAB);
			REQUIRE(out == static_cast<float>(outA + (static_cast<double>(outB) - outA) * t));
			REQUIRE(between(out, outA, outB));
			gapAB = std::max(gapAB, std::abs(outA - outB));
			++checkedAB;
			sawFirstFade = true;
			continue;
		}
		if (!v.voice.isFadingForTest() && checkedBC == 0 && out == outB) {
			++plateau; // B alone: the slack between one fade's end and the next build
			continue;
		}
		if (checkedBC < kFadeSamples) {
			const double t = static_cast<double>(checkedBC + 1) / static_cast<double>(kFadeSamples);
			CAPTURE(checkedBC);
			REQUIRE(out == static_cast<float>(outB + (static_cast<double>(outC) - outB) * t));
			REQUIRE(between(out, outB, outC));
			gapBC = std::max(gapBC, std::abs(outB - outC));
			++checkedBC;
		}
	}
	CHECK(sawFirstFade);
	CHECK(checkedBC == kFadeSamples); // the deferred build landed and faded in over a whole interval
	// NOT A HOLD-AND-STEP: between the two fades B stands alone only for the guard's carry slack (a handful of samples
	// for the decimator's carry), never for most of an interval as a 2 ms fade behind a 256-sample guard would.
	CHECK(plateau <= feathers::Halfband4x::kMaxCarry + 1);
	// And the gaps crossed are real: an instant switch would be a jump this size.
	CHECK(gapAB > 0.3f);
	CHECK(gapBC > 0.3f);
}

TEST_CASE("a rapid sweep through the character stays continuous: no step above its roughest place plus the fade's share of a jump (backlog PB (c))") {
	// A FAST MOTION, the case a rate-limited rebuild could click on: the puck swept through a whole zone of morph
	// terrain in a quarter of a second (zone 861: a smooth one, so a click would stand well clear of the waveform's own
	// edges), a new character asked for every 32 samples (the synth's tick), through the
	// rebuild pops the fade exists to mask. The bank builds at most once per fade and each build
	// fades in over the whole interval, so the output is a chain of crossfades whose sample-to-sample step is bounded
	// as the single crossfade's is (the first case in this file): the roughest of the waveforms on the path, plus one
	// ramp step's share of the largest gap between two bakes an interval apart. THE BOUND IS DERIVED, NOT FITTED: every
	// position asked for is rendered on its own (same note, same phases) to read its roughness, and positions an
	// interval apart are compared for the gap. 10% for the ramp and the decimator not commuting, and for a landed
	// build being up to one tick later than the interval.
	constexpr double kFreq = 110.0;
	constexpr int kTick = 32;
	constexpr int kRequests = 384;  // 12288 samples, a quarter second
	constexpr int kStatic = 1024;   // a static position's render: past the decimator's settling, over two cycles
	const auto at = [](int r) { return morph(861, static_cast<float>(r) / static_cast<float>(kRequests)); };

	std::vector<std::vector<float>> statics(static_cast<size_t>(kRequests));
	float roughest = 0.0f;
	for (int r = 0; r < kRequests; ++r) {
		BankedVoice s;
		s.setSampleRate(kSampleRate);
		s.setFrequency(kFreq);
		s.setCharacter(at(r));
		std::vector<float>& buf = statics[static_cast<size_t>(r)];
		buf.resize(static_cast<size_t>(kStatic));
		s.render(buf.data(), kStatic);
		const std::vector<float> settled(buf.begin() + kStatic / 2, buf.end());
		roughest = std::max(roughest, maxAdjacentDelta(settled));
	}
	// An interval apart is ceil((fade + slack) / tick) requests: the guard's length in ticks, rounded up.
	const int apart = (kFadeSamples + 8 + kTick - 1) / kTick;
	float largestGap = 0.0f;
	for (int r = 0; r + apart < kRequests; ++r) {
		const std::vector<float>& x = statics[static_cast<size_t>(r)];
		const std::vector<float>& y = statics[static_cast<size_t>(r + apart)];
		largestGap = std::max(largestGap, maxDiff(std::vector<float>(x.begin() + kStatic / 2, x.end()),
		                                          std::vector<float>(y.begin() + kStatic / 2, y.end())));
	}
	const float bound = 1.1f * (roughest + largestGap / static_cast<float>(kFadeSamples));

	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(kFreq);
	v.setCharacter(at(0));
	std::vector<float> warm(static_cast<size_t>(kStatic));
	v.render(warm.data(), kStatic);
	std::vector<float> swept;
	std::vector<float> chunk(static_cast<size_t>(kTick));
	const uint32_t generationBefore = v.bank.generation();
	for (int r = 0; r < kRequests; ++r) {
		v.setCharacter(at(r));
		v.render(chunk.data(), kTick);
		swept.insert(swept.end(), chunk.begin(), chunk.end());
	}
	const uint32_t builds = v.bank.generation() - generationBefore;
	const float step = maxAdjacentDelta(swept);
	CAPTURE(roughest);
	CAPTURE(largestGap);
	CAPTURE(bound);
	CAPTURE(builds);
	MESSAGE("sweep: largest step " << step << " against bound " << bound << " (roughest " << roughest
	                               << ", largest gap an interval apart " << largestGap << ", " << builds << " builds)");
	CHECK(step <= bound);
	// RATE-LIMITED: about one build per interval, not one per request.
	CHECK(builds <= static_cast<uint32_t>(kRequests * kTick / kFadeSamples + 1));
	CHECK(builds >= static_cast<uint32_t>(kRequests * kTick / (kFadeSamples + 8 + kTick) - 1));
	// NOT VACUOUS: the gaps the fades cross are several times the bound, so an unfaded switch would break it.
	CHECK(largestGap > 3.0f * bound);
}

TEST_CASE("the block after the crossfade is the new character and nothing else") {
	for (double freq : {110.0, 440.0}) {
		for (const Case& c : switchCases()) {
			INFO("case: ", std::string(c.name));
			CAPTURE(freq);
			const SwitchRun r = runSwitch(c.from, c.to, freq);

			// Not exact, and the residue is not the fade: it is the decimator still carrying a few
			// samples of the old character in its allpass state. A fade that had failed to terminate
			// would show a difference the size of the jump, which is 0.4 and up.
			CHECK(maxDiff(r.after, r.reference) < 0.02f);

			// Two blocks on, what is left of that memory is 7.2e-7 at worst (1.9e-6 through the
			// decimator this one replaced, 2026-09-24) — well over -110 dB under the signal, and
			// orders of magnitude below the difference one block earlier. A steeper stage 2 with a
			// slower top pole was tried during that swap and failed THIS bound at 1.3e-5, which is
			// what the bound is for. The allpass
			// cascade decays rather than terminating, so this converges towards the reference instead of
			// reaching it; the point being pinned is the decay, not equality.
			CHECK(maxDiff(r.afterLater, r.referenceLater) < 1e-5f);
		}
	}
}

TEST_CASE("the note comes out at the frequency the note asks for") {
	constexpr int kN = 16384;
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(noteToHz(69.0)); // 440 Hz exactly.
	v.setCharacter(morph(17, 0.0f));

	std::vector<float> x(static_cast<size_t>(kN), 0.0f);
	for (int i = 0; i < kN; i += kBlock) {
		v.render(x.data() + i, kBlock);
	}

	// Local peak, not global: a harmonic may well be louder than the fundamental, so the search is
	// deliberately confined to a band that only the fundamental can be in.
	const Window win(kN);
	double bestHz = 0.0;
	double best = -1.0;
	for (double hz = 420.0; hz <= 460.0; hz += 0.05) {
		const double m = magnitudeAt(x, win, kSampleRate, hz);
		if (m > best) {
			best = m;
			bestHz = hz;
		}
	}
	CAPTURE(bestHz);
	CHECK(std::abs(bestHz - 440.0) < 1.0);
}

TEST_CASE("two voices given the same instructions render bit-identically") {
	BankedVoice a;
	BankedVoice b;
	for (auto* p : {&a, &b}) {
		p->setSampleRate(kSampleRate);
		p->setFrequency(noteToHz(96.0));
		p->setCharacter(stair(108, 0.25f));
	}

	std::vector<float> xa(static_cast<size_t>(kBlock));
	std::vector<float> xb(static_cast<size_t>(kBlock));
	for (int i = 0; i < 8; i++) {
		if (i == 3) {
			a.setCharacter(stair(600, 0.0f));
			b.setCharacter(stair(600, 0.0f));
		}
		a.render(xa.data(), kBlock);
		b.render(xb.data(), kBlock);
		CAPTURE(i);
		REQUIRE(xa == xb);
	}
}

TEST_CASE("block size does not change the samples") {
	// The decimator only works in multiples of four, so a block that is not one leaves a few
	// already-decimated samples buffered for the next call. That buffering has to be invisible: the
	// oversampled stream is generated in the same order either way and the filter behind it is time
	// invariant, so chunking it differently cannot move a sample. Blocks of 37 exercise the carry on
	// every single call.
	constexpr int kTotal = 4096;

	auto renderIn = [](int block) {
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setFrequency(noteToHz(96.0));
		v.setCharacter(stair(108, 0.25f));
		std::vector<float> out(static_cast<size_t>(kTotal), 0.0f);
		for (int i = 0; i < kTotal; i += block) {
			v.render(out.data() + i, std::min(block, kTotal - i));
		}
		return out;
	};

	CHECK(renderIn(37) == renderIn(256));
	CHECK(renderIn(1) == renderIn(256));
}

TEST_CASE("the stair mip ladder engages in the voice path where the firmware switches") {
	// The original fixed-point Stair renderer plays a progressively duller table as pitch rises, switching at 700 Hz and 1800
	// Hz. That is character, not just anti-aliasing: a high note on hardware is a darker sound, and a
	// plugin that always played the bright grid would be wrong even if it aliased less.
	//
	// Measured as energy above 8 kHz against the fundamental. Raising the pitch normally pushes MORE
	// harmonics past a fixed cutoff, so this ratio drifts upward or holds; the ladder makes it fall
	// instead. Each crossing is therefore checked against a control pair inside one mip band, which
	// isolates the switch from the pitch change carrying it.
	constexpr double kTrebleFloorHz = 8000.0;
	constexpr int kN = 32768;

	auto trebleRatio = [](double f0) {
		Character c = stair(108, 0.25f);
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setFrequency(f0);
		v.setCharacter(c);

		std::vector<float> x(static_cast<size_t>(kN), 0.0f);
		for (int i = 0; i < kN; i += kBlock) {
			v.render(x.data() + i, kBlock);
		}

		const Window win(kN);
		const std::vector<double> spectrum = feathers::test::amplitudeSpectrum(x, win);
		const double binHz = kSampleRate / static_cast<double>(kN);
		double acc = 0.0;
		for (size_t k = 0; k < spectrum.size(); k++) {
			if (static_cast<double>(k) * binHz >= kTrebleFloorHz) {
				acc += spectrum[k] * spectrum[k];
			}
		}
		return std::sqrt(acc) / magnitudeAt(x, win, kSampleRate, f0);
	};

	// Selection first, so a failure downstream is unambiguous about which half broke.
	REQUIRE(feathers::stairMipForFrequency(690.0) == 0);
	REQUIRE(feathers::stairMipForFrequency(710.0) == 1);
	REQUIRE(feathers::stairMipForFrequency(1790.0) == 1);
	REQUIRE(feathers::stairMipForFrequency(1810.0) == 2);

	const double at600 = trebleRatio(600.0);
	const double at690 = trebleRatio(690.0);
	const double at710 = trebleRatio(710.0);
	const double at1700 = trebleRatio(1700.0);
	const double at1790 = trebleRatio(1790.0);
	const double at1810 = trebleRatio(1810.0);

	// Controls: 90 Hz of pitch inside one band barely moves the ratio. Measured 1.2% and 1.9%.
	CHECK(at690 > 0.95 * at600);
	CHECK(at1790 > 0.95 * at1700);

	// Crossings: 20 Hz of pitch across a threshold moves it far more, and downward. Measured 21.4%
	// across 700 Hz and 3.2% across 1800 Hz — the second is smaller because by then the first pass has
	// already taken most of what sits above 8 kHz.
	CHECK(at710 < 0.85 * at690);
	CHECK(at1810 < 0.98 * at1790);
}

TEST_CASE("a stereo build gives each ear its own table, and a mono build one table twice") {
	// Terrain stereo at the bank: the ear accessors answer with ear 0's table for a mono build, so
	// "the ears are the same table" is POINTER identity — the render path's fast test, structural
	// rather than a width compared against zero.
	feathers::CharacterBank bank;
	bank.setSampleRate(kSampleRate);

	const Character left = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character right = feathers::test::characterAt(OscEngine::Morph, 800);

	bank.setCharacterStereo(left, right);
	CHECK(bank.stereoActive());
	CHECK(&bank.current(0) != &bank.current(1));
	// And they are genuinely different sounds, or the pointer claim above is about two copies.
	CHECK(std::memcmp(&bank.current(0).morph, &bank.current(1).morph, sizeof(bank.current(0).morph))
	      != 0);

	// Collapse to mono. THE TRAP THIS GUARDS: the right ear's stale table is still sitting in the
	// slot, and an accessor that consulted the storage rather than the build kind would hand it out.
	bank.advance(10000); // drain the guard; nothing is pending, so any distance is legal
	bank.setCharacter(left);
	CHECK(&bank.current(0) == &bank.current(1));
	// The window is still open — a voice could be fading out of the stereo build — and closes with
	// the guard, which is the next case's subject.
	CHECK(bank.stereoActive());
	bank.advance(10000);
	CHECK_FALSE(bank.stereoActive());
}

TEST_CASE("the ear accessors hold through every fade combination") {
	// A flip is read by fading voices on BOTH sides, so previous(ear) has to answer by the build
	// kind of the PREVIOUS slot while current(ear) answers by the current one — mono into stereo and
	// stereo into mono each mix one mono end against two ear ends.
	feathers::CharacterBank bank;
	bank.setSampleRate(kSampleRate);

	const Character monoA = feathers::test::characterAt(OscEngine::Morph, 60);
	const Character left = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character right = feathers::test::characterAt(OscEngine::Morph, 800);
	const Character monoB = feathers::test::characterAt(OscEngine::Morph, 500);

	// MONO PREVIOUS, STEREO CURRENT: both ears fade FROM the one old table.
	bank.setCharacter(monoA);
	bank.setCharacterStereo(left, right); // first build armed no guard, so this lands now
	CHECK(bank.stereoActive());
	CHECK(&bank.previous(0) == &bank.previous(1));
	CHECK(&bank.current(0) != &bank.current(1));

	// STEREO PREVIOUS, MONO CURRENT: each ear fades from ITS OWN old table into the shared new one —
	// and the stereo window stays open while the guard could still be covering such a voice.
	bank.advance(10000);
	bank.setCharacter(monoB);
	CHECK(bank.stereoActive()); // the guard is running and the previous slot is stereo
	CHECK(&bank.previous(0) != &bank.previous(1));
	CHECK(&bank.current(0) == &bank.current(1));

	// The window closes exactly when the guard does. previous(ear) still answers by the old slot's
	// build kind — honestly two tables — but no voice can be holding them once the guard has
	// drained, which is what stereoActive going false certifies.
	bank.advance(10000);
	CHECK_FALSE(bank.stereoActive());
	CHECK(&bank.current(0) == &bank.current(1));
}

TEST_CASE("a stereo pair dedups like a character, and a collapse to mono is a real rebuild") {
	feathers::CharacterBank bank;
	bank.setSampleRate(kSampleRate);

	const Character left = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character right = feathers::test::characterAt(OscEngine::Morph, 800);

	bank.setCharacterStereo(left, right);
	bank.advance(10000);
	const uint32_t g = bank.generation();

	// A host re-sending its state every block asks for the pair it already has, every block.
	bank.setCharacterStereo(left, right);
	CHECK(bank.generation() == g);

	// A mono request equal to the live LEFT ear is not the live character — the second ear is real
	// prepared state, and collapsing it is a rebuild the fade has to mask like any other.
	bank.setCharacter(left);
	CHECK(bank.generation() == g + 1);
	bank.advance(10000);

	// And the mirror: a stereo request whose ears are both the live mono character still widens the
	// slot, whatever its characters say.
	bank.setCharacterStereo(left, left);
	CHECK(bank.generation() == g + 2);
	CHECK(bank.stereoActive());
}

TEST_CASE("a stereo request defers behind the guard exactly as a mono one does") {
	// The deferral is the double buffer's safety argument, and the pair rides it unchanged: one
	// guard, one landing sample, however the host chops its blocks.
	feathers::CharacterBank bank;
	bank.setSampleRate(kSampleRate);

	const Character monoA = feathers::test::characterAt(OscEngine::Morph, 60);
	const Character monoB = feathers::test::characterAt(OscEngine::Morph, 500);
	const Character left = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character right = feathers::test::characterAt(OscEngine::Morph, 800);

	bank.setCharacter(monoA);
	bank.setCharacter(monoB); // arms the guard
	const uint32_t g = bank.generation();

	bank.setCharacterStereo(left, right);
	CHECK(bank.generation() == g); // held, not built
	const int until = bank.samplesUntilPendingBuild();
	CHECK(until > 0);
	CHECK(until < 10000); // really the guard, not the nothing-pending sentinel

	bank.advance(until - 1);
	CHECK(bank.generation() == g); // one sample short: still held
	bank.advance(1);
	CHECK(bank.generation() == g + 1);
	CHECK(bank.stereoActive());
	CHECK(&bank.current(0) != &bank.current(1));
}

TEST_CASE("the stereo render of a mono build is the mono render twice, to the bit") {
	// The pair loop advances the same state the mono loop advances, and on a mono build both ears
	// hold the same table — so its output must be the mono render duplicated, exactly. This is the
	// claim that keeps nextSamplePair honest against nextSample: any divergence between the twins
	// shows up here as a bit difference.
	BankedVoice mono;
	mono.setSampleRate(kSampleRate);
	mono.setCharacter(feathers::test::characterAt(OscEngine::Morph, 100));
	mono.setFrequency(220.0);

	BankedVoice pair;
	pair.setSampleRate(kSampleRate);
	pair.setCharacter(feathers::test::characterAt(OscEngine::Morph, 100));
	pair.setFrequency(220.0);

	std::vector<float> ref(kBlock), l(kBlock), r(kBlock);
	mono.render(ref.data(), kBlock);
	pair.renderStereo(l.data(), r.data(), kBlock);

	int diffL = 0;
	int diffR = 0;
	for (int i = 0; i < kBlock; i++) {
		diffL += l[static_cast<size_t>(i)] == ref[static_cast<size_t>(i)] ? 0 : 1;
		diffR += r[static_cast<size_t>(i)] == ref[static_cast<size_t>(i)] ? 0 : 1;
	}
	CHECK(diffL == 0);
	CHECK(diffR == 0);
}

TEST_CASE("each ear plays its own character, exactly") {
	// An ear is not an approximation of a second voice — it IS the render a mono voice would make
	// of that ear's character, bit for bit, because the pair shares the phase, noise and fade the
	// two references have on their own. (The references share those by construction: same seed,
	// same pitch, same birth.)
	const Character left = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character right = feathers::test::characterAt(OscEngine::Morph, 800);

	BankedVoice stereo;
	stereo.setSampleRate(kSampleRate);
	stereo.setCharacterStereo(left, right);
	stereo.setFrequency(220.0);

	BankedVoice refL;
	refL.setSampleRate(kSampleRate);
	refL.setCharacter(left);
	refL.setFrequency(220.0);

	BankedVoice refR;
	refR.setSampleRate(kSampleRate);
	refR.setCharacter(right);
	refR.setFrequency(220.0);

	std::vector<float> l(kBlock), r(kBlock), rl(kBlock), rr(kBlock);
	stereo.renderStereo(l.data(), r.data(), kBlock);
	refL.render(rl.data(), kBlock);
	refR.render(rr.data(), kBlock);

	int diffL = 0;
	int diffR = 0;
	int same = 0;
	for (int i = 0; i < kBlock; i++) {
		diffL += l[static_cast<size_t>(i)] == rl[static_cast<size_t>(i)] ? 0 : 1;
		diffR += r[static_cast<size_t>(i)] == rr[static_cast<size_t>(i)] ? 0 : 1;
		same += l[static_cast<size_t>(i)] == r[static_cast<size_t>(i)] ? 1 : 0;
	}
	CHECK(diffL == 0);
	CHECK(diffR == 0);
	// And the two ears genuinely differ, or everything above compared four copies of one stream.
	CHECK(same < kBlock / 4);
}

TEST_CASE("widening mid-note continues both channels") {
	// Width turned up during a held note. Three claims, each a different failure's tripwire:
	// the LEFT ear whose character did not change plays on as if nothing happened, to the bit
	// (fade between bit-identical builds is exact: v + (v - v)·t = v); the RIGHT ear's first
	// delivered samples are the mono stream's own carry (the adoption beginStereoStream does); and
	// after the fade and the decimator's memory have drained, the right ear has become the
	// character it widened to.
	const Character monoChar = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character rightChar = feathers::test::characterAt(OscEngine::Morph, 800);
	constexpr int kSwitchAt = 103; // deliberately off the grid of four: one carried sample in flight
	constexpr int kTotal = 700;

	BankedVoice control;
	control.setSampleRate(kSampleRate);
	control.setCharacter(monoChar);
	control.setFrequency(220.0);
	std::vector<float> ctl(kTotal);
	control.render(ctl.data(), kTotal);

	BankedVoice reference;
	reference.setSampleRate(kSampleRate);
	reference.setCharacter(rightChar);
	reference.setFrequency(220.0);
	std::vector<float> ref(kTotal);
	reference.render(ref.data(), kTotal);

	BankedVoice widened;
	widened.setSampleRate(kSampleRate);
	widened.setCharacter(monoChar);
	widened.setFrequency(220.0);
	std::vector<float> pre(kSwitchAt), l(kTotal - kSwitchAt), r(kTotal - kSwitchAt);
	widened.render(pre.data(), kSwitchAt);
	widened.setCharacterStereo(monoChar, rightChar);
	widened.renderStereo(l.data(), r.data(), kTotal - kSwitchAt);

	// Before the switch, the widened run IS the control run.
	int diffPre = 0;
	for (int i = 0; i < kSwitchAt; i++) {
		diffPre += pre[static_cast<size_t>(i)] == ctl[static_cast<size_t>(i)] ? 0 : 1;
	}
	CHECK(diffPre == 0);

	// LEFT: unchanged character, unchanged stream — across the flip, through the fade, to the end.
	int diffL = 0;
	for (size_t i = 0; i < l.size(); i++) {
		diffL += l[i] == ctl[static_cast<size_t>(kSwitchAt) + i] ? 0 : 1;
	}
	CHECK(diffL == 0);

	// RIGHT, at the seam: 103 consumed is 104 generated, so exactly one mono sample was in the
	// carry, and it comes out of the right ear identical — the stream was adopted, not restarted.
	CHECK(r[0] == ctl[kSwitchAt]);

	// RIGHT, settled: past the 2 ms fade and two full blocks of allpass memory, the ear is the
	// widened character. The bound is the one "the block after the crossfade" measured for exactly
	// this decay; equality is not available because the decimator converges rather than switching.
	float tail = 0.0f;
	for (int i = kTotal - kSwitchAt - 64; i < kTotal - kSwitchAt; i++) {
		tail = std::max(tail, std::abs(r[static_cast<size_t>(i)]
		                               - ref[static_cast<size_t>(kSwitchAt) + static_cast<size_t>(i)]));
	}
	CHECK(tail < 1e-5f);
}

TEST_CASE("block size does not change the stereo samples") {
	// The stereo path inherits the mono path's contract: generated position is ceil4(consumed),
	// whatever sequence of block lengths got there — including across the widening flip, which the
	// harness lands at the same consumed sample in both runs.
	const Character monoChar = feathers::test::characterAt(OscEngine::Morph, 100);
	const Character rightChar = feathers::test::characterAt(OscEngine::Morph, 800);
	constexpr int kSwitchAt = 103;
	constexpr int kTotal = 400;

	const auto run = [&](const std::vector<int>& preChunks, const std::vector<int>& postChunks,
	                     std::vector<float>& l, std::vector<float>& r) {
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setCharacter(monoChar);
		v.setFrequency(220.0);
		std::vector<float> pre(kSwitchAt);
		int at = 0;
		for (int c : preChunks) {
			v.render(pre.data() + at, c);
			at += c;
		}
		REQUIRE(at == kSwitchAt);
		v.setCharacterStereo(monoChar, rightChar);
		at = 0;
		for (int c : postChunks) {
			v.renderStereo(l.data() + at, r.data() + at, c);
			at += c;
		}
		REQUIRE(at == kTotal - kSwitchAt);
	};

	std::vector<float> l1(kTotal - kSwitchAt), r1(kTotal - kSwitchAt);
	run({ kSwitchAt }, { kTotal - kSwitchAt }, l1, r1);

	std::vector<float> l2(kTotal - kSwitchAt), r2(kTotal - kSwitchAt);
	run({ 7, 64, 32 }, { 1, 3, 16, 100, 129, 48 }, l2, r2);

	int diff = 0;
	for (size_t i = 0; i < l1.size(); i++) {
		diff += (l1[i] == l2[i] && r1[i] == r2[i]) ? 0 : 1;
	}
	CHECK(diff == 0);
}

TEST_CASE("a phase retrigger bends where a snap jumps, and the bend completes") {
	// §5.8, AT THE VOICE — the firmware HARM mechanism ported to the core oscillator, mutation-honest
	// at the exact claim: retriggerPhase() aims the phase home as a rate correction, so the waveform
	// across the retrigger has NO step beyond its own steady slope, while a raw snap to phase 0 —
	// adoptPhase(0), used here deliberately as the KNOWN-BAD — jumps. And the correction COMPLETES:
	// the firmware's ±1 LSB floor becomes a 1e-9-cycle floor here, so after a bounded run the phase
	// sits exactly on the trajectory a phase-0 start would have reached, not asymptotically near it.
	using feathers::test::BankedVoice;
	using feathers::test::characterAt;
	constexpr double kRate = 48000.0;
	constexpr double kHz = 440.0;
	constexpr int kLead = 517; // an odd stretch, so the running phase at the event is nothing special
	constexpr int kTail = 512;

	// Returns the LAST pre-event sample: the discontinuity a snap writes lives between that sample
	// and the first one after the event, so every window below is measured across the boundary.
	const auto runToEvent = [&](BankedVoice& v) {
		v.setSampleRate(kRate);
		// Zone 17, the near-fundamental morph case: a smooth waveform, so the steady slope is
		// small and a snap's jump towers over it — at a steep zone the two blur together and the
		// discrimination has no teeth.
		v.setCharacter(characterAt(feathers::OscEngine::Morph, 17));
		v.clearModifiersForTest();
		v.setFrequency(kHz);
		std::vector<float> lead(kLead);
		v.renderWithoutOversamplingForTest(lead.data(), kLead);
		return lead.back();
	};
	const auto maxStep = [](const std::vector<float>& x) {
		float most = 0.0f;
		for (size_t i = 1; i < x.size(); i++) {
			most = std::max(most, std::abs(x[i] - x[i - 1]));
		}
		return most;
	};

	// The steady slope, from an undisturbed run over the same window.
	BankedVoice plain;
	runToEvent(plain);
	std::vector<float> steady(kTail);
	plain.renderWithoutOversamplingForTest(steady.data(), kTail);
	const float steadyStep = maxStep(steady);
	REQUIRE(steadyStep > 0.0f);

	// THE CLAIM IN THE SPEC'S OWN TERMS — "never jump phase, bend it" — measured on the phase
	// itself, which is exact where waveform amplitudes depend on the zone's shape. THE BEND: after
	// retriggerPhase, every per-sample phase step stays within increment ± increment/16 (the
	// correction's own bound: |error| ≤ 1/2 cycle, so |corr| ≤ inc/32, with margin) — a rate
	// inflection, never a jump. The waveform check above it is the audible half at the steady
	// slope's league.
	const double inc = kHz / kRate;
	const auto circularDelta = [](double a, double b) {
		double d = std::abs(a - b);
		return std::min(d, 1.0 - d);
	};
	BankedVoice bent;
	const float bentBefore = runToEvent(bent);
	const double bentPhaseAtEvent = bent.voice.phase();
	bent.voice.retriggerPhase();
	{
		double previous = bent.voice.phase();
		// The event itself moved nothing — the error is banked, not applied.
		CHECK(previous == bentPhaseAtEvent);
		std::vector<float> one(1);
		double worst = 0.0;
		for (int i = 0; i < 512; ++i) {
			bent.renderWithoutOversamplingForTest(one.data(), 1);
			const double step = circularDelta(bent.voice.phase(), previous);
			worst = std::max(worst, std::abs(step - inc));
			previous = bent.voice.phase();
		}
		CHECK(worst <= inc / 16.0);
	}
	// And the audible half: the rendered window across the event stays at the steady slope's league.
	std::vector<float> bendTail(kTail);
	bent.renderWithoutOversamplingForTest(bendTail.data(), kTail);
	bendTail.insert(bendTail.begin(), bentBefore);
	CHECK(maxStep(bendTail) < steadyStep * 1.6f);

	// THE SNAP, the known-bad in the same phase terms: adoptPhase(0) from a running phase IS the
	// jump the bend exists to remove — many increments' worth of phase in zero samples, where the
	// bend's largest single step was inc/16 above.
	BankedVoice snapped;
	runToEvent(snapped);
	const double runningPhase = snapped.voice.phase();
	// Only meaningful when the running phase is genuinely elsewhere — REQUIRE it, or the snap is a
	// no-op and the comparison proves nothing.
	REQUIRE(circularDelta(runningPhase, 0.0) > 0.1);
	snapped.voice.adoptPhase(0.0);
	CHECK(circularDelta(snapped.voice.phase(), runningPhase) > 10.0 * inc);

	// COMPLETION: after the bend has run, the phase sits on the phase-0 trajectory exactly — the
	// arithmetic reference frac(n · f/sr), no second voice needed. ~16-cycle time constant from a
	// half-cycle error to 1e-9 is ~320 cycles; at 440 Hz that is ~35k samples, so 48k is conviction.
	BankedVoice converge;
	runToEvent(converge);
	converge.voice.retriggerPhase();
	constexpr int kRun = 48000;
	std::vector<float> sink(kRun);
	converge.renderWithoutOversamplingForTest(sink.data(), kRun);
	const double reference = (static_cast<double>(kRun) * inc) - std::floor(static_cast<double>(kRun) * inc);
	double delta = std::abs(converge.voice.phase() - reference);
	delta = std::min(delta, 1.0 - delta); // circular distance
	CHECK(delta < 2e-9);
}
