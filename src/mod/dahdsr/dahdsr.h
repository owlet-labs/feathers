/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "mod/dahdsr/dahdsr_types.h"

// THE ENVELOPE ITSELF: a gate goes in, a number comes out, once per kBlockSamples.
//
// THE STATE MACHINE AND THE CURVE LAW ARE OURS (2026-09-24, the licence migration). Until then both came
// from a third-party GPL library; they were rewritten CLEAN-ROOM — nobody working on this file read
// that library's source — and fitted to it only as a BLACK BOX, by driving the old and new envelopes
// with the same gates and settings and comparing what came out. The maths is stated in dahdsr.cpp
// beside the code that does it, with its sources.
//
// It is Bitwig's AHDSR modulator control for control: delay, attack, hold, decay, sustain, release,
// and a bipolar curve on each of the three stages that MOVE (attack, decay, release).
//
// And it ships no model dropdown, which Bitwig has. Bitwig offers Analog / Relative / Digital — three
// fixed curve sets — and a continuous per-stage shape reaches all three and everything between them.
// A dropdown beside these knobs would be a control that changes nothing the knobs cannot already say,
// which is the honesty rule's own case.
//
// NO POINTER ANY MORE. The state used to live behind a unique_ptr because the vendor header pulled
// <iostream> into every TU that reached this one; the state is now a handful of numbers held by
// value, so an Envelope is an ordinary copyable object and `continueFrom` is a plain copy.

namespace feathers::mod::dahdsr {

/// THE CURVE LAW, exposed so a test can pin it and a card could draw it — see dahdsr.cpp for the
/// maths. `x` is how far through a stage (0..1), `shape` the stage's curve control (-1..1); returns
/// 0..1 with f(0) = 0, f(1) = 1, a straight line at shape 0, rising early for shape > 0 and late for
/// shape < 0. Every stage is this one function aimed at its own two levels.
[[nodiscard]] double stageCurve(double x, float shape) noexcept;

class Envelope {
public:
	/// Told the sample rate, which is the only thing a stage time needs to become a number of ticks.
	/// Resets, for PuckMotion::prepare's stated reason: a rate change is not a gesture, and carrying
	/// state across one would invent an envelope out of a sample-rate switch.
	void prepare(double sampleRate);

	/// Idle, at zero, gate down. What activate() and an all-notes-off mean.
	void reset();

	/// **A TRIG FIRED (backlog MV), taken up by the next tick** — latched, like `retrigger`, so it is one fact
	/// however the tick is reached. The tick restarts the envelope from where it stands whenever the mode lets the
	/// shape run: always under Once, Free and LFO Retrig (Once then runs its whole shot by itself), and under Gate and
	/// Loop only while the gate is up, because the gate is what holds those open. Leg does not refuse it: Leg is about
	/// notes, and a Trig is an explicit fire.
	void fire() noexcept { trigPending_ = true; }

	/// ONE TICK OF kBlockSamples SAMPLES, and the whole of this envelope's contract with the engine.
	///
	/// `gate` is whether the note stack is holding anything at all — the global gate decided on
	/// 2026-08-22, and see mod_rack.h for why it is derived from the voices' own stages rather than
	/// counted separately. `retrigger` is a note-on that has arrived since the last tick, which is a
	/// LATCH rather than an edge: a note that starts and ends inside one tick still starts the
	/// envelope, because the alternative is a fast passage with notes that silently do nothing.
	///
	/// Returns the same number value() will, so a caller that wants it need not ask twice.
	///
	/// `rateMul` SCALES HOW FAR ONE TICK CARRIES THE SHAPE, and it exists for drawing rather than for
	/// playing — the engine always passes 1. A card wants ENOUGH SAMPLES PER STAGE to see a curve,
	/// and no single sample rate can give that when the stages span half a millisecond to ten
	/// seconds: at any rate fine enough to draw a 10 ms attack, a 10 s release is a million ticks.
	/// Scaling the rate per stage lets one run of the REAL envelope produce a readable shape for all
	/// of them, which is what keeps the picture from needing a second implementation of the kernel.
	float tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
	           float rateMul = 1.0f);

	/// Where the envelope stands: 0..1, or -1..1 when Settings::bipolar. Post-smoothing.
	float value() const noexcept { return value_; }

	/// The RAW state-machine output, before smoothing and before the bipolar rescale. This is what a
	/// card draws and what a test about SHAPE asks: smoothing is a property of the route, not of the
	/// envelope, and a case about the decay curve should not have to undo a one-pole to see it.
	float rawValue() const noexcept { return raw_; }

	Stage stage() const noexcept { return stage_; }

	/// WHERE THE ENVELOPE STANDS, in the rack's shared vocabulary — the stage, and how far through it.
	///
	/// **THE PHASE IS THE STATE MACHINE'S OWN** — `phase_`, 0..1 within the running stage and reset at
	/// every transition. No arithmetic on `value()` can recover it, because decay and release cross
	/// the same levels in the same direction; this is the five-line seam the project's testing rules prescribe
	/// for precisely this case: if the claim is "this value is X at this instant", read the value.
	///
	/// **THE ORDINAL IS `Stage` ITSELF, not an index into some drawing order.** A card already has
	/// `stageEndX(stage)`; handing it a position in its own vocabulary means nothing has to agree
	/// about a second ordering.
	///
	/// **SUSTAIN REPORTS PHASE 0, AND THAT IS A STATEMENT RATHER THAN A GAP.** Sustain does not
	/// advance — it holds for as long as the gate is up — so there is no "how far through" to report
	/// and any number other than 0 would be inventing progress. A marker that sits still while a note
	/// is held is telling the truth about what the envelope is doing.
	[[nodiscard]] mod::Playhead playhead() const noexcept;

	/// Whether it is doing nothing at all — finished, or never started. The rack asks this to know
	/// whether it may stop paying for the envelope, so it must be false for every stage including a
	/// delay that has not reached its attack yet.
	///
	/// **AND FALSE WHILE THE SMOOTHING IS STILL CARRYING A TAIL.** The state machine reaches Idle the tick
	/// the raw release ends; the output does not, because the one-pole lags it by up to its time
	/// constant. The per-note table stops ticking an envelope the moment this says true, so a test on
	/// the stage alone FROZE a smoothed note where the one-pole stood — measured at 0.585 of a 0.6
	/// sustain with 1 s smoothing — and a voice whose Level it drove was held open by it for ever.
	/// "Done" is decided by the output (CLAUDE.md): `smoothed_` reaches the raw rest EXACTLY, by the
	/// snap in `tick`, so this is an exact test and not a threshold.
	bool idle() const noexcept { return stage() == Stage::Idle && smoothed_ == static_cast<double>(raw_); }

	/// **THE ONE-POLE'S REST** — `mod::kSmoothingRest`, the law all three modulators share; named here
	/// because this is where the DAHDSR's own bug was found and fixed.
	static constexpr double kSmoothingRest = mod::kSmoothingRest;

	/// **TAKE ANOTHER ENVELOPE'S STATE AS THIS ONE'S** — where it stands, its stage and phase, its gate and its
	/// smoothing — so a note that should NOT restart (Single Trigger, the card's "Leg") carries on from the
	/// note already sounding instead of starting from nothing. Now a plain copy: the state is all values.
	void continueFrom(const Envelope& other) noexcept;

	/// **SMOOTH THROUGH RETRIGGER** (backlog MM): this fresh envelope's SMOOTHER starts where `other`'s output stands,
	/// so a new note glides from there instead of from zero. The smoother only — the envelope itself starts from
	/// rest as it would, which is what tells this apart from `continueFrom`. `mod::PerNoteTable::processNotes` is the
	/// one caller.
	void carrySmoothingFrom(const Envelope& other) noexcept {
		smoothed_ = other.smoothed_;
		value_ = other.value_;
	}

private:
	/// START THE SHAPE FROM WHERE IT STANDS: into Delay (which the advance steps straight past when
	/// the delay knob is at the bottom), with the attack aimed from `from` to 1.
	void start(float from) noexcept;
	/// Walk the state machine forward by `seconds` of envelope time — see dahdsr.cpp.
	void advance(const Settings& settings, double seconds) noexcept;
	/// The gate, the notes and the Trig into the state machine, and the machine one tick on — every road but Free's
	/// on a playing transport. `rate` is `tick`'s `rateMul`, sanitised.
	void runMachine(const Settings& settings, bool gate, bool retrigger, double rate) noexcept;
	/// The level a timed stage stands at `p` (0..1) of the way through, from the levels it was started with.
	[[nodiscard]] float levelAt(const Settings& settings, Stage stage, double p) const noexcept;
	/// **FREE ON A PLAYING TRANSPORT: THE SHAPE IS READ OFF THE SONG'S SECONDS** — see `tick` in dahdsr.cpp.
	/// `cycle` is the stage sum in envelope time, `window` the same in song seconds.
	void followSong(const Settings& settings, const TransportClock& clock, double cycle, double window,
	                bool fired) noexcept;

	/// THE STATE MACHINE, and all of it. `phase_` is 0..1 through the running timed stage;
	/// `attackFrom_` is the level the attack (and the delay before it) starts at, which is where the
	/// envelope stood when it was (re)started; `releaseFrom_` is where it stood when the gate fell.
	Stage stage_ = Stage::Idle;
	double phase_ = 0.0;
	float attackFrom_ = 0.0f;
	float releaseFrom_ = 0.0f;

	/// The one-pole's state.
	double smoothed_ = 0.0;
	float raw_ = 0.0f;
	float value_ = 0.0f;
	bool gateWas_ = false;
	/// **ONE-SHOT HAS LET GO OF ITS OWN GATE.** Under `Mode::Once` the envelope stops asking the note
	/// whether to hold: it latches here the moment the shape arrives at Sustain, and from then on the
	/// state machine is told the gate is DOWN, so the release runs straight out of the decay. That is
	/// what gives a sustaining envelope a fixed duration — see `tick`. Cleared wherever the shape restarts.
	bool onceLetGo_ = false;
	/// A Trig fired since the last tick — see `fire`.
	bool trigPending_ = false;
	/// **THE PART OF A TICK LEFT AFTER THE DECAY LANDED ON SUSTAIN**, in envelope seconds, so a loop's restart on the
	/// next tick starts on time rather than on the tick grid (the cycle is its stage sum exactly; see `tick`).
	double loopCarry_ = 0.0;
	/// FREE ON A PLAYING TRANSPORT follows the song's seconds through this (`mod::TransportLock`), and the three
	/// beside it are what the last locked tick read: the turns it stood at, so the next tick can tell which stage
	/// ends it crossed, and the whole cycle it was in, so a new cycle starts its attack from the sustain.
	TransportLock lock_ {};
	double lockedTurns_ = 0.0;
	double lockedCycle_ = 0.0;
	bool locked_ = false;

	double tickSeconds_ = static_cast<double>(kBlockSamples) / 48000.0;

	/// The one-pole coefficient and the knob it was computed from, so a std::exp is paid when the
	/// smoothing time MOVES rather than 750 times a second while it sits still.
	float smoothingKnobWas_ = -1.0f;
	double smoothingCoeff_ = 1.0;
};

} // namespace feathers::mod::dahdsr
