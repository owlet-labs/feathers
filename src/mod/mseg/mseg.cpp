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

#include "mod/mseg/mseg.h"
#include "mod/per_note.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathers::mod::mseg {

namespace {

/// A SEGMENT'S CURVE, as a warp of its 0..1 phase.
///
/// One function for every segment, rising or falling alike, which is the convention mod::kShapeMax
/// states: POSITIVE MEANS FULLER — the shape stays nearer its destination longer — and negative means
/// it gets there late. The DAHDSR gets the same convention by reading its curve backwards on its falling stages; here
/// it is arithmetic, and being arithmetic it has to be written down.
///
/// Why a power rather than the DAHDSR's exponential: an MSEG segment has no fixed direction, so the warp
/// must be expressible on phase alone and be symmetric under level inversion. `t^k` is, and it is
/// monotone for every k > 0, which is what keeps a segment from doubling back on itself.
float warp(float phase, float shape) {
	if (shape == 0.0f) {
		return phase;
	}
	// shape ∈ [-1, 1] → exponent ∈ [1/3, 3]. Three is enough curve to read as a curve and little
	// enough that the ends stay controllable; the DAHDSR's own shapes land in the same neighbourhood.
	const float k = std::exp2(-shape * 1.5849625f); // log2(3)
	return std::pow(std::clamp(phase, 0.0f, 1.0f), k);
}

/// Where a segment starts from, given the shape and the segment before it.
float levelBefore(const Settings& s, size_t segment) {
	return segment == 0 ? s.startLevel : s.segment[segment - 1].level;
}

} // namespace

//---------------//
// The verbs     //
//---------------//

Row rowOf(const Settings& s) {
	Row row {};
	const auto put = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };

	put(Param::StartLevel, s.startLevel);
	put(Param::SegmentCount, static_cast<float>(s.count));
	put(Param::SustainSegment, static_cast<float>(s.sustainSegment));
	put(Param::LoopStart, static_cast<float>(s.loopStart));
	put(Param::LoopEnd, static_cast<float>(s.loopEnd));
	put(Param::TimeBase, static_cast<float>(s.timeBase));
	put(Param::TimeScale, s.timeScale);
	put(Param::Smoothing, s.smoothing);
	put(Param::Mode, static_cast<float>(static_cast<int>(s.mode)));
	put(Param::Phase, s.phase);
	put(Param::PingPong, s.pingPong ? 1.0f : 0.0f);
	put(Param::Bipolar, s.bipolar ? 1.0f : 0.0f);
	// **THE BYPASS, WHICH THIS FORGOT AND THE HEAD DID NOT.** `Row row {}` starts at zero, so an
	// unwritten Enabled reads as OFF — this line is the difference between a switch that sticks and
	// one that snaps back on the next edit.
	put(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	put(Param::Amount, s.amount);
	put(Param::Poly, s.poly ? 1.0f : 0.0f);
	put(Param::SmoothCarry, s.smoothCarry ? 1.0f : 0.0f);
	put(Param::Trig, s.trig);
	put(Param::NotesTrig, s.notesTrig ? 1.0f : 0.0f);
	put(Param::StereoPhase, s.stereoPhase);

	for (size_t i = 0; i < kMaxSegments; ++i) {
		put(segmentParam(i, SegmentField::Time), s.segment[i].share);
		put(segmentParam(i, SegmentField::Level), s.segment[i].level);
		put(segmentParam(i, SegmentField::Shape), s.segment[i].shape);
	}
	return row;
}

namespace {

/// THE DECODE ALONE, WITHOUT THE SANITISE — and the two have to be separable or they call each other
/// forever. The first draft had settingsOf sanitising at its end and sanitize round-tripping through
/// settingsOf, which is unbounded recursion and a stack overflow on the very first tick. It presented
/// as SIGSEGV in a case about linear interpolation, which is worth remembering: a crash in the
/// simplest case is more often a cycle than an index.
///
/// The DAHDSR has the same split for the same reason. Copied deliberately rather than rediscovered.
Settings decodeRow(const Row& row, size_t fieldsPresent) {
	Settings s;
	// A FIELD THE PAYLOAD DOES NOT REACH KEEPS ITS DECLARED DEFAULT, which is the whole point of
	// carrying the count: a patch written by a build with six segments must not read six zeroes for
	// the two it never had, because a zero-length segment at level zero is a real shape and a silently
	// different one.
	const auto take = [&](Param p, float fallback) {
		const size_t i = static_cast<size_t>(p);
		return i < fieldsPresent ? row[i] : fallback;
	};
	const auto def = [](Param p) {
		return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
	};

	s.startLevel = take(Param::StartLevel, def(Param::StartLevel));
	s.count = static_cast<size_t>(std::lround(take(Param::SegmentCount, def(Param::SegmentCount))));
	s.sustainSegment =
	    static_cast<int>(std::lround(take(Param::SustainSegment, def(Param::SustainSegment))));
	s.loopStart = static_cast<size_t>(std::lround(take(Param::LoopStart, def(Param::LoopStart))));
	s.loopEnd = static_cast<size_t>(std::lround(take(Param::LoopEnd, def(Param::LoopEnd))));
	s.timeBase = static_cast<size_t>(std::lround(take(Param::TimeBase, def(Param::TimeBase))));
	s.timeScale = take(Param::TimeScale, def(Param::TimeScale));
	s.smoothing = take(Param::Smoothing, def(Param::Smoothing));
	// THE MODE — clamped into the enum, never cast raw. A value outside it reads as the default (Gate)
	// rather than undefined behaviour on a switch.
	{
		const int mode = static_cast<int>(std::lround(take(Param::Mode, def(Param::Mode))));
		s.mode = (mode >= 0 && mode < static_cast<int>(Mode::kCount)) ? static_cast<Mode>(mode)
		                                                             : Mode::Gate;
	}
	s.phase = take(Param::Phase, def(Param::Phase));
	s.pingPong = take(Param::PingPong, def(Param::PingPong)) >= 0.5f;
	s.bipolar = take(Param::Bipolar, def(Param::Bipolar)) >= 0.5f;
	// THROUGH `take` LIKE EVERY OTHER FLAG, so a row too short to carry it reads the DECLARED default
	// (on) rather than the zero an absent field would otherwise supply. A patch from a narrower build
	// must come back modulating, not silently bypassed.
	s.enabled = take(Param::Enabled, def(Param::Enabled)) >= 0.5f;
	s.amount = take(Param::Amount, def(Param::Amount));
	s.poly = take(Param::Poly, def(Param::Poly)) >= 0.5f;
	s.smoothCarry = take(Param::SmoothCarry, def(Param::SmoothCarry)) >= 0.5f;
	s.trig = std::clamp(take(Param::Trig, def(Param::Trig)), 0.0f, 1.0f);
	s.notesTrig = take(Param::NotesTrig, def(Param::NotesTrig)) >= 0.5f;
	s.stereoPhase = take(Param::StereoPhase, def(Param::StereoPhase));

	for (size_t i = 0; i < kMaxSegments; ++i) {
		s.segment[i].share =
		    take(segmentParam(i, SegmentField::Time), def(segmentParam(i, SegmentField::Time)));
		s.segment[i].level =
		    take(segmentParam(i, SegmentField::Level), def(segmentParam(i, SegmentField::Level)));
		s.segment[i].shape =
		    take(segmentParam(i, SegmentField::Shape), def(segmentParam(i, SegmentField::Shape)));
	}

	return s;
}

} // namespace

Settings settingsOf(const Row& row, size_t fieldsPresent) {
	Settings s = decodeRow(row, fieldsPresent);
	sanitize(&s);
	return s;
}

void sanitize(Settings* settings) {
	if (settings == nullptr) {
		return;
	}
	Row row = rowOf(*settings);
	for (size_t i = 0; i < row.size(); ++i) {
		const device::ParamDesc& desc = kParamDescs[i];
		if (!std::isfinite(row[i])) {
			// A NON-NUMBER IS NOT A SETTING SOMEBODY MEANT, so it goes to the declared default rather
			// than to an end of the range — a corrupt byte should not read as a deliberate extreme.
			row[i] = static_cast<float>(desc.def);
			continue;
		}
		row[i] = std::clamp(row[i], static_cast<float>(desc.min), static_cast<float>(desc.max));
		if (desc.stepped) {
			row[i] = static_cast<float>(std::lround(row[i]));
		}
	}
	Settings out = decodeRow(row, row.size());

	// THE RELATIONSHIPS THE DECLARATIONS CANNOT STATE. Every field above is in range on its own; what
	// a per-field clamp cannot say is that the loop must lie inside the live segments and that its end
	// must not precede its start. These are the only three, and they are here rather than in the
	// declarations because a range is a fact about ONE control.
	out.count = std::clamp(out.count, size_t { 1 }, kMaxSegments);
	out.loopStart = std::min(out.loopStart, out.count - 1);
	out.loopEnd = std::clamp(out.loopEnd, out.loopStart, out.count - 1);
	if (out.sustainSegment >= static_cast<int>(out.count)) {
		out.sustainSegment = kNoSustain;
	}
	if (out.sustainSegment < kNoSustain) {
		out.sustainSegment = kNoSustain;
	}
	*settings = out;
}

float valueOf(const Settings& s, Param param) {
	return rowOf(s)[static_cast<size_t>(param)];
}

void setValueOf(Settings* settings, Param param, float value) {
	if (settings == nullptr) {
		return;
	}
	Row row = rowOf(*settings);
	row[static_cast<size_t>(param)] = value;
	*settings = settingsOf(row, row.size());
}

double totalSeconds(const Settings& s, TransportClock clock) {
	double total = 0.0;
	for (size_t i = 0; i < s.count && i < kMaxSegments; ++i) {
		total += timeSeconds(s.segment[i].share);
	}
	// THE SHARES SUM TO WHATEVER THEY SUM TO, and the window is what they divide. Returning the sum
	// scaled by the knob would have been the old arithmetic surviving its own meaning: `timeScale` is
	// log2 now and a share is not a duration, so that product is not a length of anything.
	(void)total;
	return windowSeconds(kDivisionBeats[std::min(s.timeBase, kDivisionCount - 1)],
	                     timeSeconds(s.timeScale), clock);
}

//---------------//
// The engine    //
//---------------//

void Shape::prepare(double sampleRate) {
	const double rate = (std::isfinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kBlockSamples) / rate;
	// The coefficient is a function of the tick length, so the cache it was computed under is stale.
	smoothingKnobWas_ = -1.0f;
	reset();
}

void Shape::reset() {
	position_ = Position {};
	position_.finished = true;
	segmentFrom_ = 0.0f;
	direction_ = 1;
	smoothed_ = 0.0;
	smoothedR_ = 0.0;
	valueR_ = 0.0f;
	raw_ = 0.0f;
	value_ = 0.0f;
	gateWas_ = false;
	restarts_ = 0;
	trigPending_ = false;
	lock_.release();
}

void Shape::restart(const Settings& s) {
	// FROM THE START LEVEL, WHETHER OR NOT IT WAS STILL RUNNING.
	//
	// IT USED TO CONTINUE FROM ITS OWN OUTPUT when re-fired mid-flight — `position_.finished ?
	// s.startLevel : raw_` — as click avoidance, on the model of the DAHDSR's attack-from-where-it-stands. Reported 2026-09-05:
	// retrig did not work if the envelope had not completed; the output stayed at the last
	// level instead of starting at the beginning. MEASURED before the change: a falling shape
	// re-fired at 0.50 read 0.498 on the next tick, and one parked at a 0.5 sustain read 0.493. The
	// restart was real and inaudible, because a shape that continues from where it is towards where
	// it was already going has not restarted in any sense a listener can hear; only its slope changed.
	//
	// An envelope's re-attack is the other case: it heads for the PEAK from wherever it stands, so
	// a note mid-decay audibly goes back up, and from-current is the right shape for it. A drawn
	// shape has no peak to head for — its first segment goes wherever the user put it — so the only
	// restart that means anything is the one that begins at the beginning. Anyone who minds the step
	// has the smoothing switch, which exists for exactly this.
	//
	// AND THE START LEVEL IS THE UNSCALED ONE, as it must be: `raw_` was read back here after the
	// depth scale had been applied to it, so a shape at depth 0.5 restarted from the scaled output
	// and was then scaled again. Starting from the declared level closes that too.
	++restarts_;
	segmentFrom_ = s.startLevel;
	position_.segment = 0;
	position_.phase = 0.0f;
	position_.sustaining = false;
	position_.finished = false;
	direction_ = 1;
	// A NON-ZERO PHASE RESTARTS MID-SHAPE. The smoothing switch exists to soften the step this makes.
	if (s.phase > 0.0f) {
		setPhase(s, s.phase);
	}
}

std::optional<Shape::SongWindow> Shape::songWindow(const Settings& s) const {
	// **A STOPPED TRANSPORT IS NOT A POSITION TO LOCK TO** — `Synth::lockedPhaseFor` established it for
	// oscillator phase and automod's LFO follows it: a stopped host reports the same position for as
	// long as the user leaves it there, so locking would freeze the shape on one point.
	if (!clock_.playing) {
		return std::nullopt;
	}
	const double scale = timeSeconds(s.timeScale);
	if (!(scale > 0.0) || !std::isfinite(scale)) {
		return std::nullopt;
	}

	// THE TEMPO ROAD, IN BEATS. `windowSeconds` makes the window `divisionBeats * spb * scale`, so in
	// BEATS it is `divisionBeats * scale` — a length that a tempo change does not move.
	if (clock_.haveTempo() && clock_.havePositionBeats()) {
		const double windowBeats =
		    kDivisionBeats[std::min(s.timeBase, kDivisionCount - 1)] * scale;
		if (windowBeats > 0.0 && std::isfinite(windowBeats)) {
			return SongWindow { clock_.songPositionBeats, windowBeats,
			                    tickSeconds_ / clock_.secondsPerBeat, kBeatsRoad };
		}
		return std::nullopt;
	}

	// THE NO-TEMPO ROAD, IN SECONDS — where `windowSeconds` reads the scale as seconds directly.
	if (clock_.havePositionSeconds()) {
		return SongWindow { clock_.songPositionSeconds, scale, tickSeconds_, kSecondsRoad };
	}
	return std::nullopt;
}

std::optional<double> Shape::lockedTurns(const Settings& s) const {
	const std::optional<SongWindow> song = songWindow(s);
	if (!song.has_value()) {
		return std::nullopt;
	}
	return song->position / song->window;
}

void Shape::lockTo(const Settings& s, double turns) {
	// **WHERE THE SONG PUTS THE SHAPE — backlog DQ, asked for on 2026-08-24.**
	//
	// `modulator.h` recorded that `Trigger::Free` had never been transport-anchored: *"the shape runs
	// from whenever it last started, and dropping the playhead into the same passage twice gives two
	// different answers."* This is the road that was missing. The doc claiming otherwise was corrected
	// rather than the code; now the code catches up with what the doc originally said.
	if (s.count == 0 || !std::isfinite(turns)) {
		return;
	}
	const double frac = turns - std::floor(turns);

	// THE SHARES, NORMALISED — the same arithmetic `advance` uses to give a segment its slice of the
	// window, so a locked position and an advanced one cannot disagree about where a boundary is.
	double shareSum = 0.0;
	const size_t count = std::min(s.count, kMaxSegments);
	for (size_t i = 0; i < count; ++i) {
		shareSum += timeSeconds(s.segment[i].share);
	}
	if (!(shareSum > 0.0)) {
		return; // no shape to be anywhere in
	}

	double walked = 0.0;
	for (size_t i = 0; i < count; ++i) {
		const double slice = timeSeconds(s.segment[i].share) / shareSum;
		// **THE LAST SEGMENT TAKES WHATEVER IS LEFT**, so floating-point drift in the walk cannot land
		// a fraction of 0.999999 past the end and leave the shape finished.
		if (frac < walked + slice || i + 1 == count) {
			position_.segment = i;
			position_.phase = slice > 0.0
			    ? static_cast<float>(std::clamp((frac - walked) / slice, 0.0, 1.0))
			    : 0.0f;
		position_.sustaining = false;
		position_.finished = false;
		direction_ = 1;
		// THE LEVEL THE SEGMENT STARTS FROM, which the interpolator reads — without it a locked
		// shape would draw from wherever the previous tick happened to leave `segmentFrom_`.
		segmentFrom_ = levelBefore(s, i);
		return;
		}
		walked += slice;
	}
}

void Shape::setPhase(const Settings& s, float phase) {
	if (s.count == 0) {
		return;
	}
	const float frac = std::clamp(phase, 0.0f, 1.0f);

	// THE SHARES, NORMALISED — the same arithmetic `advance` and `lockTo` use.
	double shareSum = 0.0;
	const size_t count = std::min(s.count, kMaxSegments);
	for (size_t i = 0; i < count; ++i) {
		shareSum += timeSeconds(s.segment[i].share);
	}
	if (!(shareSum > 0.0)) {
		return;
	}

	double walked = 0.0;
	for (size_t i = 0; i < count; ++i) {
		const double slice = timeSeconds(s.segment[i].share) / shareSum;
		if (frac < static_cast<float>(walked + slice) || i + 1 == count) {
			position_.segment = i;
			position_.phase = slice > 0.0
			    ? static_cast<float>(std::clamp((frac - walked) / slice, 0.0, 1.0))
			    : 0.0f;
			position_.sustaining = false;
			position_.finished = false;
			segmentFrom_ = levelBefore(s, i);
			return;
		}
		walked += slice;
	}
}

void Shape::advance(const Settings& s, bool gate, float rateMul) {
	advanceBy(s, gate, tickSeconds_ * static_cast<double>(rateMul));
}

void Shape::advanceBy(const Settings& s, bool gate, double seconds) {
	if (position_.finished || position_.segment >= s.count) {
		return;
	}

	// HOLDING AT THE SUSTAIN BREAKPOINT. The shape has run through its sustain segment and waits
	// there for the gate to fall. Checked before advancing, so the tick that arrives at the
	// breakpoint holds rather than overshooting it.
	//
	// AND THE HOLD IS RE-ASKED EVERY TICK, NOT ONLY WHEN THE GATE FALLS. A hold that only ever ended
	// on the gate would survive its own reason for existing: turning Loop ON under a held note, or
	// dragging the marker somewhere else, would leave the shape frozen at a breakpoint that is no
	// longer a breakpoint. `sustainInForce` answers "is THIS still where the gate holds", so all
	// three ways of stopping being one — the loop, a move, and a clear — leave by the same door the
	// gate does.
	if (position_.sustaining) {
		if (gate && sustainInForce(s) == static_cast<int>(position_.segment)) {
			return;
		}
		position_.sustaining = false;
		// The hold is over: carry on from the breakpoint into whatever follows it.
		segmentFrom_ = s.segment[position_.segment].level;
		++position_.segment;
		position_.phase = 0.0f;
		if (position_.segment >= s.count) {
			position_.finished = true;
		}
		return;
	}

	// A TICK IS A BUDGET OF TIME, AND IT IS SPENT IN SECONDS RATHER THAN IN PHASE.
	//
	// PHASE IS A FRACTION OF THE SEGMENT IT BELONGS TO, so it is not a currency two segments can trade
	// in. The first version advanced the phase by one tick's worth of the ENTERING segment and carried
	// whatever overshot the end into the next segment UNCONVERTED — a remainder standing for a
	// fraction of 2 ms spent as that fraction of 20 ms. The error is the overshoot times the
	// DIFFERENCE between the two durations, so it is nearly nothing on segments of similar length and
	// most of the cycle on a short segment beside a long one.
	//
	// MEASURED, mean period over many cycles, where the knobs say 300 ms and 22 ms:
	//   100/150/50 ms   298.96 @ 44.1 kHz   299.26 @ 48 kHz   exact @ 96 kHz
	//   2/20 ms          13.89 @ 44.1 kHz    16.00 @ 48 kHz   exact @ 96 kHz
	// Exact at 96 kHz in both because every duration there is a whole number of ticks, so there is no
	// remainder to mis-convert. That pair — wrong at two rates, exact at the third, and the third
	// being the one whose arithmetic comes out even — is what says the fault was per-boundary rather
	// than a seconds-to-ticks conversion.
	//
	// EQUAL-LENGTH SEGMENTS WERE THE ONE SHAPE IT GOT PERFECTLY, which is worth writing down because
	// it is what made the bug survive: when every segment is the same length phase IS a shared
	// currency, so carrying a remainder unconverted is exactly right. The first draft of the case for
	// this was three equal 1 ms segments and it passed against the bug.
	//
	// Spending time removes it. Each iteration asks the CURRENT segment how much of the budget it
	// wants, takes that much, and hands the rest to whatever comes next — so a boundary costs nothing,
	// a segment shorter than a tick costs only its own duration, and the arithmetic never converts a
	// number out of one segment's units into another's.
	// THE WINDOW AND THE SHARES' SUM, ASKED ONCE PER TICK RATHER THAN PER SEGMENT.
	//
	// AND THE WINDOW IS READ FRESH EVERY TICK, WHICH IS THE WHOLE TEMPO-AUTOMATION REQUIREMENT.
	// The phase advances
	// by `spent / seconds` against the CURRENT window; it is never derived from an absolute elapsed
	// time divided by a window that has since changed. That second form is what teleports a shape
	// when a tempo ramp crosses a block: the phase would be re-answered against a different length.
	// Here a tempo change alters how fast the remaining phase is spent and touches the phase itself
	// not at all.
	double shareSum = 0.0;
	for (size_t i = 0; i < std::min(s.count, kMaxSegments); ++i) {
		shareSum += timeSeconds(s.segment[i].share);
	}
	const double window = windowSeconds(kDivisionBeats[std::min(s.timeBase, kDivisionCount - 1)],
	                                    timeSeconds(s.timeScale), clock_);

	double remaining = seconds;
	if (!(remaining > 0.0)) {
		return; // frozen, or a rateMul that is not a number: the shape stays where it is
	}

	// THE GUARD IS AGAINST A SHAPE FASTER THAN THE CONTROL RATE, not against a bug. A loop of
	// zero-duration segments completes without spending anything, so the budget never runs out and the
	// only thing that ends the tick is this. Sixty-four is far past the point where a modulator is
	// saying anything a 750 Hz reader could hear, and it is reached only by shapes that are asking for
	// something the tick cannot express.
	constexpr int kMaxCompletionsPerTick = 64;
	for (int completed = 0; completed < kMaxCompletionsPerTick; ++completed) {
		// A SEGMENT'S TIME IS ITS SHARE OF THE WINDOW, normalised by the shares' own sum — which is
		// what makes "the whole curve fits inside the window" true by construction rather than by the
		// numbers happening to add up to one. A hand dragging one breakpoint must not change how long
		// the shape takes; it changes how the window is DIVIDED.
		const double seconds = shareSum > 0.0
		    ? timeSeconds(s.segment[position_.segment].share) / shareSum * window
		    : 0.0;

		// A SEGMENT WITH NO DURATION IS A STEP, not a divide by zero, and — the part the first draft
		// got wrong — not a tick either. The bottom of a LogSeconds range means "no time at all",
		// device.h's convention and the same one that cost the DAHDSR's card three bugs; a step that
		// charged a tick for itself would make three stacked steps take three ticks to happen.
		if (seconds > 0.0) {
			const double left = (1.0 - static_cast<double>(position_.phase)) * seconds;
			if (remaining < left) {
				position_.phase += static_cast<float>(remaining / seconds);
				return;
			}
			remaining -= left;
		}

		const size_t done = position_.segment;
		segmentFrom_ = s.segment[done].level;

		// THE SUSTAIN BREAKPOINT, reached. Hold here while the gate is up; a shape with no sustain
		// point runs straight through, which is what makes it a one-shot.
		//
		// ASKED OF `sustainInForce` RATHER THAN OF THE FIELD, and that is the whole of the loop fix.
		// This branch used to read `s.sustainSegment` and return, so it ran BEFORE the loop branch
		// below and never consulted `s.loop` — a sustain marker on a looping shape silently disabled
		// the loop. See sustainInForce for why the loop wins and why the stored index survives.
		const int sustainAt = sustainInForce(s);
		if (gate && sustainAt >= 0 && done == static_cast<size_t>(sustainAt)) {
			position_.sustaining = true;
			position_.phase = 0.0f;
			return;
		}

		// THE LOOP MARKERS — the thing a plain flag cannot say, and the reason this device exists
		// beside the DAHDSR. The shape jumps from the END of loopEnd back to the START of loopStart,
		// so an attack before the markers plays once and the middle repeats under it. That is the
		// sampler's sustain loop and it is Bitwig's Segments behaviour.
		//
		// WHILE THE GATE IS UP, and only then: a loop that survived the gate falling would never let
		// the shape reach whatever follows the markers, so a release after a loop would be
		// unreachable. Free and LFO Retrig substitute an always-up gate upstream, which is what makes a
		// free-running looped shape an LFO.
		//
		// PING-PONG REVERSES AT THE BOUNDARIES instead of wrapping. At loopEnd going forward, it
		// turns around and heads back toward loopStart; at loopStart going backward, it turns again.
		// The result is a back-and-forth that plays the middle segments both ways.
		const bool looping =
		    (s.mode == Mode::Loop || s.mode == Mode::Free || s.mode == Mode::LfoRetrig);
		if (looping && gate) {
			if (direction_ > 0 && done == s.loopEnd && s.loopEnd >= s.loopStart) {
				if (s.pingPong && s.loopEnd > s.loopStart) {
					direction_ = -1;
					position_.segment = s.loopEnd - 1;
					position_.phase = 0.0f;
					segmentFrom_ = levelBefore(s, position_.segment);
					continue;
				}
				position_.segment = s.loopStart;
				segmentFrom_ = levelBefore(s, s.loopStart);
				position_.phase = 0.0f;
				continue;
			}
			if (direction_ < 0 && done == s.loopStart && s.loopEnd > s.loopStart) {
				direction_ = 1;
				position_.segment = s.loopStart + 1;
				position_.phase = 0.0f;
				segmentFrom_ = levelBefore(s, position_.segment);
				continue;
			}
		}

		if (direction_ > 0) {
			++position_.segment;
		} else if (position_.segment > 0) {
			--position_.segment;
		}
		if (position_.segment >= s.count) {
			position_.finished = true;
			position_.phase = 0.0f;
			position_.segment = s.count;
			return;
		}
		position_.phase = 0.0f;
	}
}

float Shape::tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
                  float rateMul) {
	// THE CLOCK IS NOT READ YET — the window arrives in the next commit. It is threaded through now
	// so the rack's contract changes once rather than twice, and so the plumbing can be cased on its
	// own: "the tempo reaches the device, per block" is a claim worth holding before anything depends
	// on it.
	clock_ = clock;
	Settings s = settings;
	sanitize(&s);

	// THE GATE, ONCE, AT ONE PLACE. Free and LFO Retrig substitute an always-up gate here and
	// everything downstream goes on reasoning about a single boolean — the DAHDSR's arrangement, kept
	// deliberately so the two devices cannot drift apart on what a mode means.
	const bool free = (s.mode == Mode::Free || s.mode == Mode::LfoRetrig);
	const bool effectiveGate = free || gate;
	const bool rising = effectiveGate && !gateWas_;
	// **NOTES FIRE TRIG, OR ONLY THE TRIG DOES** (backlog NU; the rule per mode is `mod::notesTrigParamDesc`'s). Off, a
	// key going down neither starts nor retriggers the shape; Free's and LFO Retrig's own first start is not a note.
	const bool notes = notesFireTrig(s);
	const bool mayRetrigger = notes
	    && (((s.mode == Mode::Gate || s.mode == Mode::Loop) && retrigger) || (s.mode == Mode::LfoRetrig && retrigger));

	// **A TRIG (backlog MV) RESTARTS IT WHEREVER THE MODE LETS THE SHAPE RUN** — see `fire`.
	const bool trigRestart = std::exchange(trigPending_, false) && (effectiveGate || s.mode == Mode::Once);
	if (trigRestart || (effectiveGate && ((rising && (notes || free)) || mayRetrigger))) {
		restart(s);
	}
	gateWas_ = effectiveGate;

	// **FREE IS POSITIONED BY THE SONG — backlog DQ, asked for on 2026-08-24.**
	//
	// **THE UNIT FOLLOWS THE WINDOW, which is the same rule automod's LFO follows.** `windowSeconds`
	// makes the window `divisionBeats * scale` BEATS when a tempo is known and `scale` SECONDS when it
	// is not — so the lock reads beats in the first case and seconds in the second, and the window is
	// constant in whichever unit is being used.
	//
	// **THAT IS ALSO WHAT KEEPS `advance`'s WARNING SATISFIED.** It says the phase must never be
	// "derived from an absolute elapsed time divided by a window that has since changed", because that
	// teleports a shape when a tempo ramp crosses a block. Locking in BEATS against a beat-denominated
	// window is immune to exactly that: a tempo change moves neither term.
	//
	// **AND A ROUTE INTO RATE MUST NOT SCRUB IT, SO THE SONG'S STEP IS FOLLOWED RATHER THAN ITS PLACE
	// DIVIDED** — see `mod::TransportLock`. Placing the shape at `position / window` every tick threw it
	// between segments whenever the window moved, further the later in the song; the lock re-anchors to
	// that quotient only where the song itself jumped.
	// **ONLY FREE IS LOCKED TO THE SONG** (2026-10-03: LFO Retrig follows the Poly switch — each note restarts
	// its own shape, or the one shape restarts on each new note). LFO Retrig was locked too, and the lock overwrote
	// the note-on restart on the same tick, so on a playing transport notes did not restart it (backlog KF).
	const std::optional<SongWindow> song = s.mode == Mode::Free ? songWindow(s) : std::nullopt;
	std::optional<double> lockedAt;
	if (song.has_value()) {
		double turns = lock_.follow(song->position, song->window, song->tickLength, song->road);
		if (trigRestart) {
			lock_.restartAt(s.phase); // a Trig's restart holds against the song (backlog MV)
			turns = s.phase;
		}
		lockTo(s, turns);
		lockedAt = turns;
	}
	else {
		lock_.release();
	}

	advance(s, effectiveGate, rateMul);

	// WHERE THAT PUTS THE OUTPUT.
	raw_ = rawAtPosition(s);

	// **THE RIGHT EAR** (backlog PA): the same shape, `stereoLead` of the window on. At 0 it IS the left ear — copied
	// after the smoothing below, not recomputed, so a mono MSEG's arithmetic is the left ear's and nothing else.
	const float lead = s.stereoLead();
	const float rawR = lead != 0.0f ? rawRightAt(s, lead, effectiveGate, rateMul, lockedAt) : raw_;

	// SMOOTHING, on the same one-pole and for the same reason as the DAHDSR's: it is about what the
	// DESTINATION does with a fast shape, not about making the shape smoother.
	//
	// ONE BRANCH, BECAUSE "OFF" IS A COEFFICIENT OF 1 — the switch is deleted and
	// `mod::smoothingCoeff` returns 1 at the bottom of the travel, which is exactly what the old
	// `else` branch wrote by hand. This device was already reading the bottom as zero through
	// `timeSeconds`, so it is the one of the three whose SOUND does not move: what changes here is
	// that the law now lives in one place rather than three.
	if (s.smoothing != smoothingKnobWas_) {
		smoothingKnobWas_ = s.smoothing;
		smoothingCoeff_ = mod::smoothingCoeff(s.smoothing, tickSeconds_);
	}
	// **AND IT ARRIVES** (the DAHDSR's bug, 2026-09-26, asked of this device and MEASURED here: a shape
	// ending at 0.3 under 1 s smoothing was still 1.8e-6 short 14 s later, crawling, while `idle()` said
	// finished). Snapped exactly at rest — `mod::smoothingStep`.
	mod::smoothingStep(smoothed_, static_cast<double>(raw_), smoothingCoeff_);

	// BIPOLAR RESCALES THE OUTPUT, NOT THE SHAPE — the same arithmetic and the same consequence the
	// DAHDSR documents: an idle bipolar device reads -1 rather than 0, which the card has to know
	// about because half its travel is below a unipolar box's floor.
	value_ = s.bipolar ? static_cast<float>(2.0 * smoothed_ - 1.0) : static_cast<float>(smoothed_);
	if (lead != 0.0f) {
		mod::smoothingStep(smoothedR_, static_cast<double>(rawR), smoothingCoeff_);
		valueR_ = s.bipolar ? static_cast<float>(2.0 * smoothedR_ - 1.0) : static_cast<float>(smoothedR_);
	}
	else {
		smoothedR_ = smoothed_;
		valueR_ = value_;
	}
	return value_;
}

float Shape::rawAtPosition(const Settings& s) const noexcept {
	float raw = 0.0f;
	if (position_.finished || position_.segment >= s.count) {
		// Finished: it stays where the last segment left it rather than snapping to zero. A shape that
		// ends high ends high, and a route from it holds there until the shape is re-fired.
		raw = position_.segment == 0 ? s.startLevel : s.segment[std::min(s.count, kMaxSegments) - 1].level;
	}
	else if (position_.sustaining) {
		raw = s.segment[position_.segment].level;
	}
	else {
		const Segment& seg = s.segment[position_.segment];
		const float t = warp(position_.phase, seg.shape);
		raw = segmentFrom_ + (seg.level - segmentFrom_) * t;
	}
	return std::clamp(raw, 0.0f, 1.0f);
}

float Shape::rawRightAt(const Settings& s, float lead, bool gate, float rateMul, std::optional<double> locked) const {
	// A COPY, RUN ON AND THROWN AWAY: the right ear is a function of where the left ear stands, so it carries no state
	// of its own but its smoother — nothing to persist, and nothing that can drift from the left.
	Shape ahead = *this;
	if (locked.has_value()) {
		// THE SONG PLACES THE LEFT EAR AT `turns` OF THE WINDOW; it places the right ear `lead` further, by the same lock.
		ahead.lockTo(s, *locked + static_cast<double>(lead));
		ahead.advance(s, gate, rateMul);
	}
	else {
		// THE LEAD IS A SHARE OF THE WINDOW, spent in the window's own seconds — the budget `advance` spends a tick of, so
		// a route into Rate (which scales the tick's budget, not the window) moves the ears together and not apart.
		const double window = windowSeconds(kDivisionBeats[std::min(s.timeBase, kDivisionCount - 1)],
		                                    timeSeconds(s.timeScale), clock_);
		ahead.advanceBy(s, gate, static_cast<double>(lead) * window);
	}
	return ahead.rawAtPosition(s);
}

float Shape::rawLeadingAt(const Settings& settings, size_t segment, float phase, float lead, bool gate) {
	Settings s = settings;
	sanitize(&s);
	Shape at;
	at.restart(s);
	at.position_.segment = std::min(segment, s.count - 1);
	at.position_.phase = std::clamp(phase, 0.0f, 1.0f);
	at.segmentFrom_ = levelBefore(s, at.position_.segment);
	// THE CARD HAS NO TRANSPORT, so the window is the standalone one; the lead is a share of it either way.
	const double window = windowSeconds(kDivisionBeats[std::min(s.timeBase, kDivisionCount - 1)],
	                                    timeSeconds(s.timeScale), TransportClock::unknown());
	at.advanceBy(s, gate, static_cast<double>(lead) * window);
	return at.rawAtPosition(s);
}

} // namespace feathers::mod::mseg

//------------------------------------------------------------------------------------------------//
// THE MODULATOR OPS TABLE — how a host advances this device without knowing it is an MSEG.         //
//------------------------------------------------------------------------------------------------//

namespace feathers::mod::mseg {

namespace {

/// THE DEVICE'S STATE: its shape, poly or mono (`mod::PolyShape`), and its latched settings — the DAHDSR's
/// arrangement, so two devices with nothing in common but the tier answer one contract identically. Under Poly
/// every note runs its own shape; under mono, the one shape every destination reads.
using OpsShape = mod::PolyShape<Shape, Settings>;

OpsShape& asShape(void* storage) {
	return *static_cast<OpsShape*>(storage);
}
const OpsShape& asShape(const void* storage) {
	return *static_cast<const OpsShape*>(storage);
}

void setFieldsOf(void* storage, const float* fields, size_t fieldsPresent) {
	Row row {};
	const size_t take = fieldsPresent < row.size() ? fieldsPresent : row.size();
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	// THE MSEG READS ITS GLOBALS FROM A SHORT PAYLOAD — `settingsOf` takes `fieldsPresent` for
	// exactly that, so a row that stops early is a shape with fewer points rather than a shape
	// whose points are all zero.
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
	// A DESTINATION WITH NO VOICE reads the mono shape, or under Poly the most recently struck note's.
	.outputAt = [](const void* storage,
	               size_t output) { return output == 0 ? asShape(storage).sharedShape().value() : 0.0f; },
	// THE PER-VOICE ROAD: under Poly the note's own shape, under mono the one shape.
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
	// WHICH SEGMENT IS RUNNING AND HOW FAR THROUGH IT, of the shape `outputAt` reads. **ORDINAL 0 IS A VALID
	// SEGMENT HERE**, unlike the DAHDSR's ordinal 0 which is `Stage::Idle` — which is exactly why
	// `ModOps::playhead` may be null and a null must mean "cannot say" rather than zero.
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const mod::Playhead where = asShape(storage).sharedShape().playhead();
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

/// THE NOTE LANE'S HALF OF THE SAME DEVICE — the DAHDSR's shape: one storage, two tables, every event passes
/// through. Its `setFields` is the modulator's, so the per-note shapes run the settings the card shows.
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
		    // NO LEGATO HERE: the MSEG has no Single Trigger, so every note's shape starts fresh.
		    asShape(storage).processNotes(in, inCount, out, outCapacity, outCount, false,
		                                  [](Shape& shape, const mod::PerNoteTable<Shape>::Note*) { shape.reset(); });
	    },
	.listensOnly = true,
};

static_assert(std::size(kOutputs) == 1,
              "kModOps::outputAt answers output 0 and nothing else — see the DAHDSR's twin of this "
              "assert for why a declared output nothing answers is a silent routing bug.");

} // namespace feathers::mod::mseg
