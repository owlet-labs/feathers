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

#include "mod/dahdsr/dahdsr.h"
#include "mod/per_note.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace feathers::mod::dahdsr {

//------------------------------------------------------------------------------------------------//
// THE MATHS — the curve law and the stage machine, both ours.                                     //
//------------------------------------------------------------------------------------------------//
//
// THE CURVE LAW. Every moving stage is one normalised curve f(x; s), x = how far through the stage
// (0..1), s = the stage's shape control (-1..1), aimed at the stage's own two levels:
//
//     f(x; s) = (1 - e^(-k·x)) / (1 - e^(-k)),        k = 8 · s · |s|,        f(x; 0) = x
//
//     attack   v = a0 + (1 - a0) · f(p; sA)           a0 = where the envelope stood when (re)started
//     decay    v = S  + (1 - S)  · f(1 - p; sD)       S  = the sustain level, read live
//     release  v = r0 · f(1 - p; sR)                  r0 = where the envelope stood when the gate fell
//
// f is the step response of a first-order system (an RC charge, dv/dt ∝ target - v) with its time
// axis cut at x = 1 and rescaled so the stage lands EXACTLY on its target instead of approaching it
// forever — the same normalised exponential SuperCollider's `Env` uses for a numeric segment curve
// (SuperCollider documentation, `Env` / `Env.new`, "curve": y = a + (b - a)(1 - e^(c·x)) / (1 - e^c),
// here with c = -k), and the textbook RC law (any circuits text; e.g. Horowitz & Hill, The Art of
// Electronics, ch. 1, RC circuits). Written with expm1 so it stays exact as k approaches the linear case.
//
// WHY THE STAGES ARE ONE FUNCTION MIRRORED, which is the convention `kShapeMax` states: positive is
// FULLER on every stage. f with k > 0 rises early (concave), so an attack with a positive shape gets
// up quickly; decay and release read it BACKWARDS (1 - p), so with the same sign they STAY HIGH late.
// One hand, three knobs, one meaning.
//
// WHY k = 8·s·|s|. The two constants were FITTED, black-box, against the envelope this replaced so the
// knobs keep the feel they had: 8 sets the extreme (at |s| = 1 the curve's slope differs end to end by
// e^8 ≈ 2981:1, ~69 dB, which is where "exponential" stops getting more exponential to the ear), and the
// s·|s| taper gives the middle of the knob fine control near the straight line while keeping the sign.
// MEASURED at the switch (2026-09-24, 44.1/48/96 kHz, shapes -1..1 in quarters, every stage): wherever
// the two machines were in the same stage on the same tick, they agreed to within 1e-5 of full scale
// (float rounding). What differs is TIMING, and only by the tick at a seam — see below. The tests pin
// this law directly, against a restatement of the formula, not against anything old.
//
// THE STAGE MACHINE. Delay -> Attack -> Hold -> Decay -> Sustain, and Release from ANY stage the moment
// the gate falls; Idle when the release lands. Each timed stage keeps `phase_` (0..1) and advances it by
// `seconds / stageSeconds` per tick, where stageSeconds is the knob's own exp2 (mod::timeSeconds), so a
// stage takes EXACTLY the time its knob says. The rules at the seams, each chosen for continuity:
//
//   - A STAGE WHOSE KNOB IS AT THE BOTTOM DOES NOT HAPPEN, and costs no time: the machine steps past it
//     and spends the tick on the next one (device.h's convention for every LogSeconds range).
//   - WHEN A STAGE ENDS INSIDE A TICK, THE TICK REPORTS THE STAGE'S END LEVEL — the corner — and the
//     UNUSED PART OF THE TICK IS CARRIED into the next stage as time (seconds, then converted to that
//     stage's phase). So every corner (the peak, the sustain level, the end of the release) is emitted
//     at least once, however short the stage, and the stage after it still starts on time to the
//     sample: no tick's worth of time is lost at a seam. One seam per tick; a run of stages shorter
//     than a tick each takes a tick.
//   - A (RE)START RUNS FROM WHERE THE ENVELOPE STANDS: the delay holds that level, the attack rises
//     from it to 1 over the FULL attack time. Retriggering from the sustain, from mid-release, or from
//     mid-attack is therefore continuous (f(0) = 0), which is what makes a retrigger click-free.
//   - A RELEASE STARTS FROM WHERE THE ENVELOPE STANDS, in any stage — delay, attack, hold, decay or
//     sustain — and falls to 0 over the FULL release time. Also continuous (f(1) = 1 at p = 0).
//   - Decay's floor is the sustain knob READ LIVE, so turning sustain mid-decay moves the curve rather
//     than stepping it, and the decay lands on whatever sustain is when it gets there.
//
// WHERE THIS DIFFERS FROM THE ENVELOPE IT REPLACED, measured rather than assumed: that one ended a stage
// ONE TICK LATE whenever the stage was a whole number of ticks long (100 ms at 48 kHz is exactly 75 ticks,
// and it spent 76), and at some seams dropped or mis-scaled the carried part of a tick. This one does
// neither, so a stage can now end up to one tick (1.33 ms at 48 kHz) SOONER than it used to — and a
// looping envelope whose stages sum to whole ticks is one tick per cycle shorter: 44 ms of stages loops
// at 44.0 ms rather than 45.3 ms at 48 kHz. The loop restart is decided at the top of the tick after the
// decay lands, and since 2026-10-19 it takes up what was left of that tick (`loopCarry_`), so a loop's
// period is its stage sum exactly; until then it was the sum rounded up to the tick grid.
//
// A PHASE WITHIN 1e-6 OF ITS END IS AT ITS END. The card chooses `rateMul` so a stage advances exactly
// 1/48 per tick, and 48 such steps, through a float rateMul, land a hair either side of 1; without the
// tolerance, half the time the stage would end one tick late, on the NEXT segment's budget. A millionth
// of a stage is 10 µs of a 10 s release, well under a sample.

namespace {

constexpr double kPhaseEndTolerance = 1e-6;

double stageLengthSeconds(const Settings& s, Stage stage) {
	switch (stage) {
	case Stage::Delay:
		return mod::timeSeconds(s.delay);
	case Stage::Attack:
		return mod::timeSeconds(s.attack);
	case Stage::Hold:
		return mod::timeSeconds(s.hold);
	case Stage::Decay:
		return mod::timeSeconds(s.decay);
	case Stage::Release:
		return mod::timeSeconds(s.release);
	case Stage::Idle:
	case Stage::Sustain:
		break;
	}
	return 0.0;
}

/// The stage that follows `stage` while the gate is up. Release is not in this chain — it is entered
/// from anywhere by the gate falling — and it is followed by Idle.
Stage nextStage(Stage stage) {
	switch (stage) {
	case Stage::Delay:
		return Stage::Attack;
	case Stage::Attack:
		return Stage::Hold;
	case Stage::Hold:
		return Stage::Decay;
	case Stage::Decay:
		return Stage::Sustain;
	case Stage::Release:
	case Stage::Idle:
		return Stage::Idle;
	case Stage::Sustain:
		break;
	}
	return Stage::Sustain;
}

bool timed(Stage stage) {
	return stage != Stage::Idle && stage != Stage::Sustain;
}

float sustainLevel(const Settings& s) {
	return std::isfinite(s.sustain) ? std::clamp(s.sustain, 0.0f, 1.0f) : 0.0f;
}

} // namespace

double stageCurve(double x, float shape) noexcept {
	const double p = std::clamp(std::isfinite(x) ? x : 0.0, 0.0, 1.0);
	const double sh = std::isfinite(shape)
	                      ? std::clamp(static_cast<double>(shape), -static_cast<double>(kShapeMax),
	                                   static_cast<double>(kShapeMax))
	                      : 0.0;
	const double k = 8.0 * sh * std::abs(sh);
	// THE LINEAR LIMIT, taken explicitly: (1 - e^(-kx)) / (1 - e^(-k)) -> x as k -> 0, and the ratio of
	// two tiny expm1s is where rounding would otherwise live. At |k| < 1e-9 the curve differs from the
	// line by under 1e-10.
	if (std::abs(k) < 1e-9) {
		return p;
	}
	return std::expm1(-k * p) / std::expm1(-k);
}


double stageSeconds(float log2Seconds) {
	// THE BODY MOVED TO mod::timeSeconds — see mod/modulator.h. It is unchanged, including the
	// at-the-minimum-means-off special case; what changed is that the MSEG needs the same decode and
	// must not have a second copy of it. This name stays because a stage is what a DAHDSR calls it.
	return mod::timeSeconds(log2Seconds);
}

float stageValueFor(double seconds) {
	return mod::timeValueFor(seconds);
}

double smoothingSeconds(float log2Seconds) {
	if (!std::isfinite(log2Seconds)) {
		return std::exp2(static_cast<double>(kSmoothingLog2SecondsMin));
	}
	return std::exp2(std::clamp(static_cast<double>(log2Seconds),
	                            static_cast<double>(kSmoothingLog2SecondsMin),
	                            static_cast<double>(kSmoothingLog2SecondsMax)));
}

Row rowOf(const Settings& s) {
	// The switches ride as floats so the row is one kind of thing and its length is one
	// multiplication; a byte-packed struct here would be a place for two builds to disagree about
	// padding. Index-aligned with kParamDescs, which the static_assert below pins.
	return Row { s.delay,
		         s.attack,
		         s.hold,
		         s.decay,
		         s.sustain,
		         s.release,
		         s.attackShape,
		         s.decayShape,
		         s.releaseShape,
		         s.smoothing,
		         static_cast<float>(static_cast<int>(s.mode)),
		         s.singleTrigger ? 1.0f : 0.0f,
		         s.bipolar ? 1.0f : 0.0f,
		         s.enabled ? 1.0f : 0.0f,
		         s.amount,
		         s.poly ? 1.0f : 0.0f,
		         s.smoothCarry ? 1.0f : 0.0f,
		         s.trig,
		         s.notesTrig ? 1.0f : 0.0f };
}

namespace {
/// THE ROW DECODED AND NOTHING ELSE — no clamping, because sanitize() is written in terms of THIS
/// and calling it from here would recurse. The split is what lets the field order exist once while
/// both the decode and the clamp walk it.
Settings decodeRow(const Row& row, size_t fieldsPresent) {
	const Settings defaults;
	const size_t fields = std::min(fieldsPresent, static_cast<size_t>(Param::kCount));
	size_t at = 0;

	// Every reader walks the SAME cursor, so however short the row turns out to be the fields cannot
	// get out of step with each other.
	const auto takeFloat = [&](float fallback) {
		if (at >= fields) {
			return fallback;
		}
		const float v = row[at++];
		return std::isfinite(v) ? v : fallback;
	};
	const auto takeFlag = [&](bool fallback) {
		if (at >= fields) {
			return fallback;
		}
		const float v = row[at++];
		return std::isfinite(v) ? v >= 0.5f : fallback;
	};
	const auto takeMode = [&](Mode fallback) {
		if (at >= fields) {
			return fallback;
		}
		const float v = row[at++];
		if (!std::isfinite(v)) {
			return fallback;
		}
		// CLAMPED INTO THE ENUM, never cast raw. A value outside it — which a hand-edited file can
		// carry — would be a Mode with no case in any switch, and every consumer would find that
		// out at a different moment.
		const int index = static_cast<int>(v + 0.5f);
		if (index < 0 || index >= static_cast<int>(Mode::kCount)) {
			return fallback;
		}
		return static_cast<Mode>(index);
	};

	Settings s;
	s.delay = takeFloat(defaults.delay);
	s.attack = takeFloat(defaults.attack);
	s.hold = takeFloat(defaults.hold);
	s.decay = takeFloat(defaults.decay);
	s.sustain = takeFloat(defaults.sustain);
	s.release = takeFloat(defaults.release);
	s.attackShape = takeFloat(defaults.attackShape);
	s.decayShape = takeFloat(defaults.decayShape);
	s.releaseShape = takeFloat(defaults.releaseShape);
	s.smoothing = takeFloat(defaults.smoothing);
	s.mode = takeMode(defaults.mode);
	s.singleTrigger = takeFlag(defaults.singleTrigger);
	s.bipolar = takeFlag(defaults.bipolar);
	s.enabled = takeFlag(defaults.enabled);
	s.amount = takeFloat(defaults.amount);
	s.poly = takeFlag(defaults.poly);
	s.smoothCarry = takeFlag(defaults.smoothCarry);
	s.trig = takeFloat(defaults.trig);
	s.notesTrig = takeFlag(defaults.notesTrig);
	return s;
}
} // namespace

Settings settingsOf(const Row& row, size_t fieldsPresent) {
	Settings s = decodeRow(row, fieldsPresent);
	sanitize(&s);
	return s;
}

float valueOf(const Settings& settings, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	// READ OFF THE ROW rather than off a second switch over the same fields — the row IS this
	// device's indexed view of itself, and a switch beside it would be the second copy of the field
	// order that rowOf's comment refuses.
	return rowOf(settings)[index];
}

void setValueOf(Settings* settings, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (settings == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	if (!std::isfinite(value)) {
		return;
	}

	// CLAMPED AND ROUNDED BY THE CONTROL'S OWN DECLARATION — ParamSet::setBase's rule, and it has to
	// be the same rule: this is the door a card writes through, and a card is exactly the thing that
	// should not have to know a control's range to move it.
	//
	// A first draft skipped this and let the value fall through to settingsOf, whose job is reading a
	// FILE — so an out-of-range choice landed on the DEFAULT rather than on the nearest setting that
	// exists, and a knob dragged past its end would have snapped back to the middle. Right for a
	// corrupt byte, wrong for a hand.
	const device::ParamDesc& desc = kParamDescs[index];
	double v = std::clamp(static_cast<double>(value), desc.min, desc.max);
	if (desc.stepped) {
		v = std::round(v);
	}

	Row row = rowOf(*settings);
	row[index] = static_cast<float>(v);
	// Back through the same door a file comes through, so the switches and the choice are decoded
	// once rather than twice, and a bool set from 0.4 cannot exist.
	*settings = settingsOf(row, static_cast<size_t>(Param::kCount));
}

void sanitize(Settings* settings) {
	if (settings == nullptr) {
		return;
	}
	// CLAMPED BY THE DECLARATIONS, not by bounds written again here. Every continuous field's range
	// is already stated in kParamDescs — a card reads it to draw the control, the host reads it to
	// scale a modulation amount — so a second copy in this function would be a third place for a
	// range to be wrong, and the one most likely to be forgotten when a range moves.
	//
	// Walked by index, which is what makes it exhaustive by construction: a field APPENDED to the
	// device is sanitised without anyone remembering to come here. The switches and the mode are
	// left to settingsOf, which decodes them; clamping a bool is meaningless.
	Row row = rowOf(*settings);
	for (size_t i = 0; i < row.size(); ++i) {
		const device::ParamDesc& desc = kParamDescs[i];
		if (desc.stepped) {
			// The switches and the mode are decoded rather than clamped — decodeRow already refuses
			// a Mode outside the enum, and clamping a bool means nothing.
			continue;
		}
		row[i] = std::isfinite(row[i])
		             ? std::clamp(row[i], static_cast<float>(desc.min), static_cast<float>(desc.max))
		             // A NON-NUMBER IS NOT A SETTING SOMEBODY MEANT, so it goes to the declared
		             // default rather than to an end of the range — a corrupt byte should not read as
		             // a deliberate extreme.
		             : static_cast<float>(desc.def);
	}
	*settings = decodeRow(row, row.size());
}

void Envelope::continueFrom(const Envelope& other) noexcept {
	if (&other == this) {
		return;
	}
	*this = other;
}

void Envelope::prepare(double sampleRate) {
	const double rate = (std::isfinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kBlockSamples) / rate;
	// The coefficient is a function of the tick length, so the cache it was computed under is stale.
	smoothingKnobWas_ = -1.0f;
	reset();
}

void Envelope::reset() {
	stage_ = Stage::Idle;
	phase_ = 0.0;
	attackFrom_ = 0.0f;
	releaseFrom_ = 0.0f;
	smoothed_ = 0.0;
	raw_ = 0.0f;
	value_ = 0.0f;
	gateWas_ = false;
	onceLetGo_ = false;
	trigPending_ = false;
	loopCarry_ = 0.0;
	lock_.release();
	lockedTurns_ = 0.0;
	lockedCycle_ = 0.0;
	locked_ = false;
}

void Envelope::start(float from) noexcept {
	stage_ = Stage::Delay;
	phase_ = 0.0;
	attackFrom_ = std::isfinite(from) ? std::clamp(from, 0.0f, 1.0f) : 0.0f;
}

void Envelope::advance(const Settings& s, double seconds) noexcept {
	// ZERO-LENGTH STAGES ARE STEPPED PAST WITHOUT SPENDING TIME, so this loop turns at most once per
	// stage in the chain before it either lands inside a stage or completes one — see the header of
	// this file for the seam rules it implements.
	for (int guard = 0; guard < 8; ++guard) {
		if (stage_ == Stage::Idle) {
			raw_ = 0.0f;
			return;
		}
		if (stage_ == Stage::Sustain) {
			raw_ = sustainLevel(s);
			return;
		}
		const double length = stageLengthSeconds(s, stage_);
		if (!(length > 0.0)) {
			// A STAGE THAT DOES NOT HAPPEN. Its end level is reached at once; the tick's time goes on to
			// the next stage untouched.
			stage_ = nextStage(stage_);
			phase_ = 0.0;
			continue;
		}
		phase_ += seconds / length;
		if (phase_ < 1.0 - kPhaseEndTolerance) {
			raw_ = levelAt(s, stage_, std::max(phase_, 0.0));
			return;
		}

		// THE STAGE ENDS INSIDE THIS TICK: report its corner, carry the rest of the tick as time.
		const double carrySeconds = std::max(0.0, (phase_ - 1.0) * length);
		switch (stage_) {
		case Stage::Delay:
			raw_ = attackFrom_;
			break;
		case Stage::Attack:
		case Stage::Hold:
			raw_ = 1.0f;
			break;
		case Stage::Decay:
			raw_ = sustainLevel(s);
			break;
		case Stage::Release:
		case Stage::Idle:
		case Stage::Sustain:
			raw_ = 0.0f;
			break;
		}
		stage_ = nextStage(stage_);
		phase_ = 0.0;
		// Past any stage that does not happen, so the carry lands in one that does. Their end levels are
		// what the NEXT tick will start from, and the corner this tick reported is the stage that ended.
		while (timed(stage_) && !(stageLengthSeconds(s, stage_) > 0.0)) {
			stage_ = nextStage(stage_);
		}
		if (timed(stage_)) {
			phase_ = carrySeconds / stageLengthSeconds(s, stage_);
		}
		else if (stage_ == Stage::Sustain) {
			loopCarry_ = carrySeconds; // a loop's restart next tick takes it up (`tick`); nothing else reads it
		}
		return;
	}
}

float Envelope::levelAt(const Settings& s, Stage stage, double p) const noexcept {
	switch (stage) {
	case Stage::Delay:
		return attackFrom_;
	case Stage::Attack:
		return static_cast<float>(attackFrom_ + (1.0 - attackFrom_) * stageCurve(p, s.attackShape));
	case Stage::Hold:
		return 1.0f;
	case Stage::Decay: {
		const double floor = sustainLevel(s);
		return static_cast<float>(floor + (1.0 - floor) * stageCurve(1.0 - p, s.decayShape));
	}
	case Stage::Release:
		return static_cast<float>(releaseFrom_ * stageCurve(1.0 - p, s.releaseShape));
	case Stage::Idle:
	case Stage::Sustain:
		break;
	}
	return raw_;
}

void Envelope::followSong(const Settings& s, const TransportClock& clock, double cycle, double window,
                          bool fired) noexcept {
	double turns = lock_.follow(clock.songPositionSeconds, window, tickSeconds_, 2);
	if (fired) {
		lock_.restartAt(0.0); // a Trig's restart holds against the song (backlog MV), as the LFO's does
		turns = 0.0;
	}
	const double whole = std::floor(turns);
	// THE SONG MOVED FORWARD BY NO MORE THAN THE LOCK'S OWN JUMP WINDOW: the ticks are contiguous, so a stage end
	// between the two readings was passed through and is reported as a corner, as the state machine reports one.
	const bool contiguous = locked_ && !fired && turns >= lockedTurns_
	                        && (turns - lockedTurns_) * window <= TransportLock::kJumpTicks * tickSeconds_;

	// WHERE A CYCLE STARTS ITS ATTACK. The steady loop's is the sustain level (a looped restart starts from where the
	// decay landed), and a cycle read off the song is always a steady one, which is what makes a DAHDSR added mid-song
	// land where one that played through stands. A Trig restarts from where the envelope stands, as everywhere else in
	// this device; the cycle after it is steady again.
	if (fired) {
		attackFrom_ = raw_;
	}
	else if (!locked_ || whole != lockedCycle_) {
		attackFrom_ = sustainLevel(s);
	}

	// THE STAGE AND HOW FAR THROUGH IT, at `u` envelope seconds into the cycle. Written into the machine's own state,
	// so a transport that stops hands the free-running machine the shape where the song left it.
	const double u = std::clamp((turns - whole) * cycle, 0.0, cycle);
	constexpr Stage kCycle[] = { Stage::Delay, Stage::Attack, Stage::Hold, Stage::Decay };
	double begin = 0.0;
	stage_ = Stage::Decay;
	phase_ = 1.0;
	for (const Stage stage : kCycle) {
		const double length = stageLengthSeconds(s, stage);
		if (length > 0.0 && u < begin + length) {
			stage_ = stage;
			phase_ = (u - begin) / length;
			break;
		}
		begin += std::max(length, 0.0);
	}
	raw_ = levelAt(s, stage_, phase_);

	// **A STAGE THAT ENDED SINCE THE LAST TICK REPORTS ITS CORNER** — the machine's seam rule (see the top of this
	// file), so a peak shorter than a tick is still emitted rather than stepped over. The first end passed wins, as
	// the machine reports the stage that ended. Bounded: a contiguous step covers at most `kJumpTicks` ticks.
	if (contiguous) {
		const double from = lockedTurns_ * cycle;
		const double to = turns * cycle;
		bool found = false;
		for (double k = std::floor(lockedTurns_); !found && k <= whole && k - std::floor(lockedTurns_) < 8.0; k += 1.0) {
			double end = k * cycle;
			for (const Stage stage : kCycle) {
				const double length = stageLengthSeconds(s, stage);
				if (!(length > 0.0)) {
					continue;
				}
				end += length;
				if (end > from && end <= to) {
					raw_ = (stage == Stage::Delay)   ? attackFrom_
					       : (stage == Stage::Decay) ? sustainLevel(s)
					                                 : 1.0f;
					found = true;
					break;
				}
			}
		}
	}

	lockedTurns_ = turns;
	lockedCycle_ = whole;
	locked_ = true;
	loopCarry_ = 0.0;
}

float Envelope::tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
                     float rateMul) {
	const double rate = std::isfinite(rateMul) && rateMul > 0.0f ? static_cast<double>(rateMul) : 1.0;

	// **FREE ON A PLAYING TRANSPORT IS SONG-LOCKED** (on backlog NM, which OVERRIDES the
	// 2026-08-24 "accept no sync for now" call). The loop is a clock, and the system-wide rule is that what can be derived
	// from the song is derived rather than stored, so a reload, a bounce and a second pass all play the same shape. So
	// the cycle is read off the song's SECONDS, as the LFO's Free in Hz is (backlog NJ): the stage sum is the period,
	// a tempo change does not move it (the stages are durations, not divisions), and a DAHDSR added mid-song lands
	// where one that played through stands. ACCEPTED, by the same rule: a play start or a playhead jump STEPS the
	// output to where the song says the cycle is. Stopped, it runs free on the state machine as before, from where
	// the song left it. Only Free: LFO Retrig restarts on notes, Loop on its gate, as with the LFO and the MSEG.
	// Free is never per note (`mod::polyRuns`), so this is the collapsed envelope every note reads.
	const double cycle = stageLengthSeconds(settings, Stage::Delay) + stageLengthSeconds(settings, Stage::Attack)
	                     + stageLengthSeconds(settings, Stage::Hold) + stageLengthSeconds(settings, Stage::Decay);
	if (settings.mode == Mode::Free && clock.playing && clock.havePositionSeconds() && std::isfinite(cycle)
	    && cycle > 0.0) {
		followSong(settings, clock, cycle, cycle / rate, std::exchange(trigPending_, false));
		gateWas_ = true;
		onceLetGo_ = false;
	}
	else {
		lock_.release();
		locked_ = false;
		runMachine(settings, gate, retrigger, rate);
	}

	// **ONE BRANCH NOW, BECAUSE "OFF" IS A COEFFICIENT OF 1 RATHER THAN A DIFFERENT ROAD.** The
	// switch is deleted and the bottom of the travel carries it; `mod::smoothingCoeff`
	// returns 1 there, and a one-pole stepped by 1 IS the pass-through the old `else` branch wrote by
	// hand — including its "kept in step" property, which was the only reason that branch existed.
	// **AND IT NO LONGER BOTTOMS OUT AT 0.49 ms.** `smoothingSeconds` CLAMPS to
	// `kSmoothingLog2SecondsMin` and returns `exp2(-11)`, so with the switch gone the knob's lowest
	// position would have been a fast smoothing rather than none — the switch was hiding that the
	// range had no off in it. Still exported for the card, which wants the number to print.
	if (settings.smoothing != smoothingKnobWas_) {
		smoothingKnobWas_ = settings.smoothing;
		smoothingCoeff_ = mod::smoothingCoeff(settings.smoothing, tickSeconds_);
	}
	// **WITHIN A MILLIONTH OF ITS TARGET, IT IS ITS TARGET**. A one-pole only approaches: after the release ended the output crawled
	// toward zero for ever, so nothing it drove ever got back to its floor and `idle()` could not honestly
	// say "done". The step and its snap are `mod::smoothingStep`, the one line all three modulators share.
	mod::smoothingStep(smoothed_, static_cast<double>(raw_), smoothingCoeff_);

	// BIPOLAR IS A RESCALE OF THE WHOLE RANGE, which is Bitwig's own wording — the minimum becomes
	// -1 and the middle becomes 0. So an idle bipolar envelope reads -1 rather than 0, and a route
	// from one is pushing its destination down while nothing is playing. That is the meaning of the
	// switch rather than a defect of it, and it is why the switch is off by default.
	value_ = settings.bipolar ? static_cast<float>(2.0 * smoothed_ - 1.0)
	                          : static_cast<float>(smoothed_);
	return value_;
}

void Envelope::runMachine(const Settings& settings, bool gate, bool retrigger, double rate) noexcept {
	// WHAT STARTS IT. A rising gate always does — that is what a gate is.
	//
	// THE MODE DECIDES WHAT THE GATE MEANS. Gate and Loop hear it — the envelope runs while it is
	// held and stops when it falls. Free and LFO Retrig ignore it entirely (`effectiveGate = true`),
	// which is what makes them LFOs. Once fires one cycle and ignores everything after.
	const bool free = settings.mode == Mode::Free || settings.mode == Mode::LfoRetrig;
	const bool effectiveGate = free || gate;

	const bool rising = effectiveGate && !gateWas_;

	// THE TWO RESTARTS ARE GOVERNED SEPARATELY, because the modes disagree about them separately.
	//
	//   - A LOOP'S restart is refused by Once, where "plays once" and "repeats forever" are a
	//     contradiction in the two words. FREE MUST KEEP IT: a free-running envelope that could not
	//     loop would run its shape once and sit on its sustain forever, which is not an LFO — and the
	//     LFO is the entire reason the mode exists.
	//   - A NOTE'S restart belongs to Gate, Loop and LFO Retrig. Free does not hear notes at all, and
	//     Once is the mode whose whole content is that a second note changes nothing.
	const bool mayLoop =
	    (settings.mode == Mode::Loop || settings.mode == Mode::Free || settings.mode == Mode::LfoRetrig);
	//   - **AND "LEG" OUTRANKS ALL OF THEM**, which is the half this lost when the three old toggles
	//     became one Mode. Single Trigger says a note played under a held one does not restart the
	//     shape, and a key STRUCK AGAIN while still held is the plainest case of that — the note-on
	//     site says so in as many words ("Retrig restarts from where it stands; Single Trigger and
	//     One-shot do not") and then set `retrigger` anyway, so the comment described the rule and
	//     nothing implemented it. One-shot was excluded by the mode test above and Leg was not.
	//
	//     IT SHOWS ONLY WHERE THE SAME KEY COMES BACK WITHOUT A NOTE ID: with ids the re-press is a
	//     different note and takes the legato path below, so the suite was red on exactly three of
	//     its twelve combinations — the keyboard ones.
	//   - **AND "NOTES FIRE TRIG" OFF REFUSES BOTH NOTE STARTS** (backlog NU; the rule per mode is
	//     `mod::notesTrigParamDesc`'s): a key going down neither starts nor retriggers. The gate is still heard —
	//     a key up still releases — and Free's and LFO Retrig's own first start is not a note, so it stands.
	const bool notes = notesFireTrig(settings);
	const bool mayRetrigger =
	    notes && !settings.singleTrigger
	    && (((settings.mode == Mode::Gate || settings.mode == Mode::Loop) && retrigger)
	        || (settings.mode == Mode::LfoRetrig && retrigger));

	// **A TRIG (backlog MV) RESTARTS IT WHEREVER THE MODE LETS THE SHAPE RUN** — see `fire`. Once is the case the gate
	// cannot express: a fire with no key held runs the whole shot, which is what a one-shot envelope on a trigger is.
	const bool fired = std::exchange(trigPending_, false);
	const bool trigStarts = fired && (effectiveGate || settings.mode == Mode::Once);

	const bool looped = mayLoop && effectiveGate && !rising && stage() == Stage::Sustain;
	if (trigStarts || (effectiveGate && ((rising && (notes || free)) || looped || mayRetrigger))) {
		start(raw_);
		onceLetGo_ = false; // a fresh shot asks its own question about when to let go
	}

	// **ONE-SHOT LETS GO OF THE GATE BY ITSELF, WHICH IS WHAT GIVES IT A DURATION** (decided
	// 2026-09-19). Every other mode's end is the note's doing: this envelope's last stage IS the
	// release, and the release is what note-off MEANS, so "one full cycle then holds at the end" —
	// which the MSEG and the LFO deliver plainly, both having shapes of fixed length — had no
	// referent here at all. What Once quietly became instead was Gate minus retrigger minus loop:
	// MEASURED before this, holding at Sustain 0.7 under a held key and falling to Idle 300 ms after
	// it came up, which is a normal envelope that merely refuses to re-fire.
	//
	// SO IT STOPS ASKING. The latch trips the moment the shape ARRIVES at sustain, and from then on
	// the state machine is told the gate is down — the release runs straight out of the decay and the whole shape
	// has one length whatever the keyboard does. Note-off during the attack is ignored for the same
	// reason, which is the other half of the mode's own sentence.
	//
	// **SUSTAIN STAYS LIVE AND IS NOT GREYED OUT**, which is the opposite of what was expected when
	// this was agreed. The decay falls TOWARD the sustain level and the release falls FROM it, so
	// under Once that control is the split between the two halves of the tail — at 100% the decay
	// does nothing and the release is the whole shape, at 0% the reverse. A control that still moves
	// the sound is a control the honesty rule requires to stay reachable.
	if (settings.mode == Mode::Once && stage() == Stage::Sustain) {
		onceLetGo_ = true;
	}
	const bool gateForMachine = (settings.mode == Mode::Once) ? !onceLetGo_ : effectiveGate;

	// THE GATE FALLING RELEASES FROM WHEREVER THE SHAPE STANDS — any stage, delay included — and the
	// release then runs its full time from that level, which is what keeps a short note click-free.
	if (!gateForMachine && stage_ != Stage::Idle && stage_ != Stage::Release) {
		stage_ = Stage::Release;
		phase_ = 0.0;
		releaseFrom_ = raw_;
	}

	// One value per tick. The engine reads this at 750 Hz and every destination it reaches is itself
	// resolved per tick. The rate multiplier is 1 at every engine call site; a card scales it per
	// stage to draw. See tick() in the header.
	//
	// **A LOOP'S RESTART TAKES UP THE PART OF THE LAST TICK THE DECAY DID NOT USE** (2026-10-19, with the song lock).
	// The restart is decided here, a tick after the decay landed on Sustain, and that tick's remainder used to be
	// dropped, so a cycle cost its stage sum ROUNDED UP to the tick grid: a drift of up to a tick per cycle (44.99 ms
	// for 44 ms of stages at 44.1 kHz) against a song-locked cycle that is the sum exactly. With the carry the free
	// loop is the sum too, so a transport that stops hands over to a loop running at the rate the song's did.
	const double carry = looped ? loopCarry_ : 0.0;
	loopCarry_ = 0.0;
	advance(settings, tickSeconds_ * rate + carry);
	// **`gateWas_` TRACKS THE NOTE, NOT WHAT THE STATE MACHINE WAS TOLD.** It is what `rising` is
	// computed from, so storing `gateForMachine` here would make a one-shot's own release look like a
	// note-off and the next tick's gate look like a fresh note — the envelope would re-fire itself
	// forever off its own latch.
	gateWas_ = effectiveGate;
}

mod::Playhead Envelope::playhead() const noexcept {
	const Stage now = stage();
	if (now == Stage::Idle) {
		return {};
	}
	// SUSTAIN HOLDS RATHER THAN ADVANCING — see the declaration for why that is 0.
	const float phase = (now == Stage::Sustain)
	                        ? 0.0f
	                        : static_cast<float>(std::clamp(phase_, 0.0, 1.0));
	return { static_cast<uint32_t>(now), phase };
}

} // namespace feathers::mod::dahdsr

//------------------------------------------------------------------------------------------------//
// THE MODULATOR OPS TABLE — how a host advances this device without knowing it is a DAHDSR.        //
//------------------------------------------------------------------------------------------------//

namespace feathers::mod::dahdsr {

namespace {

/// THE DEVICE'S STATE: its envelope, poly or mono (`mod::PolyShape`), and the settings it latched from `setFields`
/// — latched rather than decoded per tick, because a host delivers fields far less often than it ticks.
///
/// **ONE NOTE'S OWN ENVELOPE** is where this began — backlog EF: under Poly every note runs its
/// own, and a new note leaves the last note's tail alone. **AND ONE FOR THE DEVICE UNDER MONO** (2026-10-03:
/// a Poly switch, and no dual path logic): every destination reads the one envelope the device is running.
using OpsEnvelope = mod::PolyShape<Envelope, Settings>;

OpsEnvelope& asEnvelope(void* storage) {
	return *static_cast<OpsEnvelope*>(storage);
}
const OpsEnvelope& asEnvelope(const void* storage) {
	return *static_cast<const OpsEnvelope*>(storage);
}

void setFieldsOf(void* storage, const float* fields, size_t fieldsPresent) {
	// THE DEVICE DECODES ITS OWN ROW, which is what puts absent fields at their DECLARED defaults rather than at
	// zero — device_row.h's rule, arriving as a bare pointer and a count because the vocabulary cannot see the
	// host's DeviceRow.
	Row row {};
	const size_t take = fieldsPresent < row.size() ? fieldsPresent : row.size();
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	asEnvelope(storage).settings = settingsOf(row, fieldsPresent);
}

} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(OpsEnvelope),
	.stateAlign = alignof(OpsEnvelope),
	.construct = [](void* storage) { new (storage) OpsEnvelope(); },
	.destruct = [](void* storage) { asEnvelope(storage).~OpsEnvelope(); },
	.prepare = [](void* storage, double sampleRate) { asEnvelope(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asEnvelope(storage).reset(); },
	.setFields = setFieldsOf,
	.blockSamples = static_cast<size_t>(kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext& ctx) { asEnvelope(storage).tick(ctx); },
	// ONE OUTPUT, and anything else answers zero rather than reading past the end. `kOutputs` is what
	// says how many there are; this must agree with it and the static_assert below is what makes that
	// a requirement rather than a hope. A DESTINATION WITH NO VOICE reads the mono envelope, or under Poly the
	// most recently struck note's.
	.outputAt = [](const void* storage,
	               size_t output) { return output == 0 ? asEnvelope(storage).sharedShape().value() : 0.0f; },
	// THE PER-VOICE ROAD (backlog EF): under Poly the note's own envelope, under mono the one envelope.
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    return output == 0 ? asEnvelope(storage).valueForNote(noteId, channel, key) : 0.0f;
	    },
	// WHERE THE ENVELOPE IS, so a host that holds DEVICES can draw the star — the envelope `outputAt` reads.
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const mod::Playhead where = asEnvelope(storage).sharedShape().playhead();
		    *ordinal = where.ordinal;
		    *phase = where.phase;
	    },
	// AND WHERE IT STANDS, because the card draws the release from the sustain level and the attack from zero, and a
	// release from mid-attack (or a re-attack from where it stood) is on neither line (`mod::Playhead::level`).
	.playheadLevel = [](const void* storage) { return asEnvelope(storage).sharedShape().rawValue(); },
	.trigFires = [](const void* storage) { return asEnvelope(storage).trigFires; },
};

/// THE NOTE LANE'S HALF OF THE SAME DEVICE (backlog EF) — the note expressions' shape: one storage, two
/// tables, and every event passes through. Its `setFields` is the modulator's, so the per-note envelopes
/// run the settings the card shows.
const device::NoteOps kNoteOps {
	.stateBytes = sizeof(OpsEnvelope),
	.stateAlign = alignof(OpsEnvelope),
	.construct = [](void* storage) { new (storage) OpsEnvelope(); },
	.destruct = [](void* storage) { asEnvelope(storage).~OpsEnvelope(); },
	.prepare = [](void* storage, double sampleRate) { asEnvelope(storage).prepare(sampleRate); },
	.setFields = setFieldsOf,
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	       size_t outCapacity, size_t* outCount) {
		    // **A NEW ENVELOPE: fresh, or — under Legato (Single Trigger) — the one already held** (backlog FG). "Leg"
		    // means a note played while another is held does not restart the shape; per note, that is the new
		    // note's envelope TAKING UP the newest held one rather than starting from nothing.
		    OpsEnvelope& e = asEnvelope(storage);
		    e.processNotes(in, inCount, out, outCapacity, outCount, e.settings.singleTrigger,
		                   [](Envelope& env, const mod::PerNoteTable<Envelope>::Note* held) {
			                   if (held != nullptr) {
				                   env.continueFrom(held->shape);
			                   }
			                   else {
				                   env.reset();
			                   }
		                   });
	    },
	// IT ONLY LISTENS: every event passes through untouched (see `NoteOps::listensOnly`).
	.listensOnly = true,
};

static_assert(std::size(kOutputs) == 1,
              "kModOps::outputAt answers output 0 and nothing else. If this device grew a second "
              "output, that lambda is where it has to be published — a declared output nothing "
              "answers reads as a source stuck at zero, which is silent and looks like a routing bug.");

} // namespace feathers::mod::dahdsr
