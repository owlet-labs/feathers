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

#include "fx/automod/automod.h"

// THE RATE CONTROL'S DECLARED SPAN IS THE CURVE'S (backlog DT): both say ±2 octaves, in two headers
// that do not include each other, so the agreement is checked where both are visible.
static_assert(feathers::fx::automod::kParamDescs[static_cast<size_t>(feathers::fx::automod::Param::Rate)].max
                  == static_cast<double>(feathers::fx::automod::kRateMultiplierOctaves)
              && feathers::fx::automod::kParamDescs[static_cast<size_t>(feathers::fx::automod::Param::Rate)].min
                     == -static_cast<double>(feathers::fx::automod::kRateMultiplierOctaves),
              "automod's Rate range and rateMultiplierFor's span disagree");

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace feathers::fx::automod {

namespace {

// THE CORNER'S ARITHMETIC LIVES IN `automod_filter.h` NOW — `kFilterBaseLowHz`, `kFilterModOctaves`,
// `baseCutoffHzFor` and `cornerHzFor`, with the whole "it is an offset from a bright floor" record
// beside them. It moved out of this anonymous namespace so the card's LFO strip can draw the corner
// the device will actually sit at rather than recompute it. One definition, two readers.

float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

//------------------------------------------------------------------------------------------------//

Settings settingsOf(const Row& row) {
	const auto get = [&row](Param p) { return row[static_cast<size_t>(p)]; };
	Settings s;
	s.enabled = get(Param::Enabled) >= 0.5f;
	s.type = get(Param::Type);
	s.flavor = get(Param::Flavor);
	s.mod = get(Param::Mod);
	s.gamma = get(Param::Gamma);
	s.freq = get(Param::Freq);
	s.rate = get(Param::Rate);
	// CLAMPED INTO THE CHOICE, because a row is what a patch load hands over — 8a's rule that the
	// guard belongs on the road the DATA takes.
	s.syncDivision = std::clamp(static_cast<int>(std::lround(get(Param::SyncDivision))), 0,
	                            static_cast<int>(kSyncChoiceCount) - 1);
	// CLAMPED INTO THE ENUM, because a row is what a patch load hands over and a patch is a file.
	// The playbook's 8a is exactly this shape: the guard belongs on the road the DATA takes.
	const int mode = static_cast<int>(std::lround(get(Param::LfoMode)));
	s.lfoMode = static_cast<LfoModeChoice>(
	    std::clamp(mode, 0, static_cast<int>(LfoModeChoice::kCount) - 1));
	s.depth = get(Param::Depth);
	s.manual = get(Param::Manual);
	s.mix = get(Param::Mix);
	// CLAMPED INTO ITS DECLARED RANGE, on the road the DATA takes — the same rule the two choices
	// above follow. A trim outside this is a gain nobody asked for on a follower nothing bounds.
	s.envTrim = std::clamp(get(Param::EnvTrim), -48.0f, 0.0f);
	// **NEITHER WRAPPED NOR CLAMPED — A RE-AIMING HAS NO ENDS, AND THIS ONE HAS NO PERIOD EITHER.**
	//
	// This wrapped into `[0, 2pi)`, under the belief that "a patch carrying 7 radians means the same
	// basis as one carrying 7 - 2pi". **MEASURED 2026-09-08, and it is false:** `basisFor(0.4, 0.25)`
	// and `basisFor(0.4 + 2pi, 0.25)` differ in their first component by 1.607 against 1.140. The
	// basis is `sliceCoefficient` feeding phi triangles, and phi multipliers are irrational — the
	// turn is not a period, which is exactly the property that makes "keep turning" hand back new
	// terrain rather than the same eight zones. So the wrap was not preserving a patch's meaning; it
	// was throwing away every plane past the first turn and calling them duplicates.
	//
	// THE DEFECT: Automod's rotate was bounded 0-100%, unlike Feathers' Explore and Shaper rotate.
	// The shaper's rotate does not wrap, and Explore's is a compass over an unbounded base
	// precisely because "there is no end of travel". This is that, and the descriptor's range was
	// widened in the same commit so a hand can reach it.
	//
	// **AND NO PATCH CHANGES MEANING.** The old declared range was `0 .. 2pi`, and over exactly that
	// interval the wrap was the identity (`floor(x / 2pi) == 0`). Every value any stored patch can
	// hold derives to the same settings it always did; only values the old range could never carry
	// are newly meaningful. The finite guard stays, because a NaN is not a plane.
	{
		const float raw = get(Param::Rotate);
		s.rotate = std::isfinite(raw) ? raw : 0.0f;
	}
	{
		const float raw = get(Param::Width);
		s.width = std::isfinite(raw) ? std::clamp(raw, 0.0f, 1.0f) : 1.0f;
	}
	s.keytrack = get(Param::Keytrack) >= 0.5f;
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	const auto put = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	put(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	put(Param::Type, s.type);
	put(Param::Flavor, s.flavor);
	put(Param::Mod, s.mod);
	put(Param::Gamma, s.gamma);
	put(Param::Freq, s.freq);
	put(Param::Rate, s.rate);
	put(Param::SyncDivision, static_cast<float>(s.syncDivision));
	put(Param::LfoMode, static_cast<float>(s.lfoMode));
	put(Param::Depth, s.depth);
	put(Param::Manual, s.manual);
	put(Param::Mix, s.mix);
	put(Param::EnvTrim, s.envTrim);
	put(Param::Rotate, s.rotate);
	put(Param::Width, s.width);
	put(Param::Keytrack, s.keytrack ? 1.0f : 0.0f);
	return row;
}

//------------------------------------------------------------------------------------------------//

void Automod::prepare(double sampleRate) {
	sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
	// IN SAMPLES AT THIS RATE, so the wait is a duration rather than a block count.
	restSilenceNeeded_ = static_cast<long>(kRestSilentSeconds * sampleRate_);
	lfo_.prepare(sampleRate_);
	filter_.prepare(sampleRate_);
	comb_.prepare(sampleRate_);
	envelope_.prepare(sampleRate_);
	for (int i = 0; i < 2; ++i) {
		filterSpring_[i].prepare(sampleRate_);
		combSpring_[i].prepare(sampleRate_);
	}
	// THE DERIVE IS INVALIDATED RATHER THAN RECOMPUTED HERE. A rate change moves the spring
	// coefficients and the LFO increment, and `refresh()` is the one place that knows how to push
	// them; doing it twice is two places to forget one.
	derivedValid_ = false;
	reset();
}

void Automod::reset() {
	lfo_.reset();
	filter_.reset();
	comb_.reset();
	envelope_.reset();
	for (int i = 0; i < 2; ++i) {
		filterSpring_[i].reset();
		combSpring_[i].reset();
	}
	gateWas_ = false;
	lock_.release();
	silentRun_ = 0;
	// THE TICK GRID RESTARTS WITH THE STREAM, which is what "counted from the start of the stream"
	// means — a reset IS a new stream.
	tickPhase_ = 0;
	rampsPrimed_ = false;

	// AND THE DERIVE IS INVALIDATED, SO THE NEXT TICK TAKES THE SAME ROAD A FRESH DEVICE DOES.
	//
	// `refresh()` does more than compute numbers: the FIRST build after it puts the springs ON THEIR TARGETS,
	// which is what stops a rebuilt waveform making them lunge. A reset that only zeroed the springs
	// left them at zero where a just-switched-on device has them sitting on their targets — so the first
	// block after a reset was a lunge that the first block of a fresh device is not.
	//
	// CAUGHT BY `EVERY audio device: reset means AS IF JUST SWITCHED ON, not merely with the memory
	// cleared`, which is exactly the distinction its name makes, and which only bit once the default
	// coordinate produced a live LFO — at (0, 0, 0) the waveform was flat, so a spring snapped to it
	// and a spring zeroed were the same thing.
	derivedValid_ = false;
}

void Automod::setTarget(const Settings& s) {
	target_ = s;

	// THE TRIM IS A dB NUMBER AND THE SAMPLE LOOP WANTS A GAIN, so it is converted HERE — once per
	// settings change rather than once per sample. It is not smoothed: it is a set-and-forget
	// calibration of the follower's input, not a performance control, and the follower's own attack
	// and release already smooth everything downstream of it.
	envTrimGain_ = std::pow(10.0f, s.envTrim / 20.0f);

	// THE RUN MODE REACHES THE LFO HERE AND NOT IN `refresh()`, AND THAT IS THE BUG THIS LINE FIXES.
	//
	// The first draft pushed the shape, the rate and the offsets in `refresh()` and never pushed the
	// mode at all — so the LFO sat at its default `RunMode::Free`, `trigger()` returned early every
	// time, and **Retrigger and Once were dead controls**. A case reading the phase across a note-on
	// caught it; nothing in the audio would have, because a Free LFO under a retrigger patch still
	// sweeps and still sounds like an auto-wah.
	//
	// IT CANNOT LIVE IN `refresh()` EITHER, which is the second half of the mistake: that function is
	// gated on the COORDINATES, and the mode is not one of them — changing only the mode would not
	// re-run it. Here it is unconditional and costs an assignment.
	switch (s.lfoMode) {
	case LfoModeChoice::Retrigger:
		lfo_.setRunMode(RunMode::Retrigger);
		break;
	case LfoModeChoice::Once:
		lfo_.setRunMode(RunMode::Once);
		break;
	case LfoModeChoice::Free:
	case LfoModeChoice::Stop:
	case LfoModeChoice::kCount:
		// STOP MAPS TO FREE AND THAT IS NOT A SHRUG: in Stop the effect never calls `tick`, so the
		// LFO's own mode is unobservable. Free is the honest label for "this object is not being
		// asked anything".
		lfo_.setRunMode(RunMode::Free);
		break;
	}
}

void Automod::setTransportClock(const device::TransportClock& clock) {
	// HELD, NOT ACTED ON. The rate is recomputed in `refresh()`, which is gated on the coordinates —
	// and the tempo is one of its inputs, so `effectiveRateHz()` reads this and `refresh()` compares
	// the RESULT. A tempo that changes the rate re-derives; one that does not, does not.
	clock_ = clock;
}

/// **HOW FAR THROUGH ITS CYCLE THE SONG SAYS THE LFO IS, 0..1 — or nothing, and the caller free-runs.**
///
/// DECISION: in Free mode Automod's phase locks to the transport — derived from the DAW playhead time
/// modulo the Automod period — and Free always locks.
///
/// **THE TIMELINE IS CHOSEN BY THE RATE'S OWN UNIT, which is the whole of the design.** It was asked
/// what would be more natural under sync and leaned beats; beats is right there, and the rule behind
/// it is stronger than the preference:
///
///   - **A SYNCED DIVISION'S PERIOD IS N BEATS**, so beats keep it on the grid: bar 3 beat 1 is the
///     same phase every time, and a tempo change moves the cycle with the music. A seconds-derived
///     phase on a musical division goes DISCONTINUOUS the moment the tempo moves, which is the one
///     thing a synced LFO exists not to do.
///   - **A FREE Hz RATE'S PERIOD IS A NUMBER OF SECONDS** and has nothing to do with the tempo, so
///     deriving it from beats would make a deliberately unmusical rate drift with the tempo anyway.
///
/// Not a compromise between them: each reads the timeline its own period is measured in.
///
/// **A STOPPED TRANSPORT IS NOT A POSITION TO LOCK TO**, and that is not a new rule — `Synth::
/// lockedPhaseFor` settled it for oscillator phase and the reasoning transfers whole: a stopped host
/// reports the SAME position for as long as the user leaves it there, so locking would freeze the LFO
/// on one phase. Free-running is what it falls back to, which is still a clock, just not the song's.
///
/// **AND NOTHING IS RETURNED WHERE THE HOST SAID NOTHING.** Standalone, or a host with no timeline:
/// the accumulator keeps running, which is exactly today's behaviour. The feature is host-dependent
/// by construction and it is better to say so than to invent a position.
std::optional<Automod::SongPeriod> Automod::songPeriod() const {
	if (!clock_.playing) {
		return std::nullopt;
	}
	const float multiplier = rateMultiplierFor(target_.rate);
	if (!(multiplier > 0.0f) || !std::isfinite(multiplier)) {
		return std::nullopt;
	}
	const double tickSeconds = static_cast<double>(kTickSamples) / sampleRate_;

	// THE SYNCED ROAD, IN BEATS. The period is the division divided by the rate multiplier, because
	// `effectiveRateHz` multiplies the rate up — so a 1/4 at 200% is a cycle every half of a beat.
	if (isSyncedRate(target_.syncDivision) && clock_.haveTempo() && clock_.havePositionBeats()) {
		const double periodBeats = syncDivisionBeats(target_.syncDivision) / multiplier;
		if (periodBeats > 0.0 && std::isfinite(periodBeats)) {
			return SongPeriod { clock_.songPositionBeats, periodBeats,
			                    tickSeconds / clock_.secondsPerBeat, kBeatsRoad };
		}
		return std::nullopt;
	}

	// THE FREE ROAD, IN SECONDS. `nominalRateHz` already folds the multiplier in, and it is the same
	// number `effectiveRateHz` falls back to — so a synced division with no tempo locks to the rate it
	// is actually running at rather than to the one it is displaying.
	if (clock_.havePositionSeconds()) {
		const double hz = static_cast<double>(nominalRateHz(target_));
		if (hz > 0.0 && std::isfinite(hz)) {
			return SongPeriod { clock_.songPositionSeconds, 1.0 / hz, tickSeconds, kSecondsRoad };
		}
	}
	return std::nullopt;
}

std::optional<double> Automod::lockedTurns() const {
	const std::optional<SongPeriod> song = songPeriod();
	if (!song.has_value()) {
		return std::nullopt;
	}
	return song->position / song->period;
}

/// WHAT THE LFO SHOULD RUN AT, in Hz — the one place the free and synced roads meet.
///
/// SYNCED WITHOUT A TEMPO FALLS BACK TO FREE, which is the honest degradation rather than a silence
/// or a guess. `mseg`'s window makes the same choice for the same reason: a standalone build and a
/// host reporting no timeline are real states, and a device that stopped moving in them would be
/// broken rather than principled. The control still reads "1/8" because that is what it will do the
/// moment a tempo arrives.
float Automod::effectiveRateHz() const {
	// **RATE SCALES EVERY CASE, WHICH IS THE WHOLE CHANGE.** It used to be consulted in NO case under
	// sync and in every case without it; now it is a multiplier on whatever the list selected, so the
	// control has one meaning and 100% is 1:1 wherever you are.
	const float multiplier = rateMultiplierFor(target_.rate);
	if (isSyncedRate(target_.syncDivision) && clock_.haveTempo()) {
		const double beats = syncDivisionBeats(target_.syncDivision);
		const double seconds = beats * clock_.secondsPerBeat;
		if (seconds > 0.0) {
			// A DIVISION IS A CYCLE LENGTH, so the rate is its reciprocal — a 1/4 at 120 bpm is one
			// cycle every half second, which is 2 Hz.
			return static_cast<float>(1.0 / seconds) * multiplier;
		}
	}
	// A DIVISION WITH NO TEMPO FALLS BACK TO A FREE RATE rather than to silence — a standalone build
	// is a real state, and the control shows what it WILL do when a tempo arrives.
	return nominalRateHz(target_);
}

void Automod::setNoteContext(const device::NoteContext& note) {
	const float semis = finiteOr(note.pitchSemitones, 60.0f);

	// A NOTE-ON RETRIGGERS THE LFO, which is what Retrigger and Once mean. The EDGE is what matters,
	// not the level — a held gate must not restart the sweep on every block.
	const bool rising = note.gate && !gateWas_;
	gateWas_ = note.gate;

	if (semis != noteSemitones_) {
		noteSemitones_ = semis;
		// PITCH TRACKING, in OCTAVES from middle C, applied multiplicatively so an octave up is
		// twice the cutoff and HALF the comb delay — the two move in opposite directions because a
		// comb's delay is the reciprocal of its pitch. Clamped, because a route can put a note
		// anywhere and a delay ratio of 400 is a read off the end of the line.
		const float octaves = (semis - 60.0f) / 12.0f;
		filterPitchRatio_ = std::clamp(std::exp2(octaves), 0.25f, 4.0f);
		combPitchRatio_ = std::clamp(std::exp2(-octaves), 0.25f, 4.0f);
	}

	if (rising) {
		lfo_.trigger();
	}
}

Automod::Rebuild Automod::refresh(float width) {
	Rebuild change;
	// **THE STEREO OFFSET IS THE FIELD'S TIMES `Width`, SET EVERY TICK and not only on a rebuild** — Width
	// is not a coordinate, so a turn of it must reach the LFO without re-deriving anything. It is set
	// BEFORE the reading below so a Width change is never mistaken for part of a rebuild: the springs
	// GLIDE across a Width turn, which is what a knob turn should sound like.
	lfo_.setStereoOffset(derived_.stereoPhaseOffset * width);

	// ASSEMBLED BY THE FREE FUNCTION, not here — the card's strip calls the same one, so a picture
	// cannot name a different point in the field from the one the sound is read at.
	const AutomodCoords coords = coordsOf(target_, effectiveRateHz());

	// THE GATE IS AN EQUALITY ON THE ARGUMENTS — see the header on why it is bitwise and not a
	// tolerance.
	if (derivedValid_ && coords == coordsWas_) {
		return change;
	}
	change.rebuilt = true;
	change.first = !derivedValid_;
	// **THE LFO, READ AT THIS PHASE UNDER THE OLD SHAPE** — see `beginTick` for what the pair is for.
	change.before = lfo_.read();

	derived_ = deriveAutomod(coords);
	coordsWas_ = coords;
	derivedValid_ = true;
	++deriveCount_;

	// PUSH WHAT DOES NOT CHANGE PER SAMPLE. The waveform, the rate, the offsets, the spring
	// coefficients, the envelope's times and the comb's settings are all functions of the
	// coordinates alone, so they belong here rather than in the sample loop.
	// THE DERIVE'S OWN SHAPE, HANDED OVER — not rebuilt from the coordinate. See `Lfo::setShape` for
	// the known-bad that proved the two copies could disagree with every suite still green.
	lfo_.setShape(derived_.shape, derived_.lfoStartPhase);
	lfo_.setRateHz(coords.lfoHz);
	lfo_.setStereoOffset(derived_.stereoPhaseOffset * width);
	lfo_.setCombOffset(derived_.combPhaseOffset);

	envelope_.setTimes(derived_.envAttackSeconds, derived_.envReleaseSeconds);

	for (int i = 0; i < 2; ++i) {
		filterSpring_[i].setResponse(derived_.springHz, derived_.springZeta);
		combSpring_[i].setResponse(derived_.combSpringHz, kCriticalDamping);
	}

	// **THE LFO, READ AT THIS PHASE UNDER THE NEW SHAPE** — the other half of the pair `beginTick` moves
	// the springs by. Read HERE, before the transport lock can move the phase, so the difference between
	// the two readings is the rebuild's and nothing else's.
	change.after = lfo_.read();
	return change;
}

/// EVERYTHING DECIDED ONCE PER TICK — and a TICK IS NOT A BLOCK, which is the whole point.
///
/// THE FIRST DRAFT DID THIS PER BLOCK AND THAT WAS A DEFECT. The LFO would then be read once per
/// host buffer, so the same audio in blocks of 64 and blocks of 512 would get eight times as many
/// modulation updates in the first case — the device's sound would be a function of the host's
/// buffer size. `multimode_filter.h` states the rule this now follows and states it as a correctness
/// constraint rather than a preference: "tests/test_synth.cpp pins that a performance renders
/// identically in blocks of 1 and 37; a coefficient smoother stepped once per block, with a rate
/// taken from the block length, is invariant only in algebra and not in floating point. This module
/// therefore steps every kTickSamples samples counted from the start of the stream, exactly as
/// PuckMotion does."
///
/// So the grid is anchored to the STREAM, not to the call. Every `kTickSamples` samples since
/// `prepare`, wherever the block boundaries happen to fall.
void Automod::beginTick() {
	const float width = std::clamp(finiteOr(target_.width, 1.0f), 0.0f, 1.0f);
	const Rebuild change = refresh(width);

	block_.mix = std::clamp(finiteOr(target_.mix, 0.0f), 0.0f, 1.0f);

	// **FREQ IS A SECOND CONTRIBUTOR TO THE TWO RATIOS NOTE TRACKING ALREADY OWNS**, and it is
	// computed HERE rather than in `setNoteContext` for a plain reason: that runs on a pitch change,
	// and a knob moves without one.
	//
	// OPPOSITE DIRECTIONS, because a comb's delay is the reciprocal of its pitch — the same sentence
	// note tracking is written under, one line of which is now shared by two causes.
	//
	// CLAMPED SEPARATELY FROM THE PITCH PAIR, which is the firmware's arrangement rather than an
	// oversight: each contributor is bounded on its own, so a note at the top of its range and a Freq
	// at the top of its can still compound. The comb's own `delayFor` bounds the result to the line,
	// and the filter's `cornerHzFor` bounds it to Nyquist, so compounding is loud rather than unsafe.
	{
		// THROUGH THE SHARED FUNCTION, which the STRIP also calls — see `filterFreqRatioFor` for what
		// it cost when the two computed this separately and one of them computed it as 1.
		filterFreqRatio_ = filterFreqRatioFor(target_.freq);
		const float freqOctaves =
		    std::clamp(finiteOr(target_.freq, 0.0f), -1.0f, 1.0f) * kFreqOctaves;
		combFreqRatio_ = std::clamp(std::exp2(-freqOctaves), 0.25f, 4.0f);
	}

	// THE LFO IS READ ONCE PER BLOCK AND SMOOTHED PER SAMPLE BY THE SPRINGS, which is the firmware's
	// arrangement and is why the springs exist: they turn a block-rate stair into a continuous sweep,
	// and under-damped they add the bounce the Mod field asks for.
	// STOP DOES NOT ADVANCE THE LFO, which is the one thing `modulationFor` cannot express: it is a
	// question about whether the phase moves, not about what a tap becomes. The taps it hands back
	// are ignored either way — `modulationFor` answers Manual in Stop — but reading the LFO here
	// would still walk its phase, and a frozen LFO that keeps counting jumps the moment it thaws.
	// WHERE THE LFO IS, TO WHOEVER IS DRAWING IT — **before the read**, so the number is the phase
	// the taps below are read AT rather than the one the next tick will start from. See
	// `setReadingsCallback` for why this sits in `beginTick` and nowhere else.
	// THE ENVELOPE IS PUBLISHED WHETHER OR NOT ANYONE IS LISTENING, so the test seam and the meter
	// read the same number by construction rather than by two computations agreeing.
	publishedEnvelope_ = std::max(std::min(envelope_.level(Channel::Left), 1.0f),
	                              std::min(envelope_.level(Channel::Right), 1.0f));
	if (readingsCallback_) {
		readingsCallback_(static_cast<float>(lfo_.phase()), publishedEnvelope_);
	}

	// **FREE MODE IS POSITIONED BY THE SONG — backlog DQ.** By decision Free always locks, its phase
	// derived from the DAW playhead time modulo the Automod period.
	//
	// **BEFORE THE READ, NOT AFTER**, so the taps this tick returns are the ones the song asks for
	// rather than the ones the accumulator happened to reach. `tick` is read-then-advance for the same
	// reason a retrigger must report the shape's start.
	//
	// **FOLLOWED, NOT DIVIDED** (option A, 2026-09-25, the modulators' rule): Rate is a route
	// destination, and `position / period` every tick scrubbed the LFO whenever a route moved the period
	// — further the later in the song. See `device::TransportLock`.
	const std::optional<SongPeriod> song =
	    target_.lfoMode == LfoModeChoice::Free ? songPeriod() : std::nullopt;
	if (song.has_value()) {
		lfo_.lockPhaseTo(lock_.follow(song->position, song->period, song->tickLength, song->road));
	}
	else {
		lock_.release();
	}

	const bool stopped = (target_.lfoMode == LfoModeChoice::Stop);
	const LfoTaps taps = stopped ? LfoTaps {} : lfo_.tick(kTickSamples);

	// DEPTH SCALES THE LFO AND MANUAL IS ADDED AFTER, so Manual reaches the sound even at zero depth
	// — which is what makes it a usable control rather than a second depth. THE ARITHMETIC IS
	// `modulationFor`'s, shared with the card's strip, and the Stop branch went into it with the
	// reasoning that used to sit here.
	// **AND THE ENVELOPE'S THREE ROADS INTO THE FILTER'S TARGET — at block rate, as the firmware has
	// them** (`envelopeSteerFor` in the header carries the reference and what the old per-sample
	// steer got wrong). The follower itself still runs per sample; what is read here is its level and
	// derivative at the block's start, and the spring turns the block-rate stair into a sweep as it
	// does for the LFO. BOUNDED AT ONE before it steers, for the reason the old road wrote down: a
	// float bus past 0 dBFS would otherwise run the LFO backwards on loud passages.
	const float taps2[2] = { taps.left, taps.right };
	const float before2[2] = { change.before.left, change.before.right };
	const float after2[2] = { change.after.left, change.after.right };
	for (int ch = 0; ch < 2; ++ch) {
		const auto channel = ch == 0 ? Channel::Left : Channel::Right;
		const EnvelopeSteer steer = envelopeSteerFor(target_, derived_, std::min(envelope_.level(channel), 1.0f),
		                                             envelope_.derivative(channel));
		block_.tap[ch] = std::isfinite(taps2[ch]) ? taps2[ch] : 0.0f;
		block_.envValue[ch] = steer.envValue;
		block_.envScale[ch] = steer.scale;
		block_.derivNorm[ch] = steer.derivNorm;
		block_.springTarget[ch] = springTargetFor(target_, taps2[ch], steer);
		// **A REBUILT WAVEFORM MOVES THE SPRING WITH ITS TARGET — IN THE TARGET'S UNITS (backlog IS).**
		// The firmware's rule (`automodulator.cpp:601`) is that a rebuild must not make the spring lunge
		// toward the new shape's value, and the old code honoured it by SNAPPING the spring to the raw
		// LFO tap. But the spring tracks `springTargetFor` — the tap through Depth, the envelope's steer
		// and Manual — so the snap put it in the wrong place: MEASURED, a 0.002 Type nudge threw the
		// corner 1.09 octaves in Stop (whose target ignores the tap entirely) and 1.0 in Free, and
		// under a Type that moved every tick the corner followed the RAW tap to 0.0004, bypassing Depth,
		// Manual, the steer and the spring. Now the spring shifts by exactly what its target moved at
		// this phase — the same steer on both sides, so the shift is the rebuild's alone — and keeps its
		// velocity. On the FIRST build there is no "before", so it starts on its target.
		if (change.rebuilt) {
			if (change.first) {
				filterSpring_[ch].snapTo(block_.springTarget[ch]);
			}
			else {
				filterSpring_[ch].shiftBy(springTargetFor(target_, after2[ch], steer)
				                          - springTargetFor(target_, before2[ch], steer));
			}
		}
	}
	block_.combTarget[0] = modulationFor(target_, taps.combLeft);
	block_.combTarget[1] = modulationFor(target_, taps.combRight);
	if (change.rebuilt) {
		const float combBefore[2] = { change.before.combLeft, change.before.combRight };
		const float combAfter[2] = { change.after.combLeft, change.after.combRight };
		for (int ch = 0; ch < 2; ++ch) {
			if (change.first) {
				combSpring_[ch].snapTo(block_.combTarget[ch]);
			}
			else {
				combSpring_[ch].shiftBy(modulationFor(target_, combAfter[ch]) - modulationFor(target_, combBefore[ch]));
			}
		}
	}

	// PITCH TRACKING ON THE COMB: an octave up HALVES the delay, because a comb's delay is the
	// reciprocal of its pitch. Clamped into the line, since a route can put a note anywhere.
	comb_.setSettings(derived_.comb);
	// **AND RAMPED ACROSS THE TICK, NOT STEPPED AT ITS START (backlog IS).** A Freq turn or a new note
	// used to jump the delay read at a tick boundary, which a sinc read turns into a click; now the base
	// walks from where the last tick left it to this tick's value over the tick's 64 samples. A fresh
	// stream starts on its value.
	// KEYTRACK OFF LEAVES THE COMB WHERE THE FIELD PUT IT AT C4 — see Param::Keytrack.
	const float combPitch = target_.keytrack ? combPitchRatio_ : 1.0f;
	const float baseNow = std::clamp(derived_.comb.baseDelaySamples * combPitch * combFreqRatio_,
	                                 kCombMinDelaySamples, kCombMaxDelaySamples);
	combBaseFrom_ = rampsPrimed_ ? combBaseTo_ : baseNow;
	combBaseTo_ = baseNow;

	// THE BAND BLEND, which the field may or may not modulate. `staticFilterMix` is part of the
	// derive rather than a runtime guess — see automod_derived.h.
	block_.mixVector = derived_.filterMix;
	if (!derived_.staticFilterMix) {
		const float phase = static_cast<float>(lfo_.phase());
		const float weights[3] = { block_.mixVector.low, block_.mixVector.band, block_.mixVector.high };
		float modulated[3];
		for (int b = 0; b < 3; ++b) {
			const float lfoAt =
			    character::evalLfoShape(derived_.shape, phase + derived_.bandPhaseOffset[b]);
			modulated[b] = weights[b] + weights[b] * derived_.bandResponse[b] * lfoAt;
		}
		block_.mixVector = { modulated[0], modulated[1], modulated[2] };
	}
	// **THE BAND BLEND IS RAMPED ACROSS THE TICK TOO (backlog IS)** — it was applied as a stair at the
	// tick rate, 750 steps a second at 48 kHz, which a fast LFO turns into a zipper.
	mixFrom_ = rampsPrimed_ ? mixTo_ : block_.mixVector;
	mixTo_ = block_.mixVector;
	rampsPrimed_ = true;
}

/// ONE SAMPLE THROUGH THE WHOLE CHAIN. Both channels, because the filter and the comb are stereo
/// objects whose state must advance together.
void Automod::processFrame(float& left, float& right) {
	const float dryL = left;
	const float dryR = right;

	// THE ENVELOPE READS THE INPUT, BEFORE ANYTHING TOUCHES IT — see the header on why any other tap
	// point is a feedback loop.
	// TRIMMED AT THE FOLLOWER'S INPUT, which is where `automod_envelope.h` says the decision belongs:
	// "the effect's decision rather than this object's — which is why nothing is normalised here".
	// Scaling the input rather than the level also keeps the DERIVATIVE consistent, since both come
	// off the same follower.
	envelope_.process(dryL * envTrimGain_, dryR * envTrimGain_);

	// The springs walk toward this block's target, one step per sample.
	const float modL = filterSpring_[0].tick(block_.springTarget[0]);
	const float modR = filterSpring_[1].tick(block_.springTarget[1]);
	const float combModL = combSpring_[0].tick(block_.combTarget[0]);
	const float combModR = combSpring_[1].tick(block_.combTarget[1]);

	// THE ENVELOPE'S STEERING IS ALREADY IN THE SPRING'S TARGET — `beginTick`, through
	// `envelopeSteerFor` — where the firmware applies it. A per-sample `1 + env * influence` on the
	// spring's OUTPUT used to sit here; it scaled Manual along with the LFO and knew nothing of the
	// derivative or value roads. The bound-at-one reasoning it carried (a float envelope past 0 dBFS
	// ran the LFO backwards at the field's most negative influence: MEASURED, +3 dBFS gave a steer of
	// -0.413) moved with it and still holds: the level is clamped before it steers.
	//
	// **ONE CORNER PER EAR (backlog IS).** This used to be one corner for both, driven by the AVERAGE of
	// the two springs — so the field's stereo offset never reached the filter at all, and averaging two
	// taps a phase apart ate sweep: MEASURED over 2000 coordinates, a median 76% of one ear's sweep
	// survived (p10 51%). Each ear now has its own corner; `Width` at zero makes the two taps one tap,
	// which is the mono the old code was, minus the averaging loss.
	//
	// THE BASE IS A FLOOR AND THE MODULATION MOVES AROUND IT, in octaves — see `automod_filter.h`
	// for why the floor is bright and absolute rather than a fraction of our own range.
	//
	// THROUGH `cornerHzFor` RATHER THAN INLINE, so the card's strip draws THIS number rather than one
	// that agrees with it today.
	// KEYTRACK OFF LEAVES THE CORNER WHERE THE FIELD PUT IT AT C4 — see Param::Keytrack.
	const float ratio = (target_.keytrack ? filterPitchRatio_ : 1.0f) * filterFreqRatio_;
	const float cutoffL = cornerHzFor(derived_.filterCutoffBase, derived_.filterModDepth, modL, ratio, sampleRate_);
	const float cutoffR = cornerHzFor(derived_.filterCutoffBase, derived_.filterModDepth, modR, ratio, sampleRate_);
	lastCutoffHz_ = cutoffL;
	lastCutoffRightHz_ = cutoffR;
	// THE TICK'S RAMPS, one sample further along — see `beginTick`.
	const float along = static_cast<float>(tickPhase_ + 1) / static_cast<float>(kTickSamples);
	const FilterMix mix { mixFrom_.low + (mixTo_.low - mixFrom_.low) * along,
		                  mixFrom_.band + (mixTo_.band - mixFrom_.band) * along,
		                  mixFrom_.high + (mixTo_.high - mixFrom_.high) * along };
	filter_.setState(cutoffL, cutoffR, derived_.filterResonance, mix);
	comb_.setBaseDelaySamples(combBaseFrom_ + (combBaseTo_ - combBaseFrom_) * along);

	float wetL = dryL;
	float wetR = dryR;
	filter_.processSample(wetL, wetR);

	lastCombDelay_ = comb_.delayFor(combModL);
	comb_.processSample(wetL, wetR, combModL, combModR);

	left = dryL + (wetL - dryL) * mixNow_;
	right = dryR + (wetR - dryR) * mixNow_;
}

void Automod::setReadingsCallback(ReadingsCallback callback) {
	readingsCallback_ = std::move(callback);
}

void Automod::processStereo(float* left, float* right, int frames) {
	mixRun_ = pendingMixRun_; // this call's window, taken before any return
	pendingMixRun_ = nullptr;
	if (frames <= 0 || left == nullptr || right == nullptr) {
		return;
	}
	// BIT-EXACTLY THE IDENTITY WHEN OFF: not one sample read, not one written — AND NOTHING KEPT FOR
	// THE WAY BACK IN (backlog EH, shape 6): the comb's line and the filter's state used to survive a
	// bypass intact, so re-enabling replayed seconds-old audio out of the comb. The multimode filter's
	// rule, applied here: the first bypassed block resets, once.
	if (!target_.enabled) {
		if (running_) {
			running_ = false;
			reset();
		}
		return;
	}
	running_ = true;
	for (int i = 0; i < frames; ++i) {
		// THE TICK IS COUNTED FROM THE STREAM, so where the caller's block boundaries fall changes
		// nothing. See beginTick.
		if (tickPhase_ == 0) {
			beginTick();
		}
		// READ BEFORE THE FRAME OVERWRITES IT IN PLACE — see `kRestSilentSeconds`.
		noteInput(left[i], right[i]);
		mixNow_ = mixRun_ != nullptr ? std::clamp(finiteOr(mixRun_[i], 0.0f), 0.0f, 1.0f) : block_.mix;
		processFrame(left[i], right[i]);
		tickPhase_ = (tickPhase_ + 1) % kTickSamples;
	}
}

/// COUNT EXACTLY-SILENT INPUT, AND CLEAR THE STATE ONCE THERE HAS BEEN ENOUGH OF IT.
///
/// The comb's feedback loop decays geometrically and reaches exact zero only when its last denormal
/// flushes; `test_fx_runner`'s tail case asks for exact silence, and measured this device still
/// writing at block 396 of 400 — about 4.2 seconds after the burst. That is not a ring anybody
/// hears, it is a denormal tail, and it costs every downstream device its fast path.
void Automod::noteInput(float left, float right) {
	if (left == 0.0f && right == 0.0f) {
		if (silentRun_ < restSilenceNeeded_) {
			++silentRun_;
			if (silentRun_ >= restSilenceNeeded_) {
				// EVERYTHING WITH MEMORY, not just the comb: the springs hold a position, the envelope
				// holds a level, and the filter holds two integrator states. A flush that cleared only
				// the loudest of them would leave the others to be found later.
				filter_.reset();
				comb_.reset();
				envelope_.reset();
				for (int i = 0; i < 2; ++i) {
					filterSpring_[i].reset();
					combSpring_[i].reset();
				}
			}
		}
	}
	else {
		silentRun_ = 0;
	}
}

void Automod::processMono(float* mono, int frames) {
	mixRun_ = pendingMixRun_;
	pendingMixRun_ = nullptr;
	if (frames <= 0 || mono == nullptr) {
		return;
	}
	// **THE SAME BYPASS ROAD AS `processStereo` (backlog IS).** This returned without the reset, so a
	// mono chain that bypassed and re-enabled replayed the comb's stale line — the defect EH fixed on the
	// stereo road, left standing on this one.
	if (!target_.enabled) {
		if (running_) {
			running_ = false;
			reset();
		}
		return;
	}
	running_ = true;
	// MONO FEEDS BOTH EARS THE SAME SIGNAL AND TAKES THE LEFT RESULT. Running the real path rather
	// than a second one is what stops the two drifting apart; the right channel's work is waste, and
	// it is cheaper than a duplicated sample loop that has to be kept in step by hand.
	for (int i = 0; i < frames; ++i) {
		if (tickPhase_ == 0) {
			beginTick();
		}
		noteInput(mono[i], mono[i]);
		mixNow_ = mixRun_ != nullptr ? std::clamp(finiteOr(mixRun_[i], 0.0f), 0.0f, 1.0f) : block_.mix;
		float l = mono[i];
		float r = mono[i];
		processFrame(l, r);
		mono[i] = l;
		tickPhase_ = (tickPhase_ + 1) % kTickSamples;
	}
}

//------------------------------------------------------------------------------------------------//

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 1> kWindowedControls { static_cast<uint8_t>(Param::Mix) };

const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Automod),
	.stateAlign = alignof(Automod),
	.construct = [](void* storage) { new (storage) Automod(); },
	.destruct = [](void* storage) { static_cast<Automod*>(storage)->~Automod(); },
	.prepare = [](void* storage, double sampleRate, int) {
		static_cast<Automod*>(storage)->prepare(sampleRate);
	},
	.reset = [](void* storage) { static_cast<Automod*>(storage)->reset(); },
	.setSlot = nullptr,
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    // ABSENT FIELDS TAKE THEIR DECLARED DEFAULTS, not zero. A row decoded with Enabled at
		    // zero would come back bypassed, and one with Depth at zero would come back inert — both
		    // of which read as "the device is broken" rather than "the patch was short".
		    Row row {};
		    for (size_t i = 0; i < row.size(); ++i) {
			    row[i] = static_cast<float>(kParamDescs[i].def);
		    }
		    const size_t take = std::min(fieldsPresent, row.size());
		    for (size_t i = 0; i < take; ++i) {
			    row[i] = fields[i];
		    }
		    Settings settings = settingsOf(row);
		    // **THE BLOB, READ PAST THE DECLARED PARAMETERS.** `take` above stops at the typed Row's
		    // width, which is the param count — the folded travel lives beyond it, in the floats the
		    // host stored and returned without parsing (`device_row.h`, `kMaxDeviceFields`). A patch
		    // that predates the blob simply has a shorter `fieldsPresent` and leaves these zero.
		    for (size_t i = 0; i < kFoldedTravelFloats; ++i) {
			    const size_t at = kFoldedTravelFirst + i;
			    settings.foldedTravel[i] = at < fieldsPresent ? fields[at] : 0.0f;
		    }
		    static_cast<Automod*>(storage)->setTarget(settings);
	    },
	.setNoteContext =
	    [](void* storage, const device::NoteContext& note) {
		    static_cast<Automod*>(storage)->setNoteContext(note);
	    },
	// THE ORDER MATTERS: a designated initialiser must follow declaration order, and `-Werror`
	// caught it. `setTransportClock` sits after `setNoteContext` in `AudioOps`, so it sits after it
	// here — this device is the first caller of the entry the fx lane gained in cc45832.
	.setTransportClock =
	    [](void* storage, const device::TransportClock& clock) {
		    static_cast<Automod*>(storage)->setTransportClock(clock);
	    },
	.processStereo = [](void* storage, float* left, float* right, int frames) {
		static_cast<Automod*>(storage)->processStereo(left, right, frames);
	},
	.processMono = [](void* storage, float* mono, int frames) {
		static_cast<Automod*>(storage)->processMono(mono, frames);
	},
	// THE HOST'S EAR FOR THE READINGS (`device::ReadingsSink`): phase, then envelope — the two an automod
	// surface reads back. Captured by value; never allocates.
	.setReadingsSink = [](void* storage, device::ReadingsSink sink) {
		static_cast<Automod*>(storage)->setReadingsCallback([sink](float phase, float envelope) {
			const float values[2] = { phase, envelope };
			if (sink.publish != nullptr) {
				sink.publish(sink.context, values, 2);
			}
		});
	},
	// MIX IS READ PER SAMPLE WHILE IT MOVES (backlog FK) — see `Automod::mixNow_`. Freq and the rest are coefficients on
	// the device's own tick grid.
	.windowedControls = kWindowedControls,
	.setFieldWindows =
	    [](void* storage, const float* const* runs, size_t count, int) {
		    const auto mix = static_cast<size_t>(Param::Mix);
		    static_cast<Automod*>(storage)->setMixRun(mix < count ? runs[mix] : nullptr);
	    },
	.holdsTail = [](const void* storage) { return static_cast<const Automod*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::automod
