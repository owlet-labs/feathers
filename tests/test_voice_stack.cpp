/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// VOICE STACKING, STEP 1 (backlog KE): several oscillators per
// note, fanned in pitch by Stack Detune, each at 1/√N of the level; a Count change on a held note re-spaced by
// walking, never by jumping; and the CPU guard that steals sooner as the stack grows.
//
// EVERY CLAIM HERE IS ABOUT A VALUE, AND READS IT (CLAUDE.md): where each part stands in the fan, the pitch its
// oscillator is at, the gain it sounds at, the polyphony the manager was handed. The references are computed in
// this file from the rule as the doc states it — `k/(N−1) − ½` of the detune, `1/√N` — never by calling the
// engine's own helpers, so a wrong helper cannot agree with itself.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "param_routes.h"
#include "voice_harness.h"
#include "plugin/stack_index.h"
#include "plugin/synth.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "rack/published_chain.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <vector>

namespace {

/// Every allocation this process makes ON THE TEST THREAD, counted — the test_vox_voice trick, for the stack's own claim:
/// a stack of any size, and a Count change on a held note, cost the AUDIO thread no allocation. The rig renders on the
/// test thread, so that thread is the audio thread here. A worker the engine runs (the phase service, the background
/// builder) may allocate, and its allocations are counted apart: on Windows, MEASURED, four of them landed inside one
/// case's window in one run and inside a different case's in the next, which is how a worker's timing looks (inferred,
/// not traced: if they are on the test thread, these cases still go red).
const std::thread::id gTestThread = std::this_thread::get_id();
std::atomic<long> gAllocations { 0 };
std::atomic<long> gOtherThreadAllocations { 0 };

void countAllocation() noexcept {
	if (std::this_thread::get_id() == gTestThread) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	else {
		gOtherThreadAllocations.fetch_add(1, std::memory_order_relaxed);
	}
}

} // namespace

// See test_vox_voice.cpp for why this warning is suppressed around a correct replacement pair.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(size_t size) {
	countAllocation();
	void* p = feathers::test::heapAlloc(size == 0 ? 1 : size);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size) {
	return ::operator new(size);
}
void* operator new(size_t size, std::align_val_t alignment) {
	countAllocation();
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(size, static_cast<size_t>(alignment));
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size, std::align_val_t alignment) {
	return ::operator new(size, alignment);
}
void operator delete(void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
void operator delete(void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
#include "nothrow_new_forwarding.h"

namespace {

using feathers::ParamId;
using feathers::ParamSet;
using feathers::Synth;

constexpr double kRate = 48000.0;
constexpr int16_t kKey = 60;

struct Rig {
	ParamSet params;
	std::unique_ptr<Synth> synth = std::make_unique<Synth>(params);
	std::vector<float> left = std::vector<float>(4096);
	std::vector<float> right = std::vector<float>(4096);

	Rig() {
		synth->activate(kRate, 4096);
		set(ParamId::AZone, 17.0);
		set(ParamId::AOffset, 0.0);
		set(ParamId::Path, 0.0);
		set(ParamId::VoiceLevel, 1.0);
	}

	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }

	void render(int frames, int block = 256) {
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			synth->render(left.data(), right.data(), n);
		}
	}

	int slot(int16_t key = kKey) const { return synth->slotOfKeyForTest(key); }
};

/// THE FAN AS THE DOC STATES IT, restated here: part k of N at `k/(N−1) − ½` of the detune, 0 for a stack of one.
double fanOf(int part, int count) {
	return count <= 1 ? 0.0 : static_cast<double>(part) / static_cast<double>(count - 1) - 0.5;
}

int samples(double seconds) {
	return static_cast<int>(std::lround(seconds * kRate));
}

} // namespace

TEST_CASE("stack: a note's parts stand on the stated fan, lowest to highest, the middle one on the note") {
	Rig rig;
	rig.set(ParamId::StackCount, 5.0);
	rig.set(ParamId::StackDetune, 40.0); // cents of total fan
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);

	for (int part = 0; part < feathers::kMaxStack; ++part) {
		CAPTURE(part);
		CHECK(rig.synth->voiceStackLiveForTest(slot, part) == (part < 5));
		if (part >= 5) {
			continue;
		}
		CHECK(rig.synth->voiceStackFanForTest(slot, part) == fanOf(part, 5));
		// 60 plus the part's share of 40 cents: -0.2, -0.1, 0, +0.1, +0.2 semitones, measured on the oscillator.
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, part) - (60.0 + 0.4 * fanOf(part, 5))) < 1e-12);
		// 1/√5 each, which the doc's equal-power rule states.
		CHECK(std::abs(rig.synth->voiceStackGainForTest(slot, part) - static_cast<float>(1.0 / std::sqrt(5.0)))
		      < 1e-7f);
	}
	// THE MIDDLE PART IS ON THE NOTE EXACTLY, not within a tolerance — an odd stack keeps its centre true.
	CHECK(rig.synth->voiceStackPitchForTest(slot, 2) == 60.0);
	CHECK(rig.synth->voiceStackPitchForTest(slot, 0) < rig.synth->voiceStackPitchForTest(slot, 4));
}

TEST_CASE("stack: a stack of one is the plain voice — one part, on the note, at unity") {
	Rig rig;
	rig.set(ParamId::StackDetune, 50.0); // a detune with no second part has nothing to fan
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	CHECK(rig.synth->voiceStackLiveForTest(slot, 0));
	for (int part = 1; part < feathers::kMaxStack; ++part) {
		CHECK_FALSE(rig.synth->voiceStackLiveForTest(slot, part));
	}
	CHECK(rig.synth->voiceStackGainForTest(slot, 0) == 1.0f);
	CHECK(rig.synth->voiceStackPitchForTest(slot, 0) == 60.0);
	CHECK(rig.synth->voicePitchForTest(slot) == 60.0);
}

TEST_CASE("stack: Stack Detune is the total fan in cents, and it moves a held note's parts") {
	Rig rig;
	rig.set(ParamId::StackCount, 2.0);
	rig.set(ParamId::StackDetune, 50.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Two parts at ±25 cents: the whole 50-cent span, the designed maximum.
	CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 1) - rig.synth->voiceStackPitchForTest(slot, 0) - 0.5)
	      < 1e-12);
	rig.set(ParamId::StackDetune, 10.0);
	rig.render(2048); // past a tick's straight line to the new target
	CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 0) - (60.0 - 0.05)) < 1e-9);
	CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 1) - (60.0 + 0.05)) < 1e-9);
}

TEST_CASE("stack: a glide carries every part, each keeping its place in the fan") {
	Rig rig;
	rig.set(ParamId::VoiceMode, static_cast<double>(feathers::VoiceMode::Mono));
	rig.set(ParamId::GlideTime, 0.5);
	rig.set(ParamId::StackCount, 3.0);
	rig.set(ParamId::StackDetune, 30.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(4096);
	rig.synth->noteOn(0, 72, 2, 1.0);
	int slot = rig.synth->newestVoiceSlotForTest();
	REQUIRE(slot >= 0);
	// MID-GLIDE, at three instants: every part is where the middle part is, plus its own fixed offset. A part
	// left out of the glide would sit at 72 ± its offset while the middle one was still on the way.
	for (int step = 0; step < 3; ++step) {
		rig.render(256);
		slot = rig.synth->newestVoiceSlotForTest();
		const double middle = rig.synth->voiceStackPitchForTest(slot, 1);
		CAPTURE(middle);
		CHECK(middle > 60.0);
		CHECK(middle < 72.0);
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 0) - (middle - 0.15)) < 1e-6);
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 2) - (middle + 0.15)) < 1e-6);
	}
}

TEST_CASE("stack: the CPU guard steals sooner — a stack of N sounds at most B / N notes") {
	// THE ARITHMETIC, restated: min(voice count, max(1, floor(B / N))).
	for (int stack = 1; stack <= feathers::kMaxStack; ++stack) {
		for (int count = 1; count <= feathers::kMaxVoices; ++count) {
			const int expected = std::min(count, std::max(1, feathers::kOscillatorBudget / stack));
			CHECK(feathers::stackVoiceLimit(count, stack) == expected);
		}
	}
	// A stack of one is never limited: B covers every voice there is.
	CHECK(feathers::stackVoiceLimit(feathers::kMaxVoices, 1) == feathers::kMaxVoices);

	// AND THE ENGINE HANDS THE MANAGER THAT NUMBER, and the notes past it are stolen.
	Rig rig;
	rig.set(ParamId::VoiceCount, 16.0);
	rig.set(ParamId::StackCount, 8.0);
	const int limit = feathers::kOscillatorBudget / 8;
	for (int16_t i = 0; i < 12; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(48 + 2 * i), i + 1, 1.0);
		rig.render(64);
	}
	CHECK(rig.synth->appliedVoiceLimitForTest() == limit);
	rig.render(4096); // every steal's fade finished
	int held = 0;
	for (int16_t i = 0; i < 12; ++i) {
		held += rig.synth->slotOfKeyForTest(static_cast<int16_t>(48 + 2 * i)) >= 0 ? 1 : 0;
	}
	CHECK(held == limit);
}

TEST_CASE("stack: a Count change on a held note re-spaces by walking — born from a neighbour, faded, never jumped") {
	Rig rig;
	rig.set(ParamId::StackCount, 2.0);
	rig.set(ParamId::StackDetune, 40.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(2048);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	const double was0 = rig.synth->voiceStackFanForTest(slot, 0);
	const double was1 = rig.synth->voiceStackFanForTest(slot, 1);
	REQUIRE(was0 == -0.5);
	REQUIRE(was1 == 0.5);

	SUBCASE("growing: 2 → 4") {
		rig.set(ParamId::StackCount, 4.0);
		rig.render(64); // the change lands; one tick of the walk at most
		// Parts 2 and 3 are BORN at the sounding part nearest their new places (+1/6 and +1/2 → part 1, at +½),
		// silent, and rendering.
		for (const int part : { 2, 3 }) {
			CAPTURE(part);
			CHECK(rig.synth->voiceStackLiveForTest(slot, part));
			CHECK(rig.synth->voiceStackGainForTest(slot, part) < 0.1f);
		}
		// Nobody has reached the new fan yet — the walk has begun, it has not jumped.
		CHECK(rig.synth->voiceStackFanForTest(slot, 1) > 1.0 / 6.0);

		rig.render(samples(feathers::kStackRespaceSeconds) + 512); // the walk over
		for (int part = 0; part < 4; ++part) {
			CAPTURE(part);
			CHECK(rig.synth->voiceStackFanForTest(slot, part) == fanOf(part, 4));
			CHECK(rig.synth->voiceStackGainForTest(slot, part) == 0.5f); // 1/√4, reached exactly
			CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, part) - (60.0 + 0.4 * fanOf(part, 4))) < 1e-9);
		}
	}

	SUBCASE("a new part is born where its nearest neighbour stands, from silence") {
		rig.set(ParamId::StackCount, 4.0);
		// Straight after the change and before a single tick (2048 samples in, a tick boundary, and one sample
		// is not a tick): part 2's NEW place is +1/6, and the sounding part nearest it is part 1, at +½ — so it is
		// born at +½, NOT at +1/6, which is the difference between a part appearing where the fan already
		// sounds and one appearing in a gap. Gain 0 plus one sample's step.
		rig.synth->render(rig.left.data(), rig.right.data(), 1);
		CHECK(rig.synth->voiceStackLiveForTest(slot, 2));
		CHECK(rig.synth->voiceStackFanForTest(slot, 2) == 0.5);
		CHECK(rig.synth->voiceStackGainForTest(slot, 2) < 0.01f);
		// And it sounds there: its oscillator is at the note plus half the detune, not at its own new place.
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 2) - (60.0 + 0.4 * 0.5)) < 1e-9);
	}

	SUBCASE("shrinking: 2 → 1 fades part 1 out and brings part 0 home to unity and the note") {
		rig.set(ParamId::StackCount, 1.0);
		rig.render(64);
		CHECK(rig.synth->voiceStackLiveForTest(slot, 1)); // fading, not cut
		CHECK(rig.synth->voiceStackGainForTest(slot, 1) > 0.0f);
		rig.render(samples(feathers::kStackRespaceSeconds) + 512);
		CHECK_FALSE(rig.synth->voiceStackLiveForTest(slot, 1));
		CHECK(rig.synth->voiceStackGainForTest(slot, 1) == 0.0f);
		CHECK(rig.synth->voiceStackGainForTest(slot, 0) == 1.0f);
		CHECK(rig.synth->voiceStackFanForTest(slot, 0) == 0.0);
		CHECK(rig.synth->voiceStackPitchForTest(slot, 0) == 60.0);
	}
}

TEST_CASE("stack: the walk is linear — halfway through the re-space, halfway along") {
	Rig rig;
	rig.set(ParamId::StackCount, 3.0);
	rig.set(ParamId::StackDetune, 40.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(2048);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	rig.set(ParamId::StackCount, 5.0); // part 0 stays at −½; part 2 walks 0 → 0 (still the middle); part 1 −0 → −¼
	// Render to a tick boundary first, so the count lands at one and the tick count is exact.
	const int ticks = static_cast<int>(std::lround(feathers::kStackRespaceSeconds * kRate / 64.0));
	rig.render(64 * (ticks / 2), 64);
	// Part 1 was at 0 (the middle of three) and is going to −¼ (the second of five).
	const double halfway = rig.synth->voiceStackFanForTest(slot, 1);
	CHECK(halfway < 0.0);
	CHECK(halfway > -0.25);
	CHECK(std::abs(halfway - (-0.25 * static_cast<double>(ticks / 2) / ticks)) < 0.25 / ticks + 1e-9);
}

TEST_CASE("stack: nothing allocates — not a deep stack, not a Count change on held notes") {
	Rig rig;
	rig.set(ParamId::StackDetune, 30.0);
	rig.render(1024); // settle the first character build
	const long before = gAllocations.load();
	rig.set(ParamId::StackCount, 8.0);
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(55 + i), i + 1, 1.0);
	}
	rig.render(2048);
	rig.set(ParamId::StackCount, 3.0);
	rig.render(2048);
	rig.set(ParamId::StackCount, 7.0);
	rig.set(ParamId::StackSpread, 0.8); // and the stereo road, slewing (step 2)
	rig.render(2048);
	CHECK(gAllocations.load() - before == 0);
}

TEST_CASE("stack index: nothing allocates — Drift walking, Random Fresh drawing, routes into pitch and level") {
	Rig rig;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::FineTune),
	           0.05f);
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::VoiceLevel),
	           0.3f);
	routes.publish(rig.params); // the editor's work, before the audio thread is watched
	rig.set(ParamId::StackCount, 6.0);
	rig.set(ParamId::StackShape, static_cast<double>(feathers::StackShape::Drift));
	rig.set(ParamId::StackShapeAmount, 1.0);
	rig.set(ParamId::StackMap, static_cast<double>(feathers::StackMap::Symmetric));
	rig.set(ParamId::StackMapAmount, 0.6);
	rig.render(1024);
	const long before = gAllocations.load();
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(55 + i), i + 1, 1.0);
	}
	rig.render(4096);
	rig.set(ParamId::StackShape, static_cast<double>(feathers::StackShape::RandomFresh));
	rig.synth->noteOn(0, 70, 9, 1.0);
	rig.render(2048);
	rig.set(ParamId::StackCount, 3.0);
	rig.render(2048);
	CHECK(gAllocations.load() - before == 0);
}

TEST_CASE("stack: the same performance renders the same samples, twice") {
	auto take = [] {
		Rig rig;
		rig.set(ParamId::StackCount, 4.0);
		rig.set(ParamId::StackDetune, 25.0);
		rig.synth->noteOn(0, kKey, 1, 1.0);
		std::vector<float> out;
		for (int b = 0; b < 16; ++b) {
			if (b == 6) {
				rig.set(ParamId::StackCount, 7.0);
			}
			rig.render(256);
			out.insert(out.end(), rig.left.begin(), rig.left.begin() + 256);
		}
		return out;
	};
	const std::vector<float> a = take();
	const std::vector<float> b = take();
	CHECK(a == b);
	// And not trivially: the stack sounds.
	float peak = 0.0f;
	for (const float s : a) {
		peak = std::max(peak, std::abs(s));
	}
	CHECK(peak > 0.01f);
}

//------------------------------------------------------------------------------------------------//
// STEP 2 — SPREAD AND THE STEREO ROAD                                                            //
//------------------------------------------------------------------------------------------------//

TEST_CASE("stack: Spread fans the parts across the stereo field, alternating, so pitch neighbours sit apart") {
	Rig rig;
	rig.set(ParamId::StackCount, 5.0);
	rig.set(ParamId::StackDetune, 40.0);
	rig.set(ParamId::StackSpread, 1.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	// ONE SAMPLE IN — well inside the 20 ms a pan would take to slew there — so a note whose parts slid out from
	// the centre would be caught here rather than having arrived by the time anyone looked.
	rig.synth->render(rig.left.data(), rig.right.data(), 1);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// THE FIRMWARE'S "−2 +1 0 −1 +2", restated: twice the fan position, the sign flipped for the parts an odd
	// distance from the fan's edge. At a new note the parts START there — nothing slews in from the centre.
	const float expected[5] = { -1.0f, 0.5f, 0.0f, -0.5f, 1.0f };
	for (int part = 0; part < 5; ++part) {
		CAPTURE(part);
		CHECK(rig.synth->voiceStackPanForTest(slot, part) == expected[part]);
	}
	// And the two channels do differ (the secondary check, proving the value above is not vacuous).
	rig.render(1024);
	CHECK(rig.left != rig.right);
}

TEST_CASE("stack: the pan law is a balance — unity at the centre, the far channel falling to silence") {
	Rig rig;
	rig.set(ParamId::StackCount, 3.0);
	rig.set(ParamId::StackSpread, 1.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	float l = 0.0f;
	float r = 0.0f;
	rig.synth->voiceStackChannelGainsForTest(slot, 0, &l, &r); // hard left
	CHECK(l == 1.0f);
	CHECK(r == 0.0f);
	rig.synth->voiceStackChannelGainsForTest(slot, 1, &l, &r); // the centre
	CHECK(l == 1.0f);
	CHECK(r == 1.0f);
	rig.synth->voiceStackChannelGainsForTest(slot, 2, &l, &r); // hard right
	CHECK(l == 0.0f);
	CHECK(r == 1.0f);
}

TEST_CASE("stack: a Spread move slews the pans, never steps them") {
	Rig rig;
	rig.set(ParamId::StackCount, 2.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth->voiceStackPanForTest(slot, 1) == 0.0f);
	rig.set(ParamId::StackSpread, 1.0);
	// ONE SAMPLE LATER, one sample's worth of the walk: a full swing (2) over the re-space time.
	const float perSample = static_cast<float>(2.0 / (feathers::kStackRespaceSeconds * kRate));
	rig.synth->render(rig.left.data(), rig.right.data(), 1);
	const float first = rig.synth->voiceStackPanForTest(slot, 1);
	CHECK(first > 0.0f);
	CHECK(first <= perSample * 1.0001f);
	// And it arrives, exactly, within the re-space time.
	rig.render(samples(feathers::kStackRespaceSeconds) + 64);
	CHECK(rig.synth->voiceStackPanForTest(slot, 1) == 1.0f);
	CHECK(rig.synth->voiceStackPanForTest(slot, 0) == -1.0f);
}

TEST_CASE("stack: at Spread 0 the stack is mono to the bit, and it goes back there when Spread returns to 0") {
	Rig rig;
	rig.set(ParamId::StackCount, 4.0);
	rig.set(ParamId::StackDetune, 30.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	for (int part = 0; part < 4; ++part) {
		CHECK(rig.synth->voiceStackPanForTest(slot, part) == 0.0f);
	}
	CHECK(rig.left == rig.right);

	rig.set(ParamId::StackSpread, 0.6);
	rig.render(2048);
	CHECK(rig.left != rig.right);

	rig.set(ParamId::StackSpread, 0.0);
	rig.render(samples(feathers::kStackRespaceSeconds) + 512); // the pans walk home
	for (int part = 0; part < 4; ++part) {
		CHECK(rig.synth->voiceStackPanForTest(slot, part) == 0.0f);
	}
	rig.render(256);
	CHECK(rig.left == rig.right); // and the mono road is back
}

TEST_CASE("stack: a part born by a Count change stands where its neighbour stands in the stereo field too") {
	Rig rig;
	rig.set(ParamId::StackCount, 2.0);
	rig.set(ParamId::StackSpread, 1.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(2048);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth->voiceStackPanForTest(slot, 1) == 1.0f); // part 1 of 2: fan +½, hard right
	rig.set(ParamId::StackCount, 4.0);
	rig.synth->render(rig.left.data(), rig.right.data(), 1);
	// Part 2 is born beside part 1 (fan +½, the nearest to its new +1/6), so at pan +1 — then walks to its own
	// place. One sample in, it has barely left: not the centre it would start from if its pan were forgotten.
	CHECK(rig.synth->voiceStackPanForTest(slot, 2) > 0.99f);
}

//------------------------------------------------------------------------------------------------//
// STEP 3 — THE STACK INDEX: SHAPE, CURVE, MAPPING, AND ITS ROUTES                                //
//------------------------------------------------------------------------------------------------//

namespace {

using feathers::StackMap;
using feathers::StackShape;
using feathers::StackValues;

/// **THE FIRMWARE'S `setupUnisonIndexSpread`, RESTATED** (reference firmware owlet-firmware-1.3, sound.cpp:3315-3607)
/// — its zones, its integer Curve, its permutation and its SORT, line for line, for the
/// shapes and mappings the design keeps. Written from the firmware's text rather than from `stack_index.cpp`, so a
/// wrong port cannot agree with itself.
std::array<float, 8> firmwareIndex(int shapeZone, float shapeMeta, int curve, int mapZone, float mapMeta, int n) {
	std::array<float, 8> raw {};
	for (int u = 0; u < n; u++) {
		const float t = static_cast<float>(u) / static_cast<float>(n - 1);
		const float bipolar = t * 2.0f - 1.0f;
		switch (shapeZone) {
		case 0:
			raw[u] = std::clamp(bipolar + shapeMeta, -1.0f, 1.0f);
			break;
		case 1: {
			const float exponent = 0.5f + shapeMeta * 2.5f;
			raw[u] = (bipolar >= 0 ? 1.0f : -1.0f) * std::pow(std::abs(bipolar), exponent);
			break;
		}
		case 2: {
			const float k = 1.0f + shapeMeta * 9.0f;
			raw[u] = std::tanh(bipolar * k) / std::tanh(k);
			break;
		}
		case 3: {
			const int levels = 2 + static_cast<int>(shapeMeta * 6.99f);
			raw[u] = std::round(bipolar * static_cast<float>(levels - 1)) / static_cast<float>(levels - 1);
			break;
		}
		default:
			raw[u] = bipolar;
		}
	}
	if (curve != 0) {
		const float exponent = curve > 0 ? 1.0f + static_cast<float>(curve) * 0.06f
		                                 : 1.0f / (1.0f + static_cast<float>(-curve) * 0.06f);
		for (int u = 0; u < n; u++) {
			raw[u] = (raw[u] >= 0 ? 1.0f : -1.0f) * std::pow(std::abs(raw[u]), exponent);
		}
	}
	std::array<int, 8> perm {};
	for (int u = 0; u < n; u++) {
		perm[u] = u;
	}
	switch (mapZone) {
	case 0:
		if (mapMeta < 0.01f) {
			for (int u = 0; u < n; u++) {
				perm[u] = (std::min(u, n - 1 - u) & 1) ? (n - 1 - u) : u;
			}
		}
		else {
			std::array<int, 8> magOrder {};
			std::array<int, 8> panOrder {};
			for (int u = 0; u < n; u++) {
				magOrder[u] = u;
				panOrder[u] = u;
			}
			for (int i = 1; i < n; i++) {
				const int key = magOrder[i];
				const float keyMag = std::abs(raw[key]);
				int j = i - 1;
				while (j >= 0 && std::abs(raw[magOrder[j]]) > keyMag) {
					magOrder[j + 1] = magOrder[j];
					j--;
				}
				magOrder[j + 1] = key;
			}
			for (int i = 1; i < n; i++) {
				const int key = panOrder[i];
				const int keyDist = std::abs(2 * key - (n - 1));
				int j = i - 1;
				while (j >= 0 && std::abs(2 * panOrder[j] - (n - 1)) > keyDist) {
					panOrder[j + 1] = panOrder[j];
					j--;
				}
				panOrder[j + 1] = key;
			}
			for (int i = 0; i < n; i++) {
				const int looseIdx = (std::min(panOrder[i], n - 1 - panOrder[i]) & 1) ? (n - 1 - panOrder[i]) : panOrder[i];
				const float blended = static_cast<float>(looseIdx) * (1.0f - mapMeta) + static_cast<float>(magOrder[i]) * mapMeta;
				perm[panOrder[i]] = std::clamp(static_cast<int>(blended + 0.5f), 0, n - 1);
			}
		}
		break;
	case 1:
		if (mapMeta < 0.01f) {
		}
		else if (mapMeta > 0.99f) {
			for (int u = 0; u < n; u++) {
				perm[u] = (std::min(u, n - 1 - u) & 1) ? u : (n - 1 - u);
			}
		}
		else {
			const int swapCount = std::max(1, static_cast<int>(mapMeta * static_cast<float>(n / 2) + 0.5f));
			for (int i = 0; i < swapCount && i < n / 2; i++) {
				perm[i] = n - 1 - i;
				perm[n - 1 - i] = i;
			}
		}
		break;
	case 2:
		if (mapMeta > 0.01f) {
			const float exponent = 1.0f + mapMeta * 3.0f;
			for (int u = 0; u < n; u++) {
				const float t = static_cast<float>(u) / static_cast<float>(n - 1);
				perm[u] = std::clamp(static_cast<int>(std::pow(t, exponent) * static_cast<float>(n - 1) + 0.5f), 0, n - 1);
			}
		}
		break;
	case 3:
		if (mapMeta < 0.01f) {
			for (int u = 0; u < n; u++) {
				perm[u] = n - 1 - u;
			}
		}
		else {
			const float exponent = 1.0f + mapMeta * 3.0f;
			for (int u = 0; u < n; u++) {
				const float t = static_cast<float>(n - 1 - u) / static_cast<float>(n - 1);
				perm[u] = std::clamp(static_cast<int>(std::pow(t, exponent) * static_cast<float>(n - 1) + 0.5f), 0, n - 1);
			}
		}
		break;
	case 4: {
		const float focusPos = mapMeta * static_cast<float>(n - 1);
		for (int u = 0; u < n; u++) {
			const float dist = std::abs(static_cast<float>(u) - focusPos);
			const float maxDist = std::max(focusPos, static_cast<float>(n - 1) - focusPos);
			const float normalizedDist = (maxDist > 0) ? (dist / maxDist) : 0.0f;
			perm[u] = std::clamp(static_cast<int>((1.0f - normalizedDist) * static_cast<float>(n - 1) + 0.5f), 0, n - 1);
		}
		break;
	}
	case 5: {
		const int shift = static_cast<int>(mapMeta * static_cast<float>(n - 1));
		for (int u = 0; u < n; u++) {
			perm[u] = (u + shift) % n;
		}
		break;
	}
	case 7: {
		const int mapRaw = 7 * 1024 + static_cast<int>(std::lround(mapMeta * 1023.0f));
		uint32_t seed = static_cast<uint32_t>(mapRaw * 2654435761u);
		for (int i = n - 1; i > 0; i--) {
			seed ^= seed >> 16;
			seed *= 0x45d9f3bu;
			seed ^= seed >> 16;
			const int j = static_cast<int>(seed % static_cast<uint32_t>(i + 1));
			std::swap(perm[i], perm[j]);
		}
		break;
	}
	default:
		break;
	}
	std::array<float, 8> sorted = raw;
	for (int i = 1; i < n; i++) {
		const float key = sorted[i];
		int j = i - 1;
		while (j >= 0 && sorted[j] > key) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}
	std::array<float, 8> out {};
	for (int u = 0; u < n; u++) {
		out[u] = sorted[std::clamp(perm[u], 0, n - 1)];
	}
	return out;
}

StackValues spread(int count, StackShape shape, float shapeAmount, StackMap map = StackMap::AsShaped,
                   float mapAmount = 0.0f, float curve = 0.0f) {
	feathers::StackIndexSettings s;
	s.count = count;
	s.shape = shape;
	s.shapeAmount = shapeAmount;
	s.map = map;
	s.mapAmount = mapAmount;
	s.curve = curve;
	return feathers::stackIndexSpread(s);
}

/// A route from the Stack Index into a synth parameter, published to the rig's params as the editor would.
void routeStackIndex(Rig& rig, feathers::test::Routes& routes, ParamId to, float amount) {
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(to), amount);
	routes.publish(rig.params);
}

} // namespace

TEST_CASE("stack index: the shapes the design keeps agree with the firmware, sort and all") {
	// LINEAR, POWER, S-CURVE AND STEP ascend with the part, so the firmware's sort was the identity on them and
	// removing it changes nothing. Every Mapping but Pairs (whose fix is the point), at
	// three amounts, three curves and every stack size.
	constexpr std::array<StackShape, 4> kShapes { StackShape::Linear, StackShape::Power, StackShape::SCurve,
		                                          StackShape::Step };
	constexpr std::array<std::pair<StackMap, int>, 7> kMaps { {
		{ StackMap::Symmetric, 0 }, { StackMap::AntiSym, 1 }, { StackMap::PitchUp, 2 }, { StackMap::PitchDown, 3 },
		{ StackMap::CenterOut, 4 }, { StackMap::Rotate, 5 }, { StackMap::Shuffle, 7 } } };
	// Metas the firmware can hold: its zone knob steps in 1023ths.
	constexpr std::array<float, 4> kMetas { 0.0f, 300.0f / 1023.0f, 511.0f / 1023.0f, 1.0f };
	constexpr std::array<int, 3> kCurves { 0, 25, -50 };
	size_t compared = 0;
	for (size_t shape = 0; shape < kShapes.size(); ++shape) {
		for (const auto& [map, zone] : kMaps) {
			for (const float shapeMeta : kMetas) {
				for (const float mapMeta : kMetas) {
					for (const int curve : kCurves) {
						for (int n = 2; n <= feathers::kMaxStack; ++n) {
							const std::array<float, 8> want =
							    firmwareIndex(static_cast<int>(shape), shapeMeta, curve, zone, mapMeta, n);
							const StackValues got = spread(n, kShapes[shape], shapeMeta, map, mapMeta,
							                               static_cast<float>(curve) / 50.0f);
							for (int u = 0; u < n; ++u) {
								CAPTURE(shape);
								CAPTURE(zone);
								CAPTURE(shapeMeta);
								CAPTURE(mapMeta);
								CAPTURE(curve);
								CAPTURE(n);
								CAPTURE(u);
								CHECK(std::abs(got[static_cast<size_t>(u)] - want[static_cast<size_t>(u)]) <= 1e-5f);
								++compared;
							}
						}
					}
				}
			}
		}
	}
	CHECK(compared == 4u * 7u * 4u * 4u * 3u * 35u); // 35 = 2 + 3 + ... + 8 parts
}

TEST_CASE("stack index: As shaped keeps the shape's order — Triangle's peak lands on the part it names") {
	// THE SORT'S FIX: at N = 5 and a peak at 0.75, part 3 is the peak. The firmware sorted first, so its peak value
	// always went to the LAST part whatever the amount said.
	const StackValues triangle = spread(5, StackShape::Triangle, 0.75f);
	CHECK(triangle[3] == 1.0f);
	for (int u = 0; u < 5; ++u) {
		CHECK(triangle[static_cast<size_t>(u)] <= triangle[3]);
	}
	CHECK(triangle[4] < 1.0f);
	// And the identity it is: the shape's own values, part for part.
	const StackValues shaped = feathers::stackShapeValues(StackShape::Sine, 0.6f, 0.0, 0, 7);
	CHECK(spread(7, StackShape::Sine, 0.6f) == shaped);
	// NOT VACUOUS: the firmware's order for the same Triangle puts the peak last.
	CHECK(firmwareIndex(4, 0.75f, 0, 1, 0.0f, 5)[4] == 1.0f);
}

TEST_CASE("stack index: Pairs gives each group its mean, so the fan stays centred and its top value is heard") {
	// Eight parts in pairs over the plain fan −1, −5/7, …, +1: each pair sits at its mean.
	const StackValues pairs = spread(8, StackShape::Linear, 0.0f, StackMap::Pairs, 0.0f);
	float sum = 0.0f;
	for (int u = 0; u < 8; u += 2) {
		const float a = static_cast<float>(u) / 7.0f * 2.0f - 1.0f;
		const float b = static_cast<float>(u + 1) / 7.0f * 2.0f - 1.0f;
		CHECK(std::abs(pairs[static_cast<size_t>(u)] - (a + b) * 0.5f) < 1e-6f);
		CHECK(pairs[static_cast<size_t>(u + 1)] == pairs[static_cast<size_t>(u)]);
		sum += pairs[static_cast<size_t>(u)] * 2.0f;
	}
	CHECK(std::abs(sum) < 1e-5f);
	// THE TOP VALUE REACHES THE TOP PAIR: (5/7 + 1)/2. The firmware's `(u / g) · g` gave it 5/7 and never read +1.
	CHECK(std::abs(pairs[7] - (5.0f / 7.0f + 1.0f) * 0.5f) < 1e-6f);
	// A ragged last group (8 in threes) is the mean of the two it has.
	const StackValues threes = spread(8, StackShape::Linear, 0.0f, StackMap::Pairs, 0.5f);
	CHECK(std::abs(threes[7] - (5.0f / 7.0f + 1.0f) * 0.5f) < 1e-6f);
}

TEST_CASE("stack index: a stack of one is all centre, whatever the Shape says") {
	for (size_t shape = 0; shape < static_cast<size_t>(StackShape::kCount); ++shape) {
		const StackValues one = spread(1, static_cast<StackShape>(shape), 0.7f, StackMap::Rotate, 0.4f, 0.3f);
		for (const float v : one) {
			CHECK(v == 0.0f);
		}
	}
}

TEST_CASE("stack index: Free reads the phi field, each part a golden step further along it") {
	// THE FIELD RESTATED: a bipolar triangle over the wrapped phase, part u at phase + u·φ⁻¹.
	feathers::StackIndexSettings s;
	s.count = 5;
	s.shape = StackShape::Free;
	s.freePhase = 0.3;
	const StackValues free = feathers::stackIndexSpread(s);
	for (int u = 0; u < 5; ++u) {
		double phase = 0.3 + static_cast<double>(u) * 0.6180340;
		phase -= std::floor(phase);
		// Rising from 0 to +1 over the first quarter, down to −1 at three quarters, back to 0.
		const double tri = phase < 0.25 ? 4.0 * phase : (phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0);
		CAPTURE(u);
		CHECK(std::abs(free[static_cast<size_t>(u)] - tri) < 1e-5);
	}
	// And the phase moves it: the same parts, a tenth further along, read other values.
	s.freePhase = 0.4;
	CHECK(feathers::stackIndexSpread(s) != free);
}

TEST_CASE("stack index: a route into Fine Tune moves each part by amount × its index × the span") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 3.0);
	rig.set(ParamId::StackDetune, 0.0); // the fan out of the way: only the index moves the parts
	routeStackIndex(rig, routes, ParamId::FineTune, 0.1f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Linear over three parts is −1, 0, +1; a tenth of Fine Tune's 200-cent span is 20 cents a unit of index.
	const std::array<double, 3> cents { -20.0, 0.0, 20.0 };
	for (int part = 0; part < 3; ++part) {
		CAPTURE(part);
		CHECK(rig.synth->voiceStackIndexForTest(slot, part) == static_cast<float>(part - 1));
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, part) - (60.0 + cents[static_cast<size_t>(part)] / 100.0))
		      < 1e-6); // the amount is a float: 0.1f is 0.1 to 1.5e-9
	}
	SUBCASE("and a Curve move reaches the held note's parts on the tick road") {
		rig.set(ParamId::StackShape, static_cast<double>(StackShape::Power));
		rig.set(ParamId::StackShapeAmount, 1.0); // exponent 3: the outer parts stay at ±1, so shift the count
		rig.set(ParamId::StackCount, 5.0);
		rig.render(8192); // past the re-space
		// Power 3 over five parts: −1, −⅛, 0, +⅛, +1 → −20, −2.5, 0, +2.5, +20 cents.
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 1) - (60.0 - 0.025)) < 1e-6);
		CHECK(std::abs(rig.synth->voiceStackPitchForTest(slot, 3) - (60.0 + 0.025)) < 1e-6);
	}
	SUBCASE("and without the route the index moves nothing") {
		routeStackIndex(rig, routes, ParamId::FineTune, 0.0f);
		rig.render(1024);
		for (int part = 0; part < 3; ++part) {
			CHECK(rig.synth->voiceStackPitchForTest(slot, part) == 60.0);
		}
	}
}

TEST_CASE("stack index: a route into Level gives each part its own level, as a share of the voice's") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 3.0);
	rig.set(ParamId::VoiceLevel, 0.5);
	routeStackIndex(rig, routes, ParamId::VoiceLevel, 0.25f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(1024);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Levels 0.25, 0.5 and 0.75 under a voice at 0.5: shares ½, 1 and 1½.
	CHECK(std::abs(rig.synth->voiceStackLevelScaleForTest(slot, 0) - 0.5f) < 1e-6f);
	CHECK(rig.synth->voiceStackLevelScaleForTest(slot, 1) == 1.0f);
	CHECK(std::abs(rig.synth->voiceStackLevelScaleForTest(slot, 2) - 1.5f) < 1e-6f);
	SUBCASE("held to 0..1 as the voice's own level is") {
		routeStackIndex(rig, routes, ParamId::VoiceLevel, 1.0f);
		rig.render(1024);
		CHECK(rig.synth->voiceStackLevelScaleForTest(slot, 0) == 0.0f); // 0.5 − 1 → 0
		CHECK(rig.synth->voiceStackLevelScaleForTest(slot, 2) == 2.0f); // 0.5 + 1 → 1, twice the voice
	}
}

TEST_CASE("stack index: Random is one draw for every note; Random Fresh draws each note anew") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 4.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::Random));
	rig.set(ParamId::StackShapeAmount, 1.0);
	routeStackIndex(rig, routes, ParamId::FineTune, 0.05f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.synth->noteOn(0, kKey + 7, 2, 1.0);
	rig.render(512);
	const int a = rig.slot(kKey);
	const int b = rig.slot(kKey + 7);
	REQUIRE(a >= 0);
	REQUIRE(b >= 0);
	bool anyNonZero = false;
	for (int part = 0; part < 4; ++part) {
		CAPTURE(part);
		CHECK(rig.synth->voiceStackIndexForTest(a, part) == rig.synth->voiceStackIndexForTest(b, part));
		CHECK(rig.synth->voiceStackIndexForTest(a, part) == feathers::stackHash(part, feathers::kStackRandomSeed));
		anyNonZero = anyNonZero || rig.synth->voiceStackIndexForTest(a, part) != 0.0f;
	}
	CHECK(anyNonZero);
	SUBCASE("and its amount is a depth, not a new draw") {
		rig.set(ParamId::StackShapeAmount, 0.5);
		rig.render(512);
		for (int part = 0; part < 4; ++part) {
			CHECK(rig.synth->voiceStackIndexForTest(a, part)
			      == 0.5f * feathers::stackHash(part, feathers::kStackRandomSeed));
		}
	}
	SUBCASE("Random Fresh: two notes, two draws") {
		rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
		rig.synth->noteOn(0, kKey + 3, 3, 1.0);
		rig.synth->noteOn(0, kKey + 5, 4, 1.0);
		rig.render(512);
		const int c = rig.slot(kKey + 3);
		const int d = rig.slot(kKey + 5);
		REQUIRE(c >= 0);
		REQUIRE(d >= 0);
		int differing = 0;
		for (int part = 0; part < 4; ++part) {
			differing += rig.synth->voiceStackIndexForTest(c, part) != rig.synth->voiceStackIndexForTest(d, part) ? 1 : 0;
		}
		CHECK(differing == 4);
	}
}

TEST_CASE("stack index: Drift starts on its mapped, curved base and walks around it, never back to the centre") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 4.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::Drift));
	rig.set(ParamId::StackShapeAmount, 1.0); // the fastest walk
	rig.set(ParamId::StackMap, static_cast<double>(StackMap::PitchDown));
	rig.set(ParamId::StackCurve, 1.0 / 3.0); // exponent 2
	routeStackIndex(rig, routes, ParamId::FineTune, 0.05f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.synth->render(rig.left.data(), rig.right.data(), 1);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// THE BASE, AT THE NOTE-ON: the fan −1, −⅓, +⅓, +1 squared with its sign, then reversed by Pitch−.
	const std::array<float, 4> base { 1.0f, 1.0f / 9.0f, -1.0f / 9.0f, -1.0f };
	for (int part = 0; part < 4; ++part) {
		CAPTURE(part);
		CHECK(std::abs(rig.synth->voiceStackIndexForTest(slot, part) - base[static_cast<size_t>(part)]) < 1e-6f);
	}
	// THE WALK, over ~5 s of ticks: it moves, and its mean is the base, not 0 (the firmware reverted to 0).
	std::array<double, 4> mean {};
	int moved = 0;
	constexpr int kBlocks = 1000;
	for (int b = 0; b < kBlocks; ++b) {
		rig.render(256);
		for (int part = 0; part < 4; ++part) {
			const float at = rig.synth->voiceStackIndexForTest(slot, part);
			mean[static_cast<size_t>(part)] += at / kBlocks;
			moved += at != base[static_cast<size_t>(part)] ? 1 : 0;
		}
	}
	CHECK(moved > kBlocks);
	CHECK(std::abs(mean[1] - base[1]) < 0.1);
	CHECK(std::abs(mean[2] - base[2]) < 0.1);
	CHECK(mean[1] > 0.0);
	CHECK(mean[2] < 0.0);
	CHECK(mean[0] > 0.8);
	CHECK(mean[3] < -0.8);
}

TEST_CASE("stack index: a Shape Amount move re-decides a held note's index") {
	// The amount is read with the pitches, so a held note follows it — the Triangle's peak walks from part to part.
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 5.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::Triangle));
	routeStackIndex(rig, routes, ParamId::FineTune, 0.05f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(256);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Amount 0: the peak on part 0.
	CHECK(rig.synth->voiceStackIndexForTest(slot, 0) == 1.0f);
	rig.set(ParamId::StackShapeAmount, 1.0);
	rig.render(256);
	// Amount 1: the peak on the last part, re-decided on the held note.
	CHECK(rig.synth->voiceStackIndexForTest(slot, 4) == 1.0f);
	CHECK(rig.synth->voiceStackIndexForTest(slot, 0) == -1.0f);
}

TEST_CASE("stack index: the same performance renders the same samples twice — Drift's walk and Random Fresh's draws too") {
	auto take = [] {
		Rig rig;
		feathers::test::Routes routes;
		routeStackIndex(rig, routes, ParamId::FineTune, 0.1f);
		rig.set(ParamId::StackCount, 4.0);
		rig.set(ParamId::StackShape, static_cast<double>(StackShape::Drift));
		rig.set(ParamId::StackShapeAmount, 1.0);
		rig.synth->noteOn(0, kKey, 1, 1.0);
		std::vector<float> out;
		for (int b = 0; b < 16; ++b) {
			if (b == 6) {
				rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
				rig.synth->noteOn(0, kKey + 4, 2, 1.0);
			}
			rig.render(256);
			out.insert(out.end(), rig.left.begin(), rig.left.begin() + 256);
		}
		return out;
	};
	const std::vector<float> a = take();
	const std::vector<float> b = take();
	// NOT SILENCE (backlog MA): two silent takes agree whatever the code does, so the first must sound.
	float peak = 0.0f;
	for (const float s : a) {
		peak = std::max(peak, std::abs(s));
	}
	REQUIRE(peak > 0.05f);
	CHECK(a == b);
}

//------------------------------------------------------------------------------------------------//
// STEP 4 — ONE VOWEL BAKE PER STACK, NOT PER PART                                                //
//------------------------------------------------------------------------------------------------//

namespace {

/// VOWEL COUNTRY: zone 640 at offset 1.3563, the vox tests' own stand and the one `bench_stack` measures on.
void standInVowelCountry(Rig& rig) {
	rig.set(ParamId::AZone, 640.0);
	rig.set(ParamId::AOffset, 1.3563);
}

} // namespace

TEST_CASE("stack bakes: a stack of eight bakes its vowel exactly as often as a stack of one") {
	// THE SHARE (§3.10): every part stands on one stand, so every part's vowel is one vowel, and only part 0 bakes it.
	// COUNTED, not timed: the bakes of each part, against the same note played as a stack of one.
	auto bakes = [](double stack, std::array<uint32_t, feathers::kMaxStack>* perPart) {
		Rig rig;
		standInVowelCountry(rig);
		rig.set(ParamId::StackCount, stack);
		rig.set(ParamId::StackDetune, 20.0);
		rig.synth->noteOn(0, kKey, 1, 1.0);
		rig.render(4096);
		rig.set(ParamId::AOffset, 1.36); // and the character moves, so it rebakes
		rig.render(4096);
		const int slot = rig.slot();
		REQUIRE(slot >= 0);
		for (int part = 0; part < feathers::kMaxStack; ++part) {
			(*perPart)[static_cast<size_t>(part)] = rig.synth->voiceStackOscForTest(slot, part).voxBakesForTest();
		}
	};
	std::array<uint32_t, feathers::kMaxStack> one {};
	std::array<uint32_t, feathers::kMaxStack> eight {};
	bakes(1.0, &one);
	bakes(8.0, &eight);
	// NOT VACUOUS: this is vowel country, and the note baked at its start and again when the character moved.
	REQUIRE(one[0] >= 2);
	CHECK(eight[0] == one[0]);
	for (int part = 1; part < feathers::kMaxStack; ++part) {
		CAPTURE(part);
		CHECK(eight[static_cast<size_t>(part)] == 0);
	}
}

TEST_CASE("stack bakes: every part reads the first part's bake, and the first part bakes at the note") {
	Rig rig;
	standInVowelCountry(rig);
	rig.set(ParamId::StackCount, 4.0);
	rig.set(ParamId::StackDetune, 20.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(4096);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	const feathers::OscVoice& lender = rig.synth->voiceStackOscForTest(slot, 0);
	CHECK(lender.voxLenderForTest() == nullptr);
	for (int part = 1; part < 4; ++part) {
		CAPTURE(part);
		const feathers::OscVoice& part_ = rig.synth->voiceStackOscForTest(slot, part);
		CHECK(part_.voxLenderForTest() == &lender);
		// THE SAME GRID, where the two play the same rung: the address is the lender's own bake.
		if (part_.mipForTest() == lender.mipForTest()) {
			CHECK(part_.freshGridForTest(0) == lender.freshGridForTest(0));
		}
	}
	// PART 0 STANDS AT THE BOTTOM OF THE FAN, 10 cents under the note, and bakes AT the note: its bake frequency is
	// middle C's, not its own.
	const double noteHz = 440.0 * std::exp2((60.0 - 69.0) / 12.0);
	CHECK(std::abs(lender.voxBakeHzForTest() - noteHz) < 1e-6);
	CHECK(std::abs(lender.frequencyHz() - noteHz * std::exp2(-0.1 / 12.0)) < 1e-6);
	SUBCASE("and a stack of one bakes at its own pitch, which is the note") {
		rig.set(ParamId::StackCount, 1.0);
		rig.render(8192);
		CHECK(lender.voxBakeHzForTest() == lender.frequencyHz());
	}
}

//------------------------------------------------------------------------------------------------//
// STEP 4 — EACH PART ON ITS OWN STAND                                                            //
//------------------------------------------------------------------------------------------------//

namespace {

/// Whether two prepared characters play the same wave, read at 97 phases through each one's own grid at mip 0.
bool samePlayed(const feathers::PreparedCharacter& a, const feathers::PreparedCharacter& b) {
	for (int i = 0; i < 97; ++i) {
		const double phase = static_cast<double>(i) / 97.0;
		if (a.eval(a.gridFor(0), phase) != b.eval(b.gridFor(0), phase)) {
			return false;
		}
	}
	return true;
}

void routeIndexTo(Rig& rig, feathers::test::Routes& routes, ParamId to, float amount) {
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(to), amount);
	routes.publish(rig.params);
}

} // namespace

TEST_CASE("stack stands: a route into Pos X stands each part at the puck plus amount × index × span") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 3.0);
	routeIndexTo(rig, routes, ParamId::PosX, 0.1f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(4096);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Linear over three parts is −1, 0, +1: stands at −0.2 and +0.2 on X (Pos's span is 2), the middle one on the puck.
	const int low = rig.synth->voiceStackStandForTest(slot, 0);
	const int high = rig.synth->voiceStackStandForTest(slot, 2);
	REQUIRE(low >= 0);
	REQUIRE(high >= 0);
	CHECK(low != high);
	CHECK(rig.synth->voiceStackStandForTest(slot, 1) == -1);
	CHECK(std::abs(rig.synth->standOffsetForTest(low)[0] - (-0.2)) < 1e-6);
	CHECK(std::abs(rig.synth->standOffsetForTest(high)[0] - 0.2) < 1e-6);
	CHECK(rig.synth->standOffsetForTest(low)[1] == 0.0);
	// EACH PART READS ITS STAND'S BANK — the oscillator's own pointer, not only the bookkeeping.
	CHECK(rig.synth->voiceStackOscForTest(slot, 0).bankForTest() == &rig.synth->standBankForTest(low));
	CHECK(rig.synth->voiceStackOscForTest(slot, 1).bankForTest() == &rig.synth->mainBankForTest());
	CHECK(rig.synth->voiceStackOscForTest(slot, 2).bankForTest() == &rig.synth->standBankForTest(high));

	SUBCASE("and a stand's bank plays what the puck would play standing there") {
		// THE INDEPENDENT ROAD: a second synth, unstacked, with its PUCK at +0.2 — its main bank is the reference.
		Rig there;
		there.set(ParamId::PosX, 0.2);
		there.synth->noteOn(0, kKey, 1, 1.0);
		there.render(4096);
		CHECK(samePlayed(rig.synth->standBankForTest(high).current(0), there.synth->mainBankForTest().current()));
		// NOT VACUOUS: the puck itself plays something else here.
		CHECK_FALSE(samePlayed(rig.synth->mainBankForTest().current(), there.synth->mainBankForTest().current()));
	}
	SUBCASE("and the route gone puts every part back on the puck, reading part 0's vowel") {
		routeIndexTo(rig, routes, ParamId::PosX, 0.0f);
		rig.render(1024);
		for (int part = 0; part < 3; ++part) {
			CAPTURE(part);
			CHECK(rig.synth->voiceStackStandForTest(slot, part) == -1);
			CHECK(rig.synth->voiceStackOscForTest(slot, part).bankForTest() == &rig.synth->mainBankForTest());
			CHECK(rig.synth->voiceStackOscForTest(slot, part).voxLenderForTest()
			      == (part == 0 ? nullptr : &rig.synth->voiceStackOscForTest(slot, 0)));
		}
	}
}

TEST_CASE("stack stands: a part crosses onto its stand with a character fade, not a jump") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 3.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(4096);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	REQUIRE_FALSE(rig.synth->voiceStackOscForTest(slot, 2).isFadingForTest());
	routeIndexTo(rig, routes, ParamId::PosX, 0.1f);
	rig.render(64); // this pass keys and builds the stand and moves the part onto it: 64 of the fade's 96 samples in
	REQUIRE(rig.synth->voiceStackStandForTest(slot, 2) >= 0);
	CHECK(rig.synth->voiceStackOscForTest(slot, 2).isFadingForTest());
	// And the part on the puck did not move, so it has nothing to fade.
	CHECK_FALSE(rig.synth->voiceStackOscForTest(slot, 1).isFadingForTest());
}

TEST_CASE("stack stands: parts on one stand share one slot, across voices") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 4.0);
	routeIndexTo(rig, routes, ParamId::PosY, 0.05f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.synth->noteOn(0, kKey + 5, 2, 1.0);
	rig.render(4096);
	const int a = rig.slot(kKey);
	const int b = rig.slot(kKey + 5);
	REQUIRE(a >= 0);
	REQUIRE(b >= 0);
	std::array<int, 4> seen {};
	for (int part = 0; part < 4; ++part) {
		CAPTURE(part);
		const int standA = rig.synth->voiceStackStandForTest(a, part);
		CHECK(standA >= 0); // Linear over four has no zero, so every part stands off the puck
		CHECK(rig.synth->voiceStackStandForTest(b, part) == standA);
		seen[static_cast<size_t>(part)] = standA;
	}
	// FOUR PARTS, FOUR STANDS: equal offsets share, different ones do not.
	std::sort(seen.begin(), seen.end());
	CHECK(std::unique(seen.begin(), seen.end()) == seen.end());
	CHECK(rig.synth->standOverflowForTest() == 0);
}

TEST_CASE("stack stands: a part on a stand of its own bakes its own vowel; parts on one stand share the first's") {
	Rig rig;
	feathers::test::Routes routes;
	standInVowelCountry(rig);
	rig.set(ParamId::StackCount, 5.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::Step)); // Step at 0: two levels, ±1
	routeIndexTo(rig, routes, ParamId::PosX, 0.005f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(8192);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	// Step with two levels over five parts rounds half away from zero: −1, −1, 0, +1, +1.
	const int s0 = rig.synth->voiceStackStandForTest(slot, 0);
	const int s1 = rig.synth->voiceStackStandForTest(slot, 1);
	REQUIRE(s0 >= 0);
	REQUIRE(s0 == s1);
	const feathers::OscVoice& p0 = rig.synth->voiceStackOscForTest(slot, 0);
	const feathers::OscVoice& p1 = rig.synth->voiceStackOscForTest(slot, 1);
	CHECK(p0.voxLenderForTest() == nullptr);
	CHECK(p1.voxLenderForTest() == &p0);
	CHECK(p0.voxBakesForTest() > 0);
	// The upper pair stands elsewhere: its first part bakes, the second reads it.
	const int s3 = rig.synth->voiceStackStandForTest(slot, 3);
	REQUIRE(s3 >= 0);
	REQUIRE(s3 != s0);
	CHECK(rig.synth->voiceStackOscForTest(slot, 3).voxLenderForTest() == nullptr);
	CHECK(rig.synth->voiceStackOscForTest(slot, 4).voxLenderForTest() == &rig.synth->voiceStackOscForTest(slot, 3));
	CHECK(rig.synth->voiceStackOscForTest(slot, 3).voxBakesForTest() > 0);
	CHECK(rig.synth->voiceStackOscForTest(slot, 4).voxBakesForTest() == 0);
}

TEST_CASE("stack stands: no route, no stands — a stack of eight keys no slot and builds only the puck's bank") {
	Rig rig;
	rig.set(ParamId::StackCount, 8.0);
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(55 + i), i + 1, 1.0);
	}
	rig.set(ParamId::PosX, 0.3); // and the puck moves, so the main bank rebuilds
	rig.render(4096);
	for (int16_t i = 0; i < 4; ++i) {
		const int slot = rig.slot(static_cast<int16_t>(55 + i));
		REQUIRE(slot >= 0);
		for (int part = 0; part < 8; ++part) {
			CHECK(rig.synth->voiceStackStandForTest(slot, part) == -1);
			CHECK(rig.synth->voiceStackOscForTest(slot, part).bankForTest() == &rig.synth->mainBankForTest());
		}
	}
	for (int s = 0; s < 16; ++s) {
		CHECK_FALSE(rig.synth->standBankForTest(s).hasTable()); // never asked for a character
	}
}


TEST_CASE("stack stands: nothing allocates — stands keyed, re-keyed, built, and parts moving between them") {
	Rig rig;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::PosX), 0.1f);
	routes.publish(rig.params);
	rig.set(ParamId::StackCount, 6.0);
	standInVowelCountry(rig);
	rig.render(1024);
	const long before = gAllocations.load();
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(55 + i), i + 1, 1.0);
	}
	rig.render(2048);
	rig.set(ParamId::StackShapeAmount, 0.5); // every stand moves: re-keyed in place
	rig.set(ParamId::PosX, 0.2);             // and the puck, so every bank rebuilds
	rig.render(2048);
	rig.set(ParamId::StackCount, 3.0);
	rig.render(2048);
	CHECK(gAllocations.load() - before == 0);
}

TEST_CASE("stack stands: the budgets hold — own-stand rebakes and stand builds per pass — and converge once still") {
	Rig rig;
	feathers::test::Routes routes;
	standInVowelCountry(rig);
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::VoiceCount, 16.0);
	routeIndexTo(rig, routes, ParamId::PosX, 0.05f);
	constexpr int kNotes = 6;
	for (int16_t i = 0; i < kNotes; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(55 + i), i + 1, 1.0);
	}
	rig.render(1024); // every part seated on its stand, every first bake made
	std::array<int, kNotes> slots {};
	for (int i = 0; i < kNotes; ++i) {
		slots[static_cast<size_t>(i)] = rig.slot(static_cast<int16_t>(55 + i));
		REQUIRE(slots[static_cast<size_t>(i)] >= 0);
	}
	const auto bakes = [&] {
		uint32_t n = 0;
		for (const int slot : slots) {
			for (int part = 0; part < 8; ++part) {
				n += rig.synth->voiceStackOscForTest(slot, part).voxBakesForTest();
			}
		}
		return n;
	};
	const auto builds = [&] {
		uint32_t n = 0;
		for (int s = 0; s < 16; ++s) {
			n += rig.synth->standBankForTest(s).generation();
		}
		return n;
	};
	// A RENDER OF TWO TICKS HOLDS THREE PASSES (the render's own, and each tick's), so its bound is three times the
	// per-pass one — and eight stands each rebuilding about once a guard (~260 samples) would pass it without the budget.
	// Linear over eight has no zero, so no part is on the puck and every bake here is an own-stand one.
	uint32_t movedBakes = 0;
	for (int t = 0; t < 100; ++t) {
		// THE SOUND MOVES, not the puck: the puck's motion smoother coasts on after a gesture (a main-bank matter, not
		// the stack's), so "still" below is only exact when the puck never moved.
		rig.set(ParamId::AOffset, 1.3563 + 0.01 * std::sin(0.4 * t));
		const uint32_t b0 = bakes();
		const uint32_t g0 = builds();
		rig.render(128, 128);
		CAPTURE(t);
		CHECK(bakes() - b0 <= 3u * 2u);
		CHECK(builds() - g0 <= 3u * 2u);
		movedBakes += bakes() - b0;
	}
	CHECK(movedBakes > 0); // not vacuous: the moving sound did make them rebake
	// STILL: within the round-robin's turn every part catches up, then nothing bakes.
	rig.render(64 * 200, 64);
	const uint32_t settled = bakes();
	const uint32_t built = builds();
	rig.render(64 * 20, 64);
	CHECK(bakes() == settled);
	CHECK(builds() == built);
}

TEST_CASE("stack stands: a move between two banks at the SAME generation is still a flip, with its fade") {
	// THE COINCIDENCE `moveToBank` GUARDS: generations are per bank, so a part can leave one bank for another whose
	// count happens to equal the one it last synced — and a sync keyed on the generation alone would call that "no
	// change" and go on playing the old bank's table. Two banks, each built once, are generation 1 both.
	using feathers::Character;
	using feathers::CharacterBank;
	using feathers::OscEngine;
	auto character = [](uint16_t zone) {
		Character c;
		c.engine = OscEngine::Morph;
		c.flagship = true;
		c.count = 1;
		c.sounds[0].zone = zone;
		c.sounds[0].weight = 1.0;
		return c;
	};
	CharacterBank from;
	CharacterBank to;
	from.setSampleRate(kRate);
	to.setSampleRate(kRate);
	from.setCharacter(character(100));
	to.setCharacter(character(700));
	REQUIRE(from.generation() == to.generation());
	feathers::OscVoice voice;
	voice.setCharacterBank(&from);
	voice.setSampleRate(kRate);
	voice.setFrequency(220.0);
	std::array<float, 128> out {};
	voice.render(out.data(), 128);
	REQUIRE_FALSE(voice.isFadingForTest());
	voice.moveToBank(&to);
	voice.render(out.data(), 8);
	CHECK(voice.isFadingForTest());
	CHECK(voice.freshGridForTest(0) == to.current().gridFor(voice.mipForTest()));
}

TEST_CASE("stack stands: sixteen moving stands rebuild no more than the build budget allows") {
	// EIGHT STANDS ARE HELD BY THEIR BANKS' OWN GUARDS (each rebuilds at most once per ~100 samples), so the budget is
	// only visible past them: two notes dealt their own index (Random Fresh) fill the pool's sixteen.
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
	rig.set(ParamId::StackShapeAmount, 1.0);
	routeIndexTo(rig, routes, ParamId::PosX, 0.05f);
	rig.synth->noteOn(0, 55, 1, 1.0);
	rig.synth->noteOn(0, 60, 2, 1.0);
	rig.render(1024);
	const auto builds = [&] {
		uint32_t n = 0;
		for (int s = 0; s < 16; ++s) {
			n += rig.synth->standBankForTest(s).generation();
		}
		return n;
	};
	int keyed = 0;
	for (int s = 0; s < 16; ++s) {
		keyed += rig.synth->standBankForTest(s).hasTable() ? 1 : 0;
	}
	REQUIRE(keyed == 16);
	uint32_t total = 0;
	for (int t = 0; t < 50; ++t) {
		rig.set(ParamId::AOffset, 0.5 + 0.2 * std::sin(0.4 * t)); // every stand's character moves
		const uint32_t g0 = builds();
		rig.render(128, 128);
		CAPTURE(t);
		CHECK(builds() - g0 <= 3u * 2u); // three passes, two a pass
		total += builds() - g0;
	}
	CHECK(total > 50); // and they did rebuild, every pass taking its turn
}

TEST_CASE("stack stands: a part crossing banks does not click — its largest step stays in the signal's own league") {
	// THE CLAIM IN ITS OWN TERMS (EQ A9's measure): across the move, no sample-to-sample step may exceed what the two
	// waveforms already do on their own. A hard switch between two characters at the same phase jumps by their
	// difference there, which towers over either one's slope; the 2 ms fade turns that into a ramp.
	using feathers::CharacterBank;
	using feathers::OscEngine;
	const auto character = [](float offset) { return feathers::test::characterAt(OscEngine::Morph, 17, offset); };
	auto voiceOn = [&](CharacterBank& bank, feathers::OscVoice& v) {
		v.setCharacterBank(&bank);
		v.setSampleRate(kRate);
		v.setFrequency(220.0);
	};
	const auto maxStep = [](const std::vector<float>& x) {
		float most = 0.0f;
		for (size_t i = 1; i < x.size(); ++i) {
			most = std::max(most, std::abs(x[i] - x[i - 1]));
		}
		return most;
	};
	CharacterBank a;
	CharacterBank b;
	a.setSampleRate(kRate);
	b.setSampleRate(kRate);
	// ZONE 17, THE SMOOTH NEAR-FUNDAMENTAL MORPH, at two offsets: each one a gentle slope, the two far apart at any one
	// phase — so a jump between them would tower over either slope, and the discrimination has teeth.
	a.setCharacter(character(0.0f));
	b.setCharacter(character(0.5f));
	a.clearModifiersForTest(); // no jitter or noise: the slopes are the waveforms' own
	b.clearModifiersForTest();

	// EACH WAVEFORM'S OWN MOTION, over the same stretch at the same pitch — and where the two stand furthest apart.
	constexpr int kRun = 2048;
	std::vector<float> ra(kRun);
	std::vector<float> rb(kRun);
	{
		feathers::OscVoice va;
		feathers::OscVoice vb;
		voiceOn(a, va);
		voiceOn(b, vb);
		va.render(ra.data(), kRun);
		vb.render(rb.data(), kRun);
	}
	const float own = std::max(maxStep(std::vector<float>(ra.begin() + 256, ra.end())),
	                           maxStep(std::vector<float>(rb.begin() + 256, rb.end())));
	REQUIRE(own > 0.0f);
	int worstAt = 256;
	for (int i = 256; i < 1536; ++i) {
		if (std::abs(ra[static_cast<size_t>(i)] - rb[static_cast<size_t>(i)])
		    > std::abs(ra[static_cast<size_t>(worstAt)] - rb[static_cast<size_t>(worstAt)])) {
			worstAt = i;
		}
	}
	// NOT VACUOUS: at the moment of the move the two are far apart, so a hard switch there would tower over both.
	CHECK(std::abs(ra[static_cast<size_t>(worstAt)] - rb[static_cast<size_t>(worstAt)]) > own * 3.0f);

	// THE MOVE, mid-note, from A to B, at exactly that moment.
	feathers::OscVoice moving;
	voiceOn(a, moving);
	std::vector<float> lead(static_cast<size_t>(worstAt));
	moving.render(lead.data(), worstAt);
	moving.moveToBank(&b);
	std::vector<float> across(512);
	moving.render(across.data(), 512);
	across.insert(across.begin(), lead.back());
	CHECK(maxStep(across) < own * 1.6f);
}

TEST_CASE("stack stands: the worst the guard allows — six notes of eight, and four stealing — all stand apart, none on the puck") {
	// THE POOL IS EVERY PART THAT CAN BE LIVE AT ONCE (step 4c): B = 48 parts sounding under the guard (6 notes of 8),
	// plus the four steal-fade spares each still fading a whole stack of 8 — 80. Random Fresh deals every note its own
	// index, so every part wants its own stand. None may be left on the puck, and nothing may overflow.
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::VoiceCount, 16.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
	rig.set(ParamId::StackShapeAmount, 1.0);
	routeIndexTo(rig, routes, ParamId::PosX, 0.05f);
	REQUIRE(feathers::Synth::standPoolForTest() == feathers::kOscillatorBudget + 4 * feathers::kMaxStack);
	for (int16_t i = 0; i < 6; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(40 + i), i + 1, 1.0);
	}
	rig.render(1024); // every part of the six seated on its own stand
	// FOUR MORE: the guard steals four of the six, which fade out over 2 ms still holding their stands.
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(60 + i), 10 + i, 1.0);
	}
	// Three short renders: three passes, enough for the note-on cap (16 new stands a pass) to seat 32 new parts, and
	// all inside the 2 ms steal fade.
	for (int k = 0; k < 3; ++k) {
		rig.render(8, 8);
	}
	int live = 0;
	int onStands = 0;
	std::vector<int> stands;
	for (int slot = 0; slot < 20; ++slot) {
		for (int part = 0; part < 8; ++part) {
			if (!rig.synth->voiceStackLiveForTest(slot, part)) {
				continue;
			}
			++live;
			const int stand = rig.synth->voiceStackStandForTest(slot, part);
			if (stand >= 0 && rig.synth->voiceStackOscForTest(slot, part).bankForTest()
			                      == &rig.synth->standBankForTest(stand)) {
				++onStands;
				stands.push_back(stand);
			}
		}
	}
	CHECK(live == 80);
	CHECK(onStands == 80);
	CHECK(rig.synth->standOverflowForTest() == 0);
	std::sort(stands.begin(), stands.end());
	CHECK(std::unique(stands.begin(), stands.end()) == stands.end()); // eighty parts, eighty stands
}

TEST_CASE("stack stands: the note-on cap — a chord's new stands build sixteen a pass, and a part waits on the puck") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::VoiceCount, 16.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
	rig.set(ParamId::StackShapeAmount, 1.0);
	routeIndexTo(rig, routes, ParamId::PosX, 0.05f);
	rig.render(256);
	for (int16_t i = 0; i < 6; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(40 + i), i + 1, 1.0);
	}
	const auto builtStands = [&] {
		int n = 0;
		for (int s = 0; s < feathers::Synth::standPoolForTest(); ++s) {
			n += rig.synth->standSlotForTest(s).hasTable() ? 1 : 0;
		}
		return n;
	};
	const auto seated = [&] {
		int n = 0;
		for (int16_t i = 0; i < 6; ++i) {
			const int slot = rig.slot(static_cast<int16_t>(40 + i));
			for (int part = 0; part < 8; ++part) {
				n += rig.synth->voiceStackOscForTest(slot, part).bankForTest() != &rig.synth->mainBankForTest() ? 1 : 0;
			}
		}
		return n;
	};
	// ONE PASS: 48 parts want 48 new stands; sixteen are built, and only their parts leave the puck.
	rig.render(1, 1);
	CHECK(builtStands() == 16);
	CHECK(seated() == 16);
	// TWO MORE PASSES and every part is on its own stand.
	rig.render(1, 1);
	rig.render(1, 1);
	CHECK(builtStands() == 48);
	CHECK(seated() == 48);
}

namespace {

/// TWO SOUNDS, BOTH WIDE, far apart in zone: terrain stereo is live and its two ears hear different blends.
void twoWideSounds(Rig& rig, double width) {
	using feathers::SoundField;
	using feathers::soundParam;
	rig.set(soundParam(1, SoundField::Enable), 1.0);
	rig.set(soundParam(1, SoundField::Zone), 600.0);
	rig.set(soundParam(0, SoundField::Width), 1.0);
	rig.set(soundParam(1, SoundField::Width), 1.0);
	rig.set(ParamId::Width, width);
}

} // namespace

TEST_CASE("stack stands: Width with a stack — each part hears one stand, its side its place in the fan, the ends exactly the ears") {
	// §6 A: at Stack 2 the parts stand at sides −1 and +1, so each must hear EXACTLY one of the main bank's two ears —
	// the independent reference, built by the terrain-stereo road that has always made them.
	Rig rig;
	rig.set(ParamId::StackCount, 2.0);

	twoWideSounds(rig, 1.0);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(2048);
	const int slot = rig.slot();
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth->mainBankForTest().stereoActive()); // terrain stereo is live
	CHECK(rig.synth->widthStandsForTest());
	const int low = rig.synth->voiceStackStandForTest(slot, 0);
	const int high = rig.synth->voiceStackStandForTest(slot, 1);
	REQUIRE(low >= 0);
	REQUIRE(high >= 0);
	CHECK(rig.synth->standOffsetForTest(low)[2] == -1.0);
	CHECK(rig.synth->standOffsetForTest(high)[2] == 1.0);
	// EarSide: Left is +1, Right is −1.
	CHECK(samePlayed(rig.synth->standBankForTest(low).current(0), rig.synth->mainBankForTest().current(1)));
	CHECK(samePlayed(rig.synth->standBankForTest(high).current(0), rig.synth->mainBankForTest().current(0)));
	// NOT VACUOUS: the two ears really differ here.
	CHECK_FALSE(samePlayed(rig.synth->mainBankForTest().current(0), rig.synth->mainBankForTest().current(1)));
	// AND EACH PART READS ITS STAND.
	CHECK(rig.synth->voiceStackOscForTest(slot, 0).bankForTest() == &rig.synth->standBankForTest(low));
	CHECK(rig.synth->voiceStackOscForTest(slot, 1).bankForTest() == &rig.synth->standBankForTest(high));
	// AND EACH RENDERS ONE STAND, MONO — the two ears per part are gone; Spread places them.
	CHECK_FALSE(rig.synth->voiceStackOscForTest(slot, 0).stereoStreamForTest());
	CHECK_FALSE(rig.synth->voiceStackOscForTest(slot, 1).stereoStreamForTest());

	SUBCASE("and the centre part of an odd stack hears the head, not the left ear") {
		rig.set(ParamId::StackCount, 3.0);
		rig.render(4096);
		const int centre = rig.synth->voiceStackStandForTest(slot, 1);
		REQUIRE(centre >= 0);
		CHECK(rig.synth->standOffsetForTest(centre)[2] == 0.0);
		// THE HEAD'S REFERENCE: the same patch with the terrain mono, whose one bank IS the head.
		Rig head;
		twoWideSounds(head, 0.0);

		head.synth->noteOn(0, kKey, 1, 1.0);
		head.render(2048);
		CHECK(samePlayed(rig.synth->standBankForTest(centre).current(0), head.synth->mainBankForTest().current(0)));
		CHECK_FALSE(samePlayed(rig.synth->standBankForTest(centre).current(0), rig.synth->mainBankForTest().current(0)));
	}
	SUBCASE("and Width back to 0 puts every part back on the puck") {
		rig.set(ParamId::Width, 0.0);
		rig.render(2048);
		CHECK_FALSE(rig.synth->widthStandsForTest());
		for (int part = 0; part < 2; ++part) {
			CHECK(rig.synth->voiceStackStandForTest(slot, part) == -1);
			CHECK(rig.synth->voiceStackOscForTest(slot, part).bankForTest() == &rig.synth->mainBankForTest());
		}
	}
}

TEST_CASE("stack stands: nothing allocates — width stands, the full pool, the note-on cap, and steals") {
	Rig rig;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::PosX), 0.05f);
	routes.publish(rig.params);
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::VoiceCount, 16.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
	rig.set(ParamId::StackShapeAmount, 1.0);
	twoWideSounds(rig, 1.0);
	rig.render(1024);
	const long before = gAllocations.load();
	for (int16_t i = 0; i < 6; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(40 + i), i + 1, 1.0);
	}
	rig.render(1024, 32);
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(60 + i), 10 + i, 1.0);
	}
	rig.render(2048, 32);
	rig.set(ParamId::Width, 0.0); // back to one bank: every part moves home
	rig.render(1024);
	CHECK(gAllocations.load() - before == 0);
}

TEST_CASE("stack coherence: 1 exactly on one bank, and on stands the power-weighted likeness of the cycles the parts play") {
	// **THE PARTS' 0-CENT COHERENCE** (`NoteContext::stackCoherence`, the Smooth Shaper's stack trim).
	// Restated here from the definition, not from the engine: one cycle of each part's table at 512 points, c_ij its mean
	// product, ρ0 = Σ_{i≠j} c_ij / ((N − 1) Σ_i c_ii). The engine reads 256 points; the two agree within 0.03.
	const auto restated = [](const Synth& synth, int slot, int count) {
		std::vector<std::vector<double>> w(static_cast<size_t>(count), std::vector<double>(512));
		for (int part = 0; part < count; ++part) {
			const feathers::OscVoice& osc = synth.voiceStackOscForTest(slot, part);
			const feathers::PreparedCharacter& table = osc.bankForTest()->current(0);
			for (int i = 0; i < 512; ++i) {
				w[static_cast<size_t>(part)][static_cast<size_t>(i)] = table.eval(osc.freshGridForTest(0), i / 512.0);
			}
		}
		double diag = 0.0;
		double off = 0.0;
		for (int a = 0; a < count; ++a) {
			for (int b = 0; b < count; ++b) {
				double c = 0.0;
				for (int i = 0; i < 512; ++i) {
					c += w[static_cast<size_t>(a)][static_cast<size_t>(i)] * w[static_cast<size_t>(b)][static_cast<size_t>(i)];
				}
				(a == b ? diag : off) += c / 512.0;
			}
		}
		return off / ((count - 1) * diag);
	};
	SUBCASE("a plain stack is one character: exactly 1") {
		Rig rig;
		rig.set(ParamId::StackCount, 8.0);
		rig.set(ParamId::StackDetune, 20.0);
		rig.synth->noteOn(0, kKey, 1, 1.0);
		rig.render(2048);
		// AND WITHOUT READING A CYCLE: one bank is one character, decided from the banks alone, so the answer is 1 by
		// construction rather than by eight identical cycles happening to multiply out to it.
		const uint64_t reads = rig.synth->coherenceCycleReadsForTest();
		CHECK(rig.synth->recomputeStackCoherenceForTest(rig.slot()) == 1.0f);
		CHECK(rig.synth->coherenceCycleReadsForTest() == reads);
	}
	SUBCASE("parts on stands of their own, by a Pos X route") {
		for (const float amount : { 0.1f, 0.25f, 0.5f }) {
			for (const int count : { 2, 4, 8 }) {
				CAPTURE(amount);
				CAPTURE(count);
				Rig rig;
				feathers::test::Routes routes;
				rig.set(ParamId::StackCount, static_cast<double>(count));
				routeIndexTo(rig, routes, ParamId::PosX, amount);
				rig.synth->noteOn(0, kKey, 1, 1.0);
				rig.render(4096);
				const int slot = rig.slot();
				REQUIRE(slot >= 0);
				REQUIRE(rig.synth->voiceStackStandForTest(slot, 0) >= 0); // standing apart, really
				const float got = rig.synth->recomputeStackCoherenceForTest(slot);
				CHECK(std::fabs(got - restated(*rig.synth, slot, count)) < 0.03);
				CHECK(got < 1.0f); // NOT VACUOUS: different stands are different sounds
				// AND ONLY A MOVED TABLE IS READ AGAIN: the reading the render takes next reads no cycle at all.
				const uint64_t reads = rig.synth->coherenceCycleReadsForTest();
				CHECK(rig.synth->updateStackCoherenceForTest(slot) == got);
				CHECK(rig.synth->coherenceCycleReadsForTest() == reads);
			}
		}
	}
	SUBCASE("and the puck moving re-reads the parts it rebuilt — the cache is keyed on the build, not the address") {
		Rig rig;
		feathers::test::Routes routes;
		rig.set(ParamId::StackCount, 4.0);
		routeIndexTo(rig, routes, ParamId::PosX, 0.25f);
		rig.synth->noteOn(0, kKey, 1, 1.0);
		rig.render(4096);
		const int slot = rig.slot();
		REQUIRE(slot >= 0);
		const float before = rig.synth->updateStackCoherenceForTest(slot);
		// Two moves, so each stand's bank flips twice and comes back to the ADDRESS it had — only the serial tells.
		for (const double x : { 0.3, -0.2 }) {
			rig.set(ParamId::PosX, x);
			rig.render(8192);
		}
		const uint64_t reads = rig.synth->coherenceCycleReadsForTest();
		float after = 0.0f;
		for (int call = 0; call < 4; ++call) { // one moved part per call: four calls converge four parts
			after = rig.synth->updateStackCoherenceForTest(slot);
		}
		CHECK(rig.synth->coherenceCycleReadsForTest() > reads);
		CHECK(std::fabs(after - restated(*rig.synth, slot, 4)) < 0.03);
		// AND IT IS THE READING A FRESH COMPUTATION TAKES, EXACTLY — a part whose stand came back to the same address
		// (an even number of rebuilds) must still have been re-read.
		CHECK(after == rig.synth->recomputeStackCoherenceForTest(slot));
		CHECK(after != before); // NOT VACUOUS: the stands moved to somewhere that sums differently
	}
	SUBCASE("and Width's mono stand per part") {
		for (const int count : { 2, 4, 8 }) {
			CAPTURE(count);
			Rig rig;
			rig.set(ParamId::StackCount, static_cast<double>(count));
			twoWideSounds(rig, 1.0);
			rig.synth->noteOn(0, kKey, 1, 1.0);
			rig.render(4096);
			const int slot = rig.slot();
			REQUIRE(slot >= 0);
			REQUIRE(rig.synth->widthStandsForTest());
			const float got = rig.synth->recomputeStackCoherenceForTest(slot);
			CHECK(std::fabs(got - restated(*rig.synth, slot, count)) < 0.03);
			CHECK(got < 1.0f);
		}
	}
}

TEST_CASE("stack coherence: nothing allocates — computed afresh for every voice, on stands and with Width") {
	Rig rig;
	feathers::test::Routes routes;
	routeIndexTo(rig, routes, ParamId::PosX, 0.3f);
	rig.set(ParamId::StackCount, 8.0);
	rig.set(ParamId::VoiceCount, 16.0);
	for (int16_t i = 0; i < 4; ++i) {
		rig.synth->noteOn(0, static_cast<int16_t>(48 + 5 * i), i + 1, 1.0);
	}
	rig.render(4096);
	const long before = gAllocations.load();
	for (int slot = 0; slot < feathers::kVoiceSlots; ++slot) {
		(void)rig.synth->recomputeStackCoherenceForTest(slot);
	}
	twoWideSounds(rig, 1.0);
	rig.render(2048, 32);
	for (int slot = 0; slot < feathers::kVoiceSlots; ++slot) {
		(void)rig.synth->recomputeStackCoherenceForTest(slot);
	}
	CHECK(gAllocations.load() - before == 0);
}

namespace {

/// A rig on a PLAYING song: every block is told where it starts, as the binding tells the synth, so `at` seconds is
/// where the next sample lands. `locate` is a host seek or a loop wrapping.
struct SongRig : Rig {
	double at = 0.0;
	void locate(double seconds) {
		at = seconds;
		synth->setTransport(at, std::numeric_limits<double>::quiet_NaN(), true); // before a note-on lands there
	}
	void play(int frames, int block = 256) {
		for (int done = 0; done < frames; done += block) {
			const int n = std::min(block, frames - done);
			synth->setTransport(at, std::numeric_limits<double>::quiet_NaN(), true);
			synth->render(left.data(), right.data(), n);
			at += static_cast<double>(n) / kRate;
		}
	}
	void arrange(feathers::test::Routes& routes) {
		set(ParamId::StackCount, 4.0);
		set(ParamId::StackShape, static_cast<double>(StackShape::Drift));
		set(ParamId::StackShapeAmount, 1.0); // the fastest walk
		routeStackIndex(*this, routes, ParamId::FineTune, 0.05f);
	}
};

} // namespace

TEST_CASE("stack index (Q9): Drift is the same at the same place in the song — after a seek, a loop, and a different run of notes before it") {
	// THE REFERENCE: the note at 2 s, nothing before it.
	SongRig a;
	feathers::test::Routes routesA;
	a.arrange(routesA);
	a.locate(2.0);
	a.synth->noteOn(0, kKey, 1, 1.0);
	// THE OTHER PASS: other notes first (which advance the note-on-order stream the old draws came from), then a loop
	// back to 2 s and the same note. Everything rendered before it is a whole number of control ticks, so both rigs'
	// engine ticks fall on the same song ticks — the walk's timing, not only its seed, is then comparable to the sample.
	SongRig b;
	feathers::test::Routes routesB;
	b.arrange(routesB);
	b.locate(5.0);
	b.synth->noteOn(0, kKey + 3, 7, 1.0);
	b.synth->noteOn(0, kKey - 5, 8, 1.0);
	b.play(4096);
	b.synth->noteOff(0, kKey + 3, 7, 0.0);
	b.synth->noteOff(0, kKey - 5, 8, 0.0);
	b.play(4096);
	b.locate(2.0); // the loop wraps
	b.synth->noteOn(0, kKey, 1, 1.0);

	const int slotA = a.slot();
	const int slotB = b.slot();
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	// THE DRAWS, the value the claim is about: the same seed and stream for the same note at the same song tick.
	CHECK(a.synth->voiceStackDrawsForTest(slotA) == b.synth->voiceStackDrawsForTest(slotB));
	// AND THE WALK, block by block for ~2.7 s: every part's index identical.
	int compared = 0;
	int moved = 0;
	float first = 0.0f;
	for (int block = 0; block < 500; ++block) {
		a.play(256);
		b.play(256);
		for (int part = 0; part < 4; ++part) {
			const float va = a.synth->voiceStackIndexForTest(slotA, part);
			const float vb = b.synth->voiceStackIndexForTest(slotB, part);
			CHECK(va == vb);
			++compared;
			if (block == 0 && part == 0) {
				first = va;
			}
			moved += part == 0 && va != first ? 1 : 0;
		}
	}
	CHECK(compared == 2000);
	CHECK(moved > 0); // not vacuous: the walk walked
}

TEST_CASE("stack index (Q9): with no transport the draws come from the note-on-order stream — two notes, two seeds") {
	Rig rig;
	feathers::test::Routes routes;
	rig.set(ParamId::StackCount, 4.0);
	rig.set(ParamId::StackShape, static_cast<double>(StackShape::RandomFresh));
	routeStackIndex(rig, routes, ParamId::FineTune, 0.05f);
	rig.synth->noteOn(0, kKey, 1, 1.0);
	rig.render(256);
	const auto firstDraws = rig.synth->voiceStackDrawsForTest(rig.slot());
	rig.synth->noteOff(0, kKey, 1, 0.0);
	rig.render(4096);
	rig.synth->noteOn(0, kKey, 2, 1.0);
	rig.render(256);
	// The same key again, with no song to place it: a fresh draw, as before Q9.
	CHECK(rig.synth->voiceStackDrawsForTest(rig.slot()) != firstDraws);
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 3 — THE SYNTH'S CHAINS GROW AT THE DRAIN                               //
//------------------------------------------------------------------------------------------------//
//
// It lives in this binary for its allocation counter: the claim is that the audio thread allocates nothing
// while every chain buffer it copies into is swapped for a bigger one.

using namespace feathers;

TEST_CASE("unbounded devices step 3: the synth's chains grow at the drain, keep what they held, and the "
          "audio thread allocates nothing") {
	Rig rig;
	FxChain chain;
	REQUIRE(addFxDevice(&chain, fx::filter::kTraits.tag) != kNoPosition);
	REQUIRE(addFxDevice(&chain, fx::harm::kTraits.tag) != kNoPosition);
	rig.synth->setFxChain(chain);
	(void)rig.synth->provisionDevices(chain, rig.synth->modRackForTest()); // built here, where it may allocate
	rig.render(1024);
	const ModRack strip = rig.synth->modRackForTest();
	REQUIRE(strip.count >= 1); // the default patch's envelope, so the strip has something to carry
	// NOTHING PUBLISHED, so the drain has only the growth to take and the chains handed in stay the ones run.
	const rack::PublishedChainState published(kMaxDeviceSlots, 2);

	REQUIRE(rig.synth->requestChainCapacity(64, 40, kMaxDeviceSlots));
	CHECK(rig.synth->chainGrowthPendingForTest());
	// NOT YET: the buffers the audio thread runs are untouched until its drain.
	CHECK(rig.synth->fxChainCapacityForTest() == kMaxFxSlots);
	CHECK(rig.synth->stripCapacityForTest() == kMaxRackSlots);

	const long before = gAllocations.load();
	(void)rig.synth->adoptChains(published); // THE DRAIN
	rig.render(2048);
	CHECK(gAllocations.load() - before == 0);

	CHECK_FALSE(rig.synth->chainGrowthPendingForTest());
	CHECK(rig.synth->fxChainCapacityForTest() == 64);
	CHECK(rig.synth->stripCapacityForTest() == 40);
	CHECK(rig.synth->rackPlayheads().size() == 40);
	CHECK(rig.synth->fxDrivePeaks().size() == 64);
	// WHAT IT HELD, IT HOLDS — the same devices under the same serials, by the chains' own equality, which is
	// capacity-blind on purpose.
	CHECK(rig.synth->fxChainForTest() == chain);
	CHECK(rig.synth->modRackForTest() == strip);
	// AND THE BLOCKS AFTER THE DRAIN RAN FROM IT: the modulated view is rebuilt from the grown chain each block.
	CHECK(fxChainLength(rig.synth->modulatedFxChain()) == 2);
	CHECK(rig.synth->modulatedFxChain().device.size() == 64);

	// THE OLD BUFFERS GO BACK TO THIS THREAD TO BE FREED, once.
	CHECK(rig.synth->reapChainGrowth());
	CHECK_FALSE(rig.synth->reapChainGrowth());
	// A CAPACITY NEVER SHRINKS: asking for less is answered and changes nothing.
	CHECK(rig.synth->requestChainCapacity(8, 4, kMaxDeviceSlots));
	CHECK_FALSE(rig.synth->chainGrowthPendingForTest());
	CHECK(rig.synth->requestedFxCapacity() == 64);
}

TEST_CASE("stack coherence on Additive: parts with one spectrum, detuned across a partial-count boundary, read as alike") {
	// THE LIKENESS OF TWO PARTIALS PARTS IS THEIR SPECTRA'S (Task 9b quality round): each part's playing cycle sums its
	// partials at Newman's flat phases for a FIXED N (OscVoice::kPartialsCycleCap), the Nyquist gains alone carrying the
	// pitch difference. With N each part's own count, two parts either side of a count boundary summed at different
	// phases and read as unlike. Restated here: each part's cycle from its own table's spectrum at its own pitch, 256
	// points, ρ0 = Σ_{i≠j} c_ij / ((N − 1) Σ_i c_ii).
	using feathers::OscVoice;
	constexpr int kCap = OscVoice::kPartialsCycleCap;
	const auto countAt = [](const feathers::partials::PartialSpectrum& s, double hz, float* gain) {
		int n = 0;
		for (; n < kCap; ++n) {
			const auto w = static_cast<float>(2.0 * std::numbers::pi * hz * static_cast<double>(s.ratio[static_cast<size_t>(n)]) / kRate);
			const float f = feathers::partials::nyquistFade(w);
			if (!(f > 0.0f)) {
				break;
			}
			gain[n] = f;
		}
		return n;
	};
	bool found = false;
	for (int16_t key = 72; key <= 96 && !found; ++key) { // counts in the forties and below, where N matters most
		Rig rig;
		feathers::test::Routes routes;
		rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive));
		rig.set(ParamId::BEnable, 0.0); // one sound: every stand plays the same spectrum
		rig.set(ParamId::StackCount, 2.0);
		rig.set(ParamId::StackDetune, 50.0);
		routeIndexTo(rig, routes, ParamId::PosX, 0.25f);
		rig.synth->noteOn(0, key, 1, 1.0);
		rig.render(4096);
		const int slot = rig.slot(key);
		REQUIRE(slot >= 0);
		std::array<std::array<double, 256>, 2> w {};
		std::array<int, 2> counts {};
		for (int part = 0; part < 2; ++part) {
			const OscVoice& osc = rig.synth->voiceStackOscForTest(slot, part);
			const feathers::partials::PartialSpectrum& s = osc.bankForTest()->current(0).partials;
			float gain[feathers::partials::kMaxPartials] = {};
			counts[static_cast<size_t>(part)] = countAt(s, osc.frequencyHz(), gain);
			for (int j = 0; j < 256; ++j) {
				double sum = 0.0;
				for (int k = 0; k < counts[static_cast<size_t>(part)]; ++k) {
					sum += double(s.envAtRatio[k]) * double(s.fine[k]) * double(gain[k])
					       * std::sin(2.0 * std::numbers::pi * s.ratio[k] * j / 256.0 + std::numbers::pi * k * k / double(kCap));
				}
				w[static_cast<size_t>(part)][static_cast<size_t>(j)] = sum;
			}
		}
		if (counts[0] == counts[1]) {
			continue; // not across a boundary at this key: try the next
		}
		found = true;
		CAPTURE(key);
		CAPTURE(counts[0]);
		CAPTURE(counts[1]);
		REQUIRE(rig.synth->voiceStackStandForTest(slot, 0) >= 0); // on stands of their own, so the cycles are read
		double c[2][2] = {};
		for (int a = 0; a < 2; ++a) {
			for (int b = 0; b < 2; ++b) {
				for (int j = 0; j < 256; ++j) {
					c[a][b] += w[size_t(a)][size_t(j)] * w[size_t(b)][size_t(j)];
				}
			}
		}
		const double want = (c[0][1] + c[1][0]) / (c[0][0] + c[1][1]);
		const float got = rig.synth->recomputeStackCoherenceForTest(slot);
		CAPTURE(want);
		CAPTURE(got);
		CHECK(std::abs(double(got) - want) < 1e-4);
		CHECK(want > 0.95); // alike: one spectrum, the Nyquist gains apart
	}
	CHECK(found); // NOT VACUOUS: a key put the two parts either side of a count boundary below the cap
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	Rig rig;
	rig.synth->noteOn(0, 60, 1, 1.0);
	rig.render(1024);
	CAPTURE(peakOf(rig.left));
	REQUIRE(peakOf(rig.left) > 0.05f);
}
