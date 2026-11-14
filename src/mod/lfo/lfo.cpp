/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "lfo.h"
#include "mod/per_note.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathers::mod::lfo {

using device::kDivisionBeats;
using device::kDivisionCount;

namespace {

/// **FREE, IN HZ, ON A PLAYING TRANSPORT THAT SAYS WHERE IT IS IN SECONDS** — the road whose phase is the song's
/// (backlog NJ). A rate of zero is a hold, not a period, so it stays on the free road, which holds.
bool followsSongSeconds(const Settings& s, const TransportClock& clock, float hz, float rateMul) noexcept {
	const double cyclesPerSecond = static_cast<double>(hz) * static_cast<double>(rateMul);
	return s.mode == Mode::Free && !s.synced() && clock.playing && clock.havePositionSeconds()
	       && std::isfinite(cyclesPerSecond) && cyclesPerSecond > 0.0;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// Shape — the DSP                                                                                //
//------------------------------------------------------------------------------------------------//

void Shape::prepare(double sampleRate) {
	sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kBlockSamples) / sampleRate_;
	smoothingCoeffValid_ = false; // the tick length moved
	reset();
}

void Shape::reset() {
	position_ = Position {};
	smoothed_ = 0.0;
	raw_ = 0.0f;
	value_ = 0.0f;
	smoothedR_ = 0.0;
	valueR_ = 0.0f;
	gateWas_ = false;
	restarts_ = 0;
	resting_ = false;
	finishing_ = false;
	cycleTravel_ = 0.0f;
	lock_.release();
	randomLock_.release();
	randomStep_ = 0;
	randomValue_ = 0.0f;
	randomPhase_ = 0.0f;
	randomPrimed_ = false;
	trigPending_ = false;
	trigRestart_ = false;
}

float Shape::tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
                  float rateMul) {
	const Settings s = settings;
	clock_ = clock;
	cycles_ = s.mode != Mode::Once;

	// THE GATE, ONCE, AT ONE PLACE. Free and LFO Retrig substitute an always-up gate.
	const bool free = (s.mode == Mode::Free || s.mode == Mode::LfoRetrig);
	const bool effectiveGate = free || gate;
	const bool rising = effectiveGate && !gateWas_;
	// **ONCE DOES NOT RE-FIRE, AND THIS DEVICE WAS THE ONLY ONE THAT DID** (decided
	// 2026-09-19). The clause `|| (s.mode == Mode::Once && retrigger)` stood here with nothing
	// explaining it, and MEASURED it restarted the shape on a second note under a held gate: phase
	// 0.267 → 0.003. The DAHDSR and the MSEG both refuse, and `mod::Mode` says in as many words that
	// Once is *"a stab that cannot be re-fired by the next note in a held chord"*.
	//
	// THE QUESTION IT RAISED IS WORTH KEEPING, because this device is MONO — one shape, no voices,
	// and the rack's gate is global, so `retrigger` is the only evidence a second note exists at all.
	// Refusing it means a chord or a legato line fires the shot on the FIRST note and not again until
	// every key is up. That is the decision: **one shot per PHRASE on a mono modulator**, where the
	// DAHDSR's per-note table gives it one shot per NOTE by having a note's worth of envelope each.
	// The mode's meaning is the same sentence in both cases; what differs is what the device has to
	// apply it to. That is now the COLLAPSED wave only: a per-voice destination reads a wave per note
	// (`OpsShape` below, 2026-10-03), where Once is one shot per NOTE.
	// **NOTES FIRE TRIG, OR ONLY THE TRIG DOES** (backlog NU, `mod::notesTrigParamDesc` for the rule per mode). Off,
	// a key going down neither starts nor retriggers the wave; Free's and LFO Retrig's own start (the gate they hold
	// up from the first tick) is not a note, so it stands.
	const bool notes = notesFireTrig(s);
	const bool mayRetrigger = notes
	    && (((s.mode == Mode::Gate || s.mode == Mode::Loop) && retrigger) || (s.mode == Mode::LfoRetrig && retrigger));

	// **A TRIG (backlog MV) RESETS THE PHASE WHEREVER THE MODE LETS THE WAVE RUN** — see `fire`.
	trigRestart_ = std::exchange(trigPending_, false) && (effectiveGate || s.mode == Mode::Once);
	if (trigRestart_ || (effectiveGate && ((rising && (notes || free)) || mayRetrigger))) {
		restart(s);
	}
	gateWas_ = effectiveGate;
	// A KEY HELD OVER A WAVE NO NOTE STARTED IS AT REST, not parked on its start phase: under Gate and Loop an unfired
	// wave reads neutral, as one whose key went up does. Only with the normal broken can a held key meet a wave that
	// was never started, so the default road never reaches this.
	if (!notes && (s.mode == Mode::Gate || s.mode == Mode::Loop) && position_.finished && !resting_) {
		rest();
	}

	// **GATE AND LOOP STOP AT THE KEY**. The gate was only ever heard here as a RESTART, so both modes cycled for ever
	// after the key — per note under Poly, and the mono wave — and were the same mode. Now: Gate goes to NEUTRAL at
	// once (through the smoothing, which is what keeps it from clicking), the phase still; Loop runs out the cycle
	// it is in, then rests at neutral the same way. Free and LFO Retrig hear no gate (`effectiveGate` is up).
	stopsAtKey_ = s.mode == Mode::Gate || s.mode == Mode::Loop;
	if (stopsAtKey_ && !effectiveGate && !position_.finished) { // running: struck, and not yet stopped
		if (s.mode == Mode::Gate) {
			rest();
		} else {
			finishing_ = true;
		}
	}

	// COMPUTE THE RATE. Synced uses the transport; free uses the scale as Hz.
	// **THE RULE MOVED TO `rateHz`, AND THIS IS NOW ITS ONLY CALLER ON THE AUDIO SIDE.** It was written
	// out here and nowhere else, which was fine until the card needed the same answer: what a one-pole
	// does to a periodic wave depends on `tau / period`, so a picture of the smoothing needs the
	// period the ENGINE is using rather than one computed beside it. See `rateHz`.
	const float hz =
	    static_cast<float>(rateHz(s, clock.haveTempo() ? clock.secondsPerBeat : 0.0));

	// ADVANCE THE PHASE.
	//
	// **FREE + SYNCED ON A PLAYING TRANSPORT: FOLLOWED FROM THE SONG** — the MSEG's rule, and two things
	// this device got wrong beside it. It placed the phase at `fmod(position, window) / window` every
	// tick, so a route into Rate threw the phase around by `turns × Δwindow / window` (see
	// `mod::TransportLock`, which now follows the song's STEP and re-anchors only at a jump). And it
	// never asked whether the transport was PLAYING, which the MSEG always has: a stopped host goes on
	// reporting the one position it stopped at, and this froze the wave on it. Stopped, it free-runs.
	const double windowBeats = s.synced()
	    ? kDivisionBeats[std::min(static_cast<size_t>(s.timeBase - 1), kDivisionCount - 1)]
	          * timeSeconds(s.timeScale)
	    : 0.0;
	// ONLY FREE IS LOCKED TO THE SONG — LFO Retrig restarts on notes. See the MSEG.
	if (s.mode == Mode::Free && clock.playing && clock.havePositionBeats() && clock.haveTempo() && windowBeats > 0.0
	    && std::isfinite(windowBeats)) {
		// **PLUS THE PHASE CONTROL** (backlog NJ follow-up, 2026-10-17): it was live here and moved nothing — the honesty
		// rule broken on the one road that ignored it. The Hz road adds it the same way.
		double turns = lock_.follow(clock.songPositionBeats, windowBeats, tickSeconds_ / clock.secondsPerBeat, 1);
		if (trigRestart_) {
			lock_.restartAt(0.0); // a Trig's reset holds against the song (backlog MV); Phase is added below
			turns = 0.0;
		}
		turns += static_cast<double>(s.phase);
		// FLOOR, NOT `fmod`: a count-in runs the song negative, and `fmod` keeps the sign.
		position_.phase = static_cast<float>(turns - std::floor(turns));
		position_.finished = false;
	} else if (followsSongSeconds(s, clock, hz, rateMul)) {
		// **FREE IN HZ ON A PLAYING TRANSPORT: FOLLOWED FROM THE SONG'S SECONDS** (backlog NJ, 2026-10-17:
		// phase = Rate × song position in seconds + Phase). SECONDS, not beats, because this rate is a frequency and
		// owes the tempo nothing: a tempo change mid-song does not move it, a playhead jump moves it with the song
		// (the lock re-anchors), and a bounce therefore matches playback. Its own unit (2), so leaving a synced base
		// for this one re-anchors rather than subtracting beats from seconds. Stopped, it runs free as before.
		double turns = lock_.follow(clock.songPositionSeconds,
		                            1.0 / (static_cast<double>(hz) * static_cast<double>(rateMul)), tickSeconds_, 2);
		if (trigRestart_) {
			lock_.restartAt(0.0); // a Trig's reset holds against the song's seconds too (backlog MV × NJ)
			turns = 0.0;
		}
		turns += static_cast<double>(s.phase);
		position_.phase = static_cast<float>(turns - std::floor(turns));
		position_.finished = false;
	} else {
		lock_.release();
		advance(s, hz, rateMul);
	}

	// EVALUATE. The walk steps first, because a Loop running out its last step rests in it (`advanceRandom`).
	if (isRandom(s.wave) && !(stopsAtKey_ && resting_)) {
		advanceRandom(s, hz, rateMul);
	}
	if (stopsAtKey_ && resting_) {
		// AT REST: neutral, whatever the wave — 0, which is also the bipolar centre in this output's terms.
		raw_ = 0.0f;
	} else if (isRandom(s.wave)) {
		// RANDOM WALK: the output depends on which step we're on, not on phase directly.
		// The step advances based on the rate. When synced, the step is derived from
		// the song position (deterministic). When free, it advances with the clock.
		// **AND `RandomGlide` IS THE SAME WALK READ BETWEEN ITS STEPS.** One line apart from `Random`
		// deliberately: the sequence, the seed, the rate and the step numbering are identical, so the
		// two waves are the same music and differ only in whether the value arrives all at once. That
		// is also what makes flipping between them a comparison rather than a new patch.
		//
		// IT REACHES EVERY TARGET EXACTLY, which is the whole reason it exists. A one-pole approaches
		// and never arrives, so turning the Smoothing knob far enough to kill the plateau cost depth —
		// MEASURED at 0.82 → 0.32 of span across the usable range (see `Waveform`). A ramp between two
		// known values has nothing to lose: it is at `randomWalkValue(step)` when the step opens and at
		// `randomWalkValue(step + 1)` when it closes, whatever the rate.
		const float target = randomWalkValue(s.seed, randomStep_ + 1);
		const float glided = randomValue_ + (target - randomValue_) * randomFrac_;
		raw_ = applyBipolar(s, s.wave == Waveform::RandomGlide ? glided : randomValue_);

		// **AND THE PLAYHEAD'S PHASE IS THE STEP'S FRACTION, NOT THE WAVEFORM'S.** `mod::Playhead`
		// means "how far through the thing `ordinal` names", and for a walk that thing is the STEP —
		// so reporting the shaped waves' `position_.phase` here was answering a question about a
		// different clock. The two agree while `Settings::phase` is 0 and a walk starts where the
		// wave does, which is why it went unnoticed; set a start phase and the scope's star sat at
		// one offset while the walk was at another. Nothing downstream of `advance` reads this for a
		// random wave — `Mode::Once` has already been decided by the time we get here — and the two
		// numbers advance at the same `hz` regardless, so this is a correction rather than a change.
		position_.phase = randomFrac_;
	} else {
		raw_ = eval(s, position_.phase);
	}

	// SMOOTHING. IIR lowpass — for random walk this turns steps into glides.
	//
	// **TWO THINGS WERE WRONG WITH THE LINE THIS REPLACES, AND THEY POINTED THE SAME WAY.** It read
	// `s.smoothingOn && s.smoothing > -10.0f`: a switch that made the knob inert (deleted 2026-09-19: turning up smooth with the toggle off was confusing), and a literal
	// ONE OCTAVE ABOVE this control's own declared minimum of -11, so the bottom octave of its travel
	// was silently dead while the box went on printing a time. Both failures were the knob moving and
	// nothing happening.
	//
	// AND THE COEFFICIENT IS THE EXACT POLE NOW. `T / (T + tau)` is the first term of
	// `1 - exp(-T/tau)`; they agree for a slow smoothing and part company where a fast one lives — at
	// tau = T, 0.5 against 0.632. The DAHDSR and the MSEG were already on the exact form; this device
	// was the odd one out, which is what `mod::smoothingCoeff` exists to stop.
	// DESIGNED ONLY WHEN THE KNOB MOVES (the DAHDSR's arrangement): the coefficient is an exp of the knob and the tick
	// length, and neither changes on most ticks. The same call, so the same bits.
	if (!smoothingCoeffValid_ || s.smoothing != smoothingKnobWas_) {
		smoothingKnobWas_ = s.smoothing;
		smoothingCoeffValid_ = true;
		smoothingCoeff_ = mod::smoothingCoeff(s.smoothing, tickSeconds_);
		++smoothingDesigns_;
	}
	//
	// **AND IT ARRIVES, IN DOUBLE** (the DAHDSR's bug, 2026-09-26, asked of this device). The one-pole was in
	// FLOAT, where a step of `c · diff` below half an ulp adds nothing: MEASURED, a Once ending in [0.5, 1) under 1 s
	// smoothing STALLED for ever 2.2e-5 short (c ≈ 1/750, so it stops ~750 half-ulps out), and one ending at 0
	// parked on the smallest denormal. In double the stall is far below the snap, which then lands it exactly —
	// `mod::smoothingStep`. While it moves the curve differs from the float one only by float rounding.
	mod::smoothingStep(smoothed_, static_cast<double>(raw_), smoothingCoeff_);
	value_ = static_cast<float>(smoothed_);
	// **THE RIGHT EAR** (backlog PA): the same wave, read `stereoLead` cycles on, through its own pole. At 0 it IS the left
	// ear — copied, not recomputed, so a mono LFO's arithmetic is the one above and nothing else.
	if (const float lead = s.stereoLead(); lead != 0.0f) {
		const float rawR = (stopsAtKey_ && resting_) ? 0.0f : rawRightAt(s, lead);
		mod::smoothingStep(smoothedR_, static_cast<double>(rawR), smoothingCoeff_);
		valueR_ = static_cast<float>(smoothedR_);
	}
	else {
		smoothedR_ = smoothed_;
		valueR_ = value_;
	}
	return value_;
}

float Shape::rawRightAt(const Settings& s, float lead) const noexcept {
	// A FINISHED ONE-SHOT HOLDS WHERE IT ENDED in both ears: the right ear, being ahead, finished first.
	if (s.mode == Mode::Once && position_.finished) {
		return raw_;
	}
	if (isRandom(s.wave)) {
		// THE SAME WALK, `lead` STEPS ALONG — a step is the walk's cycle, as the playhead's phase says. Its values are
		// `randomWalkValue(seed, step)`, so the right ear's step is the left's plus however many the lead carries it past,
		// and it reads the sequence the left ear will read, never a second draw.
		const float at = randomFrac_ + lead;
		const float whole = std::floor(at);
		const int step = randomStep_ + static_cast<int>(whole);
		const float value = randomWalkValue(s.seed, step);
		if (s.wave == Waveform::RandomGlide) {
			const float frac = at - whole;
			return applyBipolar(s, value + (randomWalkValue(s.seed, step + 1) - value) * frac);
		}
		return applyBipolar(s, value);
	}
	// A ONE-SHOT DOES NOT WRAP: the right ear reaches the end of the shape first and waits there.
	const float at = position_.phase + lead;
	return eval(s, s.mode == Mode::Once ? std::min(at, 1.0f) : at - std::floor(at));
}

void Shape::advanceRandom(const Settings& s, float hz, float rateMul) {
	if (s.mode == Mode::Once && position_.finished) {
		// AND A FINISHED ONE-SHOT HOLDS AT ITS STEP, not part-way to the next one. `randomFrac_` is
		// left wherever the last live tick put it, which for a glide would be a value between two
		// steps; the shape is over, so the honest reading is the step it ended on.
		randomFrac_ = 0.0f;
		return;
	}
	int step = 0;
	const size_t divIndex = (s.timeBase > 0) ? s.timeBase - 1 : 0;
	const double stepBeats = kDivisionBeats[std::min(divIndex, kDivisionCount - 1)] *
	                         timeSeconds(s.timeScale);
	// **THE SHAPED WAVES' RULE, AND FOR THE SAME TWO REASONS**. The step
	// was `floor(position / stepBeats)` every tick, so a route into Rate scrubbed the walk through steps
	// it had no business visiting — a Glide jumping instead of gliding — and with no `playing` test a
	// stopped host that still reports its position froze the walk on one step. Now the song's STEP is
	// followed (`mod::TransportLock`, its own, because the shaped road releases `lock_` outside Free)
	// and a stopped transport walks at the rate like an unsynced one.
	if (s.synced() && clock_.playing && clock_.havePositionBeats() && clock_.haveTempo()
	    && std::isfinite(stepBeats)) {
		if (stepBeats > 0.0) {
			double walked = randomLock_.follow(clock_.songPositionBeats, stepBeats,
			                                   tickSeconds_ / clock_.secondsPerBeat, 1);
			if (trigRestart_) {
				randomLock_.restartAt(0.0); // the walk restarts with the wave, against the song too (backlog MV)
				walked = 0.0;
			}
			step = static_cast<int>(std::floor(walked));
			// HOW FAR INTO THE STEP, which `RandomGlide` reads and `Random` ignores. Taken from the
			// song position rather than accumulated, exactly as the step number is, so a synced glide
			// is as deterministic as the walk it rides on.
			randomFrac_ = static_cast<float>(walked - std::floor(walked));
		}
	} else if (followsSongSeconds(s, clock_, hz, rateMul)) {
		// THE WALK IN HZ, FREE, PLAYING: its step from the song's seconds — the shaped waves' rule in `tick` (NJ).
		double walked = randomLock_.follow(clock_.songPositionSeconds,
		                                   1.0 / (static_cast<double>(hz) * static_cast<double>(rateMul)),
		                                   tickSeconds_, 2);
		if (trigRestart_) {
			randomLock_.restartAt(0.0); // as the synced walk above (backlog MV × NJ)
			walked = 0.0;
		}
		step = static_cast<int>(std::floor(walked));
		randomFrac_ = static_cast<float>(walked - std::floor(walked));
	} else {
		randomLock_.release();
		// **FREE-RUNNING: THE STEP ADVANCES AT THE LFO'S OWN RATE**, which is the whole of what the
		// Rate control means here. This read `step = randomStep_ + 1`, so it stepped on EVERY call —
		// a fresh value every 64-sample tick, 750 of them a second at 48 kHz, identically at every
		// Rate setting. That is not a slow random modulator, it is white noise with a dead knob, and
		// both parameters were named out of the signature (`float /*hz*/, float /*rateMul*/`) while
		// the caller went on computing them correctly.
		//
		// A PHASE OF ITS OWN, rather than reading `position_.phase`: that one is WRAPPED by `advance`
		// before this runs, so the moment a cycle completes is exactly the information it no longer
		// carries. Accumulating here keeps the wrap, and keeps the whole count when a very fast rate
		// crosses more than one step in a tick.
		const float increment = hz * rateMul * static_cast<float>(tickSeconds_);
		step = randomStep_;
		if (std::isfinite(increment) && increment > 0.0f) {
			randomPhase_ += increment;
			if (randomPhase_ >= 1.0f) {
				const float whole = std::floor(randomPhase_);
				randomPhase_ -= whole;
				step = randomStep_ + static_cast<int>(whole);
			}
		}
		// THE SAME FRACTION THE SYNCED ROAD TAKES FROM THE SONG — here it is the accumulator itself,
		// which is what `randomPhase_` already was. A rate of zero leaves it where it stands, so a
		// held walk holds at a POSITION rather than snapping to a step edge.
		randomFrac_ = randomPhase_;
		// A RATE OF ZERO HOLDS, and says so by arriving here with `step` unchanged. `timeSeconds` maps
		// the bottom of the travel to nothing at all (modulator.h), so 0 Hz is a position on the dial
		// rather than an error, and a walk that is not walking is the honest reading of it.
	}
	if (finishing_ && randomPrimed_ && step != randomStep_) {
		// A LOOP WHOSE KEY IS UP RUNS OUT ITS STEP, and rests when the next one would begin (see `advance`).
		rest();
		return;
	}
	if (step != randomStep_ || !randomPrimed_) {
		randomStep_ = step;
		randomValue_ = randomWalkValue(s.seed, step);
		randomPrimed_ = true;
	}
}

void Shape::advance(const Settings& s, float hz, float rateMul) {
	if (position_.finished) {
		return;
	}
	const float increment = hz * rateMul * static_cast<float>(tickSeconds_);
	float phase = position_.phase + increment;

	if (s.mode == Mode::Once) {
		// ONCE: run one cycle then stop.
		if (phase >= 1.0f) {
			position_.finished = true;
			position_.phase = 1.0f;
			return;
		}
	} else {
		// A LOOP WHOSE KEY IS UP ENDS WHERE ITS CYCLE BEGAN — one whole turn from the start phase (`finishing_`). A
		// random walk's cycle is its step, so it ends in `advanceRandom` instead.
		if (std::isfinite(increment) && increment > 0.0f) {
			cycleTravel_ += increment;
		}
		if (finishing_ && !isRandom(s.wave) && cycleTravel_ >= 1.0f) {
			position_.phase = s.phase - std::floor(s.phase);
			rest();
			return;
		}
		cycleTravel_ -= std::floor(cycleTravel_);
		// LOOPING MODES: wrap.
		phase = phase - std::floor(phase);
	}
	position_.phase = phase;
}

void Shape::rest() noexcept {
	resting_ = true;
	finishing_ = false;
	position_.finished = true; // the phase stands still, and the playhead says the wave is not running
}

void Shape::restart(const Settings& s) {
	++restarts_;
	position_.phase = s.phase;
	position_.finished = false;
	resting_ = false;
	finishing_ = false;
	cycleTravel_ = 0.0f;
	// AND THE WALK RESTARTS WITH IT, so a retriggered note hears the same sequence the last one did.
	// A random modulator that is deterministic PER NOTE is the useful kind — it is a shape you can
	// learn — and this is the same statement `position_.phase = s.phase` makes for every other wave.
	// Free mode reaches this once, on its first tick, which is where the walk gets its first value.
	randomStep_ = 0;
	randomPhase_ = 0.0f;
	randomFrac_ = 0.0f;
	randomPrimed_ = false;
}

Playhead Shape::playhead() const noexcept {
	Playhead p {};
	if (!position_.finished) {
		// **WHICH STEP THE WALK IS ON**, which for every other wave is 0 and always was. The card draws
		// the random walk as a scope of the real output (the card is meant to be an oscilloscope of what the actual
		// output is), and the walk is deterministic in seed,
		// step — so the step index is the whole of what the card needs to reproduce the trace
		// exactly, past included. `randomStep_` only ever moves for Random, so the shaped waves go on
		// reporting the 0 they reported before.
		//
		// Clamped because a synced walk takes its step from the song position, which runs negative
		// before bar one, and `kIdle` is the top of this field.
		const long step = static_cast<long>(randomStep_);
		p.ordinal = static_cast<uint32_t>(std::clamp<long>(step, 0, 0x7FFFFFFF));
		p.phase = position_.phase;
	}
	return p;
}

//------------------------------------------------------------------------------------------------//
// Row <-> Settings conversion                                                                    //
//------------------------------------------------------------------------------------------------//

Row rowOf(const Settings& s) {
	return Row {
		static_cast<float>(static_cast<int>(s.wave)),
		s.skewPrePeak,
		s.skewPreCross,
		s.skewPreTrough,
		static_cast<float>(s.seed),
		static_cast<float>(s.timeBase),
		s.timeScale,
		s.smoothing,
		static_cast<float>(static_cast<int>(s.mode)),
		s.phase,
		s.pingPong ? 1.0f : 0.0f,
		s.bipolar ? 1.0f : 0.0f,
		s.enabled ? 1.0f : 0.0f,
		s.amount,
		s.poly ? 1.0f : 0.0f,
		s.smoothCarry ? 1.0f : 0.0f,
		s.trig,
		s.notesTrig ? 1.0f : 0.0f,
		s.stereoPhase,
	};
}

namespace {

Settings decodeRow(const Row& row, size_t fieldsPresent) {
	const Settings d;
	const size_t fields = std::min(fieldsPresent, static_cast<size_t>(Param::kCount));
	size_t at = 0;

	const auto takeFloat = [&](float fallback) {
		if (at >= fields) return fallback;
		const float v = row[at++];
		return std::isfinite(v) ? v : fallback;
	};
	const auto takeFlag = [&](bool fallback) {
		if (at >= fields) return fallback;
		const float v = row[at++];
		return std::isfinite(v) ? v >= 0.5f : fallback;
	};
	const auto takeStepped = [&](float fallback, float min, float max) {
		if (at >= fields) return fallback;
		const float v = row[at++];
		if (!std::isfinite(v)) return fallback;
		return std::clamp(static_cast<float>(std::lround(v)), min, max);
	};

	Settings s;
	s.wave = static_cast<Waveform>(
	    std::clamp(static_cast<int>(takeStepped(static_cast<float>(static_cast<int>(d.wave)), 0,
	                                           static_cast<float>(Waveform::kCount) - 1.0f)),
	               0, static_cast<int>(Waveform::kCount) - 1));
	// 0..1, NOT 0..100 — see the descriptors. A stored row from before this reads its skews clamped
	// to 1.0, which is why the version byte moves with the range.
	s.skewPrePeak = std::clamp(takeFloat(d.skewPrePeak), 0.0f, 1.0f);
	s.skewPreCross = std::clamp(takeFloat(d.skewPreCross), 0.0f, 1.0f);
	s.skewPreTrough = std::clamp(takeFloat(d.skewPreTrough), 0.0f, 1.0f);
	s.seed = std::clamp(
	    static_cast<int>(takeStepped(static_cast<float>(d.seed), 0.0f, 999.0f)), 0, 999);
	s.timeBase = static_cast<size_t>(takeStepped(static_cast<float>(d.timeBase), 0, 200.0f));
	s.timeScale = takeFloat(d.timeScale);
	s.smoothing = takeFloat(d.smoothing);
	s.mode = static_cast<Mode>(std::clamp(
	    static_cast<int>(takeStepped(static_cast<float>(static_cast<int>(d.mode)), 0,
	                                 static_cast<float>(Mode::kCount) - 1.0f)),
	    0, static_cast<int>(Mode::kCount) - 1));
	s.phase = std::clamp(takeFloat(d.phase), 0.0f, 1.0f);
	s.pingPong = takeFlag(d.pingPong);
	s.bipolar = takeFlag(d.bipolar);
	s.enabled = takeFlag(d.enabled);
	s.amount = takeFloat(d.amount);
	s.poly = takeFlag(d.poly);
	s.smoothCarry = takeFlag(d.smoothCarry);
	s.trig = std::clamp(takeFloat(d.trig), 0.0f, 1.0f);
	s.notesTrig = takeFlag(d.notesTrig);
	s.stereoPhase = std::clamp(takeFloat(d.stereoPhase), 0.0f, static_cast<float>(M_PI));
	return s;
}

} // namespace

void sanitize(Settings* settings) {
	if (settings == nullptr) return;
	Row row = rowOf(*settings);
	for (size_t i = 0; i < row.size(); ++i) {
		if (!std::isfinite(row[i])) {
			row[i] = rowOf(defaults())[i];
		}
	}
	*settings = decodeRow(row, row.size());
	// RELATIONSHIPS. Skew knobs are independent (normalized at eval). Time base clamps.
	settings->timeBase = std::min(settings->timeBase, kNoteSyncBase);
}

Settings settingsOf(const Row& row, size_t fieldsPresent) {
	Settings s = decodeRow(row, fieldsPresent);
	sanitize(&s);
	return s;
}

float valueOf(const Settings& s, Param param) {
	return rowOf(s)[static_cast<size_t>(param)];
}

void setValueOf(Settings* settings, Param param, float value) {
	if (settings == nullptr) return;
	Row row = rowOf(*settings);
	row[static_cast<size_t>(param)] = value;
	*settings = settingsOf(row, row.size());
}

//------------------------------------------------------------------------------------------------//
// ModOps table                                                                                   //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE DEVICE'S STATE: its wave, poly or mono (`mod::PolyShape`), and its latched settings — the MSEG's
/// arrangement. Mono by default: one wave the patch breathes with; Poly gives each note its own.
using OpsShape = mod::PolyShape<Shape, Settings>;

OpsShape& asShape(void* storage) {
	return *static_cast<OpsShape*>(storage);
}

const OpsShape& asShape(const void* storage) {
	return *static_cast<const OpsShape*>(storage);
}

void setFieldsOf(void* storage, const float* fields, size_t fieldsPresent) {
	Row row {};
	const size_t take = std::min(fieldsPresent, row.size());
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	asShape(storage).settings = settingsOf(row, fieldsPresent);
}

} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(OpsShape),
	.stateAlign = alignof(OpsShape),
	.construct = [](void* storage) { new (storage) OpsShape(); },
	.destruct = [](void* storage) { asShape(storage).~OpsShape(); },
	.prepare = [](void* storage, double sampleRate) { asShape(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asShape(storage).reset(); },
	.setFields = setFieldsOf,
	.blockSamples = static_cast<size_t>(kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext& ctx) { asShape(storage).tick(ctx); },
	.outputAt = [](const void* storage, size_t output) {
		return output == 0 ? asShape(storage).sharedShape().value() : 0.0f;
	},
	// THE PER-VOICE ROAD — see the MSEG's, which this is.
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    return output == 0 ? asShape(storage).valueForNote(noteId, channel, key) : 0.0f;
	    },
	// **AND BOTH EARS** (backlog PA), asked only while Stereo Phase makes the output a pair (`SourceDesc::pairWhile`).
	.outputPairForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    if (output != 0) {
			    return device::EarPair {};
		    }
		    const Shape& shape = asShape(storage).shapeForNote(noteId, channel, key);
		    return device::EarPair { shape.value(), shape.valueRight() };
	    },
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const Playhead where = asShape(storage).sharedShape().playhead();
		    *ordinal = where.ordinal;
		    *phase = where.phase;
	    },
	// THE COLLAPSED PAIR — what a destination with no voice reads, the shape `outputAt` reads.
	.outputPairAt =
	    [](const void* storage, size_t output) {
		    if (output != 0) {
			    return device::EarPair {};
		    }
		    const Shape& shape = asShape(storage).sharedShape();
		    return device::EarPair { shape.value(), shape.valueRight() };
	    },
	.trigFires = [](const void* storage) { return asShape(storage).trigFires; },
};

/// THE NOTE LANE'S HALF — the MSEG's, which is the DAHDSR's.
const device::NoteOps kNoteOps {
	.stateBytes = sizeof(OpsShape),
	.stateAlign = alignof(OpsShape),
	.construct = [](void* storage) { new (storage) OpsShape(); },
	.destruct = [](void* storage) { asShape(storage).~OpsShape(); },
	.prepare = [](void* storage, double sampleRate) { asShape(storage).prepare(sampleRate); },
	.setFields = setFieldsOf,
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	       size_t outCapacity, size_t* outCount) {
		    asShape(storage).processNotes(in, inCount, out, outCapacity, outCount, false,
		                                  [](Shape& shape, const mod::PerNoteTable<Shape>::Note*) { shape.reset(); });
	    },
	.listensOnly = true,
};

} // namespace feathers::mod::lfo
