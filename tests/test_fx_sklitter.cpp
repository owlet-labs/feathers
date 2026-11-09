/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
// FOR `performPushOf`, `blendPush` and the weights — the perform circle, which `sklitter.h` deliberately
// does not pull in. Its arithmetic has its own suite; the cases here are about the device playing it.
#include "fx/sklitter/perform.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <climits>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;

TransportClock at120bpm(double beats = 0.0, bool playing = true) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5); // 120 BPM
	c.songPositionBeats = beats;
	c.playing = playing;
	return c;
}

/// A ramp, so every frame is DISTINGUISHABLE from every other. Silence or a constant would make
/// "played the right frame" and "played any frame" the same observation.
void ramp(std::vector<float>& l, std::vector<float>& r, float start) {
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = start + static_cast<float>(i);
		r[i] = -(start + static_cast<float>(i));
	}
}

Settings burst(float rate = 0.5f) {
	Settings s;
	s.mode = Mode::Burst;
	s.enabled = true;
	s.engage = false;
	s.rate = rate;
	s.quantized = true;
	return s;
}

} // namespace

TEST_CASE("the slice length is the rate AND the tempo, asserted as a number") {
	// **THE VALUE, NOT A DIFFERENCE.** Two different slice lengths both render "some audio"; only the
	// number distinguishes a correct mapping from a plausible one. The reference names five divisions
	// at five evenly spaced knob positions with the 16th at the centre, so 0 is a quarter note, 0.5 a
	// sixteenth, 1 a sixty-fourth.
	Sklitter s;
	s.prepare(kRate, 4096);
	std::vector<float> l(64);
	std::vector<float> r(64);

	struct Case {
		float rate;
		double beats;
		const char* what;
	};
	for (const Case c : { Case { 0.0f, 1.0, "quarter" }, Case { 0.25f, 0.5, "eighth" },
	                      Case { 0.5f, 0.25, "sixteenth" }, Case { 0.75f, 0.125, "thirty-second" },
	                      Case { 1.0f, 0.0625, "sixty-fourth" } }) {
		CAPTURE(c.what);
		Settings cfg = burst(c.rate);
		cfg.engage = true;
		Sklitter fresh;
		fresh.prepare(kRate, 4096);
		fresh.setTransportClock(at120bpm());
		fresh.setTarget(cfg);
		ramp(l, r, 0.0f);
		fresh.processStereo(l.data(), r.data(), 64);
		// beats * secondsPerBeat * sampleRate, at 120 BPM => beats * 0.5 * 48000
		CHECK(fresh.sliceFramesForTest() == static_cast<size_t>(std::llround(c.beats * 0.5 * kRate)));
	}
}

TEST_CASE("tempo changes the slice length; the rate control alone does not decide it") {
	Sklitter fast;
	fast.prepare(kRate, 4096);
	fast.setTransportClock(TransportClock::fromSecondsPerBeat(0.25)); // 240 BPM
	Settings cfg = burst(0.5f);
	cfg.engage = true;
	fast.setTarget(cfg);
	std::vector<float> l(64);
	std::vector<float> r(64);
	ramp(l, r, 0.0f);
	fast.processStereo(l.data(), r.data(), 64);
	// a sixteenth at 240 BPM is half as long as at 120
	CHECK(fast.sliceFramesForTest() == static_cast<size_t>(std::llround(0.25 * 0.25 * kRate)));
}

TEST_CASE("Quantized snaps the rate to a division; unquantized lets it land between") {
	const auto lengthFor = [](float rate, bool quantized) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = burst(rate);
		cfg.quantized = quantized;
		cfg.engage = true;
		s.setTarget(cfg);
		std::vector<float> l(8);
		std::vector<float> r(8);
		ramp(l, r, 0.0f);
		s.processStereo(l.data(), r.data(), 8);
		return s.sliceFramesForTest();
	};

	// 0.30 is nearest the 0.25 step, so quantized it is an EIGHTH exactly.
	CHECK(lengthFor(0.30f, true) == lengthFor(0.25f, true));
	// Unquantized it is not -- that is the whole difference, and it is why an unquantized press "feels" unsynced
	// with Quantized off while still following the tempo.
	CHECK(lengthFor(0.30f, false) != lengthFor(0.25f, false));
	// And it still sits between the two neighbouring divisions rather than anywhere at all.
	CHECK(lengthFor(0.30f, false) < lengthFor(0.25f, false));
	CHECK(lengthFor(0.30f, false) > lengthFor(0.5f, false));
}

TEST_CASE("Quantize snaps the PRESS and then lets go of the knob") {
	// **CLASSIC RATE IS NOT QUANTIZED CONTINUOUSLY, AS IN THE REFERENCE FIRMWARE.** The port snapped
	// on every read, so a turn during a gesture stepped between five values. The firmware snaps once,
	// when the stutter begins: it remembers the snapped position and zeroes the parameter, and from then
	// on each block adds the remembered offset to a knob that moves freely.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(0.30f); // nearest the 0.25 step: an eighth
	cfg.quantized = true;
	cfg.engage = true;
	s.setTarget(cfg);

	std::vector<float> l(8);
	std::vector<float> r(8);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 8);

	// THE PRESS IS ON THE DIVISION — 0.30 became 0.25 and the capture is an eighth.
	CHECK(s.effectiveRateForTest() == doctest::Approx(0.25));

	// **AND THE KNOB IS FREE FROM THERE, WHICH IS THE HALF THAT WAS MISSING.** Moving 0.30 -> 0.34 is
	// +0.04 against a base of 0.25, so the effective rate is 0.29 — NOT 0.25 (a ladder holding it) and
	// NOT 0.34 (the snap forgotten).
	Settings moved = cfg;
	moved.rate = 0.34f;
	s.setTarget(moved);
	CHECK(s.effectiveRateForTest() == doctest::Approx(0.29));

	// FAR ENOUGH TO CROSS A DIVISION AND IT STILL DOES NOT SNAP: +0.20 from the press lands at 0.45,
	// which is nearer 0.5 than 0.25 and would be pulled there by a live ladder.
	Settings far = cfg;
	far.rate = 0.50f;
	s.setTarget(far);
	CHECK(s.effectiveRateForTest() == doctest::Approx(0.45));

	SUBCASE("and with Quantize off the press is not snapped either") {
		Sklitter open;
		open.prepare(kRate, 4096);
		open.setTransportClock(at120bpm());
		Settings raw = burst(0.30f);
		raw.quantized = false;
		raw.engage = true;
		open.setTarget(raw);
		std::vector<float> l2(8);
		std::vector<float> r2(8);
		ramp(l2, r2, 0.0f);
		open.processStereo(l2.data(), r2.data(), 8);
		CHECK(open.effectiveRateForTest() == doctest::Approx(0.30));
	}

	SUBCASE("a looper mode never asks — quantize is Burst's alone") {
		Sklitter loop;
		loop.prepare(kRate, 4096);
		loop.setTransportClock(at120bpm());
		Settings cfg2 = burst(0.30f);
		cfg2.mode = Mode::Shuffle;
		cfg2.quantized = true;
		cfg2.engage = true;
		loop.setTarget(cfg2);
		std::vector<float> l3(256);
		std::vector<float> r3(256);
		ramp(l3, r3, 0.0f);
		loop.processStereo(l3.data(), r3.data(), 256);
		CHECK(loop.effectiveRateForTest() == doctest::Approx(0.30));
	}
}

TEST_CASE("backlog GS: the published Rate names the EFFECTIVE position during a quantized gesture") {
	// **THE READOUT MUST SAY WHAT IS PLAYING, AND DURING A QUANTIZED GESTURE THAT IS NOT THE KNOB.** A
	// press at 0.30 snaps to the eighth (0.25); turning to 0.45 is +0.15 from there, so the device plays
	// 0.40 — while the knob says 0.45. Restated here: N = a whole note over the slice, 4 / 2^(-4p).
	const auto nAt = [](double position) { return 4.0 / std::exp2(-4.0 * position); };
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	std::vector<float> published;
	s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
		                                                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                                                   } });
	Settings cfg = burst(0.30f);
	cfg.quantized = true;
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(8);
	std::vector<float> r(8);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 8);
	Settings turned = cfg;
	turned.rate = 0.45f;
	s.setTarget(turned);
	s.processStereo(l.data(), r.data(), 8);
	REQUIRE(s.effectiveRateForTest() == doctest::Approx(0.40));
	s.publishReadings(); // this fixture drives `processStereo` directly, past the ops table
	REQUIRE(published.size() == kReadingCount);
	REQUIRE(published[static_cast<size_t>(Reading::Status)] != 0.0f);

	const float division = published[static_cast<size_t>(Reading::RateDivision)];
	CHECK(division == doctest::Approx(nAt(0.40)).epsilon(1e-5)); // 12.13: what is heard
	CHECK(division != doctest::Approx(nAt(0.45)).epsilon(1e-3)); // 13.93: what the knob says

	// AND THE CARD'S TEXT, through the descriptor's own hook over exactly these readings and fields.
	const Row row = rowOf(turned);
	struct View {
		const Row* row;
		const std::vector<float>* readings;
	} const view { &row, &published };
	const feathers::device::TextView tv {
		&view,
		[](const void* h, size_t i) { return (*static_cast<const View*>(h)->row)[i]; },
		[](const void* h) { return static_cast<const View*>(h)->readings->size(); },
		[](const void* h, size_t i) { return (*static_cast<const View*>(h)->readings)[i]; },
	};
	REQUIRE(kDescriptor.text != nullptr);
	char out[32] = {};
	REQUIRE(kDescriptor.text->format(static_cast<size_t>(Param::Rate), 0.45, tv, out, sizeof(out)));
	CHECK(std::string(out) == "1/" + std::to_string(std::lround(nAt(0.40)))); // 1/12, not 1/14

	SUBCASE("released, it reads the knob again") {
		Settings released = turned;
		released.engage = false;
		s.setTarget(released);
		// A release during the capture finishes it before the gesture is over — driven until it is.
		for (int block = 0; block < 2000 && s.statusForTest() != Status::Off; ++block) {
			s.processStereo(l.data(), r.data(), 8);
		}
		s.publishReadings();
		REQUIRE(published[static_cast<size_t>(Reading::Status)] == 0.0f);
		CHECK(published[static_cast<size_t>(Reading::RateDivision)] == doctest::Approx(nAt(0.45)).epsilon(1e-5));
	}
}

TEST_CASE("Burst passes audio through untouched while it is still recording") {
	// It records into the buffer before it plays anything; Burst is INAUDIBLE until the
	// slice is full. A device that started altering the signal immediately would be a different one.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(0.5f); // a sixteenth at 120 BPM == 6000 frames
	cfg.engage = true;
	s.setTarget(cfg);

	std::vector<float> l(64);
	std::vector<float> r(64);
	ramp(l, r, 100.0f);
	const std::vector<float> before = l;
	s.processStereo(l.data(), r.data(), 64);

	REQUIRE(s.sliceFramesForTest() == 6000u);
	CHECK(s.statusForTest() == Status::Recording);
	CHECK(l == before); // untouched
}

TEST_CASE("a looper mode reads its bar at one, whatever Rate is doing") {
	// RATE BUYS SLICES THERE, NOT A SPEED. The bar is `resolveBarFrames` and owes nothing to the Rate
	// knob, so a step computed against it would be a ratio between two unrelated numbers — and would
	// resample a bar that is supposed to play at pitch.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(1.0f);
	cfg.mode = Mode::Shuffle;
	cfg.engage = true;
	s.setTarget(cfg);

	std::vector<float> l(256);
	std::vector<float> r(256);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 256);
	REQUIRE(s.sliceFramesForTest() > 0);
	CHECK(s.rateStepForTest() == doctest::Approx(1.0));

	Settings moved = cfg;
	moved.rate = 0.0f;
	s.setTarget(moved);
	CHECK(s.rateStepForTest() == doctest::Approx(1.0));
}

TEST_CASE("bypass silences the effect and releases the gesture") {
	// Enabled is the BYPASS and Engage is the GESTURE. Bypassing must leave the signal alone -- not
	// merely stop the stutter -- and must not leave a half-finished gesture behind.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(1.0f);
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(1500);
	std::vector<float> r(1500);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 1500);
	REQUIRE(s.statusForTest() == Status::Playing);

	cfg.enabled = false;
	s.setTarget(cfg);
	std::vector<float> l2(4);
	std::vector<float> r2(4);
	ramp(l2, r2, 700.0f);
	const std::vector<float> before = l2;
	s.processStereo(l2.data(), r2.data(), 4);
	CHECK(l2 == before);                      // untouched
	CHECK(s.statusForTest() == Status::Off);  // and the gesture is gone, not paused
}

TEST_CASE("with Engage off the device is dry") {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(burst(0.5f)); // engage defaults false
	std::vector<float> l(32);
	std::vector<float> r(32);
	ramp(l, r, 5.0f);
	const std::vector<float> before = l;
	s.processStereo(l.data(), r.data(), 32);
	CHECK(l == before);
	CHECK(s.statusForTest() == Status::Off);
}

TEST_CASE("a host with no tempo still produces a slice rather than silence") {
	// A device that needed a tempo to exist would be mute in a host that offers none.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(TransportClock::unknown());
	Settings cfg = burst(0.5f);
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(64);
	std::vector<float> r(64);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 64);
	CHECK(s.sliceFramesForTest() > 0u);
	// 120 BPM assumed, so a sixteenth is the same 6000 frames.
	CHECK(s.sliceFramesForTest() == 6000u);
}

TEST_CASE("releasing Engage ends Burst, because Burst never latches") {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(1.0f);
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(1500);
	std::vector<float> r(1500);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), 1500);
	REQUIRE(s.statusForTest() == Status::Playing);

	cfg.engage = false;
	s.setTarget(cfg);
	std::vector<float> l2(4);
	std::vector<float> r2(4);
	ramp(l2, r2, 900.0f);
	const std::vector<float> before = l2;
	s.processStereo(l2.data(), r2.data(), 4);
	CHECK(s.statusForTest() == Status::Off);
	CHECK(l2 == before); // and it is dry again immediately
}

TEST_CASE("prepare allocates the ring from the host rate and reset does not free it") {
	Sklitter s;
	s.prepare(kRate, 4096);
	CHECK(s.ringForTest().capacity() == static_cast<size_t>(kRate * kLooperSeconds));
	const float* before = s.ringForTest().dataForTest();
	s.reset();
	CHECK(s.ringForTest().dataForTest() == before);
}

TEST_CASE("the device is registered and reachable by its tag") {
	CHECK(std::string_view(kTraits.tag) == "SKLITTER");
	REQUIRE(kDescriptor.audio != nullptr);
	CHECK(kDescriptor.audio->stateBytes == sizeof(Sklitter));
	CHECK(kDescriptor.audio->processStereo != nullptr);
	CHECK(kDescriptor.audio->setTransportClock != nullptr); // the sync road must be wired
	CHECK(kDescriptor.audio->processMono == nullptr);       // stereo only, by construction
	CHECK(kDescriptor.params.size() == static_cast<size_t>(Param::kCount));
}

//--------------------------------------------------------------------------------------------------//
// WHAT THE REGISTRY-WIDE SWEEPS WOULD HAVE CHECKED, AT A PROBE LONG ENOUGH TO SEE THIS DEVICE.       //
//--------------------------------------------------------------------------------------------------//
//
// `test_synth.cpp` and `test_fx_devices.cpp` ask every registered device these questions, and they
// render 256 to 512 frames. SKLITTER captures a whole slice before it plays a sample -- 1500 frames at
// its FASTEST division, 24000 at its slowest -- so those sweeps cannot see it and name it as a set
// they cannot see rather than passing it vacuously. The obligation does not disappear with the
// visibility: it moves here, where the probe can be as long as the device needs.
//
// MEASURED, which is why the probe was not simply lengthened for everyone: `test_fx_devices` already
// takes 69 s, and 12x to 47x of that exceeds the container's own command bound.

TEST_CASE("the same samples at any host block size") {
	// **THE CASE THAT CAUGHT A REAL DEFECT.** The first draft recorded a whole block and then asked
	// once whether it was time to play, so the threshold was crossed at a BLOCK boundary: the same
	// audio began stuttering at frame 1536 under 256-frame blocks and at 8192 under one long call.
	// That is the host changing the sound, which is the one thing a plugin may not do. The capture
	// threshold is counted per sample for exactly this reason.
	constexpr int kTotal = 4096;
	const auto render = [](int block) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = burst(1.0f); // a 64th: 1500 frames, so the transition lands mid-run
		cfg.engage = true;
		s.setTarget(cfg);
		std::vector<float> l(kTotal);
		std::vector<float> r(kTotal);
		ramp(l, r, 0.0f);
		for (int at = 0; at < kTotal; at += block) {
			const int n = std::min(block, kTotal - at);
			s.processStereo(l.data() + at, r.data() + at, n);
		}
		return l;
	};

	const std::vector<float> whole = render(kTotal);
	for (const int block : { 1024, 512, 256, 64, 37 }) { // 37: a partition that divides nothing
		CAPTURE(block);
		const std::vector<float> parts = render(block);
		REQUIRE(parts.size() == whole.size());
		int differing = 0;
		for (int i = 0; i < kTotal; ++i) {
			if (parts[i] != whole[i]) {
				++differing;
			}
		}
		CHECK(differing == 0);
	}

	// NON-VACUITY: the render must actually contain playback, or "identical under every partition" is
	// true of a device that did nothing at all.
	Sklitter probe;
	probe.prepare(kRate, 4096);
	probe.setTransportClock(at120bpm());
	Settings cfg = burst(1.0f);
	cfg.engage = true;
	probe.setTarget(cfg);
	std::vector<float> l(kTotal);
	std::vector<float> r(kTotal);
	ramp(l, r, 0.0f);
	probe.processStereo(l.data(), r.data(), kTotal);
	CHECK(probe.statusForTest() == Status::Playing);
	CHECK(probe.sliceFramesForTest() == 1500u);
}

TEST_CASE("GO: the scratch is the host's longest block, and a call AT it and one a frame PAST it render the samples "
          "a short-block host hears") {
	// **BACKLOG GO.** `prepare` carries the host's longest block, so the wet path is sized to it rather than to a
	// constant both scratch owners once picked by luck (4096). A host keeping its promise is rendered in ONE piece;
	// a call past it is cut to the scratch — the guard — and must sound exactly the same.
	constexpr int kMax = 1000;
	constexpr int kTotal = 4096;
	const auto render = [](int block) {
		Sklitter s;
		s.prepare(kRate, kMax);
		REQUIRE(s.scratchFramesForTest() == static_cast<size_t>(kMax)); // every buffer, not only the length
		s.setTransportClock(at120bpm());
		Settings cfg = burst(1.0f); // a 64th: 1500 frames, so the slice crosses a call at every block below
		cfg.engage = true;
		s.setTarget(cfg);
		std::vector<float> l(kTotal);
		std::vector<float> r(kTotal);
		ramp(l, r, 0.0f);
		for (int at = 0; at < kTotal; at += block) {
			const int n = std::min(block, kTotal - at);
			s.processStereo(l.data() + at, r.data() + at, n);
		}
		CHECK(s.statusForTest() == Status::Playing); // NOT VACUOUS: playback is in the render
		return std::make_pair(l, r);
	};

	const auto reference = render(37); // a short-block host, well inside the scratch
	for (const int block : { kMax, kMax + 1 }) {
		CAPTURE(block);
		const auto parts = render(block);
		int differing = 0;
		for (size_t i = 0; i < static_cast<size_t>(kTotal); ++i) {
			differing += (parts.first[i] != reference.first[i]) + (parts.second[i] != reference.second[i]);
		}
		CHECK(differing == 0);
	}
	// AND THE WET IS IN IT: the dry ramp is not what came out, so "identical" is not two untouched inputs.
	std::vector<float> dryL(kTotal);
	std::vector<float> dryR(kTotal);
	ramp(dryL, dryR, 0.0f);
	CHECK(reference.first != dryL);

	// THE CAP: an announcement is a promise, not a budget — INT32_MAX (legal in CLAP) costs 65536 frames a buffer.
	Sklitter huge;
	huge.prepare(kRate, std::numeric_limits<int32_t>::max());
	CHECK(huge.scratchFramesForTest() == static_cast<size_t>(feathers::device::kMaxScratchFrames));
	// AND NOTHING BEFORE `prepare`: no scratch, and a call renders nothing rather than writing past it.
	Sklitter unprepared;
	CHECK(unprepared.scratchFramesForTest() == 0);
}

TEST_CASE("reset means AS IF JUST SWITCHED ON, not merely with the buffer cleared") {
	// A device that forgets its audio but remembers it was mid-gesture is not reset. Run three ways:
	// fresh, with history, and with history then reset -- the third must equal the first.
	constexpr int kTotal = 4096;
	const auto run = [](bool withHistory, bool withReset) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = burst(1.0f);
		cfg.engage = true;
		s.setTarget(cfg);
		if (withHistory) {
			// LOUD AND DIFFERENT, so whatever it remembers is worth remembering -- and long enough to
			// have reached Playing, which is the state a weak reset would leave behind.
			std::vector<float> hl(kTotal);
			std::vector<float> hr(kTotal);
			ramp(hl, hr, 9000.0f);
			s.processStereo(hl.data(), hr.data(), kTotal);
			REQUIRE(s.statusForTest() == Status::Playing);
		}
		if (withReset) {
			s.reset();
		}
		std::vector<float> l(kTotal);
		std::vector<float> r(kTotal);
		ramp(l, r, 0.0f);
		s.processStereo(l.data(), r.data(), kTotal);
		return l;
	};

	const std::vector<float> fresh = run(false, false);
	const std::vector<float> carried = run(true, false);
	const std::vector<float> afterReset = run(true, true);

	// NON-VACUITY FIRST: if the history does not reach the probe, "reset cleared it" is true of a wire.
	int carriedDiffs = 0;
	for (size_t i = 0; i < fresh.size(); ++i) {
		if (carried[i] != fresh[i]) {
			++carriedDiffs;
		}
	}
	CHECK(carriedDiffs > 0);

	CHECK(afterReset == fresh);
}

TEST_CASE("a hot signal through extreme settings stays a finite signal") {
	// **A NaN POISONS THE WHOLE BUS FOR THE REST OF THE SESSION** and the user has no button for it.
	// Every control at each end of its declared range, with a hot input.
	constexpr int kTotal = 4096;
	for (size_t p = 0; p < static_cast<size_t>(Param::kCount); ++p) {
		for (const double end : { kParamDescs[p].min, kParamDescs[p].max }) {
			CAPTURE(p);
			CAPTURE(end);
			Row row = rowOf(burst(0.5f));
			row[static_cast<size_t>(Param::Engage)] = 1.0f;
			row[p] = static_cast<float>(end);
			Sklitter s;
			s.prepare(kRate, 4096);
			s.setTransportClock(at120bpm());
			s.setTarget(stateOf(row));

			std::vector<float> l(kTotal);
			std::vector<float> r(kTotal);
			for (int i = 0; i < kTotal; ++i) {
				l[i] = (i % 2 == 0) ? 4.0f : -4.0f; // hot and alternating
				r[i] = -l[i];
			}
			s.processStereo(l.data(), r.data(), kTotal);
			for (int i = 0; i < kTotal; ++i) {
				if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
					FAIL("non-finite at frame " << i << " for param " << p << " at " << end);
				}
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE SIX LOOPER MODES, THROUGH THE DEVICE.                                                          //
//--------------------------------------------------------------------------------------------------//

namespace {

Settings looper(Mode m, float rate = 0.5f) {
	Settings s;
	s.mode = m;
	s.enabled = true;
	s.engage = true;
	s.rate = rate;
	s.density = 1.0f;
	s.pWrite = 0.0f; // frozen, as the reference defaults
	return s;
}

/// **WHERE `cfg.macro = m; cfg.macroConfig = c;` WENT (2026-09-19).** Both controls are deleted — the
/// perform circle's centre is a zero push and a waypoint is a configuration at macro 1.0 — so the only
/// way to ask this device for a macro is to put the PUCK somewhere. This puts the E waypoint's
/// configuration at `config` and the puck at `(amount, 0)`.
///
/// **WHY THAT IS THE SAME QUANTITY AND NOT A NEW ONE.** Inside the diamond the weights are exactly
/// `centre = 1 − amount`, `wE = amount`, and the other three are +0, so the blended push is
/// `amount · macroPushOf(1.0, config)` — which is `macroPushOf(amount, config)` up to the order of two
/// multiplies, because every term is linear in the macro above the 0.01 gate. The ONE place the two
/// part company is BELOW that gate, where the old pair gave an exact zero on the three probability
/// terms and this gives `amount ×` a live push; no caller below uses an amount that small.
///
/// It leaves the other three waypoints at their declared configurations and gives them no weight, so
/// what a case built this way hears is one waypoint and the distance to it.
/// **AND THE PUCK GOES ON `kCfg2`'s OWN AXIS, WHICH IS A DIAGONAL SINCE 2026-09-19.** It used to be
/// `perfX = amount, perfY = 0` — due east, which WAS E's axis while the waypoints were cardinal. The
/// rotation (`perform.h`'s `kWaypointAxis`) moved E to the top-right, and due east became the boundary
/// between E and S: every case built this way silently got half of `cornerCfg[kCfg3]`'s push as well.
/// **THAT IS WHAT THE SEVEN REDS IN THIS FILE WERE** — a stale fixture, not a moved device, and exactly
/// the shape CLAUDE.md names ("test fixtures take values from the platform's contract"). Asking
/// `waypointAxis` rather than spelling a direction is what stops it happening to the next rotation.
/// **THE PUCK ON ONE WAYPOINT, WHOLE**, leaving the configurations alone. Spelled once so that *"put the
/// puck on E"* is one edit when the circle next turns, rather than the eight hand-written
/// `perfX = 1.0f`s this replaces.
void puckOn(Settings& s, Corner c) {
	const auto [ax, ay] = waypointAxis(c);
	s.perfX = ax;
	s.perfY = ay;
}

void withMacro(Settings& s, float amount, float config = 0.5f) {
	s.cornerCfg[kCfg2] = config;
	const auto [ax, ay] = waypointAxis(kCfg2);
	s.perfX = amount * ax;
	s.perfY = amount * ay;
}

/// **DRIVE A DEVICE WITH A MOVING TRANSPORT.** A looper mode waits for a beat boundary, so a fixture
/// that holds the playhead still leaves it in Standby for ever -- which is what the first draft of
/// these cases did, and it read as "the device is broken" rather than "the fixture is".
void drive(Sklitter& s, int frames, float startValue = 0.0f, double* beats = nullptr) {
	constexpr int kChunk = 4800; // a tenth of a beat at 120 BPM / 48 kHz
	double at = beats != nullptr ? *beats : 0.0;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	for (int done = 0; done < frames; done += kChunk) {
		const int n = std::min(kChunk, frames - done);
		l.assign(static_cast<size_t>(n), 0.0f);
		r.assign(static_cast<size_t>(n), 0.0f);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = startValue + static_cast<float>(done + i);
			r[static_cast<size_t>(i)] = -(startValue + static_cast<float>(done + i));
		}
		s.setTransportClock(at120bpm(at));
		s.processStereo(l.data(), r.data(), n);
		at += static_cast<double>(n) / (0.5 * kRate); // frames -> beats
	}
	if (beats != nullptr) {
		*beats = at;
	}
}

/// Drive a device until it is playing, with a ramp so every captured frame is distinguishable.
///
/// **PAST THE BAR, NOT UP TO IT.** The trigger needs the capture complete AND a beat boundary after
/// it, so driving exactly one bar leaves the device one block short of playing -- measured: it flips
/// on the chunk after 96000 frames, not on it. A fixture that stopped on the bar reported "still in
/// Standby" and read as a broken device.
Sklitter running(const Settings& cfg, int barFrames = 96000) {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, barFrames + 9600);
	return s;
}

/// **THE REFERENCE'S REPEAT HOLD AND TIME'S ZONE OVERRIDE, RESTATED FROM `stutterer.cpp` — not from
/// `slice_plan.h`'s `planUnderHold`, which is what it checks.** Ported 2026-09-23. The reference, per slot
/// setup in every looper mode but Repeat:
///
///     if (!isRepeat && scatterRepeatCounter > 0) { grain = scatterCachedGrain; scatterRepeatCounter--; }   // :690
///     else { grain = computeGrainParams(..., scatterSliceIndex, ...);
///            if (isTime) { grainLength = clamp(1 + (int)(zoneA * (N-1)), 1, N);                        // :703
///                          repeatSlices = clamp(1 + (int)(zoneB * (N-1)), 1, N); }
///            if (!isRepeat) { scatterCachedGrain = grain; scatterRepeatCounter = grain.repeatSlices - 1; } } // :749
///     ... nextSliceIndex = scatterSliceIndex + effectiveGrainLength;
///     if (nextSliceIndex >= scatterNumSlices) scatterRepeatCounter = 0;                                   // :1052
///
/// So a hold never crosses a bar, and the plan a step plays is found by walking its bar from step 0 —
/// `draw(i)` is the fresh plan at step i, `span(i, plan)` how many steps the slot at i covers. `counterAfter`,
/// if given, receives `scatterRepeatCounter` as it stands once the target's slot is set up, bar end included.
template <typename Draw, typename Span>
SlicePlan referenceHeldPlan(Mode mode, float zoneA, float zoneB, int32_t numSlices, int32_t target, Draw&& draw,
                            Span&& span, int32_t* counterAfter = nullptr) {
	SlicePlan cached {};
	int32_t counter = 0;
	for (int32_t step = 0;;) {
		SlicePlan grain;
		if (mode != Mode::Repeat && counter > 0) {
			grain = cached;
			--counter;
		}
		else {
			grain = draw(step);
			if (mode == Mode::Time) {
				grain.grainLength = std::clamp(1 + static_cast<int32_t>(zoneA * static_cast<float>(numSlices - 1)), 1,
				                               numSlices);
				grain.repeatSlices = std::clamp(1 + static_cast<int32_t>(zoneB * static_cast<float>(numSlices - 1)),
				                                1, numSlices);
			}
			if (mode != Mode::Repeat) {
				cached = grain;
				counter = grain.repeatSlices - 1;
			}
		}
		const int32_t next = step + std::max(1, span(step, grain));
		if (step >= target) {
			if (counterAfter != nullptr) {
				*counterAfter = next >= numSlices ? 0 : counter;
			}
			return grain;
		}
		step = next;
	}
}

} // namespace

TEST_CASE("a looper mode captures a BAR, not a division") {
	// Burst's rate is a slice LENGTH; a looper mode's is a slice COUNT, and what it cuts up is one
	// bar. At 120 BPM and 48 kHz a bar of four beats is 96000 frames -- asserted as the number,
	// because "it captured something" is true of any length.
	for (const Mode m : { Mode::Shuffle, Mode::Grain, Mode::Pattern, Mode::Time, Mode::Pitch }) {
		CAPTURE(static_cast<int>(m));
		const Sklitter s = running(looper(m));
		CHECK(s.sliceFramesForTest() == 96000u);
	}
	// Burst, for contrast, captures a sixteenth: 6000 frames.
	Settings burstOn = burst(0.5f);
	burstOn.engage = true;
	CHECK(running(burstOn, 96000).sliceFramesForTest() == 96000u / 16u);
}

TEST_CASE("Rate buys a slice COUNT in the looper modes") {
	CHECK(running(looper(Mode::Shuffle, 0.00f)).sliceCountForTest() == 1);
	CHECK(running(looper(Mode::Shuffle, 0.25f)).sliceCountForTest() == 4);
	CHECK(running(looper(Mode::Shuffle, 0.50f)).sliceCountForTest() == 16);
	CHECK(running(looper(Mode::Shuffle, 1.00f)).sliceCountForTest() == 32);
	// Pitch halves it, so its grains are long enough to hear as a pitch.
	CHECK(running(looper(Mode::Pitch, 1.00f)).sliceCountForTest() == 16);
}

TEST_CASE("the bar advances slice by slice, and wraps") {
	// **THE STEP IS A VALUE, NOT AN IMPRESSION.** A device that played one slice for ever and a device
	// that walked the bar would both render "some audio"; only the index tells them apart.
	Settings cfg = looper(Mode::Shuffle, 0.25f); // four slices of 24000 frames
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);

	drive(s, 96000 + 9600); // past the bar: the trigger wants a beat boundary after the capture
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.sliceCountForTest() == 4);
	const int32_t startedAt = s.sliceIndexForTest();

	// One slice's worth of playback must move the step on by the grain's length.
	drive(s, 24000);
	CHECK(s.sliceIndexForTest() != startedAt);

	// And a whole bar of playback returns it to where it began.
	for (int i = 0; i < 4; ++i) {
		drive(s, 24000);
	}
	CHECK(s.sliceIndexForTest() >= 0);
	CHECK(s.sliceIndexForTest() < s.sliceCountForTest());
}

TEST_CASE("the schedule the device resolved matches the one the plan asks for") {
    // The device must not invent its own placement: whatever `scheduleFor` says for this step and
    // this plan is where the read head goes. Checked against an independent call rather than against
    // the device's own previous answer.
	const Sklitter s = running(looper(Mode::Shuffle, 0.25f));
	const SliceSchedule want = scheduleFor(Mode::Shuffle, 0.0f, s.sliceIndexForTest(),
	                                       s.sliceCountForTest(), s.sliceFramesForTest(),
	                                       s.planForTest());
	CHECK(s.scheduleForTest().targetSlice == want.targetSlice);
	CHECK(s.scheduleForTest().startOffset == want.startOffset);
	CHECK(s.scheduleForTest().sliceLength == want.sliceLength);
}

TEST_CASE("pWrite at zero FREEZES the capture, and above zero lets it evolve") {
	// **THE DEFAULT IS FROZEN AND THAT IS A REAL CHOICE**, not an omission: a looper that overwrites
	// as fast as it reads has nothing left to scatter. Asserted on the RING rather than the output,
	// because the output is shaped by the envelope and would differ for other reasons.
	//
	// **COUNTED ACROSS THE BAR RATHER THAN PROBED AT ONE FRAME.** This read frame 10000 and asked
	// whether it had changed. That was a fair question while pWrite was a per-sample blend that wrote
	// everywhere; it is the wrong question now that the reference's own rules are in — the write is a
	// per-SLICE draw, it goes to the LINEAR bar position rather than the one being read, and it is
	// DUCKED whenever the two regions overlap. Under all three, any single frame may legitimately be
	// untouched, and the case went red on a slice that was correctly ducked.
	const auto changedFramesFor = [](float pWrite) {
		Settings cfg = looper(Mode::Shuffle, 0.25f);
		cfg.zoneA = 0.6f;
		cfg.zoneB = 0.8f;
		cfg.pWrite = pWrite;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		drive(s, 96000 + 9600); // capture: frame N holds the value N
		drive(s, 96000, 500000.0f); // a whole second bar of very different audio, while playing
		size_t changed = 0;
		for (size_t frame = 1000; frame < 95000; frame += 500) {
			float l = 0.0f;
			float r = 0.0f;
			s.ringForTest().readAt(frame, &l, &r);
			if (std::abs(l - static_cast<float>(frame)) > 1.0f) {
				++changed;
			}
		}
		return changed;
	};

	// FROZEN IS FROZEN: not one frame of the capture moved.
	CHECK(changedFramesFor(0.0f) == 0u);

	// **AND AT FULL pWRITE THE BAR MOVES — BUT LESS THAN A READER EXPECTS, AND THE REASON IS THE
	// POINT.** What gets written is `srcL`: the PRE-ENVELOPE SOURCE, which for a wet slice is the
	// GRAIN just read from the buffer, not the incoming audio. So pWrite is a tape loop that copies
	// material from the shuffled read position to the linear write position — the bar slowly becomes
	// a shuffled version of itself. Fresh input enters only through a DRY slice, which is the
	// exception the reference calls out: *"Fresh input has no feedback concern — we're not reading
	// what we just wrote."*
	//
	// I expected most of the bar to be replaced and wrote `> 40` of 188 probes; it measured 18. The
	// expectation was wrong, not the device.
	const size_t evolving = changedFramesFor(1.0f);
	CAPTURE(evolving);
	CHECK(evolving > 0u);

	SUBCASE("fresh input enters through the DRY slices, which is the road density opens") {
		// Density at zero makes every slice dry, so `srcL` is the input rather than the grain and the
		// overlap duck is skipped by design. THIS is the arrangement where a second bar of new audio
		// replaces the capture.
		Settings cfg = looper(Mode::Shuffle, 0.25f);
		cfg.zoneA = 0.6f;
		cfg.zoneB = 0.8f;
		cfg.pWrite = 1.0f;
		cfg.density = 0.0f;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		drive(s, 96000 + 9600);
		drive(s, 96000, 500000.0f);
		size_t changed = 0;
		for (size_t frame = 1000; frame < 95000; frame += 500) {
			float l = 0.0f;
			float r = 0.0f;
			s.ringForTest().readAt(frame, &l, &r);
			if (std::abs(l - static_cast<float>(frame)) > 1.0f) {
				++changed;
			}
		}
		CAPTURE(changed);
		CHECK(changed > 150u); // nearly every probe, the whole bar having been written over
	}
}

TEST_CASE("every looper mode renders a finite signal and stays inside the buffer") {
	// The sweep that would catch a read past the ring or a divide by zero in any mode, at both ends
	// of the rate.
	for (const Mode m : { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Grain, Mode::Pattern,
	                      Mode::Pitch }) {
		for (const float rate : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
			for (const float zone : { 0.0f, 0.4f, 0.9f, 3.4f }) { // 3.4 is a coordinate past its lap
				CAPTURE(static_cast<int>(m));
				CAPTURE(rate);
				CAPTURE(zone);
				Settings cfg = looper(m, rate);
				cfg.zoneA = zone;
				cfg.zoneB = zone;
				withMacro(cfg, 0.5f, 0.5f);
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTransportClock(at120bpm());
				s.setTarget(cfg);
				drive(s, 96000 + 9600);
				std::vector<float> l2(8192);
				std::vector<float> r2(8192);
				ramp(l2, r2, 1.0f);
				s.setTransportClock(at120bpm(5.0));
				s.processStereo(l2.data(), r2.data(), 8192);
				for (int i = 0; i < 8192; ++i) {
					if (!std::isfinite(l2[i]) || !std::isfinite(r2[i])) {
						FAIL("non-finite at " << i);
					}
				}
			}
		}
	}
}

TEST_CASE("wherever a delay is scheduled it is a quarter bar at 25, 50 or 100 percent") {
	// The reference makes the TIME fixed -- "always quarter bar, classic rhythmic delay" -- and the
	// LEVEL a bit shift: bits 1,2,3 become shifts 2,1,0, so 25%, 50%, 100%.
	//
	// **THE CONFIGURATION IS CHOSEN, NOT HOPED FOR, AND THAT TOOK TWO TRIES.** A delay needs
	// `shouldDelay` AND `delaySendBits > 0`, and MEASURED over a 1,312-plan sweep the pair coincides
	// in nine of them -- 0.7%. The first draft picked one Zone B and sampled about seventeen plans,
	// hit none, and its `if (delaySend > 0)` wrapper meant the assertion never ran: a known-bad
	// changing the time to a HALF bar stayed green against a test doing nothing. Zone B 0.725 with 32
	// slices schedules one at step 4 and another at step 12, which makes this deterministic rather
	// than statistical.
	Settings cfg = looper(Mode::Shuffle, 1.0f); // 32 slices of 3000 frames
	cfg.zoneB = 0.725f;
	cfg.zoneA = 0.5f;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.sliceCountForTest() == 32);

	int scheduled = 0;
	double beats = 5.0;
	for (int k = 0; k < 40; ++k) {
		drive(s, 3000, 1.0f, &beats); // one slice per pass
		CAPTURE(k);
		CAPTURE(s.sliceIndexForTest());
		if (s.delaySendForTest() > 0.0f) {
			++scheduled;
			CHECK(s.delayTimeForTest() == 96000u / 4u); // a quarter bar, always
			// **AGAINST THE PLAN'S OWN BITS, not against the set of legal levels.** Checking only
			// that the send is one of {0.25, 0.5, 1.0} let a known-bad that sent EVERY grain at full
			// level through, because 1.0 is a member of that set. The claim is the mapping: bits
			// 1,2,3 become shifts 2,1,0.
			const uint8_t bits = s.planForTest().delaySendBits;
			REQUIRE(bits >= 1u);
			REQUIRE(bits <= 3u);
			CHECK(s.delaySendForTest()
			      == doctest::Approx(1.0f / static_cast<float>(1u << (3u - bits))).epsilon(1e-6));
		}
		else {
			CHECK(s.delayTimeForTest() == 0u);
		}
	}
	CHECK(scheduled > 0); // the non-vacuity guard the first draft was missing
}

TEST_CASE("the echo outlives the grain that sent it") {
	// **WHY THE DELAY NEEDS ITS OWN RING.** A send that shared the loop buffer would be overwritten
	// by the next slice, so the echo would vanish exactly when it should be heard. Driven with the
	// input silenced after the capture: anything still sounding is the delay line.
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneB = 0.6f;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);

	// The ring still holds the captured bar, so playback continues regardless; what this asserts is
	// that the delay ring is a DIFFERENT object with its own contents.
	CHECK(s.ringForTest().capacity() == static_cast<size_t>(kRate * kLooperSeconds));
	CHECK(s.delayTimeForTest() <= static_cast<size_t>(kRate * kDelaySeconds));
}

TEST_CASE("Grain runs two voices half a turn apart") {
	// **THE PHASE OFFSET IS THE WHOLE CROSSFADE.** As one triangle window closes the other is wide
	// open, so the cloud never gaps. If both voices shared a phase the output would pulse to silence
	// once per grain -- which is what this catches.
	Settings cfg = looper(Mode::Grain, 0.5f);
	cfg.zoneA = 0.8f; // a wide spread
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);

	// **ASSERTED AS THE DIFFERENCE, NOT AS A SILENCE.** A first draft measured the longest silent run
	// and a known-bad that set both voices to the SAME phase stayed green: the triangle window still
	// rises and falls, so a collapsed crossfade dips to zero for one sample rather than for a whole
	// grain. The gap is real but far too short for a run-length test to catch. The phases themselves
	// are the only honest witness.
	const auto halfTurnApart = [](const Sklitter& d) {
		double gap = d.grainPhaseBForTest() - d.grainPhaseAForTest();
		gap -= std::floor(gap); // both are [0,1), so compare them on the circle
		return gap;
	};
	CHECK(halfTurnApart(s) == doctest::Approx(0.5).epsilon(1e-9));

	// And it STAYS half a turn apart as the cloud runs -- both voices advance by the same increment,
	// so nothing but a bug can close the gap.
	for (int k = 0; k < 6; ++k) {
		std::vector<float> l(4000);
		std::vector<float> r(4000);
		ramp(l, r, 1.0f);
		s.setTransportClock(at120bpm(5.0 + k * 0.1));
		s.processStereo(l.data(), r.data(), 4000);
		CAPTURE(k);
		CHECK(halfTurnApart(s) == doctest::Approx(0.5).epsilon(1e-6));
		for (int i = 0; i < 4000; ++i) {
			REQUIRE(std::isfinite(l[i]));
		}
	}
}

TEST_CASE("backlog HR round 2: every grain onset is counted, in the gate class it plays under") {
	// **THE PERFORM CIRCLE'S ONSET SPARKS ARE ONLY AS HONEST AS THESE COUNTERS.** Asserted as values:
	// driven in blocks shorter than any grain, so a voice wraps at most once a block and the wrap is
	// visible as its phase going BACKWARDS — an independent witness of the onset, read off the voice and
	// not off the counter. After each block, the counter of exactly the class the wrapping voice's new
	// grain plays under must have moved by one, and no other.
	Settings cfg = looper(Mode::Grain, 0.55f);
	cfg.zoneA = 0.8f;
	cfg.zoneB = 0.9f;
	withMacro(cfg, 1.0f, 0.9f); // a pushed configuration, so the grains draw reverses
	cfg.density = 0.5f;         // and half the grains draw DRY (`grainDrawIsDry`)
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.grainLengthForTest() > 256u); // a block of 256 can hold at most one wrap per voice

	std::array<uint32_t, kGrainOnsetClasses> seen {};
	for (size_t c = 0; c < kGrainOnsetClasses; ++c) {
		seen[c] = s.grainOnsetCount(c);
	}
	std::array<size_t, kGrainOnsetClasses> perClass {};
	size_t wraps = 0;
	double beats = 5.0;
	std::vector<float> l(256);
	std::vector<float> r(256);
	for (int block = 0; block < 1500; ++block) {
		const double a0 = s.grainPhaseAForTest();
		const double b0 = s.grainPhaseBForTest();
		ramp(l, r, 1.0f + static_cast<float>(block));
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), 256);
		beats += 256.0 / (0.5 * kRate);
		std::array<size_t, kGrainOnsetClasses> want {};
		if (s.grainPhaseAForTest() < a0) {
			++want[s.grainGateClassForTest(false)];
		}
		if (s.grainPhaseBForTest() < b0) {
			++want[s.grainGateClassForTest(true)];
		}
		for (size_t c = 0; c < kGrainOnsetClasses; ++c) {
			const uint32_t now = s.grainOnsetCount(c);
			CAPTURE(block);
			CAPTURE(c);
			CHECK(now - seen[c] == want[c]);
			perClass[c] += want[c];
			wraps += want[c];
			seen[c] = now;
		}
	}
	// NOT VACUOUS: grains fired, and in more than one class — so a counter that put every onset in one
	// class, or that counted nothing, is red above rather than green by default.
	CAPTURE(perClass[0]);
	CAPTURE(perClass[1]);
	CAPTURE(perClass[2]);
	CAPTURE(perClass[3]);
	CHECK(wraps > 20u);
	CHECK(std::count_if(perClass.begin(), perClass.end(), [](size_t n) { return n > 0; }) >= 2);

	SUBCASE("and the readings carry the same numbers the device counted") {
		std::vector<float> published;
		struct Capture {
			static void publish(void* ctx, const float* values, size_t count) {
				auto* out = static_cast<std::vector<float>*>(ctx);
				out->assign(values, values + count);
			}
		};
		s.setReadingsSink({ &published, &Capture::publish });
		s.publishReadings();
		REQUIRE(published.size() == kReadingCount);
		for (size_t c = 0; c < kGrainOnsetClasses; ++c) {
			CAPTURE(c);
			CHECK(published[static_cast<size_t>(Reading::GrainOnsets) + c]
			      == static_cast<float>(s.grainOnsetCount(c)));
		}
	}
}

TEST_CASE("backlog HR round 2: every Repeat restart is counted, with the period it repeats at") {
	// **REPEAT SLICES GET THE SPLIT SPARK, ROUGHLY ONE PER REPEAT.** The perform
	// circle throws a splitting spark per repeat, paced by the period — so both are values. The restart is
	// witnessed independently by `repeatLoopIndexForTest` stepping (it advances once per restart), in
	// blocks shorter than the slot so a block holds at most one; the period against the frames counted
	// between two witnessed restarts.
	Settings cfg = looper(Mode::Repeat, 0.6f);
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);

	constexpr int kBlock = 32;
	uint32_t seen = s.repeatCount();
	size_t restarts = 0;
	long lastAt = -1;
	long frames = 0;
	std::vector<long> gaps;
	double beats = 5.0;
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	for (int block = 0; block < 6000; ++block) {
		const int32_t before = s.repeatLoopIndexForTest();
		ramp(l, r, 1.0f + static_cast<float>(block));
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kBlock);
		beats += kBlock / (0.5 * kRate);
		frames += kBlock;
		const uint32_t want = s.repeatLoopIndexForTest() != before ? 1u : 0u;
		CAPTURE(block);
		CHECK(s.repeatCount() - seen == want);
		seen = s.repeatCount();
		if (want != 0u) {
			++restarts;
			if (lastAt >= 0) {
				gaps.push_back(frames - lastAt);
			}
			lastAt = frames;
		}
	}
	REQUIRE(restarts > 10u); // NOT VACUOUS
	REQUIRE(!gaps.empty());
	// THE PERIOD: the frames between two witnessed restarts, to a block, over the sample rate.
	const double period = static_cast<double>(s.repeatPeriod());
	CAPTURE(period * kRate);
	CAPTURE(gaps.back());
	CHECK(std::abs(period * kRate - static_cast<double>(gaps.back())) <= kBlock);

	SUBCASE("the readings carry them, and Grain counts no repeats") {
		std::vector<float> published;
		s.setReadingsSink({ &published, [](void* ctx, const float* v, size_t n) {
			                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		                   } });
		s.publishReadings();
		REQUIRE(published.size() == kReadingCount);
		CHECK(published[static_cast<size_t>(Reading::RepeatCount)] == static_cast<float>(s.repeatCount()));
		CHECK(published[static_cast<size_t>(Reading::RepeatPeriod)] == s.repeatPeriod());

		const Sklitter g = running(looper(Mode::Grain, 0.55f));
		CHECK(g.repeatCount() == 0u);
	}
}

TEST_CASE("Grain's rate is a grain LENGTH, and Zone A is the spread") {
	// Rate means a third thing in this mode -- neither a division (Burst) nor a slice count (the
	// other loopers). Pinned because three meanings for one control is exactly what gets mixed up.
	const auto lengthAt = [](float rate, float zoneA) {
		Settings cfg = looper(Mode::Grain, rate);
		cfg.zoneA = zoneA;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		drive(s, 96000 + 9600);
		return s;
	};
	// CCW is long, CW is short -- and the floor stops it going harsh.
	const Sklitter slow = lengthAt(0.0f, 0.5f);
	const Sklitter fast = lengthAt(1.0f, 0.5f);
	CHECK(slow.statusForTest() == Status::Playing);
	CHECK(fast.statusForTest() == Status::Playing);
	// Both render finite audio at either end; the lengths differ by construction (squared curve).
	CHECK(slow.sliceFramesForTest() == 96000u);
	CHECK(fast.sliceFramesForTest() == 96000u);
}

TEST_CASE("Pitch mode reads its slice at a SCALE DEGREE, not only an octave") {
	// **PITCH MODE PLAYED ONLY OCTAVE JUMPS.** `kScaleSemitones[25][8]` and
	// `kPitchRatioFP[18]` came across verbatim into `decisions.h` and NOTHING READ EITHER of them, so
	// the only pitch movement in the device was `shouldPitchUp` — a 2x decimation the reference
	// applies in every mode, not Pitch mode's ladder. An octave, or nothing, which is the report.
	const auto ratioFor = [](Mode mode, float scale) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(mode, 0.25f);
		cfg.pitchScale = scale;
		s.setTarget(cfg);
		drive(s, 96000 + 9600);
		REQUIRE(s.statusForTest() == Status::Playing);
		return s.pitchRatioForTest();
	};

	// SCALE 18 IS THE PERFECT FIFTH AT EVERY DEGREE — `{7,7,7,7,7,7,7,7}` — so whichever degree the
	// hash draws, the semitone is 7 and the ratio is ONE NUMBER. That is what makes this an assertion
	// about the TABLE rather than about the hash: a fixed-interval row takes the draw out of the claim.
	//
	// **STATED AS 2^(7/12), NOT READ BACK OUT OF THE TABLE THE CODE READS.** Checking against
	// `kPitchRatioFP[7]` would pass for any table at all. The first draft of this line asserted 1.5 —
	// a JUST fifth — and went red at 1.49831, which is the EQUAL-TEMPERED one and is what the table
	// holds. The measurement was right and the expectation was wrong; the independent restatement is
	// the exponent.
	CHECK(ratioFor(Mode::Pitch, 18.0f) == doctest::Approx(std::exp2(7.0 / 12.0)).epsilon(0.0001));

	// SCALE 11 IS `{0,0,0,0,0,0,0,0}`, every degree the tonic. A port that had wired SOMETHING but not
	// the table would have to land on exactly 1.0 here by accident.
	CHECK(ratioFor(Mode::Pitch, 11.0f) == doctest::Approx(1.0));

	// AND THE LADDER IS PITCH'S ALONE — every other mode reads at 1 whatever the scale says.
	CHECK(ratioFor(Mode::Shuffle, 18.0f) == doctest::Approx(1.0));

	SUBCASE("and the ratio reaches the READ, interpolated rather than truncated") {
		// **THE SECOND ASSERTION, because a ratio nothing consumes is just a number in a member.** At
		// ~1.5 over a whole-number ramp the read advances about a sample and a half per output sample,
		// and the fraction lands BETWEEN two integers — which is the reference's stated reason for
		// interpolating: *"this avoids aliasing/bitcrushing artifacts from truncation."* A truncating
		// read can only ever hand back whole numbers from a whole-number ramp.
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Pitch, 0.25f);
		cfg.pitchScale = 18.0f;
		cfg.density = 1.0f;
		s.setTarget(cfg);
		double beats = 0.0;
		// **A RAMP THAT CHANGES SIGN EVERY SAMPLE**, not `drive`'s rising one. A Pitch slice at a ratio
		// other than unison does not continue the read of the slice before it, so it is not consecutive
		// and its crossing gate arms — and a ramp that never crosses zero keeps that gate shut for good,
		// which made this case read silence rather than a truncated read. Whole numbers still, so an
		// interpolated read between neighbours still lands between integers.
		{
			std::vector<float> dl(4800);
			std::vector<float> dr(4800);
			size_t n = 0;
			for (int done = 0; done < 96000 + 9600; done += 4800) {
				for (size_t i = 0; i < dl.size(); ++i, ++n) {
					dl[i] = static_cast<float>((n % 2 == 0) ? n : -static_cast<double>(n));
					dr[i] = dl[i];
				}
				s.setTransportClock(at120bpm(beats));
				s.processStereo(dl.data(), dr.data(), 4800);
				beats += 4800.0 / (0.5 * kRate);
			}
		}
		REQUIRE(s.pitchRatioForTest() == doctest::Approx(std::exp2(7.0 / 12.0)).epsilon(0.0001));

		std::vector<float> l(2048, 0.0f);
		std::vector<float> r(2048, 0.0f);
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), 2048);
		size_t fractional = 0;
		for (const float v : l) {
			if (v != 0.0f && std::abs(v - std::round(v)) > 0.01f) {
				++fractional;
			}
		}
		CAPTURE(fractional);
		CHECK(fractional > 0);
	}
}

/// A SINE, BECAUSE A ZERO CROSSING IS THE SUBJECT. The ramp the other cases here use never crosses
/// zero at all, so it cannot tell a working crossing search from one that never fires.
void driveSine(Sklitter& s, int frames, double* beats, int chunk = 4800, int phase = 0) {
	std::vector<float> l(static_cast<size_t>(chunk));
	std::vector<float> r(static_cast<size_t>(chunk));
	for (int done = 0; done < frames; done += chunk) {
		const int n = std::min(chunk, frames - done);
		for (int i = 0; i < n; ++i) {
			const double ph = 2.0 * 3.14159265358979 * 220.0 * (phase + done + i) / kRate;
			l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
		}
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		*beats += static_cast<double>(n) / (kRate * 0.5);
	}
}

TEST_CASE("a grain that does not flow on from the last one starts at a ZERO CROSSING") {
	// **THE NORMAL MODES' GRAINS CLICKED MORE THAN THE HARDWARE'S**: the transient handling and the
	// zero-crossing search had not been ported. The port had `grainEnvelope` and nothing else; the reference has three mechanisms and NONE of
	// the constants behind them (`kGrainReleaseZone`, `kBarEndZone`, `kBufferWrapFadeLen`,
	// `kMinGrainSize`) appeared anywhere in `src/fx/sklitter/`.
	//
	// **ASSERTED AS THE MECHANISM, NOT AS AN ABSENCE OF CLICKS.** "It sounds smoother" is not a claim
	// a test can make, and a spectral measure of a click would pass for any smoothing at all. The
	// claim is exact: while a channel is waiting for its crossing, that channel is SILENT.
	//
	// **AND CAUGHT AT THE FIRST GRAIN, WHICH IS THE ONE THAT IS DETERMINISTIC.** A first draft hunted
	// for a non-consecutive grain in a stochastic stream and found none inside its window, which
	// reads exactly like a gate that never arms. `reset` leaves `nextConsecutive_` false, so the
	// grain that starts when the capture completes ALWAYS arms — no search required.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	// **ZONE B PAST THE STRIDE DEADZONE AND ZONE A OFF ZERO**, so the plan actually varies from slice
	// to slice. At the origin every grain comes out consecutive — no offset, no reverse, no pitch-up —
	// and the gate is correctly switched off for all of them, which is a true statement about a
	// device sitting at its defaults and no test of this mechanism at all.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	withMacro(cfg, 0.5f);
	s.setTarget(cfg);

	double beats = 0.0;
	driveSine(s, 90000, &beats); // short of the bar: still filling
	REQUIRE(s.statusForTest() != Status::Playing);

	size_t soundedWhileWaiting = 0;
	size_t muted = 0;
	size_t clearedAt = 0;
	size_t gateRan = 0;
	bool playing = false;
	for (size_t n = 1; n <= 40000; ++n) {
		float l = 0.0f;
		float r = 0.0f;
		const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(90000 + n) / kRate;
		l = static_cast<float>(std::sin(ph));
		r = l;
		s.setTransportClock(at120bpm(beats));
		s.processStereo(&l, &r, 1);
		beats += 1.0 / (kRate * 0.5);
		if (s.statusForTest() != Status::Playing) {
			continue;
		}
		playing = true;
		// **ONLY THE GRAINS THE GATE ACTUALLY RUNS ON.** A consecutive grain with no envelope is the
		// input continuing and the protection is deliberately skipped; a dry grain is the input
		// itself, mixed in downstream of this gate. Including either would be asserting that a
		// mechanism correctly switched off is doing something.
		if (s.planForTest().useDry || !s.zcActiveForTest()) {
			continue;
		}
		// **NOR THE ENGAGE CROSSFADE**, during which the DRY input is audible by design — the wet this
		// gate silences is faded under it, and the claim below is about the wet.
		if (s.engageFadingForTest()) {
			continue;
		}
		++gateRan;
		if (s.waitingForZeroCrossForTest()) {
			++muted;
			if (l != 0.0f) {
				++soundedWhileWaiting;
			}
		}
		else if (clearedAt == 0 && muted > 0) {
			clearedAt = n;
		}
	}
	REQUIRE(playing);

	// THE CLAIM: nothing is heard from a channel still hunting for its crossing.
	CAPTURE(gateRan);
	CAPTURE(muted);
	CAPTURE(soundedWhileWaiting);
	CHECK(gateRan > 0);
	CHECK(soundedWhileWaiting == 0);

	// **AND THE MECHANISM RAN AT ALL**, which is what keeps the line above from being true of a
	// device that never arms — a gate that is never armed mutes nothing and passes it trivially.
	CHECK(muted > 0);
	// IT LET GO AGAIN. A permanent mute would be a worse defect than the click it replaces.
	CAPTURE(clearedAt);
	CHECK(clearedAt > 0);
}

TEST_CASE("the release window mutes at a crossing before the grain's edge, and the bar's") {
	// THE OTHER HALF OF THE SAME SYSTEM. The reference searches the last ~15 ms of a grain
	// (`kGrainReleaseZone`) and the last ~50 ms of the bar (`kBarEndZone`) for a crossing to stop at,
	// rather than stopping at the edge — where the waveform is wherever it happens to be.
	//
	// **THE WINDOWS ARE TIMES, NOT SAMPLE COUNTS.** The reference's constants are 44.1 kHz integers
	// whose own comments state the intent in milliseconds, and this device runs at the host's rate —
	// so at 48 kHz the release window is 720 samples, not 662, and a port that copied the integer
	// would search 11 ms at 96 k for a window sized to reach 33 Hz.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	withMacro(cfg, 0.5f);
	s.setTarget(cfg);

	double beats = 0.0;
	driveSine(s, 96000 + 9600, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);

	size_t releaseSeen = 0;
	size_t soundedWhileMuted = 0;
	for (size_t n = 0; n < 60000; ++n) {
		float a = 0.0f;
		float b = 0.0f;
		const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
		a = static_cast<float>(std::sin(ph));
		b = a;
		s.setTransportClock(at120bpm(beats));
		s.processStereo(&a, &b, 1);
		beats += 1.0 / (kRate * 0.5);
		if (s.planForTest().useDry || !s.zcActiveForTest()) {
			continue;
		}
		if (s.releaseMutedForTest()) {
			++releaseSeen;
			if (a != 0.0f) {
				++soundedWhileMuted;
			}
		}
	}
	CAPTURE(releaseSeen);
	CAPTURE(soundedWhileMuted);
	// ONCE A CHANNEL HAS TAKEN ITS RELEASE MUTE IT STAYS SILENT for the rest of that grain.
	CHECK(soundedWhileMuted == 0);
	// AND THE WINDOW WAS REACHED, or the line above is a claim about nothing.
	CHECK(releaseSeen > 0);
}

TEST_CASE("the bar counter advances, and the multi-bar branch it feeds was unreachable without it") {
	// **`PhaseOffsets::barIndex` WAS DECLARED, READ IN TWO PLACES, AND NEVER WRITTEN.** It sat at 0
	// for the life of the device, so `planFor`'s whole `if (ofs.barIndex != 0)` branch — the pair of
	// phi triangles that shift Zone A's phase from bar to bar — was unreachable code, and a four-bar
	// loop repeated exactly instead of evolving. The reference steps it at the wrap, `% kBarIndexWrap`.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	s.setTarget(cfg);

	double beats = 0.0;
	driveSine(s, 96000 + 9600, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);
	CHECK(s.barIndexForTest() == 0);

	// A WHOLE BAR OF PLAYBACK — 96000 frames at 120 BPM — must step it exactly once.
	driveSine(s, 96000, &beats, 4800, 96000);
	CHECK(s.barIndexForTest() == 1);

	// AND THE PLAN THE NEW BAR PRODUCES IS A DIFFERENT PLAN, which is the point of the counter and
	// the half a mere "it incremented" would miss.
	PhaseOffsets zero;
	PhaseOffsets one;
	one.barIndex = 1;
	PlanCache ca;
	PlanCache cb;
	CHECK_FALSE(planFor(0.6f, 0.8f, 0.0f, 0.0f, 0, zero, &ca) == planFor(0.6f, 0.8f, 0.0f, 0.0f, 0, one, &cb));

	SUBCASE("and it wraps rather than growing without bound") {
		// `kBarIndexWrap` is 64 — *"supports phrases up to 32 bars"* in the reference's own comment.
		// A device left running is the ordinary case for a looper, so this is not a theoretical bound.
		CHECK(kBarIndexWrap == 64);
		for (int bar = 0; bar < 64; ++bar) {
			driveSine(s, 96000, &beats, 4800, 96000 * (bar + 2));
		}
		CAPTURE(s.barIndexForTest());
		CHECK(s.barIndexForTest() >= 0);
		CHECK(s.barIndexForTest() < kBarIndexWrap);
	}
}

TEST_CASE("the captured loop is faded where it meets itself, which is a seam in the MATERIAL") {
	// **`kBufferWrapFadeLen` DID NOT COME ACROSS EITHER.** The reference's comment is the whole
	// argument: *"Apply fade at buffer wrap boundary (position 0) to eliminate ring buffer
	// discontinuity. Position 0 and bufSize-1 were recorded ~4s apart — fade once here instead of
	// per-sample."* Every other anti-click measure in this device works on the OUTPUT and runs per
	// sample; this one is a property of the RECORDING and is paid once, at the end of the capture.
	//
	// **AND IT IS THE LOOPER MODES' ALONE.** A looper bar is 2 s so 5 ms is a quarter of one percent,
	// while a Burst slice at a sixty-fourth is ~1500 frames and the same 5 ms is a sixth of it faded at
	// each end — a tremolo, not a declick.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.pWrite = 0.0f; // frozen, so what is read back is what the capture left
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	drive(s, 96000 + 9600); // frame N holds the value N

	const auto at = [&](size_t frame) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(frame, &l, &r);
		return l;
	};
	const size_t len = s.ringForTest().length();
	REQUIRE(len == 96000u);

	// THE TWO ENDS ARE PULLED TO ZERO — the first frame and the last, which are adjacent every time
	// the loop comes round.
	CHECK(at(0) == doctest::Approx(0.0));
	CHECK(at(len - 1) == doctest::Approx(0.0));

	// **AND THE FADE IS A RAMP, NOT A MUTE.** Halfway through the window the gain is a half: frame
	// 120 of a 240-frame fade at 48 kHz held 120, so it reads 60.
	const size_t fade = static_cast<size_t>(std::lround(0.005 * kRate));
	REQUIRE(fade == 240u);
	CHECK(at(fade / 2) == doctest::Approx(static_cast<double>(fade / 2) * 0.5).epsilon(0.02));

	// **AND THE REST OF THE BAR IS UNTOUCHED**, which is what makes this a seam treatment rather than
	// an envelope over the whole capture.
	CHECK(at(10000) == doctest::Approx(10000.0));
	CHECK(at(len / 2) == doctest::Approx(static_cast<double>(len / 2)));
	CHECK(at(len - fade - 1000) == doctest::Approx(static_cast<double>(len - fade - 1000)));
}

TEST_CASE("density is a per-slice HARD CUT, not a crossfade") {
	// **DENSITY HAD REGRESSED TO A SIMPLE MIX.** The render read
	// `l * density + input * (1 - density)` on every sample of every slice — so at 0.5 every grain
	// played at half volume over the input at half volume. The reference's own comment rules exactly
	// that out: *"Density threshold: hard cut between grain and dry (not a blend)."* Density thins
	// the grain STREAM by a per-slice hash; it does not fade it.
	//
	// **ASSERTED ON THE OUTPUT, BECAUSE THE FIRST DRAFT WAS ASSERTED ON THE DECISION AND THE KNOWN-BAD
	// WALKED STRAIGHT THROUGH IT.** `sliceIsDryForTest()` reports the per-slice choice correctly even
	// while the render blends downstream of it, so a case built on that seam is green for the exact
	// bug it was written for. What separates a cut from a blend is what comes OUT.
	//
	// THE INSTRUMENT: capture a sine, then feed a LOUD CONSTANT. A wet slice must carry no trace of
	// it — a blend at density 0.5 would put the output at half the constant, hundreds of times
	// anything the recorded sine can reach.
	constexpr float kLoud = 1000.0f;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.density = 0.5f;
	cfg.pWrite = 0.0f; // frozen, so the loud input cannot reach the buffer and confound this
	s.setTarget(cfg);

	double beats = 0.0;
	driveSine(s, 96000 + 9600, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);

	size_t wetSamples = 0;
	size_t drySlices = 0;
	size_t totalSlices = 0;
	float worstWet = 0.0f;
	int32_t lastSlice = -1;
	for (size_t n = 0; n < 96000; ++n) {
		float a = kLoud;
		float b = kLoud;
		s.setTransportClock(at120bpm(beats));
		s.processStereo(&a, &b, 1);
		beats += 1.0 / (kRate * 0.5);
		if (s.sliceIndexForTest() != lastSlice) {
			lastSlice = s.sliceIndexForTest();
			++totalSlices;
			if (s.sliceIsDryForTest()) {
				++drySlices;
			}
		}
		if (!s.sliceIsDryForTest()) {
			++wetSamples;
			worstWet = std::max(worstWet, std::abs(a));
		}
	}

	// **THE CLAIM: a wet slice is the GRAIN and nothing else.** The recorded material is a unit sine,
	// so anything above 1 is the loud input leaking in — and a blend at 0.5 would read about 500.
	CAPTURE(wetSamples);
	CAPTURE(worstWet);
	REQUIRE(wetSamples > 0);
	CHECK(worstWet <= 1.5f);

	// AND THE THINNING IS REAL: at 0.5 some slices went dry and some did not, which is what the
	// control is for. Both ends of that matter — all-wet would mean density does nothing, all-dry
	// would mean the device is off.
	CAPTURE(drySlices);
	CAPTURE(totalSlices);
	REQUIRE(totalSlices > 4);
	CHECK(drySlices > 0);
	CHECK(drySlices < totalSlices);
}

TEST_CASE("Burst is a GATE, not a loop") {
	// At the trigger it takes HALF the captured slice as a grain, then plays that grain at
	// NATIVE SPEED followed by silence until the cycle completes, over and over. Rate moves the
	// CYCLE, not the grain.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(1.0f); // a sixty-fourth at 120 BPM == 1500 frames
	cfg.mode = Mode::Burst;
	cfg.engage = true;
	s.setTarget(cfg);

	std::vector<float> l(1500);
	std::vector<float> r(1500);
	ramp(l, r, 1.0f); // frame i carries i+1, so no captured sample is zero
	s.processStereo(l.data(), r.data(), 1500);
	REQUIRE(s.statusForTest() == Status::Playing);

	// HALF THE CAPTURED SLICE, fixed at the trigger — the reference's `buffer.size() / 2`.
	CHECK(s.burstGrainForTest() == 750u);
	// AND THE CYCLE IS THE WHOLE SLICE while Rate has not moved.
	CHECK(s.burstCycleForTest() == 1500u);

	// **THE GATE ITSELF: SOUND, THEN SILENCE, AT THE RIGHT PLACES.** A loop would be sound
	// throughout, which is exactly what the old behaviour was.
	std::vector<float> out(1500, -1.0f);
	std::vector<float> outR(1500, -1.0f);
	s.processStereo(out.data(), outR.data(), 1500);
	size_t sounding = 0;
	size_t silent = 0;
	for (size_t n = 0; n < out.size(); ++n) {
		if (out[n] != 0.0f) {
			++sounding;
			// EVERY SOUNDING SAMPLE IS INSIDE THE GRAIN — the first 750 of the cycle.
			CHECK(n < 750u);
		}
		else {
			++silent;
		}
	}
	CAPTURE(sounding);
	CAPTURE(silent);
	CHECK(sounding > 700u);   // the grain played
	CHECK(silent > 700u);     // and the rest of the cycle did not

	SUBCASE("Rate moves the CYCLE and leaves the grain alone") {
		// This is the half that makes it a burst rather than a stutter: turning Rate up fires the
		// grain more often at the same pitch.
		const size_t grainBefore = s.burstGrainForTest();
		const size_t cycleBefore = s.burstCycleForTest();
		Settings faster = cfg;
		faster.rate = 0.75f; // one halving slower in division terms: 3000 frames asked for
		s.setTarget(faster);
		CHECK(s.burstGrainForTest() == grainBefore);
		CHECK(s.burstCycleForTest() != cycleBefore);
		// HALF THE STEP IS TWICE THE CYCLE — `rateStep` reads 0.5 here, so the cycle doubles.
		CHECK(s.burstCycleForTest() == cycleBefore * 2);
	}

	SUBCASE("the cycle has a floor, so a wild rate cannot make it audio-rate") {
		// The reference floors it at 64: *"Minimum to prevent audio-rate chaos"*.
		Settings wild = cfg;
		wild.rate = 0.0f; // the far end of the control
		s.setTarget(wild);
		CHECK(s.burstCycleForTest() >= 64u);
	}
}

TEST_CASE("pWrite writes to the LINEAR bar position, is a per-slice draw, and ducks on overlap") {
	// **THE THREE RULES THAT MADE THIS A DIFFERENT FEATURE FROM THE ONE THAT WAS HERE.** The port
	// blended the input into the buffer at the SHUFFLED read position, on every sample, in every
	// looper mode, always. Each clause of that is wrong.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.pWrite = 1.0f;
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);

	// **THE WRITE HEAD IS LINEAR.** *"Single buffer tape-loop: read from shuffled position, write to
	// linear position."* It walks the bar a sample at a time regardless of which slice is playing, so
	// after N samples of playback it has moved N — which a head that followed the shuffled read could
	// not do.
	const size_t before = s.linearBarPosForTest();
	drive(s, 5000, 1.0f);
	const size_t after = s.linearBarPosForTest();
	CHECK(((after + 96000 - before) % 96000) == 5000u);

	SUBCASE("at pWrite zero no slice ever writes") {
		Sklitter q;
		q.prepare(kRate, 4096);
		q.setTransportClock(at120bpm());
		Settings frozen = cfg;
		frozen.pWrite = 0.0f;
		q.setTarget(frozen);
		drive(q, 96000 + 9600);
		size_t writing = 0;
		for (int n = 0; n < 40; ++n) {
			drive(q, 4800, 1.0f);
			if (q.pWriteThisSliceForTest()) {
				++writing;
			}
		}
		// `threshold4(0, 0)` is `nibble < 0` — false for every nibble, so the draw can never pass.
		CHECK(writing == 0u);
	}

	SUBCASE("Repeat never writes, whatever pWrite says") {
		// `hasPWrite = isLooperMode && scatterMode != ScatterMode::Repeat`. Repeat is reading one
		// figure over and over; writing into it would erase the figure it exists to repeat.
		Sklitter q;
		q.prepare(kRate, 4096);
		q.setTransportClock(at120bpm());
		Settings rep = cfg;
		rep.mode = Mode::Repeat;
		rep.pWrite = 1.0f;
		q.setTarget(rep);
		drive(q, 96000 + 9600);
		size_t writing = 0;
		for (int n = 0; n < 40; ++n) {
			drive(q, 4800, 1.0f);
			if (q.pWriteThisSliceForTest()) {
				++writing;
			}
		}
		CHECK(writing == 0u);

		// AND THE SAME ARRANGEMENT IN SHUFFLE DOES WRITE, which is what makes the line above a claim
		// about Repeat rather than about this fixture.
		size_t shuffleWriting = 0;
		for (int n = 0; n < 40; ++n) {
			drive(s, 4800, 1.0f);
			if (s.pWriteThisSliceForTest()) {
				++shuffleWriting;
			}
		}
		CHECK(shuffleWriting > 0u);
	}
}

TEST_CASE("Repeat draws its plans from its OWN counter, not from the slice walk") {
	// **`grainIndex = isRepeat ? scatterRepeatLoopIndex : scatterSliceIndex`.** The reference bounds
	// Repeat's hash variation at eight — `& 0x7`, started at 1 *"for non-zero hash seed"* — while this
	// port drew from `sliceIndex_`, which walks however many repetitions fit the bar. A different
	// sequence of plans, and an unbounded one.
	//
	// **ASSERTED AS THE INDEX REACHING THE PLAN, because "the plans vary" does not distinguish the
	// two.** The first draft checked that more than one distinct plan appeared over a bar; they do
	// under both, since `sliceIndex_` advances too, and the known-bad passed it. What separates them
	// is WHICH index the plan was computed at, so the plan is recomputed here from the counter and
	// compared.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Repeat, 0.5f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	withMacro(cfg, 0.4f);
	s.setTarget(cfg);
	drive(s, 96000 + 9600);
	REQUIRE(s.statusForTest() == Status::Playing);

	// THE PLAN THE DEVICE IS PLAYING, RESTATED INDEPENDENTLY at the counter it reports.
	// **AND THE CENTRE'S SHARE OF ITS RATCHET (2026-09-23)**: the puck at 0.4 leaves the rest of the circle to
	// the centre, whose push carries `subdivMix` 0.5 (a provisional design value), summed before E's share as
	// the blend sums it.
	MacroPush push = macroPushOf(0.4f, Coordinate { 0.0f, 0.0f }, 0.0f);
	push.subdivMix = performWeightsOf(cfg.perfX, cfg.perfY).centre * 0.5f + push.subdivMix;
	const auto expectedAtCounter = [&]() {
		PhaseOffsets ofs;
		ofs.barIndex = s.barIndexForTest();
		PlanCache cold;
		return planFor(0.6f, 0.8f, 0.0f, push, s.repeatLoopIndexForTest(), ofs, &cold);
	};
	CHECK(s.planForTest() == expectedAtCounter());

	// AND IT KEEPS AGREEING AS THE COUNTER MOVES — checked across enough repetitions to wrap it.
	size_t checks = 0;
	size_t moved = 0;
	int32_t lastCounter = s.repeatLoopIndexForTest();
	for (int n = 0; n < 400; ++n) {
		drive(s, 480, 1.0f);
		if (s.repeatLoopIndexForTest() != lastCounter) {
			lastCounter = s.repeatLoopIndexForTest();
			++moved;
		}
		CHECK(s.planForTest() == expectedAtCounter());
		++checks;
	}
	CAPTURE(checks);
	CAPTURE(moved);
	// **THE COUNTER ACTUALLY MOVED**, or every check above compared one plan with itself.
	CHECK(moved > 1);
	// AND IT IS BOUNDED, which is the `& 0x7`.
	CHECK(s.repeatLoopIndexForTest() >= 0);
	CHECK(s.repeatLoopIndexForTest() < 8);
}

TEST_CASE("Grain gets the hard cut and a width that runs the way its comment says") {
	// **TWO DEFECTS IN ONE MODE, both found by the EH backlog sweep rather than by ear.**
	//
	// 1. THE HARD CUT NEVER REACHED GRAIN. `advanceSlice` settles `sliceIsDry_` for every looper mode,
	//    Grain among them, but the dispatch sends Grain to its own renderer and that one still ran the
	//    per-sample crossfade the reference rules out — *"hard cut between grain and dry (not a blend)"*.
	//    The fix landed on one road of two.
	// 2. THE WIDTH RAN BACKWARDS AND JUMPED. At 0 the formula gave voice A the left channel and B the
	//    right — full separation — and at 1 put both at half in both, which is mono; a special case at
	//    exactly 0 then forced mono. So it read mono at 0, near-hard-apart a hair above, and mono
	//    again at 1, with the default sitting at 0.5.
	// **CAPTURED FROM A SINE, IN PHASE ON BOTH CHANNELS, and both halves of that matter.** The shared
	// `drive` helper writes `r = -l` — the two channels are ANTI-PHASE by construction — so "width 0
	// is mono" measured against it reads the material's own inversion as a width and fails on a
	// correct device (measured: worst 17759 against a peak of 8879, which is exactly 2x). Its ramp
	// also reaches ~96,000 by the end of a bar, which breaks the density probe below for a second and
	// unrelated reason. A sine keeps the material bounded and the channels agreeing, so what the case
	// measures is the WIDTH and nothing else.
	const auto play = [](float width, float density) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Grain, 0.5f);
		cfg.zoneA = 0.6f;
		cfg.zoneB = 0.8f;
		cfg.stereoWidth = width;
		cfg.density = density;
		s.setTarget(cfg);
		double beats = 0.0;
		driveSine(s, 96000 + 9600, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		return s;
	};

	SUBCASE("width 0 is MONO — the two channels agree") {
		Sklitter s = play(0.0f, 1.0f);
		std::vector<float> l(2048, 0.0f);
		std::vector<float> r(2048, 0.0f);
		s.processStereo(l.data(), r.data(), 2048);
		// **THE CHANNELS ARE THE SAME SIGNAL.** Under the old arithmetic this was the one width that
		// was fully SEPARATED, so the two channels were different voices entirely.
		float worst = 0.0f;
		float peak = 0.0f;
		for (size_t i = 0; i < l.size(); ++i) {
			worst = std::max(worst, std::abs(l[i] - r[i]));
			peak = std::max(peak, std::abs(l[i]));
		}
		CAPTURE(peak);
		CAPTURE(worst);
		REQUIRE(peak > 0.0f); // it played at all, or "the channels agree" is a claim about silence
		CHECK(worst < peak * 0.01f);
	}

	SUBCASE("width 1 sends the voices APART — the channels disagree") {
		Sklitter s = play(1.0f, 1.0f);
		std::vector<float> l(2048, 0.0f);
		std::vector<float> r(2048, 0.0f);
		s.processStereo(l.data(), r.data(), 2048);
		float worst = 0.0f;
		float peak = 0.0f;
		for (size_t i = 0; i < l.size(); ++i) {
			worst = std::max(worst, std::abs(l[i] - r[i]));
			peak = std::max(peak, std::abs(l[i]));
		}
		CAPTURE(peak);
		CAPTURE(worst);
		REQUIRE(peak > 0.0f);
		// **THE TWO ENDS ARE THE OTHER WAY ROUND FROM WHAT SHIPPED**, which is the whole claim: this
		// end separates and the other does not.
		CHECK(worst > peak * 0.1f);
	}

	SUBCASE("density thins the cloud rather than fading it") {
		// A LOUD CONSTANT AS INPUT, and the grain material is a unit ramp: a wet sample carrying a
		// trace of the input means the crossfade is still there. Same instrument as the slice modes'
		// density case, for the same reason — the decision is upstream of anything a level can show.
		Sklitter s = play(0.5f, 0.5f);
		std::vector<float> l(4096, 1000.0f);
		std::vector<float> r(4096, 1000.0f);
		s.processStereo(l.data(), r.data(), 4096);
		size_t blended = 0;
		for (size_t i = 0; i < l.size(); ++i) {
			// A HARD CUT GIVES EITHER THE GRAIN OR EXACTLY THE INPUT. A blend gives neither: 1000
			// scaled by the density, which is a value no road produces on its own.
			const bool dry = l[i] == 1000.0f;
			const bool wet = std::abs(l[i]) < 500.0f;
			if (!dry && !wet) {
				++blended;
			}
		}
		CAPTURE(blended);
		CHECK(blended == 0u);
	}
}

TEST_CASE("the looper PRE-WARMS — a gesture plays a bar that is already in the ring") {
	// **THE LOOPER IS SUPPOSED TO PRE-WARM**, and it did not. The port began capturing when
	// Engage went high — a whole bar, then a wait for the next beat boundary — MEASURED in an
	// instrumented run of the real lane at four to five beats, over two seconds at 120 BPM, before a
	// note. The reference's `Stutterer::recordStandby` writes the incoming audio into the looper
	// buffer CONTINUOUSLY while the device is armed and not playing, so the gesture triggers against a
	// buffer that is already full.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings idle = looper(Mode::Shuffle, 0.25f);
	idle.engage = false; // armed, listening, NOT engaged
	s.setTarget(idle);

	// TWO BARS OF LISTENING. It is dry throughout — the ring fills, the output does not change.
	double beats = 0.0;
	driveSine(s, 96000 * 2, &beats);
	CHECK(s.statusForTest() == Status::Off);
	CHECK(s.prewarmReadyForTest());
	CAPTURE(s.prewarmedFramesForTest());
	CHECK(s.prewarmedFramesForTest() == s.sliceFramesForTest());

	SUBCASE("the captured bar is aligned to the SONG'S bar, not to whenever it started listening") {
		// **IT BEGINS IN PHASE, NOT FROM THE START OF THE CAPTURE.** MEASURED on the first version of the
		// pre-warm, which waited for the next BEAT: ring index `i` held song phase `i + 24000`, so the
		// captured bar sat ONE BEAT off the musical one. Every slice was then a beat displaced from
		// the grid, and a shuffle reordered material that was already in the wrong place — a loop that
		// will not lock however carefully the zones are set.
		//
		// **ASSERTED ON THE RING, because the OUTPUT cannot answer it.** Which frame comes out first
		// is the SHUFFLE's choice, not the alignment's — the first probe of this read the output and
		// was unreadable for exactly that reason. The question is what ring index 0 holds.
		Sklitter a;
		a.prepare(kRate, 4096);
		Settings warm = looper(Mode::Shuffle, 0.25f);
		warm.engage = false;
		a.setTarget(warm);

		// **A RAMP OFFSET SO NO SOURCE FRAME IS ZERO.** A reset ring reads 0 and so does source frame
		// 0 — one number meaning two opposite things, which made the first run of this probe
		// impossible to read. The offset is what separates "never written" from "written with zero".
		constexpr float kBase = 1000000.0f;
		long frame = 0;
		double beats = 0.0;
		const auto run = [&](long frames) {
			constexpr int kChunk = 480;
			std::vector<float> l(kChunk);
			std::vector<float> r(kChunk);
			for (long done = 0; done < frames; done += kChunk) {
				for (int i = 0; i < kChunk; ++i) {
					l[static_cast<size_t>(i)] = kBase + static_cast<float>(frame + i);
					r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
				}
				a.setTransportClock(at120bpm(beats));
				a.processStereo(l.data(), r.data(), kChunk);
				frame += kChunk;
				beats += static_cast<double>(kChunk) / (kRate * 0.5);
			}
		};
		run(96000 * 2);
		REQUIRE(a.prewarmReadyForTest());

		// **RING INDEX i HOLDS SONG PHASE i.** Frame 0 of the ring is the song's downbeat.
		const size_t bar = a.sliceFramesForTest();
		REQUIRE(bar == 96000u);
		for (const size_t at : { size_t { 480 }, size_t { 1000 }, size_t { 24000 }, size_t { 48000 },
		                         size_t { 72000 } }) {
			float l = 0.0f;
			float r = 0.0f;
			a.ringForTest().readAt(at, &l, &r);
			CAPTURE(at);
			REQUIRE(l != 0.0f); // written at all — see the offset note above
			const long phase = (static_cast<long>(l) - static_cast<long>(kBase)) % static_cast<long>(bar);
			CAPTURE(phase);
			CHECK(phase == static_cast<long>(at));
		}
	}

	// **AND THE PRESS PLAYS ALMOST AT ONCE.** A beat at 120 BPM is 24,000 frames; the gesture waits
	// for the next boundary and no longer. Before the pre-warm this took a bar PLUS a beat.
	//
	// **TWO BEATS DRIVEN, NOT ONE, AND THE MARGIN IS THE FIXTURE'S RATHER THAN THE DEVICE'S.**
	// `EngageMachine::advance` arms `armedBeat_ = beat` on its FIRST look and returns, so the
	// trigger needs a boundary to be CROSSED after that — driving exactly one beat lands on it, where
	// a float at 8.99999 and one at 9.0 decide differently. The claim is "about a beat, not about a
	// bar", and two beats states it without resting on the last ulp. (The same edge left my
	// instrumented run of the real lane sitting in Standby for ever when it drove a bar plus a beat.)
	Settings go = idle;
	go.engage = true;
	s.setTarget(go);
	driveSine(s, 48000, &beats, 480, 96000 * 2);
	CHECK(s.statusForTest() == Status::Playing);

	SUBCASE("and it is dry while it listens — the ring fills, the output does not change") {
		// A DEVICE THAT PRE-WARMS AUDIBLY WOULD BE A DEVICE THAT IS ON WHEN IT IS OFF. The listening
		// is a side effect the player must not be able to hear.
		Sklitter q;
		q.prepare(kRate, 4096);
		q.setTransportClock(at120bpm());
		Settings quiet = looper(Mode::Shuffle, 0.25f);
		quiet.engage = false;
		q.setTarget(quiet);
		double b = 0.0;
		driveSine(q, 96000, &b);

		std::vector<float> l(512);
		std::vector<float> r(512);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.5f * std::sin(0.03 * static_cast<double>(i));
			r[i] = l[i];
		}
		const std::vector<float> before = l;
		q.setTransportClock(at120bpm(b));
		q.processStereo(l.data(), r.data(), 512);
		for (size_t i = 0; i < l.size(); ++i) {
			CAPTURE(i);
			CHECK(l[i] == doctest::Approx(before[i]));
		}
	}

	SUBCASE("Burst does NOT pre-warm — it captures a division at the press") {
		// Pre-warming a BAR for a mode that records a few thousand frames at the press would write
		// over the slice it is about to record. The reference gates `recordStandby` on the looper
		// buffer existing at all, which is the same distinction.
		Sklitter c;
		c.prepare(kRate, 4096);
		c.setTransportClock(at120bpm());
		Settings cls = burst(0.5f);
		cls.engage = false;
		c.setTarget(cls);
		double b = 0.0;
		driveSine(c, 96000, &b);
		CHECK_FALSE(c.prewarmReadyForTest());
		CHECK(c.prewarmedFramesForTest() == 0u);
	}
}

TEST_CASE("the pattern is published BEFORE the gesture, and follows the knobs while it waits") {
	// **THE CARD CAN DRAW ITS PATTERN BEFORE THE GESTURE**, and it used to wait for Engage. The reason it waited was an implementation detail leaking into the
	// picture: the arrangement was published one step at a time AS EACH STEP PLAYED, because that is
	// where the device already computes it for nothing.
	//
	// **IT IS A PURE FUNCTION OF THE CONTROLS.** `planFor` and `scheduleFor` take the zones, the macro,
	// the mode and a step index — no captured audio, no playhead — so the whole arrangement can be
	// computed before a note is played.
	namespace sc = feathers::fx::sklitter;
	const auto readingsAtRest = [](float zoneA) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		// **ENOUGH SLICES THAT TWO PATTERNS CANNOT COINCIDE.** At rate 0.25 the bar is cut into FOUR,
		// and several of the eight orderings are the same permutation of four elements — so two
		// genuinely different zones produced an identical arrangement and the case read it as "the
		// knob does nothing". A finer cut separates them.
		Settings cfg = looper(Mode::Shuffle, 0.8f);
		cfg.engage = false; // never engaged
		cfg.zoneA = zoneA;
		cfg.zoneB = 0.8f;
		s.setTarget(cfg);
		std::vector<float> published;
		s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
			                                        static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		                                        } });
		double beats = 0.0;
		driveSine(s, 9600, &beats);
		REQUIRE(s.statusForTest() == Status::Off);
		// **PUBLISHED BY HAND, because this fixture drives `processStereo` DIRECTLY.** The ops table's
		// lambda is what calls `publishReadings` after the audio in the plugin; a case that reaches
		// past it gets the DSP without the publish, and reads an empty sink while the device is
		// working perfectly. (It reported `0 == 37` before this line existed.)
		s.publishReadings();
		return published;
	};

	const std::vector<float> rest = readingsAtRest(0.05f);
	REQUIRE(rest.size() == sc::kReadingCount);

	const auto steps = static_cast<size_t>(std::lround(rest[static_cast<size_t>(sc::Reading::Steps)]));
	CAPTURE(steps);
	REQUIRE(steps > 1);

	// **EVERY STEP IS KNOWN WITHOUT A NOTE HAVING PLAYED.** Before this, none were: the band drew a
	// row of empty slots and the player could not see what the zones were about to do.
	size_t known = 0;
	for (size_t k = 0; k < steps; ++k) {
		const size_t base = sc::stepReading(k);
		const auto flags = static_cast<uint32_t>(std::lround(rest[base + 1u]));
		if ((flags & static_cast<uint32_t>(sc::StepFlag::Known)) != 0u) {
			++known;
			CAPTURE(k);
			CHECK(rest[base] >= 0.0f);
			CHECK(rest[base] <= 1.0f);
		}
	}
	CHECK(known == steps);

	// **AND IT FOLLOWS THE KNOB.** Turning Zone A at rest changes the arrangement — which is the whole
	// value of drawing it early: the zones become explorable without committing to a gesture.
	const std::vector<float> moved = readingsAtRest(0.95f);
	REQUIRE(moved.size() == rest.size());
	bool differs = false;
	for (size_t k = 0; k < steps; ++k) {
		const size_t base = sc::stepReading(k);
		if (moved[base] != rest[base] || moved[base + 1u] != rest[base + 1u]) {
			differs = true;
		}
	}
	CHECK(differs);
}

TEST_CASE("the published picture is ONE arrangement, not a seam walking with the playhead") {
	// **THE SYMPTOM, on an installed build:** the displayed bar was overwritten as the playhead passed,
	// because the playhead spans one bar while the arrangement can repeat over four.
	//
	// THE MECHANISM, read before it was measured: `planFor` consumes the bar counter as TWO BITS
	// (`ofs.barIndex & 1`, `& 2`, slice_plan.h:236-240), so the arrangement evolves with a period of
	// exactly FOUR BARS — the field's own comment says "0-3, for multi-bar evolution". The picture,
	// though, is `kPictureSteps` slots indexed by `sliceIndex_`, which is a position WITHIN a bar, and
	// `advanceSlice` writes slot k as step k plays. So bar 0's step k and bar 1's step k are two
	// different plans sharing one slot, and the head rewrites each slot as it passes it.
	//
	// **WHAT THE PLAYER IS THEREFORE LOOKING AT IS NOT A PATTERN.** It is the most recent bar's
	// version of each step, assembled from up to four different bars — a seam that walks across the
	// band with the playhead. This case measures how many of the slots actually change, because "the
	// mechanism exists" and "you can see it" are different claims and only the second one is the bug.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	// **A RATE WHOSE SEQUENCE FITS THE PICTURE, DELIBERATELY.** Sixteen slices to the bar times four
	// bars is exactly `kPictureSteps`, so the band crams the whole sequence and never pages. At a
	// finer cut it MUST page — 32 slices over four bars is 128 slots against 64 — and a page turn is
	// legitimate churn rather than the smear this case is about. Asserted below, so the case cannot
	// quietly start measuring the wrong thing if the ladder or the picture size moves.
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.6f;
	cfg.engage = true;
	s.setTarget(cfg);

	std::vector<float> published;
	s.setReadingsSink(feathers::device::ReadingsSink { &published,
	                                                   [](void* ctx, const float* v, size_t n) {
		                                                   static_cast<std::vector<float>*>(ctx)
		                                                       ->assign(v, v + n);
	                                                   } });

	// ONE SNAPSHOT PER BAR, taken the moment the bar counter turns over. Driven in small chunks so
	// the boundary is not stepped over inside one call.
	struct Snap {
		int32_t bar = -1;
		std::vector<float> offsets;
		std::vector<uint32_t> flags;
	};
	std::vector<Snap> bars;
	int32_t lastBar = s.barIndexForTest();
	std::set<int32_t> visited;
	std::vector<size_t> visitedPerBar;
	double beats = 0.0;
	const auto capture = [&]() {
		s.publishReadings();
		if (published.size() != sc::kReadingCount) {
			return;
		}
		const auto steps =
		    static_cast<size_t>(std::lround(published[static_cast<size_t>(sc::Reading::Steps)]));
		Snap snap;
		snap.bar = s.barIndexForTest();
		for (size_t k = 0; k < steps && k < sc::kPictureSteps; ++k) {
			const size_t base = sc::stepReading(k);
			snap.offsets.push_back(published[base]);
			snap.flags.push_back(static_cast<uint32_t>(std::lround(published[base + 1u])));
		}
		bars.push_back(std::move(snap));
	};

	for (int block = 0; block < 1400; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
		visited.insert(s.sliceIndexForTest());
		const int32_t now = s.barIndexForTest();
		if (now != lastBar) {
			lastBar = now;
			visitedPerBar.push_back(visited.size());
			visited.clear();
			capture();
		}
		if (bars.size() >= 9) {
			break;
		}
	}

	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(bars.size() >= 5);
	MESSAGE("bars captured: " << bars.size() << ", steps in the picture: " << bars[0].offsets.size());

	// **HOW MANY SLOTS CHANGE FROM ONE BAR TO THE NEXT.** Every one of these is a slot the player
	// watched the head rewrite under it.
	size_t worst = 0;
	for (size_t i = 1; i < bars.size(); ++i) {
		const Snap& a = bars[i - 1];
		const Snap& b = bars[i];
		const size_t n = std::min(a.offsets.size(), b.offsets.size());
		size_t moved = 0;
		for (size_t k = 0; k < n; ++k) {
			if (a.offsets[k] != b.offsets[k] || a.flags[k] != b.flags[k]) {
				++moved;
			}
		}
		worst = std::max(worst, moved);
		MESSAGE("  bar " << a.bar << " -> " << b.bar << ": " << moved << " of " << n
		                 << " slots differ");
	}
	MESSAGE("worst bar-to-bar churn: " << worst << " slots");

	// **AND A SNAPSHOT FOUR BARS APART CANNOT ANSWER "IS THE PERIOD FOUR", WHICH IS WHY THIS CASE DOES
	// NOT ASK.** A first draft did, got 10 of 16 identical, and read it as "so the period is not four
	// after all" — which was a measurement answering a question nobody put to it.
	//
	// EVERY SNAPSHOT HERE IS ITSELF A MIXTURE. Taken the instant the counter turns over, slot 0 holds
	// the new bar and slots 1..15 still hold the old one, because a slot is only rewritten when the
	// head reaches it. So comparing two mixtures says nothing about the period of the thing being
	// mixed. **That the snapshot is a mixture IS THE BUG**, and the churn figure above is its size.
	//
	// The period is settled by READING instead, which is the right instrument for it: `planFor`
	// consumes the counter as two bits (slice_plan.h, `ofs.barIndex & 1` and `& 2`) and nothing else
	// in the plan or the schedule sees it, so the arrangement repeats every four bars by construction.
	//
	// **AND THE SECOND SOURCE OF STALENESS IS REAL, WHICH I FIRST RECORDED AS RULED OUT.** At a coarse
	// cut the head reaches every slot each bar and the only staleness is the bar mixing above. At a
	// FINE cut it does not: `sliceIndex_ += schedule_.grainLength`, so a grain spanning several slices
	// STEPS OVER the slots between, and those slots go on showing whatever bar last wrote them — which
	// may be many bars ago rather than one. MEASURED at 32 slices to the bar: the head visits
	// SEVENTEEN of them.
	//
	// So the picture had two independent ways of not being a picture, and a reading of the first one
	// had me assert that the second did not happen. Recorded as a number rather than as an assertion,
	// because the number is the thing that should go to zero.
	if (!visitedPerBar.empty()) {
		size_t lowest = visitedPerBar.front();
		for (const size_t v : visitedPerBar) {
			lowest = std::min(lowest, v);
		}
		MESSAGE("fewest distinct slots the head visited in one bar: " << lowest << " of "
		                                                             << bars[0].offsets.size());
		// **NOT AN ASSERTION THAT IT REACHES THEM ALL** — it does not, and that is the finding. What is
		// asserted is that the sampling saw SOMETHING, so a silent zero here is a broken fixture rather
		// than a device that never played.
		CHECK(lowest > 0);
	}

	// **THE CLAIM: NOT ONE SLOT CHANGES AS THE HEAD PASSES IT.**
	//
	// This case was written as a MEASUREMENT of the bug, with a note saying that when the picture was
	// published coherently the number would go to zero and the case should assert that instead. It
	// did, so it does. The numbers it read on the way, kept because they are what the report was
	// about: at 32 slices to the bar, TWENTY of 32 slots changed from one bar to the next; at 16,
	// TEN of 16.
	//
	// **ZERO IS THE ONLY ACCEPTABLE ANSWER AND IT IS NOT A TOLERANCE.** The arrangement is a pure
	// function of the controls, and nothing here moves a control — so any slot that differs between
	// two bars is the picture being assembled from more than one of them, which is the whole defect.
	CHECK(worst == 0u);

	// **AND NOT VACUOUS: THE HEAD REALLY DID CROSS BAR LINES.** A device that never advanced its bar
	// counter would also report zero churn, and would report it for the wrong reason.
	REQUIRE(bars.size() >= 5);
	CHECK(bars.back().bar != bars.front().bar);
	// THE PICTURE WAS NOT EMPTY EITHER — comparing two rows of zeroes is zero churn as well.
	CHECK(bars.front().offsets.size() > 1u);
	size_t known = 0;
	for (const uint32_t f : bars.front().flags) {
		if ((f & static_cast<uint32_t>(sc::StepFlag::Known)) != 0u) {
			++known;
		}
	}
	CHECK(known == bars.front().flags.size());

	// **AND THE HEAD IS ADDRESSED IN THE SEQUENCE, NOT IN THE BAR.** `Reading::Step` was `sliceIndex_`
	// alone — a position WITHIN the bar — so a device playing bar three drew its head in bar one, and
	// the band showed four bars with a head that only ever swept the first of them.
	//
	// **ASSERTED HERE BECAUSE NOTHING ELSE CAN.** The widget case publishes its readings BY HAND to
	// test the decode, so it never runs this arithmetic at all — a mutation that reverted the head to
	// `sliceIndex_` left that case GREEN. The engine is the only witness.
	s.publishReadings();
	REQUIRE(published.size() == sc::kReadingCount);
	const auto headSteps =
	    static_cast<int>(std::lround(published[static_cast<size_t>(sc::Reading::Steps)]));
	const auto head = static_cast<int>(std::lround(published[static_cast<size_t>(sc::Reading::Step)]));
	const auto seqBars =
	    static_cast<int>(std::lround(published[static_cast<size_t>(sc::Reading::SeqBars)]));
	// **AND THE BAND IS CRAMMING, NOT PAGING.** `SeqBars` is what is DRAWN; if it came back short of
	// the setting the picture would be a window that turns, and a turn is churn this case would
	// wrongly read as the smear.
	CHECK(seqBars == 4);
	REQUIRE(seqBars > 1);      // or "the head is in a later bar" is not a claim this can make
	REQUIRE(headSteps > 0);
	const int perBar = headSteps / seqBars;
	REQUIRE(perBar > 0);
	const int inBar = s.barIndexForTest() % seqBars;
	CAPTURE(head);
	CAPTURE(inBar);
	CAPTURE(perBar);
	// THE HEAD SITS INSIDE THE BAR THE DEVICE IS ACTUALLY IN — not merely somewhere in range.
	CHECK(head >= inBar * perBar);
	CHECK(head < (inBar + 1) * perBar);
	// **NOT VACUOUS: the walk really did reach a bar past the first**, or `inBar * perBar` is zero and
	// the check above is satisfied by the old behaviour too.
	CHECK(inBar > 0);

	// **AND THE CHUNK UNDER THE HEAD DESCRIBES THE SLICE ACTUALLY PLAYING.**
	//
	// This is the assertion that makes the walk answerable at all. Everything above says the picture is
	// STABLE and the head is in the right BAR; none of it says the picture is the RIGHT picture — a
	// walk that drew a coherent arrangement of the wrong thing would satisfy every one of them. Per
	// this repo's rule, the reference is an independent restatement of the intended path: the device's
	// OWN live schedule, which is computed by `advanceSlice` and not by the walk.
	//
	// MEASURED WHY IT IS NEEDED: a mutation handing the SEQUENCE index to `planFor` instead of the
	// position within the bar — drawing a sixty-four-slice bar the device never plays — left the churn
	// check green, because a wrong picture can be a perfectly stable one.
	REQUIRE(s.bufferFramesForTest() > 0);
	const size_t headBase = sc::stepReading(static_cast<size_t>(head));
	REQUIRE(headBase + 1u < published.size());
	const float drawn = published[headBase];
	const float live = static_cast<float>(s.scheduleForTest().startOffset)
	                   / static_cast<float>(s.bufferFramesForTest());
	CAPTURE(drawn);
	CAPTURE(live);
	CHECK(drawn == doctest::Approx(live).epsilon(1e-5));
}

TEST_CASE("SEQ LENGTH is how many bars the arrangement takes to repeat") {
	// **THE HONESTY SWEEP CALLED THIS CONTROL DEAD AND THE SWEEP WAS RIGHT ABOUT WHAT IT SAW.**
	// `test_fx_devices.cpp`'s probe drives a bar of capture and a tenth, and sequence length cannot
	// move the SOUND in bar zero: the plan reads `barIndex % seqBars`, and `0 % n` is 0 for every n.
	// That is a proof the instrument cannot speak, not a guess that it did not — so the value is
	// asserted here instead, and `test_fx_devices.cpp` carries the reasoning beside the entry.
	//
	// **AND IT IS ASSERTED INSIDE ONE PUBLISH, WHICH THE FIRST DRAFT COULD NOT DO.** That draft drove
	// two devices to bar 2 and compared their pictures — and once the band became the whole SEQUENCE
	// the comparison became vacuous, because step k of bar 0 is bar 0's arrangement whatever the
	// sequence length is. It read `0 of 16 differ` and would have read that for a control that did
	// nothing at all. The claim that actually distinguishes the rungs is about the picture's SHAPE:
	// **at one bar every bar of the sequence is the same bar; at four they differ.**
	namespace sc = feathers::fx::sklitter;
	const auto pictureFor = [](int32_t seqBars, float zoneB = 0.6f) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Shuffle, 0.5f); // sixteen to the bar: four of them cram exactly
		cfg.zoneA = 0.35f;
		cfg.zoneB = zoneB;
		cfg.seqBars = seqBars;
		cfg.engage = false; // at rest: the picture is a pure function of the controls
		s.setTarget(cfg);
		std::vector<float> published;
		s.setReadingsSink(feathers::device::ReadingsSink {
		    &published, [](void* ctx, const float* v, size_t n) {
			    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		    } });
		double beats = 0.0;
		driveSine(s, 9600, &beats);
		s.publishReadings(); // this fixture drives `processStereo` directly, past the ops table
		return published;
	};

	const std::vector<float> one = pictureFor(1);
	const std::vector<float> four = pictureFor(4);
	REQUIRE(one.size() == sc::kReadingCount);
	REQUIRE(four.size() == sc::kReadingCount);

	const auto stepsOf = [](const std::vector<float>& p) {
		return static_cast<size_t>(std::lround(p[static_cast<size_t>(sc::Reading::Steps)]));
	};
	const auto barsOf = [](const std::vector<float>& p) {
		return static_cast<int>(std::lround(p[static_cast<size_t>(sc::Reading::SeqBars)]));
	};

	// **THE BAND IS AS MANY BARS LONG AS THE SEQUENCE.** One rung draws one bar; four draws four, and
	// four times the steps. This is the visible half of the setting and the reason it is on the panel.
	CHECK(barsOf(one) == 1);
	CHECK(barsOf(four) == 4);
	CHECK(stepsOf(four) == stepsOf(one) * 4u);
	const size_t perBar = stepsOf(one);
	REQUIRE(perBar > 1);

	const auto chunk = [](const std::vector<float>& p, size_t k) {
		const size_t base = sc::stepReading(k);
		return std::make_pair(p[base], p[base + 1u]);
	};

	// **AT FOUR BARS THE BARS ARE DIFFERENT BARS.** If they were not, the setting would be a label on
	// a longer band showing the same thing four times — which is exactly what a broken fold produces,
	// and is indistinguishable from a working one by any check that only counts steps.
	size_t barsThatDiffer = 0;
	for (int b = 1; b < 4; ++b) {
		for (size_t k = 0; k < perBar; ++k) {
			if (chunk(four, k) != chunk(four, static_cast<size_t>(b) * perBar + k)) {
				++barsThatDiffer;
				break;
			}
		}
	}
	CAPTURE(barsThatDiffer);
	for (int b = 1; b < 4; ++b) {
		size_t n = 0;
		for (size_t k = 0; k < perBar; ++k) {
			if (chunk(four, k) != chunk(four, static_cast<size_t>(b) * perBar + k)) { ++n; }
		}
		MESSAGE("zoneB 0.6 — bar " << b << " differs from bar 0 in " << n << " of " << perBar
		                          << " chunks");
	}
	// **NOT ALL THREE, AND THAT IS THE REFERENCE RATHER THAN A DEFECT.** MEASURED at Zone B 0.6: bars 2
	// and 3 differ in 15 of 16 chunks and BAR 1 IS IDENTICAL TO BAR 0. The perturbation adds
	// `barBit0 * weight0 + barBit1 * weight1`, and both weights are `triangleSimpleUnipolar` of Zone B —
	// so a Zone B that lands one of those triangles on ZERO makes the bar that uses only that bit a
	// repeat of bar zero. Bar 1 uses bit 0 alone.
	//
	// **SO THE FLOOR IS "NOT UNIFORM", AND THE SECOND ZONE B BELOW IS WHAT MAKES THAT HONEST**: one
	// position cannot tell "this weight is zero here" from "this bit is never applied", and those are a
	// faithful port and a dead road respectively.
	CHECK(barsThatDiffer >= 2u);

	// **AND AT ANOTHER ZONE B, BAR 1 DOES MOVE.** This is the assertion that separates a weight landing
	// on zero from the bit being dead: if bar 1 matched bar 0 at EVERY Zone B, `barBit0` would be
	// reaching nothing and half the four-bar evolution would be a fiction.
	{
		const std::vector<float> other = pictureFor(4, 0.25f);
		REQUIRE(other.size() == sc::kReadingCount);
		REQUIRE(stepsOf(other) == perBar * 4u);
		size_t n = 0;
		for (size_t k = 0; k < perBar; ++k) {
			const auto a = std::make_pair(other[sc::stepReading(k)], other[sc::stepReading(k) + 1u]);
			const auto b = std::make_pair(other[sc::stepReading(perBar + k)],
			                              other[sc::stepReading(perBar + k) + 1u]);
			if (a != b) {
				++n;
			}
		}
		MESSAGE("zoneB 0.25 — bar 1 differs from bar 0 in " << n << " of " << perBar << " chunks");
		CHECK(n > 0u);
	}

	// **AND A TWO-BAR SEQUENCE REALLY DOES REPEAT AT BAR TWO, WHICH IS THE WHOLE MEANING OF THE
	// SETTING AND THE ONLY PLACE THE FOLD IS OBSERVABLE.**
	//
	// MEASURED WHILE HUNTING A GREEN KNOWN-BAD: deleting the `% seqBars` fold left every case above
	// passing, because `planFor` reads the counter as TWO BITS — so at four bars, bars 4..7 are bit-for
	// bit bars 0..3 and folding changes nothing. It is only a sequence SHORTER than four, played past
	// its own length, that can tell the difference. Without this the fold could be deleted outright.
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		cfg.zoneA = 0.35f;
		cfg.zoneB = 0.6f;
		cfg.seqBars = 2;
		cfg.engage = true;
		s.setTarget(cfg);
		std::vector<float> live;
		s.setReadingsSink(feathers::device::ReadingsSink {
		    &live, [](void* ctx, const float* v, size_t n) {
			    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		    } });
		double beats = 0.0;
		std::vector<float> atBarZero;
		std::vector<float> atBarTwo;
		for (int block = 0; block < 1400; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
			if (s.statusForTest() != Status::Playing) {
				continue;
			}
			if (atBarZero.empty() && s.barIndexForTest() == 0) {
				s.publishReadings();
				atBarZero = live;
			}
			if (atBarTwo.empty() && s.barIndexForTest() == 2) {
				s.publishReadings();
				atBarTwo = live;
				break;
			}
		}
		REQUIRE(atBarZero.size() == sc::kReadingCount);
		REQUIRE(atBarTwo.size() == sc::kReadingCount);
		REQUIRE(barsOf(atBarTwo) == 2); // cramming two bars, so a window turn is not in play
		const size_t two = stepsOf(atBarTwo);
		REQUIRE(two > 1);
		size_t moved = 0;
		for (size_t k = 0; k < two; ++k) {
			const size_t base = sc::stepReading(k);
			if (atBarZero[base] != atBarTwo[base] || atBarZero[base + 1u] != atBarTwo[base + 1u]) {
				++moved;
			}
		}
		MESSAGE("two-bar sequence, bar 0 against bar 2: " << moved << " of " << two << " chunks moved");
		CHECK(moved == 0u);
	}

	// **AND BAR ZERO IS THE SAME BAR AT EVERY RUNG**, which is the other half: a fold applied in the
	// wrong place would make the FIRST bar depend on the sequence length, and nothing in the reference
	// says it should.
	for (size_t k = 0; k < perBar; ++k) {
		CAPTURE(k);
		CHECK(chunk(one, k) == chunk(four, k));
	}
}

TEST_CASE("backlog EQ: switching INTO Grain arms Grain, without discarding the captured loop") {
	// **THE SYMPTOM, on an installed build:** switching from Shuffle to Grain left the grain frozen
	// until the device was disabled and re-enabled.
	//
	// **THE MECHANISM.** Grain's geometry — the grain length from Rate, the two-voice SPREAD from Zone
	// A, the two phases and the two read positions — is computed in ONE place: the branch that runs
	// when the gesture begins, gated on `engage_.armed().mode == Mode::Grain`. Arriving in Grain by
	// changing the MODE of a device that is already playing never runs it. So the cloud inherits the
	// construction defaults: `grainSpread_` is ZERO, which puts both voices on the same sample, and
	// `grainPosA_` and `grainPosB_` are both 0. Two voices reading one place from the top of the
	// buffer with a fixed 1024-sample grain is a short static stutter — "frozen", exactly.
	//
	// Disabling and re-enabling runs the gesture again with Grain armed, which is why that fixes it.
	//
	// **THE FIX MUST NOT BE `reset()`.** That is what the bypass road does and it DISCARDS THE CAPTURED
	// BUFFER — silencing a looper mid-performance, which is a worse bug than the one being fixed. The
	// buffer is the one thing a looper must keep across a mode change.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.7f; // a real spread to ask for, so zero is a claim rather than a coincidence
	cfg.engage = true;
	s.setTarget(cfg);
	double beats = 0.0;
	for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	// THE LOOP IS CAPTURED AND THE RING HOLDS IT. Sampled here so the check after the mode change is
	// against a real number rather than against "not zero".
	const size_t captured = s.bufferFramesForTest();
	REQUIRE(captured > 0);
	REQUIRE(s.ringForTest().length() == captured);

	// **A FINGERPRINT OF THE CAPTURED AUDIO**, taken before the mode moves. Spread across the loop so a
	// partial overwrite cannot slip between two probes.
	constexpr size_t kProbes = 8;
	std::array<size_t, kProbes> probeAt {};
	std::array<float, kProbes> beforeL {};
	std::array<float, kProbes> beforeR {};
	for (size_t k = 0; k < kProbes; ++k) {
		probeAt[k] = captured * k / kProbes;
		s.ringForTest().readAt(probeAt[k], &beforeL[k], &beforeR[k]);
	}

	// **NOW THE MODE MOVES UNDER THE PLAYING DEVICE**, which is what a player does with the dropdown.
	cfg.mode = Mode::Grain;
	s.setTarget(cfg);
	for (int block = 0; block < 60; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
	}
	REQUIRE(s.statusForTest() == Status::Playing); // the gesture survived the mode change

	CAPTURE(s.grainSpreadForTest());
	CAPTURE(s.grainPosAForTest());
	CAPTURE(s.grainPosBForTest());
	// **THE CLOUD IS TWO VOICES, NOT ONE.** Zone A at 0.7 asks for a spread across most of the loop;
	// zero is the unarmed default and is what "frozen" looks like from here.
	CHECK(s.grainSpreadForTest() > 0u);
	CHECK(s.grainPosAForTest() != s.grainPosBForTest());
	// AND THE GRAIN LENGTH FOLLOWS RATE rather than sitting on the header's 1024.
	CHECK(s.grainLengthForTest() > 1024u);

	// **AND THE BUFFER IS CARRIED INTO THE NEW MODE, SAMPLE FOR SAMPLE**, by design: a mode change
	// transfers the captured loop rather than overwriting it.
	//
	// **THE LENGTH SURVIVING IS NOT THE CLAIM.** A first draft checked only `length()` and
	// `bufferFramesForTest()`, which a device that silently re-recorded the same number of frames would
	// pass — the ring would be the right SIZE and full of the wrong audio, which is exactly the failure
	// the report describes and is inaudible to any check that counts rather than reads.
	CHECK(s.bufferFramesForTest() == captured);
	CHECK(s.ringForTest().length() == captured);
	size_t sameFrames = 0;
	for (size_t k = 0; k < kProbes; ++k) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(probeAt[k], &l, &r);
		if (l == beforeL[k] && r == beforeR[k]) {
			++sameFrames;
		}
	}
	CHECK(sameFrames == kProbes);
	// **NOT VACUOUS: the probes were not all silence.** A ring of zeroes compares equal to a ring of
	// zeroes, so "unchanged" would be true of a buffer that had been wiped.
	size_t nonSilent = 0;
	for (size_t k = 0; k < kProbes; ++k) {
		if (beforeL[k] != 0.0f || beforeR[k] != 0.0f) {
			++nonSilent;
		}
	}
	CHECK(nonSilent > 0u);
}

TEST_CASE("MEASURE: which controls the descriptor declares live in GRAIN actually reach the sound") {
	// **THE SYMPTOM, on the 80e81d76 install:** in Grain mode, no control changed the sound once the
	// device was engaged.
	//
	// **THE SHAPE, READ BEFORE IT WAS MEASURED.** `playGrain` consults `target_` for exactly TWO
	// things — Stereo Width and pWrite. Its geometry (`grainLength_` from Rate, `grainSpread_` from
	// Zone A) is captured when the mode is ARMED and never re-read; everything else the descriptor
	// declares relevant in Grain is not consulted at all.
	//
	// **AND THE FILE ALREADY CARRIES THE DECISION THIS BREAKS.** `advanceSlice`: on an earlier build no
	// control changed the pattern after it had started, and that was answered by
	// re-reading the zones and the rate at EVERY slice setup, which is what the reference's
	// `updateLiveParams` exists for. Grain never got that treatment: it is the same defect, one mode
	// over, and the FIFTH time in this device a fix has reached one road and stopped.
	//
	// This case does not assert the fix. It MEASURES which controls are dead in Grain, so the fix has
	// a list rather than an impression, and so the list is checkable afterwards.
	namespace sc = feathers::fx::sklitter;
	const auto renderWith = [](const std::function<void(Settings&)>& tweak) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Grain, 0.5f);
		cfg.zoneA = 0.4f;
		cfg.zoneB = 0.4f;
		withMacro(cfg, 0.5f, 0.4f);
		cfg.density = 1.0f;
		cfg.engage = true;
		s.setTarget(cfg);
		double beats = 0.0;
		// ENGAGE AND SETTLE IN GRAIN FIRST, so the change below lands on a PLAYING device — which is
		// the state the symptom was seen in.
		for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		for (int block = 0; block < 20; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		// **NOW THE CONTROL MOVES UNDER THE PLAYING DEVICE.**
		tweak(cfg);
		s.setTarget(cfg);
		std::vector<float> out;
		std::vector<float> l(960);
		std::vector<float> r(960);
		for (int block = 0; block < 40; ++block) {
			for (int i = 0; i < 960; ++i) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * (block * 960 + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			out.insert(out.end(), l.begin(), l.end());
		}
		return out;
	};

	const std::vector<float> base = renderWith([](Settings&) {});
	REQUIRE(!base.empty());

	struct Probe {
		const char* name;
		std::function<void(Settings&)> tweak;
	};
	const std::vector<Probe> probes {
		{ "Rate", [](Settings& c) { c.rate = 0.95f; } },
		{ "Zone A", [](Settings& c) { c.zoneA = 0.9f; } },
		{ "Zone B", [](Settings& c) { c.zoneB = 0.9f; } },
		// The `Macro Cfg` and `Macro` probes stood here; both controls are deleted (2026-09-19). What
		// asks the same question now is the CIRCLE — the waypoint's configuration the base puck faces,
		// and the puck's own distance, which is what the macro's amount became.
		{ "Cfg 2", [](Settings& c) { c.cornerCfg[kCfg2] = 0.9f; } },
		{ "Perf X", [](Settings& c) { c.perfX = 1.0f; } },
		{ "Density", [](Settings& c) { c.density = 0.1f; } },
		{ "Gamma", [](Settings& c) { c.gamma = 500.0f; } },
		{ "P Write", [](Settings& c) { c.pWrite = 0.9f; } },
		{ "Stereo Width", [](Settings& c) { c.stereoWidth = 1.0f; } },
	};

	size_t dead = 0;
	for (const Probe& p : probes) {
		const std::vector<float> moved = renderWith(p.tweak);
		REQUIRE(moved.size() == base.size());
		double worst = 0.0;
		for (size_t i = 0; i < base.size(); ++i) {
			worst = std::max(worst, std::abs(static_cast<double>(moved[i] - base[i])));
		}
		MESSAGE("GRAIN, " << std::string(p.name) << ": worst diff " << worst);
		// **PER CONTROL, NOT A TALLY — a known-bad is why.** Reverting Grain's decisions to a free LFSR
		// killed Macro Cfg and Macro and left a `dead < probes.size()` check GREEN, because the other
		// seven still moved. A count cannot see which one went out.
		//
		// **BUT TWO OF THEM ARE EXCUSED FROM THE AUDIO CHECK, AND THE EXEMPTION IS NOT A SHRUG.** Zone B
		// and Macro move a PROBABILITY; whether that changes a sample depends on a decision flipping,
		// and a thirty-block render holds only a few dozen grains. MEASURED at the plan level in "does
		// Zone B and Macro move the GRAIN plan's decisions at all": over 256 grains Zone B flips 78 and
		// Macro flips 10 — so both are wired, and asserting on the AUDIO here would be asserting
		// something this window cannot reliably show. The first draft did exactly that and went red on a
		// correct build.
		const std::string who(p.name);
		CAPTURE(who);
		// `"Macro"` was the second name here; the control is deleted (2026-09-19) and what asks the same
		// question is `"Perf X"` — the puck's distance east, which is what the macro's amount became.
		if (who != "Zone B" && who != "Perf X") {
			CHECK(worst > 0.0);
		}
		if (worst == 0.0) {
			++dead;
		}
	}
	MESSAGE("controls whose effect this short render did not happen to show: " << dead << " of "
	                                                                          << probes.size());
	// AT MOST THE TWO PROBABILISTIC ONES — if a third goes quiet, something is unwired.
	CHECK(dead <= 2u);
}

TEST_CASE("MEASURE: does Zone B and Macro move the GRAIN plan's decisions at all") {
	// **"NO AUDIBLE DIFFERENCE" AND "DEAD" LOOK THE SAME FROM A RENDER, and they are not the same
	// thing.** The audio measurement above reads zero for Zone B and Macro in Grain. That is either a
	// control that reaches nothing, or a probability that moved without flipping any decision in the
	// few dozen grains a short render contains. Only the VALUE separates them.
	namespace sc = feathers::fx::sklitter;
	const auto decisions = [](float zoneB, float macro) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Grain, 0.5f);
		cfg.zoneA = 0.4f;
		cfg.zoneB = zoneB;
		withMacro(cfg, macro, 0.4f); // the puck's distance east IS the macro since 2026-09-19
		cfg.engage = true;
		s.setTarget(cfg);
		std::vector<int> out;
		for (int32_t k = 0; k < 256; ++k) {
			const SlicePlan p = s.grainPlanForTest(k);
			out.push_back((p.shouldReverse ? 1 : 0) | (p.shouldPitchUp ? 2 : 0)
			              | (p.useDry ? 4 : 0) | (p.macroWantsDry ? 8 : 0));
		}
		return out;
	};

	const std::vector<int> base = decisions(0.4f, 0.5f);
	const auto countDiff = [&](const std::vector<int>& other) {
		size_t n = 0;
		for (size_t i = 0; i < base.size(); ++i) {
			if (base[i] != other[i]) {
				++n;
			}
		}
		return n;
	};
	const size_t byZoneB = countDiff(decisions(0.9f, 0.5f));
	const size_t byMacro = countDiff(decisions(0.4f, 1.0f));
	MESSAGE("over 256 grains — Zone B 0.4->0.9 flips " << byZoneB << " decisions; macro 0.5->1.0 flips "
	                                                   << byMacro);
	// BOTH REACH THE PLAN. If either is zero the control really is unwired in Grain, and the audio
	// measurement's zero was the truth rather than a short window.
	CHECK(byZoneB > 0u);
	CHECK(byMacro > 0u);
}

TEST_CASE("the ribbon sweeps in GRAIN, and the bar counter advances there too") {
	// **THE PLAYER RIBBON RESPONDS IN GRAIN MODE TOO.**
	//
	// `linearBarPos_` was stepped only in `playLooper`, so in Grain the bar's position never moved.
	// Three things hung off that, and only the first was visible: the playhead had nothing to sweep;
	// `barIndex_` is stepped at the bar wrap, so multi-bar evolution and `Param::SeqLength` were frozen
	// at bar zero in Grain; and pWrite's linear write head never advanced either.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Grain, 0.5f);
	cfg.zoneA = 0.4f;
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> published;
	s.setReadingsSink(feathers::device::ReadingsSink {
	    &published, [](void* ctx, const float* v, size_t n) {
		    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	    } });
	double beats = 0.0;
	for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	// SAMPLED ACROSS A BAR, so a head that moves once and stops is not mistaken for one that sweeps.
	std::vector<float> heads;
	const int32_t startBar = s.barIndexForTest();
	for (int block = 0; block < 140; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
		s.publishReadings();
		REQUIRE(published.size() == sc::kReadingCount);
		const auto step = published[static_cast<size_t>(sc::Reading::Step)];
		const auto phase = published[static_cast<size_t>(sc::Reading::StepPhase)];
		heads.push_back(step + phase);
	}

	// **IT MOVES, AND IT MOVES MORE THAN ONCE.** A single change would be the gesture settling; a
	// sweep is many distinct positions.
	std::vector<float> distinct = heads;
	std::sort(distinct.begin(), distinct.end());
	distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
	MESSAGE("GRAIN ribbon: " << distinct.size() << " distinct head positions over " << heads.size()
	                         << " blocks");
	CHECK(distinct.size() > 8u);

	// **AND THE BAR COUNTER ADVANCED**, which is the half nobody noticed: without it SeqLength and the
	// multi-bar evolution are dead in Grain, silently, because every bar is bar zero.
	CAPTURE(startBar);
	CAPTURE(s.barIndexForTest());
	CHECK(s.barIndexForTest() != startBar);
}

TEST_CASE("MIX and MIN ENV act in EVERY mode, not in the one that was being worked on") {
	// **THE DEVICE HAS A MINIMUM-ENVELOPE CONTROL AND A PLAIN MIX CONTROL.**
	//
	// **THE CASE SWEEPS EVERY MODE ON PURPOSE.** Three renderers own an output line —
	// `playBurst`, `playLooper`, `playGrain` — and this device has produced the "a fix
	// reached one road and stopped" failure FIVE times: density to the slice modes and not Grain, the
	// hard cut to the looper renderer and not the grain one, the repaint to the card and not the panel,
	// the mode re-arm to `advanceSlice` which Grain never calls, and the ribbon to `playLooper` alone.
	// A new per-sample stage is exactly the shape that produces the sixth, so the guard is a sweep
	// rather than a check on whichever mode was in front of me.
	namespace sc = feathers::fx::sklitter;
	const auto render = [](Mode m, float mix, float attackSeconds) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(m, 0.5f);
		cfg.zoneA = 0.4f;
		cfg.zoneB = 0.4f;
		cfg.mix = mix;
		cfg.minEnvSeconds = attackSeconds;
		cfg.engage = true;
		s.setTarget(cfg);
		double beats = 0.0;
		for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		std::vector<float> out;
		std::vector<float> l(960);
		std::vector<float> r(960);
		for (int block = 0; block < 30; ++block) {
			for (int i = 0; i < 960; ++i) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * (block * 960 + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			out.insert(out.end(), l.begin(), l.end());
		}
		return out;
	};

	for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
		const auto mode = static_cast<Mode>(m);
		CAPTURE(m);
		const std::vector<float> wet = render(mode, 1.0f, 0.0f);
		const std::vector<float> half = render(mode, 0.5f, 0.0f);
		const std::vector<float> dry = render(mode, 0.0f, 0.0f);
		REQUIRE(wet.size() == dry.size());
		REQUIRE(half.size() == dry.size());

		double wetVsDry = 0.0;
		double halfVsDry = 0.0;
		for (size_t i = 0; i < wet.size(); ++i) {
			wetVsDry = std::max(wetVsDry, std::abs(static_cast<double>(wet[i] - dry[i])));
			halfVsDry = std::max(halfVsDry, std::abs(static_cast<double>(half[i] - dry[i])));
		}
		MESSAGE("mode " << m << ": wet-vs-dry " << wetVsDry << ", half-vs-dry " << halfVsDry);

		// **MIX AT ZERO IS THE INPUT, IN EVERY MODE.** Not "close to" — `blend` returns `dry` exactly
		// when the mix is zero, so anything else means a renderer is writing past it.
		//
		// **AND THE SWEEP IS NOT VACUOUS**: the same mode at full wet must DIFFER from the input, or
		// "mix 0 equals the input" is satisfied by a device that is doing nothing at all.
		CHECK(wetVsDry > 0.0);
		// HALF SITS BETWEEN THEM, which is what makes it a mix rather than a switch.
		CHECK(halfVsDry > 0.0);
		CHECK(halfVsDry < wetVsDry);
	}

	SUBCASE("MIN ENV is declared off by default, and gated to the modes it acts in") {
		// **THE FLOOR IS "NO TIME AT ALL"** — `Unit::LogSeconds`' own convention — so the default is the
		// minimum and a device that never touches the control behaves as it did. The default is 0.
		const feathers::device::ParamDesc& p = sc::kDescriptor.params[static_cast<size_t>(sc::Param::MinEnv)];
		CHECK(p.def == doctest::Approx(p.min));
		CHECK(p.unit == feathers::device::Unit::LogSeconds);
		// NOT A MODULATION TARGET is not claimed here: it IS one, because a floor that can be swept is
		// a usable performance control. What matters is that it starts off.

		// **AND IT ACTS WHERE IT IS DECLARED, AND IS CORRECTLY GATED OUT OF GRAIN.** MEASURED at 0.198 in
		// Shuffle and EXACTLY ZERO in Grain — whose triangular window already starts every grain at
		// silence, so there is no edge for a floor to put under. The gate says so rather than the control
		// claiming a mode it does nothing in.
		CHECK((p.gateMask & sc::kModeBit(sc::Mode::Grain)) == 0u);
		CHECK((p.gateMask & sc::kModeBit(sc::Mode::Shuffle)) != 0u);
		// **TWO LOOPER MODES, because the control is gated to the looper modes.**
		for (const Mode m : { Mode::Shuffle, Mode::Pattern }) {
			const std::vector<float> none = render(m, 1.0f, 0.0f);
			const std::vector<float> ramped = render(m, 1.0f, 0.02f);
			REQUIRE(none.size() == ramped.size());
			double worst = 0.0;
			for (size_t i = 0; i < none.size(); ++i) {
				worst = std::max(worst, std::abs(static_cast<double>(none[i] - ramped[i])));
			}
			CAPTURE(static_cast<int>(m));
			MESSAGE("min attack, mode " << static_cast<int>(m) << ": worst diff " << worst);
			CHECK(worst > 0.0);
		}
	}
}

TEST_CASE("the four floors are DURATIONS, not the reference's 44.1 kHz sample counts") {
	// **BACKLOG EO, ITEM 8.** The anti-click windows were converted to time when they were ported —
	// `prepare` computes them as `0.015 * sampleRate_` and so on — and four lengths were not, so they
	// stayed sample COUNTS that silently mean 44.1 kHz. At 96 kHz every one was HALF its intended
	// duration: the floor doing half its job at exactly the rate where a short grain is most likely to
	// click.
	//
	// **ASSERTED AS VALUES BECAUSE A FLOOR ONLY BITES AT AN EXTREME.** Ordinary output never touches
	// `kMinGrain`, so no render distinguishes 1024 frames from 2048 — and a known-bad reverting the
	// conversion went red only INCIDENTALLY, through an unrelated case, which is a catch nobody should
	// rely on.
	namespace sc = feathers::fx::sklitter;
	const auto floorsAt = [](double rate) {
		Sklitter s;
		s.prepare(rate, 4096);
		return std::array<size_t, 4> { s.minBurstCycleForTest(), s.minBurstGrainForTest(),
			                           s.minSliceFramesForTest(), s.minGrainFramesForTest() };
	};

	// AT THE REFERENCE'S OWN RATE THEY ARE THE REFERENCE'S OWN NUMBERS, which is what says the
	// conversion preserved the intent rather than inventing new lengths.
	const std::array<size_t, 4> at441 = floorsAt(sc::kReferenceRate);
	CHECK(at441[0] == 64u);
	CHECK(at441[1] == 32u);
	CHECK(at441[2] == 256u);
	CHECK(at441[3] == 1024u);

	// AND AT TWICE THE RATE THEY ARE TWICE THE FRAMES — the same DURATION, which is the whole point.
	const std::array<size_t, 4> at882 = floorsAt(sc::kReferenceRate * 2.0);
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(at882[i] == at441[i] * 2u);
	}

	// AND AT 96 kHz — a common session rate, and the one the raw counts were wrong at.
	const std::array<size_t, 4> at96 = floorsAt(96000.0);
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		const double seconds = static_cast<double>(at441[i]) / sc::kReferenceRate;
		// **PLUS OR MINUS ONE FRAME, NOT A RELATIVE EPSILON.** The conversion rounds to whole frames, so
		// at 64 frames a 0.1%% tolerance is tighter than one frame and a correct build fails it — which is
		// what the first draft did, reading `139 == Approx(139.32)`. The claim is "the same duration", and
		// one frame is the finest a frame count can express it.
		CHECK(std::abs(static_cast<double>(at96[i]) - seconds * 96000.0) <= 1.0);
	}
	MESSAGE("min grain: " << at441[3] << " frames at 44.1k, " << at96[3] << " at 96k — "
	                      << (static_cast<double>(at441[3]) / sc::kReferenceRate * 1000.0) << " ms");
}

TEST_CASE("MIN ENV ramps BOTH ends, and each end is capped at half the grain") {
	// **THE FIRST VERSION WAS A MINIMUM ATTACK ONLY.** The control became Min Env, which shapes both the
	// attack and the release, and each of the two is capped at half the grain size.
	//
	// **ASSERTED AS GAINS, NOT AS AUDIO.** The shape is a claim about a multiplier at a position, and a
	// rendered grain carries the signal and the envelope multiplied together — separating them back out
	// is exactly what this repo's rule says a buffer cannot be asked to do.
	Sklitter s;
	s.prepare(48000.0, 4096);
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.minEnvSeconds = 0.01f; // 480 frames at 48k
	s.setTarget(cfg);
	const size_t ramp = s.envFramesForTest();
	REQUIRE(ramp == 480u);

	SUBCASE("a long grain: full at both ends' inner edge, zero at the outer") {
		// A grain far longer than two ramps — the cap does not bite and each end is the plain ramp.
		constexpr size_t kGrain = 4800;
		CHECK(s.envGainForTest(0, kGrain) == doctest::Approx(0.0f));
		CHECK(s.envGainForTest(ramp / 2, kGrain - ramp / 2) == doctest::Approx(0.5f));
		CHECK(s.envGainForTest(ramp, kGrain - ramp) == doctest::Approx(1.0f));
		// THE MIDDLE IS UNTOUCHED, which is what makes it a floor rather than an envelope.
		CHECK(s.envGainForTest(kGrain / 2, kGrain / 2) == doctest::Approx(1.0f));
		// **AND THE RELEASE END MIRRORS IT** — the half that did not exist before.
		CHECK(s.envGainForTest(kGrain - ramp, ramp) == doctest::Approx(1.0f));
		CHECK(s.envGainForTest(kGrain - ramp / 2, ramp / 2) == doctest::Approx(0.5f));
		CHECK(s.envGainForTest(kGrain, 0) == doctest::Approx(0.0f));
	}

	SUBCASE("a SHORT grain: the ramps shorten so it still reaches full level at its midpoint") {
		// **THIS IS THE CAP, AND WITHOUT IT THE GRAIN NEVER REACHES 1.** At 200 frames against a 480
		// frame ramp the two ends overlap everywhere, so the smaller always wins and the peak is
		// 100/480 — the control would be quietly changing the LEVEL of short grains rather than their
		// shape. Capped at half, the ramp is 100 and the grain is a full triangle.
		constexpr size_t kGrain = 200;
		CHECK(s.envGainForTest(0, kGrain) == doctest::Approx(0.0f));
		CHECK(s.envGainForTest(kGrain / 2, kGrain / 2) == doctest::Approx(1.0f));
		CHECK(s.envGainForTest(kGrain, 0) == doctest::Approx(0.0f));
		// A QUARTER IN IS HALF UP, which is the triangle being linear rather than merely peaking.
		CHECK(s.envGainForTest(kGrain / 4, kGrain - kGrain / 4) == doctest::Approx(0.5f));
	}

	SUBCASE("OFF means the firmware's envelope UNRAISED — which is not the same as unity") {
		// **THIS SUBCASE ASSERTED UNITY AT THE EDGES AND WAS RIGHT TO, until the firmware's grain fade
		// was ported.** While `Min Env` WAS the fade, off meant no envelope at all and both edges read
		// 1. It is the FLOOR UNDER the firmware's envelope now, so off leaves a real ramp in place —
		// and the old assertion was asking for the bug back, which is why it went red on a correct
		// build rather than on a broken one.
		//
		// **WHAT "OFF" STILL GUARANTEES is that the CONTROL adds nothing.** `envFramesForTest` is zero,
		// and the lengths are exactly the reference's — asserted by number in "the grain FADE is the
		// firmware's".
		Sklitter q;
		q.prepare(48000.0, 4096);
		Settings none = looper(Mode::Shuffle, 0.5f);
		none.minEnvSeconds = 0.0f;
		q.setTarget(none);
		CHECK(q.envFramesForTest() == 0u);
		// THE EDGES ARE THE FIRMWARE'S RAMP — neither silence nor unity.
		CHECK(q.envGainForTest(0, 4800) == doctest::Approx(0.0f));
		// AND THE MIDDLE IS UNTOUCHED, which is what makes it a fade rather than an envelope.
		CHECK(q.envGainForTest(2400, 2400) == doctest::Approx(1.0f));
	}
}

TEST_CASE("the grain FADE is the firmware's, and Min Env is a floor on top of it") {
	// **THE BIGGEST PARITY GAP IN THE PORT, AND IT IS THE CLICK THAT KEPT BEING HEARD.** The firmware
	// ramps every grain linearly at BOTH ends, always — `prepareGrainEnvelopeQ31` computes the lengths,
	// `stutterer.cpp` applies them:
	//
	//     if (pos < loopAttackLen && !scatterConsecutive) { ... pos * loopInvAttackLen ... }
	//     else if (pos > loopGatedLen - loopDecayLen && !loopNextConsecutive) { ... }
	//
	// **WE HAD THE GUARDS AND NOT THE RAMP.** `consecutive_` and `nextConsecutive_` came across — they
	// are what SUPPRESSES the fade between adjacent grains — but `attackFadeLen`, `decayFadeLen`,
	// `fadeLen` and `kAntiClickSamples` appeared NOWHERE. Our attack was the zero-crossing gate alone:
	// silent until the first crossing, then full level immediately.
	//
	// **THE FIRMWARE HAS BOTH MECHANISMS AND SO DO WE NOW** — checked rather than assumed. The gate
	// picks a good place to start; the ramp covers the case where the good place is late, or is not
	// actually at zero.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(sc::kReferenceRate, 4096); // the reference's own rate, so its counts are the expected numbers
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.minEnvSeconds = 0.0f; // Min Env OFF — the firmware's envelope, unraised
	s.setTarget(cfg);

	SUBCASE("a long grain gets the reference's 440-sample base, split by envShape") {
		constexpr size_t kLong = 44100; // far longer than two fades

		// **SYMMETRIC AT 0.5**, which is `min(0.5*2,1) == 1` on both ends: the whole base each.
		s.setPlanEnvShapeForTest(0.5f);
		Sklitter::GrainFade mid = s.grainFadeForTest(kLong);
		CHECK(mid.attack == 440u);
		CHECK(mid.decay == 440u);

		// **PERCUSSIVE AT 0**: instant attack, full decay — except the 64-sample FLOOR keeps a fade on
		// the end envShape scaled to nothing, which is what "absolute minimum for click-free audio"
		// means in the reference's own comment.
		s.setPlanEnvShapeForTest(0.0f);
		Sklitter::GrainFade perc = s.grainFadeForTest(kLong);
		CHECK(perc.attack == 64u);
		CHECK(perc.decay == 440u);

		// AND ITS MIRROR AT 1.
		s.setPlanEnvShapeForTest(1.0f);
		Sklitter::GrainFade rev = s.grainFadeForTest(kLong);
		CHECK(rev.attack == 440u);
		CHECK(rev.decay == 64u);

		// **THE ASYMMETRY IS REAL, not two equal numbers dressed up** — the whole point of envShape.
		CHECK(perc.attack != perc.decay);
		CHECK(rev.attack != rev.decay);
	}

	SUBCASE("a short grain gets half its own length, which is the same cap Min Env has") {
		// The reference: `p.fadeLen = p.gatedLength / 2` when `gatedLength <= kAntiClickSamples * 2`.
		s.setPlanEnvShapeForTest(0.5f);
		constexpr size_t kShort = 400; // under 880
		Sklitter::GrainFade f = s.grainFadeForTest(kShort);
		CHECK(f.attack == kShort / 2);
		CHECK(f.decay == kShort / 2);
		// AND THE FLOOR DOES NOT OVERRIDE THE CAP: at a base below 64 the reference declines to raise
		// it, because a floor longer than the grain is not a fade.
		Sklitter::GrainFade tiny = s.grainFadeForTest(100);
		CHECK(tiny.attack <= 50u);
		CHECK(tiny.decay <= 50u);
	}

	SUBCASE("MIN ENV RAISES the fade rather than being it — the floor, not the fade") {
		// **THE CONTROL USED TO BE THE FADE; NOW IT IS THE FLOOR UNDER THE FIRMWARE'S**, by design.
		s.setPlanEnvShapeForTest(0.5f);
		constexpr size_t kLong = 44100;
		const size_t base = s.grainFadeForTest(kLong).attack;
		REQUIRE(base == 440u);

		// OFF leaves the firmware's envelope exactly as it is — the default changes nothing.
		Settings raised = cfg;
		raised.minEnvSeconds = 0.05f; // 2205 frames at 44.1k, well above the base
		s.setTarget(raised);
		s.setPlanEnvShapeForTest(0.5f);
		const Sklitter::GrainFade up = s.grainFadeForTest(kLong);
		CHECK(up.attack > base);
		CHECK(up.decay > base);
		// STILL CAPPED AT HALF THE GRAIN, so raising the floor cannot flatten a grain.
		CHECK(up.attack <= kLong / 2);

		// AND A FLOOR BELOW THE FIRMWARE'S FADE CHANGES NOTHING — it is a max, not an override.
		Settings low = cfg;
		low.minEnvSeconds = 0.001f; // 44 frames, under the 440 base
		s.setTarget(low);
		s.setPlanEnvShapeForTest(0.5f);
		CHECK(s.grainFadeForTest(kLong).attack == base);
	}
}

TEST_CASE("SWEEP: the worst discontinuity each mode writes, across rates and the edge settings") {
	// **A SWEEP FOR CLICKS IN PLAYBACK**, both for parity with the firmware and more broadly. Modes:
	// everything but Burst.
	//
	// **A CLICK IS A DISCONTINUITY, AND THAT IS MEASURABLE — which is the whole reason this is a case
	// and not a listening session.** Drive a SMOOTH input (a sine at a known frequency) and take the
	// per-sample first difference of the output. A sine at frequency f can change by at most
	// `2*pi*f/rate` between samples; anything larger came from the device cutting the signal, not from
	// the signal itself. So the measurement has a physical bound to compare against rather than a
	// threshold somebody guessed.
	//
	// **IT MEASURES, IT DOES NOT JUDGE.** A stutter IS made of edges: a slice that jumps to a different
	// part of the buffer is SUPPOSED to be discontinuous at its seam, and the fades and the crossing
	// gate exist to soften that rather than remove it. So this reports the worst jump per mode and
	// where it happened, and a listener decides which are artefacts. Nothing here fails on a number.
	//
	// **OPTIONAL**: gated on `FEATHERS_CLICK_SWEEP` so the ordinary run does not pay
	// for it. `FEATHERS_CLICK_SWEEP=1 build/tests/test_fx_sklitter` runs it; `ctest -L clicks` is the
	// front door.
	if (std::getenv("FEATHERS_CLICK_SWEEP") == nullptr) {
		return;
	}
	namespace sc = feathers::fx::sklitter;

	constexpr double kFreq = 220.0;
	// THE MOST A 220 Hz SINE CAN MOVE IN ONE SAMPLE AT 48k — the physical bound the input itself obeys.
	const double kInputStep = 2.0 * 3.14159265358979 * kFreq / kRate;

	struct Worst {
		double jump = 0.0;
		size_t frame = 0;
		float rate = 0.0f;
		const char* setting = "";
	};

	const auto runOne = [&](Mode m, float rate, const char* label,
	                        const std::function<void(Settings&)>& tweak) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(m, rate);
		cfg.zoneA = 0.35f;
		cfg.cornerCfg[kCfg2] = 0.4f; // the E waypoint is what the macro tweaks below reach
		cfg.engage = true;
		tweak(cfg);
		s.setTarget(cfg);
		double beats = 0.0;
		for (int block = 0; block < 400 && s.statusForTest() != Status::Playing; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		if (s.statusForTest() != Status::Playing) {
			return Worst {}; // never engaged; reported as zero rather than as a finding
		}
		Worst w;
		w.rate = rate;
		w.setting = label;
		std::vector<float> l(960);
		std::vector<float> r(960);
		float prev = 0.0f;
		bool havePrev = false;
		size_t frame = 0;
		// TWO BARS, so the bar seam and a re-cut both fall inside the window.
		for (int block = 0; block < 200; ++block) {
			for (int i = 0; i < 960; ++i) {
				const double ph = 2.0 * 3.14159265358979 * kFreq * (block * 960 + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			for (int i = 0; i < 960; ++i, ++frame) {
				const float cur = l[static_cast<size_t>(i)];
				if (havePrev) {
					const double d = std::abs(static_cast<double>(cur - prev));
					if (d > w.jump) {
						w.jump = d;
						w.frame = frame;
					}
				}
				prev = cur;
				havePrev = true;
			}
		}
		return w;
	};

	const std::vector<std::pair<const char*, std::function<void(Settings&)>>> settings {
		{ "plain", [](Settings&) {} },
		// **THE EDGES THIS DEVICE MAKES ON PURPOSE**, which is where a click hides rather than in the
		// default arrangement nobody has trouble with.
		{ "lengthMult cuts", [](Settings& c) { c.zoneB = 0.9f; } },   // short grains, hard-zeroed tails
		{ "pWrite under read", [](Settings& c) { c.pWrite = 0.8f; } }, // writing where it reads
		{ "density thins", [](Settings& c) { c.density = 0.4f; } },    // dry switching mid-stream
		{ "macro hard", [](Settings& c) { withMacro(c, 1.0f, 0.4f); } },  // probabilities pushed up
		{ "gates and ratchets", [](Settings& c) { withMacro(c, 0.6f, 0.1f); } },
		{ "gates, other cfg", [](Settings& c) { withMacro(c, 0.6f, 0.7f); } },
	};
	const std::vector<float> rates { 0.2f, 0.5f, 0.8f };

	MESSAGE("a 220 Hz sine moves at most " << kInputStep << " per sample at " << kRate
	                                       << " — anything above that is the DEVICE, not the signal");
	for (int mi = 0; mi < static_cast<int>(Mode::kCount); ++mi) {
		const auto m = static_cast<Mode>(mi);
		if (m == Mode::Burst) {
			continue; // Burst is outside this sweep's scope
		}
		Worst worst;
		for (const auto& [label, tweak] : settings) {
			for (const float rate : rates) {
				const Worst w = runOne(m, rate, label, tweak);
				if (w.jump > worst.jump) {
					worst = w;
				}
			}
		}
		MESSAGE("mode " << mi << ": worst jump " << worst.jump << " (" << (worst.jump / kInputStep)
		                << "x the input's own step) at frame " << worst.frame << ", rate "
		                << worst.rate << ", " << std::string(worst.setting));
	}
	// THE FIXTURE CAN SEE SOMETHING AT ALL — a sweep that measured silence would report zeroes and read
	// as a clean bill of health.
	CHECK(kInputStep > 0.0);
}

namespace {

/// **A CONSTANT INPUT, so any step in the ring is a SEAM the write made.** `drive` feeds a ramp, whose
/// own slope would hide a small edge and whose magnitude would dwarf a large one.
void driveConst(Sklitter& s, int frames, float value, double* beats) {
	constexpr int kChunk = 960;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	for (int done = 0; done < frames; done += kChunk) {
		const int n = std::min(kChunk, frames - done);
		std::fill(l.begin(), l.begin() + n, value);
		std::fill(r.begin(), r.begin() + n, -value);
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
}

/// The largest sample-to-sample step anywhere in the captured loop, wrapping at its end — **EXCEPT AT
/// THE WRITE HEAD ITSELF.** Mid-grain, the head is where freshly written material meets what it has
/// not reached yet; that front moves with the head and is not an edge the write LEFT. Measured before
/// this exclusion: the only step over 0.2 in the whole ring sat exactly on the head.
float worstRingStep(const Sklitter& s, float* peak) {
	const size_t n = s.bufferFramesForTest();
	const size_t head = s.linearBarPosForTest() % n;
	float worst = 0.0f;
	*peak = 0.0f;
	float prevL = 0.0f;
	float prevR = 0.0f;
	s.ringForTest().readAt(n - 1, &prevL, &prevR);
	for (size_t k = 0; k < n; ++k) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(k, &l, &r);
		if (((k + n - head) % n) > 2 && ((head + n - k) % n) > 2) {
			worst = std::max({ worst, std::abs(l - prevL), std::abs(r - prevR) });
		}
		*peak = std::max(*peak, std::abs(l));
		prevL = l;
		prevR = r;
	}
	return worst;
}

} // namespace

TEST_CASE("pWrite leaves NO SEAM in the buffer — the write is faded in every mode that writes") {
	// **PWRITE CLICKED, AND GRAIN IS MEANT TO BE CLICK-FREE.** The firmware fades the write: its `pWriteEnvQ31`
	// takes the output's attack/decay ramp and a `min` against `kPWriteFade`; ours wrote with the gate
	// envelope alone in the looper and with NO envelope in Grain.
	//
	// **ASSERTED ON THE BUFFER, NOT ON THE OUTPUT**, because that is where the damage lives: a seam
	// written into the material is replayed on every pass, and no output window over the top mends it.
	// The fixture captures a constant 1.0, then feeds a constant 0.5 with pWrite fully on — so every source
	// the write can take is a nonzero constant, and any step in the ring is an edge the write made. (A
	// first version captured SILENCE, and a wet slice writing a notch of zeros over zeros could not show.) A faded edge climbs over at least `kPWriteFade` samples: steps of ~1/64.
	// **AND WITH GRAINS CUT SHORT** — the click sweep's own arrangement, Zone B at 0.9 and a faster Rate, which is where `lengthMult` hard-zeroes
	// the tail of a slot. A first version held Zone B at 0.8 and missed a notch the write stamped at
	// every cut: the read stopped at the audible end, so the write recorded silence as it released.
	struct Arrangement {
		float zoneA;
		float zoneB;
		float macroConfig;
		float rate;
		float density; // below one so some slices go DRY and write the input: a wet slice copies captured silence
	};
	for (const Arrangement arr : { Arrangement { 0.6f, 0.8f, 0.0f, 0.5f, 1.0f }, Arrangement { 0.35f, 0.9f, 0.4f, 0.8f, 0.5f } }) {
	for (const Mode mode : { Mode::Shuffle, Mode::Time, Mode::Pattern, Mode::Pitch, Mode::Grain }) {
		CAPTURE(arr.zoneA);
		CAPTURE(arr.zoneB);
		CAPTURE(arr.rate);
		CAPTURE(static_cast<int>(mode));
		Sklitter s;
		s.prepare(kRate, 4096);
		Settings cfg = looper(Mode::Shuffle, arr.rate);
		cfg.zoneA = arr.zoneA;
		cfg.cornerCfg[kCfg2] = arr.macroConfig; // the knob is deleted; the E waypoint holds the same quantity
		cfg.density = arr.density;
		cfg.zoneB = arr.zoneB;
		s.setTarget(cfg);
		double beats = 0.0;
		for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
			driveConst(s, 960, 1.0f, &beats);
		}
		REQUIRE(s.statusForTest() == Status::Playing);

		cfg.mode = mode;
		cfg.pWrite = 1.0f;
		s.setTarget(cfg);
		driveConst(s, 96000 * 3, 0.5f, &beats);

		float peak = 0.0f;
		const float step = worstRingStep(s, &peak);
		size_t written = 0;
		for (size_t k = 0; k < s.bufferFramesForTest(); ++k) {
			float wl = 0.0f;
			float wr = 0.0f;
			s.ringForTest().readAt(k, &wl, &wr);
			// **MOVED A TENTH OF THE WAY TO THE INPUT, NOT LANDED ON IT (2026-09-24, backlog JC).** In Grain
			// a write is a blend at the voice's window, and a frame reaches exactly 0.5 only where a dry
			// grain's window peaked on it — a coincidence of grain timing, which halving the pitched grains
			// moved: 0 frames within 1e-3 in the first arrangement, 123 within 0.1. The claim this guards is
			// that the write HAPPENED; a wet grain writes back the 1.0 it read, so only the input can move a
			// frame off 1.0.
			if (std::abs(wl - 1.0f) > 0.05f) {
				++written;
			}
		}
		CAPTURE(written);
		CAPTURE(step);
		// **SOMETHING WAS WRITTEN** — some of the ring holds the input's 0.5 — or a frozen capture passes
		// the seam check vacuously.
		CHECK(written > 0u);
		// **0.1, NOT 1/64.** The ramp and the slewed write decision multiply, and each moves by up to 1/64 a
		// sample, so a faded edge on a short grain measures a few sixty-fourths — 0.055 at the cut
		// arrangement. A seam is a different order: the notch this caught measured 0.98.
		CHECK(step < 0.1f);
	}
	}
}

TEST_CASE("Grain writes at the LINEAR bar position, not back where its voice reads") {
	// **THE SIXTH "FIX REACHED ONE ROAD" IN THIS DEVICE.** The looper's pWrite was moved to the linear
	// head long ago — *"read from shuffled position, write to linear position"* — and Grain kept
	// writing to `grainPosA_ + offsetA`, the sample it had just read: a feedback path, and a write that
	// lands wherever the cloud happens to be rather than where the bar is.
	//
	// **ASSERTED AS WHERE THE WRITE LANDED.** Silence is captured; then a constant is fed for a stretch
	// short enough that the linear head covers a known window of the loop. Every nonzero sample must be
	// inside that window. A write at the voice's read position lands wherever the grain is reading.
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.6f;
	s.setTarget(cfg);
	double beats = 0.0;
	for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
		driveConst(s, 960, 0.0f, &beats);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	cfg.mode = Mode::Grain;
	// **DENSITY ZERO, SO VOICE A IS DRY AND WRITES THE INPUT.** A wet voice writes its own grain —
	// `srcAL = grainAIsDry ? dryL : bufAL` — which here is captured silence, and a write of silence
	// cannot be told from no write at all.
	cfg.density = 0.0f;
	cfg.pWrite = 1.0f;
	s.setTarget(cfg);
	// **THE WINDOW IS PUT WHERE NO GRAIN READS.** A small Zone A keeps the voices near the top of the
	// buffer, and the head is walked on — writing silence — to the far half before the constant starts.
	// A first draft used half the loop from wherever the head happened to be, and the read-position
	// known-bad passed it: every write the bug made also landed inside that window.
	cfg.zoneA = 0.05f;
	s.setTarget(cfg);
	driveConst(s, 960, 0.0f, &beats);
	const size_t n = s.bufferFramesForTest();
	const size_t target = n * 3 / 4;
	driveConst(s, static_cast<int>((target + n - s.linearBarPosForTest()) % n), 0.0f, &beats);
	const size_t start = s.linearBarPosForTest();
	constexpr int kWindow = 12000;
	driveConst(s, kWindow, 1.0f, &beats);
	// THE HEAD WALKED THE WINDOW, or "inside the window" is a claim about a head that stood still.
	REQUIRE(((s.linearBarPosForTest() + n - start) % n) == static_cast<size_t>(kWindow));

	size_t inside = 0;
	size_t outside = 0;
	for (size_t k = 0; k < n; ++k) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(k, &l, &r);
		if (std::abs(l) < 1e-6f) {
			continue;
		}
		if (((k + n - start) % n) < static_cast<size_t>(kWindow)) {
			++inside;
		} else {
			++outside;
		}
	}
	CAPTURE(inside);
	CAPTURE(outside);
	CHECK(inside > 0u);
	CHECK(outside == 0u);
}

TEST_CASE("Grain's pWrite is a per-grain DRAW, and a wet voice writes the grain it is playing") {
	// **THE FIRMWARE'S GRAIN WRITE, in the two respects the port had as a different control.**
	//
	//     grainAWritesWet = (... fastRandom() ...) < pWriteProbGrain;      // at voice A's wrap
	//     q31_t srcAL = grainAIsDry ? dryL : bufAL;                          // what the voice plays
	//     ... existL * (1 - envA) + srcAL * envA ...                         // what gets written
	//
	// Ours scaled the write by pWrite on every sample and always wrote the INPUT.
	const auto grainAfterSilence = [](Sklitter& s, Settings& cfg, double* beats) {
		s.prepare(kRate, 4096);
		cfg = looper(Mode::Shuffle, 0.5f);
		cfg.zoneA = 0.6f;
		s.setTarget(cfg);
		for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
			driveConst(s, 960, 0.0f, beats);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.mode = Mode::Grain;
	};

	SUBCASE("at half pWrite, some grains write and some do not — decided per grain, not per sample") {
		Sklitter s;
		Settings cfg;
		double beats = 0.0;
		grainAfterSilence(s, cfg, &beats);
		cfg.pWrite = 0.5f;
		s.setTarget(cfg);
		size_t grains = 0;
		size_t writing = 0;
		double lastPhase = s.grainPhaseAForTest();
		for (int n = 0; n < 96000 * 40 && grains < 200; n += 96) {
			driveConst(s, 96, 0.0f, &beats);
			if (s.grainPhaseAForTest() < lastPhase) {
				++grains;
				if (s.grainWritesAForTest()) {
					++writing;
				}
			}
			lastPhase = s.grainPhaseAForTest();
		}
		CAPTURE(grains);
		CAPTURE(writing);
		REQUIRE(grains >= 40u);
		CHECK(writing > grains / 5);
		CHECK(writing < grains - grains / 5);
	}

	SUBCASE("a WET cloud over captured silence writes silence, however loud the input is") {
		// Full density and no Zone B, so no voice goes dry: every grain plays the captured buffer, and
		// that is what the write records. A write of the INPUT would put the constant straight in.
		Sklitter s;
		Settings cfg;
		double beats = 0.0;
		grainAfterSilence(s, cfg, &beats);
		cfg.density = 1.0f;
		cfg.zoneB = 0.0f;
		withMacro(cfg, 0.0f); // the puck at the centre: no macro at all, which is what `macro = 0` meant
		cfg.pWrite = 1.0f;
		s.setTarget(cfg);
		driveConst(s, 96000 * 2, 1.0f, &beats);
		float peak = 0.0f;
		(void)worstRingStep(s, &peak);
		CAPTURE(peak);
		CHECK(peak < 1e-6f);

		// AND THE SAME ARRANGEMENT WITH EVERY VOICE DRY DOES RECORD THE INPUT, which is what makes the
		// silence above a claim about the source rather than about a write that never happened.
		Sklitter d;
		Settings dcfg;
		double dbeats = 0.0;
		grainAfterSilence(d, dcfg, &dbeats);
		dcfg.density = 0.0f;
		dcfg.pWrite = 1.0f;
		d.setTarget(dcfg);
		driveConst(d, 96000 * 2, 1.0f, &dbeats);
		float dpeak = 0.0f;
		(void)worstRingStep(d, &dpeak);
		CAPTURE(dpeak);
		CHECK(dpeak > 0.5f);
	}
}

TEST_CASE("CLICKS: no looper mode steps more than six times the input's own step, pWrite aside") {
	// **THE CLICK SWEEP, TURNED INTO A GUARD.** The sweep (`FEATHERS_CLICK_SWEEP`) measures and reports;
	// this asserts, on the arrangements the sweep found edges in. Every class of click it turned up
	// measured ten times the input's per-sample step or more — a consecutive join after a cut slice, a
	// muted bar-end tail, a pitch-up read holding one sample, the engage frame, Grain's hard swap to
	// the bare input — and the clean device measures at most 3.3x. Six sits between them.
	//
	// **pWRITE IS LEFT OUT, and not because it clicks.** With pWrite on, a pitched-up read is written
	// back and read pitched-up again, so the loop climbs octaves and a smooth kilohertz sine read at
	// double speed moves 0.3 a sample. That is audio, and this measure cannot tell it from an edge.
	// Its seams are guarded on the BUFFER instead — see the pWrite seam case.
	constexpr double kFreq = 220.0;
	const double kInputStep = 2.0 * 3.14159265358979 * kFreq / kRate;
	const auto worstJump = [&](Mode m, float rate, const std::function<void(Settings&)>& tweak, size_t phase) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(m, rate);
		cfg.zoneA = 0.35f;
		cfg.zoneB = 0.6f;
		cfg.cornerCfg[kCfg2] = 0.4f;
		cfg.engage = true;
		tweak(cfg);
		s.setTarget(cfg);
		double beats = 0.0;
		// **ONE CONTINUOUS SINE FROM BEFORE THE TRIGGER**, so the engage frame — where the output leaves the
		// dry input — is measured too. A first version warmed up through `driveSine`, whose output it
		// never saw, and passed with the engage crossfade deleted.
		std::vector<float> l(960);
		std::vector<float> r(960);
		struct Result {
			double jump = 0.0;
			size_t held = 0;
		} out;
		float prev = 0.0f;
		bool havePrev = false;
		size_t run = 0;
		size_t n = 0;
		int playedBlocks = 0;
		for (int block = 0; block < 600 && playedBlocks < 200; ++block) {
			for (int i = 0; i < 960; ++i, ++n) {
				const double ph = 2.0 * 3.14159265358979 * kFreq * static_cast<double>(n + phase) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			if (s.statusForTest() == Status::Playing) {
				++playedBlocks;
			}
			for (int i = 0; i < 960; ++i) {
				const float cur = l[static_cast<size_t>(i)];
				if (havePrev) {
					out.jump = std::max(out.jump, std::abs(static_cast<double>(cur - prev)));
					// **A HELD SAMPLE IS A DROPOUT THAT DOES NOT CLICK** once the fades cover its end — a
					// read clamped to one frame — so it is counted separately: the longest run of one
					// nonzero value repeated.
					run = (cur == prev && cur != 0.0f) ? run + 1 : 0;
					out.held = std::max(out.held, run);
				}
				prev = cur;
				havePrev = true;
			}
		}
		REQUIRE(playedBlocks == 200);
		return out;
	};
	const std::vector<std::pair<const char*, std::function<void(Settings&)>>> settings {
		{ "plain", [](Settings&) {} },
		{ "lengthMult cuts", [](Settings& c) { c.zoneB = 0.9f; } },
		{ "density thins", [](Settings& c) { c.density = 0.4f; } },
		{ "macro hard", [](Settings& c) { withMacro(c, 1.0f, 0.4f); } },
		// **GATES AND RATCHETS**, which the arrangements above never produce: MEASURED over 4,800 slices at
		// Macro Cfg 0.4, not one gate below 1 and not one ratchet. They need the macro, at a Macro Cfg
		// that lets it through — 0.1 and 0.7 gate most slices and ratchet some.
		{ "gates and ratchets", [](Settings& c) { withMacro(c, 0.6f, 0.1f); } },
		{ "gates, other cfg", [](Settings& c) { withMacro(c, 0.6f, 0.7f); } },
	};
	for (const Mode m : { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Grain, Mode::Pattern, Mode::Pitch }) {
		for (const auto& [label, tweak] : settings) {
			// **AT SEVERAL INPUT PHASES**, because whether a gate lands on a live sample depends on where
			// the sine is when it fires: the muted bar-end tail read 20x in the sweep and nothing here
			// until the phase moved.
			for (const float rate : { 0.2f, 0.5f, 0.8f }) {
				for (const size_t phase : { size_t { 0 }, size_t { 37 }, size_t { 73 }, size_t { 151 } }) {
					const auto res = worstJump(m, rate, tweak, phase);
					const double ratio = res.jump / kInputStep;
					CAPTURE(static_cast<int>(m));
					CAPTURE(std::string(label));
					CAPTURE(rate);
					CAPTURE(phase);
					CAPTURE(ratio);
					CAPTURE(res.held);
					CHECK(ratio < 6.0);
					// A 220 Hz sine never repeats a value for more than a sample or two.
					CHECK(res.held < 16u);
				}
			}
		}
	}
}

TEST_CASE("MATRIX: every control in every mode — does it reach the sound, and does the UI say so") {
	// **PARITY GAPS KEPT BEING FOUND BY HAND AFTER SEVERAL PARITY SWEEPS.** The sweeps that missed them READ
	// the code.
	// This one MEASURES: for each of the eight modes and each control, render the device, move that
	// one control, render again, and compare the answer with the descriptor's own gate — the thing the
	// UI hides and shows by.
	//
	//   DEAD  the gate says the control is relevant in this mode, and moving it changes nothing
	//   LIAR  the gate hides the control in this mode, and moving it changes the sound
	//
	// **THROUGH THE HOST'S ROAD, `Row` -> `stateOf`**, not by writing `Settings` — a control that never
	// reaches `Settings` is exactly the gap a Settings-level test cannot see.
	//
	// **TWICE PER CELL**: the control set before the gesture, and the control moved under a playing
	// device. Some settings act only at the press, some only live; either counts as reaching the sound.
	namespace sc = feathers::fx::sklitter;
	constexpr int kWarm = 20;
	constexpr int kBlocks = 200; // two bars at 120 BPM, so probabilistic controls get a fair window

	const auto baseRow = [](Mode m) {
		Settings cfg = looper(m, 0.5f);
		cfg.zoneA = 0.4f;
		cfg.zoneB = 0.4f;
		withMacro(cfg, 0.5f, 0.4f);
		cfg.engage = true;
		// **FREE PHASE OFF ZERO, SO THE `Groove Mode` PROBE BELOW CAN SEE FREE 8 (backlog IO addendum).** Phase 0
		// is straight now, so Free at the default phase would read DEAD, correctly and uselessly. Inert in the
		// base row itself — it is read only in the Free modes and the base row is Straight — so it moves
		// nothing any other probe measures.
		cfg.freePhase = 0.4f;
		return sc::rowOf(cfg);
	};
	const auto render = [&](const sc::Row& before, const sc::Row& after) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(sc::stateOf(before));
		double beats = 0.0;
		std::vector<float> l(960);
		std::vector<float> r(960);
		std::vector<float> out;
		size_t n = 0;
		int played = 0;
		for (int block = 0; block < 700 && played < kWarm + kBlocks; ++block) {
			if (played == kWarm) {
				s.setTarget(sc::stateOf(after));
			}
			for (int i = 0; i < 960; ++i, ++n) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
				// A second, unrelated partial, so a control that changes WHICH material plays is not
				// hidden by a pure sine looking the same from many offsets.
				const double ph2 = 2.0 * 3.14159265358979 * 331.0 * static_cast<double>(n) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph) + 0.3 * std::sin(ph2));
				r[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph2) + 0.3 * std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			if (s.statusForTest() == Status::Playing) {
				++played;
				out.insert(out.end(), l.begin(), l.end());
				out.insert(out.end(), r.begin(), r.end());
			}
		}
		return out;
	};
	const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
		if (a.size() != b.size()) {
			return true;
		}
		for (size_t i = 0; i < a.size(); ++i) {
			if (std::abs(a[i] - b[i]) > 1e-6f) {
				return true;
			}
		}
		return false;
	};

	struct Move {
		sc::Param p;
		float value;
	};
	const std::vector<Move> moves {
		{ sc::Param::ZoneA, 0.9f },       { sc::Param::ZoneB, 0.9f },
		{ sc::Param::PWrite, 0.9f },      { sc::Param::Density, 0.2f },
		{ sc::Param::Rate, 0.9f },        { sc::Param::Quantized, 0.0f },
		{ sc::Param::Gamma, 2.7f },
		{ sc::Param::PitchScale, 5.0f },  { sc::Param::StereoWidth, 1.0f },
		{ sc::Param::SeqLength, 0.0f },   { sc::Param::BufferLength, 0.0f },
		{ sc::Param::MinEnv, static_cast<float>(sc::kMinEnvCeiling) },
		{ sc::Param::Mix, 0.5f },
		// **FULL FORCE-ON RATHER THAN FORCE-OFF, AND THE ASYMMETRY IS REAL.** At -1 the knob can only
		// remove sends phi was already making, so in an arrangement where phi makes none it is
		// correctly inert; at +1 it makes them wherever `delaySendBits` allows, which is the half that
		// is live in more arrangements. The matrix moves one control to one value.
		{ sc::Param::SendOverride, 1.0f },
		// **ON, BECAUSE OFF IS THE DEFAULT AND THE MATRIX MOVES A CONTROL AWAY FROM ITS BASE ROW.**
		{ sc::Param::TransportLocked, 1.0f },
		// **THE TWO CONTINUOUS WRITERS — backlog GU/GV.** Each writes into the ring at the linear
		// write head every sample, so what the read head finds later in the bar is different material.
		// Gated to Shuffle, Time, Pattern and Pitch (`kWriterModes`), and this matrix is what holds
		// that declaration to the engine.
		{ sc::Param::DryWrite, 0.9f },
		{ sc::Param::Feedback, 0.9f },
		// **GROOVE MODE TO FREE 8 (backlog IG; a mode of its own since IO).** Declared live in the five slice
		// modes (`kGrooveModes`) and this matrix is what holds that to the engine in all eight. FREE, because
		// the base row's Groove Amount is 0 — Swing or Groove at no amount would read DEAD here, correctly and
		// uselessly — and its Free Phase is 0.4 (`baseRow`), since Free's phase 0 is straight too.
		{ sc::Param::GrooveMode, static_cast<float>(sc::GrooveMode::Free8) },
		// **INPUT GAIN (backlog KZ), APPENDED PAST THE CIRCLE** — it scales what every mode records, so it reaches the
		// wet in all eight; the guard below counts it as the one control past `PerfDepth`.
		{ sc::Param::InputGain, 12.0f },
		// **FREEZE (backlog KX), APPENDED AFTER IT** — on from the start, the capture records nothing, so every mode plays
		// the prepared ring instead of the input it heard.
		{ sc::Param::Freeze, 1.0f },
		// **RECORD GATE (backlog MJ), AFTER FREEZE** — at 1 the threshold is 0 dBFS, which this fixture's 0.9-peak input never
		// reaches, so the capture records nothing and every mode plays the prepared ring, as Freeze does.
		{ sc::Param::RecordGate, 1.0f },
	};
	// **`FB Tap` IS DELIBERATELY NOT IN THAT LIST, FOR THE REASON THE PERFORM CIRCLE IS NOT EITHER, AND
	// IT IS A PROPERTY OF THIS FIXTURE RATHER THAN OF THE CODE.** The matrix moves ONE control away
	// from a base row, and in that row `Feedback` is at its default of 0 — so there is no feedback for
	// a tap to choose between, and the switch would read DEAD in every mode: correctly and uselessly.
	// It is doubly so here because the two taps differ only by what the MASTER LANE did, and this
	// fixture's lanes are empty, where the two taps are the same signal by construction.
	//
	// **ITS HONESTY GUARD IS A VALUE ASSERTION INSTEAD**, in `test_fx_sklitter_feedback.cpp`: with a
	// gain device in the master lane and `Feedback` up, the number written into the ring differs
	// between the two settings and equals the one the chosen tap predicts. That is the claim the
	// switch makes, and a difference check over a rendered buffer could not have made it.
	// **AND `Grain Order` IS EXCLUDED FOR THE SAME REASON, WHICH IS WORTH STATING SEPARATELY BECAUSE
	// IT IS A STRONGER VERSION OF IT** (backlog HU, `Param::GrainOrder`). It chooses which of the
	// ring's three writers lands LAST in Grain — and this matrix's base row has `Feedback` and
	// `Dry Write` at their defaults of zero, so `writersActive()` is false and there is no flush for
	// `P Write` to be ordered against. The switch is not merely hard to hear here: it is guarded off,
	// by the same condition that makes the two states bit-identical at zero knobs. A matrix that
	// moves ONE control from a base row cannot reach it by construction.
	//
	// **ITS GUARD IS A VALUE TABLE INSTEAD**, in `test_fx_sklitter_grainorder.cpp`: six settings with
	// the circuit engaged, reporting the A-against-B ring RMS at each, so the control is held to
	// changing the sound AND the settings where it changes it most are written down rather than left
	// to be hunted by ear.
	// **AND `Groove Amount` (backlog IO), FOR A THIRD REASON: ITS GATE IS NOT `Mode`.** It is gated on
	// `Groove Mode` (live wherever that is not Straight), and this matrix reads every gate as a `Mode` gate
	// (`relevantFor(desc, mi)`). In the base row Groove Mode is Straight, where the amount is hidden and
	// inert — correctly. Its guard is the value cases: the slot starts it moves in Swing, Groove and Free
	// ("backlog IG: Shuffle at 16 slices ...", "backlog IO: Free ..."), and the chained gate the surfaces
	// follow ("backlog IO: Groove's amount ... hides in Straight").
	// **AND `Free Phase` FOR THE SAME REASON** — gated on the three Free modes, not on `Mode`.
	constexpr size_t kExcludedFromMatrix = 4; // FB Tap, Grain Order, Groove Amount, Free Phase
	// **THE COMPLETENESS GUARD, AND IT STOPS AT THE PERFORM CIRCLE ON PURPOSE.** Every control up to
	// `Cfg1` is moved here but Enabled, Engage and Mode, which the harness sets itself.
	//
	// **IT SAID `MacroN` UNTIL 2026-09-19 AND THE CIRCLE'S BLOCK IS TWO CONTROLS SHORTER AT ITS HEAD.**
	// The `Macro Cfg` and `Macro` knobs left this list because they no longer exist, and the four
	// waypoint macros left the circle for the same reason — a waypoint is a configuration at macro 1.0
	// now, with the puck's radius supplying the amount. `moves` holds fourteen where it held sixteen and
	// the arithmetic below is unchanged in shape.
	//
	// **THE SEVEN FROM `Cfg1` ON ARE READ BY THE DEVICE SINCE TASK 6** — an earlier comment here said they
	// were not, and that stopped being true the moment `performPushOf` reached the plan sites. They stay
	// out of this matrix for a NEW reason, and it is a property of this fixture rather than of the code:
	// **the matrix moves ONE control away from a base row, and each arrangement fixes the puck.** A
	// waypoint the puck gives no weight cannot change the sound however well the circle is wired, so a
	// waypoint config would read DEAD here for arrangements that do not face it — correctly and
	// uselessly. `Perf Depth` is in the same position: with the puck where an arrangement puts it, depth
	// scales a push that may be zero.
	//
	// **`test_fx_devices.cpp`'s sweep CAN see them, and the difference is its arrangements.** It probes
	// each control against a set of rows that already includes `Perf X` and `Perf Y` at their ends — so a
	// waypoint's configuration is asked about at a puck that gives that waypoint weight. That is where the
	// honesty guard for these lives.
	//
	// **TWO HALVES, BECAUSE ONE COUNT CANNOT SAY BOTH THINGS.** The first says `moves` covers EXACTLY the
	// controls declared BEFORE the circle, so a control inserted anywhere among them fails here as it
	// always did. The second says nothing has been APPENDED PAST the circle unnoticed — which the old
	// `kCount` form caught for free and this one cannot, because an eighth perform control moves `kCount`
	// and leaves `Cfg1` exactly where it is.
	// all but Enabled, Engage, Mode — and FB Tap, which the paragraph above excludes by name.
	// **AND `Input Gain` PAST IT (backlog KZ)** — in `moves`, so the first half counts it as the one beyond `Cfg1`.
	// **AND `Freeze` AFTER THAT (backlog KX)**, the second beyond `Cfg1`.
	// **AND `Record Gate` AFTER IT (backlog MJ)**, the third.
	REQUIRE(moves.size() + 3 + kExcludedFromMatrix == static_cast<size_t>(sc::Param::Cfg1) + 3);
	REQUIRE(static_cast<size_t>(sc::Param::InputGain) == static_cast<size_t>(sc::Param::PerfDepth) + 1);
	REQUIRE(static_cast<size_t>(sc::Param::Freeze) == static_cast<size_t>(sc::Param::InputGain) + 1);
	REQUIRE(static_cast<size_t>(sc::Param::RecordGate) == static_cast<size_t>(sc::Param::Freeze) + 1);
	REQUIRE(static_cast<size_t>(sc::Param::kCount) == static_cast<size_t>(sc::Param::RecordGate) + 1);

	// **TWO ARRANGEMENTS, AND A CELL IS DEAD ONLY IF IT IS DEAD IN BOTH.** A first run at one arrangement
	// reported Seq Length dead in every looper mode; it was not — at exactly Zone B 0.4 both of the bar
	// counter's weights in `planFor` land on a null, so no bar differs from another THERE. A single
	// arrangement measures its own coincidences.
	struct Arrangement {
		float zoneA;
		float zoneB;
		/// **THE PUCK, WHICH IS WHERE THE MACRO WENT.** These two were `macroConfig` and `macro` until
		/// 2026-09-19; both controls are deleted, and the quantity they carried is now a POSITION on the
		/// circle — the direction picks which waypoint's configuration is heard and the distance is how
		/// much of it. Held OFF THE CENTRE deliberately: at the origin the blended push is exactly zero
		/// and every arrangement would probe a device with no macro influence at all, which is a state
		/// the device can be in and a poor one to measure fourteen controls against.
		float perfX;
		float perfY;
		float rate; // off a division in the second, or Quantized has nothing to snap
	};
	// Half-way east in the first (E's configuration at half strength, nothing from the other three), a
	// third of the way south in the second — two different waypoints at two different amounts, which is
	// the spread the old (macro, config) pairs gave.
	// **AND A THIRD, ADDED FOR `Send Override` AND FOR THE REASON THE PARAGRAPH ABOVE ALREADY GIVES.**
	// The delay send needs TWO things: the probability this knob overrides, and `delaySendBits > 0`,
	// which is drawn from Zone B's timbral bank and which the knob does not touch. MEASURED over slices
	// 0..31 at Zone A 0.35: those bits are zero for all 32 at Zone B 0.40 and nonzero for all 32 at
	// 0.35 — so the first two arrangements sit where the OTHER gate is shut, and with only those two
	// the knob read DEAD in Repeat and Pitch while reading live in Time, Shuffle and Pattern. That
	// split is the tell: a control genuinely dead in a mode is dead in all of them. The third
	// arrangement stands where the send can happen at all, which is what the sentence above means by a
	// single arrangement measuring its own coincidences.
	//
	// **ITS PUCK IS AT THE CENTRE, AGAINST THE RULE THE STRUCT'S COMMENT STATES, AND THE EXCEPTION IS
	// DISCLOSED RATHER THAN QUIET.** A centred puck is a zero push, which keeps `phRawB` at zero and so
	// keeps `planFor` on its DISCRETE branch — the branch the measurement above was taken on, and the
	// one where the plan's `stereoWidth` is zero for all 32 slices. Off-centre the push makes `phRawB`
	// nonzero, `planFor` takes its full-evolution branch, and the plan's width becomes nonzero.
	//
	// **AND THAT IS A REAL DEFECT IN A CONTROL THAT IS NOT THIS ONE, FOUND HERE AND DELIBERATELY NOT
	// FIXED HERE.** With the third puck off-centre this case reports `Stereo Width` as a LIAR in all
	// five looper modes — hidden in them by `kModeBit(Mode::Grain)` and changing the sound in them
	// anyway, because `widthNow_ = target_.stereoWidth` (`sklitter.cpp:643`) is applied inside
	// `playLooper`'s sample loop (`:1608`), which Grain never runs. Whether the remedy is to widen the
	// declared gate or to stop applying the width outside Grain is a question about the SOUND, so it
	// is a design decision rather than part of the commit that happened to surface it. The two existing
	// arrangements never had a nonzero plan width, which is why it has stood.
	const Arrangement arrangements[] { { 0.4f, 0.4f, 0.5f, 0.0f, 0.5f },
	                                   { 0.63f, 0.57f, 0.0f, 0.3f, 0.47f },
	                                   { 0.35f, 0.35f, 0.0f, 0.0f, 0.62f } };

	std::vector<std::string> dead;
	std::vector<std::string> liars;
	for (int mi = 0; mi < static_cast<int>(Mode::kCount); ++mi) {
		const auto m = static_cast<Mode>(mi);
		for (const Move& mv : moves) {
			bool movedAnywhere = false;
			std::string detail;
			for (const Arrangement& arr : arrangements) {
				sc::Row base = baseRow(m);
				base[static_cast<size_t>(sc::Param::ZoneA)] = arr.zoneA;
				base[static_cast<size_t>(sc::Param::ZoneB)] = arr.zoneB;
				base[static_cast<size_t>(sc::Param::PerfX)] = arr.perfX;
				base[static_cast<size_t>(sc::Param::PerfY)] = arr.perfY;
				base[static_cast<size_t>(sc::Param::Rate)] = arr.rate;
				const std::vector<float> baseAudio = render(base, base);
				REQUIRE(!baseAudio.empty());
				sc::Row moved = base;
				moved[static_cast<size_t>(mv.p)] = mv.value;
				const bool atPress = differs(render(moved, moved), baseAudio);
				const bool live = differs(render(base, moved), baseAudio);
				movedAnywhere = movedAnywhere || atPress || live;
				detail += std::string(" [press ") + (atPress ? "moves" : "still") + ", live "
				          + (live ? "moves" : "still") + "]";
			}
			const auto& desc = sc::kParamDescs[static_cast<size_t>(mv.p)];
			const bool relevant = feathers::device::relevantFor(desc, static_cast<double>(mi));
			const std::string cell = std::string(sc::kModeNames[mi]) + " / " + desc.name + detail;
			// **ONE DISCLOSED EXCEPTION, AND IT NAMES A CELL RATHER THAN A CONTROL (backlog GT).** This
			// fixture drives a transport that rolls and never stops, and in GRAIN the whole of
			// `Transport Locked` is the stop: `playGrain` scatters over the bar instead of walking it
			// and never calls `advanceSlice`, so there is no `sliceIndex_` for the phase derivation to
			// install. The control is live there — a stopped host silences the device and freezes the
			// ring — and this instrument is blind to the condition, which is not the same thing as the
			// control being dead.
			//
			// **GATING IT OUT OF GRAIN WOULD HAVE MADE THIS GREEN AND THE DEVICE WORSE.** A Grain user
			// who wants the buffer held when the DAW stops is asking for exactly what the control does.
			// The honesty rule is satisfied by a case that CAN see it: `backlog GT: the stop and the
			// freeze reach GRAIN too` asserts the dry passthrough and the unchanged ring by value, with
			// a lock-off control proving it is not vacuous. The exception is spelled as a whole cell
			// name so it cannot quietly excuse the same control in a mode this fixture does reach.
			//
			// **AND SINCE 2026-09-22 THE FIVE SLICE MODES TOO, FOR THE SAME REASON NOW THAT IT IS TRUE
			// THERE.** The GU decision put a press on the song's phase in BOTH settings, so on an
			// unbroken transport the settings are meant to be identical and only a stop separates them.
			// This matrix went on reading the control MOVING in these five, and that was a defect rather
			// than the feature: `lockedCursor` truncated, and a song position one ulp low at a slot end
			// ran the lock a whole slice late (-1.5 dBFS peak, up to 177 of 300 blocks). With it rounding,
			// these cells read still, which is correct. The stop they cannot see is held by value in
			// `backlog GT: the device stops with the transport, the buffer FREEZES` and `backlog GT: on a
			// restart the cursor is the one the SONG names`, a host loop in `backlog GT: a loop or a scrub
			// the host makes WITHOUT stopping`, and the steady-transport identity by `backlog GT: a hair
			// either side of a slot end`.
			static const char* const kLockBlind[] { "Grain / Transport Locked",   "Repeat / Transport Locked",
				                                    "Time / Transport Locked",    "Shuffle / Transport Locked",
				                                    "Pattern / Transport Locked", "Pitch / Transport Locked" };
			bool blindHere = false;
			for (const char* blind : kLockBlind) {
				blindHere = blindHere || cell.rfind(blind, 0) == 0;
			}
			// **AND `Time / P Write`, SINCE TIME'S ZONE OVERRIDE (2026-09-23) — THE ARRANGEMENTS, NOT THE
			// CONTROL.** Time now holds each plan for `1 + Zone B·(N−1)` slots and reads slot k from source
			// `k / that`, so above a low Zone B the read stays in the early part of the bar while the slots
			// that pass the overlap duck and write are the late ones: what is written is not what is read
			// back. All three arrangements here sit at Zone B 0.35 or more. The control acts where the hold
			// is short — `Time's P Write reaches the sound where the hold lets the read reach the write`
			// asserts it at Zone B 0.
			blindHere = blindHere || cell.rfind("Time / P Write", 0) == 0;
			if (relevant && !movedAnywhere && !blindHere) {
				dead.push_back(cell);
			}
			if (!relevant && movedAnywhere) {
				liars.push_back(cell);
			}
			MESSAGE((relevant ? "shown  " : "hidden ") << cell);
		}
	}
	// **ASSERTED, EACH CELL BY NAME.** The first run found five: P Write shown in Repeat, Density hidden
	// in Pitch, a direction shown in Burst, Quantized hidden in Burst, Gamma shown in Burst —
	// every one a statement the reference's own menu makes and its engine contradicts.
	for (const auto& d : dead) {
		CAPTURE(d);
		FAIL_CHECK("DEAD — shown in this mode and moves nothing");
	}
	for (const auto& d : liars) {
		CAPTURE(d);
		FAIL_CHECK("LIAR — hidden in this mode and changes the sound");
	}
	CHECK(dead.empty());
	CHECK(liars.empty());
}

TEST_CASE("Time's P Write reaches the sound where the hold lets the read reach the write") {
	// The MATRIX's disclosed `Time / P Write` cell, held by the matrix's own measure at an arrangement that
	// can express it: Zone B 0 is a hold of one slot, so Time reads its bar straight through and what a
	// slice writes is read back a bar later. And at the matrix's first arrangement (Zone B 0.4) the same
	// move changes nothing — the reason the exception exists, asserted rather than asserted about.
	const auto render = [](float zoneB, float pWrite) {
		Settings cfg = looper(Mode::Time, 0.5f);
		cfg.zoneA = 0.4f;
		cfg.zoneB = zoneB;
		cfg.pWrite = pWrite;
		Sklitter s;
		s.prepare(kRate, 4096);
		double beats = 0.0;
		std::vector<float> l(960);
		std::vector<float> r(960);
		std::vector<float> out;
		size_t n = 0;
		int played = 0;
		for (int block = 0; block < 700 && played < 300; ++block) {
			for (int i = 0; i < 960; ++i, ++n) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
				const double ph2 = 2.0 * 3.14159265358979 * 331.0 * static_cast<double>(n) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph) + 0.3 * std::sin(ph2));
				r[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph2) + 0.3 * std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			if (block == 0) {
				s.setTarget(cfg);
			}
			s.processStereo(l.data(), r.data(), 960);
			beats += 960.0 / (kRate * 0.5);
			if (s.statusForTest() == Status::Playing) {
				++played;
				out.insert(out.end(), l.begin(), l.end());
			}
		}
		return out;
	};
	const auto maxDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
		REQUIRE(a.size() == b.size());
		float worst = 0.0f;
		for (size_t i = 0; i < a.size(); ++i) {
			worst = std::max(worst, std::abs(a[i] - b[i]));
		}
		return worst;
	};
	const float shortHold = maxDiff(render(0.0f, 0.0f), render(0.0f, 0.9f));
	const float longHold = maxDiff(render(0.4f, 0.0f), render(0.4f, 0.9f));
	CAPTURE(shortHold);
	CAPTURE(longHold);
	CHECK(shortHold > 1e-3f);
	CHECK(longHold <= 1e-6f);
}

TEST_CASE("PARITY: the macro pushes both zones' phase before the plan is drawn, as the reference's caller does") {
	// **FOUND BY A DIFFERENTIAL, NOT BY READING.** The reference's `dsp/scatter.hpp` was compiled beside
	// `planFor` and both were asked for 153,664 plans over a zone x macro x bar x slice grid. With the
	// reference's own caller (`stutterer.cpp`) building the offsets, 71% differed; with the macro's phase
	// push removed from that caller, NONE did. `planFor` was a faithful port; its callers were not:
	//
	//     float macroZoneAPhase = macroNorm * staticTriangles.zoneAMacroInfluence * kMacroPhaseMax;
	//     float macroZoneBPhase = macroNorm * staticTriangles.zoneBMacroInfluence * kMacroPhaseMax;
	//     ScatterPhaseOffsets offsets{ zoneAPhaseOffset + macroZoneAPhase, zoneBPhaseOffset + macroZoneBPhase,
	//                                  ..., isRepeat ? 0.0f : staticTriangles.delayScale, scatterBarIndex };
	//
	// After the fix the only field that still differs is `delaySendBits`, where this port deliberately
	// clamps the reference's undefined shift (see `planFor`). The reference cannot live in this repo, so
	// what is pinned here is the formula, restated from that source, and that every caller uses it.
	namespace sc = feathers::fx::sklitter;
	using owlet::dsp::triangleSimpleUnipolar;
	namespace phi = owlet::dsp::phi;

	SUBCASE("the push, as a value") {
		const sc::Coordinate a { 0.2f, 1.0f };
		const sc::Coordinate b { 0.7f, 0.0f };
		const sc::Coordinate mc { 0.3f, 2.0f };
		const PhaseOffsets o = sc::offsetsFor(a, b, mc, 0.8f, 5.0f, 2, false);
		const float wantA = 1.0f + 0.8f * triangleSimpleUnipolar(0.3f * phi::kPhi050, 0.5f) * 0.3f;
		const float wantB = 0.0f + 0.8f * triangleSimpleUnipolar(0.3f * phi::kPhi075, 0.5f) * 0.3f;
		CHECK(o.zoneA == wantA);
		CHECK(o.zoneB == wantB);
		// AND THE PUSH IS NOT ZERO HERE, or the two lines above compare laps with laps.
		CHECK(o.zoneA != 1.0f);
		CHECK(o.zoneB != 0.0f);
		CHECK(o.macroConfig == 2.0f);
		CHECK(o.gamma == 5.0f);
		CHECK(o.barIndex == 2);
		CHECK(o.macroPushesDelay);
		// *"No delay for Repeat"*.
		CHECK_FALSE(sc::offsetsFor(a, b, mc, 0.8f, 5.0f, 2, true).macroPushesDelay);
	}

	// **THE RESTATEMENT TAKES THE CIRCLE'S ROAD NOW, because the device has no other.** `cfg.macro` and
	// `cfg.macroConfig` were what these three read; both controls are deleted (2026-09-19), so the macro
	// arrives as a PUCK POSITION and the knob coordinate the port's signature still takes is a zero
	// (`sklitter.cpp`'s `kNoKnobConfig`, restated here rather than imported so this stays a restatement).
	const sc::Coordinate kNoKnobConfig { 0.0f, 0.0f };
	const auto playing = [](Mode m) {
		Settings cfg = looper(m, 0.5f);
		cfg.zoneA = 0.35f;
		cfg.zoneB = 0.55f;
		withMacro(cfg, 0.8f, 0.3f); // the puck 0.8 east, onto a waypoint configured at 0.3
		return cfg;
	};
	const auto pushOfSettings = [](const Settings& cfg) {
		return sc::performPushOf(cfg, sc::performWeightsOf(cfg.perfX, cfg.perfY), cfg.perfDepth);
	};
	const auto withPush = [&](const Settings& cfg, int32_t index, int32_t bar, bool repeat) {
		const sc::Coordinate a = sc::splitCoordinate(cfg.zoneA);
		const sc::Coordinate b = sc::splitCoordinate(cfg.zoneB);
		const MacroPush push = pushOfSettings(cfg);
		PlanCache cold;
		return planFor(a.position, b.position, kNoKnobConfig.position, push, index,
		               sc::offsetsFor(a, b, kNoKnobConfig, push, cfg.gamma, bar, repeat), &cold);
	};
	// THE CONTROL: the SAME push inside `planFor`, and offsets that never received the two ZONE pushes —
	// which is the term this case is about and the one the port lost once.
	const auto withoutPush = [&](const Settings& cfg, int32_t index, int32_t bar) {
		PhaseOffsets o;
		o.barIndex = bar;
		PlanCache cold;
		return planFor(cfg.zoneA, cfg.zoneB, kNoKnobConfig.position, pushOfSettings(cfg), index, o, &cold);
	};
	// **UNDER THE REPEAT HOLD (2026-09-23)** a slot plays the plan drawn where its hold began, so the
	// restatement walks the bar as the reference does (`referenceHeldPlan`) and draws each fresh plan with
	// the road under test.
	const auto held = [](const Sklitter& s, const Settings& cfg, int32_t slice, const auto& draw) {
		const float za = sc::splitCoordinate(cfg.zoneA).position;
		return referenceHeldPlan(cfg.mode, za, sc::splitCoordinate(cfg.zoneB).position, s.sliceCountForTest(), slice,
		                         draw, [&](int32_t i, const SlicePlan& p) {
			                         return scheduleFor(cfg.mode, za, i, s.sliceCountForTest(), s.sliceFramesForTest(), p,
			                                            s.bufferFramesForTest(), s.minSliceFramesForTest())
			                             .grainLength;
		                         });
	};

	SUBCASE("the playing slice is drawn with it") {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		const Settings cfg = playing(Mode::Shuffle);
		s.setTarget(cfg);
		double beats = 0.0;
		drive(s, 96000 + 9600, 0.0f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		size_t checks = 0;
		size_t pushMatters = 0;
		for (int n = 0; n < 200; ++n) {
			drive(s, 480, 1.0f, &beats);
			const int32_t bar = s.barIndexForTest() % 4;
			const SlicePlan want =
			    held(s, cfg, s.sliceIndexForTest(), [&](int32_t i) { return withPush(cfg, i, bar, false); });
			CHECK(s.planForTest() == want);
			if (!(want == held(s, cfg, s.sliceIndexForTest(), [&](int32_t i) { return withoutPush(cfg, i, bar); }))) {
				++pushMatters;
			}
			++checks;
		}
		CAPTURE(pushMatters);
		// AND THE PUSH CHANGES THE PLANS THIS DEVICE PLAYED, or a push-free device passes the line above.
		CHECK(pushMatters > checks / 4);
	}

	SUBCASE("the Grain voices are drawn with it — the second road") {
		Sklitter s;
		s.prepare(kRate, 4096);
		const Settings cfg = playing(Mode::Grain);
		s.setTarget(cfg);
		size_t pushMatters = 0;
		for (int32_t g = 0; g < 64; ++g) {
			const int32_t bar = s.barIndexForTest() % 4;
			CHECK(s.grainPlanForTest(g) == withPush(cfg, g, bar, false));
			if (!(withPush(cfg, g, bar, false) == withoutPush(cfg, g, bar))) {
				++pushMatters;
			}
		}
		CHECK(pushMatters > 16u);
	}

	SUBCASE("the published picture is drawn with it — the third road") {
		// The card's pattern band is computed apart from the playing slice, at rest, from the same
		// controls; a push the device plays and the picture omits draws an arrangement nobody hears.
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = playing(Mode::Shuffle);
		cfg.engage = false;
		s.setTarget(cfg);
		std::vector<float> published;
		s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
			                                        static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		                                        } });
		double beats = 0.0;
		driveSine(s, 9600, &beats);
		s.publishReadings();
		REQUIRE(published.size() == sc::kReadingCount);
		const auto steps = static_cast<size_t>(std::lround(published[static_cast<size_t>(sc::Reading::Steps)]));
		const size_t bars = static_cast<size_t>(cfg.seqBars);
		REQUIRE(steps >= bars);
		const auto perBar = static_cast<int32_t>(steps / bars);
		const size_t barFrames = s.sliceFramesForTest();
		const size_t buffer = s.bufferFramesForTest() > 0 ? s.bufferFramesForTest() : barFrames;
		size_t pushMatters = 0;
		// **A LONG GRAIN IS ONE CELL AND THE STEPS UNDER IT ARE COVERED (backlog IX)** — the head steps by the
		// grain, so the walk restated here does too.
		size_t coveredUntil = 0;
		for (size_t k = 0; k < steps && k < sc::kPictureSteps; ++k) {
			const size_t base = sc::stepReading(k);
			CAPTURE(k);
			if (k < coveredUntil) {
				CHECK(published[base + 1u]
				      == static_cast<float>(static_cast<uint32_t>(sc::StepFlag::Known)
				                            | static_cast<uint32_t>(sc::StepFlag::Covered)));
				continue;
			}
			const auto bar = static_cast<int32_t>(k / static_cast<size_t>(perBar));
			const auto at = static_cast<int32_t>(k % static_cast<size_t>(perBar));
			const float za = sc::splitCoordinate(cfg.zoneA).position;
			const auto spanOf = [&](int32_t i, const SlicePlan& p) {
				return scheduleFor(cfg.mode, za, i, perBar, barFrames, p, buffer, s.minSliceFramesForTest()).grainLength;
			};
			// UNDER THE REPEAT HOLD — the step plays the plan its hold began with (`referenceHeldPlan`).
			const SlicePlan want = referenceHeldPlan(cfg.mode, za, sc::splitCoordinate(cfg.zoneB).position, perBar, at,
			                                         [&](int32_t i) { return withPush(cfg, i, bar, false); }, spanOf);
			const SliceSchedule sch = scheduleFor(cfg.mode, za, at, perBar, barFrames, want, buffer, s.minSliceFramesForTest());
			coveredUntil = k + static_cast<size_t>(sch.grainLength);
			CHECK(published[base] == static_cast<float>(sch.startOffset) / static_cast<float>(buffer));
			uint32_t flags = static_cast<uint32_t>(sc::StepFlag::Known);
			flags |= static_cast<uint32_t>(sch.grainLength - 1) << sc::kStepSpanShift;
			if (want.subdivisions > 1) {
				flags |= static_cast<uint32_t>(want.subdivisions - 1) << sc::kStepRatchetShift;
			}
			if (want.shouldReverse) {
				flags |= static_cast<uint32_t>(sc::StepFlag::Reversed);
			}
			if (want.shouldPitchUp) {
				flags |= static_cast<uint32_t>(sc::StepFlag::PitchedUp);
			}
			if (want.useDry || want.macroWantsDry) {
				flags |= static_cast<uint32_t>(sc::StepFlag::Dry);
			}
			CHECK(published[base + 1u] == static_cast<float>(flags));
			const SlicePlan bare = referenceHeldPlan(cfg.mode, za, sc::splitCoordinate(cfg.zoneB).position, perBar, at,
			                                         [&](int32_t i) { return withoutPush(cfg, i, bar); }, spanOf);
			const SliceSchedule bareSch = scheduleFor(cfg.mode, sc::splitCoordinate(cfg.zoneA).position, at, perBar,
			                                          barFrames, bare, buffer, s.minSliceFramesForTest());
			if (bareSch.startOffset != sch.startOffset || bare.shouldReverse != want.shouldReverse
			    || bare.shouldPitchUp != want.shouldPitchUp) {
				++pushMatters;
			}
		}
		CAPTURE(pushMatters);
		CHECK(pushMatters > 0u);
	}

	SUBCASE("Repeat withholds the macro's push on the delay decision") {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		const Settings cfg = playing(Mode::Repeat);
		s.setTarget(cfg);
		double beats = 0.0;
		drive(s, 96000 + 9600, 0.0f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		for (int n = 0; n < 100; ++n) {
			drive(s, 480, 1.0f, &beats);
			CHECK(s.planForTest() == withPush(cfg, s.repeatLoopIndexForTest(), s.barIndexForTest() % 4, true));
		}
		// AND THE FLAG DECIDES SOMETHING SOMEWHERE, or the Repeat line is the Shuffle line. Not at the
		// arrangement above — measured zero there — so over a grid, as the differential found it.
		size_t flagMatters = 0;
		for (const float zb : { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f }) {
			for (const float mcv : { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f }) {
				Settings g = cfg;
				g.zoneB = zb;
				withMacro(g, 0.9f, mcv);
				for (int32_t i = 0; i < 16; ++i) {
					if (!(withPush(g, i, 0, true) == withPush(g, i, 0, false))) {
						++flagMatters;
					}
				}
			}
		}
		CAPTURE(flagMatters);
		CHECK(flagMatters > 0u);
	}
}

TEST_CASE("PARITY: a ratchet sub-slice is floored at 24 ms, as a duration") {
	// The reference: `constexpr float kMinSubSliceMs = 24.0f; kMinSubSliceSamples = kMinSubSliceMs * 44.1f`,
	// and a sub-slice under it becomes `min(kMinSubSliceSamples, currentSliceLength)`. Ours had no floor.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(sc::kReferenceRate, 4096);
	CHECK(s.minSubSliceFramesForTest() == 1058u); // 24 * 44.1, truncated, as the reference computes it
	CHECK(s.subSliceLengthForTest(48000, 4) == 12000u); // above the floor: the plain division
	CHECK(s.subSliceLengthForTest(8000, 12) == 1058u);  // 666 would be under it: floored
	CHECK(s.subSliceLengthForTest(500, 4) == 500u);     // and never past the slice itself
	// A DURATION: at twice the rate, twice the frames.
	Sklitter d;
	d.prepare(sc::kReferenceRate * 2.0, 4096);
	CHECK(d.minSubSliceFramesForTest() == 2116u);
	CHECK(d.subSliceLengthForTest(16000, 12) == 2116u);
}

namespace {

/// **THE PICTURE A FRESH DEVICE PUBLISHES FOR THESE SETTINGS, AT REST** — the independent restatement
/// the stale-band cases compare against. A new device has no key to be stale against, so whatever it
/// publishes is what the controls imply.
std::vector<float> freshRestingPicture(const Settings& settings) {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = settings;
	cfg.engage = false;
	s.setTarget(cfg);
	std::vector<float> published;
	s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
		                                                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                                                   } });
	double beats = 0.0;
	driveSine(s, 9600, &beats);
	s.publishReadings();
	return published;
}

/// THE ARRANGEMENT HALF OF A PUBLISH — the step count and every step's source and flags. The header's
/// head and fill move with time and are not what these cases are about.
struct Arrangement {
	size_t steps = 0;
	std::vector<float> sources;
	std::vector<uint32_t> flags;
};

Arrangement arrangementOf(const std::vector<float>& published) {
	namespace sc = feathers::fx::sklitter;
	Arrangement a;
	if (published.size() != sc::kReadingCount) {
		return a;
	}
	a.steps = static_cast<size_t>(std::lround(published[static_cast<size_t>(sc::Reading::Steps)]));
	for (size_t k = 0; k < a.steps && k < sc::kPictureSteps; ++k) {
		const size_t base = sc::stepReading(k);
		a.sources.push_back(published[base]);
		a.flags.push_back(static_cast<uint32_t>(std::lround(published[base + 1u])));
	}
	return a;
}

} // namespace

TEST_CASE("the band is redrawn when the LENGTHS it divides by move, not only when a control does") {
	// **THE SCATTER RIBBON SOMETIMES DID NOT UPDATE when a control that changes it moved.**
	//
	// THE MECHANISM: `publishReadings` recomputes the arrangement only when its `RestKey` moves, and
	// the walk divides by `sliceFrames_` and `bufferFrames_` — the RESOLVED lengths — while the key held
	// the SETTING `bufferBars`. The two move at different moments: the setting when the hand moves it,
	// the frames when `prewarm` next runs, which is not while bypassed and not while engaged. So the key
	// moved first, recomputed against the old frames, and when the frames caught up nothing recomputed.
	namespace sc = feathers::fx::sklitter;
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.6f;
	cfg.engage = false;
	cfg.bufferBars = 4.0;

	std::vector<float> published;
	const feathers::device::ReadingsSink sink { &published, [](void* ctx, const float* v, size_t n) {
		                                           static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                                           } };

	SUBCASE("a device loaded BYPASSED, then enabled") {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings off = cfg;
		off.enabled = false;
		s.setTarget(off);
		s.setReadingsSink(sink);
		double beats = 0.0;
		driveSine(s, 9600, &beats);
		s.publishReadings();

		s.setTarget(cfg);
		driveSine(s, 9600, &beats);
		s.publishReadings();
		REQUIRE(s.bufferFramesForTest() == 4u * s.sliceFramesForTest());

		const Arrangement want = arrangementOf(freshRestingPicture(cfg));
		const Arrangement got = arrangementOf(published);
		REQUIRE(want.steps > 1);
		CHECK(got.steps == want.steps);
		CHECK(got.sources == want.sources);
		CHECK(got.flags == want.flags);
	}

	SUBCASE("the Buffer length changed WHILE ENGAGED, then released") {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings one = cfg;
		one.bufferBars = 1.0;
		one.engage = true;
		s.setTarget(one);
		s.setReadingsSink(sink);
		double beats = 0.0;
		for (int block = 0; block < 400 && s.statusForTest() != Status::Playing; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		s.publishReadings();

		// Turned to four bars under the playing loop — the key moves here, against the captured buffer.
		Settings four = one;
		four.bufferBars = 4.0;
		s.setTarget(four);
		driveSine(s, 960, &beats, 960);
		s.publishReadings();

		// Released: the pre-warm now resolves the four-bar buffer.
		four.engage = false;
		s.setTarget(four);
		for (int block = 0; block < 20; ++block) {
			driveSine(s, 960, &beats, 960, block * 960);
		}
		REQUIRE(s.statusForTest() == Status::Off);
		s.publishReadings();
		REQUIRE(s.bufferFramesForTest() == 4u * s.sliceFramesForTest());

		const Arrangement want = arrangementOf(freshRestingPicture(cfg));
		const Arrangement got = arrangementOf(published);
		REQUIRE(want.steps > 1);
		CHECK(got.steps == want.steps);
		CHECK(got.sources == want.sources);
		CHECK(got.flags == want.flags);
	}
}

TEST_CASE("in Grain, turning Rate while ENGAGED re-cuts the band, as it does at rest") {
	// **THE SECOND ROAD TO A STILL BAND.** While playing, the band's cut is `numSlices_`, and the only
	// line that refreshes it from Rate is in `advanceSlice` — which Grain never calls. So Rate re-cut the
	// band at rest and froze it the moment the gesture began, in Grain and in Grain alone.
	namespace sc = feathers::fx::sklitter;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Grain, 0.5f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.6f;
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> published;
	s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
		                                                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                                                   } });
	double beats = 0.0;
	for (int block = 0; block < 400 && s.statusForTest() != Status::Playing; ++block) {
		driveSine(s, 960, &beats, 960, block * 960);
	}
	REQUIRE(s.statusForTest() == Status::Playing);
	s.publishReadings();
	const Arrangement before = arrangementOf(published);

	Settings finer = cfg;
	finer.rate = 1.0f;
	s.setTarget(finer);
	driveSine(s, 960, &beats, 960);
	s.publishReadings();
	REQUIRE(s.statusForTest() == Status::Playing);

	const Arrangement want = arrangementOf(freshRestingPicture(finer));
	const Arrangement got = arrangementOf(published);
	// The cut the Rate implies, stated from the ladder rather than from either device.
	const auto perBar = static_cast<size_t>(sc::sliceCountFor(Mode::Grain, finer.rate));
	const size_t bars = std::clamp<size_t>(sc::kPictureSteps / perBar, 1, static_cast<size_t>(finer.seqBars));
	CHECK(want.steps == perBar * bars);
	CHECK(got.steps == want.steps);
	// PROVES THE RATE MOVE IS VISIBLE AT ALL. Not the step count: 16 × 4 bars and 32 × 2 bars both
	// fill the 64-step picture, so the count is the same and only the arrangement says it moved.
	CHECK(got.sources != before.sources);
	CHECK(got.sources == want.sources);
	CHECK(got.flags == want.flags);
}

TEST_CASE("backlog FK: a moving Mix and a moving Stereo Width are followed per sample inside the call") {
	// Restated independently, from twins: the output is LINEAR in Mix (`dry + (wet - dry) * m`) and in Stereo Width
	// (each voice keeps `0.5 + w/2` of its side), and neither control reaches what is rendered or written — so a device
	// handed a window must land, sample by sample, on the straight line between two twins held at the ends.
	constexpr int kFrames = 64;
	std::array<float, kFrames> run {};
	for (int i = 0; i < kFrames; ++i) {
		run[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(kFrames - 1);
	}
	struct Out {
		std::vector<float> l, r;
	};
	const auto render = [&](Settings cfg, size_t windowed, int kTotal) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTarget(cfg);
		Out o { std::vector<float>(static_cast<size_t>(kTotal)), std::vector<float>(static_cast<size_t>(kTotal)) };
		for (int i = 0; i < kTotal; ++i) { // a signal with sign changes, so grains and crossings have work to do
			o.l[static_cast<size_t>(i)] = 0.5f * std::sin(0.05f * static_cast<float>(i));
			o.r[static_cast<size_t>(i)] = 0.5f * std::sin(0.031f * static_cast<float>(i));
		}
		std::array<const float*, 32> runs {};
		if (windowed < runs.size()) {
			runs[windowed] = run.data();
		}
		for (int at = 0; at < kTotal; at += kFrames) {
			s.setTransportClock(at120bpm(static_cast<double>(at) / (0.5 * kRate))); // a moving transport: loopers wait for a beat
			if (windowed < runs.size()) {
				kAudioOps.setFieldWindows(&s, runs.data(), runs.size(), kFrames);
			}
			s.processStereo(o.l.data() + at, o.r.data() + at, kFrames);
		}
		return o;
	};
	REQUIRE(kAudioOps.setFieldWindows != nullptr);
	constexpr size_t kNone = 99;

	SUBCASE("Mix, in Burst") {
		Settings cfg = burst(1.0f);
		cfg.engage = true;
		cfg.mix = 0.0f;
		constexpr int kTotal = 8192;
		const Out dry = render(cfg, kNone, kTotal);
		cfg.mix = 1.0f;
		const Out wet = render(cfg, kNone, kTotal);
		cfg.mix = 0.3f; // the scalar the window replaces
		const Out moving = render(cfg, static_cast<size_t>(Param::Mix), kTotal);
		float worst = 0.0f;
		float apart = 0.0f;
		for (size_t i = 0; i < static_cast<size_t>(kTotal); ++i) {
			const float m = run[i % kFrames];
			worst = std::max(worst, std::abs(moving.l[i] - (dry.l[i] + (wet.l[i] - dry.l[i]) * m)));
			apart = std::max(apart, std::abs(wet.l[i] - dry.l[i]));
		}
		CAPTURE(worst);
		CHECK(apart > 0.1f); // not vacuous: playback differs from the input
		CHECK(worst < 1e-5f);
	}
	SUBCASE("Stereo Width, in Grain") {
		Settings cfg = looper(Mode::Grain, 0.5f);
		cfg.zoneA = 0.8f;
		cfg.mix = 1.0f;
		cfg.stereoWidth = 0.0f;
		constexpr int kTotal = 112000; // past the captured bar and the beat after it
		const Out narrow = render(cfg, kNone, kTotal);
		cfg.stereoWidth = 1.0f;
		const Out wide = render(cfg, kNone, kTotal);
		cfg.stereoWidth = 0.5f;
		const Out moving = render(cfg, static_cast<size_t>(Param::StereoWidth), kTotal);
		float worst = 0.0f;
		float apart = 0.0f;
		for (size_t i = 0; i < static_cast<size_t>(kTotal); ++i) {
			const float w = run[i % kFrames];
			worst = std::max(worst, std::abs(moving.l[i] - (narrow.l[i] + (wide.l[i] - narrow.l[i]) * w)));
			apart = std::max(apart, std::abs(wide.l[i] - narrow.l[i]));
		}
		CAPTURE(worst);
		CHECK(apart > 0.01f);
		CHECK(worst < 1e-5f);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PERFORM CIRCLE, WIRED.                                                                         //
//                                                                                                    //
// The maths is asserted on its own in `test_fx_sklitter_perform.cpp`; these cases are about the       //
// DEVICE — that the push it plays is that blend, that the puck reaches the playing slice and the      //
// resting picture, and that a centred puck leaves today's sound alone.                                //
//--------------------------------------------------------------------------------------------------//

namespace {

/// PER FIELD, AND APPROXIMATE. It was approximate ON PURPOSE while the blend was
/// `knob + (corner - knob) · 1`, which need not round back to `corner`; with the centre a zero push the
/// rim IS exact (`perform.h`), so the slack here is now ordinary caution about a value read back off a
/// PLAYING device rather than a documented rounding. `test_fx_sklitter_perform.cpp` pins the same epsilon
/// on the arithmetic alone, where it is the half-way point that genuinely rounds.
void checkPushNear(const MacroPush& got, const MacroPush& want) {
	CHECK(got.dryThreshold == doctest::Approx(want.dryThreshold).epsilon(1e-6f));
	CHECK(got.dReverse == doctest::Approx(want.dReverse).epsilon(1e-6f));
	CHECK(got.dPitch == doctest::Approx(want.dPitch).epsilon(1e-6f));
	CHECK(got.dDelay == doctest::Approx(want.dDelay).epsilon(1e-6f));
	CHECK(got.gatePull == doctest::Approx(want.gatePull).epsilon(1e-6f));
	CHECK(got.subdivMix == doctest::Approx(want.subdivMix).epsilon(1e-6f));
	CHECK(got.zoneAPush == doctest::Approx(want.zoneAPush).epsilon(1e-6f));
	CHECK(got.zoneBPush == doctest::Approx(want.zoneBPush).epsilon(1e-6f));
}

/// THE PUSH OF ONE WAYPOINT, read off the settings the way `performPushOf` reads it — **at macro 1.0,
/// which is what a waypoint IS since `Macro N/E/S/W` were deleted (2026-09-19)**: its character at full
/// strength, with the puck's radius supplying the amount.
MacroPush waypointPush(const Settings& s, size_t corner) {
	return macroPushOf(1.0f, splitCoordinate(s.cornerCfg[corner]), s.gamma);
}

/// A LOOPER ARRANGEMENT WITH SOMETHING TO SAY: zones off their deadzones, and the E waypoint configured
/// somewhere the macro's terms are live, so a push that changes moves decisions rather than rounding.
/// **THE PUCK IS LEFT AT THE CENTRE**, which is now a device with NO macro influence at all — the cases
/// below move it, and the one that does not is asserting exactly that neutrality.
Settings performBase(Mode m = Mode::Shuffle) {
	Settings cfg = looper(m, 0.5f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.55f;
	cfg.cornerCfg[kCfg2] = 0.3f;
	return cfg;
}

/// THE PLAN THOSE SETTINGS IMPLY AT ONE INDEX, DRAWN FROM A PUSH HANDED IN. Through a cold cache, so it
/// is the arithmetic rather than a cache hit that answers. The knob coordinate the port's signature
/// still takes is a zero, as the device hands it (`sklitter.cpp`'s `kNoKnobConfig`).
SlicePlan planFromPush(const Settings& s, const MacroPush& push, int32_t index, int32_t bar, bool repeat) {
	const Coordinate a = splitCoordinate(s.zoneA);
	const Coordinate b = splitCoordinate(s.zoneB);
	const Coordinate mc { 0.0f, 0.0f };
	PlanCache cold;
	return planFor(a.position, b.position, mc.position, push, index,
	               offsetsFor(a, b, mc, push, s.gamma, bar, repeat), &cold);
}

/// **THE CENTRED PUCK'S PUSH, RESTATED FROM THE DESIGN** (provisional, pending a listening
/// decision on whether it stays): zero in every term but `subdivMix`, which is 0.5. Not `centrePush()`,
/// which is what it checks.
MacroPush restPush() {
	MacroPush p;
	p.subdivMix = 0.5f;
	return p;
}

/// **THE PLAN A PLAYING (non-Repeat) SLOT PLAYS**, which since the repeat hold (2026-09-23) is the plan drawn
/// where its hold began — `planFromPush` through `referenceHeldPlan`, on the device's own grid.
SlicePlan heldPlanFromPush(const Sklitter& dev, const Settings& s, const MacroPush& push, int32_t slice, int32_t bar) {
	const float za = splitCoordinate(s.zoneA).position;
	return referenceHeldPlan(
	    s.mode, za, splitCoordinate(s.zoneB).position, dev.sliceCountForTest(), slice,
	    [&](int32_t i) { return planFromPush(s, push, i, bar, false); },
	    [&](int32_t i, const SlicePlan& p) {
		    return scheduleFor(s.mode, za, i, dev.sliceCountForTest(), dev.sliceFramesForTest(), p,
		                       dev.bufferFramesForTest(), dev.minSliceFramesForTest())
		        .grainLength;
	    });
}

} // namespace

TEST_CASE("the push the device plays IS the blend of its four waypoints") {
	Settings s = performBase();
	s.gamma = 0.7f;
	s.perfX = 0.4f;
	s.perfY = -0.25f;
	s.perfDepth = 0.8f;

	// **AN INDEPENDENT RESTATEMENT, NOT A SECOND CALL TO `blendPush`.** Calling the same helper the
	// implementation calls would assert that `performPushOf` calls it, which is not the claim: the claim
	// is that the eight numbers the device plays are the weighted mixture design §2.2 describes. So the
	// mixture is written out here, term by term, in the weights `performWeightsOf` gives.
	//
	// **IT HAD A FIFTH ARGUMENT, THE KNOBS' OWN PUSH, AND THE LERP FROM IT.** `Macro`/`Macro Cfg` are
	// deleted (2026-09-19), so the centre contributes nothing and the restatement is `Σ wᵢ·cornerᵢ · depth`
	// — `w.centre` appearing nowhere, which is exactly what makes the radius the amount.
	const PerformWeights w = performWeightsOf(s.perfX, s.perfY);
	// **THE MULTIPLIER IS `perfDepth^kDepthCurve`, NOT `perfDepth` — restated here rather than asked of
	// `performAmountOf`** (2026-09-19). The knob's travel is curved; `Settings::perfDepth` is still the
	// KNOB, and the one conversion happens where the push is built. A restatement that used the raw
	// field would have gone red, which is what a restatement is for.
	// **AND THE CENTRE IS A FIFTH TERM SINCE 2026-09-23, with depth measured FROM it**: the share the puck
	// has not spent carries the centre's push (zero but for `subdivMix` 0.5, a provisional design value), and
	// depth scales the way from that centre to the blend — `c + a·(w₀·c + Σ wᵢ·xᵢ − c)`. With c = 0 that is
	// the old `a · Σ`.
	const auto mixedFrom = [&](float c, float n, float e, float sth, float west) {
		const float blended = w.centre * c + w.corner[kCfg1] * n + w.corner[kCfg2] * e + w.corner[kCfg3] * sth
		                      + w.corner[kCfg4] * west;
		return c + std::pow(s.perfDepth, kDepthCurve) * (blended - c);
	};
	const auto mixed = [&](float n, float e, float sth, float west) { return mixedFrom(0.0f, n, e, sth, west); };
	const MacroPush n = waypointPush(s, kCfg1);
	const MacroPush e = waypointPush(s, kCfg2);
	const MacroPush so = waypointPush(s, kCfg3);
	const MacroPush we = waypointPush(s, kCfg4);
	const MacroPush got = performPushOf(s, performWeightsOf(s.perfX, s.perfY), s.perfDepth);
	CHECK(got.dryThreshold == mixed(n.dryThreshold, e.dryThreshold, so.dryThreshold, we.dryThreshold));
	CHECK(got.dReverse == mixed(n.dReverse, e.dReverse, so.dReverse, we.dReverse));
	CHECK(got.dPitch == mixed(n.dPitch, e.dPitch, so.dPitch, we.dPitch));
	CHECK(got.dDelay == mixed(n.dDelay, e.dDelay, so.dDelay, we.dDelay));
	CHECK(got.gatePull == mixed(n.gatePull, e.gatePull, so.gatePull, we.gatePull));
	CHECK(got.subdivMix == mixedFrom(0.5f, n.subdivMix, e.subdivMix, so.subdivMix, we.subdivMix));
	CHECK(got.zoneAPush == mixed(n.zoneAPush, e.zoneAPush, so.zoneAPush, we.zoneAPush));
	CHECK(got.zoneBPush == mixed(n.zoneBPush, e.zoneBPush, so.zoneBPush, we.zoneBPush));
	// NOT VACUOUS: at this puck the blend is neither zero nor any one waypoint, so the eight lines above
	// are not eight copies of one number.
	CHECK_FALSE(got == MacroPush {});
	CHECK_FALSE(got == e);
}

TEST_CASE("a DEFAULT patch plays the circle's CENTRE, bit for bit: no push but the ratchet's 0.5") {
	// **AMENDED 2026-09-23.** The centre is no longer the zero push: a provisional ratchet decision puts
	// `subdivMix` 0.5 there, so a patch nobody has dragged plays Zone A's own ratchet count. Every other
	// term is still zero, to the bit, and that is what `want` below now says.
	// **WHAT THIS CASE USED TO SAY.** It was *"the centre plays the knobs' push, bit for bit"* — the
	// claim that shipping the circle moved nothing until the puck moved, which rested on the centre
	// being the `Macro`/`Macro Cfg` pair. Those controls are deleted (2026-09-19) and the claim is now
	// the simpler one: a patch nobody has dragged has no macro influence whatever. Depth is 1 by default
	// (`sklitter_types.h` says why) and the centre is neutral at EVERY depth, so the default patch is
	// still silent about the circle — for a different reason, and one worth asserting on the value.
	const Settings d;
	const MacroPush got = performPushOf(d, performWeightsOf(d.perfX, d.perfY), d.perfDepth);
	const MacroPush want = restPush();
	CHECK(got == want);
	CHECK(std::memcmp(&got, &want, sizeof(MacroPush)) == 0); // including the sign of every zero
	// AND NOT ONLY AT THE DECLARED WAYPOINTS: whatever the four are configured to, and at any gamma, a
	// centred puck spends nothing on them. (This is what stops the case passing because the defaults
	// happen to build zero pushes — they do not; the not-vacuous line below reads one.)
	for (const float cfg : { 0.0f, 0.37f, 2.3f }) {
		for (const float gamma : { 0.0f, 1.3f }) {
			CAPTURE(cfg);
			CAPTURE(gamma);
			Settings s;
			s.cornerCfg = { cfg, cfg + 0.25f, cfg + 0.5f, cfg + 0.75f };
			s.gamma = gamma;
			const MacroPush viaCircle = performPushOf(s, performWeightsOf(s.perfX, s.perfY), s.perfDepth);
			CHECK(std::memcmp(&viaCircle, &want, sizeof(MacroPush)) == 0);
			// NOT VACUOUS: those same waypoints are emphatically not zero when the puck reaches one.
			CHECK_FALSE(performPushOf(s, performWeightsOf(1.0f, 0.0f), s.perfDepth) == want);
		}
	}
}

TEST_CASE("moving the puck moves the push the PLAYING device draws its plans from") {
	// **THE VALUE, BECAUSE A RENDER CANNOT BE ASKED THIS.** A plan keeps thresholded decisions taken
	// from the push and a buffer is the sound of them, so "the audio changed" would pass for a puck that
	// reached the sound by half a road. `slicePushForTest` is stamped where the playing slice is drawn.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	const Settings cfg = performBase();
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats); // a bar of capture, then playing
	REQUIRE(s.statusForTest() == Status::Playing);
	drive(s, 24000, 1.0f, &beats);        // several sixteenths, so slices have been drawn

	// AT THE CENTRE IT IS THE CENTRE'S PUSH, EXACTLY — no macro but the ratchet's 0.5 (2026-09-23), which is what a patch
	// nobody has dragged sounds like. (It was the zero push from 2026-09-19, and the knobs' own before that.)
	CHECK(s.slicePushForTest() == restPush());

	Settings east = cfg;
	puckOn(east, kCfg2); // the E waypoint, at the default depth of 1
	s.setTarget(east);
	drive(s, 24000, 2.0f, &beats);
	checkPushNear(s.slicePushForTest(), waypointPush(cfg, kCfg2));
	// NOT VACUOUS: the E waypoint's push is not zero, so the two readings above are two different
	// answers rather than one answer read twice.
	CHECK_FALSE(waypointPush(cfg, kCfg2) == MacroPush {});
	CHECK_FALSE(s.slicePushForTest() == MacroPush {});

	// **AND THE PLAN IT IS PLAYING WAS DRAWN FROM THAT PUSH, NOT MERELY STAMPED BESIDE IT.** MEASURED as a
	// gap and then closed: a mutation that keeps the seam on `performPushOf` while handing the plan calls
	// `target_.macro` is INVISIBLE to every line above — the seam is only honest because `advanceSlice`
	// shares ONE `push` variable between the stamp and both calls, which is a property of the code and not
	// something a reading of the seam can establish. This line asks the plan instead.
	// THE FOLD TAKEN FROM THE SETTING, NOT FROM A LITERAL 4: `seqBarIndex()` is `barIndex_ % seqBars`, and
	// a hard-coded 4 goes silently wrong the day Seq Length's default moves — as a perform-circle red.
	// **A WHOLE BAR ON FIRST, BECAUSE A HOLD KEEPS THE PLAN IT CACHED (2026-09-23).** The reference's repeat
	// hold replays a cached plan for up to `repeatSlices` slots whatever the controls do meanwhile, and only a
	// bar's end is sure to cancel it — so a slot a quarter bar after the move may still be playing a plan
	// drawn from the centred push, correctly. Past the bar line every hold was begun under the east push.
	drive(s, 96000, 2.0f, &beats);
	const int32_t bars = east.seqBars > 0 ? east.seqBars : 1;
	const int32_t bar = s.barIndexForTest() % bars;
	CHECK(s.planForTest()
	      == heldPlanFromPush(s, east, s.slicePushForTest(), s.sliceIndexForTest(), bar));
	CHECK_FALSE(s.planForTest() == heldPlanFromPush(s, east, restPush(), s.sliceIndexForTest(), bar));
}

TEST_CASE("the resting picture follows the puck, and the key is what lets it") {
	// **THE BAND IS DRAWN FROM THE SAME PUSH THE DEVICE WOULD PLAY**, so a puck that reaches the sound
	// and not the picture draws an arrangement nobody hears — and one that reaches neither is a frozen
	// key. Both are asserted against a METAMORPHIC pair, not an independent route.
	//
	// **THE PAIR CHANGED WITH THE CONTROLS AND THE SHAPE DID NOT.** It used to be *"the E waypoint's
	// (Macro, Macro Cfg) put on the KNOBS with the puck left at the centre"*, which is unbuildable now
	// that there are no knobs. The pair is a ROTATION instead: the puck on the E rim against the puck on
	// the N rim with N configured exactly as E is. Both are one waypoint at full weight and the same
	// configuration, so the push is identical by the design's own claim — WHICH waypoint the puck faces
	// is nothing, its CONFIGURATION is everything — and the two sides differ in which weight carries it.
	// (The genuinely independent restatement is ninety lines up, where the blend is written out term by
	// term instead of calling `blendPush`. One word for two things is what the next reader trusts instead
	// of looking, so they are named differently.)
	Settings base = performBase();
	Settings east = base;
	puckOn(east, kCfg2);
	Settings viaNorthRim = base;
	viaNorthRim.cornerCfg[kCfg1] = base.cornerCfg[kCfg2];
	puckOn(viaNorthRim, kCfg1); // N is the TOP-LEFT rim point since the 45° rotation

	const Arrangement centre = arrangementOf(freshRestingPicture(base));
	const Arrangement viaPuck = arrangementOf(freshRestingPicture(east));
	const Arrangement viaRotation = arrangementOf(freshRestingPicture(viaNorthRim));
	REQUIRE(viaPuck.steps > 0);
	REQUIRE(viaRotation.steps == viaPuck.steps);
	CHECK(viaPuck.sources == viaRotation.sources);
	CHECK(viaPuck.flags == viaRotation.flags);
	// NOT VACUOUS: the two arrangements are not the arrangement the centred puck draws, so the equality
	// above is the circle arriving rather than every picture being the same picture.
	CHECK((centre.sources != viaRotation.sources || centre.flags != viaRotation.flags));

	SUBCASE("and a device whose puck alone moves REDRAWS — the key holds the push, not the macro") {
		// **THE KEY SHORT BY ONE.** `RestKey` guards the walk so a still device does not recompute
		// sixty-four plans a block. It held `macro`, which no longer decides the picture: with the puck
		// in the key's place the band would compare equal and FREEZE while the puck moved. One device,
		// one control moved, no other input touched — the whole point being that nothing else in the key
		// changes between the two publishes.
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = base;
		cfg.engage = false;
		s.setTarget(cfg);
		std::vector<float> published;
		s.setReadingsSink(feathers::device::ReadingsSink {
		    &published, [](void* ctx, const float* v, size_t n) {
			    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		    } });
		double beats = 0.0;
		driveSine(s, 9600, &beats);
		s.publishReadings();
		const Arrangement before = arrangementOf(published);
		REQUIRE(before.steps > 0);
		Settings moved = cfg;
		puckOn(moved, kCfg2);
		s.setTarget(moved);
		s.publishReadings(); // no frames driven, so the lengths and the window cannot have moved
		const Arrangement after = arrangementOf(published);
		CHECK(after.sources == viaRotation.sources);
		CHECK(after.flags == viaRotation.flags);
		CHECK((after.sources != before.sources || after.flags != before.flags));
	}
}

TEST_CASE("a host that sends a SHORT row leaves the perform tail at its DECLARED defaults") {
	// **`setFields` STARTS FROM `rowOf(Settings{})`, AND THAT LINE IS WHAT MAKES THIS TRUE.** A row
	// zero-filled instead would load Perf Depth as 0 — eleven controls that stay inert however far the
	// puck is dragged — and nothing else in the device would complain. Asked of the seam a host reaches
	// through rather than restated from `stateOf`, which would assert the restatement.
	REQUIRE(kAudioOps.setFields != nullptr);
	const auto tail = static_cast<size_t>(Param::Cfg1);
	// The controls declared BEFORE the circle, which is what "short" means here. **22 since the
	// feedback circuit (2026-09-21, backlog GU/GV) inserted three at 19..21.**
	REQUIRE(tail == 25u); // 23 since `Grain Order` (HU), 24 since `Groove` (IG), 25 its split, 26 `Free Phase` (IO), 25 without `Direction`
	REQUIRE(static_cast<size_t>(Param::kCount) == tail + 10u); // the circle's seven, then `Input Gain` (KZ), `Freeze` (KX), `Record Gate` (MJ)

	Sklitter s;
	s.prepare(kRate, 4096);
	const Row full = rowOf(performBase());
	kAudioOps.setFields(&s, full.data(), tail); // the host stops where the circle begins
	const Settings& got = s.targetForTest();
	const Settings declared;
	CHECK(got.inputGainDb == declared.inputGainDb); // and the knob past the circle at its 0 dB (backlog KZ)
	CHECK(got.freeze == declared.freeze); // and Freeze at Off (backlog KX)
	CHECK(got.recordGate == declared.recordGate); // and the Record Gate off (backlog MJ)
	for (size_t i = 0; i < kCorners; ++i) {
		CAPTURE(i);
		CHECK(got.cornerCfg[i] == declared.cornerCfg[i]);
	}
	CHECK(got.perfX == declared.perfX);
	CHECK(got.perfY == declared.perfY);
	CHECK(got.perfDepth == declared.perfDepth);
	CHECK(got.perfDepth == 1.0f); // named, because 0 is the value a zeroed row would give
	// AND THE HEAD ARRIVED, or the tail could be right because nothing was written at all.
	CHECK(got.zoneB == performBase().zoneB);
	CHECK(got.zoneA == performBase().zoneA);

	SUBCASE("and a depth defaulted to 0 would be AUDIBLE, which is why the default is 1") {
		// The same seam one field shorter still: everything up to Perf Y arrives, Perf Depth defaults.
		// With the puck at the E rim the device must play the E waypoint's push — which it can only do
		// if the depth it defaulted to is 1.
		Sklitter d;
		d.prepare(kRate, 4096);
		d.setTransportClock(at120bpm());
		Settings east = performBase();
		puckOn(east, kCfg2);
		east.perfDepth = 0.0f; // and this value is deliberately NOT sent
		const Row row = rowOf(east);
		const auto sent = static_cast<size_t>(Param::PerfDepth); // everything but the last field
		kAudioOps.setFields(&d, row.data(), sent);
		CHECK(d.targetForTest().perfDepth == 1.0f);
		double beats = 0.0;
		drive(d, 96000 + 9600, 0.0f, &beats);
		REQUIRE(d.statusForTest() == Status::Playing);
		drive(d, 24000, 1.0f, &beats);
		checkPushNear(d.slicePushForTest(), waypointPush(east, kCfg2));
		CHECK_FALSE(d.slicePushForTest() == MacroPush {});
	}
}

TEST_CASE("GRAIN draws its cloud from the circle too — the second road") {
	// **THIS DEVICE HAS PAID FOUR TIMES FOR A FIX THAT REACHED ONE ROAD OF TWO.** Grain does not step
	// slices; it draws a plan per grain in `grainPlanFor`, which is a second call site with its own
	// cache. A puck wired only into `advanceSlice` would leave Grain playing the knobs for ever, and
	// every slice-mode case in this file would stay green.
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = performBase(Mode::Grain);
	s.setTarget(cfg);
	const auto planWith = [&](const Settings& from, int32_t grain) {
		const Coordinate a = splitCoordinate(from.zoneA);
		const Coordinate b = splitCoordinate(from.zoneB);
		const Coordinate mc { 0.0f, 0.0f }; // no knob configuration since 2026-09-19
		const MacroPush push = performPushOf(from, performWeightsOf(from.perfX, from.perfY), from.perfDepth);
		PlanCache cold;
		return planFor(a.position, b.position, mc.position, push, grain,
		               offsetsFor(a, b, mc, push, from.gamma, 0, false), &cold);
	};
	Settings east = cfg;
	puckOn(east, kCfg2);
	size_t puckMatters = 0;
	for (int32_t g = 0; g < 64; ++g) {
		CAPTURE(g);
		CHECK(s.grainPlanForTest(g) == planWith(cfg, g));
	}
	s.setTarget(east);
	for (int32_t g = 0; g < 64; ++g) {
		CAPTURE(g);
		CHECK(s.grainPlanForTest(g) == planWith(east, g));
		if (!(planWith(east, g) == planWith(cfg, g))) {
			++puckMatters;
		}
	}
	// AND THE PUCK DECIDED SOMETHING IN THIS WINDOW, or the sixty-four lines above would pass for a
	// Grain path that never looked at it.
	CAPTURE(puckMatters);
	CHECK(puckMatters > 16u);
}

TEST_CASE("the S waypoint reaches the sound in an arrangement this fixture cannot build") {
	// **THE CONTROL THIS CASE STOOD IN FOR — `Macro S` — IS DELETED (2026-09-19).** It was the one perform
	// control still on `test_fx_devices.cpp`'s dead list, and the entry recorded exactly why: that sweep
	// moves ONE control away from the default row, so the row that puts the puck south also has Zone B at
	// 0, inside the stride deadzone where the plan is the same at every slice index. There the S
	// waypoint's push was real and nearly all of it swallowed, and the one live term was short by about
	// 11% — the draw flipped at a waypoint macro of 1.113 and the control stopped at 1.
	//
	// **THE CASE SURVIVES BECAUSE THE WAYPOINT DOES, WEARING ITS CONFIGURATION INSTEAD.** What it asserts
	// is unchanged in shape: the S waypoint reaches the sound, in an arrangement with Zone B off its
	// default so the plan varies by slice. The quantity moved is now `Cfg S`, which is all a waypoint has.
	Settings south = performBase();
	puckOn(south, kCfg3); // the BOTTOM-RIGHT rim point since the 45° rotation (perform.h)

	// THE VALUE FIRST: the device's own push at the south rim IS the S waypoint's, which is the claim a
	// render cannot carry.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(south);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);
	drive(s, 24000, 1.0f, &beats);
	checkPushNear(s.slicePushForTest(), waypointPush(south, kCfg3));
	CHECK_FALSE(waypointPush(south, kCfg3) == MacroPush {});

	// AND THEN THAT MOVING `Cfg S` ALONE MOVES THE PLANS THE DEVICE DRAWS. Two devices driven by the
	// same transport through the same frames, so they stand at the same slice of the same bar.
	const auto planAt = [&](float cfgS) {
		Settings cfg = south;
		cfg.cornerCfg[kCfg3] = cfgS;
		Sklitter d;
		d.prepare(kRate, 4096);
		d.setTransportClock(at120bpm());
		d.setTarget(cfg);
		double at = 0.0;
		drive(d, 96000 + 9600 + 24000, 0.0f, &at);
		REQUIRE(d.statusForTest() == Status::Playing);
		return std::pair { d.planForTest(), d.sliceIndexForTest() };
	};
	const auto [quiet, quietSlice] = planAt(0.0f);
	const auto [loud, loudSlice] = planAt(1.0f);
	CHECK(quietSlice == loudSlice); // the same slice, or the comparison is between two different plans
	CHECK_FALSE(quiet == loud);
}

TEST_CASE("the ONE-AHEAD PEEK is drawn from the circle too — the third road out of advanceSlice") {
	// **FOUND BY A KNOWN-BAD INSIDE THE COMMIT THAT FIXED THE OTHER ROADS.** Reverting the peek's push
	// argument alone — `planFor(..., target_.macro, nextGrainIndex, ...)` — left every other perform case
	// GREEN. That is the "a fix reached one road of two" shape this device has paid for four times, and
	// the peek is the worst place for it: its plan decides `nextConsecutive_`, which decides whether the
	// join to the next slice fades. A peek drawn from the wrong push is not a wrong note, it is a click at
	// a boundary — the least testable symptom this device has, which is exactly why it gets a seam and a
	// value assertion rather than a listen.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings east = performBase();
	puckOn(east, kCfg2); // the E waypoint, where the circle's push is not the knobs'
	s.setTarget(east);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);

	size_t checked = 0;
	size_t centreWouldDiffer = 0;
	for (int n = 0; n < 60; ++n) {
		drive(s, 3000, 1.0f, &beats);
		// **ONLY WHERE THE PEEK DID NOT CROSS A BAR, ASKED OF THE DEVICE RATHER THAN INFERRED.** On a wrap
		// `advanceSlice` hands the peek a DIFFERENT `barIndex` and this restatement has only the current
		// one. Comparing the two indices would work here and is FALSE IN REPEAT, where the peek's index is
		// the repeat counter — see `nextWrapsForTest`, which exists so no case has to know that.
		if (s.nextWrapsForTest()) {
			continue;
		}
		const int32_t bar = s.barIndexForTest() % (east.seqBars > 0 ? east.seqBars : 1);
		CAPTURE(n);
		CAPTURE(s.sliceIndexForTest());
		CAPTURE(s.nextSliceIndexForTest());
		// THE VALUE: the peek's plan IS the plan the circle's push implies at that index.
		// UNDER THE REPEAT HOLD the peek predicts the plan the next slot will PLAY, which is the one its hold
		// began with — walked from the bar's first step, as the device's own hold is.
		CHECK(s.nextSlicePlanForTest()
		      == heldPlanFromPush(s, east, s.slicePushForTest(), s.nextSliceIndexForTest(), bar));
		++checked;
		// AND A CENTRED PUCK — the ZERO push, where the knobs-only push used to be — WOULD HAVE DRAWN A
		// DIFFERENT ONE; without this the assertion above passes for a peek that never looked at the puck.
		if (!(heldPlanFromPush(s, east, restPush(), s.nextSliceIndexForTest(), bar) == s.nextSlicePlanForTest())) {
			++centreWouldDiffer;
		}
	}
	CAPTURE(checked);
	CAPTURE(centreWouldDiffer);
	// FLOORS, so a run that never reached a non-wrapping boundary cannot pass by examining nothing.
	// **`checked` COUNTS READINGS OF THE SEAM, NOT DISTINCT PEEKS** — the loop reads every 3000 frames and
	// `nextPlan_` only moves when `advanceSlice` runs, so the same peek is read more than once. That is
	// deliberate and harmless: the guarantee needs one true reading, and the floor is here to catch a run
	// that reached NO usable boundary at all rather than to count how many the device drew.
	CHECK(checked >= 10u);
	CHECK(centreWouldDiffer >= checked / 2u);
}

TEST_CASE("the Send Override knob reaches the plan the device is playing") {
	// **THE ARITHMETIC IS PINNED IN `test_fx_sklitter_modes.cpp`; THIS IS THE WIRING** — that the knob
	// the host writes arrives in `Settings`, reaches `offsetsFor`, and lands on the probability the
	// PLAYING slice was drawn against. `advanceSlice` builds the offsets for the slice path and
	// `grainPlanFor` builds a second set for Grain; a change that reaches one of them and not the other
	// is the second-road defect this file records paying for four times.
	//
	// **AND THE WITNESS IS AT AN INTERMEDIATE SETTING, WHICH IS NOT THE OBVIOUS CHOICE.** A case that
	// drove the knob to its ends and checked "always" and "never" passes with the override deleted
	// whenever phi's own probability happens to sit at an end — and worse, says nothing about whether
	// the knob is CONNECTED, only about where the probability landed. The half-strength assertion below
	// is the one that fails if the value never arrives.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());

	Settings cfg = looper(Mode::Shuffle, 0.5f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.75f; // above the stride deadzone, and where phi's delay probability is intermediate
	cfg.sendOverride = 0.5f;
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);
	drive(s, 24000, 1.0f, &beats);

	// **THE REFERENCE IS COMPUTED FOR THE SLICE THE DEVICE IS ACTUALLY ON**, through `planFromPush`,
	// which takes no override — so it is phi's own probability at this instant, and the expectation
	// below is an independent restatement of the intended path rather than a second call to the
	// function under test.
	const int32_t bars = cfg.seqBars > 0 ? cfg.seqBars : 1;
	const SlicePlan phiAlone =
	    planFromPush(cfg, s.slicePushForTest(), s.sliceIndexForTest(), s.barIndexForTest() % bars, false);
	REQUIRE(phiAlone.delayProb > 0.0f); // not vacuous: there is somewhere to move it from, both ways
	REQUIRE(phiAlone.delayProb < 1.0f);

	CHECK(s.planForTest().delayProb
	      == doctest::Approx(phiAlone.delayProb + 0.5f * (1.0f - phiAlone.delayProb)));
	// AND IT IS A DIFFERENT NUMBER FROM THE ONE PHI DREW, which is what says the line above is not
	// agreeing with itself.
	CHECK(s.planForTest().delayProb != doctest::Approx(phiAlone.delayProb));

	SUBCASE("and at -100% no slice is sent, which is a deterministic claim rather than a statistical one") {
		// A probability of exactly 0 makes `Context::toU32Threshold` return 0 and `evalBoolFast` compare
		// `hash < 0`, which nothing satisfies — so this is not "rarely", it is never, and the sweep
		// below is a check on the wiring rather than an estimate of a rate.
		Settings never = cfg;
		never.sendOverride = -1.0f;
		s.setTarget(never);
		int slices = 0;
		int sent = 0;
		int32_t lastIndex = -1;
		for (int i = 0; i < 40; ++i) {
			drive(s, 4800, 2.0f, &beats);
			if (s.sliceIndexForTest() != lastIndex) {
				lastIndex = s.sliceIndexForTest();
				++slices;
				if (s.planForTest().shouldDelay) {
					++sent;
				}
				CHECK(s.planForTest().delayProb == 0.0f);
			}
		}
		CHECK(slices >= 8); // the loop actually advanced, or the zero below is about nothing
		CHECK(sent == 0);
	}

	SUBCASE("and at +100% the probability is exactly one") {
		Settings always = cfg;
		always.sendOverride = 1.0f;
		s.setTarget(always);
		drive(s, 24000, 3.0f, &beats);
		CHECK(s.planForTest().delayProb == 1.0f);
		// **`shouldDelay` IS NOT ASSERTED HERE.** `toU32Threshold(1.0f)` is 0xFFFFFFFF against a `<`
		// comparison, so the one hash value 0xFFFFFFFF still declines — one slice in 2^32. The
		// probability is the claim the knob makes; `slice_plan.h` records the asymmetry and why the
		// draw is left alone.
	}

	SUBCASE("and at 0 the device draws exactly what it drew before this knob existed") {
		// THE IDENTITY, THROUGH THE WHOLE DEVICE AND AS AN EXACT FLOAT. `planFromPush` passes no
		// override at all, so this compares the playing plan against the arithmetic of a build that has
		// none — which is the claim "default 0 changes nothing" in the only form that can fail.
		Settings rest = cfg;
		rest.sendOverride = 0.0f;
		s.setTarget(rest);
		drive(s, 24000, 4.0f, &beats);
		const SlicePlan want = planFromPush(rest, s.slicePushForTest(), s.sliceIndexForTest(),
		                                    s.barIndexForTest() % bars, false);
		CHECK(s.planForTest().delayProb == want.delayProb);
		CHECK(s.planForTest() == want); // the whole plan, so nothing else moved either
	}
}

TEST_CASE("the Send Override is AUDIBLE, and the echo's quarter-bar latency is why one device sweep cannot hear it") {
	// **THIS CASE EXISTS BECAUSE `test_fx_devices.cpp`'s SILENT-CONTROL SWEEP REPORTS THIS KNOB DEAD**,
	// and that list requires every entry to be MEASURED rather than reasoned, with the control asserted
	// by value somewhere the device can actually be heard. Both halves are here.
	//
	// **THE CAUSE IS THE PROBE'S WINDOW, AND IT IS ARITHMETIC.** The send is an ECHO: `advanceSlice`
	// sets `delayTime_ = sliceFrames_ / 4`, a quarter of the captured BAR, and the delayed copy is the
	// only thing the send ever adds to the output. That sweep drives `96000 + 9600` frames at 120 BPM —
	// one bar of capture (a looper plays nothing until its buffer is full) plus a tenth of a bar — so it
	// renders 9,600 frames of audio while the first echo is still 24,000 frames from coming back.
	// MEASURED here: the first output frame that differs between the two ends of the knob arrives 24,000
	// frames after the send opens, which is 2.5x the sweep's entire window. Same shape as `Seq Length`'s
	// entry on that list — a proof that the instrument cannot speak, not a guess that it did not.
	//
	// **A SINE, NOT THE RAMP `drive` FEEDS, AND THAT COST AN HOUR.** The anti-click machinery holds a
	// grain muted until its first ZERO CROSSING, and a monotonically increasing ramp never has one — so
	// with `drive`'s signal the grain fed into the send is EXACTLY ZERO for the first 114,000 play
	// frames (measured by instrumenting the send), the echo is a delayed copy of silence, and the two
	// ends of the knob render identically for a bar and a half. That is a property of the test signal,
	// not of the control, and it is exactly the shape of a venue that cannot express the outcome.
	const auto renderFor = [](float override, int playFrames) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		cfg.zoneA = 0.35f;
		// **THE SECOND GATE, WHICH IS NOT THIS KNOB'S.** `advanceSlice` sends only when
		// `shouldDelay && delaySendBits > 0`, and `delaySendBits` comes from Zone B's TIMBRAL BANK — a
		// different phi quantity that the override deliberately does not touch (it overrides the
		// probability only). MEASURED over slices 0..31 at Zone A 0.35: at Zone B 0.75 those bits are zero
		// for all 32, so the send is off there however certain the draw is; at 0.35 they are nonzero
		// for all 32. An audibility case has to stand where the other gate is open, or it measures
		// that gate instead.
		cfg.zoneB = 0.35f;
		cfg.sendOverride = override;
		s.setTarget(cfg);
		double beats = 0.0;
		driveSine(s, 96000, &beats); // the capture, identical in both runs
		std::vector<float> out;
		constexpr int kChunk = 2400;
		std::vector<float> l(kChunk);
		std::vector<float> r(kChunk);
		for (int done = 0; done < playFrames; done += kChunk) {
			const int n = std::min(kChunk, playFrames - done);
			for (int i = 0; i < n; ++i) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * (96000 + done + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), n);
			beats += static_cast<double>(n) / (kRate * 0.5);
			out.insert(out.end(), l.begin(), l.begin() + n);
		}
		return out;
	};

	// THE SWEEP'S OWN WINDOW: a tenth of a bar of playback. Identical at the two ends of the knob,
	// which is what that fixture measured and is the evidence for its entry.
	const std::vector<float> shortNever = renderFor(-1.0f, 9600);
	const std::vector<float> shortAlways = renderFor(1.0f, 9600);
	REQUIRE(shortNever.size() == shortAlways.size());
	CHECK(shortNever == shortAlways);
	// NOT VACUOUS: the device is making sound in that window, so "identical" is a statement about the
	// send rather than about two silences.
	CHECK(std::any_of(shortNever.begin(), shortNever.end(), [](float v) { return v != 0.0f; }));

	// **AND PAST THE ECHO'S RETURN THEY DIFFER**, which is the half that says the control is live
	// rather than merely quiet. A quarter bar is 24,000 frames; 72,000 gives the echo room to come
	// back and be heard.
	const std::vector<float> longNever = renderFor(-1.0f, 72000);
	const std::vector<float> longAlways = renderFor(1.0f, 72000);
	REQUIRE(longNever.size() == longAlways.size());
	CHECK(longNever != longAlways);

	// **AND THE FIRST DIFFERING FRAME IS PAST A QUARTER BAR**, which attributes the silence above to
	// the echo's latency rather than to a short probe being unlucky. A bound rather than an index:
	// which slice opens the send is phi's business, so the echo can start later than the earliest it
	// could and never sooner.
	size_t firstDifference = longNever.size();
	for (size_t i = 0; i < longNever.size(); ++i) {
		if (longNever[i] != longAlways[i]) {
			firstDifference = i;
			break;
		}
	}
	CAPTURE(firstDifference);
	CHECK(firstDifference >= 24000u);
	CHECK(firstDifference < longNever.size());
	CHECK(firstDifference > shortNever.size()); // past the sweep's window, said in the sweep's own units
}

//--------------------------------------------------------------------------------------------------//
// INVESTIGATION (backlog GZ): WHAT THE ECHO LINE'S LAG ACTUALLY IS.                                   //
//--------------------------------------------------------------------------------------------------//
//
// `advanceSlice` latches `delayTime_ = sliceFrames_ / 4` — a quarter BAR, since `sliceFrames_` is the
// bar in every looper mode and the slot is `schedule_.sliceLength` — and the render reads at
// `writePos() + length() - delayTime_`. That reads as a TIME and it is not one: `Ring::write` is the
// only thing that moves `writePos_`, its one call site is inside the echo block, and that block is
// gated on `delaySend_ > 0 && delayTime_ > 0`. So the lag is counted in WRITES, and a write happens
// only while a sending slice is playing. Whether that matters is a question about MAGNITUDE, which
// reading cannot answer (CLAUDE.md) — hence the numbers, and hence `delayWritePosForTest`.
//
// **DRIVEN ONE FRAME AT A TIME** so a write can be attributed to the frame that made it; a chunked
// run can only say "somewhere in these 64". The cross-check at the bottom shows the two agree on the
// COUNT (384000 against 384000), which is what makes the frame attribution trustworthy rather than
// merely fine-grained.
//
// **MEASURED 2026-09-20, sixteen bars of Shuffle at 120 BPM / 48 kHz, Zone A 0.35, Zone B 0.35,
// 16 slices of 6000 frames in a 96000-frame bar, `delayTime_` = 24000 = a quarter bar = 0.5 s.**
// `duty` is the share of played frames on which the line was written at all:
//
//   duty 1.000  lag min/mean/max  24000 / 24000  / 24000   longest freeze      0
//   duty 0.750                    24000 / 31957  / 60000                   36000
//   duty 0.594                    24000 / 40334  / 84000                   36000
//   duty 0.500                    24000 / 48400  / 102000                  42000
//   duty 0.375                    24000 / 66002  / 126000                  90000
//   duty 0.250                    24000 / 97074  / 168000                  90000
//   duty 0.000  (sendOverride <= 0 in this arrangement: no send in sixteen bars)  768000
//
// So the mean real lag is the quarter bar DIVIDED BY THE DUTY (24000/0.25 = 96000 against 97074
// measured) and the worst case reaches 168000 frames -- 3.5 s, seven times the nominal. The MINIMUM
// is exactly 24000 in every row, because a frame deep inside a run of sending slices sees a line that
// has been advancing all along; the inflation is entirely at the START of each run.
//
// **AND IT IS THE REFERENCE'S OWN BEHAVIOUR, checked against the snapshot rather than inferred from
// this port's comments.** `fx/stutterer.cpp:1022` latches `delayActive` on
// `grain.shouldDelay && grain.delaySendBits > 0`, and `delayWritePos` is advanced at `:1611` INSIDE
// `if (delayActive)` -- so the reference's line freezes between sending grains in exactly this way,
// reads at `(delayWritePos + kDelayBufferSize - delayTime)` (`:1601`), writes before mixing, and feeds
// back at a half (`delayL >> 1`, `:1609`). Same mechanism, same gate, same freeze.
//
// **KNOWN-BAD, so the `movedWhileSilent == 0` assertion is not vacuous and the counterfactual is a
// measurement rather than a model:** moving `delay_.write` OUT of the gate so the line runs free made
// every row report duty 1.000, `freezeMax` 0, and lag min/mean/max 24000/24000/24000 -- the quarter
// bar at every send density -- and turned `movedWhileSilent` from 0 into 768000 on the rows that send
// nothing. RED, so self-proving (CLAUDE.md); the source was restored and the suite is 72/72 after.
TEST_CASE("the echo line freezes between sending slices and its real lag is not the quarter bar") {
	// Zone B at 0.35 is where `delaySendBits` is nonzero for every slice (measured, and cited by the
	// Send Override case above), so this sweep moves the DRAW and not the other gate. `sendOverride`
	// is the one control that reaches `delayProb` after the push and after the clamp.
	struct Row {
		float over;
		int32_t slices;
		size_t slot;
		size_t bar;
		size_t delayTime;
		size_t writes;
		int frames;
		int longestFreeze;
		double meanLag;
		long long minLag;
		long long maxLag;
		size_t n;
		size_t overNominal;
		size_t frozenWhileSilent;
		size_t movedWhileSilent;
	};
	std::vector<Row> rows;
	constexpr int kPlay = 768000; // sixteen bars at 120 BPM, so the delayTime warm-up is a small share

	for (const float ovr : { -1.0f, 0.0f, 0.3f, 0.5f, 0.65f, 0.8f, 0.9f, 1.0f }) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		cfg.zoneA = 0.35f;
		cfg.zoneB = 0.35f;
		cfg.sendOverride = ovr;
		s.setTarget(cfg);
		double beats = 0.0;
		driveSine(s, 96000, &beats); // capture, then the loop plays

		const size_t len = s.delayLengthForTest();
		REQUIRE(len > 1u);
		size_t prev = s.delayWritePosForTest();
		std::vector<int> frameOfWrite;
		frameOfWrite.reserve(static_cast<size_t>(kPlay));
		std::vector<long long> lags;
		int freeze = 0;
		int longestFreeze = 0;
		size_t frozenWhileSilent = 0;
		size_t movedWhileSilent = 0;
		size_t delayTimeSeen = 0;
		for (int t = 0; t < kPlay; ++t) {
			const size_t dtBefore = s.delayTimeForTest();
			const float sendBefore = s.delaySendForTest();
			float lbuf = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * (96000 + t) / kRate));
			float rbuf = lbuf;
			s.setTransportClock(at120bpm(beats));
			s.processStereo(&lbuf, &rbuf, 1);
			beats += 1.0 / (kRate * 0.5);
			const size_t dtAfter = s.delayTimeForTest();
			const float sendAfter = s.delaySendForTest();
			const size_t now = s.delayWritePosForTest();
			const size_t step = (now + len - prev) % len;
			prev = now;
			REQUIRE(step <= 1u); // one frame can write at most once, or the mapping below is wrong
			// **(B), AS A VALUE.** A frame that was NOT sending at either end of it cannot have been
			// sending during it, so the head must not have moved — once the return's fade-out is over
			// (backlog JC: the line runs on for the ramp's ~1.5 ms after a send stops, and freezes when
			// the return reaches zero).
			if (sendBefore == 0.0f && sendAfter == 0.0f && s.delayEchoGainForTest() == 0.0f) {
				(step == 0u) ? ++frozenWhileSilent : ++movedWhileSilent;
			}
			if (step == 1u) {
				if (dtBefore == dtAfter && dtBefore > 0 && frameOfWrite.size() >= dtBefore) {
					// This frame read `writePos - delayTime`, which was last written by the write
					// `delayTime` events ago. Its REAL age is the distance between the two frames.
					lags.push_back(t - frameOfWrite[frameOfWrite.size() - dtBefore]);
				}
				frameOfWrite.push_back(t);
				longestFreeze = std::max(longestFreeze, freeze);
				freeze = 0;
				// **ONLY A NONZERO ONE.** The last write of a run can land on the frame a slice
				// boundary zeroes the latch, and reporting THAT as the nominal lag made a first draft
				// print `dt=0` for rows whose every measured lag was 24000.
				if (dtAfter > 0) {
					delayTimeSeen = dtAfter;
				}
			}
			else {
				++freeze;
			}
		}
		longestFreeze = std::max(longestFreeze, freeze);
		long long sum = 0;
		long long lo = lags.empty() ? 0 : lags.front();
		long long hi = 0;
		size_t over = 0;
		for (const long long v : lags) {
			sum += v;
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			if (delayTimeSeen > 0 && v > static_cast<long long>(delayTimeSeen)) {
				++over;
			}
		}
		rows.push_back(Row { ovr, s.sliceCountForTest(), s.scheduleForTest().sliceLength,
		                     s.sliceFramesForTest(), delayTimeSeen, frameOfWrite.size(), kPlay,
		                     longestFreeze,
		                     lags.empty() ? 0.0 : static_cast<double>(sum) / static_cast<double>(lags.size()),
		                     lo, hi, lags.size(), over, frozenWhileSilent, movedWhileSilent });
	}

	for (const Row& r : rows) {
		MESSAGE("over=", r.over, " slices=", r.slices, " slot=", r.slot, " bar=", r.bar, " dt=",
		        r.delayTime, " writes=", r.writes, "/", r.frames, " duty=",
		        static_cast<double>(r.writes) / static_cast<double>(r.frames), " freezeMax=",
		        r.longestFreeze, " lag[min/mean/max]=", r.minLag, "/", r.meanLag, "/", r.maxLag,
		        " n=", r.n, " overNominal=", r.overNominal, " silentFrozen=", r.frozenWhileSilent,
		        " silentMoved=", r.movedWhileSilent);
	}

	// **(B) PROVED AS A VALUE, NOT INFERRED**: across every row, no frame that was non-sending at both
	// ends of itself advanced the write head.
	for (const Row& r : rows) {
		CAPTURE(r.over);
		CHECK(r.movedWhileSilent == 0u);
	}

	// **AND THE INSTRUMENT IS SHOWN CAPABLE OF BOTH ANSWERS.** At full override every frame sends, the
	// line never freezes, and the measured lag is EXACTLY the latched `delayTime_` — so a row reporting
	// more than that is reporting a real inflation rather than a noisy instrument.
	const Row& full = rows.back();
	CHECK(full.over == 1.0f);
	CHECK(full.longestFreeze == 0);
	CHECK(full.n > 0u);
	CHECK(full.minLag == static_cast<long long>(full.delayTime));
	CHECK(full.maxLag == static_cast<long long>(full.delayTime));

	// **THE CROSS-CHECK ON THE ONE-FRAME DRIVING.** Same configuration, chunked at 64; the write COUNT
	// must agree, or the frame-by-frame run is measuring its own block size.
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		cfg.zoneA = 0.35f;
		cfg.zoneB = 0.35f;
		cfg.sendOverride = 0.65f;
		s.setTarget(cfg);
		double beats = 0.0;
		driveSine(s, 96000, &beats);
		const size_t len = s.delayLengthForTest();
		size_t prev = s.delayWritePosForTest();
		size_t writes = 0;
		constexpr int kChunk = 64;
		std::vector<float> l(kChunk);
		std::vector<float> r(kChunk);
		for (int done = 0; done < kPlay; done += kChunk) {
			for (int i = 0; i < kChunk; ++i) {
				const double ph = 2.0 * 3.14159265358979 * 220.0 * (96000 + done + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), kChunk);
			beats += static_cast<double>(kChunk) / (kRate * 0.5);
			const size_t now = s.delayWritePosForTest();
			writes += (now + len - prev) % len;
			prev = now;
		}
		MESSAGE("chunked-64 writes=", writes, " against one-frame ", rows[4].writes);
		CHECK(rows[4].over == 0.65f);
		// Not bit-identical: a 64-frame block crosses slice boundaries differently. Within one block.
		CHECK(std::llabs(static_cast<long long>(writes) - static_cast<long long>(rows[4].writes)) < kChunk);
	}
}


//--------------------------------------------------------------------------------------------------//
// THE TRANSPORT LOCK — backlog GT.                                                                 //
//                                                                                                  //
// **EVERY CLAIM HERE IS ASSERTED AS A VALUE, AND THAT IS NOT A STYLE CHOICE.** CLAUDE.md's rule    //
// names this feature as its own worked example: *"the transport lock's retrigger road deleted      //
// outright"* is one of the two mutations that survived cases written specifically to catch them,   //
// because a script that opens with a note differs between two runs on its own. So the cursor the   //
// lock DERIVES is read through `lockedSliceForTest`, the cursor it INSTALLS through                //
// `sliceIndexForTest`, and the render comparisons below appear only as SECOND assertions proving   //
// the first are not vacuous.                                                                       //
//--------------------------------------------------------------------------------------------------//

namespace {

/// Drive a device with the transport under the caller's control — position, and whether it rolls.
void driveTransport(Sklitter& s, int frames, double* beats, bool playing, float seed = 1.0f) {
	constexpr int kChunk = 4800;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	for (int done = 0; done < frames; done += kChunk) {
		const int n = std::min(kChunk, frames - done);
		l.assign(static_cast<size_t>(n), 0.0f);
		r.assign(static_cast<size_t>(n), 0.0f);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = seed + static_cast<float>(done + i);
			r[static_cast<size_t>(i)] = -(seed + static_cast<float>(done + i));
		}
		s.setTransportClock(at120bpm(*beats, playing));
		s.processStereo(l.data(), r.data(), n);
		if (playing) {
			*beats += static_cast<double>(n) / (0.5 * kRate);
		}
	}
}

/// A running looper with the lock in a chosen state, and the song position it left off at.
Sklitter lockedRunner(Mode m, bool locked, double* beats, float rate = 0.25f, float pWrite = 0.0f) {
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper(m, rate);
	cfg.transportLocked = locked;
	cfg.pWrite = pWrite;
	// **THE ZONES THE pWRITE CASES USE, AND FOR THEIR REASON.** The write goes to the LINEAR bar
	// position and is DUCKED whenever that region overlaps the one being read, so a fixture at the
	// default zones can legitimately write nothing at all — which is how the first draft of the freeze
	// case below got a vacuous control that agreed with it.
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	s.setTransportClock(at120bpm(*beats));
	s.setTarget(cfg);
	driveTransport(s, 96000 + 9600, beats, true);
	return s;
}

/// REFILL A BLOCK FROM ITS SOURCE, element-wise into a vector that is ALREADY the right size.
///
/// This exists because `l = input;` — a plain vector copy — is rejected by GCC 13 under `-Werror`
/// (CI run 35681722337, ubuntu-latest, 2026-09-22): `-Wstringop-overflow` reports the copy's memcpy
/// "writing between 5 and 19200 bytes into a region of size 0", a known false positive in that
/// compiler's tracking of `operator=`'s reallocate-then-copy path. GCC 14 here says nothing. Copying
/// into a vector that is not reallocated never enters that path, which is why this is a helper and
/// not a pragma: the warning class stays on for the code it can see correctly.
void refill(std::vector<float>& block, const std::vector<float>& source) {
	REQUIRE(block.size() == source.size());
	std::copy(source.begin(), source.end(), block.begin());
}

/// The whole ring as the device holds it — what "the buffer froze" is a claim about.
std::vector<float> ringSnapshot(const Sklitter& s) {
	const auto& ring = s.ringForTest();
	std::vector<float> out;
	out.reserve(ring.length() * 2u);
	for (size_t i = 0; i < ring.length(); ++i) {
		float a = 0.0f;
		float b = 0.0f;
		ring.readAt(i, &a, &b);
		out.push_back(a);
		out.push_back(b);
	}
	return out;
}

} // namespace

TEST_CASE("backlog GT: TRANSPORT LOCKED takes the read cursor off the SONG, and the number is the bar's own grid") {
	// **THE REFERENCE IS COMPUTED FROM THE PLATFORM'S CONTRACT, NOT FROM THE DEVICE.** At 120 BPM a
	// beat is 0.5 s and a bar is four of them, so at 48 kHz a bar is 96000 frames — and Rate 0.25 cuts
	// it into four slices of 24000. Every expectation below is that arithmetic written out, which is
	// what makes this a check on the derivation rather than a restatement of it.
	double beats = 0.0;
	Sklitter probe = lockedRunner(Mode::Shuffle, true, &beats);
	REQUIRE(probe.statusForTest() == Status::Playing);
	REQUIRE(probe.sliceFramesForTest() == 96000u);
	REQUIRE(probe.sliceCountForTest() == 4);

	struct Probe {
		double songBeats;
		double phase;
		int32_t slice;
	};
	// A downbeat, a beat past it, five eighths through, and the last sliver of the bar.
	const Probe probes[] { { 8.0, 0.0, 0 },    { 9.0, 0.25, 1 },  { 10.5, 0.625, 2 },
	                       { 11.75, 0.9375, 3 }, { 12.0, 0.0, 0 }, { 6.0, 0.5, 2 } };
	for (const Probe& p : probes) {
		CAPTURE(p.songBeats);
		probe.setTransportClock(at120bpm(p.songBeats));
		CHECK(probe.lockedBarPhaseForTest(0.0) == doctest::Approx(p.phase).epsilon(1e-9));
		CHECK(probe.lockedSliceForTest(0.0) == p.slice);
	}

	// **AND THE OFFSET ARGUMENT IS FRAMES, WHICH IS WHAT LETS A SLOT END ASK ABOUT ITS OWN FRAME**
	// rather than about the block it happens to sit in. A quarter of a bar past a downbeat is one
	// slice on, by construction: 24000 frames.
	probe.setTransportClock(at120bpm(8.0));
	CHECK(probe.lockedSliceForTest(24000.0) == 1);
	CHECK(probe.lockedSliceForTest(72000.0) == 3);
	// Past the bar it wraps rather than running off the end — the engage point is wrapped against the
	// sequence length by design, to keep it in phase.
	CHECK(probe.lockedSliceForTest(96000.0) == 0);
	CHECK(probe.lockedSliceForTest(120000.0) == 1);
}

TEST_CASE("backlog GT: a host with NO beat position cannot be locked to, and the device says so rather than guessing") {
	// **THE ONE QUESTION THE BACKLOG LEFT OPEN, ANSWERED AS A CHOICE RATHER THAN A CONSEQUENCE.** A
	// host that publishes no playhead leaves the free-running counter as the only cursor there is, and
	// `prewarm` already branches the same way for the same reason. Nothing about the code forced it —
	// the alternative (refuse to play at all without a position) is equally implementable and worse.
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Shuffle, true, &beats);
	TransportClock noPosition = TransportClock::fromSecondsPerBeat(0.5);
	noPosition.playing = true; // rolling, but silent about where
	REQUIRE(!noPosition.havePositionBeats());
	s.setTransportClock(noPosition);
	CHECK(s.lockedBarPhaseForTest(0.0) < 0.0);
	CHECK(s.lockedSliceForTest(0.0) == -1);

	// It keeps PLAYING — the fallback is the counter, not silence. The cursor advances across a slice
	// boundary, which at four slices of 24000 frames it must do inside one 48000-frame run.
	const int32_t before = s.sliceIndexForTest();
	std::vector<float> l(4800, 0.25f);
	std::vector<float> r(4800, -0.25f);
	for (int i = 0; i < 10; ++i) {
		s.setTransportClock(noPosition);
		s.processStereo(l.data(), r.data(), 4800);
	}
	CHECK(s.statusForTest() == Status::Playing);
	CHECK(s.sliceIndexForTest() != before);
}

TEST_CASE("backlog GT: the device stops with the transport, the buffer FREEZES, and the cursor is parked where it stood") {
	// **pWRITE AT FULL, SO THE BUFFER WOULD BE WRITTEN IF ANYTHING WERE RUNNING.** A freeze asserted
	// against a device that was not writing anyway is a vacuous freeze — this is the difference check
	// that proves the value check below is not.
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Shuffle, true, &beats, 0.25f, 1.0f);
	REQUIRE(s.statusForTest() == Status::Playing);

	const std::vector<float> ringBefore = ringSnapshot(s);
	const int32_t sliceBefore = s.sliceIndexForTest();
	const size_t withinBefore = s.positionInSliceForTest();
	const int32_t barBefore = s.barIndexForTest();
	REQUIRE(!ringBefore.empty());

	// THE HOST STOPS. The position stays where it is stopped, which is what a stopped host reports.
	std::vector<float> l(4800);
	std::vector<float> r(4800);
	for (int i = 0; i < 4800; ++i) {
		l[static_cast<size_t>(i)] = 0.5f + 0.001f * static_cast<float>(i);
		r[static_cast<size_t>(i)] = -l[static_cast<size_t>(i)];
	}
	const std::vector<float> inputL = l;
	const std::vector<float> inputR = r;

	// **THE STOP'S 5 ms RAMP FIRST (backlog GT, the stop-click fix).** The device renders exactly
	// `wrapFade_` frames more while the wet and the writers ramp out, then holds. So the heads are parked
	// where the RAMP left them, one ramp further on than at the stop instant, and the freeze below is
	// asserted from there. The ramp itself is bounded by "a LOCKED STOP takes the wet to the dry over 5 ms".
	const size_t ramp = s.wrapFadeForTest();
	REQUIRE(ramp > 0u);
	REQUIRE(withinBefore + ramp < s.scheduleForTest().sliceLength); // the ramp stays inside this slice
	{
		std::vector<float> rl(inputL.begin(), inputL.begin() + static_cast<std::ptrdiff_t>(ramp));
		std::vector<float> rr(inputR.begin(), inputR.begin() + static_cast<std::ptrdiff_t>(ramp));
		s.setTransportClock(at120bpm(beats, false));
		s.processStereo(rl.data(), rr.data(), static_cast<int>(ramp));
	}
	CHECK(s.parkedForTest());
	CHECK(s.parkLeftForTest() == 0u);
	CHECK(s.positionInSliceForTest() == withinBefore + ramp); // exactly the ramp, no further
	const std::vector<float> ringAtPark = ringSnapshot(s);

	// **A WHOLE BAR STOPPED, NOT A FEW BLOCKS.** pWrite is a per-SLICE draw that is ducked on overlap,
	// so a span shorter than the bar can write nothing even with the lock off — and a control that
	// writes nothing agrees with a freeze for the wrong reason.
	constexpr int kStoppedBlocks = 20; // 20 x 4800 = 96000 frames = one bar at 120 BPM / 48 kHz
	for (int block = 0; block < kStoppedBlocks; ++block) {
		l = inputL;
		r = inputR;
		s.setTransportClock(at120bpm(beats, false));
		s.processStereo(l.data(), r.data(), 4800);
		// **BIT FOR BIT, NOT "CLOSE".** The device writes no wet at all while it is held, so the blend
		// takes `renderChunk`'s `m == 0` branch and never touches the sample — which is the same
		// guarantee an empty chain gets, and the reason that branch exists.
		CHECK(std::memcmp(l.data(), inputL.data(), sizeof(float) * l.size()) == 0);
		CHECK(std::memcmp(r.data(), inputR.data(), sizeof(float) * r.size()) == 0);
	}

	// THE VALUES, which is what the claim is about.
	CHECK(ringSnapshot(s) == ringAtPark);
	CHECK(s.sliceIndexForTest() == sliceBefore);
	CHECK(s.positionInSliceForTest() == withinBefore + ramp);
	CHECK(s.barIndexForTest() == barBefore);
	CHECK(s.statusForTest() == Status::Playing); // held, not released
	CHECK(s.rejoinPendingForTest());

	// **THE CONTROL, AND IT IS THE HALF THAT MAKES THE ABOVE MEAN ANYTHING.** The same script with the
	// lock OFF keeps playing through the stop and writes the buffer while it does.
	double freeBeats = 0.0;
	Sklitter free = lockedRunner(Mode::Shuffle, false, &freeBeats, 0.25f, 1.0f);
	const std::vector<float> freeRingBefore = ringSnapshot(free);
	bool freeOutputMoved = false;
	for (int block = 0; block < kStoppedBlocks; ++block) {
		l = inputL;
		r = inputR;
		free.setTransportClock(at120bpm(freeBeats, false));
		free.processStereo(l.data(), r.data(), 4800);
		freeOutputMoved = freeOutputMoved
		                  || std::memcmp(l.data(), inputL.data(), sizeof(float) * l.size()) != 0;
	}
	CHECK(freeOutputMoved);
	CHECK(ringSnapshot(free) != freeRingBefore);
}

TEST_CASE("backlog GT: on a restart the cursor is the one the SONG names — forwards, backwards, and before the engage point") {
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Shuffle, true, &beats, 0.25f);
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.sliceCountForTest() == 4);

	std::vector<float> l(64, 0.125f);
	std::vector<float> r(64, -0.125f);

	// **THREE RESTARTS, AND THE THIRD IS THE ONE THAT SETTLED THE QUESTION.** The point is to wrap the
	// engage point against the sequence length to keep it in phase, not to trigger at the exact same
	// time in the track — so a restart BEFORE the gesture is a position like any other, and the
	// modulo is what makes it one.
	struct Restart {
		double songBeats;
		int32_t slice; ///< computed from the bar's grid: floor((beats mod 4) / 4 * 4)
	};
	const Restart restarts[] { { 40.0, 0 }, { 26.5, 2 }, { 1.0, 1 }, { 0.75, 0 }, { 3.5, 3 } };
	for (const Restart& rs : restarts) {
		CAPTURE(rs.songBeats);
		// Stop first — the seek is what a RESUME does, and a resume needs a stop in front of it.
		s.setTransportClock(at120bpm(beats, false));
		s.processStereo(l.data(), r.data(), 64);
		REQUIRE(s.rejoinPendingForTest());

		beats = rs.songBeats;
		s.setTransportClock(at120bpm(beats));
		// **THE DERIVATION IS READ BEFORE IT IS APPLIED**, so a red names the number that was wrong
		// rather than the render that noticed.
		REQUIRE(s.lockedSliceForTest(0.0) == rs.slice);
		s.processStereo(l.data(), r.data(), 1);
		// THE SLOT PLAYING COVERS THE SONG'S SLICE — it IS that slice, or (ribbon truth, decision (a)) the long
		// grain the walk has running over it, entered at its offset.
		CHECK(s.sliceIndexForTest() <= rs.slice);
		CHECK(rs.slice < s.sliceIndexForTest() + std::max<int32_t>(1, s.scheduleForTest().grainLength));
		CHECK(s.barIndexForTest() >= 0);
		CHECK(s.barIndexForTest() < feathers::fx::sklitter::kBarIndexWrap);
		CHECK(!s.rejoinPendingForTest());
		beats += 1.0 / (0.5 * kRate);
	}

	// **THE CONTROL, AND IT IS NOT THE ONE THIS CASE WAS WRITTEN WITH (backlog HE).** It used to be the
	// SAME SCRIPT WITH THE LOCK OFF, on the reading that the unlocked cursor ignores the song. A refinement
	// of the design retired that: the device can be both free-running and transport-locked, resetting to
	// the proper phase when the transport restarts, so the unlocked device follows those jumps too —
	// the case above would now pass against a device that had no lock at all, which is exactly the
	// vacuity the old control existed to prevent.
	//
	// **WHAT STILL SEPARATES THE TWO IS THE STOP, so the control removes it and keeps everything
	// else.** Same five positions, same device, one difference: the transport never stops. The
	// unlocked setting owes no rejoin then, and its free-running counter is wherever it has walked to.
	// The 1-in-4 coincidence argument is unchanged and is why five landings are counted rather than one.
	double freeBeats = 0.0;
	Sklitter free = lockedRunner(Mode::Shuffle, false, &freeBeats, 0.25f);
	int agreed = 0;
	for (const Restart& rs : restarts) {
		// **NO STOP — the jump arrives under a transport that never stopped rolling.** (The slot-end
		// derivation that WOULD follow it is the locked setting's, and has its own case.)
		freeBeats = rs.songBeats;
		free.setTransportClock(at120bpm(freeBeats));
		free.processStereo(l.data(), r.data(), 64);
		if (free.sliceIndexForTest() == rs.slice) {
			++agreed;
		}
		freeBeats += 64.0 / (0.5 * kRate);
	}
	CHECK(agreed < static_cast<int>(std::size(restarts)));
}

TEST_CASE("backlog GT: the stop and the freeze reach GRAIN too, which is the cell the honesty matrix cannot see") {
	// **WHY THIS CASE EXISTS BY NAME.** `MATRIX: every control in every mode` reports `Transport
	// Locked` DEAD in Grain, and the report is true of the fixture rather than of the device: that
	// matrix drives a transport that never stops, and the stop is the whole of what this control does
	// in a mode with no slice grid to re-phase. The matrix carries the exception and points here.
	//
	// **GRAIN GETS THE FIRST TWO THIRDS OF THE FEATURE AND NOT THE THIRD**, and that is disclosed
	// rather than papered over: `playGrain` scatters over the bar instead of walking it and never calls
	// `advanceSlice`, so there is no `sliceIndex_` for the derivation to install. The stop and the
	// freeze are real there; the phase derivation is not.
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Grain, true, &beats, 0.5f, 1.0f);
	REQUIRE(s.statusForTest() == Status::Playing);
	const std::vector<float> ringBefore = ringSnapshot(s);
	REQUIRE(!ringBefore.empty());

	std::vector<float> input(4800);
	for (int i = 0; i < 4800; ++i) {
		input[static_cast<size_t>(i)] = 0.4f + 0.0002f * static_cast<float>(i);
	}
	std::vector<float> l(input.size());
	std::vector<float> r(input.size());
	refill(l, input);
	refill(r, input);
	// THE STOP'S 5 ms RAMP FIRST (backlog GT): the freeze holds from where the ramp leaves the device.
	const size_t ramp = s.wrapFadeForTest();
	REQUIRE(ramp > 0u);
	{
		std::vector<float> rl(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(ramp));
		std::vector<float> rr(rl);
		s.setTransportClock(at120bpm(beats, false));
		s.processStereo(rl.data(), rr.data(), static_cast<int>(ramp));
	}
	CHECK(s.parkedForTest());
	CHECK(s.parkLeftForTest() == 0u);
	const std::vector<float> ringAtPark = ringSnapshot(s);
	constexpr int kStoppedBlocks = 20; // one bar, for the reason the freeze case states
	for (int block = 0; block < kStoppedBlocks; ++block) {
		refill(l, input);
		refill(r, input);
		s.setTransportClock(at120bpm(beats, false));
		s.processStereo(l.data(), r.data(), 4800);
		CHECK(std::memcmp(l.data(), input.data(), sizeof(float) * l.size()) == 0);
	}
	CHECK(ringSnapshot(s) == ringAtPark);

	// The control: Grain with the lock off plays on through the stop.
	double freeBeats = 0.0;
	Sklitter free = lockedRunner(Mode::Grain, false, &freeBeats, 0.5f, 1.0f);
	bool moved = false;
	for (int block = 0; block < kStoppedBlocks; ++block) {
		refill(l, input);
		refill(r, input);
		free.setTransportClock(at120bpm(freeBeats, false));
		free.processStereo(l.data(), r.data(), 4800);
		moved = moved || std::memcmp(l.data(), input.data(), sizeof(float) * l.size()) != 0;
	}
	CHECK(moved);
}

TEST_CASE("backlog GT: MEASURE — what the lock changes in an UNDISTURBED pass is the ENGAGE POINT's phase") {
	// **WRITTEN BECAUSE A COMMENT CLAIMED THE OPPOSITE AND THE MATRIX CONTRADICTED IT.** The first
	// draft of `playLooper`'s slot-end derivation said the lock was INERT until the host disturbed the
	// transport — `scheduleFor` tiles the bar exactly, so a slot end under a steadily-advancing
	// transport falls on the base-grid boundary the derivation names. `MATRIX: every control in every
	// mode` then reported `Transport Locked` as MOVING in all five slice modes at all three
	// arrangements, with a transport that never stops. CLAUDE.md: rule on the shape, take the number
	// from the instrument. This case is the instrument.
	//
	// **AND THE ANSWER IS THAT THE ADVANCE IS INERT AND THE START IS NOT.** `sliceIndex_ = 0` at
	// engage (`sklitter.cpp`'s gesture branch) puts the cursor at the top of the bar whatever the song
	// is doing — so a gesture pressed away from a downbeat plays the captured bar from its start while
	// the music is somewhere else. Under the lock the start is the song's phase instead, which is the
	// design read literally: a gesture triggered on the downbeat resumes with the same buffer as if it
	// had been re-triggered on the downbeat. The re-phasing of the start is a FEATURE, and it is
	// also the whole of what an undisturbed pass hears.
	const auto firstPlayingSlice = [](bool locked, double startBeats, int32_t* count) {
		Sklitter s;
		s.prepare(kRate, 4096);
		Settings cfg = looper(Mode::Shuffle, 0.25f);
		cfg.transportLocked = locked;
		s.setTransportClock(at120bpm(startBeats));
		s.setTarget(cfg);
		double beats = startBeats;
		std::vector<float> l(480);
		std::vector<float> r(480);
		for (int block = 0; block < 600; ++block) {
			for (int i = 0; i < 480; ++i) {
				l[static_cast<size_t>(i)] = 1.0f + static_cast<float>(block * 480 + i);
				r[static_cast<size_t>(i)] = -l[static_cast<size_t>(i)];
			}
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), 480);
			beats += 480.0 / (0.5 * kRate);
			if (s.statusForTest() == Status::Playing) {
				*count = s.sliceCountForTest();
				return s.sliceIndexForTest();
			}
		}
		*count = 0;
		return int32_t { -1 };
	};

	// **STARTED A BEAT AND A HALF OFF A DOWNBEAT, SO THE TWO ANSWERS CANNOT COINCIDE.** A fixture that
	// began on a bar line would give 0 from both roads and report agreement it had not tested.
	int32_t freeCount = 0;
	int32_t lockedCount = 0;
	const int32_t freeSlice = firstPlayingSlice(false, 1.5, &freeCount);
	const int32_t lockedSlice = firstPlayingSlice(true, 1.5, &lockedCount);
	REQUIRE(freeCount == 4);
	REQUIRE(lockedCount == 4);
	// **SUPERSEDED FOR THE UNLOCKED SETTING BY THE 2026-09-22 DECISION (backlog GU).** This used to
	// read `freeSlice == 0` — the free-running cursor at the top of the bar, by construction. GU then
	// phased the write head on every press, the read cursor left at 0 sat a song phase behind it, and
	// the decision is that a press starts BOTH heads at the song's phase in both settings. So at the press
	// the two settings now AGREE, and they differ only at a stop (GT/HE); the engage-point difference
	// this case was written to separate no longer exists.
	CHECK(freeSlice == lockedSlice);
	// And it is the song's slice, not the top of the bar — which is what makes the agreement non-vacuous.
	CHECK(lockedSlice != 0);
	CHECK(lockedSlice >= 0);
	CHECK(lockedSlice < 4);

	// **AND THE ADVANCE ITSELF: ONE BAR ON IS THE SAME SLICE AGAIN.** The derivation is a function of
	// the song position modulo the bar, so a whole bar later it must name the slice it named before —
	// which is the property that makes a DAW loop over the same bars play the same slices every pass,
	// and the one a free-running counter cannot promise.
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Shuffle, true, &beats, 0.25f);
	REQUIRE(s.sliceCountForTest() == 4);
	for (double at : { 0.0, 1.3, 2.75, 3.9 }) {
		CAPTURE(at);
		s.setTransportClock(at120bpm(40.0 + at));
		const int32_t here = s.lockedSliceForTest(0.0);
		s.setTransportClock(at120bpm(44.0 + at)); // exactly one bar later
		CHECK(s.lockedSliceForTest(0.0) == here);
		s.setTransportClock(at120bpm(40.0 + at - 12.0)); // and three bars earlier
		CHECK(s.lockedSliceForTest(0.0) == here);
	}
}

TEST_CASE("backlog GT: a hair either side of a slot end — on an unbroken transport the lock is the song's slice, and inaudible") {
	// **WHY THIS CASE EXISTS.** `MATRIX: every control in every mode` read `Transport Locked` MOVING in
	// the five slice modes on a transport that never stops, after the GU decision had made the two
	// settings the same everywhere but at a stop. Diagnosed by value on 2026-09-22: `lockedCursor`
	// TRUNCATED, and a slot end asks about the frame that sits exactly on a base-grid boundary — so a
	// song position a few ulps low (the matrix accumulates its position by `+= 0.04`, as a host does:
	// 8.4799999999999951 at the first failure) made 12000 into 11999.9999…, named the slice that had
	// just FINISHED, and ran the lock a whole slice late. MEASURED on the matrix's own fixture before
	// the fix: 13 to 177 of 300 played blocks differing between the settings in every one of 15 cells,
	// peak difference -1.5 dBFS; every block top nudged one ulp HIGH, none did.
	//
	// **FOUR TRANSPORTS, AND THE EXACT ONE IS NOT THE SAFE ONE.** Block tops exact; accumulated by
	// `+= 0.04` exactly as the matrix does; and a nanobeat (1e-9 beat, 2.4e-5 of a frame) low and high.
	// With EXACT tops Repeat still failed at the third arrangement, because the device's own
	// `songPositionBeats + atFrame / framesPerBeat` rounds low inside the block. **NOT ONE ULP**, which
	// is what this case first used: at a block top near beat 8 one ulp is absorbed by that same
	// in-block addition rounding to nearest, so a single-ulp nudge failed only the one cell the exact
	// transport already failed — the known-bad showed it, 5 red assertions where 15 cells were owed.
	//
	// **WHAT THE KNOWN-BAD (truncation restored) REDDENS, AND WHAT IT DOES NOT.** The ACCUMULATED
	// transport in all 15 cells and the EXACT one in Repeat at the third arrangement: 16 of 60, 649
	// assertions, ON one slice behind the integer reference every time. **NOT the two nanobeat
	// transports**, and that is a property of the defect rather than a blind spot: a CONSTANT low offset
	// is self-consistent under truncation, because the press's seek truncates too and puts the whole
	// cursor one frame late in BOTH settings, so every slot end then lands a frame past its boundary and
	// derives the right slice. What breaks the truncating build is an error that VARIES from block to
	// block, which is what an accumulating host position is. The nanobeat pair stays as the two sides
	// a rounding fix has to survive.
	//
	// **THE REFERENCE IS INTEGER ARITHMETIC, NOT THE DEVICE'S DERIVATION.** `lockedSliceForTest` is
	// the function under repair, so asking it would agree with a truncating build by construction. The
	// song frame here is a counted integer: block * 960 + offset, modulo the captured bar.
	namespace sc = feathers::fx::sklitter;
	constexpr int kBlock = 960;
	constexpr int kBlocks = 400;
	const Mode sliceModes[] { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch };
	// The matrix's three arrangements: Zone A, Zone B, Perf X, Perf Y, Rate.
	const float arrangements[3][5] { { 0.4f, 0.4f, 0.5f, 0.0f, 0.5f },
		                             { 0.63f, 0.57f, 0.0f, 0.3f, 0.47f },
		                             { 0.35f, 0.35f, 0.0f, 0.0f, 0.62f } };
	int slotEndsChecked = 0;
	for (const Mode m : sliceModes) {
		for (const auto& arr : arrangements) {
			// 0 exact, 1 accumulated, 2 a nanobeat low, 3 a nanobeat high.
			for (const int transport : { 0, 1, 2, 3 }) {
				const std::string modeName = sc::kModeNames[static_cast<int>(m)];
				CAPTURE(modeName);
				CAPTURE(arr[0]);
				CAPTURE(transport);
				Settings cfg = looper(m, 0.5f);
				withMacro(cfg, 0.5f, 0.4f);
				cfg.engage = true;
				sc::Row off = sc::rowOf(cfg);
				off[static_cast<size_t>(sc::Param::ZoneA)] = arr[0];
				off[static_cast<size_t>(sc::Param::ZoneB)] = arr[1];
				off[static_cast<size_t>(sc::Param::PerfX)] = arr[2];
				off[static_cast<size_t>(sc::Param::PerfY)] = arr[3];
				off[static_cast<size_t>(sc::Param::Rate)] = arr[4];
				sc::Row on = off;
				on[static_cast<size_t>(sc::Param::TransportLocked)] = 1.0f;
				Sklitter a;
				Sklitter b;
				for (Sklitter* s : { &a, &b }) {
					s->prepare(kRate, 4096);
					s->setTransportClock(at120bpm());
				}
				a.setTarget(sc::stateOf(off));
				b.setTarget(sc::stateOf(on));
				std::vector<float> la(kBlock);
				std::vector<float> ra(kBlock);
				std::vector<float> lb(kBlock);
				std::vector<float> rb(kBlock);
				size_t n = 0;
				bool wasPlaying = false;
				size_t differing = 0;
				double accumulated = 0.0;
				for (int block = 0; block < kBlocks; ++block) {
					for (int i = 0; i < kBlock; ++i, ++n) {
						const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
						const double ph2 = 2.0 * 3.14159265358979 * 331.0 * static_cast<double>(n) / kRate;
						la[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph) + 0.3 * std::sin(ph2));
						ra[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph2) + 0.3 * std::sin(ph));
					}
					lb = la;
					rb = ra;
					const double exact = static_cast<double>(block * kBlock) / (0.5 * kRate);
					const double beats = block == 0      ? 0.0
					                     : transport == 1 ? accumulated
					                     : transport == 2 ? exact - 1e-9
					                     : transport == 3 ? exact + 1e-9
					                                      : exact;
					accumulated += static_cast<double>(kBlock) / (kRate * 0.5);
					a.setTransportClock(at120bpm(beats));
					b.setTransportClock(at120bpm(beats));
					a.processStereo(la.data(), ra.data(), kBlock);
					b.processStereo(lb.data(), rb.data(), kBlock);
					for (int i = 0; i < kBlock; ++i) {
						if (la[static_cast<size_t>(i)] != lb[static_cast<size_t>(i)]
						    || ra[static_cast<size_t>(i)] != rb[static_cast<size_t>(i)]) {
							++differing;
						}
					}
					const bool playing = b.statusForTest() == Status::Playing;
					// **A SLOT BEGAN INSIDE THIS BLOCK** when the cursor's offset is less than the block —
					// and only on a block that was already playing, so the press's seek (which installs
					// an offset of its own) is not mistaken for one.
					if (wasPlaying && playing && b.positionInSliceForTest() < static_cast<size_t>(kBlock)) {
						const auto bar = static_cast<long long>(b.sliceFramesForTest());
						const int32_t count = b.sliceCountForTest();
						REQUIRE(bar > 0);
						REQUIRE(count > 0);
						const long long base = bar / count;
						const long long songFrame = static_cast<long long>(block) * kBlock + kBlock
						                            - static_cast<long long>(b.positionInSliceForTest());
						const auto want =
						    static_cast<int32_t>(std::min<long long>((songFrame % bar) / base, count - 1));
						CHECK(b.sliceIndexForTest() == want);
						CHECK(a.sliceIndexForTest() == b.sliceIndexForTest());
						++slotEndsChecked;
					}
					wasPlaying = playing;
				}
				CHECK(differing == 0);
			}
		}
	}
	// NOT VACUOUS: slot ends were actually reached and read, in every mode and on every transport.
	CHECK(slotEndsChecked > 60 * 20);
}


TEST_CASE("backlog GT: a loop or a scrub the host makes WITHOUT stopping is followed, at the slice boundaries") {
	// **THE ROAD THIS COVERS IS THE SLOT-END DERIVATION, AND NOTHING ELSE REACHED IT.** The resume seek
	// answers a stop; the gesture's own resync answers a press. A DAW that LOOPS never stops and never
	// re-presses — it simply hands the device a song position that has gone backwards — so without this
	// case the `sliceIndex_ += schedule_.grainLength` replacement in `playLooper` could be deleted with
	// every other GT case still green. Found by asking what a known-bad on that line would kill.
	//
	// **AND THE OBVIOUS ASSERTION IS WRONG, WHICH IS WORTH WRITING DOWN BECAUSE IT WAS WRITTEN FIRST.**
	// "The installed cursor equals the derived one" is NOT an invariant of this device at an arbitrary
	// instant: `scheduleFor` gives a long grain a slot of `baseSliceLength * grainLength`, so a grain
	// deliberately HOLDS one slice index while the song walks past several base slices. The first draft
	// asserted it mid-slot and read 1 against a derivation of 3 — a correct device and a wrong claim.
	// **The derivation is installed AT SLOT ENDS, so a slot end is where it has to be read.**
	const auto agreementsAfterJump = [](bool locked, double jumpTo, int* boundaries) {
		double beats = 0.0;
		// **32 SLICES, SO THE BOUNDARIES ARE FREQUENT.** At 120 BPM / 48 kHz a bar is 96000 frames, so
		// Rate 1.0 makes the base slot 3000 — many slot ends inside the run, rather than one or none.
		Sklitter s = lockedRunner(Mode::Shuffle, locked, &beats, 1.0f);
		REQUIRE(s.statusForTest() == Status::Playing);
		REQUIRE(s.sliceCountForTest() == 32);

		beats = jumpTo; // THE JUMP: the position moves and the transport never stops rolling.
		constexpr int kStep = 120; // 2.5 ms, so a 3000-frame slot holds twenty-five of them
		std::vector<float> l(kStep, 0.3f);
		std::vector<float> r(kStep, -0.3f);
		int agreed = 0;
		int seen = 0;
		size_t previousWithin = s.positionInSliceForTest();
		for (int block = 0; block < 500; ++block) {
			s.setTransportClock(at120bpm(beats));
			// **THE TWO ENDS OF THE BLOCK, BECAUSE A BOUNDARY MAY FALL INSIDE ONE.** The cursor is
			// installed at the exact frame the slot ends on; this samples at 2.5 ms, so the derivation
			// is asked about both ends of the window the boundary is known to be in. Widening the
			// window is the alternative and it is the one that would hide a real disagreement.
			const int32_t atStart = s.lockedSliceForTest(0.0);
			const int32_t atEnd = s.lockedSliceForTest(static_cast<double>(kStep));
			s.processStereo(l.data(), r.data(), kStep);
			beats += static_cast<double>(kStep) / (0.5 * kRate);
			const size_t within = s.positionInSliceForTest();
			if (within < previousWithin) { // a slot ended inside this block
				++seen;
				// the slot now playing COVERS the song's slice: it is that slice, or the long grain the walk has
				// running over it, joined at its offset (ribbon truth, decision (a))
				const int32_t now = s.sliceIndexForTest();
				const int32_t end = now + std::max<int32_t>(1, s.scheduleForTest().grainLength);
				if ((now <= atStart && atStart < end) || (now <= atEnd && atEnd < end)) {
					++agreed;
				}
			}
			previousWithin = within;
		}
		*boundaries = seen;
		return agreed;
	};

	// **THREE JUMPS: forwards, backwards past the engage point, and a long way on.** One jump could
	// agree by coincidence — the counter has to be somewhere, and at 32 slices it is right 1/32 of the
	// time; three independent landings make that a 1-in-32000 coincidence rather than a test.
	for (const double jump : { 9.0, 0.5, 37.25 }) {
		CAPTURE(jump);
		int boundaries = 0;
		const int agreed = agreementsAfterJump(true, jump, &boundaries);
		MESSAGE("locked: " << agreed << " of " << boundaries << " slot ends on the song's slice");
		// The fixture must actually produce boundaries to read. **EIGHT, NOT ELEVEN, SINCE THE REPEAT HOLD
		// (2026-09-23)**: a held long grain spans its steps slot after slot, so the same window holds fewer
		// slot ends — MEASURED 11, 10 and 11 across the three jumps where it was 11 each before.
		REQUIRE(boundaries >= 8);
		CHECK(agreed == boundaries);
	}

	// **THE CONTROL, AND IT IS WHAT MAKES THE ABOVE MEAN ANYTHING.** The same three jumps with the lock
	// off: the cursor is wherever the counter has walked to, so it agrees with the song only by
	// accident.
	// **COUNTED ACROSS THE THREE RATHER THAN ASSERTED ON EACH, AND THE FIXTURE TAUGHT ME WHY.** A
	// free-running counter jumped to 0.5 agreed at 11 of 11 boundaries — it had simply been left in
	// phase, because the device engaged near that position and nothing had moved it since. MEASURED:
	// 0 of 11, 11 of 11, 0 of 11. One landing is not evidence about a counter; a counter that follows
	// ALL THREE is the thing only a derivation can do.
	int fullyAgreeing = 0;
	for (const double jump : { 9.0, 0.5, 37.25 }) {
		CAPTURE(jump);
		int boundaries = 0;
		const int agreed = agreementsAfterJump(false, jump, &boundaries);
		MESSAGE("free: " << agreed << " of " << boundaries << " slot ends on the song's slice");
		REQUIRE(boundaries >= 8); // see the locked loop above for why eight
		if (agreed == boundaries) {
			++fullyAgreeing;
		}
	}
	CHECK(fullyAgreeing < 3);
}

//--------------------------------------------------------------------------------------------------//
// THE UNLOCKED REJOIN — backlog HE. The risk it carries is a CLICK, not a wrong number.             //
//                                                                                                  //
// Locked, a resume is a START: the device was silent through the gap, so the seek lands in silence //
// and `engageFadePos_` brings the wet up from the dry exactly as it does at a press. UNLOCKED the  //
// device was SOUNDING, so the same seek interrupts audio the player is hearing. No value assertion //
// can see that — it is a discontinuity in a stream that is correct on both sides of it — so it is  //
// measured as a step, in the click sweep's own units.                                              //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **311.13 Hz, AND THE FREQUENCY IS THE INSTRUMENT.** The click sweep's 220 Hz probe CANNOT SEE THIS
/// SEAM: at 48 kHz a bar is 96,000 frames, so 220 Hz completes exactly 440 cycles in it and 110 in
/// every slice — a jump to any other slice lands on the identical phase and leaves no step at all.
/// MEASURED with it: the step across the restart read 0.0288, the sine's own, in the locked run, the
/// unlocked one, and the run with no restart in it. An instrument blind to the condition, reporting
/// three clean readings. 311.13 Hz gives 155.565 cycles per slice, so no two slices are in phase.
constexpr double kRejoinFreq = 311.13;
/// The probe's own largest step per sample. Everything below is quoted against it, because the sine
/// already moves this much and a device cannot be blamed for its input.
const double kRejoinInputStep = 2.0 * 3.14159265358979 * kRejoinFreq / kRate;

struct RejoinRun {
	double worstSettled = 0.0; ///< largest step over 48,000 undisturbed frames, before anything happens
	double worstAtSeam = 0.0;  ///< largest step in the 960 frames the restart lands in
	double stepAcross = 0.0;   ///< the ONE step from the last sample before the restart to the first after
	double peak = 0.0;         ///< that the fixture is hearing the device at all
	bool duckSeen = false;     ///< whether a rejoin's wet-out was ever armed during the run
	int32_t sliceAfter = -1;
	int32_t sliceSong = -1;
};

/// **A SINE FROM THE FIRST SAMPLE, INCLUDING THE CAPTURE, AND THAT IS NOT A DETAIL.** The GT fixtures
/// above fill the ring with a RAMP, which has exactly one zero crossing — and the anti-click attack is
/// a GATE that keeps a grain silent until its first crossing, so a ramp-filled buffer plays SILENCE
/// for ever. MEASURED while writing this: peak output 0.0 over 9,600 frames with a ring peaking at
/// 95,760. A click measured on that fixture would have been a measurement of nothing.
///
/// **AND THE SEAM WINDOW IS 960 FRAMES, NOT THE WHOLE TAIL.** At `Min Env` 0 the device makes steps of
/// its own at every slice boundary — the click sweep reports 35x the input in every slice mode — so a
/// maximum taken over a second of audio is saturated by them and returns the same number whatever the
/// seam did. 960 frames covers the seek and the 440-frame engage fade behind it.
RejoinRun rejoinProbe(bool locked, bool stopTransport, double resumeBeats, float rate) {
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper(Mode::Shuffle, rate);
	cfg.transportLocked = locked;
	double beats = 0.0;
	s.setTransportClock(at120bpm(beats));
	s.setTarget(cfg);

	constexpr int kChunk = 480;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	long long frame = 0;
	double prev = 0.0;
	bool havePrev = false;
	bool settling = false;
	int seamLeft = 0; ///< frames of seam window still to score; the first of them is `stepAcross`
	bool seamFirst = false;
	RejoinRun out;

	const auto run = [&](int blocks, bool playing) {
		for (int b = 0; b < blocks; ++b) {
			for (int i = 0; i < kChunk; ++i) {
				const double ph =
				    2.0 * 3.14159265358979 * kRejoinFreq * static_cast<double>(frame + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			s.setTransportClock(at120bpm(beats, playing));
			s.processStereo(l.data(), r.data(), kChunk);
			out.duckSeen = out.duckSeen || s.rejoinDuckingForTest();
			for (int i = 0; i < kChunk; ++i) {
				const double cur = l[static_cast<size_t>(i)];
				const double step = havePrev ? std::abs(cur - prev) : 0.0;
				if (settling || seamLeft > 0) {
					out.peak = std::max(out.peak, std::abs(cur));
				}
				if (settling) {
					out.worstSettled = std::max(out.worstSettled, step);
				}
				if (seamLeft > 0) {
					out.worstAtSeam = std::max(out.worstAtSeam, step);
					if (seamFirst) {
						out.stepAcross = step;
						seamFirst = false;
					}
					--seamLeft;
				}
				prev = cur;
				havePrev = true;
			}
			frame += kChunk;
			if (playing) {
				beats += static_cast<double>(kChunk) / (0.5 * kRate);
			}
		}
	};

	run(240, true); // 115,200 frames: the ring fills and the gesture settles
	settling = true;
	run(100, true); // SETTLED: 48,000 frames of the run's own noise floor
	settling = false;
	if (stopTransport) { // THE GAP. Locked this is silence; unlocked the device plays on through it.
		run(100, false);
	}
	beats = resumeBeats; // THE RESTART, a long way from where the cursor was left
	out.sliceSong = s.lockedSliceForTest(0.0);
	seamLeft = 960;
	seamFirst = true;
	run(60, true);
	out.sliceAfter = s.sliceIndexForTest();
	return out;
}

} // namespace

TEST_CASE("backlog HE: the UNLOCKED setting slips past the stop and REJOINS in phase when the transport resumes") {
	// **THE HALF OF THE SPEC THE FIRST BUILD LEFT OUT.** The device can be both free-running and
	// transport-locked, resetting to the proper phase when the transport restarts; with the lock off the
	// effect slips past a stopped transport and rejoins when it resumes, which is a feature in the right
	// mode. So the toggle gates whether audio keeps being PRODUCED while
	// the transport is parked, and the re-phasing belongs to both settings.
	//
	// **32 SLICES, SO A COINCIDENCE IS 1 IN 32.** Three jumps — forwards, backwards past the engage
	// point, and a long way on — make an accidental agreement a 1-in-32,000 event rather than a test
	// that a free-running counter could pass by standing still.
	double beats = 0.0;
	Sklitter s = lockedRunner(Mode::Shuffle, false, &beats, 1.0f);
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.sliceFramesForTest() == 96000u);
	REQUIRE(s.sliceCountForTest() == 32);

	constexpr int kBlock = 512; // longer than the 440-frame duck, so one block completes it
	std::vector<float> input(kBlock);
	for (int i = 0; i < kBlock; ++i) {
		input[static_cast<size_t>(i)] = 0.3f + 0.0005f * static_cast<float>(i);
	}
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);

	for (const double jumpTo : { 9.25, 0.5, 37.375 }) {
		CAPTURE(jumpTo);
		// **THE STOP — AND THE DEVICE MUST BE HEARD TO SLIP PAST IT**, or the rejoin below is being
		// asserted about a device that had stopped anyway and the case proves nothing about the toggle.
		bool soundedThroughTheGap = false;
		int32_t previousSlice = s.sliceIndexForTest();
		size_t previousWithin = s.positionInSliceForTest();
		int cursorMovedInTheGap = 0;
		for (int block = 0; block < 10; ++block) {
			refill(l, input);
			refill(r, input);
			s.setTransportClock(at120bpm(beats, false));
			s.processStereo(l.data(), r.data(), kBlock);
			soundedThroughTheGap = soundedThroughTheGap
			                       || std::memcmp(l.data(), input.data(), sizeof(float) * l.size()) != 0;
			if (s.sliceIndexForTest() != previousSlice
			    || s.positionInSliceForTest() != previousWithin) {
				++cursorMovedInTheGap;
			}
			previousSlice = s.sliceIndexForTest();
			previousWithin = s.positionInSliceForTest();
		}
		CHECK(soundedThroughTheGap);
		// **AND IT FREE-RAN WHILE IT DID — the half of the setting a rejoin case can silently destroy.**
		// The debt is armed on every stopped block; if it were PAID on every stopped block the cursor
		// would seek to the same FROZEN song position on each of them and stand still, which still
		// sounds and still passes the check above. A free-running stutter over a parked DAW is
		// exactly this assertion.
		//
		// **READ AS (slice, offset) AND NOT AS THE SLICE ALONE, because the slice alone cannot see it.**
		// MEASURED: ten 512-frame blocks against a 3,000-frame base slice and a grain that holds its
		// index across several of them — the free-running cursor crossed NO boundary in the whole gap,
		// so a slice-only check read "did not move" about a device that was moving perfectly well. The
		// offset advances 512 per block and a pinned cursor repeats the same pair every block.
		CHECK(cursorMovedInTheGap == 10);
		REQUIRE(s.rejoinPendingForTest());

		// THE RESTART, a long way from where the free-running cursor was left.
		beats = jumpTo;
		refill(l, input);
		refill(r, input);
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kBlock);
		// **THE DUCK IS ARMED BEFORE THE SEEK AND IS READ AS A VALUE.** The wet is on its way to the dry
		// for 440 frames; the seek lands at the top of the next block. Nothing about the OUTPUT says
		// which of those two things is happening, which is why this is a flag and not a buffer.
		CHECK(s.rejoinDuckingForTest());
		CHECK(s.rejoinPendingForTest());
		beats += static_cast<double>(kBlock) / (0.5 * kRate);

		// **THE REFERENCE IS THE PLATFORM'S ARITHMETIC, NOT THE DEVICE'S.** At 120 BPM a bar is four
		// beats and at 48 kHz that is 96,000 frames; Rate 1.0 cuts it into 32 base slices of 3,000. The
		// expected index is that division written out, so a red names a number rather than a render.
		const double bars = beats / 4.0;
		const double phase = bars - std::floor(bars);
		const auto framesIn = static_cast<int32_t>(phase * 96000.0);
		const int32_t expected = std::min(framesIn / 3000, 31);

		s.setTransportClock(at120bpm(beats));
		REQUIRE(s.lockedSliceForTest(0.0) == expected); // the derivation agrees with the arithmetic
		s.processStereo(l.data(), r.data(), 1);
		// ...and the cursor is where the derivation says: that slice, or the long grain the walk has running over
		// it, joined at its offset (ribbon truth, decision (a))
		CHECK(s.sliceIndexForTest() <= expected);
		CHECK(expected < s.sliceIndexForTest() + std::max<int32_t>(1, s.scheduleForTest().grainLength));
		CHECK(!s.rejoinDuckingForTest());
		CHECK(!s.rejoinPendingForTest());
		// **AND THE WET COMES BACK RATHER THAN STAYING AT THE DRY.** The duck holds the device at the
		// dry until the seek; `seekToTransport` restarts the engage fade, and a rejoin that forgot to
		// would leave the effect silently off for the rest of the gesture.
		CHECK(s.engageFadingForTest());
		beats += 1.0 / (0.5 * kRate);
	}
}

TEST_CASE("backlog HE: MEASURE — the unlocked REJOIN's step at the seam, against the locked resume") {
	MESSAGE("the probe moves at most " << kRejoinInputStep
	                                   << " per sample — anything above that is the DEVICE");
	// **RATE 0.25 — FOUR SLICES OF 24,000 FRAMES IS WHERE A SEEK HAS THE MOST GRAIN TO INTERRUPT**, so
	// it is where the step is worst, which is the arrangement a bound belongs on.
	const RejoinRun lockedRun = rejoinProbe(true, true, 37.25, 0.25f);
	const RejoinRun freeRun = rejoinProbe(false, true, 37.25, 0.25f);
	const RejoinRun noStop = rejoinProbe(false, false, 37.25, 0.25f);
	REQUIRE(lockedRun.peak > 0.1); // the fixture is hearing the device, not silence
	REQUIRE(freeRun.peak > 0.1);
	REQUIRE(noStop.peak > 0.1);

	MESSAGE("LOCKED resume (shipped): floor " << lockedRun.worstSettled << ", seam "
	                                          << lockedRun.worstAtSeam << ", ACROSS "
	                                          << lockedRun.stepAcross << " ("
	                                          << (lockedRun.stepAcross / kRejoinInputStep)
	                                          << "x input), slice " << lockedRun.sliceAfter << " vs song "
	                                          << lockedRun.sliceSong);
	MESSAGE("UNLOCKED rejoin:         floor " << freeRun.worstSettled << ", seam "
	                                          << freeRun.worstAtSeam << ", ACROSS "
	                                          << freeRun.stepAcross << " ("
	                                          << (freeRun.stepAcross / kRejoinInputStep)
	                                          << "x input), slice " << freeRun.sliceAfter << " vs song "
	                                          << freeRun.sliceSong);
	MESSAGE("UNLOCKED, no stop:       floor " << noStop.worstSettled << ", seam " << noStop.worstAtSeam
	                                          << ", ACROSS " << noStop.stepAcross << " ("
	                                          << (noStop.stepAcross / kRejoinInputStep)
	                                          << "x input), slice " << noStop.sliceAfter << " vs song "
	                                          << noStop.sliceSong);

	// **THE FLOOR IS LARGE AND IT IS NOT THIS FEATURE'S.** At `Min Env` 0 the device's own slice
	// boundaries step by ~0.5-0.6 in this arrangement — the click sweep's standing report of 35x the
	// input in every slice mode — which is why the number that answers the question is the ONE step
	// across the restart and not a maximum over the tail. A maximum there is saturated by edges the
	// device makes on purpose and returns the same figure whatever the seam does.
	//
	// **WITHOUT THE DUCK THIS READ 0.511, 12.5x THE INPUT'S OWN STEP.** That is the measurement the
	// wet-out ramp in `mixAt` exists for; it is quoted in that function's header and here because a
	// bound with no known-bad number beside it is a bound nobody can calibrate.
	CHECK(freeRun.stepAcross < 3.0 * kRejoinInputStep);
	// **AND THE WHOLE 960-FRAME WINDOW, NOT ONLY ITS FIRST STEP — a known-bad taught me the
	// difference.** Dropping `mixAt`'s hold at the dry (reading the counter instead of the flag) leaves
	// the wet snapped back to FULL for the handful of samples between the ramp running out and the
	// seek landing at the next block top. `stepAcross` is the step at the restart instant and cannot
	// see it: MEASURED, that mutation left `stepAcross` at 0.0386 and unchanged, while the window
	// maximum went to **0.849**, 20.9x the input. The bound above was green for it.
	CHECK(freeRun.worstAtSeam < 3.0 * kRejoinInputStep);
	CHECK(lockedRun.worstAtSeam < 3.0 * kRejoinInputStep);
	// **AND AGAINST THE SHIPPED PATH RATHER THAN AGAINST AN ABSOLUTE**, because the locked resume is
	// the thing whose sound has already been accepted by ear. A rejoin that is not worse than it is not a
	// regression however the arrangement moves.
	CHECK(freeRun.stepAcross <= 2.0 * lockedRun.stepAcross);

	// **THE SECOND ASSERTION, PROVING THE FIRST IS NOT VACUOUS — AND IT IS A FLAG RATHER THAN A CURSOR
	// COMPARISON, WHICH IS WHAT THE FIRST DRAFT USED.** A seam that is quiet because nothing happened
	// at it passes every bound above. The obvious guard — "the cursor ended on the song's slice" —
	// CANNOT DO THIS JOB here, twice over: at four slices a free-running counter agrees one time in
	// four (MEASURED: it stayed GREEN with the rejoin re-gated on the lock), and at thirty-two a grain
	// legitimately HOLDS its index across several base slices, so the cursor and the derivation
	// disagree at an arbitrary instant on a perfectly correct device — the trap
	// `backlog GT: a loop or a scrub` already documents. The duck is armed by the rejoin road and by
	// nothing else, so seeing it is seeing that road run.
	CHECK(freeRun.duckSeen);
	CHECK(!lockedRun.duckSeen); // locked: nothing was sounding, so there is nothing to duck
	CHECK(!noStop.duckSeen);    // no stop, no rejoin, no duck
}

namespace {

/// What happens at the instant the probe changes something: the host stops, Freeze is pressed, or nothing.
enum class StopEvent { Stop, Freeze, None };

struct StopRun {
	double worstSettled = 0.0; ///< largest output step over 48,000 undisturbed frames, before the event
	double stepAcross = 0.0;   ///< the ONE output step from the last sample before the event to the first after
	double worstAtStop = 0.0;  ///< largest output step in the 960 frames the event lands in
	double peak = 0.0;
	std::vector<float> ringAfterRamp; ///< the ring 960 frames after the event
	std::vector<float> ringAtEnd;     ///< the ring at the end of the 48,000 frames after the event
	long long wetInRamp = 0;    ///< output samples that were NOT the dry, in the first 5 ms after the event
	long long wetAfterRamp = 0; ///< and after it
	bool parked = false;        ///< `parkedForTest` at the end
	size_t parkLeft = 0;        ///< `parkLeftForTest` at the end
};

/// **HE's PROBE, SCORED AT THE STOP INSTEAD OF THE RESTART — backlog GT.** The same 311.13 Hz sine from
/// the first sample, the same Shuffle at Rate 0.25, the same 480-frame host blocks, the same 960-frame
/// window and the same yardstick (`kRejoinInputStep`). `dryWrite` turns the continuous writer on so the
/// ring is being written at the instant writing stops, which is what (b) is a claim about.
StopRun stopProbe(bool locked, StopEvent event, float dryWrite) {
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.transportLocked = locked;
	cfg.dryWrite = dryWrite;
	double beats = 0.0;
	s.setTransportClock(at120bpm(beats));
	s.setTarget(cfg);

	constexpr int kChunk = 480;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	long long frame = 0;
	double prev = 0.0;
	bool havePrev = false;
	bool settling = false;
	int windowLeft = 0;
	bool windowFirst = false;
	long long sinceEvent = -1; ///< frames since the event, or −1 before it
	StopRun out;

	const auto run = [&](int blocks, bool playing) {
		for (int b = 0; b < blocks; ++b) {
			for (int i = 0; i < kChunk; ++i) {
				const double ph =
				    2.0 * 3.14159265358979 * kRejoinFreq * static_cast<double>(frame + i) / kRate;
				l[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
				r[static_cast<size_t>(i)] = static_cast<float>(std::sin(ph));
			}
			const std::vector<float> in = l;
			s.setTransportClock(at120bpm(beats, playing));
			s.processStereo(l.data(), r.data(), kChunk);
			for (int i = 0; i < kChunk; ++i) {
				const double cur = l[static_cast<size_t>(i)];
				const double step = havePrev ? std::abs(cur - prev) : 0.0;
				if (settling || windowLeft > 0) {
					out.peak = std::max(out.peak, std::abs(cur));
				}
				if (settling) {
					out.worstSettled = std::max(out.worstSettled, step);
				}
				if (sinceEvent >= 0) {
					// THE DRY BY BITS, NOT BY A TOLERANCE: a parked device writes no wet at all.
					const bool wet = std::memcmp(&l[static_cast<size_t>(i)], &in[static_cast<size_t>(i)],
					                             sizeof(float)) != 0;
					if (sinceEvent < static_cast<long long>(s.wrapFadeForTest())) {
						out.wetInRamp += wet ? 1 : 0;
					}
					else {
						out.wetAfterRamp += wet ? 1 : 0;
					}
					++sinceEvent;
				}
				if (windowLeft > 0) {
					out.worstAtStop = std::max(out.worstAtStop, step);
					if (windowFirst) {
						out.stepAcross = step;
						windowFirst = false;
					}
					--windowLeft;
				}
				prev = cur;
				havePrev = true;
			}
			frame += kChunk;
			if (playing) {
				beats += static_cast<double>(kChunk) / (0.5 * kRate);
			}
		}
	};

	run(240, true); // the ring fills and the gesture settles
	settling = true;
	run(100, true); // the run's own floor
	settling = false;
	windowLeft = 960;
	windowFirst = true;
	sinceEvent = 0;
	const bool playing = event != StopEvent::Stop;
	if (event == StopEvent::Freeze) {
		cfg.freeze = true;
		s.setTarget(cfg);
	}
	run(2, playing); // the 960-frame window: the ramp, if there is one, is inside it
	out.ringAfterRamp = ringSnapshot(s);
	run(98, playing);
	out.ringAtEnd = ringSnapshot(s);
	out.parked = s.parkedForTest();
	out.parkLeft = s.parkLeftForTest();
	return out;
}

} // namespace

TEST_CASE("backlog GT: a LOCKED STOP takes the wet to the dry over 5 ms — no step at the stop above the input's own, then the dry by bits") {
	// **MEASURED BEFORE THE RAMP: the step across a locked stop was 0.381, 9.3x the probe's own 0.0407**, with
	// a floor of 0.0407 over the settled run before it. The device cut from the wet to the dry in one sample.
	// The bound is HE's for the resume, on HE's probe: the stop and the resume are the two ends of one gap.
	MESSAGE("the probe moves at most " << kRejoinInputStep << " per sample");
	const StopRun lockedStop = stopProbe(true, StopEvent::Stop, 0.0f);
	REQUIRE(lockedStop.peak > 0.1); // the fixture is hearing the device, not silence
	MESSAGE("LOCKED stop: floor " << lockedStop.worstSettled << ", ACROSS " << lockedStop.stepAcross << " ("
	                              << (lockedStop.stepAcross / kRejoinInputStep) << "x input), window "
	                              << lockedStop.worstAtStop);
	CHECK(lockedStop.stepAcross < 3.0 * kRejoinInputStep);
	CHECK(lockedStop.worstAtStop < 3.0 * kRejoinInputStep);

	// THE VALUES: the ramp has run out and the device is held.
	CHECK(lockedStop.parked);
	CHECK(lockedStop.parkLeft == 0u);
	// **AND AFTER THE RAMP THE OUTPUT IS THE DRY BY BITS**, the GT promise from before the ramp, now from
	// 5 ms in. The second assertion proves the bound above is not vacuous: the ramp played WET, so there
	// was a wet to take away.
	CHECK(lockedStop.wetAfterRamp == 0);
	CHECK(lockedStop.wetInRamp > 0);
}

TEST_CASE("backlog GT: across a LOCKED STOP the buffer is byte-identical from the end of the ramp, with Dry Write at full") {
	// Dry Write at 1 writes the ring on every sample, so a buffer that did not move is a freeze and not a
	// device that was not writing anyway. The unlocked control below is the proof: the same script writes.
	const StopRun locked = stopProbe(true, StopEvent::Stop, 1.0f);
	REQUIRE(!locked.ringAtEnd.empty());
	CHECK(std::memcmp(locked.ringAtEnd.data(), locked.ringAfterRamp.data(),
	                  sizeof(float) * locked.ringAtEnd.size()) == 0);
	const StopRun free = stopProbe(false, StopEvent::Stop, 1.0f);
	CHECK(free.ringAtEnd != free.ringAfterRamp);
	CHECK(!free.parked);
}

TEST_CASE("backlog GT: the stop's ramp is the LOCK's alone — unlocked, or locked on a transport that never stops, nothing parks") {
	// **THE NEW CODE ACTS ONLY THROUGH `parked_` AND `parkLeft_`**, so a device that never sets either renders
	// the bits it rendered before the ramp existed, by construction. These are those two values, read at every
	// block of a script with a stop and a restart in it, in every looper mode, lock off; and lock on with the
	// host never stopping. (The bit identity was also measured directly, old engine against new: see backlog GT.)
	const Mode modes[] = { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch, Mode::Grain };
	for (const Mode m : modes) {
		for (const bool locked : { false, true }) {
			CAPTURE(static_cast<int>(m));
			CAPTURE(locked);
			Sklitter s;
			s.prepare(kRate, 4096);
			Settings cfg = looper(m, 0.25f);
			cfg.transportLocked = locked;
			cfg.dryWrite = 1.0f;
			double beats = 0.0;
			s.setTransportClock(at120bpm(beats));
			s.setTarget(cfg);
			std::vector<float> l(480);
			std::vector<float> r(480);
			bool everParked = false;
			for (int b = 0; b < 400; ++b) {
				// UNLOCKED: blocks 300..349 are a stop. LOCKED: the host never stops.
				const bool playing = locked || b < 300 || b >= 350;
				std::fill(l.begin(), l.end(), 0.25f);
				std::fill(r.begin(), r.end(), -0.25f);
				s.setTransportClock(at120bpm(beats, playing));
				s.processStereo(l.data(), r.data(), 480);
				everParked = everParked || s.parkedForTest() || s.parkLeftForTest() != 0u;
				if (playing) {
					beats += 480.0 / (0.5 * kRate);
				}
			}
			CHECK(!everParked);
		}
	}
}

namespace {

/// **DRIVE, AND HAND BACK THE LOUDEST SAMPLE THE DEVICE WROTE.** The peak is taken from the OUTPUT
/// buffer this helper owns, which is an independent restatement of what came out — the case never asks
/// the device what it produced and then compares that with what the device says it produced.
float driveAndPeak(Sklitter& s, int frames, float value, double* beats) {
	constexpr int kChunk = 960;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	float peak = 0.0f;
	for (int done = 0; done < frames; done += kChunk) {
		const int n = std::min(kChunk, frames - done);
		std::fill(l.begin(), l.begin() + n, value);
		std::fill(r.begin(), r.begin() + n, value);
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		for (int i = 0; i < n; ++i) {
			peak = std::max({ peak, std::abs(l[static_cast<size_t>(i)]),
			                  std::abs(r[static_cast<size_t>(i)]) });
		}
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
	return peak;
}

} // namespace

TEST_CASE("backlog HR: the level SKLITTER publishes is the sound it is MAKING, not the sound passing through it") {
	// **`Reading::OutLevel` EXISTS FOR THE PERFORM CIRCLE'S SPARKS** (the sparks are emitted
	// as a Poisson process driven by the instantaneous level), and the choice it encodes is an
	// HONESTY choice rather than an arithmetic one: it is the WET times the MIX — the term the blend
	// adds on top of the dry — and not the peak of what leaves the device.
	//
	// **THE DIFFERENCE IS ONLY VISIBLE IN THE STATES WHERE THE DEVICE IS DOING NOTHING**, which is
	// exactly why each of them is a subcase here. Every one of them has a LOUD signal going through,
	// so a build that metered the output would read near full scale on all four and would spark the
	// circle for a sound this instrument had no hand in.
	constexpr float kLoud = 0.75f;

	const auto capture = [](Sklitter& s, Settings cfg, double* beats) {
		s.prepare(kRate, 4096);
		s.setTarget(cfg);
		for (int block = 0; block < 300 && s.statusForTest() != Status::Playing; ++block) {
			driveConst(s, 960, kLoud, beats);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
	};

	SUBCASE("a BYPASSED device reads zero while a loud signal walks straight through it") {
		Sklitter s;
		s.prepare(kRate, 4096);
		Settings cfg = looper(Mode::Shuffle);
		cfg.enabled = false;
		cfg.mix = 1.0f;
		s.setTarget(cfg);
		double beats = 0.0;
		const float out = driveAndPeak(s, 48000, kLoud, &beats);
		// NOT VACUOUS: the signal really did come out, at the level it went in.
		REQUIRE(out == doctest::Approx(kLoud).epsilon(1e-4));
		CHECK(s.wetLevel() == 0.0f);
		CHECK(s.grainLevel() == 0.0f);
	}

	SUBCASE("an UNENGAGED device reads zero for the same reason — there is no wet to be loud") {
		Sklitter s;
		s.prepare(kRate, 4096);
		Settings cfg = looper(Mode::Shuffle);
		cfg.engage = false;
		cfg.mix = 1.0f;
		s.setTarget(cfg);
		double beats = 0.0;
		const float out = driveAndPeak(s, 48000, kLoud, &beats);
		REQUIRE(out == doctest::Approx(kLoud).epsilon(1e-4));
		CHECK(s.wetLevel() == 0.0f);
		CHECK(s.grainLevel() == 0.0f);
	}

	SUBCASE("and a PLAYING device at Mix 0 reads zero too — the sharpest form of the claim") {
		// **THIS IS THE SUBCASE A BUILD METERING THE OUTPUT CANNOT PASS.** The device is engaged, the
		// slicer is running, the ring is full of loud material — and none of it is reaching the output,
		// because the mix is down. What leaves is the dry, at full level. A contribution meter reads
		// exactly zero; an output meter reads 0.75.
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper(Mode::Shuffle);
		cfg.mix = 1.0f;
		capture(s, cfg, &beats);
		cfg.mix = 0.0f;
		s.setTarget(cfg);
		const float out = driveAndPeak(s, 96000, kLoud, &beats);
		REQUIRE(s.statusForTest() == Status::Playing); // still playing, so there IS a wet being muted
		REQUIRE(out == doctest::Approx(kLoud).epsilon(1e-4));
		CHECK(s.wetLevel() == 0.0f);
		CHECK(s.grainLevel() == 0.0f);
	}

	SUBCASE("while the same device at Mix 1 reads the material it is playing") {
		// The other half, without which every zero above is also what a deleted reading answers.
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper(Mode::Shuffle);
		cfg.mix = 1.0f;
		capture(s, cfg, &beats);
		float loudest = 0.0f;
		for (int i = 0; i < 100; ++i) {
			driveAndPeak(s, 960, kLoud, &beats);
			loudest = std::max(loudest, s.wetLevel());
		}
		// **AT MIX 1 THE CONTRIBUTION IS THE OUTPUT**, and the material is a constant `kLoud`, so the
		// level reaches it. It cannot EXCEED it: nothing in this arrangement has gain.
		CHECK(loudest == doctest::Approx(kLoud).epsilon(0.02));
		CHECK(loudest <= kLoud + 1e-4f);
	}

	SUBCASE("the two levels PARTITION: what took the grain lane leaves the wet, exactly") {
		// **THE WET OUTPUT AND THE GRAIN FX OUTPUT EMIT TWO COLOURS, AND THE GRAIN FX IS SUBTRACTED FROM THE
		// MASTER BEFORE THE SPARKS.** The second is the honesty decision: the
		// lane REJOINS the wet, so a wet read after the rejoin already contains it, and two colours
		// claiming to show two things while one contains the other is a picture asserting something
		// false.
		//
		// **THE PARTITION IS EXACT BY CONSTRUCTION AND THIS CASE IS WHAT SAYS SO IN NUMBERS.** The
		// renderer's own note at the divert site: *"`wetAt(l − sent) + wetAt(sent)` is `wetAt(l)`, so
		// diverting a grain moves it between the two buses without changing what the device produces in
		// total."* So with an EMPTY lane — a true pass — the total is unmoved and only the SPLIT moves,
		// which is exactly the pair of claims below.
		const auto levelsWith = [&](float sendOverride) {
			Sklitter s;
			double beats = 0.0;
			Settings cfg = looper(Mode::Shuffle);
			cfg.mix = 1.0f;
			cfg.sendOverride = sendOverride;
			capture(s, cfg, &beats);
			float wet = 0.0f;
			float grain = 0.0f;
			for (int i = 0; i < 100; ++i) {
				driveAndPeak(s, 960, kLoud, &beats);
				wet = std::max(wet, s.wetLevel());
				grain = std::max(grain, s.grainLevel());
			}
			return std::pair<float, float> { wet, grain };
		};

		// NOTHING DIVERTED: the lane is silent and the wet is the whole of it.
		const auto [wetNone, grainNone] = levelsWith(0.0f);
		CHECK(grainNone == 0.0f);
		CHECK(wetNone == doctest::Approx(kLoud).epsilon(0.02));

		// **EVERYTHING DIVERTED** — `sendOverride` at 1 takes `overriddenProbability` to 1 for every
		// slice, so every grain goes down the lane.
		const auto [wetAll, grainAll] = levelsWith(1.0f);
		// THE SECOND COLOUR IS REALLY LIT.
		CHECK(grainAll > 0.0f);
		CHECK(grainAll == doctest::Approx(kLoud).epsilon(0.02));
		// **AND THE FIRST WENT DARK, WHICH IS THE HALF AN OVERLAPPING PAIR WOULD FAIL.** A build that
		// read the wet AFTER the rejoin would report `wetAll` at full scale here — the lane's output
		// having been added back into it — and the two colours would both be bright while only one
		// road was carrying anything. **This line is the whole of the decision, as a number.**
		CHECK(wetAll < wetNone * 0.1f);
	}

	SUBCASE("the peak is HELD, so a surface drawing at 60 fps cannot miss a transient") {
		// **`busutil_meter.h`'s `RawPeak` ARGUMENT, RESTATED FOR THIS DEVICE.** A value published per
		// block is overwritten several times between two repaints; held for `kOutLevelHoldSeconds` it
		// cannot be. Both halves are checked, because "held" that never falls is a stuck meter and
		// "falls" that is immediate is the bug the hold exists to prevent.
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper(Mode::Shuffle);
		cfg.mix = 1.0f;
		capture(s, cfg, &beats);
		driveAndPeak(s, 9600, kLoud, &beats);
		REQUIRE(s.wetLevel() > 0.0f); // not vacuous: there is something to hold

		// THE WET IS CUT OFF AT THE SOURCE — bypassed, so the contribution is exactly zero from here.
		cfg.enabled = false;
		s.setTarget(cfg);
		// **ONE SHORT BLOCK IS NOT ENOUGH TO RETIRE IT.** 480 frames is 10 ms against a 50 ms window.
		driveAndPeak(s, 480, kLoud, &beats);
		CHECK(s.wetLevel() > 0.0f);
		// **AND TWO FULL WINDOWS ARE.** 2 × 50 ms at 48 kHz is 4800 frames; the first turns the window
		// over onto a candidate that is already zero, the second is the margin.
		driveAndPeak(s, 9600, kLoud, &beats);
		CHECK(s.wetLevel() == 0.0f);
		CHECK(s.grainLevel() == 0.0f);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE IDLE RING RECORDS WHILE THE DEVICE IS ON AND ENGAGE IS OFF — AND NOT WHILE IT IS BYPASSED.   //
//--------------------------------------------------------------------------------------------------//
// SKLITTER records while the DEVICE is active, even with the card's Engage button off. It already
// did; these pin it. They began as uncommitted
// probes (2026-09-17, tree `sklitter-refill`) asking whether a Grain gesture left the ring stale. The
// answer is that it does not. BYPASS is the one release that stops the recording, and that is correct by
// design: `Enabled` off means the device does nothing at all (see `sklitter.cpp`'s "BYPASS IS NOT THE
// GESTURE"), so a re-enabled device plays what it last heard until a buffer's length of new input has
// come in.

TEST_CASE("the idle ring RECORDS after a gesture released by Engage, in Grain and Shuffle — and a BYPASSED device does not") {
	const auto freshShare = [](bool releaseByBypass, Mode mode) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Settings cfg = looper(mode, 0.5f);
		cfg.engage = false;
		s.setTarget(cfg);
		double beats = 0.0;
		driveConst(s, 2 * 96000, 0.25f, &beats); // pre-warm with OLD material
		cfg.engage = true;
		s.setTarget(cfg);
		for (int i = 0; i < 400 && s.statusForTest() != Status::Playing; ++i) {
			driveConst(s, 960, 0.25f, &beats);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		driveConst(s, 48000, 0.25f, &beats);
		if (releaseByBypass) {
			cfg.enabled = false;
		}
		else {
			cfg.engage = false;
		}
		s.setTarget(cfg);
		driveConst(s, 3 * 96000, 0.75f, &beats); // NEW material, three bars of it
		const Ring& ring = s.ringForTest();
		size_t fresh = 0;
		for (size_t i = 0; i < ring.length(); ++i) {
			float l = 0.0f;
			float r = 0.0f;
			ring.readAt(i, &l, &r);
			if (std::abs(l - 0.75f) < 1e-3f) {
				++fresh;
			}
		}
		MESSAGE("mode " << static_cast<int>(mode) << (releaseByBypass ? " bypass" : " engage-off") << ": status "
		                << static_cast<int>(s.statusForTest()) << ", fresh " << fresh << " of " << ring.length());
		return static_cast<double>(fresh) / static_cast<double>(std::max<size_t>(1, ring.length()));
	};
	CHECK(freshShare(false, Mode::Grain) > 0.99);
	CHECK(freshShare(false, Mode::Shuffle) > 0.99);
	// BYPASSED: not a frame of the new material reaches the ring, by design (see the block above).
	CHECK(freshShare(true, Mode::Grain) == 0.0);
	CHECK(freshShare(true, Mode::Shuffle) == 0.0);
}

TEST_CASE("the NEXT Grain gesture plays the refilled ring, not the material the last one froze") {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = looper(Mode::Grain, 0.5f);
	cfg.engage = false;
	s.setTarget(cfg);
	double beats = 0.0;
	driveConst(s, 2 * 96000, 0.25f, &beats);
	cfg.engage = true;
	s.setTarget(cfg);
	for (int i = 0; i < 400 && s.statusForTest() != Status::Playing; ++i) {
		driveConst(s, 960, 0.25f, &beats);
	}
	REQUIRE(s.statusForTest() == Status::Playing);
	driveConst(s, 48000, 0.25f, &beats);
	cfg.engage = false;
	s.setTarget(cfg);
	driveConst(s, 3 * 96000, 0.75f, &beats);
	cfg.engage = true;
	s.setTarget(cfg);
	// NEW input from here is SILENCE, so anything heard is the ring.
	constexpr int kChunk = 960;
	std::vector<float> l(kChunk);
	std::vector<float> r(kChunk);
	size_t near25 = 0;
	size_t near75 = 0;
	size_t other = 0;
	int playingBlocks = 0;
	for (int block = 0; block < 600 && playingBlocks < 200; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kChunk);
		beats += kChunk / (0.5 * kRate);
		if (s.statusForTest() != Status::Playing) {
			continue;
		}
		++playingBlocks;
		for (float v : l) {
			const float a = std::abs(v);
			if (a < 1e-4f) {
				continue;
			}
			// a grain envelope scales the level, so classify by which level it cannot exceed
			(a > 0.26f ? near75 : (a > 0.0f ? near25 : other))++;
		}
	}
	MESSAGE("playing blocks " << playingBlocks << ": samples above 0.26 (only the NEW material can reach): "
	                          << near75 << ", at or below: " << near25);
	CHECK(near75 > 0u);
}

TEST_CASE("the idle ring refills by exactly what it was fed — transport playing or stopped, 1 or 4 bars, short or full wait") {
	struct V {
		const char* what;
		bool playing;
		double bufferBars;
		int waitFrames;
	};
	for (const V v : { V { "playing, 1 bar, wait 1 bar", true, 1.0, 96000 },
	                   V { "STOPPED, 1 bar, wait 1 bar", false, 1.0, 96000 },
	                   V { "playing, 4 bars, wait 1 bar", true, 4.0, 96000 },
	                   V { "playing, 4 bars, wait 4 bars", true, 4.0, 4 * 96000 },
	                   V { "STOPPED, 4 bars, wait 4 bars", false, 4.0, 4 * 96000 } }) {
		Sklitter s;
		s.prepare(kRate, 4096);
		double beats = 0.0;
		const auto drive = [&](int frames, float value) {
			constexpr int kChunk = 960;
			std::vector<float> l(kChunk);
			std::vector<float> r(kChunk);
			for (int done = 0; done < frames; done += kChunk) {
				const int n = std::min(kChunk, frames - done);
				std::fill(l.begin(), l.begin() + n, value);
				std::fill(r.begin(), r.begin() + n, -value);
				s.setTransportClock(at120bpm(beats, v.playing));
				s.processStereo(l.data(), r.data(), n);
				if (v.playing) {
					beats += static_cast<double>(n) / (0.5 * kRate);
				}
			}
		};
		Settings cfg = looper(Mode::Grain, 0.5f);
		cfg.bufferBars = v.bufferBars;
		cfg.engage = false;
		s.setTarget(cfg);
		drive(5 * 96000, 0.25f);
		cfg.engage = true;
		s.setTarget(cfg);
		for (int i = 0; i < 2000 && s.statusForTest() != Status::Playing; ++i) {
			drive(960, 0.25f);
		}
		const bool played = s.statusForTest() == Status::Playing;
		drive(48000, 0.25f);
		cfg.engage = false;
		s.setTarget(cfg);
		drive(v.waitFrames, 0.75f);
		const Ring& ring = s.ringForTest();
		size_t fresh = 0;
		for (size_t i = 0; i < ring.length(); ++i) {
			float l = 0.0f;
			float r = 0.0f;
			ring.readAt(i, &l, &r);
			fresh += std::abs(l - 0.75f) < 1e-3f ? 1u : 0u;
		}
		CAPTURE(v.what);
		REQUIRE(played); // not vacuous: a gesture happened, so there was something to release
		// Every frame fed since the release is in the ring, up to its whole length and no further.
		const size_t fed = std::min(static_cast<size_t>(v.waitFrames), ring.length());
		CHECK(fresh == fed);
	}
}

namespace {

/// **A TRANSPORT THAT CAN BE STOPPED, JUMPED AND RE-TEMPOED, WITH ITS SONG POSITION DERIVED FROM AN
/// INTEGER FRAME COUNT** (backlog GU). Accumulating `beats += n / framesPerBeat` drifts in the last
/// bits, and the pre-warm anchors on `floor(beat)` — so a sum that reads 3.9999999 where the song says
/// 4 moves ring frame 0 a whole block and the reference with it.
struct WriteHeadRig {
	Sklitter s;
	double baseBeats = 0.0; ///< song position at `sinceBase == 0`
	long long sinceBase = 0;
	double secondsPerBeat = 0.5;

	[[nodiscard]] double beats() const {
		return baseBeats + static_cast<double>(sinceBase) / (secondsPerBeat * kRate);
	}
	void jumpTo(double to) {
		baseBeats = to;
		sinceBase = 0;
	}
	void setTempo(double spb) {
		baseBeats = beats();
		sinceBase = 0;
		secondsPerBeat = spb;
	}
	/// When set, the input is a sine locked to the SONG — eight cycles a bar — so material recorded at
	/// one song position is the material any later pass plays at that position.
	bool songSine = false;
	void block(int n, bool playing) {
		std::vector<float> l(static_cast<size_t>(n), 0.25f);
		std::vector<float> r(static_cast<size_t>(n), -0.25f);
		if (songSine) {
			constexpr double kTwoPi = 6.283185307179586476925286766559;
			const double songFrame = beats() * secondsPerBeat * kRate;
			for (int k = 0; k < n; ++k) {
				const auto v = static_cast<float>(0.5 * std::sin(kTwoPi * (songFrame + k) / 12000.0));
				l[static_cast<size_t>(k)] = v;
				r[static_cast<size_t>(k)] = -v;
			}
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(secondsPerBeat);
		c.songPositionBeats = beats();
		c.playing = playing;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), n);
		if (playing) {
			sinceBase += n;
		}
	}
};

constexpr int kRigBlock = 480; // 50 to the beat at 120 BPM, so every downbeat is a block top

/// Pre-warmed from song beat 0, pressed at `pressBeats`, and run until it plays.
void startRig(WriteHeadRig& rig, bool locked, double bars, double pressBeats, float dryWrite = 0.0f) {
	rig.s.prepare(kRate, 4096);
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.engage = false;
	cfg.transportLocked = locked;
	cfg.bufferBars = bars;
	cfg.dryWrite = dryWrite;
	rig.s.setTarget(cfg);
	while (rig.beats() < pressBeats) {
		rig.block(kRigBlock, true);
	}
	REQUIRE(rig.s.prewarmReadyForTest());
	cfg.engage = true;
	rig.s.setTarget(cfg);
	for (int k = 0; k < 400 && rig.s.statusForTest() != Status::Playing; ++k) {
		rig.block(kRigBlock, true);
	}
	REQUIRE(rig.s.statusForTest() == Status::Playing);
}

/// **THE WRITE HEAD AGAINST THE SONG, IN FRAMES, SIGNED AND WRAPPED TO HALF A BUFFER.** The reference is
/// the platform's arithmetic, not the device's: the pre-warm puts ring frame 0 on song beat
/// `anchorBeats`, a buffer is `4 x bars` beats, and it holds `96000 x bars` frames because it was
/// captured at 120 BPM / 48 kHz. After a block the head is one past the block's last frame, which is
/// the song position `rig.beats()` already names.
double writeHeadDrift(const WriteHeadRig& rig, double anchorBeats, double bars) {
	const double bufferBeats = 4.0 * bars;
	const double bufferFrames = 96000.0 * bars;
	const double through = (rig.beats() - anchorBeats) / bufferBeats;
	const double expected = (through - std::floor(through)) * bufferFrames;
	double drift = static_cast<double>(rig.s.linearBarPosForTest()) - expected;
	drift -= bufferFrames * std::floor(drift / bufferFrames + 0.5);
	return drift;
}

/// **THE PRESS THE GU CASES SHARE, AND WHY IT IS OFF THE GRID.** Pre-warmed from beat 0, so ring frame 0
/// is beat 4; pressed a beat and a half past a downbeat, so the gesture starts on the NEXT beat, which
/// is three quarters through a one-bar buffer and a quarter through a two-bar one. A press on a buffer
/// line would put a head that ignores the song at 0 and a head that follows it at 0 too.
double rigPress(double bars) {
	return bars == 1.0 ? 10.5 : 13.5;
}

} // namespace

TEST_CASE("backlog GU: in BOTH settings a press starts BOTH heads at the song's phase, and they stay there") {
	// **THE REFERENCE IS THE ARITHMETIC IN `writeHeadDrift`, NOT THE DEVICE.** MEASURED before the
	// write head was re-derived: 24000 frames off in a one-bar buffer and −48000 in a two-bar one, from
	// the first sample of the gesture and for ever after — the head started at 0 whatever the song was
	// doing, so fresh input from beat 11 went into the frames that hold beat 4.
	//
	// **UNLOCKED TOO, BY TWO DESIGN DECISIONS (2026-09-22).** First the write head — with Transport
	// Locked off, every press starts recording at the song's phase — and then, when that
	// alone left the read cursor at slice 0 a song phase behind it and the feedback loop re-reading
	// what it had just written, the read cursor with it: both heads start in phase on every press, as
	// under the lock. The settings still differ at a STOP (GT/HE), not at a press.
	for (const bool locked : { false, true }) {
		for (const double bars : { 1.0, 2.0 }) {
			CAPTURE(locked);
			CAPTURE(bars);
			WriteHeadRig rig;
			startRig(rig, locked, bars, rigPress(bars));
			REQUIRE(rig.s.prewarmBeatForTest() == 4);
			CHECK(std::abs(writeHeadDrift(rig, 4.0, bars)) < 1.0);
			// **THE READ CURSOR, BY VALUE, THE SAME IN BOTH SETTINGS.** Play begins on beat 11 (one bar)
			// or 14 (two bars): three quarters or half-way through a bar, which on four slices of 24000
			// is slice 3 or 2, at offset 0 — and one 480-frame block has run since.
			const int32_t songSlice = bars == 1.0 ? 3 : 2;
			CHECK(rig.s.sliceIndexForTest() == songSlice);
			CHECK(rig.s.positionInSliceForTest() == static_cast<size_t>(kRigBlock));
			for (int k = 0; k < 600; ++k) { // three bars
				rig.block(kRigBlock, true);
			}
			CHECK(std::abs(writeHeadDrift(rig, 4.0, bars)) < 1.0);
		}
	}
}

TEST_CASE("backlog GU: a RESTART re-phases the write head with the read cursor, in BOTH settings") {
	// **BOTH, BECAUSE THE REJOIN IS BOTH SETTINGS' (backlog HE)** — the toggle gates whether the device
	// plays through the stop, and the re-phasing on the resume belongs to both. MEASURED before: 17280
	// frames off after the locked restart and 26880 after the unlocked one.
	for (const bool locked : { false, true }) {
		for (const double bars : { 1.0, 2.0 }) {
			CAPTURE(locked);
			CAPTURE(bars);
			WriteHeadRig rig;
			startRig(rig, locked, bars, rigPress(bars));
			for (int k = 0; k < 100; ++k) {
				rig.block(kRigBlock, true);
			}
			for (int k = 0; k < 20; ++k) {
				rig.block(kRigBlock, false);
			}
			// Read AFTER the stop: unlocked, the head walked 9600 frames past a song that did not move.
			const double before = writeHeadDrift(rig, 4.0, bars);
			rig.jumpTo(29.3);
			rig.block(kRigBlock, true); // the duck (unlocked) or the seek itself (locked)
			rig.block(kRigBlock, true);
			CHECK(std::abs(writeHeadDrift(rig, 4.0, bars)) < 1.0);
			for (int k = 0; k < 200; ++k) {
				rig.block(kRigBlock, true);
			}
			CHECK(std::abs(writeHeadDrift(rig, 4.0, bars)) < 1.0);
			// **NOT VACUOUS UNLOCKED:** the head free-ran through the stop and was off the song when the
			// transport came back, so the zero above is the rejoin's. (Since the 2026-09-22 decisions the
			// press phases both heads in both settings, so it cannot be the press's offset removed here.)
			if (!locked) {
				CHECK(std::abs(before) > 1000.0);
			}
		}
	}
}

TEST_CASE("backlog GU: a LOOP JUMP re-phases the write head at the next slot end under the lock, and leaves the free run alone") {
	for (const bool locked : { false, true }) {
		for (const double bars : { 1.0, 2.0 }) {
			CAPTURE(locked);
			CAPTURE(bars);
			WriteHeadRig rig;
			startRig(rig, locked, bars, rigPress(bars));
			for (int k = 0; k < 100; ++k) {
				rig.block(kRigBlock, true);
			}
			const size_t headBefore = rig.s.linearBarPosForTest();
			rig.jumpTo(17.1);
			for (int k = 0; k < 200; ++k) { // a bar: past at least one slot end
				rig.block(kRigBlock, true);
			}
			if (locked) {
				CHECK(std::abs(writeHeadDrift(rig, 4.0, bars)) < 1.0);
			}
			else {
				// **THE FREE RUN, BY VALUE: one frame per sample from where it was**, which is what GT's
				// decision leaves a transport that never stopped. The read cursor does not follow this
				// jump unlocked either, and the write head goes with the read cursor.
				const auto buffer = static_cast<size_t>(96000.0 * bars);
				CHECK(rig.s.linearBarPosForTest() == (headBefore + 200u * kRigBlock) % buffer);
			}
		}
	}
}

TEST_CASE("backlog GU: a TEMPO CHANGE — under the lock the write head is the song's at every slot end") {
	// **AT EVERY SLOT END, NOT BETWEEN THEM, AND THAT IS THE READ CURSOR'S CONTRACT TOO.** Between slot
	// ends both heads walk one captured frame per sample while the song moves 1.25 of them at 150 BPM, so
	// each slot of 24000 frames drifts 6000 before the derivation takes it back. The 16-frame blocks
	// bound the walk since the slot end to 15 samples, 4 frames at that ratio. MEASURED before: −24000.
	for (const bool locked : { false, true }) {
		CAPTURE(locked);
		WriteHeadRig rig;
		startRig(rig, locked, 1.0, rigPress(1.0));
		for (int k = 0; k < 100; ++k) {
			rig.block(kRigBlock, true);
		}
		rig.setTempo(0.4);
		const size_t headBefore = rig.s.linearBarPosForTest();
		int32_t slice = rig.s.sliceIndexForTest();
		int slotEnds = 0;
		double worst = 0.0;
		constexpr int kBlocks = 6000; // 96000 samples
		for (int k = 0; k < kBlocks; ++k) {
			rig.block(16, true);
			if (rig.s.sliceIndexForTest() != slice) {
				slice = rig.s.sliceIndexForTest();
				++slotEnds;
				worst = std::max(worst, std::abs(writeHeadDrift(rig, 4.0, 1.0)));
			}
		}
		if (locked) {
			CHECK(slotEnds >= 3);
			CHECK(worst <= 8.0);
		}
		else {
			CHECK(rig.s.linearBarPosForTest() == (headBefore + 16u * kBlocks) % 96000u);
		}
	}
}

namespace {

/// The largest step between neighbouring ring frames, across the wrap — the seam a moved write head
/// could leave in the MATERIAL.
///
/// **THE CAPTURE'S OWN SEAM FADE IS LEFT OUT** — the reference's 5 ms V, which sits at the append head where
/// the capture ended (`Ring::fadeSeamAt`, groove-click) and is a slope the device put there on purpose, not a
/// seam. While it sat at index 0 it landed on this sine's zero at the downbeat and added nothing; at the
/// append head it lands mid-cycle, and it is the same both before and after, so it can hide no move.
float ringMaxStep(const Sklitter& s) {
	const auto& ring = s.ringForTest();
	const size_t len = ring.length();
	const auto fade = static_cast<size_t>(std::lround(0.005 * kRate));
	float worst = 0.0f;
	float prevL = 0.0f;
	float prevR = 0.0f;
	ring.readAt(len - 1, &prevL, &prevR);
	for (size_t i = 0; i < len; ++i) {
		float l = 0.0f;
		float r = 0.0f;
		ring.readAt(i, &l, &r);
		const size_t fromSeam = (i + len - ring.writePos()) % len;
		const bool inFade = fromSeam <= fade || fromSeam >= len - fade;
		if (!inFade) {
			worst = std::max(worst, std::abs(l - prevL));
		}
		prevL = l;
	}
	return worst;
}

} // namespace

TEST_CASE("backlog GU: MEASURE — the seam a re-phased write head leaves in the ring") {
	// **THE QUESTION THE READ CURSOR'S DUCK RAISES FOR THE WRITE HEAD, MEASURED RATHER THAN ARGUED.** A
	// rejoin moves the reader and needed `engageFadeFrames_` to hide it; moving the WRITER is a jump in
	// where the blend lands, which could leave an edge in the material. The input is a sine locked to
	// the song, so a ring written in phase is smooth by construction and any step past the sine's own
	// is the device.
	const double sineStep = 0.5 * 6.283185307179586 / 12000.0;
	MESSAGE("the song-locked sine steps at most " << sineStep << " per frame");
	const char* const names[] { "steady", "restart", "loop jump" };
	for (const bool locked : { false, true }) {
		for (const float dry : { 0.5f, 1.0f }) {
			for (const int scenario : { 0, 1, 2 }) {
				CAPTURE(locked);
				CAPTURE(dry);
				CAPTURE(scenario);
				WriteHeadRig rig;
				rig.songSine = true;
				startRig(rig, locked, 1.0, rigPress(1.0), dry);
				for (int k = 0; k < 100; ++k) {
					rig.block(kRigBlock, true);
				}
				const float before = ringMaxStep(rig.s);
				if (scenario == 1) {
					for (int k = 0; k < 20; ++k) {
						rig.block(kRigBlock, false);
					}
					rig.jumpTo(29.3);
				}
				else if (scenario == 2) {
					rig.jumpTo(17.1);
				}
				for (int k = 0; k < 25; ++k) {
					rig.block(kRigBlock, true);
				}
				const float after = ringMaxStep(rig.s);
				MESSAGE("locked=" << locked << " dry=" << dry << " " << std::string(names[scenario])
				                  << ": ring max step before " << before << " after " << after
				                  << ", write-head drift " << writeHeadDrift(rig, 4.0, 1.0));
				// **UNDER THE LOCK THE RING WAS WRITTEN IN PHASE FROM THE PRESS, SO THE RESTART'S MOVE
				// LANDS ON MATERIAL THAT ALREADY MATCHES** — no seam, to the sine's own step. (A loop jump
				// is a seam in the INPUT, the song itself jumping; and the unlocked restart's seam is what
				// the free run wrote DURING the stop. Both are reported, neither is the re-phasing's.)
				if (locked && scenario != 2) {
					CHECK(after <= before);
					CHECK(after <= static_cast<float>(sineStep * 1.6));
				}
			}
		}
	}
}

TEST_CASE("backlog IB: at rest the band divides by the lengths the SETTINGS imply — bypassed, and parked under the lock") {
	// **THE RIBBON SOMETIMES DID NOT UPDATE, notably on a rate change.**
	//
	// THE MECHANISM: the walk divided by `sliceFrames_` and `bufferFrames_`, and at rest only `prewarm`
	// moves those — which does not run while BYPASSED, nor while the transport is parked under the
	// LOCK. The key held the frames, so it was faithful to a number that was not going to move: Buffer
	// Length never redrew in any looper mode, and a Mode change out of Burst drew every
	// source at 0 (`sliceFrames_` was 0, so the bar was one frame).
	//
	// THE REFERENCE IS RESTATED, NOT READ BACK: a bar is four beats at 0.5 s a beat, the buffer is that
	// times Buffer Length, and the cut is the ladder's. The fresh device's picture is the arrangement.
	struct Rest {
		const char* what;
		bool enabled;
		bool playing;
		bool locked;
	};
	struct Cell {
		const char* what;
		Mode from;
		Mode to;
		double bufferFrom;
		double bufferTo;
	};
	const Rest rests[] = { { "bypassed", false, true, false }, { "parked under the lock", true, false, true } };
	const Cell cells[] = {
		// TO FOUR BARS, NOT DOWN TO A HALF: a buffer shorter than the bar draws every source at 0, which
		// would compare equal to the flat band this case exists to catch.
		{ "Buffer Length in Shuffle", Mode::Shuffle, Mode::Shuffle, 1.0, 4.0 },
		{ "Buffer Length in Repeat", Mode::Repeat, Mode::Repeat, 1.0, 4.0 },
		{ "Buffer Length in Grain", Mode::Grain, Mode::Grain, 1.0, 4.0 },
		{ "Mode Burst -> Repeat", Mode::Burst, Mode::Repeat, 2.0, 2.0 },
	};
	const auto drive = [](Sklitter& s, int frames, double* beats, bool playing) {
		std::vector<float> l(960);
		std::vector<float> r(960);
		for (int done = 0; done < frames; done += 960) {
			for (int i = 0; i < 960; ++i) {
				l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = std::sin(0.03f * static_cast<float>(done + i));
			}
			s.setTransportClock(at120bpm(*beats, playing));
			s.processStereo(l.data(), r.data(), 960);
			s.publishReadings();
			if (playing) {
				*beats += 960.0 / (kRate * 0.5);
			}
		}
	};
	constexpr size_t kBar = 4u * 24000u; // four beats at 0.5 s, 48 kHz

	for (const Rest& rest : rests) {
		for (const Cell& cell : cells) {
			const std::string restName = rest.what;
			const std::string cellName = cell.what;
			CAPTURE(restName);
			CAPTURE(cellName);
			Settings cfg = looper(cell.from, 0.5f);
			cfg.zoneA = 0.35f;
			cfg.zoneB = 0.6f;
			cfg.engage = false;
			cfg.seqBars = 2;
			cfg.bufferBars = cell.bufferFrom;
			cfg.transportLocked = rest.locked;
			Sklitter s;
			s.prepare(kRate, 4096);
			s.setTarget(cfg);
			std::vector<float> published;
			s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
				                                                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
			                                                   } });
			double beats = 0.0;
			drive(s, 3 * static_cast<int>(kBar), &beats, true); // idle and warm, the ordinary way in
			cfg.enabled = rest.enabled;
			s.setTarget(cfg);
			drive(s, 9600, &beats, rest.playing);
			REQUIRE(s.statusForTest() == Status::Off);
			const Arrangement before = arrangementOf(published);

			Settings after = cfg;
			after.mode = cell.to;
			after.bufferBars = cell.bufferTo;
			s.setTarget(after);
			drive(s, 960, &beats, rest.playing);
			REQUIRE(s.statusForTest() == Status::Off);

			// THE LENGTHS THE BAND WAS DRAWN AGAINST, as numbers.
			CHECK(s.pictureBarFramesForTest() == kBar);
			CHECK(s.pictureBufferFramesForTest()
			      == static_cast<size_t>(std::llround(static_cast<double>(kBar) * cell.bufferTo)));

			// THE CUT, from the ladder.
			const size_t perBar = after.mode == Mode::Repeat
			                          ? std::max<size_t>(1, kBar / repeatSliceLength(after.rate, kBar))
			                          : static_cast<size_t>(sliceCountFor(after.mode, after.rate));
			const size_t bars =
			    std::clamp<size_t>(kPictureSteps / perBar, 1, static_cast<size_t>(after.seqBars));
			const Arrangement got = arrangementOf(published);
			CHECK(got.steps == std::min(perBar * bars, kPictureSteps));

			// THE ARRANGEMENT, against a fresh device that never had a key to be stale against — ENABLED and
			// UNLOCKED, so its `prewarm` sets the lengths and the reference does not go through the road
			// under test. (Handing it `after` as is made the bypassed rows' reference the same stale walk.)
			Settings reference = after;
			reference.enabled = true;
			reference.transportLocked = false;
			const Arrangement want = arrangementOf(freshRestingPicture(reference));
			REQUIRE(want.steps == got.steps);
			CHECK(got.sources == want.sources);
			CHECK(got.flags == want.flags);
			// NOT VACUOUS: the change is one the band can show, so a band that did not move is red above.
			CHECK(want.sources != before.sources);
		}
	}
}

TEST_CASE("backlog IO addendum: the band's groove follows every Groove control in every state and every slice mode") {
	// **THE RIBBON DID NOT ALWAYS UPDATE WHEN A GROOVE CONTROL MOVED.** IB's
	// table, for the three Groove controls: idle, ENGAGED, bypassed and parked under the lock, across the five
	// slice modes groove acts in. The published pair must be what a fresh device's `grooveFeel()` says —
	// the engine's one decode — after one block. (The card's decode of the pair is the other half, asserted
	// in `test_gui_widgets_devices.cpp`; that half is where the stale cells were.)
	struct Rest {
		const char* what;
		bool enabled;
		bool engage;
		bool playing;
		bool locked;
		Status status;
	};
	struct Move {
		const char* what;
		GrooveMode fromMode;
		float fromAmount;
		float fromPhase;
		GrooveMode toMode;
		float toAmount;
		float toPhase;
	};
	const Rest rests[] = {
		{ "idle", true, false, true, false, Status::Off },
		{ "engaged", true, true, true, false, Status::Playing },
		{ "bypassed", false, false, true, false, Status::Off },
		{ "parked under the lock", true, false, false, true, Status::Off },
	};
	const Move moves[] = {
		{ "Groove Mode Straight -> Swing 8", GrooveMode::Straight, 0.5f, 0.0f, GrooveMode::Swing8, 0.5f, 0.0f },
		{ "Groove Mode Swing 8 -> Free 16", GrooveMode::Swing8, 0.5f, 2.5f, GrooveMode::Free16, 0.5f, 2.5f },
		{ "Groove Amount 0.3 -> 0.7", GrooveMode::Groove16, 0.3f, 0.0f, GrooveMode::Groove16, 0.7f, 0.0f },
		{ "Groove Amount 0.3 -> 0", GrooveMode::Swing16, 0.3f, 0.0f, GrooveMode::Swing16, 0.0f, 0.0f },
		{ "Free Phase 0.4 -> 2.7", GrooveMode::Free8, 0.0f, 0.4f, GrooveMode::Free8, 0.0f, 2.7f },
		{ "Free Phase 2.7 -> 3.9", GrooveMode::Free8, 0.0f, 2.7f, GrooveMode::Free8, 0.0f, 3.9f },
		{ "Free Phase 0.4 -> -3.2", GrooveMode::Free4, 0.0f, 0.4f, GrooveMode::Free4, 0.0f, -3.2f },
		{ "Free Phase 0.4 -> 137.25", GrooveMode::Free16, 0.0f, 0.4f, GrooveMode::Free16, 0.0f, 137.25f },
		// THE SYMPTOM (2026-09-23): Free driven up to about 140 and then reset to zero left the groove in
		// place. Phase 0 is straight, so the band is told Straight.
		{ "Free Phase 140 -> 0", GrooveMode::Free8, 0.0f, 140.0f, GrooveMode::Free8, 0.0f, 0.0f },
	};
	const Mode modes[] = { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch };
	const auto drive = [](Sklitter& s, int frames, double* beats, bool playing) {
		std::vector<float> l(960);
		std::vector<float> r(960);
		for (int done = 0; done < frames; done += 960) {
			for (int i = 0; i < 960; ++i) {
				l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = std::sin(0.03f * static_cast<float>(done + i));
			}
			s.setTransportClock(at120bpm(*beats, playing));
			s.processStereo(l.data(), r.data(), 960);
			s.publishReadings();
			if (playing) {
				*beats += 960.0 / (kRate * 0.5);
			}
		}
	};
	constexpr int kBar = 4 * 24000;
	size_t cells = 0;
	for (const Rest& rest : rests) {
		for (const Mode mode : modes) {
			for (const Move& move : moves) {
				const std::string restName = rest.what;
				const std::string moveName = move.what;
				CAPTURE(restName);
				CAPTURE(moveName);
				CAPTURE(static_cast<int>(mode));
				Settings cfg = looper(mode, 0.5f);
				cfg.engage = rest.engage;
				cfg.transportLocked = rest.locked;
				cfg.grooveMode = static_cast<float>(move.fromMode);
				cfg.grooveAmount = move.fromAmount;
				cfg.freePhase = move.fromPhase;
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTarget(cfg);
				std::vector<float> published;
				s.setReadingsSink(feathers::device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
					                                                   static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
				                                                   } });
				double beats = 0.0;
				drive(s, 3 * kBar, &beats, true);
				cfg.enabled = rest.enabled;
				s.setTarget(cfg);
				drive(s, 9600, &beats, rest.playing);
				REQUIRE(s.statusForTest() == rest.status);
				const float beforeMode = published[static_cast<size_t>(Reading::Groove)];
				const float beforeAmount = published[static_cast<size_t>(Reading::GrooveAmount)];

				Settings after = cfg;
				after.grooveMode = static_cast<float>(move.toMode);
				after.grooveAmount = move.toAmount;
				after.freePhase = move.toPhase;
				s.setTarget(after);
				drive(s, 960, &beats, rest.playing);
				REQUIRE(s.statusForTest() == rest.status);

				// THE REFERENCE: the engine's one decode of the controls, Straight where it is the identity.
				const GrooveFeel want = after.grooveFeel();
				const bool acts = grooveActive(want);
				const float wantMode = acts ? static_cast<float>(want.mode) : 0.0f;
				const float wantAmount = acts ? static_cast<float>(want.amount) : 0.0f;
				CHECK(published[static_cast<size_t>(Reading::Groove)] == wantMode);
				CHECK(published[static_cast<size_t>(Reading::GrooveAmount)] == wantAmount);
				// RESTATED FOR FREE: the amount published IS the phase, unclamped.
				if (isFree(move.toMode)) {
					CHECK(published[static_cast<size_t>(Reading::GrooveAmount)] == move.toPhase);
					// AND AT PHASE 0 THE BAND IS STRAIGHT — stated as the literal, not through the decode.
					if (move.toPhase == 0.0f) {
						CHECK(published[static_cast<size_t>(Reading::Groove)] == 0.0f);
					}
				}
				// NOT VACUOUS: every move is one the pair shows.
				CHECK((beforeMode != wantMode || beforeAmount != wantAmount));
				++cells;
			}
		}
	}
	CHECK(cells == std::size(rests) * std::size(modes) * std::size(moves));
}

//--------------------------------------------------------------------------------------------------//
// GROOVE — backlog IG. WHEN A SLICE STARTS, SWUNG.                                                  //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **THE TEMPLATE, RESTATED FROM THE DESIGN RATHER THAN READ FROM `groove.h`.** Swing: an odd grid step
/// lands `2·ratio − 1` late, the ratio running 50% → 75% (the MPC's) over the amount, which is
/// `amount · ½` steps. Groove: step j lands `amount · ½ · shape(frac(j·φ))` late — the same ceiling — φ
/// computed here from its definition. The downbeat and the next one never move.
constexpr double kRefEnvelopeSteps = 0.5;

double refOffsetSteps(int grid, bool swing, double amount, int j) {
	if (j <= 0 || j >= grid) {
		return 0.0;
	}
	if (swing) {
		return (j % 2 == 1) ? amount * kRefEnvelopeSteps : 0.0;
	}
	// THE MPC BIAS (the templates lean towards the MPC's): an off-beat takes at least half the envelope and
	// phi the rest; an on-beat at most a quarter of it.
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	const double x = static_cast<double>(j) * phi;
	const double f = x - std::floor(x);
	return amount * kRefEnvelopeSteps * ((j % 2 == 1) ? 0.5 + 0.5 * f : 0.25 * f);
}

/// Where slice boundary k lands in frames of the bar: the straight grid the device has always cut
/// (`k · (bar / N)`, the remainder to the last slice) displaced by the warp — which is linear between
/// grid points, so a boundary between two of them takes their offsets in proportion.
double refFreeSteps(int grid, double amount, int j); // below, with the helpers backlog IO added

/// `free` takes Free's walk (backlog IO) in place of Swing's or Groove's template; `swing` is then unread.
long long refBoundary(int grid, bool swing, double amount, int k, int n, long long bar, bool free = false) {
	if (k >= n) {
		return bar;
	}
	const long long base = bar / n;
	const double at = static_cast<double>(k) * grid / static_cast<double>(n); // in grid steps
	const int j = static_cast<int>(std::floor(at));
	const double f = at - j;
	const double lo = free ? refFreeSteps(grid, amount, j) : refOffsetSteps(grid, swing, amount, j);
	const double hi = free ? refFreeSteps(grid, amount, j + 1) : refOffsetSteps(grid, swing, amount, j + 1);
	const double steps = lo + (hi - lo) * f;
	return static_cast<long long>(k) * base + std::llround(steps * static_cast<double>(bar) / grid);
}

/// The controls, set as the row would carry them (backlog IO): a mode, and the number that mode reads — the
/// depth for Swing and Groove, the unbounded phase for Free. Both are written; the device reads one.
void setGroove(Settings& s, int mode, double amount) {
	s.grooveMode = static_cast<float>(mode);
	s.grooveAmount = static_cast<float>(amount);
	s.freePhase = static_cast<float>(amount);
}

/// The amount the device will decode, restated: the float the row carries.
double storedAmount(double amount) {
	return static_cast<double>(static_cast<float>(amount));
}

/// **FREE'S OFFSET, RESTATED FROM THE DESIGN RATHER THAN READ FROM `groove.h`** (backlog IO): step j walks at
/// its own rate `r_j = φ^(1/j)` scaled by its metrical duty `d_j = ½^level` (level = trailing zero bits of j),
/// its wave is a triangle that is 0 for the first `1 − d_j` of each cycle of `|a|·r_j·d_j` — no starting
/// phase, so every wave is 0 at a = 0 (phase 0 is straight by design) — and rises to 1 and back over the
/// rest; an off-beat is late by `½·term` steps, anything stronger by `½·¼·term`. No floor.
double refFreeSteps(int grid, double amount, int j) {
	if (j <= 0 || j >= grid) {
		return 0.0;
	}
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	int level = 0;
	for (int s = j; s % 2 == 0; s /= 2) {
		++level;
	}
	const double duty = std::ldexp(1.0, -level);
	const double rate = std::exp(std::log(phi) / static_cast<double>(j));
	const double x = (amount < 0.0 ? -amount : amount) * rate * duty;
	const double p = x - std::floor(x);
	const double term = p < 1.0 - duty ? 0.0 : 1.0 - std::fabs(2.0 * (p - (1.0 - duty)) / duty - 1.0);
	return 0.5 * ((j % 2 == 1) ? term : 0.25 * term);
}

struct SlotStart {
	int32_t slice;
	long long barFrame;   ///< where in the song's bar the slot began, from the counted frame
	size_t deviceStart;   ///< where the device says it began
};

/// Drive one device over an exact 120 BPM transport and record every slot start it makes after the press.
std::vector<SlotStart> slotStartsOf(const Settings& cfg, int blocks) {
	constexpr int kBlock = 960;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	std::vector<SlotStart> out;
	size_t n = 0;
	bool wasPlaying = false;
	for (int block = 0; block < blocks; ++block) {
		for (int i = 0; i < kBlock; ++i, ++n) {
			const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
			l[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph));
			r[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph * 1.5));
		}
		s.setTransportClock(at120bpm(static_cast<double>(block * kBlock) / (0.5 * kRate)));
		s.processStereo(l.data(), r.data(), kBlock);
		const bool playing = s.statusForTest() == Status::Playing;
		if (wasPlaying && playing && s.positionInSliceForTest() < static_cast<size_t>(kBlock)) {
			const auto bar = static_cast<long long>(s.sliceFramesForTest());
			const long long songFrame = static_cast<long long>(block) * kBlock + kBlock
			                            - static_cast<long long>(s.positionInSliceForTest());
			out.push_back({ s.sliceIndexForTest(), songFrame % bar, s.slotStartFrameForTest() });
		}
		wasPlaying = playing;
	}
	return out;
}

} // namespace

TEST_CASE("backlog IG: the groove warp, per mode — offsets at every grid point, monotonic, downbeat pinned, Straight the identity") {
	const std::array<int, 10> grids { 0, 4, 8, 16, 4, 8, 16, 4, 8, 16 };
	for (int mode = 0; mode < 10; ++mode) {
		for (const double amount : { 0.0, 0.25, 2.0 / 3.0, 0.9, 1.0 }) {
			CAPTURE(mode);
			CAPTURE(amount);
			const GrooveFeel feel { static_cast<GrooveMode>(mode), amount };
			const int grid = grids[static_cast<size_t>(mode)];
			const bool swing = mode >= 1 && mode <= 3;
			const bool free = mode >= 7;
			CHECK(grooveGridOf(feel.mode) == grid);
			// THE ENDS ARE PINNED, FOR EVERY FEEL AT EVERY AMOUNT: the bar stays the bar.
			CHECK(grooveWarp(feel, 0.0) == 0.0);
			CHECK(grooveWarp(feel, 1.0) == doctest::Approx(1.0).epsilon(1e-15));
			// EVERY FAMILY IS THE IDENTITY AT 0 — Free too since its phase 0 is straight (backlog IO addendum).
			CHECK(grooveActive(feel) == (mode != 0 && amount > 0.0));
			if (mode == 0 || amount == 0.0) {
				// THE IDENTITY, EXACTLY — not approximately — which is what lets the device skip the warp.
				for (int k = 0; k <= 1000; ++k) {
					const double p = static_cast<double>(k) / 1000.0;
					CHECK(grooveWarp(feel, p) == p);
				}
				continue;
			}
			// EVERY GRID POINT, AGAINST THE RESTATED TEMPLATE.
			for (int j = 0; j <= grid; ++j) {
				CAPTURE(j);
				const double ref = free ? refFreeSteps(grid, amount, j) : refOffsetSteps(grid, swing, amount, j);
				const double want = (static_cast<double>(j) + ref) / grid;
				CHECK(grooveWarp(feel, static_cast<double>(j) / grid) == doctest::Approx(want).epsilon(1e-12));
				// ABSOLUTE, not `Approx`: its default scale of 1 passes anything within ~1e-5 of a zero offset, and
				// Free's stronger steps are exactly zero most of the walk.
				CHECK(std::fabs(grooveStepOffset(feel, j) - ref) <= 1e-12);
			}
			// STRICTLY MONOTONIC across a fine sweep: no step crosses its neighbour.
			double last = -1.0;
			for (int k = 0; k <= 4096; ++k) {
				const double w = grooveWarp(feel, static_cast<double>(k) / 4096.0);
				CHECK(w > last);
				last = w;
			}
		}
	}
	// THE TRIPLET POINT IS TWO THIRDS OF THE AMOUNT'S TRAVEL (50% → 75%, the MPC's): the odd step lands a
	// third of a step late. The ceiling is the MPC's 75% (a 5:1 widening was tried and set aside, backlog IO).
	CHECK(swingRatioOf(2.0 / 3.0) == doctest::Approx(2.0 / 3.0));
	CHECK(grooveStepOffset({ GrooveMode::Swing16, 2.0 / 3.0 }, 1) == doctest::Approx(1.0 / 3.0));
	CHECK(swingRatioOf(0.0) == 0.5);
	CHECK(swingRatioOf(1.0) == 0.75);
	// THE DECODE (backlog IO): the mode is the nearest step, clamped; the amount the stored 0..1.
	CHECK(grooveModeOf(0.0) == GrooveMode::Straight);
	CHECK(grooveModeOf(9.0) == GrooveMode::Free16);
	CHECK(grooveModeOf(5.4) == GrooveMode::Groove8);
	CHECK(grooveModeOf(42.0) == GrooveMode::Free16);
	CHECK(grooveModeOf(-3.0) == GrooveMode::Straight);
	CHECK(grooveModeOf(std::nan("")) == GrooveMode::Straight);
	{
		Settings s;
		s.mode = Mode::Shuffle;
		setGroove(s, 3, 1.7); // an amount past the range reads as its top
		CHECK(s.grooveFeel() == GrooveFeel { GrooveMode::Swing16, 1.0 });
		s.grooveAmount = std::nanf("");
		CHECK(s.grooveFeel() == GrooveFeel { GrooveMode::Swing16, 0.0 });
	}
	// AND THE ENGINE'S GATE: the five slice modes, not Burst or Grain.
	for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
		Settings s;
		s.mode = static_cast<Mode>(m);
		setGroove(s, 5, 0.5);
		const bool live = s.mode != Mode::Burst && s.mode != Mode::Grain;
		CAPTURE(m);
		CHECK(grooveActive(s.grooveFeel()) == live);
	}
}

TEST_CASE("backlog IG: Shuffle at 16 slices, Swing 16 at the triplet point — every odd slice starts a third of a step late, to the frame") {
	// 120 BPM at 48 kHz: a bar is 96000 frames, sixteen slices of 6000, and the triplet delays each odd
	// sixteenth by a third of one: 2000 frames. The frames are COUNTED from the transport, not asked of
	// the device — `deviceStart` is checked against the same number as a second assertion.
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	setGroove(cfg, 3, 2.0 / 3.0); // Swing 16 at the triplet, two thirds of the amount
	REQUIRE(cfg.grooveFeel().mode == GrooveMode::Swing16);
	const std::vector<SlotStart> starts = slotStartsOf(cfg, 600);
	int odd = 0;
	int even = 0;
	for (const SlotStart& st : starts) {
		CAPTURE(st.slice);
		const long long want = static_cast<long long>(st.slice) * 6000 + (st.slice % 2 == 1 ? 2000 : 0);
		CHECK(st.barFrame == want);
		CHECK(static_cast<long long>(st.deviceStart) == want);
		(st.slice % 2 == 1 ? odd : even) += 1;
	}
	// NOT VACUOUS: odd slices were reached — a walk of long grains could otherwise step over every one.
	CHECK(odd > 20);
	CHECK(even > 20);
}

TEST_CASE("backlog IG: Groove 8 near full — the slice starts match the phi template, to the frame") {
	// 0.99 — kept from the zoned control, whose Groove 8 could not reach 1; the split lifts that limit.
	// Sixteen slices over an eighth-note grid: an even slice sits ON grid point k/2 and takes its offset;
	// an odd one sits halfway between two and takes the mean of theirs.
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	setGroove(cfg, 5, 0.99);
	REQUIRE(cfg.grooveFeel().mode == GrooveMode::Groove8);
	const double amount = storedAmount(0.99);
	const std::vector<SlotStart> starts = slotStartsOf(cfg, 600);
	std::set<int32_t> seen;
	for (const SlotStart& st : starts) {
		CAPTURE(st.slice);
		const long long want = refBoundary(8, false, amount, st.slice, 16, 96000);
		CHECK(st.barFrame == want);
		CHECK(static_cast<long long>(st.deviceStart) == want);
		seen.insert(st.slice);
	}
	CHECK(seen.size() >= 8);
	// AND THE TEMPLATE IS NOT THE STRAIGHT GRID: grid point 1 (slice 2), an off-beat, lands
	// 0.99 · ½ · (½ + ½·0.618) steps late.
	CHECK(refBoundary(8, false, amount, 2, 16, 96000) - 12000
	      == std::llround(amount * 0.5 * (0.5 + 0.5 * 0.6180339887498949) * 12000.0));
}

TEST_CASE("backlog IG: the Groove template stays inside the swing envelope, and its off-beats carry the lateness") {
	// The templates are biased towards the MPC's. In the MPC's pair-relative terms a step `d`
	// grid steps late sits at `50 + 50·d` % of its pair. At full amount every OFF-BEAT must sit between the
	// envelope's floor (62.5%) and the MPC's ceiling (75%); every ON-BEAT must stay within an eighth of a
	// step, and never be later than the off-beat that follows it.
	for (const GrooveMode zone : { GrooveMode::Groove4, GrooveMode::Groove8, GrooveMode::Groove16 }) {
		const GrooveFeel full { zone, 1.0 };
		const int grid = grooveGridOf(zone);
		CAPTURE(grid);
		for (int j = 1; j < grid; ++j) {
			CAPTURE(j);
			const double d = grooveStepOffset(full, j);
			if (j % 2 == 1) {
				const double pct = 50.0 + 50.0 * d;
				CHECK(pct >= 62.5 - 1e-9);
				CHECK(pct <= 75.0 + 1e-9);
			}
			else {
				CHECK(d >= 0.0);
				CHECK(d <= 0.125 + 1e-12);
			}
		}
		// THE SWING ZONE AT THE SAME AMOUNT IS THE ENVELOPE'S CEILING: 75%.
		CHECK(50.0 + 50.0 * grooveStepOffset({ GrooveMode::Swing16, 1.0 }, 1) == doctest::Approx(75.0));
	}
	// THE MPC'S CLASSIC SWING SETTINGS ARE READINGS ON THE LINEAR TRAVEL, at 0/.16/.32/.48/.64/.84/1.
	// (That each is an exact READOUT — `58.0%` — is asserted where the readout is, in the widget suite.)
	for (const double pct : { 50.0, 54.0, 58.0, 62.0, 66.0, 71.0, 75.0 }) {
		const double amount = (pct - 50.0) / 25.0;
		CAPTURE(pct);
		CHECK(swingRatioOf(amount) * 100.0 == doctest::Approx(pct));
	}
	// AND FREE'S ENVELOPE IS THE SAME CEILING, not a constant of its own that could drift from it.
	CHECK(kFreeMaxSteps == kGrooveMaxSteps);
	CHECK(kGrooveMaxSteps == 0.5);
}

TEST_CASE("backlog IO: Free N — the walk is continuous and bounded step to step, and does walk") {
	// The amount control gives phi-varied, sensible offsets for each beat — Amount as a phase
	// through the φ sequence, so a route sweeping it must never JUMP. The bound, restated from the rule: a
	// triangle of duty d rises by 1 over half of d of its phase, and the phase runs at r·d per unit amount,
	// so |d term/da| = 2r ≤ 2φ whatever the duty — |d offset/da| ≤ ½·2φ = φ for an off-beat (no floor since
	// phase 0 went straight, so the whole half step is travel) and a quarter of that for a stronger step.
	// Swept at 0..16, ACROSS 0 from −8 (the mirror's kink), AND far out along the walk, since it has no lap.
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	constexpr int kSteps = 4000;
	constexpr double kDa = 1.0 / kSteps;
	REQUIRE(kFreeMaxSlope == doctest::Approx(phi)); // groove.h's constant agrees with the restatement
	for (const GrooveMode mode : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(mode);
		CAPTURE(grid);
		double widest = 0.0; // the largest step-to-step change seen, in grid steps
		for (int j = 1; j < grid; ++j) {
			CAPTURE(j);
			const double bound = (j % 2 == 1 ? phi : 0.25 * phi) * kDa + 1e-12;
			// Sixteen units of amount from each start: the slowest cycle (the half bar, duty ⅛, rate ≥ 1) is at
			// most eight units long, so every step's triangle rises at least once inside the sweep — on each
			// side of 0 for the sweep from −8.
			for (const double from : { 0.0, -8.0, 997.25 }) {
				CAPTURE(from);
				double last = grooveStepOffset({ mode, from }, j);
				double lowest = last;
				double highest = last;
				for (int k = 1; k <= 16 * kSteps; ++k) {
					const double now = grooveStepOffset({ mode, from + static_cast<double>(k) * kDa }, j);
					CHECK(std::fabs(now - last) <= bound);
					widest = std::max(widest, std::fabs(now - last));
					lowest = std::min(lowest, now);
					highest = std::max(highest, now);
					last = now;
				}
				// NOT VACUOUS: it does walk — an off-beat spans the whole half step from on its grid to the
				// MPC's 75%, a stronger step its eighth, each from zero (an off-beat touches zero only at an
				// instant, so a sampled sweep comes within one sample's travel of it).
				CHECK(lowest <= phi * kDa);
				CHECK(highest == doctest::Approx(j % 2 == 1 ? 0.5 : 0.125).epsilon(1e-3));
			}
		}
		CHECK(widest > 0.0);
		// AND THE WALK VISITS DIFFERENT PATTERNS, not one pattern shifted in time: somewhere along it the
		// off-beats' order by lateness differs from the order at 0.3. (Not at 0, where every step ties.)
		const auto order = [&](double a) {
			std::vector<std::pair<double, int>> late;
			for (int j = 1; j < grid; j += 2) {
				late.emplace_back(grooveStepOffset({ mode, a }, j), j);
			}
			std::sort(late.begin(), late.end());
			std::vector<int> out;
			for (const auto& [d, j] : late) {
				out.push_back(j);
			}
			return out;
		};
		const std::vector<int> first = order(0.3);
		bool reordered = false;
		for (int k = 1; k < 64 && !reordered; ++k) {
			reordered = order(0.3 + static_cast<double>(k) / 64.0) != first;
		}
		CHECK(reordered);
	}
}

TEST_CASE("backlog IO: Free N — every pattern along the walk is inside the MPC envelope, the off-beats carry the lateness, and the warp is monotonic") {
	for (const GrooveMode mode : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(mode);
		double offSum = 0.0;
		double onSum = 0.0;
		int offCount = 0;
		int onCount = 0;
		// 0..1, around a thousand units along, and the same below 0 — the walk has no lap, so the far ends are
		// checked too.
		for (int k = 0; k <= 1024; ++k) {
			const double along = (k % 512 > 256 ? 1000.0 : 0.0) + static_cast<double>(k % 512 % 257) / 256.0;
			const GrooveFeel feel { mode, k >= 512 ? -along : along };
			CAPTURE(grid);
			CAPTURE(feel.amount);
			REQUIRE(grooveActive(feel) == (feel.amount != 0.0));
			// THE DOWNBEATS ARE PINNED.
			CHECK(grooveStepOffset(feel, 0) == 0.0);
			CHECK(grooveStepOffset(feel, grid) == 0.0);
			for (int j = 1; j < grid; ++j) {
				CAPTURE(j);
				const double d = grooveStepOffset(feel, j);
				CHECK(d >= 0.0);
				if (j % 2 == 1) {
					// AN OFF-BEAT: 50% to 75% of its pair — the MPC's own range, never past its ceiling.
					CHECK(50.0 + 50.0 * d <= 75.0 + 1e-9);
					offSum += d;
					++offCount;
				}
				else {
					// ANY STRONGER STEP: at most an eighth of a step.
					CHECK(d <= 0.125 + 1e-12);
					onSum += d;
					++onCount;
				}
			}
			double last = -1.0;
			for (int s = 0; s <= 1024; ++s) {
				const double w = grooveWarp(feel, static_cast<double>(s) / 1024.0);
				CHECK(w > last);
				last = w;
			}
		}
		// THE OFF-BEATS CARRY THE LATENESS — by the duties and the maxima now, not by a floor: across the sweep
		// an off-beat is late on average by several times what a stronger step is (¼ against ≤ 1/32 in the
		// long run).
		CAPTURE(grid);
		const double offMean = offSum / offCount;
		const double onMean = onSum / onCount;
		MESSAGE("Free " << grid << ": mean off-beat lateness " << offMean << " steps, stronger steps " << onMean);
		CHECK(offMean > 4.0 * onMean);
	}
}

TEST_CASE("backlog IO: Free N — a step sits exactly on its grid for (1 − duty) of a long walk, the duty halving per metrical level") {
	// A beat hierarchy by subdivision in the selected mode, chosen by a principled rule. Metrical weight: level = the trailing zero bits of the step, duty = ½^level. No step carries a
	// floor (phase 0 is straight), so between its triangle's rises its offset is EXACTLY zero — and the share
	// of a long sweep it spends there is 1 − duty. The weakest steps (the off-beats, duty 1) have no zero
	// segment: they touch the grid only at the ends of their cycles, which a sweep from past 0 never samples.
	constexpr int kSamples = 400000;
	constexpr double kLength = 4000.0; // units of amount — hundreds of cycles even at the slowest rate
	for (const GrooveMode mode : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(mode);
		for (int j = 1; j < grid; ++j) {
			int level = 0;
			for (int s = j; s % 2 == 0; s /= 2) {
				++level;
			}
			const double duty = std::ldexp(1.0, -level);
			CAPTURE(grid);
			CAPTURE(j);
			CAPTURE(level);
			CHECK(freeDuty(j) == duty);
			int still = 0;
			for (int k = 1; k <= kSamples; ++k) {
				const double a = kLength * static_cast<double>(k) / kSamples;
				still += grooveStepOffset({ mode, a }, j) == 0.0 ? 1 : 0;
			}
			const double share = static_cast<double>(still) / kSamples;
			if (level == 0) {
				CHECK(still == 0); // an off-beat is moved all the time
			}
			else {
				CHECK(share == doctest::Approx(1.0 - duty).epsilon(0.02));
			}
		}
	}
	// THE LEVELS ON A SIXTEENTH GRID, spelled out: the odd steps 0, 2/6/10/14 level 1, 4/12 level 2, 8 level 3.
	CHECK(metricalLevel(1) == 0);
	CHECK(metricalLevel(6) == 1);
	CHECK(metricalLevel(12) == 2);
	CHECK(metricalLevel(8) == 3);
}

TEST_CASE("backlog IO: Free N does not repeat — the pattern at a and at a + k differs, and every pair of step rates is incommensurate") {
	// Free drives nonperiodic phi groove mutations, so its phase does not wrap and may go arbitrarily
	// high. Each step walks at `r_j = φ^(1/j)`, so `r_j / r_k = φ^(1/j − 1/k)` — a nonzero
	// rational power of φ, which is irrational (`groove.h` has the two-line proof). Asserted here as the
	// defining identity `r_j^j = φ`, in [1, 2), pairwise distinct — the premises the proof needs.
	// **A FIRST DRAFT USED `1 + frac(j·φ²)` AND THIS CASE CAUGHT IT:** that rate is `j·φ + m_j` for an integer
	// `m_j`, and steps 2 and 3 were `2φ − 2` and `3φ − 3` — commensurate, ratio exactly 2/3.
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	for (int j = 1; j < 16; ++j) {
		CAPTURE(j);
		CHECK(std::pow(freeRate(j), static_cast<double>(j)) == doctest::Approx(phi).epsilon(1e-12));
		CHECK(freeRate(j) >= 1.0);
		CHECK(freeRate(j) < 2.0);
		for (int k = j + 1; k < 16; ++k) {
			CAPTURE(k);
			CHECK(freeRate(j) != freeRate(k));
			// AND NOT A SMALL-DENOMINATOR RATIO IN DISGUISE: no p/q with q ≤ 64 within 1e-9 of the ratio.
			const double ratio = freeRate(j) / freeRate(k);
			bool nearRational = false;
			for (int q = 1; q <= 64 && !nearRational; ++q) {
				nearRational = std::fabs(ratio * q - std::round(ratio * q)) < 1e-9;
			}
			CHECK_FALSE(nearRational);
		}
	}
	// AND BY VALUE: the pattern a whole unit (or several) further along is a DIFFERENT pattern — the
	// periodic rule this replaced made these identical to the bit.
	for (const GrooveMode mode : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(mode);
		for (const double a : { 0.0, 0.37, 5.5, 123.25 }) {
			for (const int k : { 1, 2, 3, 5, 8, 13, 100 }) {
				CAPTURE(grid);
				CAPTURE(a);
				CAPTURE(k);
				double most = 0.0;
				for (int j = 1; j < grid; ++j) {
					most = std::max(most, std::fabs(grooveStepOffset({ mode, a + k }, j) - grooveStepOffset({ mode, a }, j)));
				}
				CHECK(most > 0.01); // grid steps; the off-beats' whole travel is 0.5
			}
		}
	}
}

TEST_CASE("backlog IO: Free 16 in Shuffle — the slice starts are the walk's pattern, to the frame") {
	// Sixteen slices on a sixteenth grid: slice k sits ON grid point k and takes its offset, restated from the
	// design (`refFreeSteps`), counted from the transport as the Swing and Groove cases count theirs.
	Settings cfg = looper(Mode::Shuffle, 0.5f);
	setGroove(cfg, static_cast<int>(GrooveMode::Free16), 0.3);
	const double amount = storedAmount(0.3);
	const std::vector<SlotStart> starts = slotStartsOf(cfg, 600);
	std::set<int32_t> seen;
	for (const SlotStart& st : starts) {
		CAPTURE(st.slice);
		const long long want = static_cast<long long>(st.slice) * 6000
		                       + std::llround(refFreeSteps(16, amount, st.slice) * 6000.0);
		CHECK(st.barFrame == want);
		CHECK(static_cast<long long>(st.deviceStart) == want);
		seen.insert(st.slice);
	}
	CHECK(seen.size() >= 8);
	// NOT VACUOUS: slice 1 lands 0.3·φ into its rising triangle — ½·2·0.485 ≈ 0.485 of a step (2912 frames)
	// late — and the slices seen include odd ones.
	CHECK(std::llround(refFreeSteps(16, amount, 1) * 6000.0) >= 1500);
	CHECK(std::any_of(seen.begin(), seen.end(), [](int32_t k) { return k % 2 == 1; }));
}

namespace {

/// **A DEVICE PLAYING ONE GROOVE, SWITCHED TO ANOTHER MID-RUN, AND THE SLOT STARTS IT MAKES TWO BARS AFTER.**
/// `slotStartsOf`, with a `setTarget` at `switchBlock` — the road a hand on Groove Mode takes while it plays.
std::vector<SlotStart> slotStartsAfterSwitch(const Settings& from, const Settings& to, int switchBlock, int blocks) {
	constexpr int kBlock = 960;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(from);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	std::vector<SlotStart> out;
	size_t n = 0;
	bool wasPlaying = false;
	const int recordFrom = switchBlock + 2 * 100; // two bars of 96000 frames on
	for (int block = 0; block < blocks; ++block) {
		if (block == switchBlock) {
			s.setTarget(to);
		}
		for (int i = 0; i < kBlock; ++i, ++n) {
			const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
			l[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph));
			r[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph * 1.5));
		}
		s.setTransportClock(at120bpm(static_cast<double>(block * kBlock) / (0.5 * kRate)));
		s.processStereo(l.data(), r.data(), kBlock);
		const bool playing = s.statusForTest() == Status::Playing;
		if (block >= recordFrom && wasPlaying && playing && s.positionInSliceForTest() < static_cast<size_t>(kBlock)) {
			const auto bar = static_cast<long long>(s.sliceFramesForTest());
			const long long songFrame = static_cast<long long>(block) * kBlock + kBlock
			                            - static_cast<long long>(s.positionInSliceForTest());
			out.push_back({ s.sliceIndexForTest(), songFrame % bar, s.slotStartFrameForTest() });
		}
		wasPlaying = playing;
	}
	return out;
}

} // namespace

TEST_CASE("backlog IO addendum: Groove Amount has no effect in Free and Free Phase none in Swing or Groove — "
          "offsets, slice starts and the band's reading") {
	// Swing settings are not carried over into Free: with a nonzero swing applied and the mode switched to
	// Free, no swing is added on top of the groove. Asserted by
	// VALUE against the design restated (`refFreeSteps`, `refOffsetSteps`), with the unread control at values
	// that differ — `setGroove` above writes the SAME number to both, which could not see a leak.
	namespace dv = feathers::device;
	const dv::ParamDesc& amountDesc = kDescriptor.params[static_cast<size_t>(Param::GrooveAmount)];
	constexpr float kPhase = 2.3f;
	// THE AMOUNTS A FREE DEVICE MAY BE HOLDING: none, the user's 0.8 from a Swing, full, and what a full-depth route
	// on Amount delivers either way (`withRouteOffset`, the one resolve every host uses — the route's only road
	// into the device is this field).
	const std::array<float, 5> amounts { 0.0f, 0.8f, 1.0f,
		                                 static_cast<float>(dv::withRouteOffset(amountDesc, 0.8, 1.0)),
		                                 static_cast<float>(dv::withRouteOffset(amountDesc, 0.8, -1.0)) };

	SUBCASE("in Free, the warp and the band's reading are Free's at the phase, whatever the amount") {
		for (const GrooveMode free : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
			const int grid = grooveGridOf(free);
			for (const float a : amounts) {
				CAPTURE(static_cast<int>(free));
				CAPTURE(a);
				Settings cfg = looper(Mode::Shuffle, 0.5f);
				cfg.grooveMode = static_cast<float>(free);
				cfg.grooveAmount = a;
				cfg.freePhase = kPhase;
				const GrooveFeel feel = cfg.grooveFeel();
				CHECK(feel.mode == free);
				CHECK(feel.amount == static_cast<double>(kPhase));
				for (int j = 0; j <= grid; ++j) {
					CAPTURE(j);
					CHECK(std::fabs(grooveStepOffset(feel, j) - refFreeSteps(grid, static_cast<double>(kPhase), j)) <= 1e-12);
				}
				// THE READING THE BAND DRAWS FROM.
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTarget(cfg);
				std::vector<float> published;
				s.setReadingsSink(dv::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
					                                     static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
				                                     } });
				s.publishReadings();
				CHECK(published[static_cast<size_t>(Reading::Groove)] == static_cast<float>(free));
				CHECK(published[static_cast<size_t>(Reading::GrooveAmount)] == kPhase);
			}
		}
	}

	SUBCASE("in Swing and Groove, the warp and the reading are the depth's, whatever the phase") {
		for (const GrooveMode depth : { GrooveMode::Swing16, GrooveMode::Groove8 }) {
			const int grid = grooveGridOf(depth);
			for (const float phase : { 0.0f, 2.3f, -3.2f, 137.25f }) {
				CAPTURE(static_cast<int>(depth));
				CAPTURE(phase);
				Settings cfg = looper(Mode::Shuffle, 0.5f);
				cfg.grooveMode = static_cast<float>(depth);
				cfg.grooveAmount = 0.8f;
				cfg.freePhase = phase;
				const GrooveFeel feel = cfg.grooveFeel();
				CHECK(feel.amount == static_cast<double>(0.8f));
				for (int j = 0; j <= grid; ++j) {
					CAPTURE(j);
					CHECK(grooveStepOffset(feel, j)
					      == doctest::Approx(refOffsetSteps(grid, isSwing(depth), static_cast<double>(0.8f), j)).epsilon(1e-9));
				}
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTarget(cfg);
				std::vector<float> published;
				s.setReadingsSink(dv::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
					                                     static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
				                                     } });
				s.publishReadings();
				CHECK(published[static_cast<size_t>(Reading::GrooveAmount)] == 0.8f);
			}
		}
	}

	SUBCASE("SWING 16 AT 0.8, SWITCHED TO FREE 16 WHILE PLAYING: the slice starts are Free's alone, to the frame") {
		Settings swing = looper(Mode::Shuffle, 0.5f);
		swing.grooveMode = static_cast<float>(GrooveMode::Swing16);
		swing.grooveAmount = 0.8f;
		swing.freePhase = 0.0f;
		std::vector<std::vector<SlotStart>> runs;
		for (const float a : { 0.8f, 0.0f }) {
			CAPTURE(a);
			Settings free = swing;
			free.grooveMode = static_cast<float>(GrooveMode::Free16);
			free.grooveAmount = a; // 0.8: the swing left where it was; 0: the reference that cannot leak
			free.freePhase = kPhase;
			const std::vector<SlotStart> starts = slotStartsAfterSwitch(swing, free, 200, 600);
			REQUIRE(starts.size() >= 16u);
			for (const SlotStart& st : starts) {
				CAPTURE(st.slice);
				const long long want = static_cast<long long>(st.slice) * 6000
				                       + std::llround(refFreeSteps(16, static_cast<double>(kPhase), st.slice) * 6000.0);
				CHECK(st.barFrame == want);
				CHECK(static_cast<long long>(st.deviceStart) == want);
			}
			runs.push_back(starts);
		}
		REQUIRE(runs.size() == 2u);
		REQUIRE(runs[0].size() == runs[1].size());
		for (size_t i = 0; i < runs[0].size(); ++i) {
			CHECK(runs[0][i].barFrame == runs[1][i].barFrame);
		}
		// NOT VACUOUS: Swing 16 at 0.8 would put slice 1 at 6000 + 2400, Free at the phase somewhere else.
		CHECK(std::llround(refOffsetSteps(16, true, 0.8, 1) * 6000.0)
		      != std::llround(refFreeSteps(16, static_cast<double>(kPhase), 1) * 6000.0));
	}

	SUBCASE("FREE 16 AT 137.25, SWITCHED TO SWING 16 AT 0.8: the slice starts are the swing's alone") {
		Settings free = looper(Mode::Shuffle, 0.5f);
		free.grooveMode = static_cast<float>(GrooveMode::Free16);
		free.grooveAmount = 0.8f;
		free.freePhase = 137.25f;
		Settings swing = free;
		swing.grooveMode = static_cast<float>(GrooveMode::Swing16);
		const std::vector<SlotStart> starts = slotStartsAfterSwitch(free, swing, 200, 600);
		REQUIRE(starts.size() >= 16u);
		for (const SlotStart& st : starts) {
			CAPTURE(st.slice);
			const long long want = static_cast<long long>(st.slice) * 6000
			                       + std::llround(refOffsetSteps(16, true, static_cast<double>(0.8f), st.slice) * 6000.0);
			CHECK(st.barFrame == want);
			CHECK(static_cast<long long>(st.deviceStart) == want);
		}
	}
}

TEST_CASE("backlog IO: a route on Groove Amount moves the amount across its whole range and cannot reach the mode") {
	namespace dv = feathers::device;
	const dv::ParamDesc& amount = kDescriptor.params[static_cast<size_t>(Param::GrooveAmount)];
	const dv::ParamDesc& mode = kDescriptor.params[static_cast<size_t>(Param::GrooveMode)];
	REQUIRE(amount.modulatable);
	REQUIRE_FALSE(mode.modulatable); // no route can be drawn to the mode at all
	CHECK(dv::routeSpanOf(amount) == 1.0);
	// A full-depth swing past either end stops at the end: the amount of THIS mode, never another's.
	CHECK(dv::withRouteOffset(amount, 0.6, 2.0) == 1.0);
	CHECK(dv::withRouteOffset(amount, 0.6, -2.0) == 0.0);
	CHECK(dv::withRouteOffset(amount, 0.6, 0.25) == doctest::Approx(0.85));
	// And the engine decodes the mode from its own column, whatever the amount column holds.
	Settings base;
	base.mode = Mode::Shuffle;
	Row row = rowOf(base);
	row[static_cast<size_t>(Param::GrooveMode)] = static_cast<float>(GrooveMode::Swing8);
	for (const float a : { 0.0f, 0.5f, 1.0f, 1.9f, -0.4f }) {
		CAPTURE(a);
		row[static_cast<size_t>(Param::GrooveAmount)] = a;
		CHECK(stateOf(row).grooveFeel().mode == GrooveMode::Swing8);
		// THE ROW ROUND-TRIPS BOTH COLUMNS BIT FOR BIT — the mode and the amount are separate numbers.
		CHECK(rowOf(stateOf(row))[static_cast<size_t>(Param::GrooveAmount)] == a);
		CHECK(rowOf(stateOf(row))[static_cast<size_t>(Param::GrooveMode)] == static_cast<float>(GrooveMode::Swing8));
	}
	// SWING AND GROOVE STILL CLAMP AT 1: an amount stored past it plays as full.
	row[static_cast<size_t>(Param::GrooveAmount)] = 7.0f;
	CHECK(stateOf(row).grooveFeel().amount == 1.0);
	row[static_cast<size_t>(Param::GrooveMode)] = static_cast<float>(GrooveMode::Groove16);
	CHECK(stateOf(row).grooveFeel().amount == 1.0);
}

TEST_CASE("backlog IO: a route on Free Phase pushes it past 1 and keeps going — and the walk it reaches is played there") {
	// In Free the phase does not wrap and may go arbitrarily high. Free Phase is Zone A's kind of
	// control: a drag window with offsets that may leave it, moved one unit per full-depth route.
	namespace dv = feathers::device;
	const dv::ParamDesc& phase = kDescriptor.params[static_cast<size_t>(Param::FreePhase)];
	REQUIRE(phase.modulatable);
	REQUIRE(phase.offsetsMayLeaveRange);
	CHECK(phase.max >= 8388608.0);
	CHECK(dv::routeSpanOf(phase) == 1.0); // `kOnePhaseUnit`, Zone A's span
	// A route at full depth, full source, from 0.6: 1.6. Five of them stacked: 5.6. Far out: no ceiling near.
	CHECK(dv::withRouteOffset(phase, 0.6, 1.0 * dv::routeSpanOf(phase)) == doctest::Approx(1.6));
	CHECK(dv::withRouteOffset(phase, 0.6, 5.0 * dv::routeSpanOf(phase)) == doctest::Approx(5.6));
	CHECK(dv::withRouteOffset(phase, 1234.5, 3.0) == doctest::Approx(1237.5));
	// AND THE DEVICE PLAYS THE PHASE IT IS GIVEN, UNCLAMPED: the row's value past 1 reaches the feel, and the
	// offsets there are the walk's own at that position, restated.
	Settings base;
	base.mode = Mode::Shuffle;
	Row row = rowOf(base);
	row[static_cast<size_t>(Param::GrooveMode)] = static_cast<float>(GrooveMode::Free16);
	for (const float at : { 1.6f, 5.6f, 1237.5f }) {
		CAPTURE(at);
		row[static_cast<size_t>(Param::FreePhase)] = at;
		const GrooveFeel feel = stateOf(row).grooveFeel();
		CHECK(feel.amount == static_cast<double>(at));
		for (int j = 1; j < 16; ++j) {
			CAPTURE(j);
			CHECK(std::fabs(grooveStepOffset(feel, j) - refFreeSteps(16, static_cast<double>(at), j)) <= 1e-12);
		}
	}
	// A ROUTE ON GROOVE AMOUNT DOES NOT CARRY INTO FREE: in Free the amount column is not read at all.
	row[static_cast<size_t>(Param::FreePhase)] = 3.0f;
	row[static_cast<size_t>(Param::GrooveAmount)] = 0.9f;
	const double withAmount = stateOf(row).grooveFeel().amount;
	row[static_cast<size_t>(Param::GrooveAmount)] = 0.1f;
	CHECK(stateOf(row).grooveFeel().amount == withAmount);
	CHECK(withAmount == 3.0);
}

TEST_CASE("backlog IG: with groove ON, Transport Locked On == Off on an unbroken transport — a hair either side of a warped slot end") {
	// **THE GT/GU PAIR'S CASE, WITH THE GRID WARPED.** The free walk ends each swung slot on the warp's
	// boundary; the lock derives the slice by inverting it. On a transport that never stops the two must
	// name the same slice at every slot end and render the same samples — on the four transports `a hair
	// either side` uses, including the accumulated one that found the truncation defect.
	namespace sc = feathers::fx::sklitter;
	constexpr int kBlock = 960;
	constexpr int kBlocks = 400;
	const Mode sliceModes[] { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch };
	struct Feel {
		int zone; ///< the `GrooveMode` — "zone" from before backlog IO split it out
		double amount;
	};
	// FREE 8 ADDED BY BACKLOG IO: the lock inverts the same warp whatever computes the offsets.
	const Feel feels[] { { 3, 2.0 / 3.0 }, { 5, 0.99 }, { 1, 0.9 }, { 6, 0.6 }, { 8, 0.3 } };
	int slotEndsChecked = 0;
	for (const Mode m : sliceModes) {
		for (const Feel feel : feels) {
			for (const int transport : { 0, 1, 2, 3 }) {
				const std::string modeName = sc::kModeNames[static_cast<int>(m)];
				CAPTURE(modeName);
				CAPTURE(feel.zone);
				CAPTURE(transport);
				Settings cfg = looper(m, 0.5f);
				withMacro(cfg, 0.5f, 0.4f);
				cfg.engage = true;
				setGroove(cfg, feel.zone, feel.amount);
				Settings locked = cfg;
				locked.transportLocked = true;
				Sklitter a;
				Sklitter b;
				for (Sklitter* s : { &a, &b }) {
					s->prepare(kRate, 4096);
					s->setTransportClock(at120bpm());
				}
				a.setTarget(cfg);
				b.setTarget(locked);
				const int grid = grooveGridOf(static_cast<GrooveMode>(feel.zone));
				const bool swing = feel.zone <= 3;
				const bool free = feel.zone >= 7;
				const double amount = storedAmount(feel.amount);
				std::vector<float> la(kBlock);
				std::vector<float> ra(kBlock);
				std::vector<float> lb(kBlock);
				std::vector<float> rb(kBlock);
				size_t n = 0;
				bool wasPlaying = false;
				size_t differing = 0;
				double accumulated = 0.0;
				for (int block = 0; block < kBlocks; ++block) {
					for (int i = 0; i < kBlock; ++i, ++n) {
						const double ph = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(n) / kRate;
						const double ph2 = 2.0 * 3.14159265358979 * 331.0 * static_cast<double>(n) / kRate;
						la[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph) + 0.3 * std::sin(ph2));
						ra[static_cast<size_t>(i)] = static_cast<float>(0.6 * std::sin(ph2) + 0.3 * std::sin(ph));
					}
					lb = la;
					rb = ra;
					const double exact = static_cast<double>(block * kBlock) / (0.5 * kRate);
					const double beats = block == 0      ? 0.0
					                     : transport == 1 ? accumulated
					                     : transport == 2 ? exact - 1e-9
					                     : transport == 3 ? exact + 1e-9
					                                      : exact;
					accumulated += static_cast<double>(kBlock) / (kRate * 0.5);
					a.setTransportClock(at120bpm(beats));
					b.setTransportClock(at120bpm(beats));
					a.processStereo(la.data(), ra.data(), kBlock);
					b.processStereo(lb.data(), rb.data(), kBlock);
					for (int i = 0; i < kBlock; ++i) {
						if (la[static_cast<size_t>(i)] != lb[static_cast<size_t>(i)]
						    || ra[static_cast<size_t>(i)] != rb[static_cast<size_t>(i)]) {
							++differing;
						}
					}
					const bool playing = b.statusForTest() == Status::Playing;
					if (wasPlaying && playing && b.positionInSliceForTest() < static_cast<size_t>(kBlock)) {
						const auto bar = static_cast<long long>(b.sliceFramesForTest());
						const int32_t count = b.sliceCountForTest();
						REQUIRE(bar > 0);
						REQUIRE(count > 0);
						const long long songFrame = static_cast<long long>(block) * kBlock + kBlock
						                            - static_cast<long long>(b.positionInSliceForTest());
						// THE REFERENCE: the last warped boundary at or before the counted frame.
						const long long at = songFrame % bar;
						int32_t want = 0;
						for (int32_t k = 0; k < count; ++k) {
							if (refBoundary(grid, swing, amount, k, count, bar, free) <= at) {
								want = k;
							}
						}
						CHECK(b.sliceIndexForTest() == want);
						CHECK(a.sliceIndexForTest() == b.sliceIndexForTest());
						CHECK(b.slotStartFrameForTest() == a.slotStartFrameForTest());
						++slotEndsChecked;
					}
					wasPlaying = playing;
				}
				CHECK(differing == 0);
			}
		}
	}
	CHECK(slotEndsChecked > 80 * 20);
}

TEST_CASE("backlog IG: Straight is the zero change — at any amount, and any groove in a mode it is gated off in") {
	// **THE ENGINE TAKES ITS ORIGINAL ARITHMETIC FOR AN INACTIVE FEEL** (`applyGrooveToSlot` returns
	// before touching the slot), so these renders are the pre-groove device's own. Asserted against a
	// device with Groove at 0 — the default every row before this control read as — bit for bit.
	constexpr int kBlock = 512;
	const auto render = [](const Settings& cfg) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		std::vector<float> out;
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		size_t n = 0;
		for (int block = 0; block < 500; ++block) {
			for (int i = 0; i < kBlock; ++i, ++n) {
				l[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.013 * static_cast<double>(n)));
				r[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.021 * static_cast<double>(n)));
			}
			s.setTransportClock(at120bpm(static_cast<double>(block * kBlock) / (0.5 * kRate)));
			s.processStereo(l.data(), r.data(), kBlock);
			out.insert(out.end(), l.begin(), l.end());
			out.insert(out.end(), r.begin(), r.end());
		}
		return out;
	};
	for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
		const auto mode = static_cast<Mode>(m);
		CAPTURE(m);
		Settings base = looper(mode, 0.5f);
		withMacro(base, 0.5f, 0.4f);
		const std::vector<float> reference = render(base);
		Settings straight = base;
		setGroove(straight, 0, 0.9); // Straight, with an amount it has nothing to be an amount of
		CHECK(render(straight) == reference);
		const bool gatedOff = mode == Mode::Burst || mode == Mode::Grain;
		Settings grooved = base;
		setGroove(grooved, 5, 0.9);
		// GATED OFF: bit-identical. GATED ON: it must move, or the gate above is declaring a dead control.
		CHECK((render(grooved) == reference) == gatedOff);
		// FREE (backlog IO) AT PHASE 0 IS STRAIGHT, bit for bit, in every mode (phase 0 is straight by
		// design); along the walk it moves wherever it is gated on. 2.5, not a small phase: near 0 only the
		// eighth-grid's off-beats move, and Repeat in this row starts no slot near one — MEASURED: at 0.4
		// Repeat rendered bit-identical to Straight. At 2.5 step 2 has risen too.
		Settings free = base;
		setGroove(free, static_cast<int>(GrooveMode::Free8), 0.0);
		CHECK(render(free) == reference);
		REQUIRE(grooveStepOffset({ GrooveMode::Free8, 2.5 }, 2) > 0.0);
		setGroove(free, static_cast<int>(GrooveMode::Free8), 2.5);
		CHECK((render(free) == reference) == gatedOff);
	}
}

TEST_CASE("backlog IG: MEASURE — Groove modulated mid-bar keeps the bar anchored, and the seam it leaves") {
	// **THE WARP IS LATCHED PER SLOT, AND A SLOT ENDS ON THE CURRENT WARP'S ABSOLUTE BOUNDARY.** So
	// turning Groove while a bar plays — by hand or by a route — cannot slip the loop against the song:
	// every bar top must still land on a song downbeat. Unlocked, because that is the setting where a
	// walk that summed lengths would drift for ever and nothing would re-derive it.
	//
	// **AND THE SEAM, MEASURED AGAINST THE SAME DEVICE UNMODULATED.** A changed warp can only take effect
	// at a slot boundary (the length is settled when the slot starts), so what a route can add is a slot
	// boundary somewhere other than where a fixed feel would put it — and every slot boundary already has
	// the device's fades. The number reported is the worst sample-to-sample step and the count of steps
	// over 0.1, across three runs on the same input: Straight, Groove 8 held at 50%, and the sawtooth.
	constexpr int kBlock = 480;
	struct Seam {
		int barTops = 0;
		int slotStarts = 0;
		float worstStep = 0.0f;
		int bigSteps = 0;
	};
	const auto run = [](int which) {
		Seam out;
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		size_t n = 0;
		bool wasPlaying = false;
		float lastOut = 0.0f;
		for (int block = 0; block < 4000; ++block) {
			Settings moving = cfg;
			// 0 Straight; 1 Groove 8 at 50%; 2 Free 16 with its amount a slow sawtooth, every block — the
			// walk through the φ patterns under a route (backlog IO; this swept the seven zones before the split).
			const double saw = std::fmod(static_cast<double>(block) / 700.0, 1.0);
			setGroove(moving, which == 0 ? 0 : which == 1 ? 5 : static_cast<int>(GrooveMode::Free16),
			          which == 0 ? 0.0 : which == 1 ? 0.5 : saw);
			s.setTarget(moving);
			for (int i = 0; i < kBlock; ++i, ++n) {
				l[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.013 * static_cast<double>(n)));
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			s.setTransportClock(at120bpm(static_cast<double>(block * kBlock) / (0.5 * kRate)));
			const int32_t sliceBefore = s.sliceIndexForTest();
			s.processStereo(l.data(), r.data(), kBlock);
			const bool playing = s.statusForTest() == Status::Playing;
			if (wasPlaying && playing) {
				for (int i = 0; i < kBlock; ++i) {
					const float prev = i == 0 ? lastOut : l[static_cast<size_t>(i - 1)];
					const float step = std::abs(l[static_cast<size_t>(i)] - prev);
					out.worstStep = std::max(out.worstStep, step);
					out.bigSteps += step > 0.1f ? 1 : 0;
				}
				if (s.sliceIndexForTest() != sliceBefore) {
					++out.slotStarts;
				}
				if (s.sliceIndexForTest() == 0 && sliceBefore != 0) {
					const long long songFrame = static_cast<long long>(block) * kBlock + kBlock
					                            - static_cast<long long>(s.positionInSliceForTest());
					CHECK(songFrame % 96000 == 0); // THE BAR TOP IS A SONG DOWNBEAT, EVERY BAR
					++out.barTops;
				}
			}
			lastOut = l[static_cast<size_t>(kBlock - 1)];
			wasPlaying = playing;
		}
		return out;
	};
	const Seam straight = run(0);
	const Seam held = run(1);
	const Seam swept = run(2);
	for (const Seam* seam : { &straight, &held, &swept }) {
		CHECK(seam->barTops >= 17);
		CHECK(seam->slotStarts > 200);
	}
	MESSAGE("seam, worst step / steps over 0.1 / bar tops — Straight " << straight.worstStep << " / "
	        << straight.bigSteps << " / " << straight.barTops << "; Groove 8 held " << held.worstStep
	        << " / " << held.bigSteps << " / " << held.barTops << "; swept " << swept.worstStep << " / "
	        << swept.bigSteps << " / " << swept.barTops << " (the input's own step is <= 0.0065)");
}

namespace {

/// **EVERY INPUT FRAME NAMES ITSELF**, so a frame read back from the ring or the output says which song
/// frame it was recorded at. Offset so no frame is zero (the reset ring reads 0), and the sign flips
/// each frame so the input crosses zero every sample and no crossing gate waits on it. Exact in a float
/// while `frame < 2^24 - 10^6`, which every run below stays inside.
constexpr float kFrameBase = 1000000.0f;
float frameCode(long long frame) {
	return (kFrameBase + static_cast<float>(frame)) * ((frame & 1) != 0 ? -1.0f : 1.0f);
}
/// The song frame a sample carries, or −1 when it is not a whole frame code — a sample under a fade or
/// a crossfade is a code times a gain, and says nothing about WHERE it came from.
long long frameOf(float sample) {
	const double a = std::fabs(static_cast<double>(sample));
	if (std::fabs(a - std::round(a)) > 0.01) {
		return -1;
	}
	const long long src = std::llround(a) - static_cast<long long>(kFrameBase);
	return src >= 0 ? src : -1;
}

/// **THE DEVICE AS A HOST DRIVES IT, AT THE DESCRIPTOR'S DEFAULTS.** The song frame is an integer count,
/// and each block's position is derived from it, as a host's is (`WriteHeadRig`'s reason).
struct HostRig {
	Sklitter s;
	double sampleRate = kRate;
	double secondsPerBeat = 0.5;
	double bufferBars = 1.0;
	long long frame = 0; ///< song frames rolled so far
	std::vector<int> blocks;
	size_t nextBlock = 0;

	[[nodiscard]] double framesPerBeat() const { return secondsPerBeat * sampleRate; }
	void run(long long frames, bool rolling) {
		const long long end = frame + frames;
		std::vector<float> l;
		std::vector<float> r;
		long long parked = 0; // a stopped host's input still arrives; the song does not move
		while ((rolling ? frame : parked) < (rolling ? end : frames)) {
			const int n = blocks[nextBlock++ % blocks.size()];
			l.resize(static_cast<size_t>(n));
			r.resize(static_cast<size_t>(n));
			for (int k = 0; k < n; ++k) {
				l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] = rolling ? frameCode(frame + k) : 0.3f;
			}
			TransportClock c = TransportClock::fromSecondsPerBeat(secondsPerBeat);
			c.songPositionBeats = static_cast<double>(frame) / framesPerBeat();
			c.playing = rolling;
			s.setTransportClock(c);
			s.processStereo(l.data(), r.data(), n);
			if (rolling) {
				out(l, frame);
				frame += n;
			}
			else {
				parked += n;
			}
		}
	}
	/// Output frames whose code is whole, and how many of those sit on the song's own phase of the bar.
	long long wholeCodes = 0;
	long long onPhase = 0;
	bool listening = false;
	void out(const std::vector<float>& l, long long at) {
		if (!listening) {
			return;
		}
		// **MOD THE BAR, NOT THE BUFFER.** Which bar of a longer buffer plays under which song bar is the
		// arrangement's (`planFor`, Seq Length) — MEASURED at two bars, 37% of frames on the song's bar
		// of the two in every setting, fix or no fix. Where in the bar is the claim; the ring's value
		// check owns which bar.
		const auto bar = static_cast<long long>(s.bufferFramesForTest())
		                 / static_cast<long long>(std::max(1.0, bufferBars));
		for (size_t k = 0; k < l.size(); ++k) {
			const long long src = frameOf(l[k]);
			const long long f = at + static_cast<long long>(k);
			if (src < 0 || src == f) { // a fade, or the dry input itself
				continue;
			}
			++wholeCodes;
			onPhase += (((f - src) % bar) + bar) % bar == 0 ? 1 : 0;
		}
	}
	/// **RING INDEX i HOLDS SONG PHASE i**, read off the ring by value at `probes` evenly spaced indices
	/// including both ends. Returns how many are off, and the first offender's phase error.
	/// `margin` frames either side of the append head are skipped: after a trigger they are under the
	/// capture's seam fade (`Ring::fadeSeamAt`, which is where the capture ended — not index 0).
	int ringOffPhase(long long* firstError, long long margin = 0) const {
		const Ring& ring = s.ringForTest();
		const auto len = static_cast<long long>(ring.length());
		const double barExact = 4.0 * framesPerBeat();
		// **RING FRAME 0 IS ON THE ANCHOR, A SONG DOWNBEAT** (`lockedWriteHead`'s contract), and for a
		// buffer longer than a bar the anchor is what says WHICH bar is first. The caller asserts it is
		// stamped and on a downbeat; here it is only used.
		const double anchorFrames = s.prewarmBeatForTest() == INT64_MIN
		                                ? 0.0
		                                : static_cast<double>(s.prewarmBeatForTest()) * framesPerBeat();
		const double bufferExact = barExact * bufferBars;
		int off = 0;
		const auto seam = static_cast<long long>(ring.writePos());
		for (long long i = 0; i < len; i += len / 97) {
			for (const long long at : { i, len - 1 - i }) {
				const long long fromSeam = ((at - seam) % len + len) % len;
				if (fromSeam < margin || fromSeam >= len - margin) {
					continue;
				}
				float l = 0.0f;
				float r = 0.0f;
				ring.readAt(static_cast<size_t>(at), &l, &r);
				const long long src = frameOf(l);
				// THE PLATFORM'S ARITHMETIC: the song frame's phase in the buffer from the anchor, scaled
				// to the ring's whole frames — `lockedWriteHead`'s contract restated, not called.
				const double phase = (static_cast<double>(src) - anchorFrames) / bufferExact;
				long long want = std::llround((phase - std::floor(phase)) * static_cast<double>(len)) % len;
				long long err = at - want;
				err -= len * static_cast<long long>(std::llround(static_cast<double>(err) / static_cast<double>(len)));
				if (src < 0 || std::llabs(err) > (barExact == std::floor(barExact) ? 0 : 1)) {
					if (off++ == 0) {
						*firstError = src < 0 ? LLONG_MIN : err;
					}
				}
			}
		}
		return off;
	}
};

Settings defaultSettings() {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	return stateOf(row);
}

} // namespace

TEST_CASE("backlog IN: at DEFAULT settings the loop plays on the song's grid — any host block, any pre-roll, either lock setting") {
	// **A CAPTURED LOOP DID NOT PLAY STRAIGHT OVER ITSELF AT DEFAULT SETTINGS**, where playback should be
	// straight and unaltered. The defaults DO play straight (Shuffle at Zone 0 walks the slices in order, P Write 0 freezes the bar);
	// what did not was WHICH song frame each ring index held. MEASURED before the fix, output against
	// input as a frame offset, the same in both lock settings:
	//   - the anchor, block-granular: 0 at 480-frame blocks (which divide the 120 BPM bar — every case
	//     in this file drove 480), 256 at 512 and 1024, 138 at 441, 120/376/888 at 44.1 kHz. EARLY.
	//   - the pre-roll: every frame the device heard before the transport rolled, mod the bar, LATE — 7777
	//     for 7777 here, and on the rack's CLAP road exactly the pre-roll block (1024, 2048, 4096).
	// **ASSERTED ON THE RING, BY VALUE**, where the question lives; the output is the second assertion,
	// proving the reader plays what the ring holds where the song is.
	struct Host {
		const char* what;
		double rate;
		std::vector<int> blocks;
	};
	const Host hosts[] = {
		{ "48k/512", 48000.0, { 512 } },  { "48k/1024", 48000.0, { 1024 } }, { "48k/256", 48000.0, { 256 } },
		{ "48k/441", 48000.0, { 441 } },  { "48k/irregular", 48000.0, { 256, 1024, 37, 512, 700, 1 } },
		{ "44.1k/512", 44100.0, { 512 } }, { "44.1k/1024", 44100.0, { 1024 } },
		{ "44.1k/irregular", 44100.0, { 256, 1024, 37, 512, 700, 1 } },
	};
	// **AND A TWO-BAR BUFFER, WHICH IS NOT THE DEFAULT AND IS HERE FOR ONE LINE.** At a bar or less the
	// per-block seat alone puts every index on its phase — a known-bad deleting the re-anchor after a
	// pre-roll stayed GREEN at the default — because any downbeat is ring frame 0's downbeat. Past a bar
	// the anchor is what says which bar comes first, and without it the loop plays a bar rotated.
	for (const double bufferBars : { 1.0, 2.0 }) {
	for (const Host& host : hosts) {
		for (const long long preRoll : { 0LL, 7777LL }) {
			for (const bool locked : { false, true }) {
				CAPTURE(bufferBars);
				const std::string what = host.what;
				CAPTURE(what);
				CAPTURE(preRoll);
				CAPTURE(locked);
				HostRig rig;
				rig.sampleRate = host.rate;
				rig.blocks = host.blocks;
				rig.bufferBars = bufferBars;
				rig.s.prepare(host.rate, 4096);
				Settings cfg = defaultSettings();
				REQUIRE(cfg.mode == Mode::Shuffle); // the default population
				REQUIRE(cfg.pWrite == 0.0f);
				REQUIRE(cfg.bufferBars == 1.0);
				cfg.bufferBars = bufferBars;
				cfg.transportLocked = locked;
				cfg.engage = false;
				rig.s.setTarget(cfg);
				rig.run(preRoll, false);
				const auto bar = std::llround(4.0 * rig.framesPerBeat());
				// listening, off the grid: beat 9.5 plus a buffer's worth
				rig.run(bar * 9 / 4 + 3 * bar / 8 + bar * static_cast<long long>(bufferBars - 1.0), true);

				REQUIRE(rig.s.prewarmReadyForTest());
				CHECK(rig.s.prewarmBeatForTest() != INT64_MIN);
				CHECK(rig.s.prewarmBeatForTest() % 4 == 0);
				long long firstError = 0;
				CHECK(rig.ringOffPhase(&firstError) == 0);
				CAPTURE(firstError);

				cfg.engage = true;
				rig.s.setTarget(cfg);
				rig.run(bar / 2, true);
				REQUIRE(rig.s.statusForTest() == Status::Playing);
				rig.listening = true;
				rig.run(bar * 3, true);
				// **NOT 100%, AND THE REMAINDER IS NOT AN OFFSET.** A frame under a gain that lands on a
				// whole number decodes as a code from somewhere else — MEASURED at ~0.15%, in steps of
				// ~593 frames of "offset", which is a fade's gain step rather than a position.
				CAPTURE(rig.onPhase);
				CAPTURE(rig.wholeCodes);
				REQUIRE(rig.wholeCodes > bar * 5 / 2);
				CHECK(static_cast<double>(rig.onPhase) >= 0.995 * static_cast<double>(rig.wholeCodes));

				// **AND AGAIN AFTER A RELEASE** — the idle ring keeps appending from wherever the gesture
				// left its head, so the second press is the one a player makes.
				cfg.engage = false;
				rig.s.setTarget(cfg);
				rig.listening = false;
				rig.run(bar * static_cast<long long>(bufferBars) + bar / 3, true);
				firstError = 0;
				CHECK(rig.ringOffPhase(&firstError) == 0);
			}
		}
	}
	}

	// **AND A PRESS BEFORE THE RING IS WARM** — Play, then Engage inside the first bar, which is the
	// quickest way to "capture a loop". The press finds too little audio, resets the ring and records
	// a buffer from the press, and that capture is appended from wherever its first frame lands; the
	// heads it is read by are sought to the song (GU). So its first frame must go to the song's phase.
	for (const Host& host : hosts) {
		for (const bool locked : { false, true }) {
			const std::string what = host.what;
			CAPTURE(what);
			CAPTURE(locked);
			HostRig rig;
			rig.sampleRate = host.rate;
			rig.blocks = host.blocks;
			rig.s.prepare(host.rate, 4096);
			Settings cfg = defaultSettings();
			cfg.transportLocked = locked;
			cfg.engage = false;
			rig.s.setTarget(cfg);
			const auto bar = std::llround(4.0 * rig.framesPerBeat());
			rig.run(bar * 537 / 400, true); // beat 5.37: anchored on 4, a bar short of warm
			REQUIRE_FALSE(rig.s.prewarmReadyForTest());
			cfg.engage = true;
			rig.s.setTarget(cfg);
			for (int k = 0; k < 4000 && rig.s.statusForTest() != Status::Playing; ++k) {
				rig.run(1, true);
			}
			REQUIRE(rig.s.statusForTest() == Status::Playing);
			rig.run(bar / 4, true);
			rig.listening = true;
			rig.run(bar * 3, true);
			CAPTURE(rig.onPhase);
			CAPTURE(rig.wholeCodes);
			REQUIRE(rig.wholeCodes > bar * 5 / 2);
			CHECK(static_cast<double>(rig.onPhase) >= 0.995 * static_cast<double>(rig.wholeCodes));
			long long firstError = 0;
			CHECK(rig.ringOffPhase(&firstError, bar / 100) == 0);
			CAPTURE(firstError);
		}
	}
}

TEST_CASE("backlog IN addendum: a swung hit plays ONCE, late, on a floor-ramped attack — the slot before it is silent past its own slice") {
	// **THE SWING SETTINGS FELT WRONG ON AN INSTALLED BUILD.** MEASURED at Swing 16
	// 66%, a self-naming click of every frame through the default arrangement: the off-beat slot WAS
	// delayed correctly (0.32 of a sixteenth), but the lengthened on-beat slot before it read straight on
	// into the off-beat's own material — so every off-beat hit sounded at its UNSWUNG time at full level
	// for ~1200 frames, then 720 frames of release mute, then again at its swung time behind a 478-frame
	// fade. A flam, not a swing. MPC/REX play a swung hit once, late, out of silence.
	//
	// **ASSERTED BY VALUE, PER SLOT, AGAINST THE WARP'S BOUNDARIES.** Swing's are restated here
	// independently (the off-beat sixteenth at 66% of its pair); Groove 8's are the phi template's and
	// are taken from `grooveBoundary`, which is that template's one statement.
	struct Feel {
		const char* what;
		GrooveMode mode;
		float amount; ///< Swing 16 at 66%: 0.64 of the MPC's 50 → 75% travel
		bool swing16;
	};
	const Feel feels[] = { { "Swing 16 66%", GrooveMode::Swing16, 0.64f, true },
	                       { "Groove 8 50%", GrooveMode::Groove8, 0.5f, false } };
	struct Host {
		const char* what;
		double rate;
		int block;
	};
	const Host hosts[] = { { "48k/512", 48000.0, 512 }, { "44.1k/1024", 44100.0, 1024 } };
	constexpr int32_t kSlices = 16; // Rate 0.5 buys sixteen — MEASURED, s0..s15 of 6000 frames at 48k
	for (const Feel& feel : feels) {
		for (const Host& host : hosts) {
			for (const bool locked : { false, true }) {
				const std::string what = std::string(feel.what) + " " + host.what;
				CAPTURE(what);
				CAPTURE(locked);
				Sklitter s;
				s.prepare(host.rate, 4096);
				Settings cfg = defaultSettings();
				setGroove(cfg, static_cast<int>(feel.mode), feel.amount);
				cfg.transportLocked = locked;
				cfg.engage = false;
				s.setTarget(cfg);
				const double fpb = 0.5 * host.rate;
				const auto bar = static_cast<size_t>(std::llround(4.0 * fpb));
				const auto base = bar / static_cast<size_t>(kSlices);
				const GrooveFeel gf = cfg.grooveFeel();
				REQUIRE(grooveActive(gf));

				std::vector<size_t> edge(static_cast<size_t>(kSlices) + 1);
				for (int32_t k = 0; k <= kSlices; ++k) {
					edge[static_cast<size_t>(k)] = grooveBoundary(gf, k, kSlices, bar);
				}
				if (feel.swing16) {
					// MPC SWING, RESTATED: every odd sixteenth late by (2 x 66% - 1) of a sixteenth.
					const auto late = static_cast<size_t>(std::llround(0.32 * static_cast<double>(bar) / kSlices));
					for (int32_t k = 0; k < kSlices; ++k) {
						CAPTURE(k);
						CHECK(edge[static_cast<size_t>(k)] == static_cast<size_t>(k) * base + ((k % 2) != 0 ? late : 0));
					}
				}

				// Listen, press off the grid at beat 9.37, and keep bars 3 and 4 after the song's start of play.
				const long long captureFrom = static_cast<long long>(bar) * 4;
				const long long captureTo = static_cast<long long>(bar) * 6;
				std::vector<float> out(static_cast<size_t>(captureTo - captureFrom));
				std::vector<float> l(static_cast<size_t>(host.block));
				std::vector<float> r(static_cast<size_t>(host.block));
				for (long long frame = 0; frame < captureTo; frame += host.block) {
					if (!cfg.engage && static_cast<double>(frame) / fpb >= 9.37) {
						cfg.engage = true;
						s.setTarget(cfg);
					}
					for (int k = 0; k < host.block; ++k) {
						l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] = frameCode(frame + k);
					}
					TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
					c.songPositionBeats = static_cast<double>(frame) / fpb;
					c.playing = true;
					s.setTransportClock(c);
					s.processStereo(l.data(), r.data(), host.block);
					for (int k = 0; k < host.block; ++k) {
						const long long f = frame + k;
						if (f >= captureFrom && f < captureTo) {
							out[static_cast<size_t>(f - captureFrom)] = l[static_cast<size_t>(k)];
						}
					}
				}
				REQUIRE(s.statusForTest() == Status::Playing);

				long long checkedFrames = 0;
				long long wrongMaterial = 0; // a frame of some OTHER slice's material inside this slot
				long long gapFrames = 0;
				long long gapNotSilent = 0;
				long long delayedStarts = 0;
				long long dirtyStarts = 0; // a delayed start that is not its hit's first frame at full gain
				for (long long b = captureFrom; b < captureTo; b += static_cast<long long>(bar)) {
					for (int32_t k = 0; k < kSlices; ++k) {
						const size_t matStart = static_cast<size_t>(k) * base;
						const size_t matEnd = k == kSlices - 1 ? bar : matStart + base;
						const size_t slotStart = edge[static_cast<size_t>(k)];
						const size_t slotEnd = edge[static_cast<size_t>(k) + 1];
						for (size_t at = slotStart; at < slotEnd; ++at) {
							const float v = out[static_cast<size_t>(b - captureFrom) + at];
							const size_t sounding = std::min(slotEnd - slotStart, matEnd - matStart);
							if (at >= slotStart + sounding) {
								++gapFrames;
								gapNotSilent += v != 0.0f ? 1 : 0;
								continue;
							}
							const long long src = frameOf(v);
							// **A CODE COUNTS ONLY WITH A NEIGHBOUR THAT AGREES.** Under a fade a gained code
							// lands on a whole number now and then — MEASURED, isolated frames of random phase
							// inside the bar top's attack ramp — while a frame truly read is followed by its
							// successor.
							const size_t idx = static_cast<size_t>(b - captureFrom) + at;
							const bool agreed = src >= 0 && idx + 1 < out.size() && frameOf(out[idx + 1]) == src + 1;
							if (!agreed) {
								continue; // under a fade: says nothing about where it came from
							}
							++checkedFrames;
							const auto phase = static_cast<size_t>(src % static_cast<long long>(bar));
							wrongMaterial += (phase < matStart || phase >= matEnd) ? 1 : 0;
						}
						const bool delayed = slotStart > matStart;
						const bool afterLengthened =
						    k > 0 && slotStart - edge[static_cast<size_t>(k) - 1] >= base;
						if (delayed && afterLengthened) {
							++delayedStarts;
							// **OUT OF THE GAP ON THE FLOOR RAMP, THEN WHOLE (groove-click).** This read "the first 32
							// frames at full gain" until a sine showed what that is: a step from the gap's zero to
							// whatever the slice starts on. So the hit's first frame is the ramp's zero, and from
							// `kMinAntiClickSeconds` on (64 samples at 44.1 kHz, the host's rate rounded) the next 32
							// are whole codes, each its hit's own frame — the attack is 1.45 ms late to full, not 10.
							const auto gapRamp = static_cast<size_t>(std::llround(64.0 / 44100.0 * host.rate));
							dirtyStarts += out[static_cast<size_t>(b - captureFrom) + slotStart] == 0.0f ? 0 : 1;
							for (size_t j = gapRamp; j < gapRamp + 32; ++j) {
								const float v = out[static_cast<size_t>(b - captureFrom) + slotStart + j];
								const long long src = frameOf(v);
								const bool clean = src >= 0 && static_cast<size_t>(src % static_cast<long long>(bar)) == matStart + j;
								dirtyStarts += clean ? 0 : 1;
							}
						}
					}
				}
				CAPTURE(checkedFrames);
				CAPTURE(gapFrames);
				CAPTURE(delayedStarts);
				REQUIRE(checkedFrames > static_cast<long long>(bar)); // not vacuous
				REQUIRE(gapFrames > 0);                               // the groove did lengthen slots
				REQUIRE(delayedStarts >= 4);                          // and delayed hits after them
				CHECK(wrongMaterial == 0);
				CHECK(gapNotSilent == 0);
				CHECK(dirtyStarts == 0);
			}
		}
	}
}

namespace {

/// ONE RENDER OF MUSICAL MATERIAL through the defaults with a groove, pressed off the grid (groove-click).
struct ClickRun {
	std::vector<float> out;   ///< left output from `from`
	long long from = 0;       ///< song frame of out[0]
	long long pressAt = -1;   ///< song frame of the press
	size_t bar = 0;
	std::vector<float> ringL; ///< the ring at the end
	float inMaxStep = 0.0f;   ///< the input's own largest first difference
	size_t seam = SIZE_MAX;   ///< the ring's append head when play began
};

constexpr double kClickPeak = 0.5;

/// **BAR-PERIODIC, SO RING PHASE i HOLDS `clickMaterial(i)` AND THE RING'S OWN WRAP IS NO SEAM.** The sine is
/// 220.5 Hz — 441 cycles in the 120 BPM bar — so a sixteenth starts 0.5625 of a cycle on from the last and
/// slice starts are NOT zero crossings (220 Hz put every one of them on a zero at 48k, where a step from
/// silence measures nothing). The burst is a decaying 440 Hz tone per beat, starting at phase 0.
/// **A HIT THAT SAYS WHICH HIT IT IS, UNDER ANY GAIN.** Silence, except a two-frame doublet `kFlamOffset`
/// frames into every sixteenth: `a` then `a·r_k`, with the ratio naming the sixteenth mod 8 (×1.5 apart).
/// A linear ramp moves the ratio by (n+1)/n at frame n — 5% at `kFlamOffset` — and ramps STACK (a choke's
/// grain fade under the capture's seam fade is 10%), so the spacing is wide enough that three of them cannot
/// move an id to its neighbour: ×1.2 did, and read a stacked fade as a double. Every non-zero output pair is
/// then a copy of one identified hit at a readable gain — a flam is the same id twice.
constexpr long long kFlamOffset = 20;
constexpr int kFlamIds = 8;
constexpr double kFlamPeak = 0.3;
constexpr double kFlamStep = 1.5;
double flamRatio(int id) { return 0.15 * std::pow(kFlamStep, id); }
float flamDoublet(long long f, double rate) {
	const auto bar = std::llround(4.0 * 0.5 * rate);
	const long long base = bar / 16;
	const long long phase = f % bar;
	const long long k = std::min<long long>(phase / base, 15);
	const long long n = phase - k * base;
	// A CROSSING FIRST, so a zero-crossing gate armed at this slot opens before the hit rather than muting it
	// for want of one (real material crosses; a lone positive doublet never would).
	if (n == kFlamOffset - 10) {
		return 0.01f;
	}
	if (n == kFlamOffset - 9) {
		return -0.01f;
	}
	if (n == kFlamOffset) {
		return static_cast<float>(kFlamPeak);
	}
	if (n == kFlamOffset + 1) {
		return static_cast<float>(kFlamPeak * flamRatio(static_cast<int>(k % kFlamIds)));
	}
	return 0.0f;
}

float clickMaterial(int material, long long f, double rate) {
	const double t = static_cast<double>(f) / rate;
	if (material == 0) {
		return static_cast<float>(kClickPeak * std::sin(2.0 * M_PI * 220.5 * t));
	}
	if (material == 2) {
		return flamDoublet(f, rate);
	}
	if (material == 3) {
		return frameCode(f);
	}
	const double fpb = 0.5 * rate;
	const double inBeat = std::fmod(static_cast<double>(f), fpb) / rate;
	return static_cast<float>(kClickPeak * std::exp(-inBeat / 0.08) * std::sin(2.0 * M_PI * 440.0 * inBeat));
}

ClickRun clickRender(int grooveMode, double amount, double rate, int block, bool locked, int material, int bars) {
	ClickRun run;
	Sklitter s;
	s.prepare(rate, 4096);
	Settings cfg = defaultSettings();
	setGroove(cfg, grooveMode, amount);
	cfg.transportLocked = locked;
	cfg.engage = false;
	s.setTarget(cfg);
	const double fpb = 0.5 * rate;
	run.bar = static_cast<size_t>(std::llround(4.0 * fpb));
	run.from = static_cast<long long>(9.37 * fpb) - block;
	const long long to = static_cast<long long>(run.bar) * (3 + bars);
	std::vector<float> l(static_cast<size_t>(block));
	std::vector<float> r(static_cast<size_t>(block));
	float p1 = 0.0f;
	for (long long frame = 0; frame < to; frame += block) {
		if (!cfg.engage && static_cast<double>(frame) / fpb >= 9.37) {
			cfg.engage = true;
			s.setTarget(cfg);
			run.pressAt = frame;
		}
		for (int k = 0; k < block; ++k) {
			const float v = clickMaterial(material, frame + k, rate);
			l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] = v;
			if (frame + k >= 1) {
				run.inMaxStep = std::max(run.inMaxStep, std::fabs(v - p1));
			}
			p1 = v;
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
		c.songPositionBeats = static_cast<double>(frame) / fpb;
		c.playing = true;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), block);
		if (run.seam == SIZE_MAX && s.statusForTest() == Status::Playing) {
			run.seam = s.ringForTest().writePos(); // where the capture ended: the loop's one real seam
		}
		for (int k = 0; k < block; ++k) {
			if (frame + k >= run.from) {
				run.out.push_back(l[static_cast<size_t>(k)]);
			}
		}
	}
	const Ring& ring = s.ringForTest();
	for (size_t i = 0; i < ring.length(); ++i) {
		float a = 0.0f;
		float b = 0.0f;
		ring.readAt(i, &a, &b);
		run.ringL.push_back(a);
	}
	return run;
}

} // namespace

TEST_CASE("groove-click: no edge steps further than the signal does — Straight plays the input and a swing gap is ramped both ways") {
	// **A SUBTLE GLITCH WAS HEARD, APPARENTLY ON ATTACKS.** MEASURED on
	// musical material (a sine, and a decaying tone per beat), every frame's first difference classed by the
	// event it sits at. Two defects, and the larger one was not the groove's:
	//   - EVERY SLOT, STRAIGHT INCLUDED: the looper's read crossfaded its last 64 frames toward the slice's
	//     own start, as though a second wrap pass followed; at unison it never does. A 0.45 step on a 0.5
	//     sine at every slot start, both hosts, both lock settings — a faint copy of each slice's attack just
	//     before the next.
	//   - A SWING GAP (backlog IN addendum): the lengthened slot's material stopped dead into the gap and
	//     the delayed hit started from it at full height — 0.13 and 0.11 on the sine at 44.1k.
	// The ring itself was continuous (its largest neighbour step is the sine's own), so no write-head seat
	// seamed it. And a smooth one no step bound can see, only the value: the capture's seam fade sat at ring
	// index 0, which since backlog IN is the song's DOWNBEAT, so every downbeat dipped to zero over 10 ms
	// while the real seam, at the append head, went bare. The fade now sits at the append head.
	struct Feel {
		const char* what;
		GrooveMode mode;
		double amount;
	};
	const Feel feels[] = { { "Straight", GrooveMode::Straight, 0.0 },
	                       { "Swing16 66%", GrooveMode::Swing16, 0.64 },
	                       { "Groove8 50%", GrooveMode::Groove8, 0.5 },
	                       { "Free8 3.7", GrooveMode::Free8, 3.7 } };
	struct Host {
		const char* what;
		double rate;
		int block;
	};
	const Host hosts[] = { { "48k/512", 48000.0, 512 }, { "44.1k/1024", 44100.0, 1024 } };
	constexpr int32_t kSlices = 16;
	for (const int material : { 0, 1 }) {
		for (const Feel& feel : feels) {
			for (const Host& host : hosts) {
				for (const bool locked : { false, true }) {
					const ClickRun run = clickRender(static_cast<int>(feel.mode), feel.amount, host.rate, host.block,
					                                 locked, material, 4);
					Settings cfg = defaultSettings();
					setGroove(cfg, static_cast<int>(feel.mode), feel.amount);
					const GrooveFeel gf = cfg.grooveFeel();
					const size_t bar = run.bar;
					const size_t base = bar / kSlices;
					std::vector<size_t> edge(kSlices + 1);
					for (int32_t k = 0; k <= kSlices; ++k) {
						edge[static_cast<size_t>(k)] = grooveActive(gf) ? grooveBoundary(gf, k, kSlices, bar)
						                                                : static_cast<size_t>(k) * base;
					}
					std::map<std::string, std::pair<int, float>> byEvent; // count, max step
					for (size_t n = 1; n < run.out.size(); ++n) {
						const float x0 = run.out[n];
						const float x1 = run.out[n - 1];
						const long long f = run.from + static_cast<long long>(n);
						const auto phase = static_cast<size_t>(f % static_cast<long long>(bar));
						std::string ev = "other";
						const auto near = [&](size_t at, size_t tol) {
							const long long d = static_cast<long long>(phase) - static_cast<long long>(at);
							return std::llabs(d) <= static_cast<long long>(tol)
							       || std::llabs(d + static_cast<long long>(bar)) <= static_cast<long long>(tol)
							       || std::llabs(d - static_cast<long long>(bar)) <= static_cast<long long>(tol);
						};
						if (f - run.pressAt < static_cast<long long>(bar) / 8 && f >= run.pressAt) {
							ev = "engage";
						}
						else if (near(0, 4)) {
							ev = "bar top";
						}
						else {
							for (int32_t k = 1; k < kSlices; ++k) {
								const size_t e = edge[static_cast<size_t>(k)];
								const size_t mat = static_cast<size_t>(k) * base;
								const size_t prevLen = e - edge[static_cast<size_t>(k) - 1];
								const size_t prevMatEnd = edge[static_cast<size_t>(k) - 1] + base;
								if (near(e, 4)) {
									ev = e > mat ? "delayed hit start" : (e < mat ? "early hit start" : "slot start");
									break;
								}
								if (prevLen > base && near(prevMatEnd, 4)) {
									ev = "gated material end";
									break;
								}
							}
						}
						auto& slot = byEvent[ev];
						++slot.first;
						slot.second = std::max(slot.second, std::fabs(x0 - x1));
					}
					const std::string what = std::string(material == 0 ? "sine " : "burst ") + feel.what + " " + host.what;
					CAPTURE(what);
					CAPTURE(locked);

					// **THE BOUND IS THE SIGNAL'S OWN STEP, PLUS WHAT ONE FLOOR RAMP MAY ADD TO IT** — a gain
					// moving 0 → 1 over `gapRamp` frames on material of peak A adds at most A / gapRamp per frame.
					// Nothing at any edge may step further than that. The contract restated: `kMinAntiClickSeconds`
					// is 64 samples at 44.1 kHz, rounded to the host's rate.
					const auto gapRamp = static_cast<size_t>(std::llround(64.0 / 44100.0 * host.rate));
					const double bound = static_cast<double>(run.inMaxStep) + kClickPeak / static_cast<double>(gapRamp) + 1e-5;
					CAPTURE(bound);
					REQUIRE(byEvent.count("slot start") + byEvent.count("delayed hit start") > 0); // edges were seen
					for (const auto& [ev, v] : byEvent) {
						CAPTURE(ev);
						const double step = static_cast<double>(v.second);
						CAPTURE(step);
						CHECK(step <= bound);
					}

					// **THE LOOP'S ONE REAL SEAM IS WHERE THE CAPTURE ENDED, AND ITS FADE BELONGS THERE.** The
					// reference's 5 ms V, restated: gain i/n on the i-th frame either side of the append head. The
					// press waits for a grid line that is not a downbeat here, so a fade left at index 0 — the song's
					// downbeat since backlog IN, continuous material — is told apart from one at the seam.
					const auto wrapN = static_cast<size_t>(std::lround(0.005 * host.rate));
					REQUIRE(run.seam != SIZE_MAX);
					REQUIRE(run.seam % bar > wrapN);
					REQUIRE(run.seam % bar < bar - wrapN);
					const auto seamGain = [&](size_t phase) {
						const size_t up = (phase + bar - run.seam) % bar; // frames on from the seam
						const size_t down = (run.seam + bar - 1 - phase) % bar; // frames back from it
						if (up < wrapN) {
							return static_cast<double>(static_cast<float>(up) / static_cast<float>(wrapN));
						}
						if (down < wrapN) {
							return static_cast<double>(static_cast<float>(down) / static_cast<float>(wrapN));
						}
						return 1.0;
					};

					// THE RING, which the write-head seat (backlog IN) could seam: every neighbour as close as the
					// input's own, but for the seam fade's own slope.
					float ringMaxStep = 0.0f;
					for (size_t i = 1; i < run.ringL.size(); ++i) {
						ringMaxStep = std::max(ringMaxStep, std::fabs(run.ringL[i] - run.ringL[i - 1]));
					}
					CHECK(static_cast<double>(ringMaxStep)
					      <= static_cast<double>(run.inMaxStep) + kClickPeak / static_cast<double>(wrapN) + 1e-6);

					const long long settled = run.pressAt + static_cast<long long>(bar) / 4;
					REQUIRE(settled > run.from);
					if (!grooveActive(gf)) {
						// **STRAIGHT AT DEFAULTS PLAYS THE INPUT, FRAME FOR FRAME, BUT FOR THE SEAM FADE** — Shuffle at
						// Zone 0 reads the slices in order and the ring holds the song's phase (backlog IN). By value:
						// the known-bad that crossfades each slot's tail toward its own start misses this by ~0.45 at
						// every slot end, and a seam fade left at index 0 by ~0.4 at every downbeat.
						double worst = 0.0;
						long long firstOff = -1;
						for (long long f = settled; f < run.from + static_cast<long long>(run.out.size()); ++f) {
							const auto phase = static_cast<size_t>(f % static_cast<long long>(bar));
							const double want = static_cast<double>(clickMaterial(material, f, host.rate)) * seamGain(phase);
							const double d = std::fabs(static_cast<double>(run.out[static_cast<size_t>(f - run.from)]) - want);
							if (d > 1e-6 && firstOff < 0) {
								firstOff = f;
							}
							worst = std::max(worst, d);
						}
						CAPTURE(firstOff);
						CHECK(worst <= 1e-6);
						continue;
					}

					// **A SWING GAP, BY VALUE, AT EVERY DELAYED HIT THAT FOLLOWS A LENGTHENED SLOT.** The material
					// ends on the ramp's last step (gain 1/gapRamp), the gap is zero, and the hit starts at gain 0
					// and plays its own material, untouched, from `gapRamp` on.
					long long gaps = 0;
					for (long long b = (settled / static_cast<long long>(bar) + 1) * static_cast<long long>(bar);
					     b + static_cast<long long>(bar) <= run.from + static_cast<long long>(run.out.size());
					     b += static_cast<long long>(bar)) {
						for (int32_t k = 1; k < kSlices; ++k) {
							const size_t slotStart = edge[static_cast<size_t>(k)];
							const size_t prevStart = edge[static_cast<size_t>(k) - 1];
							const size_t mat = static_cast<size_t>(k) * base;
							if (!(slotStart > mat && slotStart - prevStart > base)) {
								continue;
							}
							++gaps;
							CAPTURE(k);
							const auto at = [&](size_t phase) {
								return static_cast<double>(run.out[static_cast<size_t>(b + static_cast<long long>(phase) - run.from)]);
							};
							const size_t matEnd = prevStart + base - 1; // the lengthened slot's last material frame
							const double tail = std::fabs(at(matEnd));
							CHECK(tail <= kClickPeak / static_cast<double>(gapRamp) + 1e-6);
							CHECK(at(matEnd + 1) == 0.0);
							CHECK(at(slotStart - 1) == 0.0);
							CHECK(at(slotStart) == 0.0);
							double worst = 0.0;
							for (size_t j = gapRamp; j < gapRamp + 64; ++j) {
								const double want =
								    static_cast<double>(clickMaterial(material, static_cast<long long>(mat + j), host.rate))
								    * seamGain(mat + j); // the ring's phase mat + j, where the seam fade may reach
								worst = std::max(worst, std::fabs(at(slotStart + j) - want));
							}
							CHECK(worst <= 1e-6);
						}
					}
					CAPTURE(gaps);
					CHECK(gaps >= 8);
				}
			}
		}
	}
}

TEST_CASE("groove-click: in Straight a slot's last frames are its own material and never its start's — by frame number") {
	// **THE PRE-ECHO BY NAME.** The looper's wrap crossfade used to run in the last `xfade` frames of every
	// slot, blending toward the slice's START — so those frames carried the slot's own opening material,
	// faded in. With every input frame naming itself, a frame there must be exactly the loop's copy of the
	// song phase it sits at (Straight at defaults plays the ring in order): a blend decodes to no frame at
	// all, and a copy of the start decodes to the wrong one.
	struct Host {
		const char* what;
		double rate;
		int block;
	};
	const Host hosts[] = { { "48k/512", 48000.0, 512 }, { "44.1k/1024", 44100.0, 1024 } };
	for (const Host& host : hosts) {
		for (const bool locked : { false, true }) {
			const std::string what = host.what;
			CAPTURE(what);
			CAPTURE(locked);
			const ClickRun run = clickRender(static_cast<int>(GrooveMode::Straight), 0.0, host.rate, host.block, locked, 3, 4);
			const auto bar = static_cast<long long>(run.bar);
			const long long base = bar / 16;
			const auto xfade = static_cast<long long>(std::llround(64.0 / 44100.0 * host.rate)); // kPWriteFadeSeconds
			const auto wrapN = static_cast<long long>(std::lround(0.005 * host.rate));
			REQUIRE(run.seam != SIZE_MAX);
			const long long settled = run.pressAt + bar / 4;
			long long tails = 0;
			long long fromStart = 0;
			long long notItself = 0;
			for (long long f = settled; f < run.from + static_cast<long long>(run.out.size()); ++f) {
				const long long phase = f % bar;
				const long long slot = std::min<long long>(phase / base, 15);
				const long long slotEnd = slot == 15 ? bar : (slot + 1) * base;
				if (phase < slotEnd - xfade) {
					continue;
				}
				const long long fromSeam = ((phase - static_cast<long long>(run.seam)) % bar + bar) % bar;
				if (fromSeam < wrapN || fromSeam >= bar - wrapN) {
					continue; // the capture's seam fade, asserted by value in the case above
				}
				++tails;
				const long long src = frameOf(run.out[static_cast<size_t>(f - run.from)]);
				if (src >= 0 && (src % bar) >= slot * base && (src % bar) < slot * base + xfade) {
					++fromStart;
				}
				// the loop's own copy of this song phase, recorded a whole buffer (here a bar) earlier
				notItself += (src >= 0 && src < f && (f - src) % bar == 0) ? 0 : 1;
			}
			CAPTURE(tails);
			REQUIRE(tails > 16 * 3 * xfade); // three bars of slot tails, not vacuous
			CHECK(fromStart == 0);
			CHECK(notItself == 0);
		}
	}
}

TEST_CASE("groove-click: no hit is heard twice — every groove incl. Free, both hosts and both locks, through the press") {
	// **THE GLITCH, HEARD MORE CLOSELY, IS A DOUBLE HIT ON THE ATTACK, NOT A FADE-IN.** So the instrument is identity, not steps: every sixteenth carries a doublet whose RATIO names it
	// (`flamDoublet`), and every non-zero output pair is decoded to the hit it is a copy of and the gain it
	// arrives at. A hit may sound at its own sixteenth (dry, before and through the press) or at its slot's
	// start under the groove (wet) — anywhere else it is a double.
	//
	// MEASURED AS INSTALLED (`decd94ab`, all three groove-click fixes reverted): 59-77 doubled copies in 4.6
	// bars in EVERY setting, Straight included (72): each slot ended with its own hit again at −10 dB
	// (0.29-0.31 of the hit), ~1 ms before the next slot began — so every attack was preceded by a ghost of
	// the one before it, 1.5 ms ahead. Under swing the on-beat after a chopped off-beat starts under the 10 ms
	// grain fade, so 20 frames in it is at 0.04 while the ghost ahead of it was at 0.29 — 6.8x louder than the
	// hit's own first moment. One more copy per press sat in the engage crossfade. After the fixes: zero.
	struct Feel {
		const char* what;
		GrooveMode mode;
		double amount;
	};
	const Feel feels[] = { { "Straight", GrooveMode::Straight, 0.0 },   { "Swing16 66%", GrooveMode::Swing16, 0.64 },
	                       { "Swing8 66%", GrooveMode::Swing8, 0.64 },   { "Groove8 50%", GrooveMode::Groove8, 0.5 },
	                       { "Groove16 50%", GrooveMode::Groove16, 0.5 }, { "Free8 3.7", GrooveMode::Free8, 3.7 },
	                       { "Free16 3.7", GrooveMode::Free16, 3.7 } };
	struct Host {
		const char* what;
		double rate;
		int block;
	};
	const Host hosts[] = { { "48k/512", 48000.0, 512 }, { "44.1k/1024", 44100.0, 1024 } };
	constexpr int32_t kSlices = 16;
	for (const Feel& feel : feels) {
		for (const Host& host : hosts) {
			for (const bool locked : { false, true }) {
				const ClickRun run = clickRender(static_cast<int>(feel.mode), feel.amount, host.rate, host.block, locked, 2, 4);
				Settings cfg = defaultSettings();
				setGroove(cfg, static_cast<int>(feel.mode), feel.amount);
				const GrooveFeel gf = cfg.grooveFeel();
				const auto bar = static_cast<long long>(run.bar);
				const long long base = bar / kSlices;
				std::vector<long long> edge(kSlices + 1);
				for (int32_t k = 0; k <= kSlices; ++k) {
					edge[static_cast<size_t>(k)] = grooveActive(gf) ? static_cast<long long>(grooveBoundary(gf, k, kSlices, run.bar))
					                                                : k * base;
				}
				const long long engageEnd = run.pressAt + bar / 4;
				int mains = 0;
				int extras = 0;
				int engageExtras = 0;
				std::map<std::string, std::tuple<int, double, double, long long>> kinds;
				const auto outAt = [&](long long f) {
					const long long i = f - run.from;
					return i >= 0 && i < static_cast<long long>(run.out.size()) ? static_cast<double>(run.out[static_cast<size_t>(i)]) : 0.0;
				};
				for (long long t = run.from + 1; t + 2 < run.from + static_cast<long long>(run.out.size()); ++t) {
					const double a = outAt(t);
					if (std::fabs(a) < 1e-6 || std::fabs(outAt(t - 1)) > 1e-9) {
						continue;
					}
					const double ratio = outAt(t + 1) / a;
					if (ratio <= 0.0) {
						continue; // the arming crossing, not a hit
					}
					const int id = static_cast<int>(std::lround(std::log(ratio / 0.15) / std::log(kFlamStep)));
					long long best = LLONG_MAX;
					long long bestAt = 0;
					for (long long b = t / bar - 1; b <= t / bar + 1; ++b) {
						for (int32_t k = std::max(id, 0); k < kSlices; k += kFlamIds) {
							for (const long long start : { b * bar + edge[static_cast<size_t>(k)], b * bar + k * base }) {
								const long long want = start + kFlamOffset;
								if (std::llabs(t - want) < std::llabs(best)) {
									best = t - want;
									bestAt = want;
								}
							}
						}
					}
					if (id < 0 || id >= kFlamIds || std::llabs(best) <= 2) {
						++mains;
						continue;
					}
					++extras;
					const bool inEngage = t >= run.pressAt - 1024 && t < engageEnd;
					engageExtras += inEngage ? 1 : 0;
					const long long phase = ((t % bar) + bar) % bar;
					long long toNext = bar;
					for (int32_t k = 0; k <= kSlices; ++k) {
						const long long d = edge[static_cast<size_t>(k)] - phase;
						if (d > 0 && d < toNext) {
							toNext = d;
						}
					}
					const double mainGain = std::fabs(outAt(bestAt)) / kFlamPeak;
					const double rel = mainGain > 0.0 ? std::fabs(a) / kFlamPeak / mainGain : -1.0;
					char key[160];
					std::snprintf(key, sizeof key, "%s gap %+.1f ms, %lld frames before next slot", inEngage ? "ENGAGE" : "play",
					              1000.0 * static_cast<double>(best) / host.rate, toNext);
					auto& kd = kinds[key];
					++std::get<0>(kd);
					std::get<1>(kd) = 1000.0 * static_cast<double>(best) / host.rate;
					std::get<2>(kd) = std::max(std::get<2>(kd), rel);
					std::get<3>(kd) = toNext;
				}
				const std::string what = std::string(feel.what) + " " + host.what;
				CAPTURE(what);
				CAPTURE(locked);
				std::string firstExtra = kinds.empty() ? std::string("none") : kinds.begin()->first;
				CAPTURE(firstExtra);
				CAPTURE(engageExtras);
				// ~4.6 bars of sixteenths from the press on: every one heard, and heard once.
				CHECK(mains >= 70);
				CHECK(extras == 0);
			}
		}
	}
}

TEST_CASE("backlog IO addendum: Free N at phase 0 IS Straight — to the bit, in every Free grid, both hosts and both locks") {
	// **FREE DRIVEN TO ABOUT 140 AND RESET TO ZERO LEFT THE GROOVE IN PLACE.** Phase 0 is exactly straight by
	// design. Asserted as
	// the output and the ring of the whole render through the press, against Straight's, bit for bit — and
	// first as the values the render is made of, since a render can only say that something differs.
	for (const GrooveMode free : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(free);
		CAPTURE(grid);
		// THE VALUES: every step on its grid from either side of 0, and the feel the identity.
		for (const double zero : { 0.0, -0.0 }) {
			const GrooveFeel feel { free, zero };
			CHECK_FALSE(grooveActive(feel));
			for (int j = 0; j <= grid; ++j) {
				CAPTURE(j);
				CHECK(grooveStepOffset(feel, j) == 0.0);
				CHECK(freeTerm(j, zero) == 0.0);
			}
		}
		// THE DECODE: a device holding Free at phase 0 plays the identity — the Settings road the engine reads.
		Settings cfg = looper(Mode::Shuffle, 0.5f);
		setGroove(cfg, static_cast<int>(free), 0.0);
		CHECK(cfg.grooveFeel() == GrooveFeel { free, 0.0 });
		CHECK_FALSE(grooveActive(cfg.grooveFeel()));
	}
	struct Host {
		const char* what;
		double rate;
		int block;
	};
	const Host hosts[] = { { "48k/512", 48000.0, 512 }, { "44.1k/1024", 44100.0, 1024 } };
	for (const Host& host : hosts) {
		for (const bool locked : { false, true }) {
			CAPTURE(host.what);
			CAPTURE(locked);
			const ClickRun straight = clickRender(0, 0.0, host.rate, host.block, locked, 0, 4);
			REQUIRE(straight.pressAt >= 0);
			for (const GrooveMode free : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
				CAPTURE(static_cast<int>(free));
				const ClickRun atZero = clickRender(static_cast<int>(free), 0.0, host.rate, host.block, locked, 0, 4);
				CHECK(atZero.out == straight.out);
				CHECK(atZero.ringL == straight.ringL);
				// NOT VACUOUS: the same render a little way along the walk is NOT Straight's, so the defaults
				// this renders through are a mode the groove acts in and the comparison can see a groove.
				const ClickRun along = clickRender(static_cast<int>(free), 0.4, host.rate, host.block, locked, 0, 4);
				CHECK(along.out != straight.out);
			}
		}
	}
}

TEST_CASE("backlog IO addendum: Free N near phase 0 — a tiny phase is a tiny groove, only the off-beats move, and "
          "−a plays what a plays") {
	// The wave's SHAPE at 0, restated: an off-beat's triangle has duty 1 and no zero segment, so it rises from 0
	// at once and linearly — late = ½ · 2·r_j·|a| for |a| < 1/(2·r_j), r_j = φ^(1/j). Every stronger step sits
	// in its zero segment until |a|·r_j·d_j reaches 1 − d_j, so at a thousandth it is exactly on its grid. The
	// bound for a thousandth is φ · 1e-3 ≈ 0.0016 of a step (`kFreeMaxSlope`).
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	for (const GrooveMode free : { GrooveMode::Free4, GrooveMode::Free8, GrooveMode::Free16 }) {
		const int grid = grooveGridOf(free);
		for (const double a : { 1e-3, -1e-3 }) {
			CAPTURE(grid);
			CAPTURE(a);
			const GrooveFeel feel { free, a };
			REQUIRE(grooveActive(feel));
			double most = 0.0;
			for (int j = 1; j < grid; ++j) {
				CAPTURE(j);
				const double d = grooveStepOffset(feel, j);
				most = std::max(most, d);
				CHECK(d <= phi * 1e-3 + 1e-15);
				if (j % 2 == 1) {
					const double rate = std::exp(std::log(phi) / static_cast<double>(j));
					// AN ABSOLUTE BOUND, NOT `Approx`: its default scale of 1 would let any number within 1e-9 of
					// 1.0 through — tight here, but the form is the trap, so it is not used on a small quantity.
					CHECK(d > 0.0);
					CHECK(std::fabs(d - rate * 1e-3) <= 1e-12);
				}
				else {
					CHECK(d == 0.0);
				}
			}
			// NOT VACUOUS: step 1 moves, by very nearly the whole bound (its rate is φ).
			CHECK(std::fabs(most - phi * 1e-3) <= 1e-12);
		}
		// THE MIRROR: the walk below 0 is the walk above it, so a route swinging through 0 passes straight
		// through the straight grid rather than meeting a different pattern on the other side.
		for (const double a : { 0.05, 0.37, 2.5, 137.25 }) {
			CAPTURE(a);
			for (int j = 1; j < grid; ++j) {
				CAPTURE(j);
				CHECK(grooveStepOffset({ free, -a }, j) == grooveStepOffset({ free, a }, j));
				CHECK(std::fabs(grooveStepOffset({ free, a }, j) - refFreeSteps(grid, a, j)) <= 1e-12);
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// REPEAT'S FACE: the ribbon draws a repeat as a bar split horizontally, the repeating duration on   //
// the bottom and its subdivisions on top.                                                            //
//--------------------------------------------------------------------------------------------------//

namespace {

/// Every published reading, the last publish's.
struct Published {
	std::vector<float> v;
	[[nodiscard]] float at(Reading r) const { return v.at(static_cast<size_t>(r)); }
	[[nodiscard]] float source(size_t step) const {
		return v.at(stepReading(step));
	}
};

void listen(Sklitter& s, Published& p) {
	s.setReadingsSink(feathers::device::ReadingsSink { &p, [](void* ctx, const float* v, size_t n) {
		                                                   static_cast<Published*>(ctx)->v.assign(v, v + n);
	                                                   } });
}

/// **THE REPEAT COUNT, RESTATED FROM THE REFERENCE'S CURVE** rather than read off `loopSlicesFor`: a slice
/// of `bar·(1−r)²`, floored at 256 frames and capped at the bar, and as many of them as fit.
int32_t repeatsRestated(float rate, size_t bar) {
	const double n = 1.0 - static_cast<double>(rate);
	const auto len = std::clamp<size_t>(static_cast<size_t>(static_cast<double>(bar) * n * n), 256u, bar);
	return static_cast<int32_t>(std::max<size_t>(1u, bar / len));
}

/// A Rate that cuts the bar into `n` repeats — just past `1 − 1/√n`, so the truncation lands on `n`.
float rateForRepeats(int n) { return static_cast<float>(1.0 - 1.0 / std::sqrt(static_cast<double>(n)) + 0.004); }

} // namespace

TEST_CASE("Repeat's face: the band publishes the subdivisions and the repeating duration, at rest and engaged") {
	constexpr size_t kBar = 96000; // four beats at 120 BPM, 48 kHz
	for (const int want : { 3, 5, 8 }) {
		CAPTURE(want);
		const float rate = rateForRepeats(want);
		REQUIRE(repeatsRestated(rate, kBar) == want); // the fixture's own premise
		REQUIRE(loopSlicesFor(Mode::Repeat, rate, kBar) == want);

		Settings cfg = looper(Mode::Repeat, rate);
		cfg.seqBars = 1;
		cfg.engage = false;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Published p;
		listen(s, p);
		s.setTarget(cfg);
		std::vector<float> l(480);
		std::vector<float> r(480);
		ramp(l, r, 0.0f);
		s.processStereo(l.data(), r.data(), 480);
		s.publishReadings();
		REQUIRE(p.v.size() == kReadingCount);

		// AT REST: the top row is N cells; the bottom row's span is one repeat's length, of the capture.
		CHECK(p.at(Reading::Status) == 0.0f);
		CHECK(p.at(Reading::Steps) == static_cast<float>(want));
		CHECK(p.at(Reading::RateDivision) == static_cast<float>(want)); // the GS readout's 1/N agrees
		const float restLength = p.at(Reading::RepeatLength);
		CHECK(restLength == doctest::Approx(static_cast<double>(kBar / static_cast<size_t>(want)) / kBar));

		// ENGAGED: what PLAYS (backlog IB). The span is the playing slot's own length, over the buffer.
		cfg.engage = true;
		s.setTarget(cfg);
		double beats = 480.0 / (0.5 * kRate);
		drive(s, 96000 + 9600 + 30000, 0.0f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		s.publishReadings();
		CHECK(p.at(Reading::Status) == 3.0f);
		CHECK(p.at(Reading::Steps) == static_cast<float>(want));
		CHECK(p.at(Reading::RateDivision) == static_cast<float>(want));
		const double playing = static_cast<double>(s.scheduleForTest().sliceLength)
		                       / static_cast<double>(s.bufferFramesForTest());
		CHECK(p.at(Reading::RepeatLength) == doctest::Approx(playing));
		CHECK(p.at(Reading::RepeatLength) > 0.0f);
	}
}

TEST_CASE("Repeat's face: every other mode publishes no repeat length, so its band keeps the chunk face") {
	for (const Mode m : { Mode::Burst, Mode::Time, Mode::Shuffle, Mode::Grain, Mode::Pattern,
	                      Mode::Pitch }) {
		CAPTURE(static_cast<int>(m));
		for (const bool engaged : { false, true }) {
			CAPTURE(engaged);
			Settings cfg = m == Mode::Burst ? burst(0.5f) : looper(m);
			cfg.mode = m;
			cfg.engage = engaged;
			Sklitter s;
			s.prepare(kRate, 4096);
			s.setTransportClock(at120bpm());
			Published p;
			listen(s, p);
			s.setTarget(cfg);
			drive(s, engaged ? 96000 + 9600 + 30000 : 4800);
			s.publishReadings();
			REQUIRE(p.v.size() == kReadingCount);
			CHECK(p.at(Reading::RepeatLength) == 0.0f);
		}
	}
	// AND REPEAT, THE CONTROL, PUBLISHES ONE — or the zeros above say nothing.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Published p;
	listen(s, p);
	s.setTarget(looper(Mode::Repeat));
	drive(s, 4800);
	s.publishReadings();
	CHECK(p.at(Reading::RepeatLength) > 0.0f);
}

TEST_CASE("Repeat's face: the groove acts in Repeat, so the subdivisions are ruled on the swung grid") {
	Settings cfg = looper(Mode::Repeat, rateForRepeats(8));
	cfg.engage = false;
	cfg.grooveMode = static_cast<float>(GrooveMode::Swing16);
	cfg.grooveAmount = 0.5f;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Published p;
	listen(s, p);
	s.setTarget(cfg);
	drive(s, 4800);
	s.publishReadings();
	CHECK(p.at(Reading::Groove) == static_cast<float>(GrooveMode::Swing16));
	CHECK(p.at(Reading::GrooveAmount) == 0.5f);
}

//--------------------------------------------------------------------------------------------------//
// REPEAT REPEATS (backlog IX, option A1) — the reference's Repeat start, ported.                     //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **`stutterer.cpp`'s Repeat start, RESTATED** — not `scheduleFor` called back:
///     availableRange = playbackLength - currentSliceLength;
///     offsetAmount = (sliceOffset * availableRange) >> 4;    // skipTarget on a skip
///     sliceStartOffset = playbackLength - currentSliceLength - offsetAmount;
/// with the slot as the span (the grid's `bar / N`, the last carrying the remainder) and the buffer as
/// `playbackLength`, which is what "A1" asked for.
size_t referenceRepeatStart(const SlicePlan& plan, size_t slot, size_t buffer) {
	const size_t availableRange = buffer - slot;
	const auto pick = static_cast<size_t>(plan.shouldSkip ? plan.skipTarget : plan.sliceOffset);
	const size_t offsetAmount = (pick * availableRange) >> 4;
	return buffer - slot - offsetAmount;
}

} // namespace

TEST_CASE("PARITY: Repeat loops one span from the END of the capture, moved earlier by the plan") {
	constexpr size_t kBar = 96000;
	SUBCASE("the schedule, over every offset and a skip") {
		for (const int32_t n : { 1, 3, 5, 8 }) {
			CAPTURE(n);
			const size_t slot = kBar / static_cast<size_t>(n);
			for (int32_t offset = 0; offset < 16; ++offset) {
				for (const bool skip : { false, true }) {
					CAPTURE(offset);
					CAPTURE(skip);
					SlicePlan plan;
					plan.sliceOffset = skip ? 3 : offset;
					plan.shouldSkip = skip;
					plan.skipTarget = offset;
					plan.grainLength = 4; // a long grain is refused in Repeat, as the reference's is
					for (int32_t i = 0; i < n; ++i) {
						const SliceSchedule s = scheduleFor(Mode::Repeat, 0.0f, i, n, kBar, plan, kBar);
						const size_t len = slot + (i == n - 1 ? kBar - slot * static_cast<size_t>(n) : 0);
						CHECK(s.grainLength == 1);
						CHECK(s.sliceLength == len);
						CHECK(s.startOffset == referenceRepeatStart(plan, len, kBar));
						CHECK(s.startOffset + s.sliceLength <= kBar);
					}
				}
			}
		}
		// A TWO-BAR BUFFER IS THE REACH: at offset 0 the span is the capture's last slot.
		SlicePlan plan;
		const SliceSchedule s = scheduleFor(Mode::Repeat, 0.0f, 0, 4, kBar, plan, 2 * kBar);
		CHECK(s.startOffset == 2 * kBar - kBar / 4);
	}

	SUBCASE("the device: every repeat of a bar at zone A 0 reads the SAME span, the bar's last") {
		for (const int want : { 3, 5, 8 }) {
			CAPTURE(want);
			Settings cfg = looper(Mode::Repeat, rateForRepeats(want));
			Sklitter s;
			s.prepare(kRate, 4096);
			s.setTransportClock(at120bpm());
			s.setTarget(cfg);
			double beats = 0.0;
			drive(s, 96000 + 9600, 0.0f, &beats);
			REQUIRE(s.statusForTest() == Status::Playing);
			const size_t slot = kBar / static_cast<size_t>(want);
			size_t seen = 0;
			int last = -1;
			for (int k = 0; k < 4000 && seen < static_cast<size_t>(3 * want); ++k) {
				drive(s, 96, 0.0f, &beats);
				if (s.sliceIndexForTest() == last) {
					continue;
				}
				last = s.sliceIndexForTest();
				++seen;
				const SliceSchedule& sch = s.scheduleForTest();
				CAPTURE(last);
				CHECK(sch.startOffset + sch.sliceLength == kBar); // every repeat ends at the capture's end
				CHECK(sch.startOffset == referenceRepeatStart(s.planForTest(), sch.sliceLength, kBar));
				CHECK(sch.sliceLength >= slot);
			}
			CHECK(seen >= static_cast<size_t>(3 * want));
		}
	}

	SUBCASE("the device at zone A 0.6: the span moves by the plan's offset, restated at each repeat") {
		Settings cfg = looper(Mode::Repeat, rateForRepeats(4));
		cfg.zoneA = 0.6f;
		cfg.zoneB = 0.8f;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		double beats = 0.0;
		drive(s, 96000 + 9600, 0.0f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		std::set<size_t> starts;
		int last = -1;
		for (int k = 0; k < 4000 && starts.size() < 64; ++k) {
			drive(s, 96, 0.0f, &beats);
			if (s.sliceIndexForTest() == last) {
				continue;
			}
			last = s.sliceIndexForTest();
			const SliceSchedule& sch = s.scheduleForTest();
			CHECK(sch.startOffset == referenceRepeatStart(s.planForTest(), sch.sliceLength, kBar));
			starts.insert(sch.startOffset);
		}
		// THE OFFSET ACTS, or the zone-A-0 subcase above is the only thing this proves.
		CHECK(starts.size() > 1u);
	}
}

TEST_CASE("Repeat's band draws the plan the device PLAYS: its own counter, from 1 at each press") {
	// The walk planned Repeat's steps at the step index while `advanceSlice` plans them at
	// `repeatLoopIndex_`; past zone A 0 the band's sources were not what played (backlog IX).
	Settings cfg = looper(Mode::Repeat, rateForRepeats(4));
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.seqBars = 1;
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Published p;
	listen(s, p);
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);
	size_t checked = 0;
	size_t differsFromStepIndex = 0;
	int last = -1;
	for (int k = 0; k < 12000 && checked < 24; ++k) {
		drive(s, 96, 0.0f, &beats);
		if (s.sliceIndexForTest() == last) {
			continue;
		}
		last = s.sliceIndexForTest();
		s.publishReadings();
		const auto step = static_cast<size_t>(std::lround(p.at(Reading::Step)));
		CAPTURE(step);
		const double playing = static_cast<double>(s.scheduleForTest().startOffset)
		                       / static_cast<double>(s.bufferFramesForTest());
		CHECK(p.source(step) == doctest::Approx(playing));
		++checked;
		if (s.repeatLoopIndexForTest() != static_cast<int32_t>(step)) {
			++differsFromStepIndex;
		}
	}
	CHECK(checked == 24u);
	CHECK(differsFromStepIndex > 0u); // the counter and the step part company, or this proves nothing
	// A NEW PRESS STARTS THE COUNTER AT 1 (the reference's `triggerPlaybackNow`).
	Settings off = cfg;
	off.engage = false;
	s.setTarget(off);
	drive(s, 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() != Status::Playing);
	s.setTarget(cfg);
	for (int k = 0; k < 4000 && s.statusForTest() != Status::Playing; ++k) {
		drive(s, 96, 0.0f, &beats);
	}
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(s.sliceIndexForTest() == 0);
	CHECK(s.repeatLoopIndexForTest() == 1);
}

//--------------------------------------------------------------------------------------------------//
// THE LONG GRAIN AND THE RATCHET ON THE BAND (backlog IX): the grain-repeat effect applies in other  //
// modes as well, a long grain draws a larger grain, and the two can stack.                           //
//--------------------------------------------------------------------------------------------------//

namespace {

struct StepFace {
	float source = 0.0f;
	uint32_t flags = 0;
	float piece = 0.0f;
	[[nodiscard]] int span() const { return 1 + static_cast<int>((flags >> kStepSpanShift) & kStepSpanMask); }
	[[nodiscard]] int ratchet() const {
		return 1 + static_cast<int>((flags >> kStepRatchetShift) & kStepCountMask);
	}
	[[nodiscard]] bool covered() const { return (flags & static_cast<uint32_t>(StepFlag::Covered)) != 0u; }
};

StepFace stepFace(const Published& p, size_t k) {
	return { p.v.at(stepReading(k)), static_cast<uint32_t>(std::lround(p.v.at(stepReading(k) + 1u))),
		     p.v.at(stepReading(k) + 2u) };
}

} // namespace

TEST_CASE("backlog IX: every slot the device plays is the cell the band publishes — its span, its ratchet, "
          "its piece") {
	// THE CLAIM IS "WHAT THE BAND DRAWS IS WHAT PLAYS", so it is checked at every slot the head starts, against
	// the device's own schedule and plan — and the piece against the ratchet's arithmetic RESTATED here:
	// the audible length (slot × lengthMult) over the count, floored at 24 ms and never past the audible end.
	size_t longSeen = 0;
	size_t ratchetSeen = 0;
	size_t stackedSeen = 0;
	size_t slotsChecked = 0;
	// **THE RATCHET NEEDS THE PUCK, AND THE STACK IS RARE.** `subdivisions = 1 + (target − 1) · push.subdivMix`,
	// and the push is zero with the puck at the centre — MEASURED: 454 slots over three zones with the puck
	// centred, 8 long grains, NOT ONE ratchet. And a ratchet is drawn only where `repeatSlices` is 1, which the
	// same high Zone A that buys long grains makes rarer: over a 20 × 10 × 10 × 16 grid of `planFor` at full
	// macro, 4,610 long grains, 806 ratchets, FOUR stacked (all at Zone A 0.05, Zone B 0.8). So the sweep
	// visits that corner too.
	for (const Mode mode : { Mode::Shuffle, Mode::Pattern, Mode::Pitch, Mode::Time, Mode::Repeat }) {
		for (const float za : { 0.05f, 0.55f, 0.95f }) {
			// (puck config, Zone B); a config below zero leaves the puck at the centre.
			for (const auto& [mc, zb] : std::initializer_list<std::pair<float, float>> {
			         { -1.0f, 0.3f }, { -1.0f, 0.8f }, { 0.1f, 0.3f }, { 0.1f, 0.8f }, { 0.4f, 0.3f }, { 0.4f, 0.8f },
			         { 0.7f, 0.3f }, { 0.7f, 0.8f } }) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(za);
				CAPTURE(zb);
				CAPTURE(mc);
				Settings cfg = looper(mode, mode == Mode::Repeat ? rateForRepeats(8) : 0.5f);
				cfg.zoneA = za;
				cfg.zoneB = zb;
				cfg.seqBars = 1;
				if (mc >= 0.0f) {
					withMacro(cfg, 1.0f, mc);
				}
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTransportClock(at120bpm());
				Published p;
				listen(s, p);
				s.setTarget(cfg);
				double beats = 0.0;
				drive(s, 96000 + 9600, 0.0f, &beats);
				REQUIRE(s.statusForTest() == Status::Playing);
				const size_t floorFrames = s.minSubSliceFramesForTest();
				int last = -1;
				for (int k = 0; k < 2000; ++k) {
					drive(s, 96, 0.0f, &beats);
					if (s.sliceIndexForTest() == last) {
						continue;
					}
					last = s.sliceIndexForTest();
					s.publishReadings();
					const auto step = static_cast<size_t>(std::lround(p.at(Reading::Step)));
					CAPTURE(step);
					const StepFace f = stepFace(p, step);
					const SliceSchedule& sch = s.scheduleForTest();
					const SlicePlan& plan = s.planForTest();
					// THE HEAD NEVER STARTS A SLOT ON A COVERED STEP.
					CHECK_FALSE(f.covered());
					CHECK(f.span() == sch.grainLength);
					// THE STEPS THE GRAIN SPANS ARE COVERED, AND ONLY THOSE.
					for (int j = 1; j < sch.grainLength; ++j) {
						CHECK(stepFace(p, step + static_cast<size_t>(j)).covered());
					}
					// THE COUNT PUBLISHED IS THE COUNT HEARD, which is at most the plan's: pieces that would start in
					// the release zone or past the audible end are silence (ribbon truth, which asserts the heard
					// count against what the looper read). The piece is the plan's arithmetic whenever it ratchets.
					const int count = std::max(1, plan.subdivisions);
					CHECK(f.ratchet() <= count);
					if (f.ratchet() > 1) {
						const auto audible = static_cast<size_t>(static_cast<double>(sch.sliceLength) * plan.lengthMult);
						const size_t divided = audible / static_cast<size_t>(count);
						const size_t piece = divided < floorFrames ? std::min(floorFrames, audible) : divided;
						CHECK(f.piece == doctest::Approx(static_cast<double>(piece) / static_cast<double>(sch.sliceLength)));
					}
					else {
						CHECK(f.piece == 0.0f);
					}
					// THE CELL'S EXTENT IS THE SLOT'S REAL LENGTH: `span` steps of the bar's grid, the bar's last
					// grain carrying the remainder.
					const size_t perBar = static_cast<size_t>(std::lround(p.at(Reading::Steps)));
					const size_t base = 96000u / perBar;
					const size_t extent = base * static_cast<size_t>(f.span())
					                      + (step + static_cast<size_t>(f.span()) >= perBar ? 96000u - base * perBar : 0u);
					CHECK(sch.sliceLength == std::max<size_t>(extent, s.minSliceFramesForTest()));
					++slotsChecked;
					longSeen += f.span() > 1 ? 1u : 0u;
					ratchetSeen += f.ratchet() > 1 ? 1u : 0u;
					stackedSeen += f.span() > 1 && f.ratchet() > 1 ? 1u : 0u;
				}
			}
		}
	}
	MESSAGE("slots " << slotsChecked << ", long grains " << longSeen << ", ratchets " << ratchetSeen << ", stacked "
	                 << stackedSeen);
	// EACH EFFECT ACTUALLY OCCURRED, or the agreement above was measured on plain slots only — and they STACK.
	CHECK(longSeen > 0u);
	CHECK(ratchetSeen > 0u);
	CHECK(stackedSeen > 0u);
}

TEST_CASE("backlog IX: a band with no long grain and no ratchet publishes exactly the flags it always did") {
	// THE PIN FOR EVERY OTHER MODE'S FACE: at Zone A 0 the plan draws neither (`effectiveZoneA8 < 2`), so
	// the new bits must be zero and the new float zero — and in Burst and Grain, which never cut
	// the bar through `advanceSlice`, they are zero at ANY zone.
	const auto check = [](Mode mode, float za) {
		CAPTURE(static_cast<int>(mode));
		CAPTURE(za);
		Settings cfg = mode == Mode::Burst ? burst(0.5f) : looper(mode, 0.6f);
		cfg.mode = mode;
		cfg.zoneA = za;
		cfg.engage = false;
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		Published p;
		listen(s, p);
		s.setTarget(cfg);
		drive(s, 4800);
		s.publishReadings();
		const auto steps = static_cast<size_t>(std::lround(p.at(Reading::Steps)));
		REQUIRE(steps > 0u);
		constexpr uint32_t kOld = static_cast<uint32_t>(StepFlag::Reversed) | static_cast<uint32_t>(StepFlag::PitchedUp)
		                          | static_cast<uint32_t>(StepFlag::Dry) | static_cast<uint32_t>(StepFlag::Known);
		for (size_t k = 0; k < steps; ++k) {
			CAPTURE(k);
			const StepFace f = stepFace(p, k);
			CHECK((f.flags & ~kOld) == 0u);
			CHECK(f.piece == 0.0f);
		}
	};
	for (const Mode m : { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch, Mode::Grain }) {
		check(m, 0.0f);
	}
	for (const Mode m : { Mode::Burst, Mode::Grain }) {
		check(m, 0.95f);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE REPEAT HOLD AND TIME'S ZONE OVERRIDE — ported 2026-09-23. Both are the                        //
// reference's (`stutterer.cpp:690-751`, `:703-712`, `:1052`) and neither had been ported.             //
//--------------------------------------------------------------------------------------------------//

namespace {

/// Every slot the head starts, for `slots` slots, as the device reports it after the slot's setup.
struct SlotSeen {
	int32_t slice = 0;
	int32_t numSlices = 0;
	SlicePlan plan {};
	SliceSchedule schedule {};
	int32_t holdLeft = 0;
	size_t barFrames = 0;
	size_t bufferFrames = 0;
	size_t minSliceFrames = 0;
	/// How many steps the slot at `i` spans with plan `p`, on this device's grid — the walk's step.
	[[nodiscard]] int32_t spanOf(Mode mode, float zoneA, int32_t i, const SlicePlan& p) const {
		return scheduleFor(mode, zoneA, i, numSlices, barFrames, p, bufferFrames, minSliceFrames).grainLength;
	}
};

std::vector<SlotSeen> slotsOf(const Settings& cfg, size_t slots) {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);
	std::vector<SlotSeen> seen;
	int last = -1;
	for (int k = 0; k < 40000 && seen.size() < slots; ++k) {
		drive(s, 96, 0.0f, &beats);
		if (s.sliceIndexForTest() == last) {
			continue;
		}
		last = s.sliceIndexForTest();
		seen.push_back({ s.sliceIndexForTest(), s.sliceCountForTest(), s.planForTest(), s.scheduleForTest(),
		                 s.holdLeftForTest(), s.sliceFramesForTest(), s.bufferFramesForTest(),
		                 s.minSliceFramesForTest() });
	}
	return seen;
}

} // namespace

TEST_CASE("PARITY: the repeat hold — a fresh plan replays for repeatSlices slots, and the bar's end cancels it") {
	// THE PUCK AT THE CENTRE throughout: the hold is the zones' (`repeatSlices` is Zone A's hash, or in Time
	// Zone B's count), so it is asserted where the ratchet question does not reach.
	size_t holdsBegun = 0;    // slots that cached a plan for later slots
	size_t heldDiffers = 0;   // slots playing a cached plan that a fresh draw there would NOT have given
	size_t freshAtBarTop = 0; // slot 0 of a bar, which is always fresh
	size_t checked = 0;
	for (const Mode mode : { Mode::Shuffle, Mode::Pattern, Mode::Pitch, Mode::Time }) {
		for (const float za : { 0.3f, 0.55f, 0.8f }) {
			for (const float zb : { 0.1f, 0.45f }) {
				for (const float rate : { 0.5f, 0.8f }) {
					CAPTURE(static_cast<int>(mode));
					CAPTURE(za);
					CAPTURE(zb);
					CAPTURE(rate);
					Settings cfg = looper(mode, rate);
					cfg.zoneA = za;
					cfg.zoneB = zb;
					cfg.seqBars = 1; // every bar is bar 0, so the draw below needs no bar counter
					const auto draw = [&](int32_t i) { return planFromPush(cfg, restPush(), i, 0, false); };
					for (const SlotSeen& slot : slotsOf(cfg, 48)) {
						CAPTURE(slot.slice);
						const auto span = [&](int32_t i, const SlicePlan& p) { return slot.spanOf(mode, za, i, p); };
						int32_t counter = -1;
						const SlicePlan want =
						    referenceHeldPlan(mode, za, zb, slot.numSlices, slot.slice, draw, span, &counter);
						CHECK(slot.plan == want);
						CHECK(slot.holdLeft == counter);
						++checked;
						// A FRESH DRAW AT THIS SLOT, Time's override applied as the reference applies it to one.
						SlicePlan freshHere = draw(slot.slice);
						if (mode == Mode::Time) {
							freshHere.grainLength = want.grainLength;
							freshHere.repeatSlices = want.repeatSlices;
						}
						if (slot.slice == 0) {
							// THE BAR'S END CANCELLED WHATEVER WAS HELD: the top of a bar is a fresh draw.
							CHECK(slot.plan == freshHere);
							++freshAtBarTop;
						}
						heldDiffers += !(freshHere == slot.plan) ? 1u : 0u;
						holdsBegun += slot.holdLeft > 0 ? 1u : 0u;
					}
				}
			}
		}
	}
	MESSAGE("slots " << checked << ", holds begun " << holdsBegun << ", held and unlike a fresh draw "
	                 << heldDiffers << ", bar tops " << freshAtBarTop);
	// NOT VACUOUS: holds began, a held plan was one a fresh draw would NOT have given (or the equality above
	// could not tell a hold from no hold), and bar tops were seen.
	CHECK(holdsBegun > 0u);
	CHECK(heldDiffers > 0u);
	CHECK(freshAtBarTop > 0u);
}

TEST_CASE("PARITY: Repeat never holds — the reference's hold is `!isRepeat`") {
	// Repeat draws at its own counter every slot (`repeatLoopIndex_`), so a hold there would freeze the one
	// figure it repeats. At a Zone A where the other modes hold often.
	Settings cfg = looper(Mode::Repeat, rateForRepeats(8));
	cfg.zoneA = 0.8f;
	cfg.zoneB = 0.45f;
	size_t seen = 0;
	for (const SlotSeen& slot : slotsOf(cfg, 24)) {
		CHECK(slot.holdLeft == 0);
		++seen;
	}
	CHECK(seen == 24u);
}

TEST_CASE("PARITY: Time's zones are its combine and its hold — stutterer.cpp:703-712") {
	// RESTATED from the reference, not from `applyTimeOverride`:
	//     combine = 1 + (int32_t)(zoneANorm * (scatterNumSlices - 1));  grainLength  = clamp(combine, 1, N)
	//     repeat  = 1 + (int32_t)(zoneBNorm * (scatterNumSlices - 1));  repeatSlices = clamp(repeat, 1, N)
	// and then the slot: its source is `(slice / repeatSlices)` plus the plan's offset (the stretch,
	// 1111 2222), its span `min(grainLength, steps left, slices left)`.
	size_t longSeen = 0;
	size_t stretchSeen = 0;
	size_t checked = 0;
	for (const float za : { 0.0f, 0.2f, 0.45f, 0.9f }) {
		for (const float zb : { 0.0f, 0.3f, 0.75f }) {
			for (const float rate : { 0.3f, 0.5f, 0.8f }) {
				CAPTURE(za);
				CAPTURE(zb);
				CAPTURE(rate);
				Settings cfg = looper(Mode::Time, rate);
				cfg.zoneA = za;
				cfg.zoneB = zb;
				cfg.seqBars = 1;
				for (const SlotSeen& slot : slotsOf(cfg, 32)) {
					CAPTURE(slot.slice);
					const int32_t n = slot.numSlices;
					const int32_t combine = std::clamp(1 + static_cast<int32_t>(za * static_cast<float>(n - 1)), 1, n);
					const int32_t repeat = std::clamp(1 + static_cast<int32_t>(zb * static_cast<float>(n - 1)), 1, n);
					CHECK(slot.plan.grainLength == combine);
					CHECK(slot.plan.repeatSlices == repeat);
					// THE SPAN: the combine, clamped to the steps and the slices that remain. The source ranges over
					// the buffer at the bar's grid (`scheduleFor`), which is the reference's `N` at a one-bar buffer.
					const auto held = static_cast<int32_t>(std::max<long long>(
					    1, std::llround(static_cast<double>(n) * static_cast<double>(slot.bufferFrames)
					                    / static_cast<double>(slot.barFrames))));
					const int32_t base = slot.slice / repeat;
					int32_t target = (base + ((slot.plan.sliceOffset * held) >> 4)) % held;
					if (slot.plan.shouldSkip) {
						target = ((slot.plan.skipTarget * held) >> 4) % held;
					}
					CHECK(slot.schedule.targetSlice == target);
					CHECK(slot.schedule.grainLength
					      == std::max(1, std::min({ combine, n - slot.slice, held - target })));
					++checked;
					longSeen += slot.schedule.grainLength > 1 ? 1u : 0u;
					stretchSeen += repeat > 1 && base != slot.slice ? 1u : 0u;
				}
			}
		}
	}
	MESSAGE("slots " << checked << ", long " << longSeen << ", stretched " << stretchSeen);
	CHECK(longSeen > 0u);
	CHECK(stretchSeen > 0u);
}

//--------------------------------------------------------------------------------------------------//
// THE PUCK AT REST CARRIES A RATCHET — provisional, pending a listening decision: the circle's      //
// centre has `subdivMix` 0.5.                                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **WHAT `subdivMix` 0.5 MAKES OF A PLAN'S RATCHET, RESTATED FROM THE REFERENCE'S LINE** —
/// `subdivisions = 1 + (int)((targetSub - 1) * subdivMix)` (`dsp/scatter.hpp:1189`) — with `targetSub` read
/// from the same plan drawn at `subdivMix` 1.0, where that line gives `targetSub` itself.
int32_t ratchetAtRest(int32_t targetSub) {
	return 1 + static_cast<int32_t>(static_cast<float>(targetSub - 1) * 0.5f);
}

} // namespace

TEST_CASE("RATCHETS AT REST: a centred puck plays Zone A's own ratchet count, and Zone A 0 plays none") {
	// Asserted on the PLAN'S VALUE at every slice, not on a buffer. `targetSub` is `min(2·baseSub, 12)`, so
	// where it is below 12 the count at rest is exactly `baseSub` = `targetSub / 2` — the hash's own count,
	// which is the point of 0.5 — and at 12 (baseSub 6, 8 or 12) it is 6. `targetSub` 2 is `baseSub` 1 (neither
	// hash fired), which is no ratchet at rest: `1 + ⌊1 · 0.5⌋`.
	const Settings rest; // the puck centred, depth at its default
	const MacroPush push = performPushOf(rest, performWeightsOf(rest.perfX, rest.perfY), rest.perfDepth);
	REQUIRE(push == restPush());
	MacroPush full = restPush();
	full.subdivMix = 1.0f; // where the reference's line returns `targetSub` itself
	const Coordinate mc { 0.0f, 0.0f };
	size_t ratchets = 0;
	size_t equalToBase = 0;
	size_t plans = 0;
	for (int za = 1; za < 20; ++za) {
		for (int zb = 0; zb < 10; ++zb) {
			const Coordinate a { static_cast<float>(za) / 20.0f, 0.0f };
			const Coordinate b { static_cast<float>(zb) / 10.0f, 0.0f };
			for (int32_t bar = 0; bar < 4; ++bar) {
				const PhaseOffsets ofs = offsetsFor(a, b, mc, push, 0.0f, bar, false);
				for (int32_t slice = 0; slice < 16; ++slice) {
					CAPTURE(za);
					CAPTURE(zb);
					CAPTURE(slice);
					const SlicePlan p = planFor(a.position, b.position, mc.position, push, slice, ofs, nullptr);
					const SlicePlan atFull = planFor(a.position, b.position, mc.position, full, slice, ofs, nullptr);
					++plans;
					if (atFull.subdivisions <= 1) {
						// NO RATCHET TO DRAW — a repeat drew instead, or Zone A is in its off-switch.
						CHECK(p.subdivisions == 1);
						continue;
					}
					const int32_t target = atFull.subdivisions;
					CHECK(p.subdivisions == ratchetAtRest(target));
					if (target < 12) {
						CHECK(p.subdivisions == target / 2);
						++equalToBase;
					}
					else {
						CHECK(p.subdivisions == 6);
					}
					// THE MUTUAL EXCLUSION IS THE PLAN'S AND STANDS: a ratchet is drawn only where no repeat is.
					CHECK(p.repeatSlices == 1);
					ratchets += p.subdivisions > 1 ? 1u : 0u;
				}
			}
		}
	}
	MESSAGE("plans " << plans << ", ratchets at rest " << ratchets << ", equal to baseSub " << equalToBase);
	CHECK(ratchets > 0u);
	CHECK(equalToBase > 0u);

	SUBCASE("Zone A 0 is the off-switch, at rest as everywhere") {
		const Coordinate a { 0.0f, 0.0f };
		for (const float zb : { 0.0f, 0.4f, 0.9f }) {
			const Coordinate b { zb, 0.0f };
			const PhaseOffsets ofs = offsetsFor(a, b, mc, push, 0.0f, 0, false);
			for (int32_t slice = 0; slice < 32; ++slice) {
				CHECK(planFor(a.position, b.position, mc.position, push, slice, ofs, nullptr).subdivisions == 1);
			}
		}
	}
}

TEST_CASE("RATCHETS AT REST reach the playing device: a default puck, Shuffle, Zone A mid") {
	// The device's road, not the plan's: every slot the head starts is asked for its count, against the held
	// plan (`referenceHeldPlan`) drawn at `subdivMix` 1.0 and put through the reference's line at 0.5.
	//
	// **ZONE B ABOVE 0.3, AND THAT IS THE PLAN'S, NOT THIS FIXTURE'S.** At Zone B ≤ 0.3 the reference's stride
	// is zero, so every slice of a bar draws the SAME hash, and at bar 0 that hash drew no ratchet at any Zone A
	// — MEASURED over 19 Zone A × 4 Zone B ≤ 0.3, 32 slices each: none. The default Zone B is 0.
	Settings cfg = looper(Mode::Shuffle, 0.8f);
	cfg.zoneA = 0.5f;
	cfg.zoneB = 0.4f;
	cfg.seqBars = 1;
	MacroPush full = restPush();
	full.subdivMix = 1.0f;
	size_t ratchets = 0;
	for (const SlotSeen& slot : slotsOf(cfg, 96)) {
		CAPTURE(slot.slice);
		const auto span = [&](int32_t i, const SlicePlan& p) { return slot.spanOf(cfg.mode, 0.5f, i, p); };
		const SlicePlan atFull = referenceHeldPlan(
		    cfg.mode, 0.5f, 0.4f, slot.numSlices, slot.slice,
		    [&](int32_t i) { return planFromPush(cfg, full, i, 0, false); }, span);
		const int32_t want = atFull.subdivisions > 1 ? ratchetAtRest(atFull.subdivisions) : 1;
		CHECK(slot.plan.subdivisions == want);
		ratchets += slot.plan.subdivisions > 1 ? 1u : 0u;
	}
	MESSAGE("ratchets in 96 slots at rest: " << ratchets);
	CHECK(ratchets > 0u);
}

//--------------------------------------------------------------------------------------------------//
// RIBBON TRUTH — the playing sound did not always match the ribbon. The                             //
// case above checks the band against the device's own schedule and plan, which is the device agreeing  //
// with itself. This one checks it against what the looper READ: identity-coded input, the ring frame   //
// of every sample's read (`setReadTraceForTest`), and the output, slot by slot, against the cell the   //
// band published for the head's step at a real host's block ends.                                      //
//--------------------------------------------------------------------------------------------------//

namespace {

struct TruthSetting {
	Mode mode = Mode::Shuffle;
	float zoneA = 0.0f;
	float zoneB = 0.0f;
	float puck = -1.0f; ///< the second waypoint's configuration with the puck on it; below zero, centred
	bool swing = false;
	bool locked = false;
	float density = 1.0f;
	/// **TURN THINGS WHILE IT PLAYS** — MID-BAR, three eighths into the fifth: Zone B (the plan and, in Time,
	/// the hold), density to 0.6, the Send Override to −50%. A band keyed on less than the walk reads keeps
	/// the old picture (IB's shape), and an engine carrying a hold across the turn plays one (decision (a)).
	bool change = false;
	int32_t seqBars = 0; ///< 0 leaves the descriptor's default (four bars)
	/// Repeat's cut. EIGHT to the bar keeps its plan counter (mod 8) in step with the step for ever, so a
	/// band planning at the wrong counter would agree by coincidence; five and three make them part company.
	int repeats = 8;
	/// With `change`, the groove turned to the other feel too — which, under the lock, moves the grid under
	/// the head.
	bool turnGroove = false;
};

std::string truthName(const TruthSetting& t) {
	char line[160] = {};
	std::snprintf(line, sizeof(line), "%s A%.2f B%.2f puck%s%.1f %s lock%s dens%.1f%s", kModeNames[static_cast<size_t>(t.mode)],
	              static_cast<double>(t.zoneA), static_cast<double>(t.zoneB), t.puck < 0.0f ? "C" : "",
	              static_cast<double>(std::max(0.0f, t.puck)), t.swing ? "Swing16" : "Straight", t.locked ? "On" : "Off",
	              static_cast<double>(t.density), t.change ? (t.turnGroove ? " turned+groove" : " turned") : "");
	return std::string(line) + (t.seqBars > 0 ? " seq" + std::to_string(t.seqBars) : "")
	       + (t.mode == Mode::Repeat && t.repeats != 8 ? " x" + std::to_string(t.repeats) : "");
}

/// **WHAT THE AUDIT FOUND, BY FIELD.** A field is counted once per slot however many of its publishes
/// disagreed; `first` is the first offender, for the report.
struct TruthTally {
	std::map<std::string, size_t> mismatches;
	std::map<std::string, std::string> first;
	size_t slots = 0;
	size_t ratchets = 0; ///< slots that restarted, heard
	size_t longs = 0;    ///< slots longer than one step
	size_t stacked = 0;
	size_t repeats = 0;  ///< Repeat-mode slots
	size_t dry = 0;
	size_t reversed = 0;
	size_t confirmed = 0; ///< slots with a sample bit-equal to what the trace says was read
	size_t ahead = 0;     ///< slots whose walk-drawn cell, published before they began, was judged too
	void add(const TruthTally& o) {
		for (const auto& [k, v] : o.mismatches) {
			mismatches[k] += v;
			if (first.count(k) == 0) {
				first[k] = o.first.at(k);
			}
		}
		slots += o.slots;
		ratchets += o.ratchets;
		longs += o.longs;
		stacked += o.stacked;
		repeats += o.repeats;
		dry += o.dry;
		reversed += o.reversed;
		confirmed += o.confirmed;
		ahead += o.ahead;
	}
};

/// A signed ring distance `a − b`, folded into (−L/2, L/2].
long long ringDelta(long long a, long long b, long long len) {
	long long d = ((a - b) % len + len) % len;
	return d > len / 2 ? d - len : d;
}

TruthTally ribbonTruth(const TruthSetting& t, int block) {
	Settings cfg = looper(t.mode, t.mode == Mode::Repeat ? rateForRepeats(t.repeats) : 0.5f);
	cfg.zoneA = t.zoneA;
	cfg.zoneB = t.zoneB;
	cfg.density = t.density;
	cfg.transportLocked = t.locked;
	if (t.seqBars > 0) {
		cfg.seqBars = t.seqBars;
	}
	if (t.puck >= 0.0f) {
		withMacro(cfg, 1.0f, t.puck);
	}
	setGroove(cfg, static_cast<int>(t.swing ? GrooveMode::Swing16 : GrooveMode::Straight), t.swing ? 0.64 : 0.0);
	Sklitter s;
	s.prepare(kRate, 4096);
	Published p;
	listen(s, p);
	s.setTarget(cfg);

	// **THE HOST, AS `HostRig` DRIVES IT:** the song frame counts, each block's clock is derived from it, the
	// input names its own frame, and the readings are published after every block as the rack does.
	std::vector<Sklitter::ReadTraceEntry> trace(static_cast<size_t>(block));
	s.setReadTraceForTest(trace.data(), trace.size());
	constexpr long long kBar = 96000;
	constexpr long long kFrames = 7 * kBar;
	constexpr long long kChangeAt = 4 * kBar + 36 * 1024; // mid-bar, and a block boundary at 512 and at 1024
	Settings changed = cfg;
	changed.density = 0.6f;
	changed.sendOverride = -0.5f;
	changed.zoneB = cfg.zoneB > 0.5f ? cfg.zoneB - 0.45f : cfg.zoneB + 0.45f;
	if (t.turnGroove) {
		setGroove(changed, static_cast<int>(t.swing ? GrooveMode::Straight : GrooveMode::Swing16), t.swing ? 0.0 : 0.64);
	}
	std::vector<Sklitter::ReadTraceEntry> all;
	std::vector<float> out;
	std::vector<float> outR;
	outR.reserve(static_cast<size_t>(kFrames));
	std::vector<float> in;
	all.reserve(static_cast<size_t>(kFrames));
	out.reserve(static_cast<size_t>(kFrames));
	in.reserve(static_cast<size_t>(kFrames));
	std::vector<std::pair<long long, std::vector<float>>> publishes; // (the block's last frame, its readings)
	std::vector<float> l(static_cast<size_t>(block));
	std::vector<float> r(static_cast<size_t>(block));
	for (long long frame = 0; frame < kFrames; frame += block) {
		for (int k = 0; k < block; ++k) {
			// `frameCode`'s sign flips every frame, which an octave up (every OTHER frame) reads as DC: the
			// crossing gate then waits the whole slot and the slot is silent. A period of three crosses under
			// any read step the device uses.
			const long long f = frame + k;
			l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] =
			    (kFrameBase + static_cast<float>(f)) * (f % 3 == 0 ? -1.0f : 1.0f);
			in.push_back(l[static_cast<size_t>(k)]);
		}
		std::fill(trace.begin(), trace.end(), Sklitter::ReadTraceEntry {});
		if (t.change && frame == kChangeAt) {
			s.setTarget(changed);
		}
		s.setTransportClock(at120bpm(static_cast<double>(frame) / 24000.0));
		s.processStereo(l.data(), r.data(), block);
		s.publishReadings();
		all.insert(all.end(), trace.begin(), trace.end());
		out.insert(out.end(), l.begin(), l.end());
		outR.insert(outR.end(), r.begin(), r.end()); // a hard pan silences one side (Repeat's alternate bars)
		publishes.emplace_back(frame + block - 1, p.v);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	const auto len = static_cast<long long>(s.ringForTest().length());
	const auto buffer = static_cast<long long>(s.bufferFramesForTest());
	REQUIRE(len == buffer);
	const auto bar = static_cast<long long>(s.sliceFramesForTest());
	const GrooveFeel feelBefore = cfg.grooveFeel();
	const GrooveFeel feelAfter = t.change ? changed.grooveFeel() : feelBefore;

	TruthTally tally;
	const std::string name = truthName(t);
	const auto miss = [&](const std::string& field, long long at, const std::string& what) {
		if (tally.mismatches[field]++ == 0) {
			tally.first[field] = name + " @" + std::to_string(at) + ": " + what;
		}
	};
	const auto heard = [&](long long from) {
		for (long long j = from; j < from + 64 && j < static_cast<long long>(out.size()); ++j) {
			if (out[static_cast<size_t>(j)] != 0.0f || outR[static_cast<size_t>(j)] != 0.0f) {
				return true;
			}
		}
		return false;
	};

	// THE SLOTS: runs of samples with one slice whose position climbs from 0.
	size_t publishIdx = 0;
	for (long long b = 0; b < static_cast<long long>(all.size());) {
		const Sklitter::ReadTraceEntry& e0 = all[static_cast<size_t>(b)];
		long long e = b + 1;
		while (e < static_cast<long long>(all.size()) && all[static_cast<size_t>(e)].slice == e0.slice
		       && all[static_cast<size_t>(e)].position == all[static_cast<size_t>(e - 1)].position + 1) {
			++e;
		}
		const bool whole = e0.slice >= 0 && e < static_cast<long long>(all.size());
		const long long slotB = b;
		b = e;
		if (!whole) {
			continue; // before the play, or the run's last slot
		}
		if (t.change && slotB < kChangeAt && e > kChangeAt) {
			continue; // planned under the old controls and published under the new: neither picture is its own
		}
		// **ENTERED PART WAY IN** — a seek, the lock, or an arrival that JOINED a long grain (decision (a)). Its
		// extent, dry and cell are checked; where it reads, which way and how often it restarts are a whole
		// slot's questions and are asked only of whole slots.
		const bool partial = e0.position != 0;
		const GrooveFeel& feel = t.change && slotB >= kChangeAt ? feelAfter : feelBefore;
		const long long slotLen = (e - slotB) + static_cast<long long>(e0.position);

		// WHAT PLAYED.
		bool dry = true;
		for (long long j = slotB; j < e; ++j) {
			dry = dry && all[static_cast<size_t>(j)].dry;
		}
		const long long at0 = e0.at >= 0 ? e0.at % len : -1;
		int restarts = 1;
		long long firstRestart = -1;
		bool reversed = false;
		long long lowest = 0; // the first piece's lowest frame, relative to `at0`
		if (!dry && at0 >= 0) {
			bool sawStep = false;
			// THE FIRST PIECE'S READS, UNWRAPPED step by step: a long grain can read most of the buffer, and a
			// distance folded against `at0` alone flips sign past half of it.
			long long unwrapped = 0;
			long long prevAt = at0;
			for (long long j = slotB + 1; j < e; ++j) {
				const Sklitter::ReadTraceEntry& x = all[static_cast<size_t>(j)];
				if (x.at < 0) {
					continue;
				}
				const long long d = ringDelta(x.at % len, at0, len);
				unwrapped += ringDelta(x.at % len, prevAt, len);
				prevAt = x.at % len;
				if (x.sub != all[static_cast<size_t>(j - 1)].sub) {
					// A PIECE BOUNDARY IS A RESTART ONLY IF THE READ REALLY WENT BACK, AND ONLY IF IT WAS HEARD.
					if (d == 0 && heard(j)) {
						++restarts;
						firstRestart = firstRestart < 0 ? j : firstRestart;
					}
					continue;
				}
				if (x.sub == 0) {
					lowest = std::min(lowest, unwrapped);
					if (!sawStep && d != 0) {
						sawStep = true;
						reversed = d < 0;
					}
				}
			}
		}
		// **THE OUTPUT SAYS THE SAME — the trace is only evidence if it is what was heard.** A sample at unity
		// gain is BIT-EQUAL to the ring frame read (a wet slot) or to the input (a dry one); `frameOf`'s
		// whole-code test cannot say that here, because a float near 10^6 is a multiple of 1/16 and one gained
		// sample in sixteen decodes as whole. So a slot is CONFIRMED when some sample of it is bit-equal to
		// what the trace says it read — and a trace that named the wrong frames would confirm none.
		bool confirmed = false;
		for (long long j = slotB; j < e && !confirmed; ++j) {
			const Sklitter::ReadTraceEntry& x = all[static_cast<size_t>(j)];
			float ringL = 0.0f;
			float ringR = 0.0f;
			if (x.at >= 0) {
				s.ringForTest().readAt(static_cast<size_t>(x.at), &ringL, &ringR);
			}
			for (const float heardNow : { out[static_cast<size_t>(j)], outR[static_cast<size_t>(j)] }) {
				confirmed = confirmed
				            || (heardNow != 0.0f
				                && (x.dry ? heardNow == in[static_cast<size_t>(j)] : (x.at >= 0 && heardNow == ringL)));
			}
		}
		// THE EXTENT, ON THE DEVICE'S GRID: how many of its steps this slot's length is.
		const int32_t n = s.sliceCountForTest();
		int span = 1;
		{
			long long best = LLONG_MAX;
			for (int m = 1; e0.slice + m <= n; ++m) {
				const auto edgeLen = static_cast<long long>(grooveBoundary(feel, e0.slice + m, n, static_cast<size_t>(bar)))
				                     - static_cast<long long>(grooveBoundary(feel, e0.slice, n, static_cast<size_t>(bar)));
				if (std::llabs(slotLen - edgeLen) < best) {
					best = std::llabs(slotLen - edgeLen);
					span = m;
				}
			}
		}
		const double source = static_cast<double>(((at0 + lowest) % len + len) % len) / static_cast<double>(buffer);
		const double piece = firstRestart >= 0 ? static_cast<double>(firstRestart - slotB) / static_cast<double>(slotLen) : 0.0;

		++tally.slots;
		tally.ratchets += restarts > 1 ? 1u : 0u;
		tally.longs += span > 1 ? 1u : 0u;
		tally.stacked += restarts > 1 && span > 1 ? 1u : 0u;
		tally.repeats += t.mode == Mode::Repeat ? 1u : 0u;
		tally.dry += dry ? 1u : 0u;
		tally.reversed += reversed ? 1u : 0u;

		// WHAT THE BAND SAID, at every block end inside the slot — the first (is it late?) and the rest (does
		// it hold?). A field counts once per slot.
		while (publishIdx < publishes.size() && publishes[publishIdx].first < slotB) {
			++publishIdx;
		}
		tally.confirmed += confirmed ? 1u : 0u;
		bool anySound = false;
		for (long long j = slotB; j < e && !anySound; ++j) {
			anySound = out[static_cast<size_t>(j)] != 0.0f || outR[static_cast<size_t>(j)] != 0.0f;
		}
		std::map<std::string, std::string> wrong; // field -> what the band said
		// ONE CELL AGAINST WHAT PLAYED. `prefix` names the reading: "" for the band while the slot plays (the
		// head's cell, which the device sizes from the playing slot), "ahead-" for the band's WALK before it began.
		const auto judge = [&](const Published& pub, size_t step, const std::string& when, const std::string& prefix) {
			const auto steps = static_cast<size_t>(std::lround(pub.at(Reading::Steps)));
			const StepFace f = stepFace(pub, step);
			char said[160] = {};
			std::snprintf(said, sizeof(said), "%s step %zu: span %d ratchet %d piece %.4f rev %d dry %d src %.4f cov %d",
			              when.c_str(), step, f.span(), f.ratchet(), static_cast<double>(f.piece),
			              (f.flags & static_cast<uint32_t>(StepFlag::Reversed)) != 0u ? 1 : 0,
			              (f.flags & static_cast<uint32_t>(StepFlag::Dry)) != 0u ? 1 : 0, static_cast<double>(f.source),
			              f.covered() ? 1 : 0);
			// A DRY SLOT READS NOTHING, so its reverse, ratchet and source are claims about a read that never happened.
			const std::string tag = dry ? " (dry slot)" : "";
			if (f.covered()) {
				wrong.emplace(prefix + "covered-head", said);
			}
			if (f.span() != span) {
				wrong.emplace(prefix + "span", said);
			}
			for (int j = 1; j < f.span() && step + static_cast<size_t>(j) < steps; ++j) {
				if (!stepFace(pub, step + static_cast<size_t>(j)).covered()) {
					wrong.emplace(prefix + "covered", said);
				}
			}
			const bool pubDry = (f.flags & static_cast<uint32_t>(StepFlag::Dry)) != 0u;
			if (pubDry != dry) {
				wrong.emplace(prefix + "dry", said);
			}
			if (partial) {
				return; // see `partial`
			}
			if (f.ratchet() != restarts) {
				wrong.emplace(prefix + "ratchet" + tag, said);
			}
			if (restarts > 1 && f.ratchet() == restarts
			    && std::fabs(static_cast<double>(f.piece) - piece) > 2.0 / static_cast<double>(slotLen)) {
				wrong.emplace(prefix + "piece", said);
			}
			const bool pubReversed = (f.flags & static_cast<uint32_t>(StepFlag::Reversed)) != 0u;
			if (pubReversed != reversed) {
				wrong.emplace(prefix + "reversed" + tag, said);
			}
			if (!dry) {
				const long long off = ringDelta(std::llround(static_cast<double>(f.source) * static_cast<double>(buffer)),
				                                std::llround(source * static_cast<double>(buffer)), len);
				if (std::llabs(off) > 2) {
					wrong.emplace(prefix + "source", said);
				}
			}
		};
		size_t seen = 0;
		size_t headStep = SIZE_MAX;
		const auto perBar = static_cast<size_t>(std::max(1, n));
		for (size_t q = publishIdx; q < publishes.size() && publishes[q].first + 1 < e; ++q) {
			Published pub;
			pub.v = publishes[q].second;
			++seen;
			const auto steps = static_cast<size_t>(std::lround(pub.at(Reading::Steps)));
			const auto step = static_cast<size_t>(std::lround(pub.at(Reading::Step)));
			const std::string when = q == publishIdx ? "first publish" : "later publish";
			if (steps == 0 || step >= steps || step % perBar != static_cast<size_t>(e0.slice)) {
				wrong.emplace("head", when + ": step " + std::to_string(step) + " of " + std::to_string(steps) + ", per bar "
				                          + std::to_string(perBar));
				continue;
			}
			headStep = headStep == SIZE_MAX ? step : headStep;
			judge(pub, step, when, "");
		}
		// **AND THE WALK, BEFORE THE SLOT BEGAN** — the cell the band showed for this step while the slot before
		// it played. The playing cell takes its ratchet from the playing slot, so without this the walk's own
		// arithmetic would go unasked. Not across a turn (the walk moves at the turn, the arrangement a bar
		// later), and not for a slot entered part way.
		const bool settled = !(t.change && slotB >= kChangeAt && slotB < kChangeAt + kBar);
		if (headStep != SIZE_MAX && !partial && settled) {
			size_t q = publishIdx;
			while (q > 0 && publishes[q - 1].first + 1 >= slotB) {
				--q;
			}
			if (q > 0) {
				Published ahead;
				ahead.v = publishes[q - 1].second;
				const auto steps = static_cast<size_t>(std::lround(ahead.at(Reading::Steps)));
				// The band draws THIS pass of the sequence: a step behind the head it was published under is the
				// NEXT pass's, which in Repeat (its counter is mod 8, not mod the pass) can plan differently.
				const auto aheadHead = static_cast<size_t>(std::lround(ahead.at(Reading::Step)));
				if (headStep < steps && headStep > aheadHead) {
					judge(ahead, headStep, "ahead publish", "ahead-");
					++tally.ahead;
				}
			}
		}
		if (seen == 0) {
			wrong.emplace("unpublished", "no block ended inside the slot");
		}
		if (!anySound && !partial) {
			// THE BAND DRAWS EVERY SLOT AS SOMETHING THAT SOUNDS; one that is silent from end to end is not that.
			// (A slot ENTERED part way can be entered in its own cut tail, which the cell never claimed sounds.)
			wrong.emplace("silent", "a cell for a slot that made no sound");
		}
		for (const auto& [w, said] : wrong) {
			char what[360] = {};
			std::snprintf(what, sizeof(what), "PLAYED slice %d len %lld span %d restarts %d piece %.4f rev %d dry %d src %.4f; BAND %s",
			              e0.slice, slotLen, span, restarts, piece, reversed ? 1 : 0, dry ? 1 : 0, source, said.c_str());
			miss(w, slotB, what);
		}
	}
	return tally;
}

std::vector<TruthSetting> truthSweep() {
	std::vector<TruthSetting> out;
	for (const Mode mode : { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Pattern, Mode::Pitch }) {
		for (const auto& [za, zb] : std::initializer_list<std::pair<float, float>> {
		         { 0.05f, 0.8f }, { 0.55f, 0.3f }, { 0.55f, 0.8f }, { 0.95f, 0.3f } }) {
			for (const float puck : { -1.0f, 0.4f, 0.7f }) {
				for (const bool swing : { false, true }) {
					for (const bool locked : { false, true }) {
						out.push_back({ mode, za, zb, puck, swing, locked, 1.0f });
					}
				}
			}
			out.push_back({ mode, za, zb, 0.4f, false, false, 0.6f }); // density thins the stream
			out.push_back({ mode, za, zb, 0.7f, false, true, 1.0f, true }); // turned while it plays
			out.push_back({ mode, za, zb, 0.7f, false, true, 1.0f, true, 0, 8, true }); // and the groove with it
			out.push_back({ mode, za, zb, 0.4f, true, false, 1.0f, true, 0, 8, true }); // unlocked, from Swing
			out.push_back({ mode, za, zb, 0.4f, false, false, 1.0f, false, 1 }); // a sequence shorter than Time's phrase
			if (mode == Mode::Repeat) {
				out.push_back({ mode, za, zb, 0.4f, false, false, 1.0f, false, 0, 5 });
				out.push_back({ mode, za, zb, 0.7f, true, true, 1.0f, false, 1, 3 });
			}
		}
	}
	return out;
}

} // namespace

TEST_CASE("ribbon truth: every slot the device plays is the cell the band shows for the head's step — by what was read") {
	TruthTally all;
	std::map<std::string, TruthTally> byGroup; // mode / groove / lock, for the report
	for (const TruthSetting& t : truthSweep()) {
		const TruthTally one = ribbonTruth(t, t.change ? 1024 : 512);
		all.add(one);
		char group[80] = {};
		std::snprintf(group, sizeof(group), "%-8s %-8s lock%-3s%s%s", kModeNames[static_cast<size_t>(t.mode)],
		              t.swing ? "Swing16" : "Straight", t.locked ? "On" : "Off", t.density < 1.0f ? " density0.6" : "",
		              t.change ? (t.turnGroove ? " turned+groove" : " turned") : "");
		byGroup[std::string(group) + (t.seqBars > 0 ? " seq" + std::to_string(t.seqBars) : "")
		        + (t.mode == Mode::Repeat && t.repeats != 8 ? " x" + std::to_string(t.repeats) : "")]
		    .add(one);
	}
	std::string table = "\n";
	for (const auto& [group, g] : byGroup) {
		table += group + "  slots " + std::to_string(g.slots) + ":";
		for (const auto& [field, count] : g.mismatches) {
			table += " " + field + "=" + std::to_string(count);
		}
		table += "\n";
	}
	for (const auto& [field, what] : all.first) {
		table += "  first " + field + ": " + what + "\n";
	}
	MESSAGE("ribbon truth — slots " << all.slots << ", ratchets " << all.ratchets << ", long grains " << all.longs
	                                << ", stacked " << all.stacked << ", repeats " << all.repeats << ", dry " << all.dry
	                                << ", reversed " << all.reversed << ", confirmed by the output " << all.confirmed
	                                << ", walk judged ahead " << all.ahead
	                                << table);
	// NOT VACUOUS: each thing the band draws must have played.
	CHECK(all.ratchets > 0u);
	CHECK(all.longs > 0u);
	CHECK(all.stacked > 0u);
	CHECK(all.repeats > 0u);
	CHECK(all.reversed > 0u);
	CHECK(all.dry > 0u);
	// AND THE TRACE IS WHAT WAS HEARD: most slots have a sample bit-equal to the frame it names (the rest sit
	// under envelopes, pans and fades that never reach unity).
	CHECK(all.confirmed * 2u > all.slots);
	CHECK(all.ahead * 2u > all.slots); // and the walk, not only the playing cell, was asked of most of them
	for (const auto& [field, count] : all.mismatches) {
		CAPTURE(field);
		CAPTURE(all.first[field]);
		CHECK(count == 0u);
	}
}

TEST_CASE("ribbon truth (a): a control turned mid-hold takes effect at the next slot — the fresh walk under the CURRENT controls") {
	// DECISION (a): the plan a step plays is a function of the bar and the controls NOW.
	// The hold caches a plan; carried across a turn it replayed one the current controls would never draw,
	// until the bar's end, while the band (which walks the bar under the current controls) drew another.
	// Restated from the reference's walk (`referenceHeldPlan`) with the NEW controls, from step 0.
	namespace sc = feathers::fx::sklitter;
	const sc::Coordinate kNoKnobConfig { 0.0f, 0.0f };
	const auto fresh = [&](const Settings& cfg, int32_t index) {
		const sc::Coordinate a = sc::splitCoordinate(cfg.zoneA);
		const sc::Coordinate b = sc::splitCoordinate(cfg.zoneB);
		const MacroPush push = sc::performPushOf(cfg, sc::performWeightsOf(cfg.perfX, cfg.perfY), cfg.perfDepth);
		PlanCache cold;
		return planFor(a.position, b.position, kNoKnobConfig.position, push, index,
		               sc::offsetsFor(a, b, kNoKnobConfig, push, cfg.gamma, 0, false, cfg.sendOverride), &cold);
	};
	const auto same = [](const SlicePlan& x, const SlicePlan& y) {
		return x.subdivisions == y.subdivisions && x.repeatSlices == y.repeatSlices && x.grainLength == y.grainLength
		       && x.sliceOffset == y.sliceOffset && x.lengthMult == y.lengthMult && x.shouldReverse == y.shouldReverse
		       && x.shouldPitchUp == y.shouldPitchUp && x.useDry == y.useDry && x.shouldSkip == y.shouldSkip;
	};
	struct Turn {
		Mode mode;
		float zoneA;
		float zoneB;
		float turnedB; ///< Zone B after the turn: Time's hold length, everything's plan draw
	};
	size_t turned = 0;
	for (const Turn& turn : { Turn { Mode::Time, 0.1f, 0.8f, 0.2f }, Turn { Mode::Shuffle, 0.55f, 0.8f, 0.3f },
	                          Turn { Mode::Pattern, 0.55f, 0.8f, 0.3f } }) {
		CAPTURE(static_cast<int>(turn.mode));
		Settings cfg = looper(turn.mode, 0.5f);
		cfg.zoneA = turn.zoneA;
		cfg.zoneB = turn.zoneB;
		cfg.seqBars = 1; // the offsets' bar is 0 throughout
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm());
		s.setTarget(cfg);
		double beats = 0.0;
		drive(s, 96000 + 9600, 0.0f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		// A SLOT WHOSE HOLD STILL HAS SLOTS TO RUN, early enough in the bar that the next slot is in it.
		int last = s.sliceIndexForTest();
		bool found = false;
		for (int k = 0; k < 4000 && !found; ++k) {
			drive(s, 96, 0.0f, &beats);
			if (s.sliceIndexForTest() == last) {
				continue;
			}
			last = s.sliceIndexForTest();
			found = s.holdLeftForTest() > 0
			        && last + std::max<int32_t>(1, s.scheduleForTest().grainLength) < s.sliceCountForTest();
		}
		REQUIRE(found);
		const SlicePlan carried = s.planForTest(); // what the hold would replay next
		Settings now = cfg;
		now.zoneB = turn.turnedB;
		s.setTarget(now);
		const int from = s.sliceIndexForTest();
		for (int k = 0; k < 4000 && s.sliceIndexForTest() == from; ++k) {
			drive(s, 96, 0.0f, &beats);
		}
		const int32_t slice = s.sliceIndexForTest();
		REQUIRE(slice != from);
		REQUIRE(slice != 0); // still inside the bar, where only the turn can end the hold
		const float za = sc::splitCoordinate(now.zoneA).position;
		const int32_t n = s.sliceCountForTest();
		const SlicePlan want = referenceHeldPlan(
		    now.mode, za, sc::splitCoordinate(now.zoneB).position, n, slice, [&](int32_t i) { return fresh(now, i); },
		    [&](int32_t i, const SlicePlan& p) {
			    return scheduleFor(now.mode, za, i, n, s.sliceFramesForTest(), p, s.bufferFramesForTest(),
			                       s.minSliceFramesForTest())
			        .grainLength;
		    });
		CAPTURE(slice);
		CAPTURE(want.subdivisions);
		CAPTURE(want.repeatSlices);
		CAPTURE(s.planForTest().subdivisions);
		CAPTURE(s.planForTest().repeatSlices);
		// NOT VACUOUS: the hold, carried, would have played something else.
		REQUIRE_FALSE(same(carried, want));
		CHECK(same(s.planForTest(), want));
		++turned;
	}
	CHECK(turned == 3u);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG JC — REPITCHED GRAINS: artefacts on repitched grains, traced to the combination of the   //
// speed-up and the repeat.                                                                           //
//--------------------------------------------------------------------------------------------------//

namespace {

struct GrainTrace {
	std::vector<Sklitter::GrainTraceEntry> t;
	size_t ringLen = 0;
	size_t grainLen = 0;
};

/// Grain mode, played for `blocks` blocks of `kBlock` (960 unless asked) once it is Playing, with every
/// sample's two voice reads recorded. The block is under `kRenderChunk`, so the trace is one entry per sample.
GrainTrace traceGrain(const Settings& cfg, int blocks, int kBlock = 960) {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	std::vector<Sklitter::GrainTraceEntry> block(kBlock);
	s.setGrainTraceForTest(block.data(), block.size());
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	double beats = 0.0;
	GrainTrace out;
	int played = 0;
	for (int b = 0; b < blocks + 3 * 96000 / kBlock && played < blocks; ++b) { // up to three bars to engage
		ramp(l, r, static_cast<float>(b * kBlock));
		std::fill(block.begin(), block.end(), Sklitter::GrainTraceEntry {});
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kBlock);
		beats += kBlock / (kRate * 0.5);
		if (s.statusForTest() == Status::Playing) {
			++played;
			out.t.insert(out.t.end(), block.begin(), block.end());
		}
	}
	REQUIRE(played == blocks);
	out.ringLen = s.ringForTest().length();
	out.grainLen = s.grainLengthForTest();
	return out;
}

/// Voice A's grains in a trace, as [begin, end) sample ranges: a grain begins where `indexA` moves.
std::vector<std::pair<size_t, size_t>> grainsOfA(const GrainTrace& g) {
	std::vector<std::pair<size_t, size_t>> out;
	size_t begin = 0;
	for (size_t i = 1; i < g.t.size(); ++i) {
		if (g.t[i].indexA != g.t[i - 1].indexA) {
			if (begin > 0) {
				out.emplace_back(begin, i);
			}
			begin = i;
		}
	}
	return out;
}

Settings grainWithPitch() {
	Settings cfg = looper(Mode::Grain, 0.8f); // short grains, so a few seconds hold dozens
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.6f; // MEASURED: a third of voice-samples pitched up here, with the puck at rest
	return cfg;
}

} // namespace

TEST_CASE("backlog JC: a pitched-up grain reads by twos to its end and holds no frame — the firmware's three lines") {
	// **THE REFERENCE, RESTATED** (stutterer.cpp:1209-1211, 1311, 1319-1323): a voice's offset steps by
	// 2 when it is pitched up, and its envelope phase steps by twice `0xFFFFFFFF / grainLength` — so the
	// grain covers `grainLength` frames of buffer in HALF the samples, and ends as the window closes. The
	// port kept the first two lines and dropped the third: its window ran at 1x, the read reached the
	// grain's last frame at the window's PEAK, and held that one frame for the second half.
	const GrainTrace g = traceGrain(grainWithPitch(), 300);
	const auto grains = grainsOfA(g);
	const auto L = static_cast<long long>(g.grainLen);
	const auto ring = static_cast<long long>(g.ringLen);
	size_t pitched = 0;
	size_t plain = 0;
	size_t held = 0;
	size_t offStep = 0;
	for (const auto& [b, e] : grains) {
		const bool up = g.t[b].upA;
		const auto len = static_cast<long long>(e - b);
		CAPTURE(up);
		CAPTURE(len);
		CAPTURE(L);
		// THE LENGTH: half a grain pitched, a whole one plain — to the sample, give or take the phase's
		// rounding at the wrap.
		CHECK(std::llabs(len - (up ? L / 2 : L)) <= 1);
		for (size_t i = b + 1; i < e; ++i) {
			const long long d = ((g.t[i].atA - g.t[i - 1].atA) % ring + ring) % ring;
			const long long want = g.t[b].revA ? ring - (up ? 2 : 1) : (up ? 2 : 1);
			held += (d == 0) ? 1u : 0u;
			// THE STEP, by the firmware's integer offset. The port derives the offset from a double phase, so
			// one step in a grain may land a frame either side; a HOLD is a step of zero and is counted apart.
			offStep += (std::llabs(d - want) > 1) ? 1u : 0u;
		}
		// AND THE READ SPANS THE WHOLE GRAIN: the last frame read is the grain's own last frame (first,
		// reversed), which is what "reaches the end as the window closes" means in frames.
		const long long first = ((g.t[b].atA - static_cast<long long>(g.t[b].posA)) % ring + ring) % ring;
		const long long last = ((g.t[e - 1].atA - static_cast<long long>(g.t[e - 1].posA)) % ring + ring) % ring;
		CHECK(first == (g.t[b].revA ? L - 1 : 0));
		CHECK(std::llabs(last - (g.t[b].revA ? 0 : L - 1)) <= 2);
		(up ? pitched : plain) += 1;
	}
	CAPTURE(pitched);
	CAPTURE(plain);
	REQUIRE(pitched >= 10u); // NOT VACUOUS: the material pitched grains up, and some did not
	REQUIRE(plain >= 10u);
	CHECK(held == 0u);
	CHECK(offStep == 0u);
}

TEST_CASE("backlog JC: no frame of a pitched grain is held, in EITHER voice, anywhere the puck goes") {
	// **THE HOLD, COUNTED WHERE IT LIVED.** Before the fix this was exactly half of every pitched-up
	// voice-sample (156,000 of 312,000 here, puck at rest). Voice B is counted too — it is a different
	// read with its own flag, and a fix that reached A alone would pass the case above.
	for (const float amount : { 0.0f, 1.0f }) {
		Settings cfg = grainWithPitch();
		if (amount > 0.0f) {
			withMacro(cfg, amount, 0.4f);
		}
		const GrainTrace g = traceGrain(cfg, 300);
		const auto ring = static_cast<long long>(g.ringLen);
		size_t pitchedA = 0;
		size_t pitchedB = 0;
		size_t heldA = 0;
		size_t heldB = 0;
		for (size_t i = 1; i < g.t.size(); ++i) {
			const auto& p = g.t[i - 1];
			const auto& c = g.t[i];
			// A new grain begins where the window restarts; a held frame across that seam is a new grain
			// landing where the last ended, not a hold.
			if (c.upA && p.upA && c.envA > p.envA - 0.5f && c.indexA == p.indexA) {
				++pitchedA;
				heldA += (((c.atA - p.atA) % ring + ring) % ring == 0) ? 1u : 0u;
			}
			if (c.upB && p.upB && c.envB > p.envB - 0.5f) {
				++pitchedB;
				heldB += (((c.atB - p.atB) % ring + ring) % ring == 0) ? 1u : 0u;
			}
		}
		CAPTURE(amount);
		CAPTURE(pitchedA);
		CAPTURE(pitchedB);
		REQUIRE(pitchedA > 10000u);
		REQUIRE(pitchedB > 10000u);
		CHECK(heldA == 0u);
		CHECK(heldB == 0u);
	}
}

TEST_CASE("backlog JC: voice A replays its grain's position for the plan's repeatSlices grains — the firmware's counter") {
	// **THE REFERENCE, RESTATED** (stutterer.cpp:1342-1348), at voice A's wrap:
	//
	//     if (loopGrainRepeatCounter > 0) loopGrainRepeatCounter--;
	//     else { grainPosA = fastRandom() % spread; loopGrainRepeatCounter = loopGrainRepeatSlices - 1; }
	//
	// with the run's length taken from the plan of the grain that starts it (this port draws a plan per
	// grain; see `playGrain`). The plan is drawn here by `planFor` directly, not read back from the device,
	// so a device that took its count from anywhere else fails.
	namespace sc = feathers::fx::sklitter;
	Settings cfg = grainWithPitch();
	cfg.seqBars = 1; // one bar in the sequence, so every grain's plan is drawn at bar phase 0
	const GrainTrace g = traceGrain(cfg, 600);
	const auto plan = [&](int32_t index) {
		const sc::Coordinate a = sc::splitCoordinate(cfg.zoneA);
		const sc::Coordinate b = sc::splitCoordinate(cfg.zoneB);
		const sc::Coordinate none { 0.0f, 0.0f };
		const MacroPush push = sc::performPushOf(cfg, sc::performWeightsOf(cfg.perfX, cfg.perfY), cfg.perfDepth);
		PlanCache cold;
		return planFor(a.position, b.position, none.position, push, index,
		               sc::offsetsFor(a, b, none, push, cfg.gamma, 0, false, cfg.sendOverride), &cold);
	};
	const auto grains = grainsOfA(g);
	REQUIRE(grains.size() > 40u);
	// A grain whose trace reads `indexA == m` was started by the wrap that drew plan `m − 1`.
	int32_t left = -1; // unknown until the first observed fresh draw
	size_t repeated = 0;
	size_t fresh = 0;
	size_t wrong = 0;
	for (size_t k = 1; k < grains.size(); ++k) {
		const auto& cur = g.t[grains[k].first];
		const auto& prev = g.t[grains[k - 1].first];
		const bool samePos = cur.posA == prev.posA;
		const int32_t drawn = cur.indexA - 1;
		if (left < 0) {
			if (!samePos) {
				left = std::max<int32_t>(plan(drawn).repeatSlices, 1) - 1;
			}
			continue;
		}
		const bool wantRepeat = left > 0;
		CAPTURE(k);
		CAPTURE(drawn);
		CAPTURE(left);
		if (wantRepeat) {
			wrong += samePos ? 0u : 1u;
			--left;
			++repeated;
		}
		else {
			// A fresh draw may land where the last grain was by chance; with a spread of most of a bar
			// that is one in tens of thousands, so it is counted as wrong rather than excused.
			wrong += samePos ? 1u : 0u;
			left = std::max<int32_t>(plan(drawn).repeatSlices, 1) - 1;
			++fresh;
		}
	}
	CAPTURE(repeated);
	CAPTURE(fresh);
	REQUIRE(repeated >= 10u); // NOT VACUOUS: runs happened, and so did fresh draws
	REQUIRE(fresh >= 10u);
	CHECK(wrong == 0u);
}

TEST_CASE("backlog JC: a repeat run lasts its N grains ACROSS host blocks — the firmware's per-block reset is not ported") {
	// **THE DIVERGENCE, ASSERTED.** The reference's counter is a local, `int32_t loopGrainRepeatCounter = 0;`
	// at the top of every render call (stutterer.cpp:1171-1172), never written back — so a run is cut at the
	// next block boundary, against its own comment ("hold position for N grain cycles"). Treated as a firmware
	// bug: the counter here persists. So at a 64-frame block — where every grain spans
	// dozens of blocks and the reference's reset would end every run after its first grain — each run must
	// still last exactly the plan's `repeatSlices` grains, and each must cross block boundaries to count.
	namespace sc = feathers::fx::sklitter;
	constexpr int kBlock = 64;
	Settings cfg = grainWithPitch();
	cfg.seqBars = 1;
	const GrainTrace g = traceGrain(cfg, 96000 * 4 / kBlock, kBlock);
	const auto plan = [&](int32_t index) {
		const sc::Coordinate a = sc::splitCoordinate(cfg.zoneA);
		const sc::Coordinate b = sc::splitCoordinate(cfg.zoneB);
		const sc::Coordinate none { 0.0f, 0.0f };
		const MacroPush push = sc::performPushOf(cfg, sc::performWeightsOf(cfg.perfX, cfg.perfY), cfg.perfDepth);
		PlanCache cold;
		return planFor(a.position, b.position, none.position, push, index,
		               sc::offsetsFor(a, b, none, push, cfg.gamma, 0, false, cfg.sendOverride), &cold);
	};
	const auto grains = grainsOfA(g);
	REQUIRE(grains.size() > 40u);
	// Runs of voice A's grains at one position, each opened by a fresh draw (a position that moved).
	size_t runs = 0;
	size_t longRuns = 0;
	size_t wrongLength = 0;
	size_t k = 1;
	while (k < grains.size() && g.t[grains[k].first].posA == g.t[grains[k - 1].first].posA) {
		++k; // skip the run already under way when the trace began
	}
	while (k < grains.size()) {
		const size_t open = k;
		const size_t pos = g.t[grains[k].first].posA;
		while (k + 1 < grains.size() && g.t[grains[k + 1].first].posA == pos) {
			++k;
		}
		if (k + 1 >= grains.size()) {
			break; // the trace ended inside this run, so its length is unknown
		}
		const auto length = static_cast<int32_t>(k - open + 1);
		const int32_t want = std::max<int32_t>(plan(g.t[grains[open].first].indexA - 1).repeatSlices, 1);
		CAPTURE(open);
		CAPTURE(length);
		CAPTURE(want);
		wrongLength += (length != want) ? 1u : 0u;
		++runs;
		if (want > 1) {
			// It crossed block boundaries: its first sample and its last are many 64-frame blocks apart.
			const size_t blocksSpanned = (grains[k].second - 1) / kBlock - grains[open].first / kBlock;
			CHECK(blocksSpanned >= static_cast<size_t>(want));
			++longRuns;
		}
		++k;
	}
	CAPTURE(runs);
	CAPTURE(longRuns);
	REQUIRE(longRuns >= 5u); // NOT VACUOUS: runs of more than one grain happened and completed
	CHECK(wrongLength == 0u);
}

namespace {

/// A pitched arrangement under Dry Write 1, traced sample by sample: what each read heard, where it read,
/// where the head was, and the input the host handed in at every sample.
struct LiveRun {
	std::vector<Sklitter::ReadTraceEntry> trace;
	std::vector<float> in;
	std::vector<float> out;
	size_t ringLen = 0;
	size_t playFrom = 0; ///< the first sample of the first block that played
};

LiveRun liveRun(Mode m, float amount, float zoneA = 0.1f, float zoneB = 0.6f, float rate = 0.2f) {
	Settings cfg = looper(m, rate);
	cfg.zoneA = zoneA;
	cfg.zoneB = zoneB;
	cfg.dryWrite = 1.0f;
	if (amount > 0.0f) {
		withMacro(cfg, amount, 0.4f);
	}
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	constexpr int kBlock = 960;
	constexpr size_t kTotal = 96000 * 10;
	std::vector<Sklitter::ReadTraceEntry> block(kBlock);
	s.setReadTraceForTest(block.data(), block.size());
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	LiveRun run;
	run.playFrom = kTotal;
	for (size_t n = 0; n < kTotal; n += kBlock) {
		// A sine that does not repeat once a bar, so the head's fresh input and the bar-old frame ahead of
		// it differ — at 110 Hz they are the same sample and no stale read could be seen.
		for (int i = 0; i < kBlock; ++i) {
			l[static_cast<size_t>(i)] = static_cast<float>(
			    std::sin(2.0 * 3.14159265358979 * 113.3 * static_cast<double>(n + static_cast<size_t>(i)) / kRate));
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		run.in.insert(run.in.end(), l.begin(), l.end());
		std::fill(block.begin(), block.end(), Sklitter::ReadTraceEntry {});
		s.setTransportClock(at120bpm(static_cast<double>(n) / (kRate * 0.5)));
		s.processStereo(l.data(), r.data(), kBlock);
		if (s.statusForTest() == Status::Playing && run.playFrom == kTotal) {
			run.playFrom = n;
		}
		run.trace.insert(run.trace.end(), block.begin(), block.end());
		run.out.insert(run.out.end(), l.begin(), l.end());
	}
	run.ringLen = s.bufferFramesForTest();
	return run;
}

} // namespace

TEST_CASE("backlog JC: the DRY writer lands in the sample — every read hears what the head last wrote there") {
	// **THE CLAIM IS A VALUE, RESTATED FROM THE HEAD'S OWN WALK.** With Dry Write at 1 and nothing else
	// writing, a frame holds exactly the input the head handed it when it last passed. A read of frame
	// `at`, with the head at `head` before this sample's write, is `d = head − at` samples behind it (mod the
	// bar), so it must hear `in[i − d]` — or `in[i − bar]` when `d` is 0, the frame the head is about to
	// overwrite. With the blend deferred to the chunk's end, a read inside the chunk's own span heard the
	// bar-old frame instead, then the fresh one after the flush: a pitched read trailing the head by less
	// than a block flipped between two recordings at every block boundary.
	for (const Mode m : { Mode::Time, Mode::Shuffle, Mode::Pitch }) {
		for (const float amount : { 0.0f, 1.0f }) {
			CAPTURE(static_cast<int>(m));
			CAPTURE(amount);
			const LiveRun run = liveRun(m, amount);
			REQUIRE(run.playFrom < run.in.size());
			const auto len = static_cast<long long>(run.ringLen);
			size_t checked = 0;
			size_t sameChunk = 0;
			size_t stale = 0;
			long long firstStale = -1;
			// From a bar into the playing, so every frame has been written by the head at least once.
			for (size_t i = run.playFrom + run.ringLen; i < run.trace.size(); ++i) {
				const auto& t = run.trace[i];
				if (t.at < 0) {
					continue; // a dry slot reads nothing
				}
				const long long d = ((static_cast<long long>(t.head) - t.at) % len + len) % len;
				const size_t wrote = i - static_cast<size_t>(d == 0 ? len : d);
				const float want = run.in[wrote];
				++checked;
				// THE HAZARD ITSELF: written earlier in this same 960-frame block.
				sameChunk += (d > 0 && static_cast<size_t>(d) <= i % 960) ? 1u : 0u;
				if (std::abs(t.heardL - want) > 1e-6f) {
					firstStale = firstStale < 0 ? static_cast<long long>(i) : firstStale;
					++stale;
				}
			}
			CAPTURE(checked);
			CAPTURE(sameChunk);
			CAPTURE(firstStale);
			// NOT VACUOUS: reads DID land on frames the head wrote earlier in the same block, which is the
			// read a deferred dry writer served stale.
			REQUIRE(sameChunk > 1000u);
			CHECK(stale == 0u);
		}
	}
}

TEST_CASE("backlog JC: a read crossing the live write head is ducked to silence at the join, and no step survives it") {
	// **THE JOIN.** With Dry Write on, behind the head is the input of moments ago and ahead of it the input
	// of a bar ago; a read at any velocity but the head's passes through that cut. MEASURED before the duck
	// (after the dry writer stopped deferring): 17/76 Time, 8/13 Shuffle, 40/155 Pitch clicks at the puck's
	// rest / on E at 0.4, each a read passing `linearBarPos_`.
	//
	// **TWO ASSERTIONS, THE VALUE FIRST.** At every crossing — two consecutive reads of one piece with the
	// head between them — the wet's gain is at most the ramp's first step. Then, as a second assertion that
	// the first is not vacuous about what is heard: no wet sample steps more than 8x the input's own step.
	const double inputStep = 2.0 * 3.14159265358979 * 113.3 / kRate;
	size_t totalCrossings = 0;
	for (const Mode m : { Mode::Time, Mode::Shuffle, Mode::Pitch }) {
		for (const float amount : { 0.0f, 1.0f }) {
			CAPTURE(static_cast<int>(m));
			CAPTURE(amount);
			size_t crossings = 0;
			float worstGainAtJoin = 0.0f;
			size_t clicks = 0;
			double worstRatio = 0.0;
			// Two arrangements the sweep found crossings in, both at Zone B's pitch-ups and a mid rate: one
			// where the read passes the head outright, one where a loop's wrap crossfade carries its TAIL
			// across it. Counted together, so a mode that crosses in either is enough.
			struct Arr {
				float zoneA;
				float zoneB;
				float rate;
			};
			for (const Arr a : { Arr { 0.35f, 0.6f, 0.5f }, Arr { 0.1f, 0.6f, 0.5f } }) {
			const LiveRun run = liveRun(m, amount, a.zoneA, a.zoneB, a.rate);
			const auto len = static_cast<long long>(run.ringLen);
			const auto signedDelta = [&](const Sklitter::ReadTraceEntry& t) {
				long long d = ((t.at - static_cast<long long>(t.head)) % len + len) % len;
				return d > len / 2 ? d - len : d;
			};
			for (size_t i = run.playFrom + 1; i < run.trace.size(); ++i) {
				const auto& p = run.trace[i - 1];
				const auto& c = run.trace[i];
				if (p.at < 0 || c.at < 0 || p.slice != c.slice || p.sub != c.sub) {
					continue;
				}
				const long long dp = signedDelta(p);
				const long long dc = signedDelta(c);
				// A crossing: the head lies between the two reads, and they are near it (not a wrap jump
				// that happens to straddle the far side of the ring).
				if (((dp < 0 && dc >= 0) || (dp > 0 && dc <= 0)) && std::llabs(dp) < 64 && std::llabs(dc) < 64) {
					++crossings;
					worstGainAtJoin = std::max(worstGainAtJoin, std::min(p.duck, c.duck));
				}
				const double ratio = std::abs(static_cast<double>(run.out[i]) - static_cast<double>(run.out[i - 1])) / inputStep;
				worstRatio = std::max(worstRatio, ratio);
				clicks += ratio > 8.0 ? 1u : 0u;
			}
			}
			CAPTURE(crossings);
			CAPTURE(worstGainAtJoin);
			CAPTURE(clicks);
			CAPTURE(worstRatio);
			// At 1 ms and |v| ≥ 1 the gain is at most 1/48 one frame from the join; 0.05 leaves room for |v| < 2.
			CHECK(worstGainAtJoin < 0.05f);
			CHECK(clicks == 0u);
			totalCrossings += crossings;
		}
	}
	// NOT VACUOUS: main reads did cross the head. (Shuffle's crossings in these arrangements are a wrap
	// crossfade's TAIL passing it, which the trace's main read does not show; its click count is the
	// assertion that covers it.)
	CAPTURE(totalCrossings);
	CHECK(totalCrossings >= 10u);
}

TEST_CASE("backlog JC: in GRAIN, a voice crossing the live write head is ducked to silence at the join") {
	// The looper's crossing duck, per Grain voice: a pitched (2 frames a sample) or reversed voice passes the
	// head, a forward 1x one never does. MEASURED before it, Dry Write 1: 23/103/124 clicks at the puck's
	// rest / on E at 0.4 / half way to E at 0.1.
	//
	// **ASSERTED ON WHAT THE VOICE IS HEARD AT — ITS WINDOW TIMES ITS DUCK.** A grain that STARTS within the
	// duck's reach of the head arrives there before the slewed gain has fallen; its window is what is silent
	// then, a few samples into a triangle. MEASURED with the repeat counter knocked out (so voice A's grains
	// land elsewhere): one such crossing at duck 0.417 and window 0.029, so heard at 0.012.
	for (const float amount : { 0.0f, 1.0f }) {
		CAPTURE(amount);
		Settings cfg = grainWithPitch();
		cfg.dryWrite = 1.0f;
		if (amount > 0.0f) {
			withMacro(cfg, amount, 0.4f);
		}
		const GrainTrace g = traceGrain(cfg, 400);
		const auto len = static_cast<long long>(g.ringLen);
		const auto signedDelta = [&](long long at, size_t head) {
			long long d = ((at - static_cast<long long>(head)) % len + len) % len;
			return d > len / 2 ? d - len : d;
		};
		size_t crossings = 0;
		float worst = 0.0f;
		for (size_t i = 1; i < g.t.size(); ++i) {
			const auto& p = g.t[i - 1];
			const auto& c = g.t[i];
			const auto crossed = [&](long long ap, long long ac) {
				const long long dp = signedDelta(ap, p.head);
				const long long dc = signedDelta(ac, c.head);
				return ((dp < 0 && dc >= 0) || (dp > 0 && dc <= 0)) && std::llabs(dp) < 64 && std::llabs(dc) < 64;
			};
			if (!c.dryA && !p.dryA && c.indexA == p.indexA && crossed(p.atA, c.atA)) {
				++crossings;
				worst = std::max(worst, std::min(p.duckA * p.envA, c.duckA * c.envA));
			}
			if (!c.dryB && !p.dryB && c.envB > p.envB - 0.5f && crossed(p.atB, c.atB)) {
				++crossings;
				worst = std::max(worst, std::min(p.duckB * p.envB, c.duckB * c.envB));
			}
		}
		CAPTURE(crossings);
		CAPTURE(worst);
		REQUIRE(crossings >= 1u); // NOT VACUOUS: voices did cross the head, in each arrangement
		CHECK(worst < 0.05f);
	}
}

TEST_CASE("backlog JC: the internal delay's return ramps in and out — a DRY slice hears no step when the send switches") {
	// **THE EXAMPLE, AS FOUND** (2026-09-24 sweep): Time, puck half way to E at 0.1, Zone A 0.35, Zone B 0,
	// Rate 0.8, a 113.3 Hz sine. In a dry slice the output was the input plus the echo, and at the sample
	// the send switched the echo went from nothing to 1.0: an output of exactly 2.0 where it had been 0.997.
	// Asserted on the ECHO ALONE — output minus input, which in a dry slice is exactly what the send adds —
	// so the sine's own motion is not in the number.
	Settings cfg = looper(Mode::Time, 0.8f);
	cfg.zoneA = 0.35f;
	cfg.zoneB = 0.0f;
	withMacro(cfg, 0.5f, 0.1f);
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	Sklitter::ReadTraceEntry one {};
	s.setReadTraceForTest(&one, 1);
	double beats = 0.0;
	float prevEcho = 0.0f;
	bool havePrev = false;
	float worstStep = 0.0f;
	float peakEcho = 0.0f;
	size_t sendingDry = 0;
	size_t switches = 0;
	float lastSend = 0.0f;
	for (size_t n = 0; n < 96000 * 6; ++n) {
		const float in = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 113.3 * static_cast<double>(n) / kRate));
		float l = in;
		float r = in;
		one = {};
		s.setTransportClock(at120bpm(beats));
		s.processStereo(&l, &r, 1);
		beats += 1.0 / (kRate * 0.5);
		if (s.statusForTest() != Status::Playing || !one.dry) {
			havePrev = false;
			continue;
		}
		const float echo = l - in;
		const float send = s.delaySendForTest();
		switches += (send != lastSend) ? 1u : 0u;
		lastSend = send;
		sendingDry += send > 0.0f ? 1u : 0u;
		peakEcho = std::max(peakEcho, std::abs(echo));
		if (havePrev) {
			worstStep = std::max(worstStep, std::abs(echo - prevEcho));
		}
		prevEcho = echo;
		havePrev = true;
	}
	CAPTURE(sendingDry);
	CAPTURE(switches);
	CAPTURE(peakEcho);
	CAPTURE(worstStep);
	// NOT VACUOUS: dry slices did send, the send did switch while one played, and the echo was loud.
	REQUIRE(sendingDry > 10000u);
	REQUIRE(switches > 4u);
	REQUIRE(peakEcho > 0.5f);
	// The echo of a 113.3 Hz sine moves at most ~0.015 a sample per unit of level, and the ramp adds at
	// most its level over `minAntiClickFrames_` (~70 at 48 kHz): a few hundredths. The step this was
	// built from measured 1.0.
	CHECK(worstStep < 0.1f);
}

TEST_CASE("MEASURE: backlog JC — the repitch click sweep, every looper mode and Grain, by puck" *
          doctest::skip(std::getenv("FEATHERS_REPITCH_SWEEP") == nullptr)) {
	// **THE INSTRUMENT BEHIND BACKLOG JC, KEPT.** Reports, per mode × puck (zones and rates swept inside a
	// row): output steps over 8x the input's own, how many of those sat within 64 samples of a read moving
	// faster than 1x, and the longest run of one repeated nonzero sample. Prints the trace around the first
	// clicks. Knobs, all optional: SW_DRY / SW_FB / SW_PW (the writers), SW_SCALE (Pitch's scale), SW_FREQ
	// (the probe — keep it off a whole number of cycles a bar, or a live ring's join is invisible: 110 Hz
	// is 220 cycles a bar exactly and measured nothing), SW_ALL (print unpitched clicks too).
	const double kFreq = std::getenv("SW_FREQ") ? std::atof(std::getenv("SW_FREQ")) : 113.3;
	const double kInputStep = 2.0 * 3.14159265358979 * kFreq / kRate;
	constexpr int kBlock = 960;
	struct Cond {
		const char* puck;
		float amount;
		float cfg;
	};
	const Cond pucks[] = { { "centre", 0.0f, 0.5f }, { "E0.1", 1.0f, 0.1f }, { "E0.4", 1.0f, 0.4f },
		                   { "E0.7", 1.0f, 0.7f }, { "half0.1", 0.5f, 0.1f } };
	const std::pair<Mode, const char*> modes[] = { { Mode::Repeat, "Repeat" }, { Mode::Time, "Time" },
		                                           { Mode::Shuffle, "Shuffle" }, { Mode::Pattern, "Pattern" },
		                                           { Mode::Pitch, "Pitch" }, { Mode::Grain, "Grain" } };
	[[maybe_unused]] int shown = 0;
	for (const auto& [m, mname] : modes) {
		for (const Cond& pk : pucks) {
			size_t clicks = 0;
			int shownRow = 0;
			size_t heldWorst = 0;
			size_t heldAt = 0;
			size_t clicksPitched = 0;
			size_t pitchedSamples = 0;
			double worst = 0.0;
			for (const float za : { 0.1f, 0.35f, 0.6f, 0.85f }) {
				for (const float zb : { 0.0f, 0.6f }) {
					for (const float rate : { 0.2f, 0.5f, 0.8f }) {
						Sklitter s;
						s.prepare(kRate, 4096);
						s.setTransportClock(at120bpm());
						Settings cfg = looper(m, rate);
						cfg.zoneA = za;
						cfg.zoneB = zb;
						cfg.pitchScale = std::getenv("SW_SCALE") ? float(std::atof(std::getenv("SW_SCALE"))) : 0.0f;
						cfg.dryWrite = std::getenv("SW_DRY") ? float(std::atof(std::getenv("SW_DRY"))) : 0.0f;
						cfg.pWrite = std::getenv("SW_PW") ? float(std::atof(std::getenv("SW_PW"))) : 0.0f;
						cfg.feedback = std::getenv("SW_FB") ? float(std::atof(std::getenv("SW_FB"))) : 0.0f;
						if (pk.amount > 0.0f) {
							withMacro(cfg, pk.amount, pk.cfg);
						}
						s.setTarget(cfg);
						std::vector<Sklitter::ReadTraceEntry> trace(kBlock);
						s.setReadTraceForTest(trace.data(), trace.size());
						std::vector<Sklitter::ReadTraceEntry> all;
						std::vector<float> out;
						std::vector<float> l(kBlock);
						std::vector<float> r(kBlock);
						double beats = 0.0;
						size_t n = 0;
						for (int block = 0; block < 600; ++block) {
							for (int i = 0; i < kBlock; ++i, ++n) {
								l[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * kFreq * double(n) / kRate));
								r[i] = l[i];
							}
							for (auto& t : trace) {
								t = {};
							}
							s.setTransportClock(at120bpm(beats));
							s.processStereo(l.data(), r.data(), kBlock);
							beats += kBlock / (kRate * 0.5);
							all.insert(all.end(), trace.begin(), trace.end());
							out.insert(out.end(), l.begin(), l.end());
						}
						size_t run = 0;
						for (size_t i = 2; i < out.size(); ++i) {
							const bool sped = all[i].at >= 0 && all[i - 1].at >= 0 && all[i].slice == all[i - 1].slice
							                  && all[i].sub == all[i - 1].sub
							                  && std::llabs(all[i].at - all[i - 1].at) >= 2
							                  && std::llabs(all[i].at - all[i - 1].at) < 20;
							pitchedSamples += sped ? 1 : 0;
							const double step = std::abs(double(out[i]) - double(out[i - 1]));
							const double ratio = step / kInputStep;
							worst = std::max(worst, ratio);
							run = (out[i] == out[i - 1] && out[i] != 0.0f) ? run + 1 : 0;
							if (run > heldWorst) { heldWorst = run; heldAt = i; }
							if (ratio > 8.0) {
								++clicks;
								// was the slice pitched? look at the read delta around it
								bool pitched = false;
								for (size_t k = i > 64 ? i - 64 : 1; k < std::min(out.size(), i + 64); ++k) {
									const long long d = all[k].at - all[k - 1].at;
									if (all[k].at >= 0 && all[k - 1].at >= 0 && all[k].slice == all[i].slice && d >= 2
									    && d < 20) {
										pitched = true;
									}
								}
								clicksPitched += pitched ? 1 : 0;
								if ((std::getenv("SW_ALL") != nullptr || pitched) && shownRow < 3) {
									++shownRow;
									++shown;
									MESSAGE((std::string(mname) + " " + std::string(pk.puck) + " za=" + std::to_string(za) + " zb=" + std::to_string(zb) + " rate=" + std::to_string(rate)
									        + " i=" + std::to_string(i) + " ratio=" + std::to_string(ratio)));
									std::string w;
									for (size_t k = i - 6; k < i + 4; ++k) {
										w += " [" + std::to_string(k) + " at=" + std::to_string(all[k].at)
										     + " sl=" + std::to_string(all[k].slice) + " pos="
										     + std::to_string(all[k].position) + " sub=" + std::to_string(all[k].sub)
										     + (all[k].dry ? " D" : "") + " h=" + std::to_string(all[k].head) + " g=" + std::to_string(all[k].duck) + " o=" + std::to_string(out[k]) + "]";
									}
									MESSAGE(w);
								}
							}
						}
					}
				}
			}
			MESSAGE(("ROW " + std::string(mname) + " " + std::string(pk.puck) + " clicks=" + std::to_string(clicks) + " pitchedClicks=" + std::to_string(clicksPitched)
			        + " pitchedSamples=" + std::to_string(pitchedSamples) + " worst=" + std::to_string(worst) + " held=" + std::to_string(heldWorst) + "@" + std::to_string(heldAt)));
		}
	}
}

TEST_CASE("MEASURE: backlog JC — clicks with Dry Write at 1 against the frozen ring's own" *
          doctest::skip(std::getenv("FEATHERS_REPITCH_SWEEP") == nullptr)) {
	// **WHAT BISECTED THE LIVE-RING CLICKS AND SET `kCrossingDuckSeconds`.** Per looper mode, at the puck's
	// rest and on E at 0.4, zones and rates swept: clicks (steps over 8x the input's) with the ring frozen
	// and with Dry Write at 1, and how many slots were pitched, so a zero cannot come from a sweep that
	// played no pitched slot. The live count over the frozen one is what the crossing duck exists to remove.
	const double kFreq = 113.3;
	const double kInputStep = 2.0 * 3.14159265358979 * kFreq / kRate;
	constexpr int kBlock = 960;
	const std::pair<Mode, const char*> modes[] = { { Mode::Time, "Time" }, { Mode::Shuffle, "Shuffle" },
		                                           { Mode::Pattern, "Pattern" }, { Mode::Pitch, "Pitch" } };
	for (const auto& [m, mname] : modes) {
		for (const int puck : { 0, 1 }) {
			size_t clicks[2] = { 0, 0 };
			size_t pitchedSlots = 0;
			for (const int dry : { 0, 1 }) {
				for (const float za : { 0.1f, 0.35f, 0.6f, 0.85f }) {
					for (const float zb : { 0.0f, 0.6f }) {
						for (const float rate : { 0.2f, 0.5f, 0.8f }) {
							Sklitter s;
							s.prepare(kRate, 4096);
							s.setTransportClock(at120bpm());
							Settings cfg = looper(m, rate);
							cfg.zoneA = za;
							cfg.zoneB = zb;
							cfg.dryWrite = static_cast<float>(dry);
							if (puck == 1) {
								withMacro(cfg, 1.0f, 0.4f);
							}
							s.setTarget(cfg);
							std::vector<float> l(kBlock);
							std::vector<float> r(kBlock);
							double beats = 0.0;
							size_t n = 0;
							float prev = 0.0f;
							int lastSlice = -1;
							for (int block = 0; block < 600; ++block) {
								for (int i = 0; i < kBlock; ++i, ++n) {
									l[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * kFreq * double(n) / kRate));
									r[i] = l[i];
								}
								s.setTransportClock(at120bpm(beats));
								s.processStereo(l.data(), r.data(), kBlock);
								beats += kBlock / (kRate * 0.5);
								for (int i = 0; i < kBlock; ++i) {
									if (std::abs(double(l[i]) - double(prev)) / kInputStep > 8.0) {
										++clicks[dry];
									}
									prev = l[i];
								}
								if (dry == 0 && s.sliceIndexForTest() != lastSlice) {
									lastSlice = s.sliceIndexForTest();
									if (s.pitchRatioForTest() != 1.0 || s.planForTest().shouldPitchUp) {
										++pitchedSlots;
									}
								}
							}
						}
					}
				}
			}
			MESSAGE(("BROW " + std::string(mname) + (puck ? " E0.4" : " centre") + " frozen=" + std::to_string(clicks[0])
			         + " live=" + std::to_string(clicks[1]) + " pitchedSlotsSampled=" + std::to_string(pitchedSlots)));
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE CRUSH TRIPWIRE'S DETECTOR — `tripwire.h`. The logging only exists in a diagnostic build; the   //
// arithmetic that decides WHEN it logs is here in every build, and these cases pin its values.       //
//--------------------------------------------------------------------------------------------------//

namespace {

std::vector<float> tripSine(size_t n, double hz, size_t from = 0) {
	std::vector<float> v(n);
	for (size_t i = 0; i < n; ++i) {
		v[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * hz * double(from + i) / kRate));
	}
	return v;
}

} // namespace

TEST_CASE("crush tripwire: a clean wet holds nothing and does not trip; a 2x sample-and-hold of it holds half and trips") {
	const size_t n = 48000;
	const std::vector<float> in = tripSine(n, 443.7);
	const std::vector<float> clean = tripSine(n, 887.4); // a wet that is not the input, and not held
	float pi = 0.0f;
	float pw = 0.0f;
	const WetScan c = scanWet(in.data(), clean.data(), n, pi, pw);
	// Every sample of both is live and moving except the few where a sine lands on exact zero.
	CHECK(c.moving > n - 16);
	CHECK(c.held == 0u);
	CHECK(c.nonFinite == 0u);
	CHECK_FALSE(wetTripped(c));

	std::vector<float> crushed = clean;
	for (size_t i = 1; i < n; i += 2) {
		crushed[i] = crushed[i - 1]; // every other sample repeats: 24 kHz in a 48 kHz stream
	}
	pi = 0.0f;
	pw = 0.0f;
	const WetScan h = scanWet(in.data(), crushed.data(), n, pi, pw);
	CHECK(h.moving > n - 16);
	// Exactly the odd samples: half, give or take the zero crossings the denominator skipped.
	CHECK(h.held <= n / 2);
	CHECK(h.held > n / 2 - 16);
	CHECK(wetTripped(h));
}

TEST_CASE("crush tripwire: a held wet under a FROZEN input is not a crush, a zero wet is not counted, and a NaN trips alone") {
	const size_t n = 4096;
	const std::vector<float> still(n, 0.25f); // an input that does not move
	const std::vector<float> hold(n, 0.5f);   // a wet that does not move either
	float pi = 0.0f;
	float pw = 0.0f;
	const WetScan a = scanWet(still.data(), hold.data(), n, pi, pw);
	CHECK(a.moving == 1u); // only the first sample, where the input stepped off the carry's 0
	CHECK(a.held == 0u);
	CHECK_FALSE(wetTripped(a));

	const std::vector<float> in = tripSine(n, 443.7);
	const std::vector<float> silent(n, 0.0f);
	pi = 0.0f;
	pw = 0.0f;
	const WetScan z = scanWet(in.data(), silent.data(), n, pi, pw);
	CHECK(z.moving == 0u);
	CHECK(z.held == 0u);
	CHECK_FALSE(wetTripped(z));

	std::vector<float> bad = tripSine(n, 887.4);
	bad[1000] = std::numeric_limits<float>::quiet_NaN();
	bad[2000] = std::numeric_limits<float>::infinity();
	pi = 0.0f;
	pw = 0.0f;
	const WetScan f = scanWet(in.data(), bad.data(), n, pi, pw);
	CHECK(f.nonFinite == 2u);
	CHECK(f.held == 0u);
	CHECK(wetTripped(f));
}

TEST_CASE("crush tripwire: the carries make a split block count what the whole block counts, and the trip needs a live window") {
	const size_t n = 3000;
	const std::vector<float> in = tripSine(n, 443.7);
	std::vector<float> crushed = tripSine(n, 887.4);
	for (size_t i = 1; i < n; i += 2) {
		crushed[i] = crushed[i - 1];
	}
	float pi = 0.0f;
	float pw = 0.0f;
	const WetScan whole = scanWet(in.data(), crushed.data(), n, pi, pw);
	// Cut at an ODD index, so the pair straddling the cut is a held one that only the carry can see.
	pi = 0.0f;
	pw = 0.0f;
	const size_t cut = 1001;
	const WetScan a = scanWet(in.data(), crushed.data(), cut, pi, pw);
	const WetScan b = scanWet(in.data() + cut, crushed.data() + cut, n - cut, pi, pw);
	CHECK(a.moving + b.moving == whole.moving);
	CHECK(a.held + b.held == whole.held);
	CHECK(b.held > 0u);

	// THE FLOOR: 255 live samples all held is not a trip; 256 is.
	WetScan few;
	few.moving = kTripMinMoving - 1;
	few.held = kTripMinMoving - 1;
	CHECK_FALSE(wetTripped(few));
	few.moving = kTripMinMoving;
	few.held = kTripMinMoving / 4 + 1;
	CHECK(wetTripped(few));
	few.held = kTripMinMoving / 4; // exactly a quarter is not over it
	CHECK_FALSE(wetTripped(few));
}

TEST_CASE("MEASURE: crush tripwire — the scan's cost against SKLITTER's own block" *
          doctest::skip(std::getenv("FEATHERS_TRIPWIRE_COST") == nullptr)) {
	constexpr int kBlock = 256;
	const std::vector<float> in = tripSine(kBlock, 443.7);
	const std::vector<float> wet = tripSine(kBlock, 887.4);
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg;
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	for (int rep = 0; rep < 3; ++rep) { // alternate, per the shared-host benchmarking note
		double beats = 0.0;
		const auto t0 = std::chrono::steady_clock::now();
		for (int b = 0; b < 4000; ++b) {
			std::copy(in.begin(), in.end(), l.begin());
			std::copy(in.begin(), in.end(), r.begin());
			s.setTransportClock(at120bpm(beats));
			s.processStereo(l.data(), r.data(), kBlock);
			beats += kBlock / (kRate * 0.5);
		}
		const auto t1 = std::chrono::steady_clock::now();
		float pi = 0.0f;
		float pw = 0.0f;
		size_t sink = 0;
		for (int b = 0; b < 4000; ++b) {
			sink += scanWet(in.data(), wet.data(), kBlock, pi, pw).held;
		}
		const auto t2 = std::chrono::steady_clock::now();
		const double process = std::chrono::duration<double, std::nano>(t1 - t0).count() / (4000.0 * kBlock);
		const double scan = std::chrono::duration<double, std::nano>(t2 - t1).count() / (4000.0 * kBlock);
		MESSAGE("ns/sample: processStereo " << process << ", scanWet " << scan << " (" << 100.0 * scan / process
		                                    << "%), sink " << sink);
	}
}

TEST_CASE("the 2026-09-24 crush, inside the device: a call longer than the render chunk seats each chunk on its own sample — silence after a tone leaves an empty ring") {
	// `processStereo` cuts a call at its scratch (prepared at 4096 here) and `clock_` is the CALL's, so the idle ring's
	// seat must add the chunk's offset (`windowBase_`) as every other seat in the device does. At 0, each
	// chunk went back to the call's first sample: in 8192-frame calls the second chunk wrote over the first
	// and half the ring was never written. MEASURED before: the live span still peaked at 0.5 after three
	// seconds of silence. An offline render is where a host hands blocks this long.
	for (const int longCall : { 8192, 12288 }) {
		CAPTURE(longCall);
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTarget(Settings {}); // disengaged: the ring listens
		double beats = 0.0;
		const auto feed = [&](double seconds, float amplitude, int block) {
			std::vector<float> l(static_cast<size_t>(block));
			std::vector<float> r(static_cast<size_t>(block));
			const auto calls = static_cast<int>(seconds * kRate / block);
			for (int c = 0; c < calls; ++c) {
				for (int i = 0; i < block; ++i) {
					l[static_cast<size_t>(i)] = amplitude * std::sin(0.058f * static_cast<float>(c * block + i));
					r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
				}
				s.setTransportClock(at120bpm(beats));
				s.processStereo(l.data(), r.data(), block);
				beats += block / (kRate * 0.5);
			}
		};
		const auto livePeak = [&]() {
			const Ring& ring = s.ringForTest();
			float peak = 0.0f;
			for (size_t i = 0; i < ring.length() * 2; ++i) {
				peak = std::max(peak, std::fabs(ring.dataForTest()[i]));
			}
			return peak;
		};
		feed(3.0, 0.5f, 256); // short calls: every ring frame holds the tone
		REQUIRE(livePeak() > 0.1f); // NOT VACUOUS
		feed(3.0, 0.0f, longCall);
		CHECK(livePeak() == 0.0f);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE IDLE HEAD'S GUARD — seated once, appended, re-seated only on a CONFIRMED discontinuity           //
// (`Sklitter::seatPrewarmHead`, `kPrewarmSeatToleranceFrames`).                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A host at `bpm` and `rate` whose song position is the sample count, quantised to CLAP's beat time
/// (2^31 ticks a beat) as a CLAP host hands it. `blockOf(c)` sizes call `c`. Answers the largest |drift|
/// the idle head measured and how many times it re-seated.
struct DriftRun {
	long long worst = 0;
	size_t reseats = 0;
};

template <typename BlockOf>
DriftRun idleDrift(double rate, double bpm, double seconds, BlockOf blockOf, double startBeats = 0.0) {
	Sklitter s;
	s.prepare(rate, 4096);
	s.setTarget(Settings {});
	const double spb = 60.0 / bpm;
	std::vector<float> l(16384);
	std::vector<float> r(16384);
	DriftRun run;
	size_t n = 0;
	for (int c = 0; static_cast<double>(n) < seconds * rate; ++c) {
		const int block = blockOf(c);
		for (int i = 0; i < block; ++i) {
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.3f * std::sin(0.05f * static_cast<float>(n + static_cast<size_t>(i)));
		}
		TransportClock clock = TransportClock::fromSecondsPerBeat(spb);
		const double beats = startBeats + static_cast<double>(n) / (rate * spb);
		clock.songPositionBeats = std::round(beats * 2147483648.0) / 2147483648.0;
		clock.songPositionSeconds = clock.songPositionBeats * spb;
		clock.playing = true;
		s.setTransportClock(clock);
		s.processStereo(l.data(), r.data(), block);
		n += static_cast<size_t>(block);
		if (s.prewarmReadyForTest() || s.prewarmedFramesForTest() > 0) {
			run.worst = std::max(run.worst, std::llabs(s.prewarmDriftForTest()));
		}
	}
	run.reseats = s.prewarmReseatsForTest();
	return run;
}

} // namespace

TEST_CASE("MEASURE: the idle head's drift against the song on a well-behaved host — the noise floor the tolerance sits above" *
          doctest::skip(std::getenv("FEATHERS_SEAT_DRIFT") == nullptr)) {
	uint32_t seed = 12345u;
	const auto irregular = [&](int) {
		seed = seed * 1664525u + 1013904223u;
		return 1 + static_cast<int>((seed >> 8) % 2048u);
	};
	long long floor = 0;
	for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 }) {
		for (const double bpm : { 60.0, 97.3, 120.0, 173.71 }) {
			for (const int block : { 1, 32, 64, 441, 512, 1024, 4096, 8192, 0 }) {
				const DriftRun run = block > 0 ? idleDrift(rate, bpm, 60.0, [&](int) { return block; })
				                               : idleDrift(rate, bpm, 60.0, irregular);
				const DriftRun late = idleDrift(rate, bpm, 20.0, [&](int) { return block > 0 ? block : 256; }, 1.0e6);
				floor = std::max({ floor, run.worst, late.worst });
				MESSAGE("DRIFT rate " << rate << " bpm " << bpm << " block " << block << ": worst " << run.worst
				                      << " reseats " << run.reseats << " | at 1e6 beats worst " << late.worst
				                      << " reseats " << late.reseats);
			}
		}
	}
	MESSAGE("DRIFT FLOOR " << floor);
}

TEST_CASE("the idle head's guard: a host that repeats one song position for a run of calls leaves the ring holding the input, not the same frames") {
	// THE FEATHERS LANE BEFORE de54acb1, AS A HOST: every call of a 512-frame block is handed the block's
	// first position, in 32-frame calls. The idle head appends through it; the ring after a tone and then
	// silence holds exactly the silence. Known-bad: the per-call seat put back, which rewrote the same 32
	// frames sixteen times a block and left the tone standing.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTarget(Settings {});
	double beats = 0.0;
	const auto feed = [&](double seconds, float amplitude) {
		std::vector<float> l(32);
		std::vector<float> r(32);
		const auto blocks = static_cast<int>(seconds * kRate / 512);
		for (int b = 0; b < blocks; ++b) {
			for (int c = 0; c < 16; ++c) { // sixteen calls, ONE position
				for (int i = 0; i < 32; ++i) {
					l[static_cast<size_t>(i)] = amplitude * std::sin(0.058f * static_cast<float>(b * 512 + c * 32 + i));
					r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
				}
				s.setTransportClock(at120bpm(beats));
				s.processStereo(l.data(), r.data(), 32);
			}
			beats += 512 / (kRate * 0.5);
		}
	};
	const auto livePeak = [&]() {
		const Ring& ring = s.ringForTest();
		float peak = 0.0f;
		for (size_t i = 0; i < ring.length() * 2; ++i) {
			peak = std::max(peak, std::fabs(ring.dataForTest()[i]));
		}
		return peak;
	};
	feed(3.0, 0.5f);
	REQUIRE(livePeak() > 0.1f); // NOT VACUOUS: the ring heard the tone
	feed(3.0, 0.0f);
	CHECK(livePeak() == 0.0f);
	// AND IT NEVER TOOK THE STALE POSITION FOR A LOCATE: the disagreement grew every call and never held.
	CHECK(s.prewarmReseatsForTest() == 0u);
}

TEST_CASE("the idle head's guard: a LOCATE is followed — the head is re-seated on the song within two calls, and frame 0 stays the downbeat") {
	// A host that jumps the playhead while the device listens. The song-derived head and the appended one
	// disagree by the jump on the call after it and by the same amount on the next: a disagreement that
	// holds still is a discontinuity, and the head follows it.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTarget(Settings {});
	std::vector<float> l(256);
	std::vector<float> r(256);
	double beats = 0.0;
	const auto call = [&]() {
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), 256);
		beats += 256 / (kRate * 0.5);
	};
	for (int c = 0; c < 1000; ++c) {
		call();
	}
	REQUIRE(s.prewarmedFramesForTest() > 0); // warming
	const size_t before = s.prewarmReseatsForTest();
	beats += 1.37; // THE LOCATE: 1.37 beats forward, not a whole bar
	call();
	CHECK(s.prewarmReseatsForTest() == before); // one call to see it hold still…
	call();
	CHECK(s.prewarmReseatsForTest() == before + 1); // …and then followed
	// AND THE HEAD IS WHERE THE SONG SAYS: after the re-seat the disagreement is the noise floor.
	for (int c = 0; c < 8; ++c) {
		call();
		CHECK(std::llabs(s.prewarmDriftForTest()) <= kPrewarmSeatToleranceFrames);
	}
}

namespace {

/// `frames` of exact silence, in the same tenth-of-a-beat chunks `drive` uses, with the song moving on.
void driveSilence(Sklitter& s, int frames, double* beats) {
	constexpr int kChunk = 4800;
	std::vector<float> l(kChunk, 0.0f);
	std::vector<float> r(kChunk, 0.0f);
	for (int done = 0; done < frames; done += kChunk) {
		const int n = std::min(kChunk, frames - done);
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
}

/// Frames of the ring, over its length, holding anything but an exact zero.
size_t ringFramesNotZero(const Sklitter& s) {
	size_t count = 0;
	for (size_t frame = 0; frame < s.ringForTest().length(); ++frame) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(frame, &l, &r);
		count += (l != 0.0f || r != 0.0f) ? 1 : 0;
	}
	return count;
}

} // namespace

TEST_CASE("backlog MC: a loop decaying into silence reaches EXACT zero — every loop write is floored, so the ring and the device say they hold nothing") {
	// **THE CLASS THE SILENCE SWEEP MISSED.** A loop that keeps rewriting itself at a fraction of what it held decays
	// geometrically, and a geometric decay never reaches zero: on the factory preset machine_elf_book_of_dreams a
	// latched SKLITTER kept 72,500 frames of its capture decaying from 1e-22 to 1e-30 for a minute while its output
	// was exactly silent, so `holdsTail` (backlog MC) kept a host awake for nothing. Every write into the loop is now
	// floored at `kLoopFloor` (-140 dBFS, the house `kSilenceFloor`), so the decay ENDS. Asserted on the ring itself.
	//
	// THE ROAD: Dry Write at 0.9 over a silent input is `old + (0 - old)·0.9`, a tenth per pass of the bar — from a
	// ramp up to 96,000, under the floor in about 13 passes (26 s); without the floor still ~1e-15 at 40 s.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.dryWrite = 0.9f;
	REQUIRE(cfg.hasFeedbackCircuit());
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, 96000 + 9600, 0.0f, &beats); // capture: frame N holds the value N
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(ringFramesNotZero(s) > 90000u);

	// NOT VACUOUS: six seconds in (three passes, a thousandth of what it held) the loop is decaying, not cleared.
	driveSilence(s, 6 * 48000, &beats);
	const size_t early = ringFramesNotZero(s);
	CAPTURE(early);
	CHECK(early > 90000u);
	CHECK(s.ringForTest().holdsSound());
	CHECK(s.holdsTail()); // engaged with sound in its capture: it holds

	driveSilence(s, 34 * 48000, &beats);
	const size_t late = ringFramesNotZero(s);
	CAPTURE(late);
	CHECK(late == 0u);                           // every frame EXACTLY zero
	CHECK_FALSE(s.ringForTest().holdsSound());   // the ring's own count agrees
	CHECK(s.statusForTest() == Status::Playing); // still engaged...
	CHECK_FALSE(s.holdsTail());                  // ...and holding nothing, so a host may sleep
}

//--------------------------------------------------------------------------------------------------//
// CLEAN-ROOM ORACLE: a quantized press's snap and the live rate after it, pinned by value.          //
//--------------------------------------------------------------------------------------------------//
//
// Expected values were recorded from the implementation these cases were written against, and each
// one is also derivable by hand from the rules the case names. They pin the snap's tie rule, the
// clamp on the relative rate, which no earlier case reached.

namespace {

/// A Burst press at `rate`, fed one block of `frames` ramp frames (frame i carries i, right -i).
Sklitter& pressBurst(Sklitter& s, float rate, bool quantized, size_t frames) {
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	Settings cfg = burst(rate);
	cfg.quantized = quantized;
	cfg.engage = true;
	s.setTarget(cfg);
	std::vector<float> l(frames);
	std::vector<float> r(frames);
	ramp(l, r, 0.0f);
	s.processStereo(l.data(), r.data(), static_cast<int>(frames));
	return s;
}

} // namespace

TEST_CASE("CLEAN-ROOM ORACLE: a quantized press lands on the nearest of five divisions, a tie on the slower") {
	struct Case {
		float press;
		double snapped;
	};
	for (const Case c : { Case { 0.0f, 0.0 }, Case { 0.1f, 0.0 }, Case { 0.125f, 0.0 }, Case { 0.126f, 0.25 },
	                      Case { 0.3f, 0.25 }, Case { 0.375f, 0.25 }, Case { 0.376f, 0.5 }, Case { 0.5f, 0.5 },
	                      Case { 0.625f, 0.5 }, Case { 0.7f, 0.75 }, Case { 0.875f, 0.75 }, Case { 0.9f, 1.0 },
	                      Case { 1.0f, 1.0 } }) {
		CAPTURE(c.press);
		Sklitter s;
		pressBurst(s, c.press, true, 8);
		// THE KNOB HAS NOT MOVED SINCE THE PRESS, so the effective position is the snapped one, exactly.
		CHECK(s.effectiveRateForTest() == c.snapped);
		// And the captured length is that division's: a quarter note over 2^(4p), at 120 BPM and 48 kHz.
		CHECK(s.sliceFramesForTest() == static_cast<size_t>(std::llround(std::exp2(-4.0 * c.snapped) * 0.5 * kRate)));
		CHECK(s.capturedSliceFramesForTest() == s.sliceFramesForTest());
	}
}

TEST_CASE("CLEAN-ROOM ORACLE: the rate after a press is the snap plus the knob's travel, clamped to 0..1") {
	Sklitter s;
	pressBurst(s, 0.3f, true, 8);
	REQUIRE(s.effectiveRateForTest() == 0.25);
	const auto turnTo = [&](float rate) {
		Settings cfg = burst(rate);
		cfg.engage = true;
		s.setTarget(cfg);
		return s.effectiveRateForTest();
	};
	CHECK(turnTo(0.34f) == 0.25 + (static_cast<double>(0.34f) - static_cast<double>(0.3f)));
	CHECK(turnTo(1.0f) == doctest::Approx(0.95).epsilon(1e-7));
	CHECK(turnTo(1.0f) == 0.94999998807907104);
	CHECK(turnTo(0.0f) == 0.0); // 0.25 - 0.3 clamps at the bottom
	CHECK(turnTo(0.05f) == 0.0);
	CHECK(turnTo(0.06f) == 0.25 + (static_cast<double>(0.06f) - static_cast<double>(0.3f)));
	// THE LIVE KNOB IS CLAMPED BEFORE THE TRAVEL IS TAKEN; the press value is not.
	CHECK(turnTo(-1.0f) == 0.0);

	SUBCASE("near the top the sum clamps at 1") {
		Sklitter t;
		pressBurst(t, 0.9f, true, 8);
		REQUIRE(t.effectiveRateForTest() == 1.0);
		Settings cfg = burst(1.0f);
		cfg.engage = true;
		t.setTarget(cfg);
		CHECK(t.effectiveRateForTest() == 1.0);
	}
	SUBCASE("a press value outside 0..1 snaps from its clamp, but the travel is measured from the raw value") {
		Sklitter t;
		pressBurst(t, 1.3f, true, 8);
		// snap(clamp(1.3)) = 1.0; then 1.0 + (clamp(1.3) - 1.3) = 0.7 (approximately: 1.3f is not 1.3).
		CHECK(t.effectiveRateForTest() == 1.0 + (1.0 - static_cast<double>(1.3f)));
		CHECK(t.effectiveRateForTest() == doctest::Approx(0.7).epsilon(1e-6));
	}
	SUBCASE("unquantized, the position is the live knob, clamped") {
		Sklitter t;
		pressBurst(t, 0.3f, false, 8);
		CHECK(t.effectiveRateForTest() == static_cast<double>(0.3f));
		Settings cfg = burst(1.7f);
		cfg.quantized = false;
		cfg.engage = true;
		t.setTarget(cfg);
		CHECK(t.effectiveRateForTest() == 1.0);
		cfg.rate = -0.5f;
		t.setTarget(cfg);
		CHECK(t.effectiveRateForTest() == 0.0);
	}
}
