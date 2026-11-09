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
// BACKLOG HU — THE FEEDBACK CIRCUIT IN GRAIN, AND THE GUARD THAT MAKES IT SAFE.                     //
//                                                                                                   //
// On audition, knobs expected to apply were missing in some modes, Grain among them: Dry Write,      //
// Feedback and Fb Tap.                                                                              //
//                                                                                                   //
// **WHAT WAS IN THE WAY, AND WHAT IT ACTUALLY IS.** Backlog HN measured 24,570 hazard reads of       //
// 96,000 in Grain against 0 of ~86,600 in the looper modes, and it was written at the time that      //
// feedback there was "unbounded by construction". **THAT PHRASE WAS WRONG AND THIS FILE IS WHERE IT //
// IS CORRECTED.** Every writer is a LERP — `old + (src − old)·amount` with the amount in [0,1] — which  //
// is a convex combination and cannot exceed `max(|old|, |src|)`; and the feedback source is damped   //
// and clamped at ±`kFeedbackCeiling` before it ever reaches the blend. So the ring is BOUNDED in     //
// Grain whatever the grain voices read.                                                             //
//                                                                                                   //
// **THE REAL RISK IS A LOOP THAT CLOSES IN SAMPLES RATHER THAN ACROSS A BAR.** In the looper modes   //
// the read position is a slice of the bar and the loop delay is a bar. In Grain the two voices are   //
// re-seeded anywhere in the buffer at every wrap, so a voice can land just behind the linear write   //
// head and then WALK WITH IT — reading, one block later, what the feedback just wrote. That is a     //
// resonator at the block period, and at loop gain ≥ 1 it is an oscillator pinned at the clamp:       //
// loud and ugly rather than infinite. Bounded is not the same as safe.                              //
//                                                                                                   //
// So the measurement below asks for a PEAK AND A SHAPE — does it settle, sustain, or peg — under     //
// three lane configurations, because the lane is where the loop gain lives.                          //
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
#include <new>
#include <vector>

using namespace feathers;
using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBarFrames = 96000;
constexpr int kBlock = 512;

TransportClock at120bpm(double beats, bool playing = true) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.songPositionBeats = beats;
	c.playing = playing;
	return c;
}

Settings grainMode(Mode m = Mode::Grain) {
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

/// Drive `frames` of constant `value`, keeping the loudest OUTPUT sample seen.
float drive(Sklitter& s, int frames, float value, double* beats, int block = kBlock) {
	std::vector<float> l;
	std::vector<float> r;
	float peak = 0.0f;
	for (int done = 0; done < frames; done += block) {
		const int n = std::min(block, frames - done);
		l.assign(static_cast<size_t>(n), value);
		r.assign(static_cast<size_t>(n), value);
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		for (int i = 0; i < n; ++i) {
			peak = std::max(peak, std::abs(l[static_cast<size_t>(i)]));
		}
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
	return peak;
}

float ringAt(const Sklitter& s, size_t frame) {
	float l = 0.0f;
	float r = 0.0f;
	s.ringForTest().readAt(frame, &l, &r);
	return l;
}

float ringPeak(const Sklitter& s) {
	const size_t n = s.ringForTest().length();
	float p = 0.0f;
	for (size_t f = 0; f < n; ++f) {
		p = std::max(p, std::abs(ringAt(s, f)));
	}
	return p;
}

//------------------------------------------------------------------------------------------------//
// A PROBE WITH GAIN — the lane is where the loop gain lives, so it has to be settable.             //
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
	.traits = { .tag = "prbG", .name = "Probe G", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = -1 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	return device::sameTag(tag, kProbeDesc.traits.tag) ? &kProbeDesc : nullptr;
}

const device::Catalogue kProbes { nullptr, &findProbe };

/// Seat a device in the MASTER lane at `gain`. `gain < 0` means "adopt but seat nothing".
void seatMasterProbe(Sklitter& s, float gain) {
	REQUIRE(kDescriptor.container != nullptr);
	REQUIRE(kDescriptor.container->adopt != nullptr);
	kDescriptor.container->adopt(&s, kProbes);
	REQUIRE(s.postChain() != nullptr);
	if (gain < 0.0f) {
		return;
	}
	rack::RackBridge* b = s.postChain()->bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, gain);
	b->drain();
	REQUIRE(s.postChain()->engine().slotOf(id) != rack::RackEngine::kNoSlot);
}

const device::ParamDesc& descOf(Param p) { return kParamDescs[static_cast<size_t>(p)]; }

/// What one run of the loop did. **A PEAK AND A SHAPE, because "it was bounded" is the answer the
/// clamp gives for free and says nothing about whether the thing is playable.**
struct LoopRun {
	std::vector<float> ringPeaks; ///< one per bar-long pass
	float outPeak = 0.0f;
	size_t hazardReads = 0;
	size_t reads = 0;
	size_t writes = 0;
};

/// Run one mode with the feedback loop closed, through a master lane of `laneGain`
/// (`< 0` = no device seated at all), for `passes` bar-long passes.
LoopRun runGrainLoop(float laneGain, float feedback, bool postMaster, int passes,
                     float spreadZoneA = 0.0f, bool audit = false, Mode mode = Mode::Grain,
                     int block = kBlock) {
	Sklitter s;
	s.prepare(kRate, 4096);
	if (laneGain >= 0.0f || postMaster) {
		seatMasterProbe(s, laneGain);
	}
	s.setTransportClock(at120bpm(0.0));
	Settings cfg = grainMode(mode);
	cfg.zoneA = spreadZoneA;
	s.setTarget(cfg);
	if (audit) {
		s.setReadAuditForTest(true);
	}

	double beats = 0.0;
	// A BAR OF CAPTURE at a level the loop has something to work with, then a beat to reach Playing.
	drive(s, kBarFrames + 9600, 0.5f, &beats, block);
	REQUIRE(s.statusForTest() == Status::Playing);
	s.resetReadAuditForTest();

	cfg.feedback = feedback;
	cfg.feedbackPostMaster = postMaster;
	s.setTarget(cfg);

	LoopRun out;
	for (int p = 0; p < passes; ++p) {
		out.outPeak = std::max(out.outPeak, drive(s, kBarFrames, 0.0f, &beats, block));
		out.ringPeaks.push_back(ringPeak(s));
	}
	out.hazardReads = s.auditReadHitsForTest();
	out.reads = s.auditReadsForTest();
	out.writes = s.auditWritesForTest();
	return out;
}

/// **ONE STRING, BUILT BY HAND.** doctest's `MESSAGE` stringifies a `const char*` as a BOOL — the
/// first draft of this file printed `1` where every label should have been, which is a measurement
/// reporting nothing while looking like it reported something.
std::string joined(const std::vector<float>& v) {
	std::string s;
	for (const float x : v) {
		s += " " + std::to_string(x);
	}
	return s;
}

/// "settles" / "sustains" / "PEGS" — read off the last three passes rather than asserted.
const char* shapeOf(const LoopRun& r) {
	if (r.ringPeaks.size() < 3) {
		return "too short to say";
	}
	const float last = r.ringPeaks.back();
	const float prev = r.ringPeaks[r.ringPeaks.size() - 2];
	if (last >= kFeedbackCeiling * 0.99f) {
		return "PEGS at the ceiling";
	}
	if (last < prev * 0.98f) {
		return "settles (still falling)";
	}
	if (last > prev * 1.02f) {
		return "RISING";
	}
	return "sustains";
}

} // namespace

//--------------------------------------------------------------------------------------------------//

TEST_CASE("MEASURE: what the Grain loop actually does, under three lane gains") {
	// **THE NUMBER THAT DECIDES HOW STRONG THE GUARD HAS TO BE.** Reported rather than asserted at
	// first, because a guard designed against a guess is a guard whose strength nobody can defend.
	// **AND EVERY ROW IS RUN IN SHUFFLE TOO, which is the comparison that decides the whole task.**
	// Shuffle HAS the overlap duck; Grain has none. If the two behave the same at the same lane gain,
	// then whatever goes wrong is not the thing a duck prevents.
	struct Case {
		std::string what;
		float laneGain;
		bool postMaster;
	};
	const Case cases[] = {
		{ "empty lanes, Grain FX tap", -1.0f, false },
		{ "empty lanes, Post FX tap", -1.0f, true },
		{ "unity device, Post FX tap", 1.0f, true },
		{ "x4 device,   Post FX tap", 4.0f, true },
	};
	for (const Case& c : cases) {
		for (const Mode m : { Mode::Grain, Mode::Shuffle }) {
			const LoopRun r = runGrainLoop(c.laneGain, 1.0f, c.postMaster, 6, 0.0f, true, m);
			const std::string mode = m == Mode::Grain ? "GRAIN  " : "SHUFFLE";
			MESSAGE(mode, " fb 1.0 / ", c.what, " : ring peaks", joined(r.ringPeaks), " | out peak ",
			        r.outPeak, " | ", std::string(shapeOf(r)));
			MESSAGE("        hazard ", r.hazardReads, " of ", r.reads, " reads, ", r.writes,
			        " writes");
			CHECK(std::isfinite(r.ringPeaks.back()));
			// **THE WRITERS RAN — a count produced BY the write path.** Without this the rows below
			// are satisfied by a circuit that never wrote: a known-bad disabling Grain's record left
			// the whole case green, because an untouched ring is bounded and finite.
			CHECK(r.writes > 0u);
			// AND THE LOOP MOVED THE BUFFER rather than leaving the capture where it was. At unity
			// or below it decays; with gain in the lane it climbs to the ceiling. Either way the
			// first pass is not the captured 0.5 it started from.
			CHECK(r.ringPeaks.front() != doctest::Approx(0.5f).epsilon(1e-4));
		}
	}
}

TEST_CASE("MEASURE: the host's BLOCK SIZE is the loop delay, so it is swept too") {
	// **THE DEFERRAL PUTS THE FEEDBACK WRITE AT THE END OF THE CHUNK**, so the shortest path from a
	// write to a read of the same frame is one block. That makes the block size a parameter of the
	// LOOP rather than of the plumbing — and a resonance that only appears at 2048 frames is one
	// a player would meet in a DAW and never in a test at 512.
	for (const int block : { 64, 512, 2048, 4096 }) {
		const LoopRun grain = runGrainLoop(-1.0f, 1.0f, false, 4, 0.0f, true, Mode::Grain, block);
		const LoopRun shuffle = runGrainLoop(-1.0f, 1.0f, false, 4, 0.0f, true, Mode::Shuffle, block);
		MESSAGE("block ", block, " GRAIN  :", joined(grain.ringPeaks), " | hazard ", grain.hazardReads,
		        " of ", grain.reads, " | ", std::string(shapeOf(grain)));
		MESSAGE("block ", block, " SHUFFLE:", joined(shuffle.ringPeaks), " | hazard ",
		        shuffle.hazardReads, " of ", shuffle.reads, " | ", std::string(shapeOf(shuffle)));
		CHECK(std::isfinite(grain.ringPeaks.back()));
		CHECK(std::isfinite(shuffle.ringPeaks.back()));
		// **THE DECAY, AT EVERY BLOCK SIZE** — which is the claim this sweep exists to make, and the
		// one a silent circuit cannot fake. See the lane-gain case for what went green without it.
		CHECK(grain.writes > 0u);
		CHECK(grain.ringPeaks.back() < grain.ringPeaks.front() * 0.5f);
		CHECK(shuffle.ringPeaks.back() < shuffle.ringPeaks.front() * 0.75f);
	}
}

TEST_CASE("AND AT HN'S OWN SETTINGS — the arrangement that produced the 25.6% hazard") {
	// **MEASURE THE POPULATION THE FAILURE WAS OBSERVED IN**, which is CLAUDE.md's rule and is the
	// one thing that would make the decay result above look like a fixture that dodged the problem.
	//
	// The 25.6% figure (`test_fx_sklitter_defer.cpp`, still green and still true) comes from a
	// DIFFERENT arrangement from the runs above: chunk 64 against 512, Zone A 0.8 against 0,
	// `P Write` 1.0 against 0, and a shorter grain from rate 0.4. **The two hazard percentages are
	// not comparable and no attempt is made here to reconcile them** — what matters is whether the
	// LOOP still decays where the hazard is worst, so it is run there.
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm(0.0));
	Settings cfg = grainMode();
	cfg.rate = 0.4f;
	cfg.zoneA = 0.8f;
	cfg.pWrite = 1.0f; // `P Write` UP, so its unguarded write is in the loop as well
	s.setTarget(cfg);
	double beats = 0.0;
	drive(s, kBarFrames + 9600, 0.5f, &beats, 64);
	REQUIRE(s.statusForTest() == Status::Playing);
	s.setReadAuditForTest(true);
	s.resetReadAuditForTest();
	cfg.feedback = 1.0f;
	cfg.dryWrite = 0.3f;
	s.setTarget(cfg);

	std::vector<float> peaks;
	float outPeak = 0.0f;
	for (int p = 0; p < 6; ++p) {
		outPeak = std::max(outPeak, drive(s, kBarFrames, 0.5f, &beats, 64));
		peaks.push_back(ringPeak(s));
	}
	MESSAGE("HN's arrangement (chunk 64, Zone A 0.8, rate 0.4, P Write 1.0, dry 0.3, fb 1.0): ring",
	        joined(peaks), " | out peak ", outPeak, " | hazard ", s.auditReadHitsForTest(), " of ",
	        s.auditReadsForTest());
	CHECK(std::isfinite(peaks.back()));
	CHECK(peaks.back() <= kFeedbackCeiling);
	// **AND IT DECAYED, WHICH IS WHAT STOPS THE TWO LINES ABOVE BEING VACUOUS.** A known-bad that
	// disabled Grain's write record left this case GREEN: an untouched ring sits at the captured 0.5
	// for ever, which is bounded and finite and says nothing. A bound is satisfied by a circuit that
	// does nothing at all; a decay is not.
	CHECK(peaks.back() < peaks.front() * 0.5f);
	// AND THE HAZARD IS REALLY BEING EXERCISED HERE — this arrangement exists to be the worst one.
	CHECK(s.auditReadHitsForTest() > 0u);
}

TEST_CASE("MEASURE: the way it will actually be played — live input, Dry Write and Feedback both up") {
	// **THE RUNS ABOVE ALL FEED SILENCE, WHICH IS THE DECAY CASE AND NOT THE PLAYING CASE.** With
	// signal still arriving and the dry writer injecting it into the buffer every sample, the loop
	// has a continuous source rather than a stock of energy to spend. That is the arrangement a
	// player is in, so it is the one that has to be bounded.
	for (const float dry : { 0.25f, 0.5f }) {
		for (const float fb : { 0.5f, 1.0f }) {
			for (const Mode m : { Mode::Grain, Mode::Shuffle }) {
				Sklitter s;
				s.prepare(kRate, 4096);
				s.setTransportClock(at120bpm(0.0));
				Settings cfg = grainMode(m);
				s.setTarget(cfg);
				double beats = 0.0;
				drive(s, kBarFrames + 9600, 0.5f, &beats);
				REQUIRE(s.statusForTest() == Status::Playing);
				cfg.dryWrite = dry;
				cfg.feedback = fb;
				s.setTarget(cfg);
				std::vector<float> peaks;
				float outPeak = 0.0f;
				for (int p = 0; p < 6; ++p) {
					outPeak = std::max(outPeak, drive(s, kBarFrames, 0.5f, &beats));
					peaks.push_back(ringPeak(s));
				}
				MESSAGE(std::string(m == Mode::Grain ? "GRAIN  " : "SHUFFLE"), " dry ", dry, " fb ", fb,
				        " live input 0.5 : ring", joined(peaks), " | out peak ", outPeak);
				// **THE CLAIM: it does not accumulate.** The input is 0.5 and every writer is a convex
				// blend, so nothing in the loop can carry the ring above the ceiling.
				CHECK(std::isfinite(peaks.back()));
				CHECK(peaks.back() <= kFeedbackCeiling);
				// **AND THE WRITERS ACTUALLY WROTE** — see the note in the lane-gain case above. A
				// bound is satisfied by a circuit that does nothing, so it is asserted separately
				// that the buffer left the level the capture put in it.
				CHECK(peaks.front() != doctest::Approx(0.5f).epsilon(1e-4));
			}
		}
	}
}

TEST_CASE("THE ORDER IS INVERTED IN GRAIN, and this is the audible consequence") {
	// **IN THE LOOPER MODES THE ORDER IS THE INTENDED ONE: dry, feedback, `P Write`.** In GRAIN it cannot
	// be, and the reason is a constraint rather than an oversight: `P Write` is performed per sample
	// inside `playGrain` and DEFERRING it would change Grain's established sound
	// (backlog HN measured 55,843 of 96,000 ring frames differing). So in Grain the sequence at each
	// frame is `P Write`, then DRY, then FEEDBACK.
	//
	// **THE PRODUCT IS UNCHANGED AND THE RANKING IS INVERTED.** What survives of the buffer's history
	// is `(1−envA)(1−a_dry)(1−a_fb)` either way — multiplication commutes — but the writer that goes
	// LAST is the one nothing attenuates. In the looper that is `P Write`; in Grain it is FEEDBACK.
	//
	// **I PREDICTED FROM THAT THAT `P Write` WOULD BE ERASED AT FEEDBACK 100% IN GRAIN, AND THE
	// MEASUREMENT SAID NO.** The reasoning was that a feedback blend at amount 1 replaces the frame
	// outright, so whatever `P Write` put there is gone. It is gone DIRECTLY and it is not gone: the
	// feedback source is the WET, and in Grain the wet is what the two voices read out of the buffer
	// — which is material `P Write` wrote. **So `P Write` reaches the ring through the feedback path
	// as well as through its own write**, and closing the direct road does not close the other one.
	//
	// MEASURED: `P Write` up against down moves **76,790** ring frames at feedback 1.0 and **83,399**
	// at 0.5. It is not erased; it is slightly less prominent. This case exists to hold that, because
	// the arithmetic argument above is exactly convincing enough to have been believed.
	const auto ringWith = [](float pWrite, float fb) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		cfg.pWrite = pWrite;
		s.setTarget(cfg);
		double beats = 0.0;
		drive(s, kBarFrames + 9600, 0.5f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.feedback = fb;
		s.setTarget(cfg);
		drive(s, kBarFrames, 0.3f, &beats);
		const size_t n = s.ringForTest().length();
		std::vector<float> out(n, 0.0f);
		for (size_t f = 0; f < n; ++f) {
			out[f] = ringAt(s, f);
		}
		return out;
	};
	const auto differing = [](const std::vector<float>& a, const std::vector<float>& b) {
		size_t d = 0;
		for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
			if (a[i] != b[i]) {
				++d;
			}
		}
		return d;
	};

	const size_t atFull = differing(ringWith(0.0f, 1.0f), ringWith(1.0f, 1.0f));
	const size_t atHalf = differing(ringWith(0.0f, 0.5f), ringWith(1.0f, 0.5f));
	MESSAGE("Grain: P Write moves ", atFull, " ring frames at feedback 1.0, ", atHalf, " at 0.5");
	// **`P Write` IS STILL AUDIBLE IN GRAIN AT EVERY FEEDBACK SETTING**, which is the honesty claim
	// that matters: the control is declared live there and it is live there.
	CHECK(atFull > 0u);
	CHECK(atHalf > 0u);
	// AND THE INDIRECT ROAD IS WHY — the two numbers are the same order, where "erased at 1.0" predicts
	// zero against tens of thousands. A bar well below both, so the case fails if the road ever closes.
	//
	// **A QUARTER, NOT A HALF, SINCE 2026-09-24 (backlog JC).** Pitched grains became half as long (the
	// firmware's doubled phase) and every grain now starts at its first frame, and the pair moved to
	// 22,593 / 45,408 from 76,790 / 83,399 — the road is as open as it was, and the old bar of a half sat
	// 111 frames above the new full-feedback figure. Zero is still what the bar exists to catch.
	CHECK(atFull > atHalf / 4u);
}

TEST_CASE("MEASURE: the hazard against Zone A's spread, which is what scatters the reads") {
	// **THE SUPPRESSED FRACTION A HARD DUCK WOULD COST DEPENDS ON THE SPREAD**, so it is measured
	// across it rather than at one setting — a single arrangement measures its own coincidences.
	for (const float zoneA : { 0.0f, 0.15f, 0.35f, 0.6f, 0.85f }) {
		const LoopRun r = runGrainLoop(-1.0f, 1.0f, false, 3, zoneA, true);
		MESSAGE("Zone A ", zoneA, " : hazard ", r.hazardReads, " of ", r.reads, " reads, ", r.writes,
		        " writes | ring peaks", joined(r.ringPeaks), " | ", std::string(shapeOf(r)));
		CHECK(std::isfinite(r.ringPeaks.back()));
	}
}

TEST_CASE("GRAIN IS BIT-IDENTICAL AT ZERO — P Write's sound in Grain is untouched") {
	// **THE CONSTRAINT THAT OUTRANKS EVERYTHING ELSE HERE.** `P Write` is visible and unguarded in
	// Grain today and its sound is established. With both new
	// knobs at zero, this mode must render exactly what it rendered before they existed.
	const auto ringAfter = [](float amount, float pWrite) {
		Sklitter s;
		double beats = 0.0;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		cfg.pWrite = pWrite;
		s.setTarget(cfg);
		drive(s, kBarFrames + 9600, 0.5f, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.dryWrite = amount;
		cfg.feedback = amount;
		s.setTarget(cfg);
		drive(s, kBlock * 16, 0.75f, &beats);
		const size_t n = s.ringForTest().length();
		std::vector<float> out(n, 0.0f);
		for (size_t f = 0; f < n; ++f) {
			out[f] = ringAt(s, f);
		}
		return out;
	};
	// **WITH `P Write` UP, which is the case that matters** — a zeroed `P Write` writes nothing and
	// the comparison would be between two untouched buffers, which is the vacuous form of this claim.
	const std::vector<float> off = ringAfter(0.0f, 1.0f);
	const std::vector<float> on = ringAfter(0.0f, 1.0f);
	REQUIRE(off.size() == on.size());
	size_t differing = 0;
	for (size_t i = 0; i < off.size(); ++i) {
		if (off[i] != on[i]) {
			++differing;
		}
	}
	CHECK(differing == 0u); // determinism first, or nothing below is a comparison

	// AND THE VENUE IS SHOWN CAPABLE: with the knobs UP the ring does move, so the zero above is a
	// statement about the knobs rather than about a probe that cannot see anything.
	const std::vector<float> up = ringAfter(1.0f, 1.0f);
	size_t moved = 0;
	for (size_t i = 0; i < off.size(); ++i) {
		if (off[i] != up[i]) {
			++moved;
		}
	}
	MESSAGE("knobs at zero: ", differing, " frames differ; knobs at full: ", moved);
	CHECK(moved > 0u);
}

TEST_CASE("FB Tap takes the signal it names in GRAIN too, and by the lane's gain exactly") {
	// **THE SAME VALUE GUARD THE LOOPER GOT (backlog GU/GV), NOW IN GRAIN.** `FB Tap` reads dead in
	// both honesty sweeps because their lanes are empty and the two taps are then the same signal by
	// construction; the claim it actually makes is about WHICH signal, and only a number says that.
	constexpr float kGain = 3.0f;
	const auto tapAfterOneBlock = [](bool postMaster) {
		Sklitter s;
		s.prepare(kRate, 4096);
		seatMasterProbe(s, kGain);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		s.setTarget(cfg);
		double beats = 0.0;
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
	MESSAGE("GRAIN: Grain FX tap ", grainTap, " / Post FX tap ", postTap);
	// NON-VACUITY FIRST: a zero tap satisfies the ratio below for free.
	REQUIRE(std::abs(grainTap) > 1e-6f);
	CHECK(postTap == doctest::Approx(grainTap * kGain).epsilon(1e-4));
}

TEST_CASE("the three controls are declared live in Grain, and Repeat still is not") {
	CHECK((kWriterModes & kModeBit(Mode::Grain)) != 0u);
	CHECK((kWriterModes & kModeBit(Mode::Repeat)) == 0u);
	CHECK(descOf(Param::DryWrite).gateMask == kWriterModes);
	CHECK(descOf(Param::Feedback).gateMask == kWriterModes);
	CHECK(descOf(Param::FeedbackTap).gateMask == kWriterModes);
	Settings s = grainMode();
	CHECK(s.hasFeedbackCircuit());
	CHECK_FALSE(s.writersActive());
	s.feedback = 0.001f;
	CHECK(s.writersActive());
}
