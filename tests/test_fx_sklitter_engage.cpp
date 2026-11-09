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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/engage.h"

using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

/// A rolling transport at a known beat. The clock is a plain value, so a case can say exactly where
/// the playhead is instead of arranging for one.
TransportClock rolling(double beats) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.songPositionBeats = beats;
	c.playing = true;
	return c;
}

/// A host with a tempo but NO position timeline. The distinction matters and is easy to get wrong:
/// a stopped host still reports a tempo, so tempo is not the thing to fall back on.
TransportClock noPosition() {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.playing = true;
	return c;
}

Settings withMode(Mode m, bool /*latch*/ = false) {
	Settings s;
	s.mode = m;
	return s;
}

} // namespace

TEST_CASE("a gesture in Burst records, then plays once it has enough audio") {
	EngageMachine e;
	CHECK(e.status() == Status::Off);

	e.setEngaged(true, withMode(Mode::Burst));
	CHECK(e.status() == Status::Recording); // Burst does not use the looper road

	e.advance(rolling(0.0), 100);
	CHECK(e.status() == Status::Recording); // nothing captured yet

	e.captured(99);
	e.advance(rolling(0.0), 100);
	CHECK(e.status() == Status::Recording); // one frame short is still short

	e.captured(1);
	e.advance(rolling(0.0), 100);
	CHECK(e.status() == Status::Playing);
}

TEST_CASE("a looper gesture waits for a BEAT BOUNDARY, not merely for enough audio") {
	// The pending trigger targets the NEXT beat index and will not fire before it.
	// Audio sufficiency alone is not the trigger -- that is the whole point of the pending road.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Shuffle));
	CHECK(e.status() == Status::Standby);

	e.captured(1000);
	e.advance(rolling(3.2), 100); // first advance sets the target to beat 4
	CHECK(e.status() == Status::Standby);

	e.advance(rolling(3.9), 100); // still inside beat 3
	CHECK(e.status() == Status::Standby);

	e.advance(rolling(4.0), 100); // crossed into beat 4
	CHECK(e.status() == Status::Playing);
}

TEST_CASE("a beat boundary with too little audio DEFERS to the next one") {
	// Too little recorded audio at a boundary moves the trigger to the next beat. Starting anyway would play a region that was never written -- silence dressed as
	// audio.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Grain));
	e.captured(10); // not enough

	e.advance(rolling(3.2), 100); // target becomes beat 4
	e.advance(rolling(4.0), 100); // boundary reached, audio short
	CHECK(e.status() == Status::Standby);

	e.advance(rolling(4.5), 100); // still short, still waiting
	CHECK(e.status() == Status::Standby);

	e.captured(90); // now at 100
	e.advance(rolling(4.7), 100);
	CHECK(e.status() == Status::Standby); // must wait for the DEFERRED boundary, beat 5

	e.advance(rolling(5.0), 100);
	CHECK(e.status() == Status::Playing);
}

TEST_CASE("with no position timeline the trigger is immediate, and TEMPO is not the fallback") {
	// **A STOPPED HOST STILL REPORTS A TEMPO.** TransportClock's own header is emphatic that validity
	// is independent of `playing`. Quantised triggering needs a POSITION, so the fallback must be
	// havePositionBeats(), never haveTempo().
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Shuffle));
	e.captured(100);

	const TransportClock c = noPosition();
	REQUIRE(c.haveTempo() == true);        // there IS a tempo
	REQUIRE(c.havePositionBeats() == false); // and no position

	e.advance(c, 100);
	CHECK(e.status() == Status::Playing); // immediate, not stuck waiting for a beat
}

TEST_CASE("a stopped transport triggers immediately even though it reports a position") {
	// A stopped playhead sits at the same beat forever, so waiting for a boundary would wait for ever.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Shuffle));
	e.captured(100);

	TransportClock c = rolling(3.2);
	c.playing = false;
	e.advance(c, 100);
	CHECK(e.status() == Status::Playing);
}

TEST_CASE("release ends an unlatched gesture and is ignored by a latched one") {
	SUBCASE("Burst obeys the toggle") {
		EngageMachine e;
		e.setEngaged(true, withMode(Mode::Burst, /*latch=*/true));
		e.captured(100);
		e.advance(rolling(0.0), 100);
		REQUIRE(e.status() == Status::Playing);

		e.setEngaged(false, withMode(Mode::Burst, true));
		CHECK(e.status() == Status::Off); // A RELEASE ALWAYS ENDS IT — see engage.h
	}
	SUBCASE("a looper mode latches whatever the toggle says") {
		EngageMachine e;
		e.setEngaged(true, withMode(Mode::Pattern, /*latch=*/false));
		e.captured(100);
		e.advance(rolling(3.2), 100);
		e.advance(rolling(4.0), 100);
		REQUIRE(e.status() == Status::Playing);

		e.setEngaged(false, withMode(Mode::Pattern, false));
		CHECK(e.status() == Status::Off); // no mode latches any more
	}
}

TEST_CASE("the press snapshot still decides which ROAD the gesture takes") {
	// **WHAT `armedConfig` IS FOR NOW.** It used to decide the latch, and a whole case here asserted
	// that the decision was made at PRESS rather than at release. Latch is gone (see `engage.h`), but
	// the snapshot is not: it still says whether this gesture is filling a rate-sized buffer or a
	// bar, and turning the Mode knob mid-gesture must not switch roads underneath it.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Burst)); // not a looper: fills a rate-sized buffer
	REQUIRE(e.status() == Status::Recording);
	CHECK(e.armed().mode == Mode::Burst);

	// The user turns the knob to a looper mode while it is filling.
	e.captured(50);
	e.advance(rolling(0.5), 100);
	CHECK(e.status() == Status::Recording); // still on the road it started on
	CHECK(e.armed().mode == Mode::Burst);   // the snapshot, not the live value
}

TEST_CASE("a release before playback starts is remembered, not dropped") {
	// The gesture ended before the trigger fired, so it must
	// not start afterwards -- a stutter that begins after you let go is one you cannot control.
	//
	// **THIS CASE FIRST USED A LOOPER MODE AND WAS WRONG ABOUT THE DOMAIN.** Every looper mode always
	// latches, so its release is ignored by construction and the early-release path is unreachable
	// from there. The path only exists for a mode that does NOT latch and has not yet started, which
	// means Burst.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Burst, /*latch=*/false));
	REQUIRE(e.status() == Status::Recording);
	e.captured(10); // not enough to start yet

	e.setEngaged(false, withMode(Mode::Burst, false)); // released while still filling
	CHECK(e.status() == Status::Recording);            // not ended yet -- it never started

	e.captured(90);              // the audio it was waiting for arrives
	e.advance(rolling(0.0), 100);
	CHECK(e.status() == Status::Off); // and it does NOT play
}

TEST_CASE("a looper mode released during standby does NOT go on to play") {
	// **THIS CASE ASSERTED THE OPPOSITE UNTIL THE FIRST INSTALL.** It read "still goes on to play,
	// because it latches", which was faithful to the reference and wrong for a plugin: a player could
	// start the effect and had no way to stop it — switching the enable off did not work, and it kept
	// going. A release always ends the gesture now; see `engage.h` for why latch does not
	// survive the port.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Shuffle));
	e.captured(100);
	e.advance(rolling(3.2), 100);
	REQUIRE(e.status() == Status::Standby);

	e.setEngaged(false, withMode(Mode::Shuffle));
	e.advance(rolling(4.0), 100);
	CHECK(e.status() == Status::Off);
}

TEST_CASE("re-engaging after a release starts a fresh gesture with a fresh snapshot") {
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Burst, false));
	e.captured(100);
	e.advance(rolling(0.0), 100);
	e.setEngaged(false, withMode(Mode::Burst, false));
	REQUIRE(e.status() == Status::Off);

	e.setEngaged(true, withMode(Mode::Repeat, false));
	CHECK(e.status() == Status::Standby);
	CHECK(e.armed().mode == Mode::Repeat); // the new press, not the old one
	CHECK(e.capturedFrames() == 0u);      // and the count starts again
}

TEST_CASE("holding Engage across blocks is not a re-press") {
	// setEngaged is called every block with the same value. Only the EDGE starts a gesture; treating
	// the level as a press would reset the capture count every block and never reach the trigger.
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Shuffle));
	for (int i = 0; i < 5; ++i) {
		e.captured(30);
		e.setEngaged(true, withMode(Mode::Shuffle)); // still held
	}
	CHECK(e.capturedFrames() == 150u);
	e.advance(rolling(3.2), 100);
	e.advance(rolling(4.0), 100);
	CHECK(e.status() == Status::Playing);
}

TEST_CASE("reset returns the machine to Off from anywhere") {
	EngageMachine e;
	e.setEngaged(true, withMode(Mode::Grain));
	e.captured(500);
	e.advance(rolling(3.2), 100);
	e.advance(rolling(4.0), 100);
	REQUIRE(e.status() == Status::Playing);

	e.reset();
	CHECK(e.status() == Status::Off);
	CHECK(e.capturedFrames() == 0u);
}
