/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

//--------------------------------------------------------------------------------------------------//
// BACKLOG MJ — SKLITTER'S RECORD GATE AND RECORD LIGHT: a gate threshold for recording (0, the      //
// default, is off) so silence is not recorded over the buffer, and a record light.                    //
// While the input stays under the threshold (after a hardcoded 500 ms                                 //
// hold) KX's write gate shuts, so nothing lands; the light is the device's own count of writes that   //
// landed. Every claim is read off the RING or the device's own readings, by value.                    //
//--------------------------------------------------------------------------------------------------//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

using namespace feathers;
using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBarFrames = 96000; // 4 beats at 120 BPM, 48 kHz
constexpr int kBlock = 512;
/// The seam's fade, `round(0.005 · rate)`, and the hold, `round(0.5 · rate)` — restated rather than read.
constexpr size_t kFade = 240;
constexpr size_t kHold = 24000;
/// A knob of 0.5 is a threshold of exactly 0.125 (0.5³, exact in a float) — the law restated, not read.
constexpr float kKnob = 0.5f;
constexpr float kThreshold = 0.125f;

TransportClock at120bpm(double beats, bool playing = true) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.songPositionBeats = beats;
	c.playing = playing;
	return c;
}

Settings looper(Mode m = Mode::Shuffle) {
	Settings s;
	s.mode = m;
	s.enabled = true;
	s.engage = true;
	s.rate = 0.5f;
	s.density = 1.0f;
	s.pWrite = 0.0f;
	s.mix = 1.0f;
	return s;
}

/// Drive `frames` of `input(frame, right)` in blocks, advancing the transport as a host would; `perBlock` is called
/// after each block, so a case can read the device's per-block state.
template <typename Fn, typename Each>
void driveWith(Sklitter& s, int frames, double* beats, long long* frame, Fn input, Each perBlock) {
	std::vector<float> l(kBlock), r(kBlock);
	for (int done = 0; done < frames; done += kBlock) {
		const int n = std::min(kBlock, frames - done);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = input(*frame + i, false);
			r[static_cast<size_t>(i)] = input(*frame + i, true);
		}
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		*frame += n;
		*beats += static_cast<double>(n) / (0.5 * kRate);
		perBlock();
	}
}
template <typename Fn>
void driveWith(Sklitter& s, int frames, double* beats, long long* frame, Fn input) {
	driveWith(s, frames, beats, frame, input, [] {});
}

/// A constant, both ears.
auto constant(float v) {
	return [v](long long, bool) { return v; };
}

/// A 220 Hz / 331 Hz pair, peaks 0.5 and 0.4 — well over `kThreshold`, and crossing it many times a hold.
float pairInput(long long frame, bool right) {
	const double t = static_cast<double>(frame) / kRate;
	return static_cast<float>(right ? 0.4 * std::sin(2.0 * 3.14159265358979 * 331.0 * t)
	                                : 0.5 * std::sin(2.0 * 3.14159265358979 * 220.0 * t));
}

/// Deterministic noise in ±`scale`, so "any input" is an input nobody chose to be kind.
auto noise(float scale) {
	return [scale](long long frame, bool right) {
		uint32_t x = static_cast<uint32_t>(frame * 2 + (right ? 1 : 0)) * 2654435761u + 0x9E3779B9u;
		x ^= x >> 15;
		x *= 0x2C1B3C6Du;
		x ^= x >> 12;
		return scale * (static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f);
	};
}

float ringAt(const Sklitter& s, size_t frame) {
	float l = 0.0f;
	float r = 0.0f;
	s.ringForTest().readAt(frame, &l, &r);
	return l;
}

/// THE WHOLE RING, BOTH CHANNELS, AS BITS — "byte-identical" compared as bytes.
std::vector<uint32_t> ringBits(const Sklitter& s) {
	const size_t n = s.ringForTest().length();
	std::vector<uint32_t> out(2 * n, 0u);
	for (size_t f = 0; f < n; ++f) {
		float l = 0.0f;
		float r = 0.0f;
		s.ringForTest().readAt(f, &l, &r);
		std::memcpy(&out[2 * f], &l, sizeof(float));
		std::memcpy(&out[2 * f + 1], &r, sizeof(float));
	}
	return out;
}

size_t differingFrames(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
	size_t d = 0;
	for (size_t i = 0; i < std::min(a.size(), b.size()); i += 2) {
		d += (a[i] != b[i] || a[i + 1] != b[i + 1]) ? 1u : 0u;
	}
	return d + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) / 2;
}

/// Capture a bar of the pair with EVERY writer at full and the gate at `knob`, and hand back the device playing.
void captureWriting(Sklitter& s, Settings* cfg, float knob, double* beats, long long* frame) {
	s.prepare(kRate, 4096);
	*cfg = looper();
	cfg->pWrite = 1.0f;
	cfg->dryWrite = 1.0f;
	cfg->feedback = 1.0f;
	cfg->recordGate = knob;
	s.setTarget(*cfg);
	driveWith(s, kBarFrames + 9600, beats, frame, pairInput);
	REQUIRE(s.statusForTest() == Status::Playing);
}

/// A SILENT capture, ungated, then Dry Write at full and the gate at `knob` — so the ring is 0 ahead of the head and
/// whatever Dry Write lands is exactly the input times the write gain. KX's own fixture, with the gate in Freeze's place.
void silentThenDryWrite(Sklitter& s, Settings* cfg, float knob, double* beats, long long* frame) {
	s.prepare(kRate, 4096);
	*cfg = looper();
	s.setTarget(*cfg);
	driveWith(s, kBarFrames + 9600, beats, frame, constant(0.0f));
	REQUIRE(s.statusForTest() == Status::Playing);
	REQUIRE(ringAt(s, 0) == 0.0f);
	cfg->dryWrite = 1.0f;
	cfg->recordGate = knob;
	s.setTarget(*cfg);
}

} // namespace

TEST_CASE("MJ: below the Record Gate's threshold the buffer is byte-identical — every writer at full, any quiet input, nothing lands") {
	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	long long frame = 0;
	captureWriting(s, &cfg, kKnob, &beats, &frame);
	REQUIRE(s.recordGateOpenForTest()); // the pair kept it open

	// QUIET NOISE (peak 0.1, under 0.125): the hold, then the 5 ms ramp, then the gate is shut and the gain exactly 0.
	driveWith(s, static_cast<int>(kHold + kFade) + kBlock, &beats, &frame, noise(0.1f));
	REQUIRE_FALSE(s.recordGateOpenForTest());
	REQUIRE(s.freezeWriteGainForTest() == 0.0f);
	const std::vector<uint32_t> kept = ringBits(s);

	// TWO BARS OF EACH — quiet noise, then true silence — so every writer has crossed the whole buffer twice.
	driveWith(s, 2 * kBarFrames, &beats, &frame, noise(0.1f));
	CHECK(differingFrames(ringBits(s), kept) == 0u);
	driveWith(s, 2 * kBarFrames, &beats, &frame, constant(0.0f));
	CHECK(differingFrames(ringBits(s), kept) == 0u);
	CHECK_FALSE(s.recordingForTest()); // and the light says so
	// AND SO THE LOOP SUSTAINS, BY DESIGN: engaged, holding the sound the silence did not overwrite — Freeze's case, not a
	// tail that failed to decay (`test_fx_devices.cpp`'s silence sweep turns the gate off to ask about the other road).
	CHECK(s.ringForTest().holdsSound());
	CHECK(s.holdsTail());

	// NOT VACUOUS: the same run with the gate OFF rewrites most of the buffer under the same quiet noise.
	Sklitter t;
	Settings tc;
	double tb = 0.0;
	long long tf = 0;
	captureWriting(t, &tc, 0.0f, &tb, &tf);
	driveWith(t, static_cast<int>(kHold + kFade) + kBlock, &tb, &tf, noise(0.1f));
	const std::vector<uint32_t> before = ringBits(t);
	driveWith(t, 2 * kBarFrames, &tb, &tf, noise(0.1f));
	CHECK(differingFrames(ringBits(t), before) > static_cast<size_t>(kBarFrames) / 2);
	CHECK(t.recordingForTest());
}

TEST_CASE("MJ: a release and a fresh press while the gate is SHUT leave the buffer as it stood — no reset, no capture, no seam fade") {
	// KX's press case with the gate in Freeze's place: the wholesale roads (a press's reset, the pre-warm's restart, the
	// capture's seam fade at the head) are writes too, and a shut gate skips them as Freeze does.
	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	long long frame = 0;
	captureWriting(s, &cfg, kKnob, &beats, &frame);
	driveWith(s, static_cast<int>(kHold + kFade) + kBlock, &beats, &frame, noise(0.1f));
	REQUIRE_FALSE(s.recordGateOpenForTest());
	const std::vector<uint32_t> kept = ringBits(s);
	REQUIRE(std::any_of(kept.begin(), kept.end(), [](uint32_t v) { return v != 0u; })); // NOT VACUOUS: it holds sound

	// RELEASE, idle a bar (the looper pre-warms its ring), then PRESS and capture on to Playing — all under the line.
	cfg.engage = false;
	s.setTarget(cfg);
	driveWith(s, kBarFrames, &beats, &frame, noise(0.1f));
	CHECK(differingFrames(ringBits(s), kept) == 0u);
	cfg.engage = true;
	s.setTarget(cfg);
	driveWith(s, kBarFrames + 9600, &beats, &frame, noise(0.1f));
	CHECK(s.statusForTest() == Status::Playing);
	CHECK(differingFrames(ringBits(s), kept) == 0u);

	// NOT VACUOUS: the same release and press with the gate OFF rewrites the buffer with the quiet noise.
	Sklitter t;
	Settings tc;
	double tb = 0.0;
	long long tf = 0;
	captureWriting(t, &tc, 0.0f, &tb, &tf);
	driveWith(t, static_cast<int>(kHold + kFade) + kBlock, &tb, &tf, noise(0.1f));
	const std::vector<uint32_t> before = ringBits(t);
	tc.engage = false;
	t.setTarget(tc);
	driveWith(t, kBarFrames, &tb, &tf, noise(0.1f));
	tc.engage = true;
	t.setTarget(tc);
	driveWith(t, kBarFrames + 9600, &tb, &tf, noise(0.1f));
	CHECK(differingFrames(ringBits(t), before) > static_cast<size_t>(kBarFrames) / 2);
}

TEST_CASE("MJ: at or above the threshold it records — Dry Write lands the input exactly, from the first sample") {
	// THE BOUNDARY ITSELF: an input of exactly the threshold opens the gate (`>=`), and one above it too.
	for (const float level : { kThreshold, 0.6f, 1.0f }) {
		CAPTURE(level);
		Sklitter s;
		Settings cfg;
		double beats = 0.0;
		long long frame = 0;
		silentThenDryWrite(s, &cfg, kKnob, &beats, &frame);
		const size_t head = s.linearBarPosForTest();
		driveWith(s, 2 * kBlock, &beats, &frame, constant(level));
		CHECK(s.recordGateOpenForTest());
		CHECK(s.freezeWriteGainForTest() == 1.0f);
		// THE GAIN WAS 1 WHEN THE GATE CAME ON (the device was writing ungated), and the first sample reached the
		// threshold, so nothing ramps: every frame is the input, exactly.
		CHECK(ringAt(s, head) == level);
		CHECK(ringAt(s, head + 100) == level);
		CHECK(ringAt(s, head + 2 * static_cast<size_t>(kBlock) - 1) == level);
		CHECK(ringAt(s, head + 2 * static_cast<size_t>(kBlock)) == 0.0f); // and not a frame further
	}
}

TEST_CASE("MJ: the gate HOLDS 500 ms after the input falls under the threshold, then ramps the writers out over the seam's 5 ms") {
	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	long long frame = 0;
	silentThenDryWrite(s, &cfg, kKnob, &beats, &frame);
	driveWith(s, 2 * kBlock, &beats, &frame, constant(1.0f)); // open
	REQUIRE(s.recordGateOpenForTest());

	// THE FALL, to a level just under the line: a decay's tail, which the hold must not chop.
	constexpr float kTail = 0.124f;
	const size_t fall = s.linearBarPosForTest();
	bool shutEarly = false;
	long long fellAt = frame;
	driveWith(s, static_cast<int>(kHold) + 2 * kBlock, &beats, &frame, constant(kTail), [&] {
		// THE GATE'S OWN STATE, block by block: open through every block that ends inside the hold.
		if (static_cast<size_t>(frame - fellAt) <= kHold && !s.recordGateOpenForTest()) {
			shutEarly = true;
		}
	});
	CHECK_FALSE(shutEarly);
	CHECK_FALSE(s.recordGateOpenForTest());
	CHECK(s.freezeWriteGainForTest() == 0.0f);

	// THE RING, restated: the H samples after the last loud one land at gain 1 (the tail, exactly); sample H + k
	// lands at gain 1 − (k + 1)/240 over the 0 ahead; from H + 240 on, nothing.
	CHECK(ringAt(s, fall) == kTail);
	CHECK(ringAt(s, fall + kHold / 2) == kTail);
	CHECK(ringAt(s, fall + kHold - 1) == kTail);
	for (const size_t k : { size_t { 0 }, size_t { 119 }, size_t { 200 } }) {
		CAPTURE(k);
		const float g = 1.0f - static_cast<float>(k + 1) / static_cast<float>(kFade);
		CHECK(ringAt(s, fall + kHold + k) == doctest::Approx(kTail * g).epsilon(1e-4));
	}
	// 240 float steps of 1/240 leave sample H + 239 a hair above 0 (KX's note); the clamp lands exactly 0 from H + 240.
	CHECK(std::abs(ringAt(s, fall + kHold + kFade - 1)) < 1e-6f);
	CHECK(ringAt(s, fall + kHold + kFade) == 0.0f);
	CHECK(ringAt(s, fall + kHold + kFade + 100) == 0.0f);

	// AND A LOUD SAMPLE RE-ARMS IT: the writers ramp back in from 0 over the same 5 ms.
	const size_t back = s.linearBarPosForTest();
	driveWith(s, 2 * kBlock, &beats, &frame, constant(1.0f));
	CHECK(s.recordGateOpenForTest());
	CHECK(ringAt(s, back) == doctest::Approx(1.0f / static_cast<float>(kFade)).epsilon(1e-4));
	CHECK(ringAt(s, back + kFade) == 1.0f);
}

TEST_CASE("MJ: the record light is the write gate's own reading — lit while writes land, dark when the gate, Freeze or rest stops them") {
	// THE PUBLISHED READING, through the sink a host installs: `Reading::Recording` must equal what the device saw.
	struct Ear {
		std::vector<float> last;
		static void publish(void* ctx, const float* values, size_t count) {
			static_cast<Ear*>(ctx)->last.assign(values, values + count);
		}
	} ear;
	const auto light = [&] {
		REQUIRE(ear.last.size() > static_cast<size_t>(Reading::Recording));
		return ear.last[static_cast<size_t>(Reading::Recording)];
	};

	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	long long frame = 0;
	silentThenDryWrite(s, &cfg, kKnob, &beats, &frame);
	s.setReadingsSink(device::ReadingsSink { &ear, &Ear::publish });
	// EACH BLOCK: the light, as published, against the ring's own count of writes that landed in that block.
	bool agrees = true;
	size_t blocks = 0;
	size_t lit = 0;
	size_t landedBefore = s.ringForTest().landed();
	const auto check = [&] {
		s.publishReadings();
		const bool landed = s.ringForTest().landed() != landedBefore;
		landedBefore = s.ringForTest().landed();
		agrees = agrees && (light() == (landed ? 1.0f : 0.0f)) && (s.recordingForTest() == landed);
		++blocks;
		lit += landed ? 1u : 0u;
	};

	// LOUD: lit.
	driveWith(s, 4 * kBlock, &beats, &frame, constant(1.0f), check);
	CHECK(light() == 1.0f);
	// QUIET, past the hold and the ramp: dark — and lit all through the hold, which is still writing.
	driveWith(s, static_cast<int>(kHold + kFade) + 2 * kBlock, &beats, &frame, constant(0.01f), check);
	CHECK(light() == 0.0f);
	// LOUD AGAIN, then FREEZE with the input still loud: dark once Freeze's ramp is done, whatever the gate says.
	driveWith(s, 2 * kBlock, &beats, &frame, constant(1.0f), check);
	CHECK(light() == 1.0f);
	cfg.freeze = true;
	s.setTarget(cfg);
	driveWith(s, 2 * kBlock, &beats, &frame, constant(1.0f), check);
	CHECK(s.recordGateOpenForTest());
	CHECK(light() == 0.0f);
	CHECK(agrees);
	CHECK(lit > 0u);
	CHECK(lit < blocks); // NOT VACUOUS: both states were seen

	// AND AT REST IN BURST, gate off, a loud input records nothing — Burst captures only on a press — so the light
	// is dark although the gate (off) would let anything through.
	Sklitter c;
	c.prepare(kRate, 4096);
	Settings cc = looper(Mode::Burst);
	cc.engage = false;
	c.setTarget(cc);
	c.setReadingsSink(device::ReadingsSink { &ear, &Ear::publish });
	double cb = 0.0;
	long long cf = 0;
	driveWith(c, 4 * kBlock, &cb, &cf, constant(1.0f), [&] { c.publishReadings(); });
	CHECK(light() == 0.0f);
	CHECK_FALSE(c.recordingForTest());
}

TEST_CASE("MJ: a Record Gate of 0 is OFF — the write gain never leaves 1, and a fresh capture lands from its first frame where an armed gate fades in") {
	// TWO FRESH CAPTURES OF THE SAME PAIR: knob 0, and a knob so small the pair never falls under it (1e-4, a threshold
	// of 1e-12). The armed gate starts SHUT (nothing has reached it yet) and fades the writers in over 5 ms; OFF has no
	// gate to open, so its capture is the input from frame 0. Over silence ahead of the head, gain g lands `in · g`.
	const auto capture = [](float knob, bool* gainAlwaysOne) {
		auto s = std::make_unique<Sklitter>();
		s->prepare(kRate, 4096);
		Settings cfg = looper();
		cfg.recordGate = knob;
		s->setTarget(cfg);
		double beats = 0.0;
		long long frame = 0;
		Sklitter& ref = *s;
		driveWith(ref, 4 * kBlock, &beats, &frame, pairInput,
		          [&] { *gainAlwaysOne = *gainAlwaysOne && ref.freezeWriteGainForTest() == 1.0f; });
		return s;
	};
	bool offAtOne = true;
	bool armedAtOne = true;
	const auto off = capture(0.0f, &offAtOne);
	const auto armed = capture(1.0e-4f, &armedAtOne);
	CHECK(offAtOne);
	CHECK(armed->recordGateOpenForTest());
	size_t wrongRamp = 0;
	size_t wrongPast = 0;
	// THE PAIR STARTS AT EXACTLY 0, which is under any threshold: the gate opens on frame 1, so frame f lands at gain
	// f/240 — up to frame 240, which 240 float steps of 1/240 leave a hair short of 1 (KX's own note), hence the bound.
	for (size_t f = 0; f <= kFade; ++f) {
		const float g = static_cast<float>(f) / static_cast<float>(kFade);
		wrongRamp += std::abs(ringAt(*armed, f) - ringAt(*off, f) * g) > 2e-6f ? 1u : 0u;
	}
	for (size_t f = kFade + 1; f < 4 * static_cast<size_t>(kBlock); ++f) {
		wrongPast += ringAt(*armed, f) != ringAt(*off, f) ? 1u : 0u;
	}
	CHECK(wrongRamp == 0u);
	CHECK(wrongPast == 0u);
	// OFF IS THE INPUT ITSELF from the first frame, which is what "bit-identical to today" means for a capture.
	CHECK(ringAt(*off, 0) == pairInput(0, false));
	CHECK(ringAt(*off, 100) == pairInput(100, false));
	CHECK(ringAt(*off, 1000) == pairInput(1000, false));
	// NOT VACUOUS: the ramp had something to scale.
	CHECK(std::abs(ringAt(*off, 100)) > 0.1f);
}

TEST_CASE("MJ: RECORD GATE is a keyed, modulatable 0..1 control at 0 (off), on the card's face beside Input Gain, cubed into a level") {
	const device::ParamDesc& d = kParamDescs[static_cast<size_t>(Param::RecordGate)];
	CHECK(std::string_view(d.key) == "record_gate");
	CHECK(d.min == 0.0);
	CHECK(d.max == 1.0);
	CHECK(d.def == 0.0);
	CHECK_FALSE(d.stepped);
	CHECK(d.modulatable);
	CHECK(d.gateControl < 0); // every mode records
	const auto* gain = std::find(std::begin(kFace), std::end(kFace), static_cast<uint8_t>(Param::InputGain));
	REQUIRE(gain != std::end(kFace));
	REQUIRE(gain + 1 != std::end(kFace));
	CHECK(gain[1] == static_cast<uint8_t>(Param::RecordGate));
	// THE LAW, restated: the knob cubed, 0 exactly 0.
	CHECK(recordGateThreshold(0.0f) == 0.0f);
	CHECK(recordGateThreshold(0.5f) == 0.125f);
	CHECK(recordGateThreshold(1.0f) == 1.0f);
	CHECK(20.0 * std::log10(static_cast<double>(recordGateThreshold(0.1f))) == doctest::Approx(-60.0).epsilon(1e-4));
	CHECK(kRecordGateHoldSeconds == 0.5);
	// AND THE ROW CARRIES IT BOTH WAYS, clamped to its declaration when a route pushes it past.
	Row row = rowOf(Settings {});
	CHECK(row[static_cast<size_t>(Param::RecordGate)] == 0.0f);
	row[static_cast<size_t>(Param::RecordGate)] = 0.3f;
	CHECK(stateOf(row).recordGate == 0.3f);
	row[static_cast<size_t>(Param::RecordGate)] = 1.7f;
	CHECK(stateOf(row).recordGate == 1.0f);
	row[static_cast<size_t>(Param::RecordGate)] = -0.2f;
	CHECK(stateOf(row).recordGate == 0.0f);
}
