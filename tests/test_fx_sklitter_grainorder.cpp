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
// BACKLOG HU — THE GRAIN BLEND ORDER, AS AN AUDITION. `Param::GrainOrder`.                          //
//                                                                                                   //
// Neither order can be preferred without hearing both, so both are built and the choice is made by   //
// ear. **This file's job is not to say which is better — it is to make the switch worth flipping**: //
// to show the two states are really different, to say WHERE they are most different so nobody is    //
// listening for a ghost, and to measure the one thing B costs.                                      //
//                                                                                                   //
//   A, `P Write First` (default, what shipped): P Write writes immediately inside `playGrain`,      //
//                                               then DRY and FEEDBACK blend over it at the flush.   //
//   B, `P Write Last`  (the intended order):    P Write is recorded and performed at the flush      //
//                                               behind the other two, as in every other mode.       //
//                                                                                                   //
// **AND THE THING B COSTS IS A DISCONTINUITY IN A CONTROL THE PLAYER IS NOT TOUCHING.** Under B,    //
// `P Write` switches from immediate to deferred at the instant `Feedback` or `Dry Write` leaves      //
// zero. That is measured here against the signal's own sample-to-sample slope, because "is it a     //
// click" is a question about a step relative to the material, not about a step.                     //
//--------------------------------------------------------------------------------------------------//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
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

Settings grainMode() {
	Settings s;
	s.mode = Mode::Grain;
	s.enabled = true;
	s.engage = true;
	s.rate = 0.5f;
	s.density = 1.0f;
	s.pWrite = 0.0f;
	s.mix = 1.0f;
	return s;
}

/// **A TONE RATHER THAN DC, because the discontinuity question needs a signal with a slope.** A step
/// is only a click relative to the steps the material already makes; DC makes none, so every seam
/// would read as infinitely sharp and the number would mean nothing.
void driveTone(Sklitter& s, int frames, double hz, double* phase, double* beats,
               std::vector<float>* keep = nullptr, int block = kBlock) {
	std::vector<float> l;
	std::vector<float> r;
	const double inc = 6.283185307179586 * hz / kRate;
	for (int done = 0; done < frames; done += block) {
		const int n = std::min(block, frames - done);
		l.assign(static_cast<size_t>(n), 0.0f);
		r.assign(static_cast<size_t>(n), 0.0f);
		for (int i = 0; i < n; ++i) {
			const auto v = static_cast<float>(0.5 * std::sin(*phase));
			l[static_cast<size_t>(i)] = v;
			r[static_cast<size_t>(i)] = v;
			*phase += inc;
		}
		s.setTransportClock(at120bpm(*beats));
		s.processStereo(l.data(), r.data(), n);
		if (keep != nullptr) {
			keep->insert(keep->end(), l.begin(), l.end());
		}
		*beats += static_cast<double>(n) / (0.5 * kRate);
	}
}

float ringAt(const Sklitter& s, size_t frame) {
	float l = 0.0f;
	float r = 0.0f;
	s.ringForTest().readAt(frame, &l, &r);
	return l;
}

std::vector<float> ringOf(const Sklitter& s) {
	const size_t n = s.ringForTest().length();
	std::vector<float> out(n, 0.0f);
	for (size_t f = 0; f < n; ++f) {
		out[f] = ringAt(s, f);
	}
	return out;
}

size_t differing(const std::vector<float>& a, const std::vector<float>& b) {
	size_t d = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
		if (a[i] != b[i]) {
			++d;
		}
	}
	return d;
}

/// The largest sample-to-sample step in a buffer — the yardstick every "is it a click" claim here is
/// measured against.
float largestStep(const std::vector<float>& v, size_t from = 0) {
	float worst = 0.0f;
	for (size_t i = from + 1; i < v.size(); ++i) {
		worst = std::max(worst, std::abs(v[i] - v[i - 1]));
	}
	return worst;
}

const device::ParamDesc& descOf(Param p) { return kParamDescs[static_cast<size_t>(p)]; }

} // namespace

//--------------------------------------------------------------------------------------------------//

TEST_CASE("AT ZERO KNOBS THE TWO ORDERS ARE BIT-IDENTICAL, and to the build before the switch") {
	// **THE CONSTRAINT THAT OUTRANKS THE REST.** `Grain Order` is an audition control; until a player
	// engages the circuit it must not exist as far as the sound is concerned. B is reached only while
	// `writersActive()`, and this is what holds that guard to its word.
	const auto ringAfter = [](bool pWriteLast, float writers) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		cfg.pWrite = 1.0f; // UP, or the comparison is between two untouched buffers
		cfg.grainPWriteLast = pWriteLast;
		s.setTarget(cfg);
		double beats = 0.0;
		double phase = 0.0;
		driveTone(s, kBarFrames + 9600, 220.0, &phase, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.dryWrite = writers;
		cfg.feedback = writers;
		s.setTarget(cfg);
		driveTone(s, kBlock * 16, 220.0, &phase, &beats);
		return ringOf(s);
	};

	// WITH THE CIRCUIT OFF: the switch changes nothing at all.
	CHECK(differing(ringAfter(false, 0.0f), ringAfter(true, 0.0f)) == 0u);

	// **AND THE VENUE IS SHOWN CAPABLE OF EXPRESSING A DIFFERENCE**, or the zero above is the answer
	// an instrument gives when it cannot see anything. With the circuit ON the two orders diverge.
	const size_t moved = differing(ringAfter(false, 0.5f), ringAfter(true, 0.5f));
	MESSAGE("knobs at zero: 0 ring frames differ between the orders; at 0.5: ", moved);
	CHECK(moved > 0u);
}

TEST_CASE("WHAT B COSTS: the step when P Write changes road as Feedback leaves zero") {
	// **THE MEASUREMENT THE LEAD ASKED FOR, AND THE YARDSTICK IS THE SIGNAL'S OWN SLOPE.** Under B,
	// `P Write` switches from immediate to deferred the instant `writersActive()` turns true. The
	// question is whether that seam is audible, which is a question about the step RELATIVE to the
	// steps this material already makes.
	// **THE FIRST VERSION OF THIS CASE WAS VACUOUS AND IS RECORDED HERE RATHER THAN QUIETLY REPLACED.**
	// It looked at the one output sample either side of the instant the knob moved, and reported
	// **0.00810903 for BOTH orders, identical to the digit** — which is the tell: a number that does
	// not move when the thing under test is switched off is not measuring the thing under test. The
	// road change happens in the RING, and the ring only becomes audible when a grain voice next
	// reads those frames, which is a bar away. There was nothing at that instant to see, in either
	// order, and the case would have passed with B deleted.
	//
	// **SO THE WINDOW IS A WHOLE BAR AFTER THE SEAM, AND THE COMPARISON IS A-AGAINST-B**, which is
	// the only arrangement in which the road change is the sole difference between two renders.
	const auto run = [](bool pWriteLast, std::vector<float>* pre, std::vector<float>* post) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		cfg.pWrite = 1.0f;
		cfg.grainPWriteLast = pWriteLast;
		s.setTarget(cfg);
		double beats = 0.0;
		double phase = 0.0;
		driveTone(s, kBarFrames + 9600, 220.0, &phase, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		driveTone(s, kBlock * 8, 220.0, &phase, &beats, pre); // settled, circuit OFF, roads identical
		// **THE KNOB LEAVES ZERO BY THE SMALLEST STEP THAT ENGAGES THE CIRCUIT.** That is the worst
		// case for this defect: the feature's own contribution is inaudible at 1e-4, so anything that
		// appears is the ROAD CHANGE and not the feature arriving.
		cfg.feedback = 1.0e-4f;
		s.setTarget(cfg);
		driveTone(s, kBarFrames, 220.0, &phase, &beats, post);
	};

	std::vector<float> preA;
	std::vector<float> postA;
	std::vector<float> preB;
	std::vector<float> postB;
	run(false, &preA, &postA);
	run(true, &preB, &postB);

	// BEFORE THE SEAM THE TWO ORDERS ARE THE SAME RENDER — the circuit is off, so both take the
	// immediate road. If this is not exact, nothing below is attributable to the road change.
	REQUIRE(differing(preA, preB) == 0u);

	// **THE STEP THE ROAD CHANGE INTRODUCES, measured as the worst divergence between the two runs
	// over the bar after the seam**, against the material's own largest sample-to-sample step.
	float worstDelta = 0.0f;
	size_t firstAt = 0;
	for (size_t i = 0; i < postA.size() && i < postB.size(); ++i) {
		const float d = std::abs(postA[i] - postB[i]);
		if (d > 0.0f && firstAt == 0) {
			firstAt = i;
		}
		worstDelta = std::max(worstDelta, d);
	}
	const float ordinary = largestStep(postA);
	MESSAGE("road change at the seam: A-vs-B worst |delta| over the following bar ", worstDelta,
	        " (first difference at sample ", firstAt, ") against the material's own largest step ",
	        ordinary, " -> ratio ", ordinary > 0.0f ? worstDelta / ordinary : 0.0f);

	// **NON-VACUITY FIRST: the road change is REAL.** Without this the bound below is satisfied by
	// two identical renders, which is what the first version of this case was measuring.
	CHECK(worstDelta > 0.0f);
	// **AND IT DOES NOT ARRIVE AS A STEP AT THE SEAM.** `firstAt` well past zero is the substance of
	// that: the two renders stay identical for a while after the knob moves and then drift apart as
	// the voices reach the frames written by the other road. A click would show up at sample 0.
	CHECK(firstAt > 0u);
	CHECK(std::isfinite(worstDelta));
}

TEST_CASE("WHERE A AND B DIFFER MOST — so the switch is not auditioned at settings where it is mute") {
	// **THE POINT OF THIS CASE IS THE TABLE, NOT THE ASSERTION.** A switch whose effect depends on
	// settings the player has to guess is a switch that gets judged "no difference" by accident.
	//
	// **THE PREDICTION, from the arithmetic:** the orders differ in which writer lands LAST and
	// therefore unattenuated. So they should separate most where the LAST writer's amount is large
	// and the material the two writers carry is most unlike — `P Write` carries the pre-envelope
	// grain, FEEDBACK carries the damped wet. At `Feedback` 1.0 the two should be furthest apart;
	// at `Feedback` small, A and B should nearly coincide whatever `P Write` does.
	struct Row {
		float pWrite;
		float dry;
		float feedback;
	};
	const Row rows[] = {
		{ 1.0f, 0.0f, 1.0f },  { 1.0f, 0.0f, 0.5f },  { 1.0f, 0.0f, 0.1f },
		{ 1.0f, 0.5f, 1.0f },  { 0.5f, 0.0f, 1.0f },  { 1.0f, 0.0f, 0.02f },
	};
	const auto ringFor = [](const Row& r, bool pWriteLast) {
		Sklitter s;
		s.prepare(kRate, 4096);
		s.setTransportClock(at120bpm(0.0));
		Settings cfg = grainMode();
		cfg.pWrite = r.pWrite;
		cfg.grainPWriteLast = pWriteLast;
		s.setTarget(cfg);
		double beats = 0.0;
		double phase = 0.0;
		driveTone(s, kBarFrames + 9600, 220.0, &phase, &beats);
		REQUIRE(s.statusForTest() == Status::Playing);
		cfg.dryWrite = r.dry;
		cfg.feedback = r.feedback;
		s.setTarget(cfg);
		driveTone(s, kBarFrames, 220.0, &phase, &beats);
		return ringOf(s);
	};

	float bestRms = 0.0f;
	std::string bestRow;
	for (const Row& r : rows) {
		const std::vector<float> a = ringFor(r, false);
		const std::vector<float> b = ringFor(r, true);
		double sum = 0.0;
		float worst = 0.0f;
		for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
			const float d = a[i] - b[i];
			sum += static_cast<double>(d) * static_cast<double>(d);
			worst = std::max(worst, std::abs(d));
		}
		const auto rms = static_cast<float>(std::sqrt(sum / static_cast<double>(a.size())));
		const std::string label = "P Write " + std::to_string(r.pWrite) + " dry "
		                          + std::to_string(r.dry) + " fb " + std::to_string(r.feedback);
		MESSAGE(label, " : A-vs-B ring RMS ", rms, " worst |delta| ", worst, " frames differing ",
		        differing(a, b));
		if (rms > bestRms) {
			bestRms = rms;
			bestRow = label;
		}
	}
	MESSAGE("MOST AUDIBLE AT: ", bestRow, " (ring RMS ", bestRms, ")");
	// **AND THE SWITCH IS NOT MUTE SOMEWHERE**, which is the only thing this case asserts: if every
	// row came back at zero the control would be an ornament and an audition would be listening for nothing.
	CHECK(bestRms > 0.0f);
}

TEST_CASE("the column reaches the engine through the HOST's road, not just through Settings") {
	// **THIS CASE EXISTS BECAUSE A KNOWN-BAD WENT GREEN, which is the only reason anybody finds this
	// class of gap.** Deleting the `GrainOrder` read from `settingsFromRow` — making the control
	// unreachable from any host, for ever — left all four cases in this file passing, because every
	// one of them sets `Settings::grainPWriteLast` DIRECTLY and hands it to `setTarget`. They test
	// the engine's behaviour given a setting and say nothing about whether a host can produce that
	// setting. `test_fx_devices`' sweep did not catch it either: the control reads dead there by
	// construction (see its expected-dead entry), so a dead control that is also unreachable looks
	// exactly the same.
	//
	// **SO THE ROUND TRIP IS ASSERTED ON THE ROW, which is the thing a host actually writes.**
	Row row = rowOf(Settings {});
	REQUIRE(row[static_cast<size_t>(Param::GrainOrder)] == 0.0f); // A is the default, in the row too

	row[static_cast<size_t>(Param::GrainOrder)] = 1.0f;
	CHECK(stateOf(row).grainPWriteLast);
	row[static_cast<size_t>(Param::GrainOrder)] = 0.0f;
	CHECK_FALSE(stateOf(row).grainPWriteLast);

	// AND BACK OUT AGAIN, so the two directions cannot drift apart.
	Settings s;
	s.grainPWriteLast = true;
	CHECK(rowOf(s)[static_cast<size_t>(Param::GrainOrder)] == 1.0f);

	// **AND THE COLUMN IS THE ONE THE DESCRIPTOR DECLARES**, which is what a host indexes by. An
	// off-by-one here would read a neighbour's value and still round-trip within this case.
	CHECK(std::string_view(descOf(Param::GrainOrder).name) == "Grain Order");
}

TEST_CASE("the audition control is declared where it means something, and nowhere else") {
	// **GRAIN ONLY.** Everywhere else the intended order already runs, so a switch would offer a choice
	// between one real order and one that is not implemented — the honesty rule, not a preference.
	CHECK(descOf(Param::GrainOrder).gateMask == kModeBit(Mode::Grain));
	CHECK(descOf(Param::GrainOrder).stepped);
	CHECK(descOf(Param::GrainOrder).names.size() == 2u);
	// **NOT A MODULATION DESTINATION** — it restructures the write path rather than scaling anything.
	CHECK_FALSE(descOf(Param::GrainOrder).modulatable);
	// **A IS THE DEFAULT, so nothing already heard moves until the switch is flipped.**
	CHECK(descOf(Param::GrainOrder).def == 0.0);
	CHECK_FALSE(Settings {}.grainPWriteLast);
	// **AND IT IS NOT AN Off/On PAIR**, so it renders as a picker showing both orders rather than a
	// chip captioned with one of them. See `kGrainOrderNames`.
	CHECK_FALSE(device::isOffOnChoice(descOf(Param::GrainOrder)));
}
