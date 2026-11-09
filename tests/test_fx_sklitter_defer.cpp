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
// BACKLOG HN — IS THE DEFERRED pWRITE EXACT? A FEASIBILITY PROOF, NOT A FEATURE.                   //
//                                                                                                   //
// Backlog GV wants a feedback circuit whose source is post-LANE material. `Sklitter::renderChunk`   //
// runs `renderStereo` (which writes the ring per sample) BEFORE `grainChain_->process` and          //
// `runPostChain`, so at the instant of a write the post-lane signal for that sample does not exist. //
// The proposed fix is to DEFER the writes: record position, source and mix during the render, and   //
// perform them once the lanes have returned.                                                        //
//                                                                                                   //
// **THE CLAIM UNDER TEST IS NOT "IT SOUNDS THE SAME". IT IS BIT IDENTITY OF THE RING**, with the    //
// lane absent, because the deferral's only defensible justification is that nothing read during a   //
// block comes from a position written during that block. That claim was offered as an inference     //
// from the overlap duck (`sklitter.cpp`'s "THE OVERLAP DUCK, which is the feedback guard"). These   //
// cases measure it instead, and the audit counter beside them says whether the material could have  //
// expressed a difference at all — a bit-identity result on material with no hazard in it proves     //
// nothing, and that vacuity is the failure mode this file is built to avoid.                        //
//--------------------------------------------------------------------------------------------------//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kBarFrames = 96000; // 4 beats at 120 BPM, 48 kHz

TransportClock at120bpm(double beats = 0.0, bool playing = true) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.songPositionBeats = beats;
	c.playing = playing;
	return c;
}

Settings looper(Mode m, float rate = 0.5f) {
	Settings s;
	s.mode = m;
	s.enabled = true;
	s.engage = true;
	s.rate = rate;
	s.density = 1.0f;
	s.pWrite = 0.0f;
	return s;
}

/// What one run produced. **THE RING IS THE SUBJECT**, because the output is shaped by envelopes and
/// would differ for a dozen reasons that have nothing to do with where a write landed.
struct Run {
	std::vector<float> ring;
	std::vector<float> outL;
	std::vector<float> outR;
	size_t hits = 0;
	size_t reads = 0;
	size_t writes = 0;
};

/// Drive with a ramp so every frame is distinguishable from every other, in blocks of `chunk`.
void drive(Sklitter& s, int frames, float startValue, double* beats, int chunk,
           std::vector<float>* keepL = nullptr, std::vector<float>* keepR = nullptr) {
	double at = *beats;
	std::vector<float> l;
	std::vector<float> r;
	for (int done = 0; done < frames; done += chunk) {
		const int n = std::min(chunk, frames - done);
		l.assign(static_cast<size_t>(n), 0.0f);
		r.assign(static_cast<size_t>(n), 0.0f);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = startValue + static_cast<float>(done + i);
			r[static_cast<size_t>(i)] = -(startValue + static_cast<float>(done + i));
		}
		s.setTransportClock(at120bpm(at));
		s.processStereo(l.data(), r.data(), n);
		if (keepL != nullptr) {
			keepL->insert(keepL->end(), l.begin(), l.end());
			keepR->insert(keepR->end(), r.begin(), r.end());
		}
		at += static_cast<double>(n) / (0.5 * kRate);
	}
	*beats = at;
}

/// One scenario, run end to end. `defer` picks the path; `audit` counts the hazard.
Run run(const Settings& cfg, bool defer, int chunk, int playFrames) {
	Sklitter s;
	s.prepare(kRate, 4096);
	s.setTransportClock(at120bpm());
	s.setDeferWritesForTest(defer);
	s.setReadAuditForTest(true);
	s.setTarget(cfg);

	double beats = 0.0;
	// **CAPTURE FIRST, WITH THE AUDIT OFF IN EFFECT** — nothing is playing, so nothing reads. The
	// bar plus a beat is what `test_fx_sklitter.cpp` uses to reach Playing, and the reason is there.
	drive(s, static_cast<int>(kBarFrames) + 9600, 0.0f, &beats, chunk);
	REQUIRE(s.statusForTest() == Status::Playing);
	s.resetReadAuditForTest();

	Run out;
	// A SECOND BAR OF VERY DIFFERENT AUDIO while playing, so a dry slice has something to write that
	// is distinguishable from what the capture left.
	drive(s, playFrames, 500000.0f, &beats, chunk, &out.outL, &out.outR);

	const size_t frames = s.ringForTest().length();
	out.ring.assign(frames * 2u, 0.0f);
	for (size_t f = 0; f < frames; ++f) {
		s.ringForTest().readAt(f, &out.ring[f * 2u], &out.ring[f * 2u + 1u]);
	}
	out.hits = s.auditReadHitsForTest();
	out.reads = s.auditReadsForTest();
	out.writes = s.auditWritesForTest();
	return out;
}

/// How many ring FRAMES differ, and by how much at the worst one. **A COUNT AND A MAGNITUDE, not a
/// bool**: "they differ" is the answer a difference check gives, and it cannot distinguish one
/// rounding bit from half a bar of the wrong material.
struct Diff {
	size_t frames = 0;
	float worst = 0.0f;
	size_t firstFrame = 0;
	/// **THE OUTPUT IS COUNTED SEPARATELY, because the ring being identical does not by itself say the
	/// block sounded the same.** A read that hit a freshly written position would move both — but the
	/// two are different claims, and inferring the second from the first is the move this repo's
	/// measurement rules exist to stop.
	size_t outFrames = 0;
};

Diff compare(const Run& a, const Run& b) {
	Diff d;
	REQUIRE(a.ring.size() == b.ring.size());
	for (size_t f = 0; f * 2u + 1u < a.ring.size(); ++f) {
		const float dl = std::abs(a.ring[f * 2u] - b.ring[f * 2u]);
		const float dr = std::abs(a.ring[f * 2u + 1u] - b.ring[f * 2u + 1u]);
		const float m = std::max(dl, dr);
		if (m != 0.0f) {
			if (d.frames == 0) {
				d.firstFrame = f;
			}
			++d.frames;
			d.worst = std::max(d.worst, m);
		}
	}
	REQUIRE(a.outL.size() == b.outL.size());
	for (size_t i = 0; i < a.outL.size(); ++i) {
		if (a.outL[i] != b.outL[i] || a.outR[i] != b.outR[i]) {
			++d.outFrames;
		}
	}
	return d;
}

} // namespace

//--------------------------------------------------------------------------------------------------//

TEST_CASE("the audit can see the hazard it exists to count, which is what makes a zero mean anything") {
	// **THE VACUITY CHECK, FIRST AND ON ITS OWN.** Every claim below is of the form "the deferral
	// changed nothing", and that claim is worthless from an instrument that cannot register a
	// difference. So: a scenario in which reads DO land on freshly written positions must produce a
	// non-zero hit count, or the zeroes elsewhere are silence rather than evidence.
	//
	// GRAIN MODE WAS THAT SCENARIO, and finding that out was the headline result of this file — see the
	// case below for why it no longer is (backlog JC, 2026-09-24: 24,570 hits became 1). **THE SCENARIO
	// NOW IS A PITCHED READ UNDER DRY WRITE**, which is where JC found the hazard for real: Pitch mode at
	// Zone B's pitch-ups, reading at 2x and faster behind the continuous writer's head.
	Settings cfg = looper(Mode::Pitch, 0.2f);
	cfg.zoneA = 0.1f;
	cfg.zoneB = 0.6f;
	cfg.dryWrite = 1.0f;
	const Run r = run(cfg, /*defer=*/false, /*chunk=*/960, /*playFrames=*/96000 * 3);
	CAPTURE(r.reads);
	CAPTURE(r.writes);
	CAPTURE(r.hits);
	MESSAGE("Pitch-under-Dry-Write audit: reads=" << r.reads << " writes=" << r.writes << " hits=" << r.hits);
	REQUIRE(r.reads > 0u);   // it looked
	REQUIRE(r.writes > 0u);  // there was something to find
	CHECK(r.hits > 0u);      // and it found it
}

TEST_CASE("GRAIN AT HN'S SETTINGS: its writes are not ducked and its reads are free — and the hazard was one voice a frame behind") {
	// **THE REFUTATION, AND IT IS THE POINT OF THIS FILE.** The deferral was justified by the overlap
	// duck. The duck lives in `advanceSlice` and sets `pWriteThisSlice_`; Grain's write
	// (`playGrain`'s `target_.pWrite > 0.0f && grainWritesA_`) never consults it, and Grain's reads
	// are at `grainPosA_ + offsetA` and `grainPosB_ + offsetB`, positions re-seeded per grain
	// anywhere in the buffer. **So in Grain there is no guard at all** — a voice can read a frame the
	// write head passed over earlier in the same chunk, and deferring the write changes what it hears.
	//
	//----------------------------------------------------------------------------------------------//
	// **WHAT THIS CASE CLAIMS CHANGED ON 2026-09-21 (backlog HU), THOUGH NOT ONE NUMBER IN IT DID.** //
	//----------------------------------------------------------------------------------------------//
	//
	// The measurement below is unaltered and still exact: deferring `P Write` in Grain moves 55,843
	// of 96,000 ring frames. **What it was taken to PROVE was wrong.** I read it as "a feedback
	// circuit is impossible in Grain" and wrote in `kWriterModes` that feedback there would be
	// *"unbounded by construction"*. Neither follows. Every writer is a convex blend and the feedback
	// source is damped and clamped, so the ring is bounded in Grain; and measured over six bar-long
	// passes the Grain loop DECAYS THIRTEEN TIMES FURTHER than Shuffle, which has the duck.
	//
	// **AND THE SHARPEST VERSION OF THAT IS RUN AT THIS CASE'S OWN SETTINGS.** In
	// `test_fx_sklitter_grainfb.cpp`, this exact arrangement — chunk 64, Zone A 0.8, rate 0.4,
	// `P Write` 1.0, the one that produces the 24,570 hazard reads below — decays to **0.000081**
	// over six passes, faster than any other arrangement measured. **The hazard and the stability are
	// not merely uncorrelated here, they point opposite ways**: scattered reads decorrelate the loop,
	// which is the same property that makes the hazard count high.
	//
	// **SO WHAT THIS CASE NOW GUARDS IS A DESIGN DECISION RATHER THAN A PROHIBITION.** Because
	// deferring `P Write` in Grain really does change 55,843 frames, `P Write` in Grain is left
	// IMMEDIATE — it is the sound that has been in use — and only the two continuous writers are
	// deferred there. The cost is that Grain's blend order is `P Write`, DRY, FEEDBACK rather than
	// the designed DRY, FEEDBACK, `P Write`. **If this case ever goes green with a small number, that
	// trade is no longer necessary and the order can be made uniform.**
	Settings cfg = looper(Mode::Grain, 0.4f);
	cfg.pWrite = 1.0f;
	cfg.zoneA = 0.8f;
	cfg.density = 1.0f;

	const Run immediate = run(cfg, /*defer=*/false, /*chunk=*/64, /*playFrames=*/48000);
	const Run deferred = run(cfg, /*defer=*/true, /*chunk=*/64, /*playFrames=*/48000);
	const Diff d = compare(immediate, deferred);
	CAPTURE(immediate.hits);
	CAPTURE(immediate.reads);
	CAPTURE(d.frames);
	CAPTURE(d.worst);
	CAPTURE(d.firstFrame);
	MESSAGE("Grain: hits=" << immediate.hits << "/" << immediate.reads << " differing ring frames="
	                       << d.frames << " worst=" << d.worst << " first=" << d.firstFrame);
	//----------------------------------------------------------------------------------------------//
	// **AND THEN, 2026-09-24 (backlog JC), IT WENT GREEN WITH A SMALL NUMBER — hits 1, frames 0 — AND  //
	// THE REASON IS NOT THAT GRAIN GREW A GUARD.**                                                     //
	//----------------------------------------------------------------------------------------------//
	//
	// Instrumented at the old arithmetic, every one of the 24,570 hits was VOICE A'S FIRST GRAIN reading
	// ONE FRAME BEHIND THE HEAD: armed at position 0 with the head at 0, its offset was
	// `floor(phase · grainLength)`, and a phase accumulated in doubles floors to `k − 1` at sample `k` — so
	// for the whole of a 34,559-frame grain it read the frame the head had written a sample earlier, in the
	// same chunk. The port now counts the offset as the firmware does (an integer, stepped by 1 or 2), the
	// first grain reads AT the head before its write, and the coincidence is gone. **So the hazard this
	// case measured was a rounding artefact meeting an alignment, not scattered reads** — scattered reads do
	// land behind the head, but at 64-frame chunks they do so about once in these 96,000.
	//
	// **WHAT THAT MEANS FOR THE ORDER IS A SOUND DECISION.** The trade above — `P Write` left immediate in Grain, so
	// Grain's order is `P Write`, DRY, FEEDBACK — was bought with the 55,843 frames this case no longer
	// finds. The number is recorded here and the order is left as it is; making it uniform is a sound
	// change and a design decision, not a test edit.
	CHECK(immediate.hits < 100u);
	CHECK(d.frames < 1000u);
}

TEST_CASE("the looper modes: is the deferral exact, and did the material let it matter") {
	// **ONE CASE PER MODE, AND BOTH NUMBERS REPORTED.** `hits` is the audit; `frames` is the
	// difference. The pair is the finding: zero differences with zero hits is vacuous, zero
	// differences with non-zero hits is a real (if surprising) exactness, and non-zero differences
	// refute the deferral for that mode.
	//
	// **WHAT THIS NEVER EXERCISED, FOUND 2026-09-24 (backlog JC): A PITCHED READ TRAILING THE HEAD.** The
	// zero hits here are at the puck's rest with pWrite as the only writer, where every read runs at 1x and
	// so never lands behind the head within a chunk. A read at 2x or above does, routinely — and with the
	// continuous DRY writer deferred to the flush, such a read heard the frame's bar-old contents and then
	// the fresh ones, flipping at every block boundary: 77/117/66/128 clicks in Time/Shuffle/Pattern/Pitch
	// at Dry Write 1. The dry blend is now written in the sample (`writeDryNow`); this case still measures
	// the pWrite deferral it was written for, and says nothing about pitched reads.
	struct Case {
		Mode mode;
		const char* name;
	};
	for (const Case c : { Case { Mode::Shuffle, "Shuffle" }, Case { Mode::Time, "Time" },
	                      Case { Mode::Pattern, "Pattern" }, Case { Mode::Pitch, "Pitch" } }) {
		CAPTURE(c.name);
		Settings cfg = looper(c.mode, 0.25f);
		cfg.zoneA = 0.6f;
		cfg.zoneB = 0.8f;
		cfg.pWrite = 1.0f;

		const Run immediate = run(cfg, /*defer=*/false, /*chunk=*/64, /*playFrames=*/96000);
		const Run deferred = run(cfg, /*defer=*/true, /*chunk=*/64, /*playFrames=*/96000);
		const Diff d = compare(immediate, deferred);
		CAPTURE(immediate.reads);
		CAPTURE(immediate.writes);
		CAPTURE(immediate.hits);
		CAPTURE(d.frames);
		CAPTURE(d.worst);
		CAPTURE(d.firstFrame);
		// **THE DUCK'S PROMISE, STATED AS A NUMBER RATHER THAN QUOTED.** If it holds across a whole
		// chunk, this is zero.
		MESSAGE("mode " << std::string(c.name) << ": hits=" << immediate.hits << "/" << immediate.reads
		                << " writes=" << immediate.writes << " differing ring frames=" << d.frames
		                << " worst=" << d.worst);
		CHECK(immediate.hits == 0u);
		CHECK(d.frames == 0u);
		CHECK(d.outFrames == 0u);
	}
}

TEST_CASE("the DRY-slice exemption is where the duck stops looking, so it is where this looks hardest") {
	// **THE FIRST PLACE TO LOOK FOR A COUNTEREXAMPLE.** The duck is skipped
	// entirely when `sliceIsDry_` — *"Fresh input has no feedback concern — we're not reading what we
	// just wrote"* — so a dry slice writes an UNDUCKED region, and a LATER slice in the same chunk is
	// under no obligation to read elsewhere. Density at zero makes every slice dry.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.pWrite = 1.0f;
	cfg.density = 0.0f;

	const Run immediate = run(cfg, /*defer=*/false, /*chunk=*/64, /*playFrames=*/96000);
	const Run deferred = run(cfg, /*defer=*/true, /*chunk=*/64, /*playFrames=*/96000);
	const Diff d = compare(immediate, deferred);
	CAPTURE(immediate.reads);
	CAPTURE(immediate.writes);
	CAPTURE(immediate.hits);
	CAPTURE(d.frames);
	CAPTURE(d.worst);
	MESSAGE("all-dry Shuffle: hits=" << immediate.hits << "/" << immediate.reads << " writes="
	                                 << immediate.writes << " differing ring frames=" << d.frames);
	CHECK(immediate.hits == 0u);
	CHECK(d.frames == 0u);
		CHECK(d.outFrames == 0u);

	// **AND THE ZERO ABOVE IS ZERO FOR A REASON THAT DISQUALIFIES IT AS EVIDENCE, WHICH IS WHY IT IS
	// ASSERTED RATHER THAN CELEBRATED.** `reads` is ZERO here: a dry slice takes its source from the
	// live input, so `readOne` never runs and the looper reads nothing from the ring at all. An
	// all-dry arrangement therefore cannot be the counterexample — not because the duck protects it
	// but because there is no read to protect. **The hazard needs dry and wet slices in the SAME
	// chunk**, which is what the mixed case below arranges.
	CHECK(immediate.reads == 0u);
}

TEST_CASE("MIXED density is the real shape of the dry exemption: unducked writes beside live reads") {
	// **THE COUNTEREXAMPLE CANDIDATE, PROPERLY POSED.** At density 0.5 some slices are dry (write
	// unducked, read nothing) and some are wet (read the ring). A chunk containing the boundary has an
	// unducked write region from the dry slice and a live read from the wet one, and nothing in
	// `advanceSlice` has compared them — the duck only ever compared a slice's write with its OWN read.
	// **THE DRAW IS PER SLICE INDEX AND NOTHING ELSE** (`hash::mix(slice ^ 0xBADC0FFE)`, dry when
	// `draw >= density`), so a bar with few slices has few draws and a density can round to all-dry or
	// all-wet by accident. Several rates and densities are swept, and the case REQUIRES that at least
	// one of them was genuinely mixed — otherwise it would be the vacuous zero again, in a new coat.
	size_t mixedCombinations = 0;
	for (const float rate : { 0.25f, 0.5f, 0.75f }) {
		for (const float density : { 0.4f, 0.6f, 0.8f, 0.95f }) {
			CAPTURE(rate);
			CAPTURE(density);
			Settings cfg = looper(Mode::Shuffle, rate);
			cfg.zoneA = 0.6f;
			cfg.zoneB = 0.8f;
			cfg.pWrite = 1.0f;
			cfg.density = density;

			const Run immediate = run(cfg, /*defer=*/false, /*chunk=*/64, /*playFrames=*/96000);
			const Run deferred = run(cfg, /*defer=*/true, /*chunk=*/64, /*playFrames=*/96000);
			const Diff d = compare(immediate, deferred);
			CAPTURE(immediate.reads);
			CAPTURE(immediate.writes);
			CAPTURE(immediate.hits);
			CAPTURE(d.frames);
			CAPTURE(d.worst);
			MESSAGE("mixed rate " << rate << " density " << density << ": hits=" << immediate.hits
			                      << "/" << immediate.reads << " writes=" << immediate.writes
			                      << " differing ring frames=" << d.frames << " worst=" << d.worst);
			// A combination counts as MIXED only when the ring was both read and written: a dry slice
			// reads nothing, so reads > 0 is the proof that wet slices were in play beside the
			// unducked dry writes.
			if (immediate.reads > 0u && immediate.writes > 0u) {
				++mixedCombinations;
			}
			CHECK(immediate.hits == 0u);
			CHECK(d.frames == 0u);
		CHECK(d.outFrames == 0u);
		}
	}
	CAPTURE(mixedCombinations);
	CHECK(mixedCombinations > 0u);
}

TEST_CASE("THE CHUNK LENGTH IS THE HAZARD WINDOW, and 64 is not the only length this device sees") {
	// **THE CHUNK IS THE SCRATCH — THE HOST'S LONGEST BLOCK (backlog GO), 4096 AS THIS FILE PREPARES IT, NOT 64.** In
	// the rack `RackEngine::processStereo` cuts every device's block at `device::kTickWindow`, so the chunk is 64
	// there — but `Sklitter::processStereo` cuts at its scratch and nothing in the device requires a rack. A
	// deferral's exposure is proportional to the chunk: the longer the chunk, the more of the buffer the write head
	// covers before a read can be affected by it. So the same scenarios are run at the ceiling.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.pWrite = 1.0f;

	const Run immediate = run(cfg, /*defer=*/false, /*chunk=*/4096, /*playFrames=*/96000);
	const Run deferred = run(cfg, /*defer=*/true, /*chunk=*/4096, /*playFrames=*/96000);
	const Diff d = compare(immediate, deferred);
	CAPTURE(immediate.reads);
	CAPTURE(immediate.writes);
	CAPTURE(immediate.hits);
	CAPTURE(d.frames);
	CAPTURE(d.worst);
	CAPTURE(d.firstFrame);
	MESSAGE("Shuffle @4096: hits=" << immediate.hits << "/" << immediate.reads << " writes="
	                               << immediate.writes << " differing ring frames=" << d.frames);
	CHECK(immediate.hits == 0u);
	CHECK(d.frames == 0u);
		CHECK(d.outFrames == 0u);
}

TEST_CASE("what the deferral COSTS, measured A/B/A/B on this host rather than estimated") {
	// **ALTERNATED, NOT BATCHED.** This host runs several agents and its load swings by an order of
	// magnitude; two consecutive batches would measure the machine's afternoon rather than the change.
	// The audit is OFF here — it is the instrument, not the feature, and its per-read modulo would
	// dominate a measurement of four stores.
	//
	// **AND IT REPORTS RATHER THAN ASSERTS.** A timing threshold on a shared host is a flake with a
	// number on it; the finding is the ratio, for a human to read.
	Settings cfg = looper(Mode::Shuffle, 0.25f);
	cfg.zoneA = 0.6f;
	cfg.zoneB = 0.8f;
	cfg.pWrite = 1.0f;

	constexpr int kChunk = 64;
	constexpr int kPlay = 480000; // ten seconds of audio per pass
	constexpr int kPasses = 4;

	double offNs = 0.0;
	double onNs = 0.0;
	for (int pass = 0; pass < kPasses; ++pass) {
		for (int which = 0; which < 2; ++which) {
			Sklitter s;
			s.prepare(kRate, 4096);
			s.setTransportClock(at120bpm());
			s.setDeferWritesForTest(which == 1);
			s.setTarget(cfg);
			double beats = 0.0;
			drive(s, static_cast<int>(kBarFrames) + 9600, 0.0f, &beats, kChunk);
			REQUIRE(s.statusForTest() == Status::Playing);
			const auto t0 = std::chrono::steady_clock::now();
			drive(s, kPlay, 500000.0f, &beats, kChunk);
			const auto t1 = std::chrono::steady_clock::now();
			const auto ns = static_cast<double>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
			(which == 0 ? offNs : onNs) += ns;
		}
	}
	const double offPerFrame = offNs / (kPasses * static_cast<double>(kPlay));
	const double onPerFrame = onNs / (kPasses * static_cast<double>(kPlay));
	MESSAGE("cost: immediate " << offPerFrame << " ns/frame, deferred " << onPerFrame
	                           << " ns/frame, ratio " << (onPerFrame / offPerFrame));
	CHECK(offPerFrame > 0.0);
}
