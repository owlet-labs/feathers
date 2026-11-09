/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

//--------------------------------------------------------------------------------------------------//
// BACKLOG GU/GV — THE THREE SEQUENTIAL BLENDS. The 2026-09-21 design, held to the engine.        //
//                                                                                                   //
// Every write to the buffer is a blend, reducing the buffer content and adding the signal, in the   //
// order dry write, then feedback, then P Write.                                                     //
//                                                                                                   //
// **EVERY CLAIM HERE IS A VALUE ASSERTION AND THAT IS NOT A STYLE CHOICE.** CLAUDE.md: *"A CLAIM     //
// ABOUT A VALUE CANNOT BE INFERRED FROM A RENDERED BUFFER — ASSERT THE VALUE."* A feedback level is  //
// precisely the quantity a rendered buffer cannot be asked about: every pass of the loop carries it  //
// and so does everything else, so a comparison between two renders goes green whenever ANYTHING      //
// differs — and every instance of that failure this project has found was a PARTIAL effect           //
// surviving where a total one would have been caught. A feedback amount is partial by definition.    //
//                                                                                                   //
// So each case computes its reference by an independent restatement of the intended arithmetic,      //
// against a buffer whose contents it put there, and keeps a difference or a count only as the        //
// SECOND assertion that proves the first is not vacuous.                                             //
//                                                                                                   //
// **THE FIXTURE'S ONE TRICK IS THE SILENT CAPTURE.** Driving silence through the capture bar leaves  //
// the ring exactly zero, which makes `old` a number the case knows rather than one it has to read    //
// back and trust. Driving DC through it leaves the ring exactly one. Those two are what turn         //
// `old + (src − old)·a` from an arrangement into an equation.                                        //
//--------------------------------------------------------------------------------------------------//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

using namespace feathers;
using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBarFrames = 96000; // 4 beats at 120 BPM, 48 kHz
constexpr int kBlock = 512;

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

/// Drive `frames` of CONSTANT `value` in blocks, advancing the transport as a host would.
void drive(Sklitter& s, int frames, float value, double* beats, int block = kBlock) {
	std::vector<float> l;
	std::vector<float> r;
	for (int done = 0; done < frames; done += block) {
		const int n = std::min(block, frames - done);
		l.assign(static_cast<size_t>(n), value);
		r.assign(static_cast<size_t>(n), value);
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
}

/// **CAPTURE A BAR OF EXACTLY `fill`, SO THE RING IS A NUMBER THE CASE CHOSE.** The bar plus a beat
/// is what the other sklitter suites use to reach Playing, and the reason is there.
void captureTo(Sklitter& s, const Settings& cfg, float fill, double* beats) {
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm(0.0));
	s.setTarget(cfg);
	drive(s, kBarFrames + 9600, fill, beats);
	REQUIRE(s.statusForTest() == Status::Playing);
}

float ringAt(const Sklitter& s, size_t frame) {
	float l = 0.0f;
	float r = 0.0f;
	s.ringForTest().readAt(frame, &l, &r);
	return l;
}

/// The whole ring, left channel, for the identity claims.
std::vector<float> ringOf(const Sklitter& s) {
	const size_t n = s.ringForTest().length();
	std::vector<float> out(n, 0.0f);
	for (size_t f = 0; f < n; ++f) {
		out[f] = ringAt(s, f);
	}
	return out;
}

float peakOf(const std::vector<float>& v) {
	float p = 0.0f;
	for (const float x : v) {
		p = std::max(p, std::abs(x));
	}
	return p;
}

//------------------------------------------------------------------------------------------------//
// A PROBE WITH GAIN, for the one claim that needs a device inside the master lane.                 //
//------------------------------------------------------------------------------------------------//

struct Probe {
	float gain = 1.0f;
};

Probe& probeAt(void* storage) { return *static_cast<Probe*>(storage); }

constexpr device::AudioOps kProbeOps {
	.stateBytes = sizeof(Probe),
	.stateAlign = alignof(Probe),
	.construct = [](void* s) { new (s) Probe {}; },
	.destruct = [](void* s) { static_cast<Probe*>(s)->~Probe(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { probeAt(s).gain = n > 0 ? f[0] : 1.0f; },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    const float g = probeAt(s).gain;
		    for (int i = 0; i < frames; ++i) {
			    l[i] *= g;
			    r[i] *= g;
		    }
	    },
};

constexpr std::array<device::ParamDesc, 1> kProbeParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
} };

const device::Descriptor kProbeDesc {
	.traits = { .tag = "prbF", .name = "Probe F", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = -1 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	return device::sameTag(tag, kProbeDesc.traits.tag) ? &kProbeDesc : nullptr;
}

const device::Catalogue kProbes { nullptr, &findProbe };

void seatMasterProbe(Sklitter& s, float gain) {
	REQUIRE(kDescriptor.container != nullptr);
	REQUIRE(kDescriptor.container->adopt != nullptr);
	kDescriptor.container->adopt(&s, kProbes);
	REQUIRE(s.postChain() != nullptr);
	rack::RackBridge* b = s.postChain()->bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, gain);
	b->drain();
	REQUIRE(s.postChain()->engine().slotOf(id) != rack::RackEngine::kNoSlot);
}

/// Where the descriptor for one control lives.
const device::ParamDesc& descOf(Param p) { return kParamDescs[static_cast<size_t>(p)]; }

} // namespace

//--------------------------------------------------------------------------------------------------//

TEST_CASE("DRY WRITE blends the fresh INPUT into the ring by exactly its amount") {
	// **THE ARITHMETIC IS RESTATED, NOT OBSERVED.** The ring is all zeros (a silent capture) and the
	// input is a constant 1, so `old + (src − old)·a` is `0 + (1 − 0)·a`, which is `a` — a number this
	// case computes from the design rather than from the engine.
	constexpr float kAmount = 0.375f;

	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	captureTo(s, cfg, 0.0f, &beats);
	REQUIRE(ringAt(s, 0) == 0.0f); // the silent capture did what it says, or nothing below means anything

	cfg.dryWrite = kAmount;
	s.setTarget(cfg);
	const size_t head = s.linearBarPosForTest();
	drive(s, kBlock, 1.0f, &beats);

	// **THE VALUE.** Every frame the head passed over in that block holds exactly the amount.
	CHECK(ringAt(s, head) == kAmount);
	CHECK(ringAt(s, head + 1) == kAmount);
	CHECK(ringAt(s, head + static_cast<size_t>(kBlock) - 1) == kAmount);
	// AND THE FRAME AFTER THE BLOCK IS UNTOUCHED, which is what says the write is the head's and not
	// a smear over the buffer.
	CHECK(ringAt(s, head + static_cast<size_t>(kBlock)) == 0.0f);

	// **THE NON-VACUITY ASSERTION: a count produced BY the write.** Delete the dry blend and this
	// goes to zero with it — a counter standing beside the call could not promise that.
	CHECK(s.dryWritesForTest() == static_cast<size_t>(kBlock));

	// AND A SECOND AMOUNT, because one point does not distinguish "blends by a" from "writes a".
	Sklitter t;
	double tb = 0.0;
	Settings tc = looper();
	captureTo(t, tc, 0.0f, &tb);
	tc.dryWrite = 1.0f;
	t.setTarget(tc);
	const size_t th = t.linearBarPosForTest();
	drive(t, kBlock, 1.0f, &tb);
	CHECK(ringAt(t, th) == 1.0f); // at full it IS the input
}

TEST_CASE("FEEDBACK carries the WET and NOT the dry, which is the design that makes it usable") {
	// Only the wet signal is fed back. The ring records the INPUT, so a loop
	// carrying dry would double the dry every pass.
	//
	// **THE DISCRIMINATING SETUP: a silent buffer under a loud input.** The wet is then silence (the
	// device is playing zeros back) while the dry is 1.0 at every sample. Wet-only leaves the ring at
	// exactly zero; a path that fed the dry would leave it at the damped 1.0, which is nowhere near.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	captureTo(s, cfg, 0.0f, &beats);

	cfg.feedback = 1.0f; // the most unfavourable setting for the claim
	cfg.dryWrite = 0.0f;
	s.setTarget(cfg);
	const size_t head = s.linearBarPosForTest();
	drive(s, kBlock, 1.0f, &beats);

	// **THE TAP ITSELF, READ RATHER THAN INFERRED.** This is the wet the feedback path took.
	CHECK(s.feedbackTapForTest() == 0.0f);
	CHECK(s.feedbackWrittenForTest() == 0.0f);
	// AND THE RING IT LANDED IN.
	CHECK(ringAt(s, head) == 0.0f);
	CHECK(ringAt(s, head + static_cast<size_t>(kBlock) - 1) == 0.0f);
	// **NON-VACUITY: the feedback blend RAN**, at every sample of the block, and still wrote nothing.
	// Without this the case above is satisfied by a feedback path that was never reached.
	CHECK(s.feedbackWritesForTest() == static_cast<size_t>(kBlock));
}

TEST_CASE("the damping is a real filter at the host's rate, and it is what lands in the ring") {
	// **THREE LINKS, ASSERTED SEPARATELY, because a single end-to-end check cannot tell which of them
	// is wrong** — and the middle one is the class of defect this file has already paid for four
	// times: a constant that means 44.1 kHz used at 96.

	// 1. THE COEFFICIENTS, restated from the declared FREQUENCIES and the rate.
	Sklitter s;
	s.prepare(kRate, 4096);
	float lp = 0.0f;
	float dc = 0.0f;
	s.dampCoeffsForTest(&lp, &dc);
	constexpr double kTwoPi = 6.283185307179586476925286766559;
	CHECK(lp == doctest::Approx(1.0 - std::exp(-kTwoPi * kFeedbackDampHz / kRate)));
	CHECK(dc == doctest::Approx(std::exp(-kTwoPi * kFeedbackDcHz / kRate)));

	// AND AT A DIFFERENT RATE THEY MOVE, which is the whole of the claim that they are derived.
	Sklitter fast;
	fast.prepare(96000.0, 4096);
	float lp96 = 0.0f;
	float dc96 = 0.0f;
	fast.dampCoeffsForTest(&lp96, &dc96);
	CHECK(lp96 == doctest::Approx(1.0 - std::exp(-kTwoPi * kFeedbackDampHz / 96000.0)));
	CHECK(lp96 != lp);

	// 2. THE FILTER, restated sample by sample against the real member on the real state.
	float refLp = 0.0f;
	float refX1 = 0.0f;
	float refY1 = 0.0f;
	for (int i = 0; i < 64; ++i) {
		const float x = (i % 7 == 0) ? 1.0f : -0.25f;
		float gotL = 0.0f;
		float gotR = 0.0f;
		s.dampOneForTest(x, x, &gotL, &gotR);
		refLp += (x - refLp) * lp;
		const float y = refLp - refX1 + dc * refY1;
		refX1 = refLp;
		refY1 = y;
		CAPTURE(i);
		CHECK(gotL == doctest::Approx(y));
	}

	// 3. **AND THE CEILING, which is what makes a runaway loud instead of a NaN.** See
	// `kFeedbackCeiling`: clamped BEFORE the filters, so the state never holds the unbounded value.
	Sklitter c;
	c.prepare(kRate, 4096);
	float cl = 0.0f;
	float cr = 0.0f;
	for (int i = 0; i < 4096; ++i) {
		c.dampOneForTest(1.0e9f, 1.0e9f, &cl, &cr);
	}
	CHECK(std::isfinite(cl));
	CHECK(std::abs(cl) <= kFeedbackCeiling + 1e-3f);
}

TEST_CASE("the three blends compose in THE DESIGNED ORDER — dry, then feedback, then P Write") {
	// **THE ORDER IS AUDIBLE AND THIS IS THE ASSERTION THAT PINS IT.** Because the blends compose,
	// what survives of the buffer's history is `(1 − a_dry)(1 − a_fb)(1 − a_pw)` and a writer later
	// in the chain is attenuated less by the ones after it. Run the same arithmetic in the other
	// order and the number is different, so this cannot pass for a permutation.
	constexpr float kV = 1.0f;       // what the capture put in every ring frame
	constexpr float kDry = 0.25f;    // and the input is silence, so DRY pulls the frame DOWN by this
	constexpr float kFb = 0.5f;

	// (a) DRY ALONE: `V + (0 − V)·a_dry` = `V·(1 − a_dry)`.
	Sklitter d;
	double db = 0.0;
	Settings dc = looper();
	captureTo(d, dc, kV, &db);
	// **NOT FRAME 0 — `Ring::fadeSeamAt` FADES BOTH ENDS OF THE CAPTURE**, so the first and last
	// frames of a DC capture are deliberately not DC. Read from the middle, where the number is the
	// one the case put there. (The first draft read frame 0 and got exactly 0.)
	REQUIRE(ringAt(d, static_cast<size_t>(kBarFrames) / 2) == doctest::Approx(kV));
	dc.dryWrite = kDry;
	d.setTarget(dc);
	const size_t dh = d.linearBarPosForTest();
	drive(d, kBlock, 0.0f, &db);
	const float afterDry = ringAt(d, dh + static_cast<size_t>(kBlock) - 1);
	CHECK(afterDry == doctest::Approx(kV * (1.0f - kDry)).epsilon(1e-6));

	// (b) DRY THEN FEEDBACK, at the SAME dry amount, so the only new term is the second blend —
	// and the reference is built ON the first blend's result. **If the feedback ran FIRST the number
	// would be `(V + (W − V)·a_fb)·(1 − a_dry)`, which is a different number for every W ≠ V.**
	Sklitter f;
	double fb = 0.0;
	Settings fc = looper();
	captureTo(f, fc, kV, &fb);
	fc.dryWrite = kDry;
	fc.feedback = kFb;
	f.setTarget(fc);
	const size_t fh = f.linearBarPosForTest();
	drive(f, kBlock, 0.0f, &fb);

	const float w = f.feedbackWrittenForTest(); // the damped wet at the last flushed sample
	const float last = ringAt(f, fh + static_cast<size_t>(kBlock) - 1);
	const float afterDryHere = kV * (1.0f - kDry);
	const float expected = afterDryHere + (w - afterDryHere) * kFb;
	CHECK(last == doctest::Approx(expected).epsilon(1e-5));

	// **AND THE WRONG ORDER IS RULED OUT BY NUMBER, not by argument.** Stated so the case fails if
	// the two ever coincide, which would make the assertion above vacuous.
	const float reversed = (kV + (w - kV) * kFb) * (1.0f - kDry);
	REQUIRE(std::abs(reversed - expected) > 1e-4f);
	CHECK(std::abs(last - reversed) > 1e-4f);

	// (c) AND `P Write` LAST, which needs its two unknowns read rather than guessed.
	Sklitter p;
	double pb = 0.0;
	Settings pc = looper();
	captureTo(p, pc, kV, &pb);
	pc.dryWrite = kDry;
	pc.pWrite = 1.0f; // every slice writes, so the third blend is reached
	p.setTarget(pc);
	const size_t ph = p.linearBarPosForTest();
	drive(p, kBlock, 0.0f, &pb);
	const size_t lastFrame = ph + static_cast<size_t>(kBlock) - 1;
	if (p.pWriteMixForTest() > 0.0f) {
		const float m = p.pWriteMixForTest();
		const float src = p.pWriteSrcForTest();
		const float afterDryP = kV * (1.0f - kDry);
		CHECK(ringAt(p, lastFrame) == doctest::Approx(afterDryP + (src - afterDryP) * m).epsilon(1e-5));
	}
	else {
		// The slice did not write; then the frame is the dry blend alone, which is still a value.
		CHECK(ringAt(p, lastFrame) == doctest::Approx(kV * (1.0f - kDry)).epsilon(1e-6));
	}
}

TEST_CASE("it does not run away — a decay MEASURED over passes, not a buffer that looked fine") {
	// **THE QUANTITY IS A DECAY OVER N PASSES.** Backlog GV names this as the thing to assert and
	// names why: *"A feedback path that is stable at 120 BPM, one bar and 48 kHz may not be at other
	// settings; the quantity to assert is a decay over N passes, measured, not 'it sounded fine.'"*
	//
	// The loop is closed at FULL: feedback 1.0, silent input, nothing else writing. Each bar the
	// buffer is replaced by the damped version of what the device played out of it.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	captureTo(s, cfg, 1.0f, &beats);
	cfg.feedback = 1.0f;
	s.setTarget(cfg);

	const float start = peakOf(ringOf(s));
	REQUIRE(start > 0.5f); // there is something to decay, or the claim below is about silence

	// **THE FIRST PASS RISES AND THE REST FALL, AND THAT IS STATED RATHER THAN SMOOTHED OVER.**
	// MEASURED, six passes at feedback 1.0 off a DC-1.0 capture: **1.207, 0.559, 0.471, 0.424,
	// 0.381, 0.350.** The first is the DC blocker's own step response — the loop opens on a buffer
	// that is pure DC, which is exactly the thing that filter is there to remove, and removing it
	// from a step overshoots. It is a transient of ONE pass on the most adversarial material anyone
	// can hand it, and every pass after it decays monotonically.
	//
	// So the assertion is written as two claims rather than one, because one number cannot say both:
	// a BOUND that holds from the first sample, and a MONOTONIC DECAY from the second pass on.
	std::vector<float> peaks;
	for (int pass = 0; pass < 6; ++pass) {
		drive(s, kBarFrames, 0.0f, &beats);
		const float peak = peakOf(ringOf(s));
		MESSAGE("pass ", pass + 1, " peak ", peak, " ratio ", peak / start);
		CHECK(std::isfinite(peak));
		// **THE BOUND — the ceiling, which holds at every pass including the transient.**
		CHECK(peak <= kFeedbackCeiling);
		peaks.push_back(peak);
	}
	for (size_t k = 2; k < peaks.size(); ++k) {
		CAPTURE(k);
		// A hair of slack for the float arithmetic of the blend itself.
		CHECK(peaks[k] <= peaks[k - 1] * 1.0001f);
	}
	MESSAGE("six passes at feedback 1.0: ", start, " -> ", peaks.back(), " (", peaks.back() / start,
	        "x)");
	// **AND IT HAS ACTUALLY DECAYED rather than merely not grown** — the damping is what does this,
	// and without it the loop returns a spectrally identical copy and the peak would sit still. Half
	// is a bar set well below the measurement (0.35) so that a change in the damping moves it before
	// the case complains, and well above nothing so that "it decayed" is not satisfied by silence.
	CHECK(peaks.back() < start * 0.5f);
	CHECK(peaks.back() > 0.0f);
}

TEST_CASE("a lane with GAIN in the loop is bounded rather than infinite — the case GV names") {
	// Backlog GV: *"a device with its own feedback, placed in a lane that is itself inside a feedback
	// loop. That is not a reason to refuse it; it is a reason to bound it deliberately rather than
	// discover it."* A unity-plus device on the `Post FX` tap multiplies every pass. `kFeedbackCeiling`
	// is the bound, and this is where it is held to its word.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	s.prepare(kRate, 4096);
	seatMasterProbe(s, 4.0f); // four times round is a runaway by any measure
	s.setTransportClock(at120bpm(0.0));
	s.setTarget(cfg);
	drive(s, kBarFrames + 9600, 0.5f, &beats);
	REQUIRE(s.statusForTest() == Status::Playing);

	cfg.feedback = 1.0f;
	cfg.feedbackPostMaster = true; // the master lane INSIDE the loop
	s.setTarget(cfg);
	for (int pass = 0; pass < 4; ++pass) {
		drive(s, kBarFrames, 0.0f, &beats);
	}
	const std::vector<float> ring = ringOf(s);
	const float peak = peakOf(ring);
	MESSAGE("four passes through a x4 master lane: ring peak ", peak);
	CHECK(std::isfinite(peak));
	CHECK(peak <= kFeedbackCeiling + 1e-3f);
	// **AND IT IS NOT SILENT, which is the other way a bound can be satisfied and a worse one.**
	CHECK(peak > 0.0f);
}

TEST_CASE("the FB Tap switch really takes a different signal, and the one it names") {
	// **THE HONESTY GUARD FOR `FB Tap`**, which the mode matrix in `test_fx_sklitter.cpp` excludes by
	// name because its lanes are empty and its base row has `Feedback` at zero. With a x3 device in
	// the master lane the two taps are three times apart, and that is a number rather than a
	// difference: `Post FX` sees what `Grain FX` saw, multiplied by the lane.
	constexpr float kGain = 3.0f;
	const auto tapAfterOneBlock = [](bool postMaster) {
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper();
		s.prepare(kRate, 4096);
		seatMasterProbe(s, kGain);
		s.setTransportClock(at120bpm(0.0));
		s.setTarget(cfg);
		drive(s, kBarFrames + 9600, 0.5f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.feedback = 1.0f;
		cfg.feedbackPostMaster = postMaster;
		s.setTarget(cfg);
		drive(s, kBlock, 0.0f, &beats);
		return s.feedbackTapForTest();
	};

	const float grainTap = tapAfterOneBlock(false);
	const float postTap = tapAfterOneBlock(true);
	MESSAGE("Grain FX tap ", grainTap, " / Post FX tap ", postTap);
	// NON-VACUITY FIRST: a zero tap would satisfy the ratio below for free.
	REQUIRE(std::abs(grainTap) > 1e-4f);
	// **THE VALUE: the master lane's gain, exactly.**
	CHECK(postTap == doctest::Approx(grainTap * kGain).epsilon(1e-4));
}

TEST_CASE("the two writers are gated OFF where they were never proved, and the ring says so") {
	// **GRAIN WAS OUT HERE AND IS NOW IN — backlog HU, and the change is a RETRACTION rather than an
	// extension.** This case used to name Grain among the excluded, citing HN's 24,570 hazard reads
	// of 96,000 and my own conclusion that feedback there was unbounded by construction. The hazard
	// was real; the conclusion was not. Every writer is a convex blend and the feedback source is
	// damped and clamped, so the ring is BOUNDED in Grain — and measured over six passes Grain decays
	// thirteen times further than Shuffle, which has the duck. `test_fx_sklitter_grainfb.cpp` holds
	// the numbers and the argument.
	//
	// **REPEAT STAYS OUT ON `P Write`'s OWN REASON** (writing into a figure it reads over and over
	// erases it), and Burst is not a looper at all. `kWriterModes` is the one statement
	// of that, read by the gate AND by the descriptors, so they cannot drift.
	CHECK(kWriterModes == (kModeBit(Mode::Shuffle) | kModeBit(Mode::Time) | kModeBit(Mode::Pattern)
	                       | kModeBit(Mode::Pitch) | kModeBit(Mode::Grain)));
	CHECK(descOf(Param::DryWrite).gateMask == kWriterModes);
	CHECK(descOf(Param::Feedback).gateMask == kWriterModes);
	CHECK(descOf(Param::FeedbackTap).gateMask == kWriterModes);

	// **AND THE ENGINE AGREES, BY IDENTITY RATHER THAN BY READING THE GATE.** In each excluded mode
	// the ring after a block with both knobs at FULL is bit for bit the ring with both at zero.
	for (const Mode m : { Mode::Repeat, Mode::Burst }) {
		CAPTURE(static_cast<int>(m));
		const auto ringAfter = [m](float amount) {
			Sklitter s;
			double beats = 0.0;
			Settings cfg = looper(m);
			captureTo(s, cfg, 0.5f, &beats);
			cfg.dryWrite = amount;
			cfg.feedback = amount;
			s.setTarget(cfg);
			drive(s, kBlock * 8, 1.0f, &beats);
			return ringOf(s);
		};
		const std::vector<float> off = ringAfter(0.0f);
		const std::vector<float> on = ringAfter(1.0f);
		REQUIRE(off.size() == on.size());
		size_t differing = 0;
		for (size_t i = 0; i < off.size(); ++i) {
			if (off[i] != on[i]) {
				++differing;
			}
		}
		CHECK(differing == 0u);
	}

	// **AND THE VENUE IS SHOWN CAPABLE OF EXPRESSING THE OUTCOME**, or the zeros above mean nothing:
	// the same comparison in Shuffle, where the gate is open, must differ.
	const auto shuffleRing = [](float amount) {
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper(Mode::Shuffle);
		captureTo(s, cfg, 0.5f, &beats);
		cfg.dryWrite = amount;
		cfg.feedback = amount;
		s.setTarget(cfg);
		drive(s, kBlock * 8, 1.0f, &beats);
		return ringOf(s);
	};
	const std::vector<float> sOff = shuffleRing(0.0f);
	const std::vector<float> sOn = shuffleRing(1.0f);
	size_t sDiff = 0;
	for (size_t i = 0; i < sOff.size(); ++i) {
		if (sOff[i] != sOn[i]) {
			++sDiff;
		}
	}
	CHECK(sDiff > 0u);
}

TEST_CASE("neither knob is a modulation destination, and that false is MEANT") {
	// Deliberately not a mod destination for now; it may become one later. CLAUDE.md's honesty
	// rule runs both ways — a destination flag that nothing routes to is the same lie told from the
	// other end — so this is asserted rather than left to be noticed.
	CHECK_FALSE(descOf(Param::DryWrite).modulatable);
	CHECK_FALSE(descOf(Param::Feedback).modulatable);
	CHECK_FALSE(descOf(Param::FeedbackTap).modulatable);

	// AND THE DEFAULTS ARE THE IDENTITY, which is a correctness argument and not a compatibility one:
	// a device that has never had these touched writes exactly what it wrote before they existed.
	CHECK(descOf(Param::DryWrite).def == 0.0);
	CHECK(descOf(Param::Feedback).def == 0.0);
	Settings s;
	CHECK(s.dryWrite == 0.0f);
	CHECK(s.feedback == 0.0f);
	CHECK_FALSE(s.writersActive());

	// **TWO STATES ON THE TAP, so it is a SwitchChip and not the multivalue toggle the project rules
	// out** — with two, *next* and *which* are the same thing.
	CHECK(descOf(Param::FeedbackTap).names.size() == 2u);
	CHECK(descOf(Param::FeedbackTap).stepped);
}

TEST_CASE("a patch that never touches them takes the write path it always took") {
	// **THE DEFERRAL IS ENGAGED ONLY WHEN THE CIRCUIT IS.** Backlog HN proved the deferred write
	// bit-exact in these modes, but a proof is a thing somebody has to trust; this is the narrower
	// fact that needs no trust at all — at the defaults the old immediate write runs, unchanged.
	Settings s = looper();
	CHECK(s.hasFeedbackCircuit());
	CHECK_FALSE(s.writersActive()); // circuit available, nothing asking for it
	s.dryWrite = 0.001f;
	CHECK(s.writersActive());
	s.dryWrite = 0.0f;
	s.feedback = 0.001f;
	CHECK(s.writersActive());
	// **REPEAT, NOT GRAIN — Grain joined the circuit in backlog HU.** This line named Grain until the
	// loop there was measured rather than reasoned about.
	s.mode = Mode::Repeat;
	CHECK_FALSE(s.hasFeedbackCircuit());
	CHECK_FALSE(s.writersActive());
	s.mode = Mode::Grain;
	CHECK(s.hasFeedbackCircuit());
	CHECK(s.writersActive());
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG KZ — INPUT GAIN. What the device RECORDS is scaled by the knob's dB; what it lets through  //
// dry is not. The dB law is 20·log10, so +6 dB is 10^(6/20) = 1.99526 of the input, not exactly 2.   //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE LINEAR GAIN OF `db`, restated by the law the knob is declared in rather than read off the device.
float kzGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

/// A 220 Hz / 331 Hz pair at −6 dBFS-ish — material whose every frame differs, so "frame for frame" means something.
float kzInput(long long frame, bool right) {
	const double t = static_cast<double>(frame) / kRate;
	return static_cast<float>(right ? 0.4 * std::sin(2.0 * 3.14159265358979 * 331.0 * t)
	                                : 0.5 * std::sin(2.0 * 3.14159265358979 * 220.0 * t));
}

/// CAPTURE A BAR AND A BIT OF THE PAIR at `db` with Mix at `mix`, then play one more block. Answers the ring and
/// whether every OUTPUT sample equalled its input exactly (the dry, bit for bit) — read while capturing and playing.
struct KzRun {
	std::vector<float> ring;
	bool outputIsInput = true;
};
KzRun kzCapture(double db, float mix) {
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper();
	cfg.mix = mix;
	cfg.inputGainDb = static_cast<float>(db);
	s.setTarget(cfg);
	double beats = 0.0;
	long long frame = 0;
	KzRun out;
	std::vector<float> l(kBlock), r(kBlock);
	const int blocks = (kBarFrames + 9600) / kBlock + 1;
	for (int b = 0; b < blocks; ++b) {
		for (int i = 0; i < kBlock; ++i) {
			l[static_cast<size_t>(i)] = kzInput(frame + i, false);
			r[static_cast<size_t>(i)] = kzInput(frame + i, true);
		}
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kBlock);
		for (int i = 0; i < kBlock; ++i) {
			out.outputIsInput = out.outputIsInput && l[static_cast<size_t>(i)] == kzInput(frame + i, false)
			                    && r[static_cast<size_t>(i)] == kzInput(frame + i, true);
		}
		frame += kBlock;
		beats += static_cast<double>(kBlock) / (0.5 * kRate);
	}
	REQUIRE(s.statusForTest() == Status::Playing);
	out.ring = ringOf(s);
	return out;
}

} // namespace

TEST_CASE("KZ: INPUT GAIN scales what the capture records by exactly its dB, frame for frame, and the dry is untouched") {
	const KzRun unity = kzCapture(0.0, 0.0f);
	const KzRun up = kzCapture(6.0, 0.0f);
	const KzRun down = kzCapture(-12.0, 0.0f);
	REQUIRE(unity.ring.size() == up.ring.size());
	REQUIRE(unity.ring.size() == down.ring.size());
	// NOT VACUOUS: the capture holds the material, at its level.
	REQUIRE(peakOf(unity.ring) > 0.45f);
	const float g6 = kzGain(6.0);
	const float g12 = kzGain(-12.0);
	size_t wrong6 = 0;
	size_t wrong12 = 0;
	for (size_t f = 0; f < unity.ring.size(); ++f) {
		// THE LAW, PER FRAME: the recording at +6 dB is the recording at 0 dB times 10^(6/20). One float multiply
		// apart, so within an ulp or two rather than exactly; counted so a red names how many frames disagree.
		wrong6 += std::abs(up.ring[f] - unity.ring[f] * g6) > 1e-6f ? 1 : 0;
		wrong12 += std::abs(down.ring[f] - unity.ring[f] * g12) > 1e-6f ? 1 : 0;
	}
	CHECK(wrong6 == 0);
	CHECK(wrong12 == 0);
	CHECK(peakOf(up.ring) == doctest::Approx(peakOf(unity.ring) * g6).epsilon(1e-5));

	// THE DRY IS THE INPUT, BIT FOR BIT, at every gain: Mix 0 hears only the dry, and the knob never reaches it.
	CHECK(unity.outputIsInput);
	CHECK(up.outputIsInput);
	CHECK(down.outputIsInput);
}

TEST_CASE("KZ: Dry Write records the input at the INPUT GAIN too, ramping to it over the block the knob moved in") {
	// DRY WRITE's own case, restated with the knob: a silent capture, then a constant 1 written at Dry Write 1, so
	// what lands is the input as recorded — 10^(db/20).
	for (const double db : { 6.0, -6.0, 0.0 }) {
		CAPTURE(db);
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper();
		captureTo(s, cfg, 0.0f, &beats);
		REQUIRE(ringAt(s, 0) == 0.0f);
		cfg.dryWrite = 1.0f;
		cfg.inputGainDb = static_cast<float>(db);
		s.setTarget(cfg);
		const size_t head = s.linearBarPosForTest();
		drive(s, 2 * kBlock, 1.0f, &beats);
		const float g = kzGain(db);
		// THE BLOCK THE KNOB MOVED IN RAMPS from 0 dB to the new gain, sample by sample (`seatCapture`): 1 + (g − 1)·(i + 1)/n.
		CHECK(ringAt(s, head) == doctest::Approx(1.0f + (g - 1.0f) / static_cast<float>(kBlock)).epsilon(1e-6));
		CHECK(ringAt(s, head + static_cast<size_t>(kBlock) / 2 - 1) == doctest::Approx(1.0f + (g - 1.0f) * 0.5f).epsilon(1e-6));
		CHECK(ringAt(s, head + static_cast<size_t>(kBlock) - 1) == doctest::Approx(g).epsilon(1e-6));
		// AND EVERY BLOCK AFTER IT RECORDS AT THE GAIN, flat.
		CHECK(ringAt(s, head + static_cast<size_t>(kBlock)) == doctest::Approx(g).epsilon(1e-6));
		CHECK(ringAt(s, head + 2 * static_cast<size_t>(kBlock) - 1) == doctest::Approx(g).epsilon(1e-6));
	}
}

TEST_CASE("KZ: INPUT GAIN is a keyed, modulatable dB control at 0 dB, on the card's face") {
	const device::ParamDesc& d = descOf(Param::InputGain);
	CHECK(std::string_view(d.key) == "input_gain");
	CHECK(d.min == -24.0);
	CHECK(d.max == 24.0);
	CHECK(d.def == 0.0);
	CHECK(d.unit == device::Unit::Decibel);
	CHECK(d.modulatable);
	CHECK(d.gateControl < 0); // every mode captures
	CHECK(std::find(std::begin(kFace), std::end(kFace), static_cast<uint8_t>(Param::InputGain)) != std::end(kFace));
	// AND THE ROW CARRIES IT BOTH WAYS, clamped to the declaration when a route pushes it past.
	Row row = rowOf(Settings {});
	CHECK(row[static_cast<size_t>(Param::InputGain)] == 0.0f);
	row[static_cast<size_t>(Param::InputGain)] = 40.0f;
	CHECK(stateOf(row).inputGainDb == 24.0f);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG KX — FREEZE. While it is on the ring takes NO writes at all, whatever the writers say and  //
// whatever arrives; the boundary is a crossfade over the loop seam's own 5 ms (`wrapFade_`), so the   //
// material the read heads cross where writing stopped is no harder than an ordinary loop point.      //
// Every claim is read off the RING, by value: a frozen buffer is the one thing a render cannot show.  //
//--------------------------------------------------------------------------------------------------//

namespace {

/// The seam's fade at this fixture's rate — `wrapFade_ = round(0.005 · rate)`, restated rather than read.
constexpr size_t kKxFade = 240;

/// THE WHOLE RING, BOTH CHANNELS, AS BITS — so "byte-identical" is compared as bytes, and a −0 against a +0 or
/// a NaN against itself cannot hide behind float equality.
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

/// Drive `frames` of `input(frame)` in blocks, collecting the left output.
template <typename Fn>
void driveWith(Sklitter& s, int frames, double* beats, long long* frame, Fn input, std::vector<float>* outL = nullptr,
               bool playing = true) {
	std::vector<float> l(kBlock), r(kBlock);
	for (int done = 0; done < frames; done += kBlock) {
		const int n = std::min(kBlock, frames - done);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = input(*frame + i, false);
			r[static_cast<size_t>(i)] = input(*frame + i, true);
		}
		s.setTransportClock(at120bpm(*beats, playing));
		s.processStereo(l.data(), r.data(), n);
		if (outL != nullptr) {
			outL->insert(outL->end(), l.begin(), l.begin() + n);
		}
		*frame += n;
		if (playing) {
			*beats += static_cast<double>(n) / (0.5 * kRate);
		}
	}
}

/// Deterministic noise in ±1, so "any input" is an input nobody chose to be kind.
float kxNoise(long long frame, bool right) {
	uint32_t x = static_cast<uint32_t>(frame * 2 + (right ? 1 : 0)) * 2654435761u + 0x9E3779B9u;
	x ^= x >> 15;
	x *= 0x2C1B3C6Du;
	x ^= x >> 12;
	return static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f;
}

/// Capture a bar of the KZ pair with EVERY writer at full, then hand back the device playing.
void kxCaptureWriting(Sklitter& s, Settings* cfg, double* beats, long long* frame) {
	s.prepare(kRate, 4096);
	*cfg = looper();
	cfg->pWrite = 1.0f;
	cfg->dryWrite = 1.0f;
	cfg->feedback = 1.0f;
	s.setTarget(*cfg);
	driveWith(s, kBarFrames + 9600, beats, frame, kzInput);
	REQUIRE(s.statusForTest() == Status::Playing);
}

/// Largest step between neighbouring ring frames (left) over `[from, from + count)`.
float largestStep(const Sklitter& s, size_t from, size_t count) {
	float worst = 0.0f;
	for (size_t f = from; f + 1 < from + count; ++f) {
		worst = std::max(worst, std::abs(ringAt(s, f + 1) - ringAt(s, f)));
	}
	return worst;
}

} // namespace

TEST_CASE("KX: while FROZEN the buffer takes no writes at all — byte for byte, whatever the input, every writer at full — and the slices still cut it") {
	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	long long frame = 0;
	kxCaptureWriting(s, &cfg, &beats, &frame);

	cfg.freeze = true;
	s.setTarget(cfg);
	// ONE BLOCK FOR THE RAMP (240 of its 512 frames), then the gain is exactly 0 and the buffer is what it is.
	driveWith(s, kBlock, &beats, &frame, kzInput);
	REQUIRE(s.freezeWriteGainForTest() == 0.0f);
	const std::vector<uint32_t> frozen = ringBits(s);

	// ANY INPUT: noise, a hot DC, silence — two bars of each, so every writer has crossed the whole buffer.
	std::vector<float> heard;
	driveWith(s, 2 * kBarFrames, &beats, &frame, kxNoise);
	CHECK(differingFrames(ringBits(s), frozen) == 0u);
	driveWith(s, 2 * kBarFrames, &beats, &frame, [](long long, bool) { return 8.0f; });
	CHECK(differingFrames(ringBits(s), frozen) == 0u);
	driveWith(s, 2 * kBarFrames, &beats, &frame, [](long long, bool) { return 0.25f; }, &heard);
	CHECK(differingFrames(ringBits(s), frozen) == 0u);
	// AND THE OUTPUT STILL CUTS IT: under a CONSTANT input the output moves, and most of it is not the input — so it
	// is the frozen buffer being read by the slices.
	REQUIRE_FALSE(heard.empty());
	const auto [lo, hi] = std::minmax_element(heard.begin(), heard.end());
	CHECK(*hi - *lo > 0.2f);
	CHECK(std::count(heard.begin(), heard.end(), 0.25f) < static_cast<std::ptrdiff_t>(heard.size() / 2));

	// NOT VACUOUS: the same run unfrozen rewrites most of the buffer under the noise.
	Sklitter t;
	Settings tc;
	double tb = 0.0;
	long long tf = 0;
	kxCaptureWriting(t, &tc, &tb, &tf);
	driveWith(t, kBlock, &tb, &tf, kzInput);
	const std::vector<uint32_t> before = ringBits(t);
	driveWith(t, 2 * kBarFrames, &tb, &tf, kxNoise);
	CHECK(differingFrames(ringBits(t), before) > static_cast<size_t>(kBarFrames) / 2);
}

TEST_CASE("KX: a press while FROZEN records nothing — the pre-warm, its restart, the cold reset and the seam fade all leave the buffer as it stood") {
	Sklitter s;
	s.prepare(kRate, 4096);
	Settings cfg = looper();
	cfg.engage = false; // idle: the looper pre-warms its ring
	cfg.pWrite = 1.0f;  // and every writer at full for when it plays
	cfg.dryWrite = 1.0f;
	cfg.feedback = 1.0f;
	s.setTarget(cfg);
	double beats = 0.0;
	long long frame = 0;
	driveWith(s, 4 * kBarFrames, &beats, &frame, kzInput);
	REQUIRE(s.prewarmReadyForTest());

	cfg.freeze = true;
	s.setTarget(cfg);
	driveWith(s, kBlock, &beats, &frame, kzInput);
	const std::vector<uint32_t> frozen = ringBits(s);
	REQUIRE(peakOf(ringOf(s)) > 0.45f); // it holds the pair, at its level

	// IDLE, FROZEN, under noise: the pre-warm keeps its clock and lands nothing.
	driveWith(s, kBarFrames, &beats, &frame, kxNoise);
	CHECK(differingFrames(ringBits(s), frozen) == 0u);
	// THEN THE PRESS, still frozen, and on to Playing.
	cfg.engage = true;
	s.setTarget(cfg);
	driveWith(s, kBarFrames + 9600, &beats, &frame, kxNoise);
	CHECK(s.statusForTest() == Status::Playing);
	CHECK(differingFrames(ringBits(s), frozen) == 0u);

	// AND A COLD PRESS — a frozen device loaded over an EMPTY ring (by design: it stays frozen and empty). The
	// capture counts its frames and plays; the ring is still exactly zero.
	Sklitter c;
	c.prepare(kRate, 4096);
	Settings cc = looper();
	cc.freeze = true;
	c.setTarget(cc);
	double cb = 0.0;
	long long cf = 0;
	driveWith(c, kBarFrames + 9600, &cb, &cf, kzInput);
	CHECK(c.statusForTest() == Status::Playing);
	CHECK(peakOf(ringOf(c)) == 0.0f);

	// AND THE PRE-WARM'S RESTART: a ring warmed on a STOPPED transport has no anchor, so the first rolling block
	// discards it and starts again from the next downbeat — a reset, which a frozen device skips.
	Sklitter w;
	w.prepare(kRate, 4096);
	Settings wc = looper();
	wc.engage = false;
	w.setTarget(wc);
	double wb = 0.0;
	long long wf = 0;
	driveWith(w, kBarFrames + kBlock, &wb, &wf, kzInput, nullptr, /*playing=*/false);
	REQUIRE(peakOf(ringOf(w)) > 0.45f);
	wc.freeze = true;
	w.setTarget(wc);
	driveWith(w, kBlock, &wb, &wf, kzInput, nullptr, /*playing=*/false);
	const std::vector<uint32_t> stopped = ringBits(w);
	driveWith(w, 2 * kBarFrames, &wb, &wf, kxNoise); // the transport rolls: re-anchor, restart, re-warm
	CHECK(differingFrames(ringBits(w), stopped) == 0u);
}

TEST_CASE("KX: UNFREEZING writes again — the gain ramps back over the seam's 5 ms and then the writers land exactly as configured") {
	// A FROZEN CAPTURE leaves the prepared ring at exactly 0; Dry Write at full then blends a constant 1 by the gain.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	cfg.freeze = true;
	captureTo(s, cfg, 0.5f, &beats);
	REQUIRE(peakOf(ringOf(s)) == 0.0f);
	cfg.dryWrite = 1.0f;
	s.setTarget(cfg);
	drive(s, 2 * kBlock, 1.0f, &beats);
	REQUIRE(peakOf(ringOf(s)) == 0.0f); // frozen: Dry Write wrote nothing

	cfg.freeze = false;
	s.setTarget(cfg);
	const size_t head = s.linearBarPosForTest();
	drive(s, 2 * kBlock, 1.0f, &beats);
	CHECK(s.freezeWriteGainForTest() == 1.0f);
	// THE RAMP, restated: sample j writes at gain (j + 1)/240 over a 0, so it holds exactly that gain.
	for (const size_t j : { size_t { 0 }, size_t { 119 }, kKxFade - 1 }) {
		CAPTURE(j);
		CHECK(ringAt(s, head + j) == doctest::Approx(static_cast<float>(j + 1) / static_cast<float>(kKxFade)).epsilon(1e-5));
	}
	// AND PAST IT, THE INPUT ITSELF, exactly, to the end of what was driven — from the sample the gain is clamped onto
	// 1, since 240 float steps of 1/240 land a hair short of it.
	CHECK(ringAt(s, head + kKxFade) == 1.0f);
	CHECK(ringAt(s, head + kKxFade + 100) == 1.0f);
	CHECK(ringAt(s, head + 2 * static_cast<size_t>(kBlock) - 1) == 1.0f);
	CHECK(ringAt(s, head + 2 * static_cast<size_t>(kBlock)) == 0.0f); // and not a frame further
}

TEST_CASE("KX: the freeze boundary is a crossfade — no step across it larger than the 5 ms ramp's, on the way in or out") {
	// A SILENT CAPTURE, then Dry Write at full over a constant 1: behind the head the ring is 1, ahead it is 0. A
	// write that simply STOPPED would leave a step of exactly 1 where it stopped, which is the click the read
	// heads would cross every pass; the ramp spreads it over 240 frames, a step of 1/240.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	captureTo(s, cfg, 0.0f, &beats);
	cfg.dryWrite = 1.0f;
	s.setTarget(cfg);
	drive(s, 2 * kBlock, 1.0f, &beats);

	cfg.freeze = true;
	s.setTarget(cfg);
	const size_t in = s.linearBarPosForTest();
	drive(s, 4 * kBlock, 1.0f, &beats);
	const float bound = 1.0f / static_cast<float>(kKxFade) + 1e-6f;
	// NOT VACUOUS: the seam is there to cross — fresh material before it, the old silence after.
	REQUIRE(ringAt(s, in - 1) == 1.0f);
	REQUIRE(ringAt(s, in + kKxFade + 16) == 0.0f);
	CHECK(largestStep(s, in - 64, kKxFade + 128) <= bound);

	// AND ON THE WAY OUT, the same bound where writing resumes over the silence.
	cfg.freeze = false;
	s.setTarget(cfg);
	const size_t out = s.linearBarPosForTest();
	drive(s, 2 * kBlock, 1.0f, &beats);
	REQUIRE(ringAt(s, out - 1) == 0.0f);
	REQUIRE(ringAt(s, out + kKxFade + 16) == 1.0f);
	CHECK(largestStep(s, out - 64, kKxFade + 128) <= bound);
}

TEST_CASE("backlog GT: a LOCKED STOP leaves no seam where the writers stopped — KX's gate ramps them out over 5 ms, and back in at the resume") {
	// KX's fixture, with the host stopping instead of Freeze being pressed. MEASURED BEFORE THE RAMP: the stop
	// ended Dry Write in one sample and left a step of exactly 1 in the ring — the click every read pass that
	// crosses it would play.
	Sklitter s;
	double beats = 0.0;
	Settings cfg = looper();
	cfg.transportLocked = true;
	captureTo(s, cfg, 0.0f, &beats);
	cfg.dryWrite = 1.0f;
	s.setTarget(cfg);
	drive(s, 2 * kBlock, 1.0f, &beats);

	const auto driveStopped = [&](int frames) {
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		for (int done = 0; done < frames; done += kBlock) {
			std::fill(l.begin(), l.end(), 1.0f);
			std::fill(r.begin(), r.end(), 1.0f);
			s.setTransportClock(at120bpm(beats, false));
			s.processStereo(l.data(), r.data(), kBlock);
		}
	};
	const float bound = 1.0f / static_cast<float>(kKxFade) + 1e-6f;
	const size_t len = s.ringForTest().length();

	// THE STOP.
	const size_t in = s.linearBarPosForTest();
	driveStopped(4 * kBlock);
	CHECK(s.parkedForTest());
	CHECK(s.freezeWriteGainForTest() == 0.0f); // the writers are out, by KX's gate
	REQUIRE(ringAt(s, in - 1) == 1.0f);        // NOT VACUOUS: fresh material before the stop
	REQUIRE(ringAt(s, in + kKxFade + 16) == 0.0f); // the old silence after it
	CHECK(largestStep(s, in - 64, kKxFade + 128) <= bound);

	// THE RESUME, somewhere else in the song (a stopped host is moved as often as not). Under the lock the
	// write head lands on the song's phase at the first frame, so it is where the head stands one block on.
	beats += 1.3;
	drive(s, kBlock, 1.0f, &beats);
	CHECK(!s.parkedForTest());
	const size_t out = (s.linearBarPosForTest() + len - static_cast<size_t>(kBlock)) % len;
	drive(s, kBlock, 1.0f, &beats);
	REQUIRE(ringAt(s, out - 1) == 0.0f);
	REQUIRE(ringAt(s, out + kKxFade + 16) == 1.0f);
	CHECK(largestStep(s, out - 64, kKxFade + 128) <= bound);
	CHECK(s.freezeWriteGainForTest() == 1.0f);
}

TEST_CASE("KX: FREEZE is a keyed, modulatable Off/On switch at Off, on the card's face, crossing at 0.5") {
	const device::ParamDesc& d = descOf(Param::Freeze);
	CHECK(std::string_view(d.key) == "freeze");
	CHECK(d.def == 0.0);
	CHECK(d.stepped);
	CHECK(device::isOffOnChoice(d)); // a SwitchChip, never a multivalue toggle
	CHECK(d.modulatable);
	CHECK(d.gateControl < 0); // every mode writes its ring
	CHECK(std::find(std::begin(kFace), std::end(kFace), static_cast<uint8_t>(Param::Freeze)) != std::end(kFace));
	Row row = rowOf(Settings {});
	CHECK(row[static_cast<size_t>(Param::Freeze)] == 0.0f);
	row[static_cast<size_t>(Param::Freeze)] = 0.49f;
	CHECK_FALSE(stateOf(row).freeze);
	row[static_cast<size_t>(Param::Freeze)] = 0.5f;
	CHECK(stateOf(row).freeze);
	Settings on;
	on.freeze = true;
	CHECK(rowOf(on)[static_cast<size_t>(Param::Freeze)] == 1.0f);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG EU — THE WRITE-DRY MODE IS DRY WRITE AT FULL. EU (2026-09-16) asked for a mode in which the //
// ring records the input continuously while it plays, at the LINEAR bar position and with no overlap //
// duck. GU's Dry Write (2026-09-20, de3b60ed) is exactly that write at a dialled depth, so EU is      //
// delivered by Dry Write = 100% rather than by a second control. This case is the claim, by value.   //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("EU: Dry Write at full keeps the ring a rolling recording of the input while the device plays, every frame and across the bar's wrap") {
	// EVERY MODE THE CONTINUOUS WRITERS ACT IN (`kWriterModes`: the looper modes but Repeat, which they would erase).
	for (const Mode m : { Mode::Shuffle, Mode::Time, Mode::Pattern, Mode::Grain, Mode::Pitch }) {
		CAPTURE(static_cast<int>(m));
		Sklitter s;
		double beats = 0.0;
		Settings cfg = looper(m);
		captureTo(s, cfg, 0.0f, &beats);
		cfg.dryWrite = 1.0f;
		s.setTarget(cfg);
		// MORE THAN A BAR, block by block, each block its own level, so every frame of the ring was written by THIS pass
		// and the most recent bar of input is what it must hold — a frozen loop or a write that followed the read head
		// would hold something else.
		const int blocks = kBarFrames / kBlock + 40;
		std::vector<std::pair<size_t, float>> written;
		for (int b = 0; b < blocks; ++b) {
			const float level = 0.001f * static_cast<float>(b + 1);
			written.emplace_back(s.linearBarPosForTest(), level);
			drive(s, kBlock, level, &beats);
			REQUIRE(s.statusForTest() == Status::Playing); // it played the whole time
		}
		// THE LAST BAR'S WORTH OF BLOCKS: each one's frames hold exactly its level.
		size_t checked = 0;
		bool allExact = true;
		size_t misses = 0;
		for (size_t k = written.size() - static_cast<size_t>(kBarFrames / kBlock); k < written.size(); ++k) {
			const size_t head = written[k].first;
			for (size_t i = 0; i < static_cast<size_t>(kBlock); i += 37) {
				// TO AN ULP: the full-depth blend is `old + (src − old)·1`, which is `src` up to one rounding.
				const bool ok = std::abs(ringAt(s, head + i) - written[k].second) <= 1e-6f;
				misses += ok ? 0 : 1;
				allExact = allExact && ok;
				++checked;
			}
		}
		CHECK(allExact);
		CHECK(misses == 0);
		CHECK(checked > 2000);
		// AND IT WRAPPED: the write head came round the bar, so the oldest of those blocks sits after the newest.
		CHECK(written.back().first < written[written.size() - static_cast<size_t>(kBarFrames / kBlock)].first);
	}
}
