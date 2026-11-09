/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#pragma once

#include "device/device.h"
#include "fx/sklitter/sklitter_types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// THE GESTURE, AND WHAT SURVIVES OF THE REFERENCE'S STATUS MACHINE.                                  //
//--------------------------------------------------------------------------------------------------//
//
// The reference starts scatter by HOLDING an encoder. A plugin has no encoder, so `Param::Engage` is a
// 0/1 control the DAW automates and MIDI drives, and this class turns its EDGES into the reference's
// transitions.
//
// **WHAT IS NOT HERE, AND WHY.** The reference has ONE stutter engine that every track contends for;
// every Feathers device slot owns its own instance, so source contention, orphaning and configuration
// takeover have nothing left to mean and none of them came across. Of the reference's waiting states,
// only the one that aligns a looper start to a beat is live there, and that is the one re-expressed
// below.
//
// **ONE PIECE OF STATE IS LOAD-BEARING AND IS KEPT: THE CONFIG AT PRESS.** The release is decided by
// the configuration as it was when the gesture began, not as it is at release, because the live one
// may have been overwritten meanwhile: the user can turn the Mode knob or a DAW can automate it while
// Engage is held. Consult the live config instead and the same gesture ends or does not end depending
// on what was touched: press in Burst unlatched, turn to Shuffle (which always latches), release, and
// it never stops.

enum class Status : uint8_t {
	Off,
	/// Burst: filling the rate-sized buffer, not yet audible.
	Recording,
	/// The looper modes: filling the ring and waiting for a grid line.
	Standby,
	Playing,
};

class EngageMachine {
public:
	void reset() noexcept {
		status_ = Status::Off;
		engaged_ = false;
		captured_ = 0;
		armedBeat_ = kNoBeat;
		releasedEarly_ = false;
	}

	/// THE GESTURE, LEVEL-TRIGGERED BUT EDGE-ACTED. Called every block with Engage's current value;
	/// only a CHANGE does anything, because treating the level as a press would restart the capture
	/// count every block and never reach a trigger.
	void setEngaged(bool engaged, const Settings& live) noexcept {
		if (engaged == engaged_) {
			return;
		}
		engaged_ = engaged;
		if (engaged) {
			// **A RE-PRESS DURING A LOOPER CAPTURE KEEPS IT (design option B).** Released early,
			// the gesture is still in Standby and still recording at the song's phase; zeroing the count
			// here threw that away. MEASURED in the tripwire log: released at 14.47 with 6.8 of 8 beats
			// captured, re-pressed at 15.87, and the trigger moved from 16 to 24. The same capture, the
			// same buffer — only the release is undone.
			const bool resumes = status_ == Status::Standby && live.isLooperMode();
			// THE SNAPSHOT. Everything the release will consult is decided here.
			armed_ = live;
			if (!resumes) {
				captured_ = 0;
			}
			armedBeat_ = kNoBeat;
			releasedEarly_ = false;
			status_ = armed_.isLooperMode() ? Status::Standby : Status::Recording;
			return;
		}
		// **A RELEASE ALWAYS ENDS IT HERE, AND THE REFERENCE'S LATCH DOES NOT SURVIVE THE PORT.**
		//
		// The first install could not be stopped: turning the enable switch off did nothing and it kept
		// going. It was the latch doing exactly what the reference does — every looper mode latches, so
		// a release was ignored and there was no way back out but the bypass.
		//
		// **LATCH EXISTS TO TURN A MOMENTARY GESTURE INTO A SUSTAINED ONE, AND A DAW PARAMETER IS
		// ALREADY SUSTAINED.** On the reference the gesture is an encoder HELD DOWN: you cannot hold it
		// for eight bars while playing, so the firmware offers to keep it on after you let go. Here
		// the gesture is a 0/1 control the host holds at 1 for exactly as long as the automation says
		// — the holding is free, and the thing latch was invented to solve does not happen. Keeping it
		// only made the control ONE-WAY, which is a control the hand cannot trust.
		//
		// This is the same reasoning that made `Engage` a parameter rather than an encoder hold. The
		// mechanism is ported where it means something and dropped where it was compensating for a
		// constraint a plugin does not have.
		if (status_ == Status::Playing) {
			status_ = Status::Off;
			return;
		}
		// Released before playback ever began. The reference remembers this and ends at the moment
		// the trigger would have fired rather than starting; a stutter that begins after you let go
		// is one you cannot control. (asked whether it should end at once: no — the
		// hardware way stands. What changed is that the capture it keeps making is no longer thrown
		// away; see the press above.)
		releasedEarly_ = true;
	}

	/// HOW MUCH AUDIO HAS ARRIVED SINCE THE PRESS.
	void captured(size_t frames) noexcept { captured_ += frames; }

	/// ONE BLOCK. `needFrames` is how much audio a start requires.
	void advance(const device::TransportClock& clock, size_t needFrames) noexcept {
		if (status_ == Status::Off || status_ == Status::Playing) {
			return;
		}
		const bool haveAudio = captured_ >= needFrames;

		if (status_ == Status::Recording) {
			advanceRecording(haveAudio);
			return;
		}

		// Standby: wait for the next beat boundary, measured in beats.
		//
		// **THE FALLBACK IS ON THE POSITION, NOT ON THE TEMPO.** A stopped host still reports a tempo
		// (TransportClock says so at length), so a device that fell back on `haveTempo()` would wait
		// for a boundary that never moves. And a stopped transport sits at one beat for ever, so it
		// takes the immediate road too.
		if (!clock.havePositionBeats() || !clock.playing) {
			if (haveAudio) {
				finish();
			}
			return;
		}

		// **THE NEXT BOUNDARY IS "THE BEAT CHANGED", NOT "THE BEAT REACHED ONE MORE" — and inside a DAW
		// loop those are different questions.** This waited for `floor(beat) >= armed + 1`, and a cycle
		// region never gets there when the loop ends first: the song wraps back and the target sits past
		// its end for ever. In a ONE-BAR loop a looper press NEVER played — a one-bar buffer needs a whole
		// pass to capture, the re-target below walks to beat 4, and the song wraps to 0. MEASURED
		// (the playhead stops and engage does nothing): Shuffle, Repeat and Grain
		// still in Standby after eight bars of a four-beat loop. On a song that only moves forward the
		// two tests are the same test; a wrap back to the loop's start is a grid line as well.
		const long long beat = static_cast<long long>(std::floor(clock.songPositionBeats));
		if (armedBeat_ == kNoBeat) {
			armedBeat_ = beat; // the beat the wait starts in; the NEXT boundary ends it, as the reference's first check
			return;
		}
		if (beat == armedBeat_) {
			return;
		}
		if (!haveAudio) {
			// Too little recorded audio: the trigger moves to the next beat. Starting anyway plays a
			// region that was never written.
			armedBeat_ = beat;
			return;
		}
		finish();
	}

	[[nodiscard]] Status status() const noexcept { return status_; }

	/// THE CONFIG AS IT WAS AT PRESS. What the release consults, and what the DSP should read for
	/// anything decided when the gesture began.
	[[nodiscard]] const Settings& armed() const noexcept { return armed_; }

	/// HOW MUCH HAS ARRIVED. **Not test-only**: the device reads it to find the EXACT frame the
	/// threshold is crossed on, so playback can begin mid-block. Advancing only at block boundaries
	/// made the device block-size dependent -- the same audio started stuttering at frame 1536 under
	/// 256-frame blocks and at 8192 under one long call.
	[[nodiscard]] size_t capturedFrames() const noexcept { return captured_; }

private:
	static constexpr long long kNoBeat = INT64_MIN;

	/// One block of Burst's Recording state, given whether enough audio has arrived.
	void advanceRecording(bool haveAudio) noexcept {
		// Recording ends as soon as enough audio is in; until then the state holds.
		if (haveAudio) {
			finish();
		}
	}

	/// The trigger fired. If the gesture was already released and did not latch, it ends here rather
	/// than starting.
	void finish() noexcept {
		// A RELEASE THAT ARRIVED BEFORE THE TRIGGER ENDS IT, whatever mode it was pressed in. This
		// used to consult the latch and so let a looper mode start playing AFTER the gesture
		// had ended — the same one-way behaviour, reached by the other road.
		status_ = releasedEarly_ ? Status::Off : Status::Playing;
		releasedEarly_ = false;
	}

	Status status_ = Status::Off;
	bool engaged_ = false;
	bool releasedEarly_ = false;
	size_t captured_ = 0;
	long long armedBeat_ = kNoBeat;
	Settings armed_ {};
};

} // namespace feathers::fx::sklitter
