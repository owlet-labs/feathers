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

#pragma once

#include <optional>
#include "mod/mseg/mseg_types.h"

// THE SHAPE, RUNNING: a gate goes in, a number comes out, once per kBlockSamples — the same contract
// the DAHDSR's Envelope has, deliberately, because the rack ticks them the same way.
//
// THE STATE MACHINE IS OURS, as the DAHDSR's now is too (2026-09-24: the DAHDSR's was a third-party
// GPL component until the licence migration replaced it). A variable segment count with loop markers
// inside the shape is not a fixed-stage envelope with a flag, so there was never anything to share.
// What that costs is this file; what it buys is that the loop markers are expressible at all.
//
// NO ALLOCATION, EVER, and not only on the audio thread: the segments are an array in Settings and
// the position is two numbers. tick() does arithmetic on a struct the caller owns.

namespace feathers::mod::mseg {

/// Where the shape is, so a case and a card can ask rather than infer it from the output. A value
/// alone cannot say which segment produced it — an MSEG can pass through the same level three times —
/// which is the "assert the value, not the difference" rule seen from the other end.
struct Position {
	/// Which segment is running, or kMaxSegments when the shape has finished.
	size_t segment = 0;
	/// How far through that segment, 0..1.
	float phase = 0.0f;
	/// Whether the shape is holding at a sustain breakpoint rather than advancing.
	bool sustaining = false;
	/// Whether it has run off the end and is doing nothing.
	bool finished = true;

	friend bool operator==(const Position&, const Position&) = default;
};

class Shape {
public:
	/// Told the sample rate, which is the only thing a duration needs to become a number of ticks.
	/// Resets, for the reason the DAHDSR's prepare states: a rate change is not a gesture, and
	/// carrying state across one would invent a shape out of a sample-rate switch.
	void prepare(double sampleRate);

	/// Idle, at the start level, gate down. What activate() and an all-notes-off mean.
	void reset();

	/// **SMOOTH THROUGH RETRIGGER** (backlog MM): this fresh shape's SMOOTHER starts where `other`'s output stands, so
	/// a new note glides from there instead of from zero. The smoother only — the shape itself starts as it would.
	/// `mod::PerNoteTable::processNotes` is the one caller.
	void carrySmoothingFrom(const Shape& other) noexcept {
		smoothed_ = other.smoothed_;
		value_ = other.value_;
		smoothedR_ = other.smoothedR_;
		valueR_ = other.valueR_;
	}

	/// **A TRIG FIRED (backlog MV): the next tick restarts the shape from its start** — the DAHDSR's rule (see
	/// `dahdsr::Envelope::fire`): always under Once, Free and LFO Retrig, under Gate and Loop only while the gate is
	/// up. On a Free shape the song is holding, the restart shifts the lock (`TransportLock::restartAt`).
	void fire() noexcept { trigPending_ = true; }

	/// ONE TICK OF kBlockSamples SAMPLES, and the whole of this device's contract with the engine.
	///
	/// `gate`, `retrigger` and `rateMul` mean exactly what they mean on the DAHDSR — see dahdsr.h.
	/// They are the same three because the rack calls both the same way, and a second device that
	/// needed a fourth would be telling us the rack's contract was wrong rather than that this device
	/// was special.
	float tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
	           float rateMul = 1.0f);

	/// THE CLOCK THIS DEVICE WAS LAST TICKED WITH — a test-only seam, and it exists for CLAUDE.md's
	/// reason: "a claim about a value cannot be inferred from a rendered buffer — assert the value".
	/// Whether the host's tempo reaches the device is exactly such a claim, and nothing about the
	/// output can answer it until something depends on it.
	TransportClock clockForTest() const noexcept { return clock_; }

	/// Where the shape stands: 0..1, or -1..1 when Settings::bipolar. Post-smoothing.
	float value() const noexcept { return value_; }
	/// **THE RIGHT EAR'S LAST OUTPUT** (backlog PA): the same shape read `Settings::stereoLead` of the window further on,
	/// through its own smoother. `value()` exactly while Stereo Phase is 0.
	float valueRight() const noexcept { return valueR_; }

	/// **WHERE THE RIGHT EAR STANDS WHEN THE LEFT IS `phase` THROUGH `segment`, running forward** — the shape placed
	/// there and run `lead` of the window on under `gate`, raw: what the card draws the right ear's trace from, by the
	/// engine's own walk rather than a restatement of it. A place, not a picture x: the editor's x gives a tiny segment
	/// a floor of width to grab (`layoutOf`), which the engine's time does not.
	[[nodiscard]] static float rawLeadingAt(const Settings& settings, size_t segment, float phase, float lead, bool gate);

	/// The raw output, before smoothing and before the bipolar rescale — what a card draws and what a
	/// case about SHAPE asks. Smoothing is a property of the route rather than of the shape.
	float rawValue() const noexcept { return raw_; }

	/// Where in the shape it is. See Position for why this is exposed rather than inferred.
	Position position() const noexcept { return position_; }

	/// THE SAME READING IN THE RACK'S SHARED VOCABULARY, for the surface that draws a playhead.
	///
	/// A TRANSLATION AND NOT A SECOND SOURCE — it is computed from `position_` here rather than
	/// tracked alongside it, so the two cannot drift. `Automod::lfo()` and `Lfo::phase()` stand in the
	/// same relation.
	///
	/// FINISHED IS `kIdle` RATHER THAN THE LAST SEGMENT: a shape that has run off the end is not at
	/// its end, it is nowhere, and `Position::segment` already says so by holding `kMaxSegments`.
	[[nodiscard]] mod::Playhead playhead() const noexcept {
		if (position_.finished || position_.segment >= kMaxSegments) {
			return {};
		}
		return { static_cast<uint32_t>(position_.segment), position_.phase };
	}

	/// Finished AND the smoothed output has arrived where the shape ended — the DAHDSR's rule (2026-09-26):
	/// done is decided by the output. MEASURED before: idle for 57 ticks while a 2 ms smoothing was still
	/// carrying the last segment down. Exact, because `mod::smoothingStep` snaps onto the target.
	/// The right ear's smoother is asked too (backlog PA): it ends where the left does, and done is the output's.
	bool idle() const noexcept {
		return position_.finished && smoothed_ == static_cast<double>(raw_) && smoothedR_ == static_cast<double>(raw_);
	}

	/// HOW MANY TIMES THE SHAPE HAS BEEN SENT BACK TO ITS START since `reset`. A test seam: a restart
	/// is not recoverable from the output (a looping shape passes its start every cycle anyway).
	[[nodiscard]] uint32_t restartsForTest() const noexcept { return restarts_; }

private:
	/// Move the position forward by one tick's worth of phase, honouring the sustain point and the
	/// loop markers. Split out because it is the whole of the state machine and the arithmetic around
	/// it is not.
	void advance(const Settings& settings, bool gate, float rateMul);
	/// The same walk with its budget in seconds of the window's time — `advance` spends one tick's worth, the right
	/// ear's lookahead spends its lead.
	void advanceBy(const Settings& settings, bool gate, double seconds);

	/// The raw output where `position_` stands: the finished hold, the sustain level, or the segment's curve. Clamped.
	[[nodiscard]] float rawAtPosition(const Settings& settings) const noexcept;

	/// **THE RIGHT EAR'S RAW VALUE** (backlog PA): a copy of this shape run `lead` of the window further on — through the
	/// loop, the hold and the end exactly as the left ear will run, under the gate the left was just given. On a
	/// song-locked Free shape (`locked`), the song's place plus the lead, then the tick's own walk, as the left ear.
	[[nodiscard]] float rawRightAt(const Settings& settings, float lead, bool gate, float rateMul,
	                               std::optional<double> locked) const;

	/// Start the shape from its start level, wherever the output currently sits. It used to continue
	/// from the live output when re-fired mid-flight, and that was a restart nobody could hear — see
	/// the definition for the report and the measurement.
	void restart(const Settings& settings);

	/// **PUT THE SHAPE WHERE THE SONG SAYS — backlog DQ.** `turns` is how many whole cycles through
	/// the window the playhead is; only its fraction is used. Walks the segment shares to find which
	/// segment that lands in and how far into it, and sets the entry level with it so the interpolator
	/// draws from the right place.
	void lockTo(const Settings& settings, double turns);

	/// **SET THE STARTING POSITION FROM A 0..1 PHASE VALUE.** Walks the segment shares to find the
	/// segment and phase, the same arithmetic `lockTo` uses, so a phase setting and a locked position
	/// cannot disagree about where a boundary is. Used by `restart` when `phase` is non-zero.
	void setPhase(const Settings& settings, float phase);

public:
	/// **HOW MANY WINDOWS THROUGH THE SONG THE PLAYHEAD IS, or nothing where it cannot say.**
	///
	/// Public because it is a VALUE: "the song puts the shape here" cannot be recovered from an
	/// output level, which is one number standing for a segment, a phase and a curve at once. The
	/// same seam automod's `lockedTurns` is, for the same reason.
	[[nodiscard]] std::optional<double> lockedTurns(const Settings& settings) const;

private:
	/// Where the song is and how long the window is, in ONE unit — beats when a tempo is known, seconds
	/// when not — with one tick's length in that unit. What `lockedTurns` divides and `TransportLock`
	/// follows; nothing when the shape is not to be locked at all.
	struct SongWindow {
		double position;
		double window;
		double tickLength;
		int road;
	};
	static constexpr int kBeatsRoad = 1;
	static constexpr int kSecondsRoad = 2;
	[[nodiscard]] std::optional<SongWindow> songWindow(const Settings& settings) const;

	TransportLock lock_ {};

	Position position_ {};
	/// Where the segment now running began: the start level for segment 0, the previous breakpoint
	/// otherwise. Held rather than recomputed so `advance` and the output arithmetic agree on it.
	float segmentFrom_ = 0.0f;

	/// **PING-PONG DIRECTION: +1 forward, -1 reverse.** Toggled by the advance loop at the loop
	/// boundaries; reset to +1 on restart. A non-pinging loop ignores this and always wraps forward.
	int direction_ = 1;

	double smoothed_ = 0.0;
	float raw_ = 0.0f;
	float value_ = 0.0f;
	/// THE RIGHT EAR (backlog PA) — its own smoother, the left's coefficient. Held equal to the left's while Stereo Phase
	/// is 0, so turning it up starts the right ear from where the left stands rather than from rest.
	double smoothedR_ = 0.0;
	float valueR_ = 0.0f;
	bool gateWas_ = false;
	uint32_t restarts_ = 0;
	/// A Trig fired since the last tick — see `fire`.
	bool trigPending_ = false;

	/// What the rack last handed over. Held rather than passed down, because `advance` does not need
	/// it yet and a parameter nothing reads is a parameter that drifts.
	TransportClock clock_ {};

	double tickSeconds_ = static_cast<double>(kBlockSamples) / 48000.0;

	/// The one-pole coefficient and the knob it was computed from, so a std::exp is paid when the
	/// smoothing time MOVES rather than 750 times a second while it sits still. Same trick, same
	/// reason, as the DAHDSR's.
	float smoothingKnobWas_ = -1.0f;
	double smoothingCoeff_ = 1.0;
};

} // namespace feathers::mod::mseg
